#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/*
 * zhdemo.c -- v2.8 Chinese + truecolour demo, running entirely in RING3.
 *
 * Demonstrates:
 *   - UTF-8 Chinese output through the ordinary write() syscall;
 *   - 2x2 CJK cells that survive mode switches;
 *   - user-definable 256-entry CLUT (set_palette);
 *   - literal 0xRRGGBB colours (set_style_rgb), full depth on VBE;
 *   - switching text -> gfx -> VBE, and the gated return to text.
 *
 * There is STILL no drawing syscall: every visual below is a text cell.
 * Run:  run /bin/zhdemo.ce
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;

    api->write("zhdemo: Chinese + truecolour text demo (ring3)\n");
    api->write("  mode=");        api->print_int(api->display_info(0));
    api->write("  bpp=");         api->print_int(api->display_info(7));
    api->write("  cjk=");         api->print_int(api->display_info(8));
    api->write("  glyphs=");      api->print_int(api->display_info(9));
    api->write("\n");

    api->write("switching to GRAPHICS (8bpp indexed)...\n");
    int m = api->display_mode(1);
    api->write("  mode="); api->print_int(m); api->write("\n");

    /* UTF-8 Chinese.  The front-end decodes this and paints 16x16
     * glyphs occupying 2x2 logical cells. */
    api->set_style_rgb(0xFFD700, 0x000000, 0x01);   /* gold, bold */
    api->write("你好，世界！ CatOS 中文显示\n");
    api->set_style_rgb(0x7CFC00, 0x000040, 0x00);
    api->write("图形模式：256 色索引 + 8 种文字样式\n");

    /* Redefine one palette entry and use it as an index. */
    api->set_palette(200, 0x00FF80);
    api->set_style(200, 0, 0x00);
    api->write("palette[200] = spring green\n");
    api->write("  readback = ");
    api->print_hex(api->palette(200));
    api->write("\n");

    /* Truecolour mode, when the hardware supports it. */
    api->set_style_rgb(0xFFFFFF, 0x000000, 0);
    api->write("trying VBE 800x600x32...\n");
    int v = api->display_mode(2);
    if (v == 2) {
        api->set_style_rgb(0xFF3366, 0x101030, 0x01);
        api->write("真彩模式：每个字符都是真正的 RGB\n");
        api->set_style_rgb(0x66FF99, 0x000000, 0x08);
        api->write("中文不再受 256 色限制，共 1677 万色可用\n");
        api->set_style_rgb(0xFFFFFF, 0x000000, 0);
    } else {
        api->write("VBE unavailable (needs QEMU -vga std)\n");
    }

    api->write("zhdemo: requesting TEXT (needs YOUR 'Y')\n");
    int t = api->display_mode(0);
    api->write("zhdemo: display_mode(0) = ");
    api->print_int(t);
    api->write(t < 0 ? " (denied)\n" : " (consent granted)\n");
    api->write("zhdemo: done. no drawing API was used.\n");
    return 0;
}
