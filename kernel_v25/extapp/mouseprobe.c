#include "ce_app.h"

/* mouseprobe.c -- v2.69 P1 regression for CE ABI slots 66..68.
 * Reads the live pointer via the three new generic mouse primitives and
 * prints them.  The host probe injects a mouse move + left click through
 * QEMU and asserts the numbers change -- proving the thunks (0 args) and
 * the mouse_get_state() snapshots are wired end to end.
 * Old kernels expose NULL for these slots: we guard each one.
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    (void)argc; (void)argv;

    if (!api->mouse_x || !api->mouse_y || !api->mouse_buttons) {
        api->write("MOUSEPROBE: slots missing\n");
        return 1;
    }
    api->write("MOUSE X=");
    api->print_int(api->mouse_x());
    api->write(" Y=");
    api->print_int(api->mouse_y());
    api->write(" B=");
    api->print_int(api->mouse_buttons());
    api->write(" MOUSEPROBE-DONE\n");
    return 0;
}
