#include "vajra/hal.h"
#include "runtime.h"

/* ------------------------------------------------------------------
 * The user-side half of the syscall boundary, for LOADED programs --
 * see runtime.h's own comment. This is its own copy of exactly the
 * same `int 0x80` invocation hal/x86_64/syscall_invoke.c already has
 * for the kernel image's own built-in demo actors, not a reuse of that
 * one: a loaded program is a completely separate link (its own
 * link.ld, its own address space at PROGRAM_VBASE), so it has no
 * symbol connecting it to that function's actual address inside
 * kernel.bin at link time. No __attribute__((section(".user_text")))
 * needed here either, unlike that copy -- program.ld places this
 * ENTIRE binary at PROGRAM_VBASE, already mapped present+writable+user
 * as one whole window (hal/x86_64/paging.c's
 * hal_address_space_map_program()), not split between supervisor-only
 * and ring-3-reachable ranges the way the kernel image itself is.
 * ---------------------------------------------------------------- */

/* Named raw_syscall, not hal_syscall -- vajra/hal.h (included above
 * for the SYS_* constants, portable and architecture-neutral either
 * way) already declares an extern hal_syscall() for the KERNEL
 * image's own copy (hal/x86_64/syscall_invoke.c); a second, static
 * definition under that same name in this wholly separate link would
 * conflict with that declaration's linkage. */
static uint64_t raw_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

void user_write(const char *str) {
    raw_syscall(SYS_WRITE, (uint64_t)str, 0, 0);
}

void user_exit(void) {
    raw_syscall(SYS_EXIT, 0, 0, 0);
}
