#ifndef TASK_H
#define TASK_H

/* task.h - processes, scheduling, ring3 syscall interface, permissions.
 *
 * The five pillars meet here:
 *   - processes: PCB array, spawn/exit/kill, round-robin scheduling
 *   - preemption: the PIT ISR preempts ring3 tasks running in user mode
 *     (uniform interrupt frames); kernel code is non-preemptive and
 *     yields cooperatively at blocking points (keyboard, network, disk)
 *   - isolation: each CE program runs in ring3 with a private page
 *     directory (see paging.h) and calls the kernel only through the
 *     int 0x80 gate
 *   - binary compatibility: the kernel maps a per-task "vsyscall" page at
 *     0x2080000 containing int 0x80 thunks; the CE api-pointer table in
 *     that page has the exact shape of catos_api_t, so UNMODIFIED old
 *     .ce binaries run in ring3
 *   - security: every task carries uid/gid/caps from its user record
 *     (/etc/passwd); file access is checked against a persistent unix
 *     owner/group/other ACL (/etc/acl) and privileged syscalls against
 *     the user's capability bitmask (see user.h)
 */

#include <stdint.h>
#include "cpu.h"
#include "paging.h"
#include "user.h"

typedef enum {
    T_UNUSED = 0,
    T_READY,        /* runnable, not on CPU                  */
    T_RUNNING,      /* on the CPU                            */
    T_SLEEPING,     /* until timer wake_tick                 */
    T_WAIT_KB,      /* blocked for a key                     */
    T_WAIT_CHILD,   /* shell waiting for a child to exit     */
    T_ZOMBIE,       /* exited, waiting to be reaped          */
    T_STOPPED,      /* v2.52: suspended by SIGTSTP, held by job control */
} tstate_e;

/* v2.52 minimal signal abstraction for job control.  This is NOT the full
 * POSIX signal set: only the handful a shell needs to stop/continu a job,
 * plus INT/TERM as terminate mappings and CHLD as a parent-notice hint. */
#define SIGCONT  1
#define SIGINT   2
#define SIGTSTP  3
#define SIGCHLD  4
#define SIGTERM  9

#define MAX_TASKS  16
#define TID_SHELL  0
#define UID_ROOT   0
#define UID_USER   100

typedef struct task {
    int         tid;
    int         state;
    int         uid;
    int         gid;            /* group of the spawning user (/etc/passwd) */
    uint32_t    caps;           /* capability bitmask of the spawning user  */
    int         ring0;          /* shell / idle run in ring0           */
    char        name[16];
    uint32_t    kstack_top;     /* kernel stack (ring0: boot stack)    */
    uint32_t    ksp;            /* saved kernel esp when not running   */
    int         resume_frame;   /* 1: ksp -> ISR frame (iretd resume);
                                   0: ksp -> cpu_switch_to frame       */
    task_space_t space;         /* ring3 address space (pt==0 if ring0)*/
    uint32_t    wake_tick;      /* T_SLEEPING wakeup                   */
    int         exit_code;
    struct task *parent;
    int         pgid;          /* v2.52: job/process group id (== tid for a
                                   standalone job; pipelines share one later) */
    int         pending_sig;    /* v2.52: last delivered signal, 0 = none   */
    uint32_t    heap_end;       /* user heap break                     */
    vma_t      *vm;             /* VMA list for this task (NULL if ring0)*/
    /* ls syscall snapshot (one listing at a time per task) */
    int         ls_count;       /* entries in the snapshot  */
    int         ls_step;        /* next index for LS_NEXT   */
    uint32_t    cpu_ticks;      /* timer ticks charged to this task */
    /* v2.5: output / input redirection targets (minimal fd abstraction).
     * out_pipe>=0: SYS_WRITE goes to the pipe; else out_file[0] set:
     * SYS_WRITE appends/truncates that file; else vga.
     * in_pipe>=0: SYS_READLINE reads a line from the pipe; else keyboard. */
    int         out_pipe;
    int         in_pipe;
    char        out_file[160];  /* FS_PATH_MAX; "" = none */
    int         out_append;     /* 1 = append mode (>>) */
    /* #59 background output capture: a task started with '&' has its
     * SYS_WRITE console output diverted into this per-task ring instead of
     * scribbling over the shell prompt.  `log TID` drains it, `log -f TID`
     * follows it until the task exits. */
    int         bg_mode;        /* 1 = background job -> capture stdout */
    int         daemon;         /* v2.54: detached service (survives session,
                                   logged like a bg job; not an interactive job */
    uint16_t    log_head;       /* write index (bytes)                 */
    uint16_t    log_tail;       /* read index  (bytes)                 */
    uint32_t    log_lost;       /* bytes dropped because the ring was full */
    uint8_t     log_buf[1024];
} task_t;

/* ---- lifecycle ------------------------------------------------------- */
void     tasks_init(void);
task_t  *task_current(void);
int      tasks_ready(void);
int      task_spawn_ce(const char *path, int argc, char **argv, int uid);
/* P1: flatten a supported PE32 into a CE01 .ce file (shell convert-on-run).
 * err/errsz receive a human-readable failure reason; returns 0 / negative. */
int      pe_to_ce_file(const char *pe_path, const char *ce_path,
                       char *err, int errsz);
void     task_exit_current(int code) __attribute__((noreturn));
int      task_kill(int tid, int by_uid);
void     task_wait_child(task_t *child);
/* v2.52: minimal signal delivery.  Returns 0 ok, -1 no such task. */
int      task_signal(int tid, int sig);
/* Wait for a child to either exit (reaped, returns 0) or stop (returns 1). */
int      task_wait_child_or_stop(task_t *child);

/* ---- scheduling ------------------------------------------------------ */
void     task_yield(void);            /* cooperative: give up the CPU now */
void     task_maybe_yield(void);      /* cheap poll-loop helper           */
void     task_block(int state);       /* block current until woken        */
void     task_sleep_ticks(uint32_t ticks);
void     task_tick_wake(void);        /* timer IRQ hook                   */
uint32_t sched_timer_tick(int_frame_t *f);   /* timer stub hook           */
void     task_die_fault(int_frame_t *f) __attribute__((noreturn));

/* ---- info ------------------------------------------------------------ */
int      task_count(void);
void     task_ps_print(void);
task_t  *task_by_tid(int tid);
uint32_t task_uptime_ms(void);

/* ---- misc ------------------------------------------------------------ */
int      task_count(void);
void     task_ps_print(void);
task_t  *task_by_tid(int tid);
uint32_t task_uptime_ms(void);

/* ---- per-task stats for `top` (kernel side) --------------------------- */
int      task_stat(int slot, int *tid, uint32_t *cpu_ticks, uint32_t *pages,
                   int *state, int *uid, int *ring, char *name, int namelen);
uint32_t task_idle_ticks(void);

/* ---- #59 background output capture ------------------------------------ */
void     task_log_append(task_t *t, const char *buf, uint32_t len);
/* Drain up to max bytes of pending log text into out; returns byte count.
 * Also reports bytes dropped since the last drain in *lost (may be NULL). */
int      task_log_drain(task_t *t, char *out, int max, uint32_t *lost);
int      task_log_pending(task_t *t);

/* ---- VMA-backed user-pointer helpers (declared in vma.c) ------------- */
/* Check a single pointer is inside the task's VMA list (any page). */
int      uaddr_in_vma(task_t *t, uint32_t addr);
/* Validate [addr, addr+len) is within the task's VMA list.
 * Maps any unmapped pages on demand (anonymous zero-fill).
 * Returns 0 ok, -14 EFAULT. */
int      validate_uaddr(task_t *t, uint32_t addr, uint32_t len);
/* Safe copy: kernel->user or user->kernel with VMA-backed validation.
 * Returns 0 ok, -14 EFAULT. */
int      copy_to_user(task_t *t, void *dst, const void *src, uint32_t len);
int      copy_from_user(task_t *t, void *dst, const void *src, uint32_t len);

/* ---- file permissions (implemented in user.c) -------------------------- */
#define PERM_READ  0x1
#define PERM_WRITE 0x2
#define PERM_EXEC  0x4
int      fs_perm_check(const char *path, int uid, int gid,
                      int need);                 /* need = PERM_* bits */

/* ---- console input ownership ------------------------------------------ */
int      console_try_own(void);
int      console_owned_by_current(void);
void     console_release(void);
void     console_wake_all(void);        /* IRQ hook: keys arrived */
void     console_wake_release(int tid);

/* ---- syscall entry (cpu.s -> here) ------------------------------------ */
void     isr_syscall(int_frame_t *f);
/* page-fault handler: returns 0=retry (fault handled), -14=kill task */
int      task_page_fault(int_frame_t *f);

/* ---- v2.5 pipes: ring0 helpers for the shell -------------------------- */
int      kpipe_create(void);        /* handle >= 0, -1 if no slots */
void     kpipe_assign_out(int tid, int h);
void     kpipe_assign_in(int tid, int h);

#endif
