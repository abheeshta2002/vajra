#include "vajra/hal.h"
#include "vajra/actor.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 10. This file brings up the second physical core;
 * core/actor.c's scheduler_start_ap() is what then puts it to work.
 * Milestone 12 proved the core could be woken and run linked C code
 * at all; the follow-up (this milestone) folded it into the scheduler:
 * per-core current-actor / scheduler contexts, one TSS and one local-
 * APIC preemption tick per core, and a kernel lock (hal/x86_64/cpu.c)
 * keeping every kernel global's single-caller assumption true. See
 * docs/ROADMAP.md's Phase 10 entry.
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
 * see their own comments for what occupies 0x08000-0x0AFFF, 0x0C000,
 * and the kernel image + .bss at 0x20000+):
 *   0x0E000-0x0EFFF  AP trampoline code (copied from ap_trampoline_blob)
 *   0x0EFF0          AP_BOOT_FLAG_ADDR -- 0 until the AP writes (its APIC ID + 1)
 *   0x0EFF8          AP_ENTRY_PTR_ADDR -- BSP writes ap_entry_c's address here
 *                                         before sending SIPI; the trampoline,
 *                                         assembled as its own standalone,
 *                                         position-independent blob with no
 *                                         visibility into this kernel's own
 *                                         symbol table, reads it back to hand
 *                                         off into real, linked kernel code.
 *   0x11000          top of the AP's own dedicated stack
 *
 * Originally 0x70000/0x70FF0/0x70FF8/0x7A000, then 0x96000/0x96FF0/
 * 0x96FF8/0x99000 (Milestone 13, after the kernel's own .bss growth
 * swallowed the first set -- see hal/x86_64/start.asm's own comment
 * for the matching boot-stack relocation that growth also forced).
 * Moved again here (same session, Milestone 18's desktop rewrite):
 * boot.asm's own "structural fix" comment explains why -- every
 * address ABOVE the kernel's 0x20000 load address was competing with
 * kernel .bss's own growth forever, so everything fixed and
 * low-memory (page tables, E820 map, this trampoline) moved BELOW
 * 0x20000 instead, into space the kernel itself used to occupy before
 * Milestone 13 moved it up. Kernel .bss only ever grows upward from
 * 0x20000, so this can't collide again the way it did twice already. */

#define AP_TRAMPOLINE_ADDR 0x0E000ULL
#define AP_TRAMPOLINE_PAGE 0x0E
#define AP_BOOT_FLAG_ADDR  0x0EFF0ULL
#define AP_ENTRY_PTR_ADDR  0x0EFF8ULL
#define AP_STACK_TOP       0x11000ULL

/* Built by tools/build-c.ps1 from ap_trampoline.asm -> ap_trampoline.bin,
 * then wrapped as inert .rodata by ap_trampoline_blob.asm -- see its
 * own comment for why this can't just be assembled directly into the
 * kernel image the normal way. */
extern uint8_t ap_trampoline_blob[];
extern uint8_t ap_trampoline_blob_end[];

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
static volatile int cpu_online[MAX_CPUS] = { 1, 0, 0, 0 }; /* the BSP is trivially online */

int hal_cpu_online(int cpu) {
    return (cpu >= 0 && cpu < MAX_CPUS) ? cpu_online[cpu] : 0;
}

static void ap_entry_c(void) {
    /* INIT leaves CR0.CD (cache disable) and CR0.NW set, and unlike the
     * BSP -- whose firmware already cleared them -- nothing on this
     * core's path from reset to here does. QEMU's TCG ignores it; real
     * hardware would run this core with caches off, an order of
     * magnitude slower, which a benchmark like the Cores app's
     * parallelism test would then misreport. */
    uint64_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ULL << 30) | (1ULL << 29));
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");

    hal_gdt_load_ap(AP_STACK_TOP);
    hal_idt_load_ap();
    hal_lapic_enable();

    uint32_t id = hal_lapic_id();
    *(volatile uint64_t *)AP_BOOT_FLAG_ADDR = (uint64_t)(id + 1);

    /* Phase 10 (remainder): this core is no longer a spectator. It joins
     * the actor scheduler -- see core/actor.c's scheduler_start_ap() --
     * and from then on runs actors in parallel with the BSP. (Until
     * Milestone 12's follow-up it only counted in a busy loop to prove
     * it was alive; that proof is now the whole demo running on both.) */
    cpu_online[id < MAX_CPUS ? id : 0] = 1;
    scheduler_start_ap();
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
