#ifndef USER_H
#define USER_H
#include <stdint.h>

/*
 * CatOS user + capability security model (v2.13).
 *
 * Two persistent stores on the FAT32 volume:
 *   /etc/passwd  - one line per user:  name:uid:gid:home:shell:caps:pw
 *   /etc/acl     - one line per path:  path:uid:gid:mode   (12-bit unix mode)
 *
 * A task carries its spawning user's uid/gid/caps (task_t).  The kernel
 * gates privileged operations on the caps bitmask below -- this is the
 * "deeply fine-grained, customizable" layer.  root (uid 0) is seeded with
 * CAP_ALL.  The model is enforced kernel-side; ring0 shell commands expose
 * it (whoami/id/useradd/...).  No new CE syscall slots are added, so the
 * 64-slot ABI and existing binaries are untouched.
 */

/* capability bitmask */
#define CAP_NET      0x0001u   /* network: http / dns / udp / ping / ip            */
#define CAP_KILL     0x0002u   /* kill tasks not owned by self                     */
#define CAP_FSADMIN  0x0004u   /* format / fsck                                    */
#define CAP_AUDIO    0x0008u   /* SB16 PCM audio                                   */
#define CAP_USERADM  0x0010u   /* useradd / userdel / passwd / su                  */
#define CAP_DEVICE   0x0020u   /* raw device + low-level hw info                  */
#define CAP_SYS      0x0040u   /* reserved (reboot, kernel introspection)          */
#define CAP_ALL      0xFFFFFFFFu

#define MAX_USERS    32
#define ACL_MAX      256

typedef struct {
    char     name[16];
    uint16_t uid;
    uint16_t gid;
    char     home[48];
    char     shell[32];
    uint32_t caps;
    /* password record, stored verbatim in /etc/passwd field 7:
     *   ""                        = open account (no password)
     *   "<8 hex>"                 = legacy djb2 (pre-v2.22, verified then
     *                               transparently upgraded on next login)
     *   "$S$<8hex><8hex>"         = salted iterated FNV-1a (v2.22),
     *                               8192 rounds, salt is 32-bit random.
     * NOT cryptographic -- no crypto primitives in the kernel; the goal
     * is to resist shoulder-surfing AND cheap offline dictionary scans
     * (the old bare djb2 of a low-entropy password was instant to brute). */
    char     pw[24];
    int      used;
} user_rec_t;

typedef struct {
    char     path[160];  /* FS_PATH_MAX */
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;       /* 12-bit unix: (owner<<6) | (group<<3) | other       */
    int      used;
} acl_entry_t;

/* ---- user database (persistent /etc/passwd) ---- */
void        user_db_init(void);                 /* load or seed                 */
user_rec_t *user_by_name(const char *name);
user_rec_t *user_by_uid(int uid);
user_rec_t *user_at(int idx);        /* 0..MAX_USERS-1, .used may be 0 */
uint32_t    user_caps_of(int uid);              /* 0 if unknown, CAP_ALL if root*/
int         user_check_pw(user_rec_t *u, const char *pw);
int         user_add(const char *name, int uid, int gid,
                     const char *home, const char *shell, uint32_t caps);
int         user_del(const char *name);         /* refuses root                 */
int         user_set_pw(const char *name, const char *pw);
int         user_set_caps(const char *name, uint32_t caps);
int         user_save(void);                     /* rewrite /etc/passwd          */
uint32_t    user_hash_pw(const char *pw);       /* djb2, non-cryptographic     */
int         user_pw_is_set(const user_rec_t *u);/* non-empty password record    */
void        user_pw_set_rec(user_rec_t *u, const char *pw); /* salt+store      */

/* ---- access control list (persistent /etc/acl) ---- */
void        acl_init(void);
int         acl_lookup(const char *path, int *uid, int *gid, int *mode);
int         acl_set(const char *path, int uid, int gid, int mode);  /* insert/replace */
int         acl_default(const char *path, int uid, int gid, int mode);/* seed only   */
int         acl_save(void);

#endif /* USER_H */
