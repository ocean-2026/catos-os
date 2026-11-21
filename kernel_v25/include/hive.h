#ifndef HIVE_H
#define HIVE_H

/*
 * hive.h -- CatOS binary configuration hive (v2.67).
 *
 * Replaces the v2.62 INI text file /etc/reg.  Rationale, in one line:
 * a text store cannot carry types, cannot express a tree, cannot detect
 * a torn write, and silently truncates on overflow.  A binary hive can do
 * all four, and "trust nothing" is easier to hold when the on-disk state
 * is checksummed instead of merely parsed.
 *
 * ---- ON-DISK FORMAT (/etc/hive) ----------------------------------------
 *
 *   offset  size  field
 *   0       8     magic "CHV1\r\n\x1a"
 *   8       4     format version (1)
 *   12      4     generation (bumped on every successful save)
 *   16      4     node count
 *   20      4     payload length in bytes
 *   24      4     CRC-32 of the node area (IEEE 802.3 polynomial)
 *   28      4     header CRC-32 over bytes 0..27
 *   32      n*122 node records
 *
 * Node record (122 bytes, little-endian):
 *   u16 parent, u16 child, u16 sibling      (0xFFFF = none)
 *   u8  kind     0 = key, 1 = REG_SZ, 2 = REG_DWORD, 3 = REG_BINARY
 *   u8  namelen
 *   u16 datalen
 *   char name[32]
 *   u8  data[80]
 *
 * ---- SECURITY POSITION ("trust nothing") -------------------------------
 *   - every byte read off the volume is validated: magic, version, header
 *     CRC, payload CRC, node count, per-node link bounds, name lengths,
 *     data lengths, parent/child/sibling acyclicity.  A hive that fails
 *     ANY check is quarantined wholesale (.bad) and defaults are used.
 *   - a save is verified by reading the file back and comparing CRCs; on
 *     mismatch the previous content is restored from .bak and the call
 *     fails loudly instead of leaving a half-written store.
 *   - values never truncate silently: an over-long name/value is refused
 *     with HIVE_E2BIG.
 *   - the hive is a settings store, not a security boundary; permission
 *     checks still belong to the caller (uid/gid + cap gate), and writes
 *     go through fs_write_raw_mode with the caller's mode.
 *
 * ---- PATH SYNTAX -------------------------------------------------------
 *   Case-insensitive, '\' or '/' separated, rooted:
 *
 *     HKLM\System\Language
 *     HKLM\Software\CatOS\Terminal\Colors\Background
 *     HKCU\Terminal\Colors\Command             (per-session subtree)
 *     HKSEC\Policy\Cmd\rm\Allow
 *
 *   Roots: HKLM (machine), HKCU (current user), HKSEC (security policy).
 *   HKCU is stored in the hive as HKU\<uid> and re-rooted per session, so
 *   two logged-in users can hold different terminal themes on one image.
 */

#include <stdint.h>

#define HIVE_PATH       "/etc/hive"
#define HIVE_BAK        "/etc/hive.bak"
#define HIVE_BAD        "/etc/hive.bad"

#define HIVE_MAGIC      "CHV1\r\n\x1a"
#define HIVE_VERSION    1u
#define HIVE_MAX_NODES  384
#define HIVE_NAME_MAX   32
#define HIVE_DATA_MAX   80
#define HIVE_PATH_MAX   200
#define HIVE_MAX_DEPTH  12

/* value kinds (also the on-disk `kind` byte) */
#define HIVE_KEY        0u
#define HIVE_SZ         1u
#define HIVE_DW         2u
#define HIVE_BIN        3u

/* return codes */
#define HIVE_OK         0
#define HIVE_ENOENT   (-1)   /* key/value not found                  */
#define HIVE_EINVAL   (-2)   /* malformed path / argument            */
#define HIVE_EFULL    (-3)   /* node table exhausted                 */
#define HIVE_E2BIG    (-4)   /* name or value too long (NOT truncated)*/
#define HIVE_EIO      (-5)   /* volume not ready / write failed      */
#define HIVE_ECORRUPT (-6)   /* on-disk image failed validation      */
#define HIVE_ENOTDIR  (-7)   /* a path component is a value, not key */
#define HIVE_EPERM    (-8)   /* caller lacks permission              */
#define HIVE_ECODE    (-9)   /* read-back verification mismatch      */
#define HIVE_ESTALE  (-10)   /* image valid but stale vs the live tree*/

typedef struct {
    uint8_t  kind;              /* HIVE_SZ / HIVE_DW / HIVE_BIN       */
    uint16_t len;               /* datalen                            */
    uint8_t  data[HIVE_DATA_MAX];
} hive_val_t;

typedef struct {
    uint32_t nodes;             /* live node count                    */
    uint32_t values;            /* key vs value breakdown             */
    uint32_t keys;
    uint32_t generation;
    uint32_t bytes;             /* serialized size                    */
    int      loaded;            /* 1 = read from disk, 0 = defaults   */
    int      migrated;          /* 1 = converted from text this boot  */
    int      corrupt;           /* 1 = on-disk image was quarantined  */
    uint32_t crc;
} hive_stat_t;

/* Boot: load /etc/hive, or migrate /etc/reg + legacy one-key files, or
 * seed defaults.  Always leaves a usable in-memory tree.  0 on success
 * (which includes "seeded defaults"), negative only when the volume is
 * unusable AND nothing could be seeded. */
int  hive_init(void);

/* Load / save.  hive_save() is atomic-by-verify: write, read back, CRC
 * compare, restore .bak on mismatch.  Returns HIVE_OK / HIVE_EIO /
 * HIVE_ECODE. */
int  hive_load(void);
int  hive_save(void);
int  hive_dirty(void);
/* v2.67: validate /etc/hive WITHOUT touching the live node table.
 * HIVE_OK / HIVE_ENOENT / HIVE_ECORRUPT / HIVE_ESTALE */
int  hive_check_image(int *out_len);
void hive_stat(hive_stat_t *st);

/* ---- read -------------------------------------------------------------*/
/* hive_sz() never returns NULL; a missing key yields `def` (which may be
 * NULL, in which case "" is returned). */
const char *hive_sz(const char *path, const char *def);
/* current HKCU owner (shell session uid); HKCU\... paths are rewritten
 * to HKU\<uid>\... before any lookup. */
void        hive_set_user(int uid);
uint32_t    hive_dw(const char *path, uint32_t def);
int         hive_get(const char *path, hive_val_t *out);   /* typed      */
int         hive_exists(const char *path);
int         hive_kind(const char *path);                   /* -1 missing */

/* ---- write ------------------------------------------------------------*/
int  hive_set_sz(const char *path, const char *val);
int  hive_set_dw(const char *path, uint32_t val);
int  hive_set_bin(const char *path, const uint8_t *data, uint16_t len);
int  hive_set(const char *path, const hive_val_t *v);
int  hive_del(const char *path);            /* value only              */
int  hive_delkey(const char *path);         /* subtree incl. children  */
int  hive_mkkey(const char *path);

/* ---- enumerate --------------------------------------------------------*/
/* Children of a key.  index is 0..count-1 in insertion order.
 * Returns HIVE_OK and fills name/kind, or HIVE_ENOENT past the end. */
int hive_child_count(const char *keypath);
int hive_child_at(const char *keypath, int index,
                  char *name, int namecap, uint8_t *kind);
/* Full path of a child (helper for `hive ls -R`). */
int hive_child_path(const char *keypath, int index, char *out, int cap);

/* ---- migration / interchange -----------------------------------------*/
/* Parse a legacy INI text blob into the hive under `root` (section -> key).
 * Used once for /etc/reg; returns the number of values imported. */
int hive_import_ini(const char *a_root, const char *text, int len);
/* Dump the whole tree as INI-ish text (for `hive export` / `hive dump`).
 * Returns bytes written, or -1 when `cap` is too small. */
int hive_dump(char *out, int cap);
/* Validate the in-memory tree (links, cycles, kinds). 0 = sane. */
int hive_verify(void);

/* ---- compat shims for the v2.62 call sites ----------------------------*/
/* Section/key pairs used by shell/lang/bsod map onto real hive paths; the
 * mapping lives in hive.c so there is ONE source of truth. */
const char *hive_compat_get(const char *section, const char *key);
int         hive_compat_set(const char *section, const char *key,
                            const char *value);

#endif /* HIVE_H */
