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
    T_WAIT_PIPE,    /* perf(pipe): blocked on pipe space/data,
                       woken by the peer's write/read/close   */
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

/* 3A-2: process descriptor.  Owns the address space and all process-shared
 * resources (page space, VMA list, heap root, fd table, pipe/console/log
 * ownership).  The main thread embeds one; worker threads reference it.
 * Full field migration from task_t lands with the 3A-3 thread path; this
 * forward declaration only lets task_t hold the back-pointer. */
struct proc;

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

    /* ---- 3A-2 proc/thread split scaffolding --------------------------
     * Existing single-thread tasks map onto the new model as: main thread ==
     * sole thread, owning an implicit process.  The process-shared fields
     * (space/vm/heap/fd/pipe/console/log) still live in task_t this step;
     * they move under `proc` when worker threads (3A-3) share an address
     * space.  The thread-side fields below are placeholders wired up in 3A-3. */
    struct proc *proc;          /* owning process; NULL = implicit self proc */
    int         is_thread;      /* 0 = main thread/process, 1 = worker thread */
    int         detached;       /* 1 = join not required, auto-reap on exit */
    uint32_t    tls_va;         /* per-thread TLS block VA (3A-3)          */
    uint32_t    thr_stack_va;   /* worker user-stack VMA base (3A-3)       */
    int         thread_ret;      /* join retval                              */
    struct task *waiter;        /* task blocked in thread_join on this one  */
    uint32_t    kstack_va;      /* kvheap kernel stack VA (3A-3); 0 = legacy static */
    /* Stage 1 FPU: 512-byte coprocessor save area, 16-byte aligned for
     * fxsave.  fpu_used=1 means fpu_save holds live state that may need
     * restoring; owned lazily (see fpu.h). */
    uint8_t     fpu_save[512] __attribute__((aligned(16)));
    int         fpu_used;
    /* ls syscall snapshot (one listing at a time per task) */
    int         ls_count;       /* entries in the snapshot  */
    int         ls_step;        /* next index for LS_NEXT   */
    uint32_t    cpu_ticks;      /* timer ticks charged to this task */
    uint32_t    switches;       /* times this task was picked up by the scheduler */
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

/* Minimal preempt-nesting count (single core).  Critical sections that touch
 * shared kernel state wrap their body in preempt_disable()/preempt_enable();
 * the timer tick and syscall-exit reschedule only fire when the depth is 0.
 * This kernel remains cooperatively scheduled in ring0 -- the count defers a
 * pending reschedule until the section drops, it does not make ring0 preemptive. */
void     preempt_disable(void);
void     preempt_enable(void);
int      preempt_depth(void);
task_t  *task_current(void);
int      tasks_ready(void);
int      task_spawn_ce(const char *path, int argc, char **argv, int uid);
/* v2.71: spawn a ring0 kernel thread (entry never returns; cooperative) */
int      task_spawn_r0(void (*entry)(void), const char *name, int daemon);
/* P1: flatten a supported PE32 into a CE01 .ce file (shell convert-on-run).
 * err/errsz receive a human-readable failure reason; returns 0 / negative.
 * uid/gid are the session identity used for the target-path write check
 * (#182); the conversion never writes a path the caller may not write. */
int      pe_to_ce_file(const char *pe_path, const char *ce_path,
                       int uid, int gid, char *err, int errsz);
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
/* Ring0 FIFO accessors mirroring the SYS_PIPE_WRITE / SYS_PIPE_READ paths
 * (same static pipes, same blocking semantics): no user-pointer copy or
 * validation.  Used by the heavy bench scenarios; do not call from ISR. */
int      kpipe_write(int h, const void *data, int len);
int      kpipe_read(int h, void *buf, int max);
void     kpipe_close_write(int h);
void     kpipe_close_read(int h);
/* Number of task_sleep_ticks() blockings across the pipe helpers since boot. */
uint32_t kpipe_sleeps(void);
/* perf(pipe): TSC cycles spent blocked waiting on the FIFO (wakeup latency
 * included) and cycles spent in FIFO byte copy, accumulated since boot. */
uint64_t kpipe_wait_cycles(void);
uint64_t kpipe_copy_cycles(void);

#endif
