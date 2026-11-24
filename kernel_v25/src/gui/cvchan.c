/*
 * cvchan.c -- 隔离式画布通道（v2.81，规格 §8 应用唯一合法像素出口）。
 *
 * 口径：
 *   - 应用只能拿到属于自己画布块的可写 24bpp 缓冲：缓冲在应用扩展 arena
 *     匿名 mmap（非 VRAM、非合成器 surface、非内核 backing 指针）。
 *   - present() 只把脏子矩形从应用缓冲拷进内核私有 backing 缓冲并标脏；
 *     合成/上屏仍走块树通道。应用侧禁止 gfx_info/gfx_rect/gfx_blit 与
 *     display_mode(3)，由 syscall 门禁统一 -E_EPERM。
 *   - 配额（保守初值，实测后再调）：单应用 ≤2 块；单块 ≤320×240；
 *     通道总面积 ≤384 KiB；单次 present 矩形 ≤ 全屏 1/4；单帧 present ≤8 次。
 *   - 应用退出/被杀：task_cleanup 强制回收其全部画布，不留孤儿句柄。
 *
 * 串口键名（探针线回填共用）：
 *   GUI:cv_create res=<r> w=<w> h=<h> pid=<tid>
 *   GUI:cv_present res=<r> x=<x> y=<y> w=<w> h=<h> ms=<ms>
 *   GUI:cv_destroy res=<r>
 *   GUI:cv_reject code=<n> pid=<tid>
 */
#include <stdint.h>
#include <stddef.h>
#include "task.h"
#include "vma.h"
#include "paging.h"
#include "kstring.h"
#include "serial_log.h"
#include "ce.h"
#include "intr.h"

#define CV_POOL_MAX       8
#define CV_MAX_PER_TASK   2
#define CV_MAX_W          320
#define CV_MAX_H          240
#define CV_TOTAL_BYTES    (384u * 1024u)
#define CV_PRESENT_AREA   (800 * 600 / 4)   /* 单次 present ≤ 全屏 1/4   */
#define CV_PRESENT_FRAME  8                 /* 单帧 present 次数上限      */
#define CV_PX             3u                 /* RGB24 bytes per pixel     */

struct cv_slot {
    int      used;
    int      tid;
    int32_t  w, h, stride;
    uint32_t bytes;
    uint8_t *kbuf;            /* 内核私有 backing（身份映射帧页） */
    uint32_t phys[CV_MAX_W * CV_MAX_H * CV_PX / 4096 + 1];
    int      pages;
    uint32_t uaddr;           /* 应用 arena mmap 缓冲（ring3 可写） */
};

static struct cv_slot cv_pool[CV_POOL_MAX];
static uint8_t cv_present_cnt[MAX_TASKS];

static void cv_dec(int v) {
    char tmp[12]; int n = 0; unsigned int u;
    if (v < 0) { ser_putc('-'); u = (unsigned int)(-(int)v); }
    else u = (unsigned int)v;
    if (u == 0) { ser_putc('0'); return; }
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) ser_putc(tmp[--n]);
}

static void cv_reject(int code, int tid) {
    ser_write("GUI:cv_reject code="); cv_dec(code);
    ser_write(" pid="); cv_dec(tid); ser_write("\n");
}

static int cv_count_task(int tid) {
    int n = 0;
    for (int i = 0; i < CV_POOL_MAX; i++)
        if (cv_pool[i].used && cv_pool[i].tid == tid) n++;
    return n;
}

static uint32_t cv_total_bytes(void) {
    uint32_t b = 0;
    for (int i = 0; i < CV_POOL_MAX; i++)
        if (cv_pool[i].used) b += cv_pool[i].bytes;
    return b;
}

static struct cv_slot *cv_find(int res, int tid) {
    if (res < 0 || res >= CV_POOL_MAX) return 0;
    struct cv_slot *s = &cv_pool[res];
    if (!s->used || s->tid != tid) return 0;
    return s;
}

int cvchan_owner(int res, int tid) {
    return cv_find(res, tid) != 0;
}

static void cv_free_slot(struct cv_slot *s) {
    if (s->uaddr && s->tid >= 0 && s->tid < MAX_TASKS) {
        /* VMA 节点由 task_cleanup 的地址空间销毁兜底；这里只解除映射。 */
        vma_munmap(task_by_tid(s->tid), s->uaddr, s->bytes);
    }
    for (int p = 0; p < s->pages; p++) frame_free(s->phys[p]);
    s->used = 0; s->tid = -1; s->kbuf = 0; s->uaddr = 0;
    s->pages = 0; s->bytes = 0;
}

int cvchan_create(task_t *t, int w, int h, ce_canvas_t *out) {
    int tid = t ? t->tid : -1;
    if (!out) return E_PIX_FIELD;
    if (w <= 0 || h <= 0 || w > CV_MAX_W || h > CV_MAX_H) {
        cv_reject(E_CANVAS_BOUNDS, tid);
        return E_CANVAS_BOUNDS;
    }
    uint32_t bytes = (uint32_t)w * (uint32_t)h * CV_PX;
    if (cv_count_task(tid) >= CV_MAX_PER_TASK ||
        cv_total_bytes() + bytes > CV_TOTAL_BYTES) {
        cv_reject(E_CANVAS_QUOTA, tid);
        return E_CANVAS_QUOTA;
    }
    int res = -1;
    for (int i = 0; i < CV_POOL_MAX; i++)
        if (!cv_pool[i].used) { res = i; break; }
    if (res < 0) { cv_reject(E_CANVAS_QUOTA, tid); return E_CANVAS_QUOTA; }

    struct cv_slot *s = &cv_pool[res];
    int pages = (int)((bytes + 4095u) / 4096u);
    for (int p = 0; p < pages; p++) {
        uint32_t pa = frame_alloc();
        if (!pa) {
            for (int q = 0; q < p; q++) frame_free(s->phys[q]);
            cv_reject(E_CANVAS_QUOTA, tid);
            return E_CANVAS_QUOTA;
        }
        s->phys[p] = pa;
    }
    s->kbuf = (uint8_t *)s->phys[0];
    memset(s->kbuf, 0, bytes);

    uint32_t ua = vma_mmap_anon(t, bytes, 1);
    if (!ua) {
        for (int p = 0; p < pages; p++) frame_free(s->phys[p]);
        cv_reject(E_CANVAS_QUOTA, tid);
        return E_CANVAS_QUOTA;
    }
    s->used = 1; s->tid = tid;
    s->w = w; s->h = h; s->stride = (int32_t)w * CV_PX;
    s->bytes = bytes; s->pages = pages; s->uaddr = ua;

    out->res = res; out->w = w; out->h = h;
    out->stride = s->stride; out->buf = (void *)ua;

    ser_write("GUI:cv_create res="); cv_dec(res);
    ser_write(" w="); cv_dec(w);
    ser_write(" h="); cv_dec(h);
    ser_write(" pid="); cv_dec(tid); ser_write("\n");
    return 0;
}

/* 校验矩形归属/边界/配额；返回槽位或错误码。 */
static int cv_present_validate(task_t *t, int res, int x, int y,
                               int w, int h, struct cv_slot **out) {
    int tid = t ? t->tid : -1;
    if (w <= 0 || h <= 0 ||
        (uint32_t)w * (uint32_t)h > CV_PRESENT_AREA) {
        cv_reject(E_CANVAS_BOUNDS, tid);
        return E_CANVAS_BOUNDS;
    }
    struct cv_slot *s = cv_find(res, tid);
    if (!s) { cv_reject(E_CANVAS_BADH, tid); return E_CANVAS_BADH; }
    if (x < 0 || y < 0 || x + w > s->w || y + h > s->h) {
        cv_reject(E_CANVAS_BOUNDS, tid);
        return E_CANVAS_BOUNDS;
    }
    if (cv_present_cnt[tid] >= CV_PRESENT_FRAME) {
        cv_reject(E_CANVAS_QUOTA, tid);
        return E_CANVAS_QUOTA;
    }
    *out = s;
    return 0;
}

int cvchan_present(task_t *t, const cv_present_t *r) {
    int tid = t ? t->tid : -1;
    if (!r) return E_PIX_FIELD;
    struct cv_slot *s;
    int vc = cv_present_validate(t, r->res, r->x, r->y, r->w, r->h, &s);
    if (vc != 0) return vc;

    uint32_t t0 = timer_ticks;
    for (int j = 0; j < r->h; j++) {
        const uint8_t *urow =
            (const uint8_t *)(s->uaddr + (uint32_t)(r->y + j) * s->stride +
                              (uint32_t)r->x * CV_PX);
        uint8_t *krow =
            s->kbuf + (uint32_t)(r->y + j) * s->stride +
            (uint32_t)r->x * CV_PX;
        if (copy_from_user(t, krow, urow, (uint32_t)r->w * CV_PX) != 0) {
            cv_reject(E_CANVAS_BADH, tid);
            return E_CANVAS_BADH;
        }
    }
    cv_present_cnt[tid]++;
    uint32_t ms = (timer_ticks - t0) * 10u;   /* 100Hz -> ms */

    ser_write("GUI:cv_present res="); cv_dec(r->res);
    ser_write(" x="); cv_dec(r->x);
    ser_write(" y="); cv_dec(r->y);
    ser_write(" w="); cv_dec(r->w);
    ser_write(" h="); cv_dec(r->h);
    ser_write(" ms="); cv_dec((int)ms);
    ser_write("\n");
    return 0;
}

/* 内核内自检路径：源即 backing 本身，不经用户 VMA 校验。 */
static int cvchan_present_internal(task_t *t, int res, int x, int y,
                                   int w, int h) {
    struct cv_slot *s;
    int vc = cv_present_validate(t, res, x, y, w, h, &s);
    if (vc != 0) return vc;
    cv_present_cnt[t->tid]++;
    ser_write("GUI:cv_present res="); cv_dec(res);
    ser_write(" x="); cv_dec(x);
    ser_write(" y="); cv_dec(y);
    ser_write(" w="); cv_dec(w);
    ser_write(" h="); cv_dec(h);
    ser_write(" ms=0\n");
    return 0;
}

int cvchan_destroy(task_t *t, int res) {
    int tid = t ? t->tid : -1;
    struct cv_slot *s = cv_find(res, tid);
    if (!s) { cv_reject(E_CANVAS_BADH, tid); return E_CANVAS_BADH; }
    cv_free_slot(s);
    ser_write("GUI:cv_destroy res="); cv_dec(res); ser_write("\n");
    return 0;
}

void cvchan_task_cleanup(task_t *t) {
    if (!t) return;
    for (int i = 0; i < CV_POOL_MAX; i++)
        if (cv_pool[i].used && cv_pool[i].tid == t->tid)
            cv_free_slot(&cv_pool[i]);
    if (t->tid >= 0 && t->tid < MAX_TASKS) cv_present_cnt[t->tid] = 0;
}

void cvchan_frame_tick(void) {
    for (int i = 0; i < MAX_TASKS; i++) cv_present_cnt[i] = 0;
}

/* `gui cvchan` 自检：建画布 -> 写棋盘(backing) -> present -> 销毁 一轮。
 * ring0 自检不经用户 VMA 回拷；CE 侧 arena 缓冲往返由后续 CE ABI 用例覆盖。 */
int cvchan_self_test(void) {
    task_t *t = task_current();
    ce_canvas_t cv;
    int rc_create = -1, rc_present = -1, rc_destroy = -1;
    int ok = 1;

    rc_create = cvchan_create(t, 64, 48, &cv);
    if (rc_create != 0 || cv.res < 0) { ok = 0; goto out; }

    /* 棋盘格直接写进内核 backing（自检路径，非用户指针）。 */
    {
        struct cv_slot *s = &cv_pool[cv.res];
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 64; x++) {
                uint32_t c = (((x / 8) ^ (y / 8)) & 1) ? 0xFF : 0x20;
                uint8_t *p = s->kbuf + y * s->stride + x * CV_PX;
                p[0] = (uint8_t)c; p[1] = (uint8_t)c; p[2] = (uint8_t)c;
            }
    }

    rc_present = cvchan_present_internal(t, cv.res, 0, 0, 64, 48);
    if (rc_present != 0) ok = 0;

    /* 负路径：未知句柄必须回 E_CANVAS_BADH，不得 deref。 */
    {
        int rc_bad = cvchan_present_internal(t, CV_POOL_MAX + 3, 0, 0, 4, 4);
        if (rc_bad != E_CANVAS_BADH) ok = 0;
    }

    rc_destroy = cvchan_destroy(t, cv.res);
    if (rc_destroy != 0) ok = 0;

out:
    ser_write("GUI:cvchan create="); cv_dec(rc_create);
    ser_write(" present="); cv_dec(rc_present);
    ser_write(" destroy="); cv_dec(rc_destroy);
    ser_write(" result="); cv_dec(ok ? 1 : 0);
    ser_write("\n");
    return ok ? 0 : 1;
}
