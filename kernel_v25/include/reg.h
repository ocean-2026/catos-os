#ifndef REG_H
#define REG_H

/*
 * reg.h -- CatOS file-backed registry (v2.62).
 *
 * A single INI-style text file /etc/reg is the canonical store for all
 * small system settings.  Sections group related keys:
 *
 *   [system]
 *   lang=en
 *   theme=dark
 *   panicmode=reboot
 *   videomode=gfx24
 *   size=1
 *
 *   [env]
 *   PATH=/bin
 *   PS1=
 *
 * Design constraints (kept deliberately simple for a ring0 shell):
 *   - fixed static table, no heap: REG_MAX_SECTIONS x REG_MAX_KEYS
 *   - all values are NUL-terminated strings (callers parse numbers/enums)
 *   - mutating API marks dirty; reg_flush() writes the WHOLE file back
 *   - reg_init() seeds a default table when /etc/reg is absent
 *   - readers fall back to the legacy /etc/<name> files so an old volume
 *     or a hand-edited file keeps working (migration is additive, never
 *     destructive).  See each legacy loader in its own .c.
 *
 * Case sensitivity: section and key names are case-insensitive (ASCII),
 * values are stored verbatim.  Leading/trailing spaces around '=' are
 * trimmed; '#' at column 0 starts a comment line.
 */

#define REG_MAX_SECTIONS  16
#define REG_MAX_KEYS      32
#define REG_KEY_MAX       32
#define REG_VAL_MAX       128
#define REG_SEC_MAX       24

/* Load /etc/reg into the static table; seed defaults if missing.
 * Safe to call once at shell boot.  Returns 0 on ok, -1 if the FS is not
 * ready (the in-memory default table is still usable). */
int  reg_init(void);

/* Lookup.  Returns the value string (never NULL; "" when absent).
 * section/key are case-insensitive. */
const char *reg_get(const char *section, const char *key);

/* Set / delete.  0 = ok, -1 = full / bad argument.  Both mark dirty but
 * do NOT write to disk -- call reg_flush() when done (batches writes). */
int  reg_set(const char *section, const char *key, const char *value);
int  reg_del(const char *section, const char *key);

/* Write the whole table back to /etc/reg.  0 = ok. */
int  reg_flush(void);

/* Set + flush in one step (convenience for shell commands). */
int  reg_set_now(const char *section, const char *key, const char *value);
int  reg_del_now(const char *section, const char *key);

/* Count keys in a section; iterate by index.  Used by `reg list`. */
int  reg_key_count(const char *section);
const char *reg_key_at(const char *section, int index);
const char *reg_val_at(const char *section, int index);

/* Number of sections / section name by index (for `reg list` with no arg). */
int  reg_section_count(void);
const char *reg_section_at(int index);

/* True when the backing file existed at reg_init() time. */
int  reg_loaded_from_file(void);

#endif /* REG_H */
