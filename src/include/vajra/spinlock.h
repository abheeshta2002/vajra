#ifndef VAJRA_SPINLOCK_H
#define VAJRA_SPINLOCK_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * A minimal test-and-set spinlock -- the first genuine cross-core
 * synchronization primitive this kernel has needed. Every critical
 * section before Phase 10/Milestone 12 (core/actor.c's scheduler
 * state, in particular) got away with just disabling interrupts,
 * because "interrupts off" was equivalent to "nothing else can
 * possibly run" on the single core that existed. That equivalence
 * breaks the moment a second physical core exists: `cli` on core 0
 * does nothing to stop core 1 from executing at the exact same
 * instant. hal/x86_64/console.c is the first place this kernel
 * actually has two cores racing on shared mutable state
 * (cursor_x/cursor_y) -- see its own comment for the bug this exists
 * to fix, found by deliberately booting without it first.
 *
 * core/actor.c's own scheduler state is NOT yet protected by one of
 * these -- see hal/x86_64/smp.c's top comment for why the AP doesn't
 * touch it at all yet. A future milestone that lets the AP run actor
 * code will need this same treatment applied there too.
 * ---------------------------------------------------------------- */

typedef volatile uint32_t hal_spinlock_t;

static inline void hal_spin_lock(hal_spinlock_t *lock) {
    while (!__sync_bool_compare_and_swap(lock, 0, 1)) {
        __asm__ __volatile__("pause");
    }
}

static inline void hal_spin_unlock(hal_spinlock_t *lock) {
    __sync_lock_release(lock);
}

#endif
