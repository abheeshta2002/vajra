#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 10, deliberately scoped narrow: bring up exactly one
 * additional physical core and PROVE it is genuinely, independently
 * executing in parallel with the BSP -- not yet folding it into the
 * actor scheduler. core/actor.c's actors[]/current_actor/
 * schedule_next() remain entirely BSP-only in this milestone; the AP
 * never spawns, runs, or touches a single actor. That's real,
 * deliberately deferred work (per-core run queues, a per-core
 * current_actor, the AP's own preemption timer, extending
 * hal_address_space_create() so actor-context code can reach the
 * LAPIC too), the same way Phase 3 (per-actor address spaces) shipped
 * before Phase 4 (ring 3) rather than both at once -- see
 * docs/ROADMAP.md's own Phase 10 entry.
 *
 * What this DOES prove, genuinely: a second physical core can be
 * woken from a cold, sleeping state via the real INIT-SIPI-SIPI
 * hardware sequence (hal/x86_64/apic.c), transition through 16-bit
 * real mode -> 32-bit protected mode -> 64-bit long mode entirely on
 * its own (hal/x86_64/ap_trampoline.asm), and then run ordinary,
 * linked C kernel code -- concurrently with the BSP, not merely
 * "afterward" -- for the first time in this project's C rewrite.
 *
 * Low-memory layout this file depends on (verified free of every
 * other fixed structure boot.asm/paging.c/gdt.c/e820.c already use --
 * see their own comments for what occupies 0x20000-~0x80a0c,
 * 0x90000-0x93FFF, 0x94000+):
 *   0x96000-0x96FFF  AP trampoline code (copied from ap_trampoline_blob)
 *   0x96FF0          AP_BOOT_FLAG_ADDR -- 0 until the AP writes (its APIC ID + 1)
 *   0x96FF8          AP_ENTRY_PTR_ADDR -- BSP writes ap_entry_c's address here
 *                                         before sending SIPI; the trampoline,
 *                                         assembled as its own standalone,
 *                                         position-independent blob with no
 *                                         visibility into this kernel's own
 *                                         symbol table, reads it back to hand
 *                                         off into real, linked kernel code.
 *   0x99000          top of the AP's own dedicated stack
 *
 * Originally 0x70000/0x70FF0/0x70FF8/0x7A000 -- moved here in
 * Milestone 13 after an actual hang (not a review-time guess) traced
 * to the kernel's own .bss growing (a PCI scanner + virtio-net driver,
 * on top of that same milestone's kernel-load relocation from 0x1000
 * to 0x20000) far enough to completely swallow the old addresses --
 * they sat squarely inside what had become live kernel .bss, and the
 * trampoline copy was overwriting real kernel data out from under
 * itself. See hal/x86_64/start.asm's own comment for the matching
 * boot-stack relocation the same growth forced. Both new locations
 * stay comfortably below 0xA0000 (the conventional PC VGA/BIOS-shadow
 * memory hole this kernel has never tested writing into) with room to
 * spare, rather than just clearing the immediate collision. */

#define AP_TRAMPOLINE_ADDR 0x96000ULL
#define AP_TRAMPOLINE_PAGE 0x96
#define AP_BOOT_FLAG_ADDR  0x96FF0ULL
#define AP_ENTRY_PTR_ADDR  0x96FF8ULL
#define AP_STACK_TOP       0x99000ULL

/* Built by tools/build-c.ps1 from ap_trampoline.asm -> ap_trampoline.bin,
 * then wrapped as inert .rodata by ap_trampoline_blob.asm -- see its
 * own comment for why this can't just be assembled directly into the
 * kernel image the normal way. */
extern uint8_t ap_trampoline_blob[];
extern uint8_t ap_trampoline_blob_end[];

/* This core's identity for anything that wants to print "which CPU is
 * this" -- just an on-demand LAPIC register read (hal_lapic_id()), not
 * a cached per-CPU variable: there's no per-CPU data mechanism in this
 * kernel yet (GS-base or otherwise), and a fresh MMIO read is cheap
 * enough at this scale not to need one. */
static void ap_demo_loop(void) {
    uint32_t id = hal_lapic_id();

    hal_console_write("[AP core ");
    hal_console_write_dec64((uint64_t)id);
    hal_console_write("] alive, running independently of the BSP\n");

    /* Genuinely concurrent, observable work: count as fast as this
     * core can for a fixed number of iterations while the BSP is, at
     * the very same wall-clock time, running its entire actor demo
     * (scheduler_start(), back in core/main.c) on the OTHER physical
     * core. If this core were not actually running in parallel -- say,
     * SIPI silently failed to wake it, or hal_smp_boot_ap() somehow
     * serialized the two cores -- this message (and the "alive" one
     * above) would either never appear at all, or would appear only
     * fully before or fully after the BSP's entire trace, never
     * interleaved partway through it the way genuine concurrent
     * execution produces. */
    volatile uint64_t counter = 0;
    for (uint64_t i = 0; i < 30000000; i++) {
        counter++;
    }

    hal_console_write("[AP core ");
    hal_console_write_dec64((uint64_t)id);
    hal_console_write("] counted to ");
    hal_console_write_dec64(counter);
    hal_console_write(" while the BSP's actor demo ran\n");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

/* The first C code to ever run on the AP, reached via the mailbox
 * hand-off ap_trampoline.asm's own tail performs. Everything here is
 * genuinely per-core, hardware-enforced state this core has never
 * initialized for itself before this exact moment: its own GDTR/TR
 * (hal_gdt_load_ap), its own IDTR (hal_idt_load_ap -- the IDT's
 * CONTENTS are shared kernel commons, already built by the BSP, but
 * IDTR itself resets to an invalid default independently on every
 * core), and its own local APIC (hal_lapic_enable). Skipping any of
 * these would leave this core one hardware exception away from a
 * silent triple fault instead of this kernel's own diagnostic panic
 * screen. */
static void ap_entry_c(void) {
    hal_gdt_load_ap(AP_STACK_TOP);
    hal_idt_load_ap();
    hal_lapic_enable();

    uint32_t id = hal_lapic_id();
    *(volatile uint64_t *)AP_BOOT_FLAG_ADDR = (uint64_t)(id + 1);

    ap_demo_loop();
}

/* Brings up exactly one Application Processor. Returns 1 if it
 * responded (observed via AP_BOOT_FLAG_ADDR going nonzero) within a
 * bounded wait, 0 otherwise -- e.g. because QEMU was started with the
 * default `-smp 1`, the honest, expected outcome on a single-CPU run
 * rather than a bug: this function never hangs waiting for a core
 * that was never going to exist. */
int hal_smp_boot_ap(void) {
    hal_map_lapic_mmio();
    hal_lapic_enable(); /* the BSP's own -- needed to SEND the IPIs below */

    uint8_t *src = ap_trampoline_blob;
    uint8_t *dst = (uint8_t *)AP_TRAMPOLINE_ADDR;
    uint64_t len = (uint64_t)(ap_trampoline_blob_end - ap_trampoline_blob);
    for (uint64_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }

    *(volatile uint64_t *)AP_BOOT_FLAG_ADDR = 0;
    *(volatile uint64_t *)AP_ENTRY_PTR_ADDR = (uint64_t)ap_entry_c;

    hal_lapic_send_init_sipi(AP_TRAMPOLINE_PAGE);

    for (volatile uint32_t spin = 0; spin < 0x2000000; spin++) {
        if (*(volatile uint64_t *)AP_BOOT_FLAG_ADDR != 0) {
            return 1;
        }
    }
    return 0;
}
