/* elf64.c - P0.6: minimal ELF64 PT_LOAD loader + ring3 entry */
#include "k64.h"

extern void enter_user(u64 entry, u64 stack_top);

u64 user_code_pa;
static u64 user_stack_pa;

/* map a 2 MiB user page: PDPT[1] covers VA 0x40000000..0x40400000 */
static void map_user_window(u64 va, u64 pa)
{
    u64 idx = (va - USER_CODE_VA) / 0x200000;
    if (idx > 1) return;
    pd_user_t[idx] = pa | 0x87;                    /* P | RW | US | PS */
    __asm__ volatile ("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

void user_window_init(void)
{
    user_code_pa  = pmm_alloc_2m();
    user_stack_pa = pmm_alloc_2m();
    map_user_window(USER_CODE_VA,  user_code_pa);
    map_user_window(USER_STACK_VA, user_stack_pa);

    serial_puts("  usermap  : 0x40000000 -> ");
    serial_hex(user_code_pa);
    serial_puts(", 0x40200000 -> ");
    serial_hex(user_stack_pa);
    serial_puts("\n");
}

struct elf64_ehdr {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

void elf64_demo_run(void)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)demo_elf;
    if (!(eh->ident[0] == 0x7F && eh->ident[1] == 'E' &&
          eh->ident[2] == 'L' && eh->ident[3] == 'F')) {
        serial_puts("[k64] demo: not an ELF\n");
        return;
    }
    if (eh->ident[4] != 2 || eh->machine != 62) {
        serial_puts("[k64] demo: not ELF64/x86-64\n");
        return;
    }

    for (u32 i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(demo_elf + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != 1) continue;                       /* PT_LOAD */
        if (ph->vaddr < USER_CODE_VA || ph->vaddr + ph->memsz > USER_CODE_VA + USER_WIN_SIZE) {
            serial_puts("[k64] demo: segment outside user window\n");
            return;
        }
        u8 *dst = (u8 *)(user_code_pa + (ph->vaddr - USER_CODE_VA));
        const u8 *src = demo_elf + ph->offset;
        for (u64 k = 0; k < ph->filesz; k++) dst[k] = src[k];
        for (u64 k = ph->filesz; k < ph->memsz; k++) dst[k] = 0;
    }

    serial_puts("[k64] entering ring3 at ");
    serial_hex(eh->entry);
    serial_puts(" (image ");
    serial_dec(demo_elf_size);
    serial_puts(" bytes)\n");

    enter_user(eh->entry, USER_STACK_TOP);
}

