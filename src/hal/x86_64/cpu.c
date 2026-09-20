#include "vajra/hal.h"
#include "vajra/spinlock.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 10 (remainder): what a second core needs before it can
 * run actors -- a way to ask "which core am I?" from ANY address
 * space, and one lock that makes the kernel's single-core assumptions
 * true again.
 *
 * hal_cpu_id(): CPUID leaf 1 reports the executing core's initial APIC
 * ID in EBX[31:24]. Unlike reading the LAPIC's own ID register
 * (hal_lapic_id()), that needs no MMIO mapping, so it works under
 * every actor's private CR3 -- and unlike GS-base per-CPU data, ring-3
 * code can't corrupt it (a user `mov gs, ax` would reset a GS base).
 * The APIC ID doubles as the core's index into per-core arrays, which
 * holds on QEMU (IDs 0..N-1) and is checked against MAX_CPUS below;
 * an ACPI MADT walk would replace that assumption on real hardware.
 *
 * The kernel lock (a "big kernel lock", as early Linux/BSD SMP had):
 * every path that enters the kernel from ring 3 -- syscall, timer,
 * keyboard/mouse IRQ, fault -- takes it, and it is released only when
 * that core goes back to ring 3 (or idles). Ring-3 code, i.e. all the
 * actors' own work, runs truly in parallel on both cores; kernel work
 * is serialized. That is deliberately the coarsest lock that is still
 * correct: every kernel global that today relies on "interrupts are
 * off, so only one caller exists" (storage.c's and loader.c's scratch
 * buffers, syscall.c's safe_string_buf, actors[], the allocator...)
 * stays valid unchanged, instead of each needing its own audit and
 * finer lock first. Narrowing it is a later, measurable step.
 *
 * Ownership is per CORE, not per actor: an actor blocked in the
 * kernel is switched away from while its core keeps holding the lock,
 * and whichever core later resumes it is holding the lock at that
 * moment too -- so "the lock is held whenever a core runs kernel code"
 * is the invariant, and kernel_lock_held[] is what lets a nested
 * interrupt on the same core (e.g. a timer tick arriving in kernel
 * code) see that it already has it instead of deadlocking on itself.
 * ---------------------------------------------------------------- */

int hal_cpu_id(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    int id = (int)((ebx >> 24) & 0xFF);
    return (id < MAX_CPUS) ? id : 0;
}

/* A TICKET lock, not the test-and-set hal_spin_lock(): with two cores
 * hammering the kernel (an idle shell polling for keys is a syscall
 * loop), a test-and-set lock is unfair -- the core that just released
 * it can win it straight back, and the other core's syscall waits an
 * unbounded time. Seen as the boot demo taking anywhere from 15 to 100+
 * seconds under -smp 2. Tickets are served strictly in arrival order. */
static volatile uint32_t kernel_lock_next;    /* next ticket to hand out */
static volatile uint32_t kernel_lock_serving; /* ticket currently allowed in */
static volatile int kernel_lock_held[MAX_CPUS];

/* Raw COM1 output -- no console lock, no allocation, nothing that could
 * itself need the lock being diagnosed. */
static void raw_serial_str(const char *str) {
    while (*str) {
        __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)*str), "Nd"((uint16_t)0x3F8));
        str++;
    }
}

static void raw_serial_hex(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4) {
        int nib = (int)((v >> i) & 0xF);
        char c = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
        __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
    }
}

/* Interrupts must be OFF when called (every interrupt-gate entry and
 * the scheduler's own idle path guarantee it). Returns 1 if this call
 * actually took the lock -- pass that to hal_kernel_leave(), which is
 * then a no-op for a nested entry that found the lock already held. */
int hal_kernel_enter(void) {
    int cpu = hal_cpu_id();
    if (kernel_lock_held[cpu]) {
        return 0;
    }
    /* Lock watchdog: a healthy wait is microseconds. If this core spins
     * for an absurd count, something holds the lock forever (a leak, or
     * a deadlock against another lock) -- report who's waiting, who
     * owns it and where we were called from, ONCE, on the serial port,
     * then keep waiting. Diagnostic only: added chasing an intermittent
     * -smp 2 stall, and cheap enough to keep. */
    uint64_t spins = 0;
    int reported = 0;
    uint32_t ticket = __sync_fetch_and_add(&kernel_lock_next, 1);
    while (kernel_lock_serving != ticket) {
        __asm__ __volatile__("pause" : : : "memory");
        if (++spins == 200000000ULL && !reported) {
            reported = 1;
            raw_serial_str("\n[LOCK WATCHDOG] core ");
            raw_serial_hex((uint64_t)cpu);
            raw_serial_str(" has waited for the kernel lock too long; held flags ");
            raw_serial_hex((uint64_t)kernel_lock_held[0]);
            raw_serial_str(" ");
            raw_serial_hex((uint64_t)kernel_lock_held[1]);
            raw_serial_str("; called from ");
            raw_serial_hex((uint64_t)__builtin_return_address(0));
            raw_serial_str("\n");
        }
    }
    kernel_lock_held[cpu] = 1;
    return 1;
}

void hal_kernel_leave(int took) {
    if (!took) {
        return;
    }
    kernel_lock_held[hal_cpu_id()] = 0;
    __sync_synchronize();
    kernel_lock_serving++; /* only the holder writes this, so a plain increment is safe */
}
