/*
 * font8.c -- cap2 8x8 点阵字库（ASCII 95 + 常用标点），仅服务 8px 档。
 *
 * 接线桩：字模由文本包生成/内联；在此之前 8px 档回落 16px 位图降采样。
 */
#include <stdint.h>

/* 占位符号，避免空翻译单元；文本包替换为真实字模表。 */
const uint8_t cap2_font8_placeholder = 0;
