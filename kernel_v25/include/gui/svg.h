#ifndef CATOS_GUI_SVG_H
#define CATOS_GUI_SVG_H

/*
 * svg.h -- 按钮图片资源：内核极简 SVG 光栅化子集（规格 v3 追加）。
 *
 * 支持图元：rect / circle / ellipse / line / polygon / polyline / path
 * （path 仅 M/L/C/Q/Z，绝对与相对坐标）；纯色 fill（#rgb / #rrggbb + 少量
 * 命名色）；viewBox 等比映射。明确不支持 gradient / clipPath / transform /
 * filter / 文本 / pattern —— 遇到不支持元素跳过而不报错。
 *
 * 资源回落三级：文件 SVG 解析成功 → 内置兜底单色图标 → NULL（纯文字按钮）。
 * 缓存按 (path,size,state) 键，固定槽位、堆分配、LRU 淘汰；单图源 ≤64KB、
 * 输出 ≤64×64。
 */
#include <stdint.h>

#define SVG_MAX_OUT   64            /* 输出边长上限 px */
#define SVG_MAX_SRC   65536u        /* 源文件上限字节 */
#define SVG_CACHE_SLOTS 8

/* 内置兜底图标号（与文件路径无关；mono 单色，绘制时取按钮前景色）。 */
enum {
    SVG_BTN_OK = 0,
    SVG_BTN_CANCEL = 1,
    SVG_BTN_FOLDER = 2,
    SVG_BTN_FILE = 3
};

struct svg_icon {
    int      w, h;
    uint8_t *rgb;               /* 3*w*h；mono=1 时为 NULL */
    uint8_t *mask;              /* w*h，0/255；mono=1 时有效 */
    uint8_t  mono;              /* 1=单色兜底，rgb 忽略，绘制用前景色 */
    uint8_t  kind;              /* 兜底内置号 SVG_BTN_* */
    uint32_t key;               /* 缓存键哈希 */
    uint32_t lru;               /* LRU 时间戳 */
};

/* 按路径 + 目标尺寸解析；state 入缓存键（预留 hover/pressed 变体）。
 * 命中缓存直接返回；文件缺失/解析失败 → 内置兜底；再失败 → NULL。
 * 回落仅在真正回退时打一条串口（缓存命中不重复打点）。 */
const struct svg_icon *svg_resolve(const char *path, int size, uint8_t state);

/* 内置兜底号 -> 短名（探针/审计用）。 */
const char *svg_builtin_name(uint8_t kind);

/* 缓存自检：返回占用槽数；*miss / *fallback 回填累计计数（可传 NULL）。 */
int svg_cache_stats(int *miss, int *fallback);

/* 复位缓存（探针/桌面重置用）：归还槽位堆分配。 */
void svg_cache_reset(void);

#endif /* CATOS_GUI_SVG_H */
