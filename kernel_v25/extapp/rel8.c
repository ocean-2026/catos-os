/* rel8.c -- 2C block ③ synthetic R_386_RELATIVE(8) probe library.
 *
 * The host linker chooses R_386_32 for local absolute pointers, so a natural
 * PIC build does not emit a RELATIVE reloc.  This tiny library is wrapped by
 * build/mkrel8.py, which hand-inserts one R_386_RELATIVE(8) rela entry
 * targeting the writable `rel8_slot` word, to prove the engine's type-8
 * branch executes at load time and logs
 *   reloc: type=8 slot=0x... base=0x...
 *
 * Build: sh build_so.sh rel8.c bin 0x2710000 val=rel8_val
 * then:  python3 build/mkrel8.py <in.so> <out.so> rel8_slot
 */
#include "ce_abi.h"

catos_api_t *__ce_api __attribute__((section(".ce_api")));

/* Writable word targeted by the hand-crafted RELATIVE reloc.  The reloc
 * writes base + pre-relocation-slot into it; the value is never read by
 * the probe -- the branch execution and its serial line are the evidence.
 * Initialized to a nonzero value so it lands in .data (in the flat image),
 * not .bss. */
int rel8_slot = 0x2a;

int rel8_val(void) { return 42; }
