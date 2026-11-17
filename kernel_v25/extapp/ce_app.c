/*
 * ce_app.c -- v2.49 shared CE application layer (libc.a 雏形第一步).
 *
 * This translation unit holds the ONE definition of the per-image API
 * pointer slot.  Previously every app declared its own copy of the
 * `.ce_api` section cell; the kernel loader still patches that single
 * 4-byte slot before jumping to ce_main().
 *
 * Pure user-mode string/console helpers live as `static inline` in
 * ce_app.h, so they cost no extra symbol and no per-app copy.  The build
 * links this file next to every app (see extapp/build_ce.sh); an ar(1)
 * archive (libc.a) and a separate crt0 are a later small design step.
 */
#include "ce_app.h"

/* v2.49: weak definition.  Apps not yet converted still declare their own
 * strong `.ce_api` cell; the linker prefers that one and drops this weak
 * copy.  Converted apps (hello/spinner/sysinfo/...) rely on this single
 * shared cell.  Once every app is converted the weak attr can go. */
catos_api_t *__ce_api __attribute__((section(".ce_api"), weak))
    = (catos_api_t *)0;
