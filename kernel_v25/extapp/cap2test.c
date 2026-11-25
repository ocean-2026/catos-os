/* cap2test.c -- ring3 end-to-end probe for the cap2 decl channel ABI
 * (slots 77..79: decl_caps / blk_commit2 / blk_event_poll2).
 *
 * Unlike the kernel-internal `gui cap2-*` probes, this program runs as a
 * plain CE task and reaches the kernel only through the ring3 syscall
 * table.  Steps:
 *
 *   1. decl_caps() must report ALPHA | BUTTON | EVENTS2.
 *   2. blk_commit2() submits a rec2 subtree: a WORK column holding a
 *      BUTTON (alpha=192, corner_tl=10, font_size=1) and an INPUT.
 *   3. blk_event_poll2() drains the queue: a click event (ev_kind=0,
 *      target_id + local x/y) and an input submit (ev_kind=3, UTF-8 text).
 *   4. The same tree is resubmitted through the legacy 40B record path
 *      (blk_commit) to prove the old path still works and does not carry
 *      over any rec2-only field.
 *
 * Verdict line consumed by the gate driver:
 *   CAP2TEST caps=0x... click=<got> id=<gid> x=<lx> y=<ly> \
 *            input=<got> text=<utf8> rec1=<rc> PASS|FAIL
 *
 * Built freestanding; no libc.  Keep comments plain ASCII.
 */
#include "ce_app.h"

/* block kinds (mirror of include/gui/block.h) */
#define K_COLUMN   1
#define K_TEXT     4
#define K_RECT     5
#define K_BUTTON   12
#define K_INPUT    13
#define LAYER_WORK 200

/* capability bits (mirror of include/gui/decl.h) */
#define CAP_ALPHA     (1u << 0)
#define CAP_BUTTON    (1u << 7)
#define CAP_EVENTS2   (1u << 8)
#define CAP_WANT      (CAP_ALPHA | CAP_BUTTON | CAP_EVENTS2)

static void put_int(int v) {
    char b[12];
    int n = 0;
    unsigned int u;
    if (v < 0) { ce_write("-"); u = (unsigned int)(-(int)v); }
    else u = (unsigned int)v;
    if (u == 0) { ce_write("0"); return; }
    while (u) { b[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) ce_putc(b[--n]);
}

static void put_hex(uint32_t v) {
    ce_write("0x");
    const char *dig = "0123456789abcdef";
    int s;
    for (s = 28; s >= 0; s -= 4) ce_putc(dig[(v >> s) & 0xF]);
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    (void)argc; (void)argv;
    catos_api_t *api = ce_api();

    /* ---- 1. capability probe ------------------------------------------ */
    uint32_t caps = 0;
    int caps_ok = 0;
    if (api->decl_caps) {
        caps = api->decl_caps();
        caps_ok = (caps & CAP_WANT) == CAP_WANT;
    }

    /* ---- 2. rec2 commit: column + button + input ---------------------- */
    ce_blk_rec2_t r[3];
    char pool[] = "OK";
    int i;
    for (i = 0; i < 3; i++) {
        for (int j = 0; j < (int)sizeof(r[i]); j++) ((uint8_t *)&r[i])[j] = 0;
    }

    r[0].base.id = 1; r[0].base.parent = 0;
    r[0].base.kind = K_COLUMN; r[0].base.layer = LAYER_WORK;
    r[0].base.req_w = -1; r[0].base.req_h = -1;
    r[0].base.text_off = -1; r[0].base.pix_off = -1;
    r[0].base.pix_w = 0; r[0].base.pix_h = 0; r[0].base.res = -1;
    r[0].cap_ver = CE_BLK_REC2_VER;

    r[1].base.id = 2; r[1].base.parent = 1;
    r[1].base.kind = K_BUTTON; r[1].base.layer = LAYER_WORK;
    r[1].base.req_w = -1; r[1].base.req_h = -1;
    r[1].base.text_off = 0; r[1].base.pix_off = -1;
    r[1].base.pix_w = 0; r[1].base.pix_h = 0; r[1].base.res = -1;
    r[1].cap_ver = CE_BLK_REC2_VER;
    r[1].alpha = 192;
    r[1].corner_tl = 10;
    r[1].font_size = 1;

    r[2].base.id = 3; r[2].base.parent = 1;
    r[2].base.kind = K_INPUT; r[2].base.layer = LAYER_WORK;
    r[2].base.req_w = -1; r[2].base.req_h = -1;
    r[2].base.text_off = -1; r[2].base.pix_off = -1;
    r[2].base.pix_w = 0; r[2].base.pix_h = 0; r[2].base.res = -1;
    r[2].cap_ver = CE_BLK_REC2_VER;

    int rc2 = -1;
    if (api->blk_commit2) {
        rc2 = api->blk_commit2(r, 3, pool, (int)sizeof(pool),
                               (uint32_t)sizeof(ce_blk_rec2_t));
    }

    /* ---- 3. poll events: click then input ----------------------------- */
    int click_got = 0, input_got = 0;
    int click_id = 0, click_x = 0, click_y = 0;
    char itext[24]; itext[0] = 0;
    ce_blk_event2_t ev;

    /* Up to ~12 s: the gate driver injects the click / typing while this
     * task polls.  yield() lets the scheduler run; delay(100) backs off. */
    for (i = 0; i < 120 && !(click_got && input_got); i++) {
        if (api->blk_event_poll2) {
            int j;
            for (j = 0; j < (int)sizeof(ev); j++) ((uint8_t *)&ev)[j] = 0;
            int got = api->blk_event_poll2(&ev, 0);
            if (got == 1) {
                if (ev.ev_kind == 0 && !click_got) {
                    click_got = 1;
                    click_id = ev.target_id;
                    click_x = ev.x;
                    click_y = ev.y;
                } else if (ev.ev_kind == 3 && !input_got) {
                    input_got = 1;
                    int k = 0;
                    while (k < 23 && ev.utext[k]) { itext[k] = (char)ev.utext[k]; k++; }
                    itext[k] = 0;
                }
            }
        }
        if (api->yield) api->yield();
        if (api->delay) api->delay(100);
    }

    /* ---- 4. legacy 40B record regression --------------------------------
     * The old path only accepts the legacy kind whitelist (COLUMN..CANVAS),
     * so this resubmit mirrors dektest's column/text/rect tree; it proves
     * the 40B path still commits cleanly and does not read any rec2-only
     * extension byte. */
    ce_blk_rec_t r1[3];
    for (i = 0; i < 3; i++) {
        int j;
        for (j = 0; j < (int)sizeof(r1[i]); j++) ((uint8_t *)&r1[i])[j] = 0;
    }
    r1[0].id = 1; r1[0].parent = 0;
    r1[0].kind = K_COLUMN; r1[0].layer = LAYER_WORK;
    r1[0].req_w = -1; r1[0].req_h = -1;
    r1[0].text_off = -1; r1[0].pix_off = -1;
    r1[0].pix_w = 0; r1[0].pix_h = 0; r1[0].res = -1;
    r1[1].id = 2; r1[1].parent = 1;
    r1[1].kind = K_TEXT; r1[1].layer = LAYER_WORK;
    r1[1].req_w = -1; r1[1].req_h = -1;
    r1[1].text_off = 0; r1[1].pix_off = -1;
    r1[1].pix_w = 0; r1[1].pix_h = 0; r1[1].res = -1;
    r1[2].id = 3; r1[2].parent = 1;
    r1[2].kind = K_RECT; r1[2].layer = LAYER_WORK;
    r1[2].req_w = -1; r1[2].req_h = -1;
    r1[2].text_off = -1; r1[2].pix_off = -1;
    r1[2].pix_w = 0; r1[2].pix_h = 0; r1[2].res = -1;

    int rc1 = -1;
    if (api->blk_commit) {
        rc1 = api->blk_commit(r1, 3, pool, (int)sizeof(pool));
    }

    /* ---- verdict ------------------------------------------------------ */
    int ok = caps_ok && rc2 == 0 && click_got && input_got && rc1 == 0;

    ce_write("CAP2TEST caps="); put_hex(caps);
    ce_write(" rc2="); put_int(rc2);
    ce_write(" click="); put_int(click_got);
    ce_write(" id="); put_int(click_id);
    ce_write(" x="); put_int(click_x);
    ce_write(" y="); put_int(click_y);
    ce_write(" input="); put_int(input_got);
    ce_write(" text="); ce_write(itext);
    ce_write(" rec1="); put_int(rc1);
    ce_write(ok ? " PASS\n" : " FAIL\n");

    return ok ? 0 : 1;
}
