#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* paging.c - frame allocator + per-task address spaces (see cpu.h for the
 * descriptor/interrupt side).  User virtual layout lives in 32..36 MiB:
 *   0x2000000 image+bss   0x2080000 vsyscall   0x2100000 heap
 *   0x2200000 stack top (grows down from 0x21F0000..0x2200000)
 */

void     paging_init(uint32_t mem_lower_kb, uint32_t mem_upper_kb);
uint32_t paging_kdir(void);

uint32_t frame_alloc(void);
void     frame_stats(uint32_t *total, uint32_t *used);   /* 4 KiB frames */
void     frame_free(uint32_t pa);
void    *kzalloc_frame(void);          /* one zeroed frame, kernel-virtual */

typedef struct task_space {
    uint32_t dir;
    uint32_t pt;
} task_space_t;

int      task_space_create(task_space_t *s);
int      task_map_page(task_space_t *s, uint32_t vaddr, uint32_t pa, int user_writable);
uint32_t task_virt_to_phys(task_space_t *s, uint32_t vaddr);
void     task_space_destroy(task_space_t *s);

#define USER_IMAGE_BASE 0x2000000u
#define USER_VSYSCALL   0x2080000u
#define USER_HEAP_BASE  0x2100000u
#define USER_HEAP_MAX   0x40000u       /* 256 KiB */
#define USER_STACK_TOP  0x2200000u
#define USER_STACK_SZ   0x10000u       /* 64 KiB */
#define USER_ARENA_END  0x2400000u

#endif
