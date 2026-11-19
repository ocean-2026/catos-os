#include "ce_app.h"

/* demo.c - beginner-friendly tour of the CatOS CE API (v2.0, 25 calls).
 * Exercises every new helper and shows its result on screen.
 * Run it with:  run /bin/demo.ce
 */
__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    char name[32];
    (void)argc; (void)argv;

    api->set_fg(2);                      /* green title */
    api->write("=== CatOS API Demo ===\n");
    api->set_fg(7);                      /* back to light grey */

    api->write("int : ");
    api->print_int(-12345);
    api->write("\n");

    api->write("hex : ");
    api->print_hex(0xDEADBEEFu);
    api->write("\n");

    ce_strcpy(name, "CatOS");
    api->write("strcpy -> '");
    api->write(name);
    api->write("'  strlen = ");
    api->print_int(ce_strlen(name));
    api->write("\n");

    if (ce_strcmp(name, "CatOS") == 0)
        api->write("strcmp: equal\n");
    else
        api->write("strcmp: NOT equal\n");

    api->write("atoi(\"  42 \") = ");
    api->print_int(ce_atoi("  42 "));
    api->write("\n");

    if (api->file_exists("/bin/calc.ce"))
        api->write("file_exists /bin/calc.ce: YES\n");
    else
        api->write("file_exists /bin/calc.ce: NO\n");

    api->write("fsize(/bin/calc.ce) = ");
    api->print_int(api->fsize("/bin/calc.ce"));
    api->write("\n");

    api->write("delay(60ms) ... ");
    api->delay(60);
    api->write("done\n");

    api->write("press any key to exit ...\n");
    api->getch();
    api->write("bye from demo!\n");

    api->exit(0);                        /* exit path: back to shell */
    return 0;
}
