#include "runtime.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 16's own verification target: a genuinely separately-
 * compiled program, never linked into kernel.bin at all -- see
 * tools/build-c.ps1's program-build step and
 * src/hal/x86_64/hello_blob.asm for how its compiled bytes reach the
 * kernel image as opaque data (the same incbin technique
 * ap_trampoline_blob.asm already uses), and core/main.c for how they
 * get seeded into a storage object at boot and actually loaded.
 *
 * _start lives in its own ".text.start" section (program.ld places it
 * first) so it lands at exactly PROGRAM_VBASE regardless of whatever
 * order the compiler would otherwise emit functions in -- entry_offset
 * is 0, not something extracted from a symbol table after the fact.
 * ---------------------------------------------------------------- */

__attribute__((section(".text.start")))
void _start(void) {
    user_write("Hello from a genuinely loaded program -- Phase 16's loader works.\n");
    user_exit();
}
