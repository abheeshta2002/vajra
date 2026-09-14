#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Local APIC driver -- the piece SMP bring-up needs that the PIT/8259
 * PIC pair (hal/x86_64/pic.c, timer.c) can't provide: every core has
 * its OWN local APIC, and the only way to wake a sleeping Application
 * Processor at all is an Inter-Processor Interrupt sent through it
 * (INIT, then SIPI -- "Startup IPI", whose vector field is the
 * physical page number the AP starts executing at, not an ordinary
 * interrupt vector). See hal/x86_64/smp.c for the trampoline this
 * hands the AP off into.
 *
 * The local APIC's registers are memory-mapped at a fixed physical
 * address (0xFEE00000, architectural on every x86-64 CPU that has
 * one), far outside the 0-256MB boot.asm identity-maps -- see
 * hal_map_lapic_mmio() (hal/x86_64/paging.c) for the one extra mapping
 * this requires before anything here can be touched.
 * ---------------------------------------------------------------- */

#define LAPIC_BASE   0xFEE00000ULL
#define LAPIC_ID     0x020 /* this core's own APIC ID, top 8 bits of the register */
#define LAPIC_SVR    0x0F0 /* spurious-interrupt vector register; bit 8 = APIC software enable */
#define LAPIC_ICR_LO 0x300

#define IA32_APIC_BASE_MSR    0x1B
#define IA32_APIC_BASE_ENABLE (1u << 11)

static inline volatile uint32_t *lapic_reg(uint32_t offset) {
    return (volatile uint32_t *)(LAPIC_BASE + offset);
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Enables THIS core's own local APIC -- per-core state (the MSR is
 * banked per-core in hardware, and so is every register above despite
 * sharing one fixed physical address), so every core that wants to
 * send or receive IPIs must call this itself, not just the BSP. */
void hal_lapic_enable(void) {
    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    wrmsr(IA32_APIC_BASE_MSR, base | IA32_APIC_BASE_ENABLE);

    /* Bit 8 enables the APIC's interrupt delivery (distinct from the
     * MSR bit above, which is closer to "is the hardware unit powered
     * at all"); the low byte is the spurious-vector number, set to an
     * otherwise-unused value -- this kernel doesn't register a real
     * handler for it, since a genuine spurious interrupt here would
     * just mean "nothing to do", not a fault. */
    *lapic_reg(LAPIC_SVR) = 0x1FF;
}

uint32_t hal_lapic_id(void) {
    return (*lapic_reg(LAPIC_ID)) >> 24;
}

/* A fixed hardware delay -- the exact mechanism legacy-asm's own SMP
 * bring-up used (kernel/kernel.asm's setup_smp), proven in this same
 * QEMU environment to give an AP enough time to actually latch INIT
 * before SIPI arrives. There's no calibrated timer available this
 * early (hal_timer_init() only starts ticking once the PIC is
 * unmasked, well after this runs), so a plain busy-loop is what's
 * available. */
static void ipi_delay(void) {
    for (volatile uint32_t i = 0; i < 0x100000; i++) {
    }
}

/* Broadcasts INIT then SIPI to every other core ("all excluding self"
 * -- destination shorthand 0b11 in the ICR's bits 19:18), the
 * architectural two-IPI sequence that wakes a sleeping AP and starts
 * it executing at physical address (trampoline_page * 4096) in 16-bit
 * real mode. This function only sends the wake-up signal -- it
 * doesn't know or care what code is actually sitting there; see
 * hal/x86_64/smp.c for making sure the trampoline is copied into
 * place first. */
void hal_lapic_send_init_sipi(uint8_t trampoline_page) {
    volatile uint32_t *icr_lo = lapic_reg(LAPIC_ICR_LO);

    *icr_lo = 0x000C4500u; /* INIT, all-excluding-self, assert */
    ipi_delay();

    *icr_lo = 0x000C4600u | trampoline_page; /* SIPI, all-excluding-self, vector = page number */
    ipi_delay();
    /* Real hardware wants SIPI sent twice for reliability; QEMU's
     * emulated APIC (and every target this kernel is likely to meet)
     * is satisfied by one, matching legacy-asm's own working version. */
}
