/*
 * dash.h -- v2.67 full-screen TUI dashboard (the TUI toolkit's demo).
 */
#ifndef CATOS_DASH_H
#define CATOS_DASH_H

#include <stdint.h>

/* Run the interactive dashboard until the user quits.  `uid`/`caps` come
 * from the shell session (dash.c must not reach into shell globals). */
/* start = initial page (0..3): 0 system, 1 storage, 2 network, 3 security;
 * `dash storage` opens straight on that page so a screenshot run does not
 * have to synthesise keystrokes. */
void dash_run(int uid, uint32_t caps, int start);

#endif
