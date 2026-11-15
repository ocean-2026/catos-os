/* rps.c - P4 package: one-round rock-paper-scissors against the machine.
 *
 * Usage:  rps rock|paper|scissors   (short: r|p|s)
 * The CPU pick is a tiny LCG seeded from the player's choice hash, so a
 * single round is deterministic per choice but still varies across the
 * three options.  Packaged CE app: `pkg install rps`.
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api"))) = (catos_api_t *)0;

static const char *const NAMES[3] = { "rock", "paper", "scissors" };

static int parse_choice(const char *s) {
    if (!s || !*s) return -1;
    if (s[0] == 'r' || s[0] == 'R') return 0;
    if (s[0] == 'p' || s[0] == 'P') return 1;
    if (s[0] == 's' || s[0] == 'S') return 2;
    return -1;
}

__attribute__((section(".text.ce_entry")))
int ce_main(int argc, char **argv) {
    catos_api_t *api = CE_API();
    if (argc < 2) {
        api->write("usage: rps rock|paper|scissors\n");
        return 1;
    }
    int me = parse_choice(argv[1]);
    if (me < 0) {
        api->write("rps: choose rock, paper or scissors\n");
        return 1;
    }

    /* tiny LCG: seed = hash of the choice string; no clock API needed */
    unsigned seed = 0;
    for (const char *p = argv[1]; *p; p++) seed = seed * 31u + (unsigned char)*p;
    seed = seed * 1103515245u + 12345u;
    int cpu = (int)((seed >> 16) % 3);

    api->write("you: "); api->write(NAMES[me]);
    api->write("   cpu: "); api->write(NAMES[cpu]);
    api->write("   -> ");

    if (me == cpu)            api->write("draw!\n");
    else if ((me + 1) % 3 == cpu) api->write("you lose :(\n");
    else                     api->write("you win!\n");
    return 0;
}
