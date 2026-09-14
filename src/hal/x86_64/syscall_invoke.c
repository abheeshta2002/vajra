#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * The user-side half of the syscall boundary -- see syscall.c for the
 * kernel side, and hal.h for the full picture.
 *
 * This function's own compiled code must be reachable at CPL 3 (it's
 * called directly by actor code running in ring 3), so it's marked to
 * live in the .user_text section link.ld carves out and
 * hal/x86_64/paging.c marks user-accessible -- unlike ordinary kernel
 * code, which is supervisor-only and completely unreachable from
 * ring 3. This is the ONE piece of the syscall boundary that has to
 * sit on the ring-3 side of that line; everything past the `int 0x80`
 * instruction below runs at CPL 0 in syscall.c.
 * ---------------------------------------------------------------- */

__attribute__((section(".user_text")))
uint64_t hal_syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}
