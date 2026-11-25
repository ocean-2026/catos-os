/*
 * anim.c -- cap2 固定缓动枚举 + 16ms tick 预生成偏移表 + 可中断动画。
 *
 * 接线桩：全部空实现（无动画 = 旧行为）。
 */
#include "gui/block.h"
#include "gui_int.h"

void anim_tick(uint16_t dt_ms) { (void)dt_ms; }
void anim_apply(block *b) { (void)b; }
void anim_kick(block *b) { (void)b; }
void anim_reduced(int on) { (void)on; }
int  anim_busy(void) { return 0; }
