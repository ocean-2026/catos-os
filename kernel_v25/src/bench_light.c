/* bench_light.c -- "light" performance axes: boot, shell command round trip,
 * syscall round trip, login (password digest) path.
 *
 * STUB: the scenario bodies are supplied by the perf-bench owner of this
 * axis.  Keep every scenario self-contained here (no edits to shell.c or
 * task.c beyond the timing hooks that already exist) so the two bench
 * modules can be developed in parallel without touching the same files.
 */
#include "perf.h"

int bench_light_cmd(const char *arg) {
    (void)arg;
    return 0;
}
