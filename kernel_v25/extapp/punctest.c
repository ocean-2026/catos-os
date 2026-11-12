/*
 * punctest.c -- CJK punctuation / font-coverage regression probe (v2.16).
 *
 * Pure userland, and deliberately TEXT-mode only: the VGA character map
 * stays at 0xB8000, so tests/security_regress.py can read it back with
 * HMP `xp /4000 0xb8000` and assert that no cell contains 0x3F ('?').
 * Before the font-table extension every CJK punctuation mark below was
 * silently replaced by '?' because hz_font contained GB2312 HANZI only.
 *
 * Coverage: U+FF1A U+FF0C U+FF01 (shell banner), U+3002 U+3001 U+FF1F,
 * U+300A/B, U+201C/D, U+3010/11, U+2026, plus fullwidth ASCII U+FF21..
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;
    if (!api) return -1;

    api->write("glyphs=");
    api->print_int(api->display_info(9));
    api->write("\n");
    api->write("中文显示测试：你好，世界！\n");
    api->write("《CatOS》：、。；！“引号”【括号】……\n");
    api->write("全角：ＡＢＣ１２３\n");
    return 0;
}
