#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

/*
 * gfxdemo.c -- v2.7 display front-end demo, running entirely in RING3.
 *
 * Proves the three policy points:
 *   1. a user program CAN switch text -> graphics mode;
 *   2. it can only paint STYLED TEXT (256 colours + attributes);
 *      there is no putpixel / fillrect / framebuffer call in the ABI;
 *   3. switching graphics -> text asks the human at the console for a
 *      physical 'Y'.  The program cannot grant that consent itself.
 *
 * Run:  run /bin/gfxdemo.ce
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;

    api->write("gfxdemo: ring3 display front-end test\n");
    api->write("  mode=");      api->print_int(api->display_info(0));
    api->write("  grid=");      api->print_int(api->display_info(1));
    api->write("x");            api->print_int(api->display_info(2));
    api->write("  colors=");    api->print_int(api->display_info(3));
    api->write("\n");

    api->write("requesting GRAPHICS mode from ring3...\n");
    int m = api->display_mode(1);
    api->write("  now mode="); api->print_int(m);
    api->write("  colors=");   api->print_int(api->display_info(3));
    api->write("  styles=0x"); api->print_hex((unsigned)api->display_info(4));
    api->write("\n");

    /* Nothing below draws a pixel.  set_style + write is the whole API. */
    api->set_style(15, 1, 0x01);            /* white on blue, bold */
    api->write("  BOLD  white on blue\n");
    api->set_style(14, 0, 0x08);            /* yellow, underline   */
    api->write("  UNDERLINE\n");
    api->set_style(11, 0, 0x04);            /* light cyan, italic  */
    api->write("  ITALIC\n");
    api->set_style(15, 4, 0x20);            /* inverse on red      */
    api->write("  INVERSE\n");
    api->set_style(200, 0, 0x00);           /* grey-200 from ramp  */
    api->write("  256-color palette index 200\n");
    api->set_style(7, 0, 0x00);             /* restore defaults    */

    api->write("query via display_mode(-1) = ");
    api->print_int(api->display_mode(-1));
    api->write("\n");

    api->write("gfxdemo: requesting TEXT mode (needs YOUR 'Y')\n");
    int r = api->display_mode(0);
    api->write("gfxdemo: display_mode(0) returned ");
    api->print_int(r);
    api->write(r < 0 ? " (denied / still graphics)\n"
                     : " (consent granted / text mode)\n");
    api->write("gfxdemo: done. no drawing API was used.\n");
    return 0;
}
