#ifndef FPU_H
#define FPU_H

/* fpu.h - per-task FPU (x87 / SSE) lazy context switching for CatOS.
 *
 * Stage 1 (feat/hosted): the kernel itself stays soft-float
 * (-mno-80387 -mno-sse); this module only saves/restores the coprocessor
 * state on behalf of ring3 CE tasks that use x87/SSE instructions.
 *
 * Model (lazy FPU, classic x86):
 *   - CR0.TS is set on every context switch; the first coprocessor
 *     instruction in the incoming task traps #NM (vector 7).
 *   - The #NM handler saves the previous owner's state into its per-task
 *     512-byte area, restores the current task's state (or fninit for a
 *     fresh task), then clts.
 *   - Instruction set is chosen per CPUID: fxsave/fxrstor when FXSR is
 *     present, otherwise fnsave/frstor (x87 only).
 *
 * The per-task save area lives inside task_t (kept in the `tasks` key
 * table below the ring3 arena); no standalone large .bss array is added.
 */

#include <stdint.h>

struct task;

/* capability bits (CPUID leaf 1 EDX) */
#define FPU_CAP_X87   (1u << 0)   /* x87 FPU present (EDX bit 0)  */
#define FPU_CAP_FXSR  (1u << 1)   /* fxsave/fxrstor (EDX bit 24) */
#define FPU_CAP_SSE   (1u << 2)   /* SSE (EDX bit 25)            */
#define FPU_CAP_SSE2  (1u << 3)   /* SSE2 (EDX bit 26)           */

/* boot-time probe + CR0/CR4 setup.  Called once after tasks_init(). */
void     fpu_init(void);

/* bitmask of FPU_CAP_* detected at boot. */
uint32_t fpu_caps(void);

/* Called on every context switch before resuming the next task: raises
 * CR0.TS so the incoming task traps #NM on its first coprocessor use. */
void     fpu_on_switch(void);

/* #NM (vector 7) lazy trap handler for the current task.  Saves the prior
 * owner, restores (or fresh-inits) the current task, clears TS. */
void     fpu_nm_trap(struct task *cur);

/* Drop FPU ownership when a task exits/dies; its save area is discarded. */
void     fpu_task_cleanup(struct task *t);

/* Shell `fpu` diagnostic: caps, CR0/CR4 bits, owner, and an in-kernel
 * save/restore roundtrip self-test.  Returns bytes written (>=0), -1 on
 * no buffer. */
int      fpu_diag(char *buf, int max);

#endif
