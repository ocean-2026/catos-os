/*
 * sec.h -- v2.67 capability policy + audit trail.
 *
 * The old model asked "is this root, yes/no" in a handful of commands and
 * had no way to see what had been refused.  sec.c gives CatOS a real
 * security surface:
 *
 *   - fine-grained capabilities (NET, KILL, FSADMIN, USERADM, DEVICE, ...)
 *     parsed by name from a compact "NET|REBOOT" string;
 *   - a *deny by default* check (sec_allow) applied centrally in the
 *     dispatcher, so a command cannot forget its own gate;
 *   - every command's requirement overridable at runtime through the
 *     registry (HKSEC\Policy\Cmd\<name>\Caps), no rebuild needed;
 *   - an append-only audit ring: who ran what, who was refused and why.
 */
#ifndef CATOS_SEC_H
#define CATOS_SEC_H

#include <stdint.h>

#define SEC_ALLOW     0
#define SEC_DENIED    1
#define SEC_HOSTILE   2   /* malformed policy entry: fail closed */

/* Parse "NET|KILL|0x0040" into capability bits.  Returns 0 on success,
 * -1 on an unknown token (caller must fail closed). */
int sec_parse_caps(const char *s, uint32_t *out);

/* Render bits as "NET|KILL" (or "none"). */
void sec_caps_str(uint32_t caps, char *out, int max);

/* Bits required to run `cmd` right now (registry override first,
 * built-in table second). */
uint32_t sec_required(const char *cmd, int *bad_policy);

/* Central gate: 0 = allowed, SEC_DENIED / SEC_HOSTILE otherwise.
 * Prints nothing; the dispatcher formats the message. */
int sec_allow(const char *cmd, int uid, uint32_t caps);

/* Record one decision.  kind = "exec" / "deny" / "policy" / "exec-ce". */
void sec_audit(int uid, const char *kind, const char *what, int rc);

/* Dump (or clear) the ring.  Returns number of lines printed. */
int  sec_audit_dump(int n);
void sec_audit_clear(void);

/* Exec policy for a concrete .ce path (v2.67 "怀疑一切"):
 * refuses world-writable binaries when HKSEC\Policy\Exec\Strict=1,
 * returns 0 = allowed, -1 = refused (why may carry a short reason). */
int  sec_exec_check(const char *path, int uid, char *why, int whymax);

/* ---- foreground watchdog (v2.67.1) --------------------------------------
 * The dispatcher arms a deadline before a builtin runs.  Long-running
 * builtins (hang / sleep / pager waits) poll sec_fg_expired(); when the
 * budget is spent the command must release the console instead of
 * freezing the machine.  HKSEC\Policy\Foreground\TimeoutSec sets the
 * budget, AllowUnbounded=1 disables it (old behaviour, opt-in). */
void     sec_fg_arm(const char *cmd);
void     sec_fg_disarm(void);
int      sec_fg_expired(void);      /* 1 = budget spent */
int      sec_fg_should_abort(void);/* poll + print + audit; 1 = abort */
uint32_t sec_fg_used(void);         /* seconds since arm */
const char *sec_fg_cmd(void);       /* command currently holding the console */

/* Effective policy snapshot for the audit report. */
int sec_policy_brief(char *out, int max);

#endif /* CATOS_SEC_H */
