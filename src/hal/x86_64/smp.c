#include "vajra/hal.h"
#include "vajra/actor.h"
#include "vajra/memory.h"

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
 *   0x0EFE8          AP_STACK_PTR_ADDR -- the BSP writes each core's own stack top here (per-core, from the allocator) before waking it
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
#define AP_STACK_PTR_ADDR  0x0EFE8ULL /* per-core stack top, written by the BSP before each SIPI */
#define AP_STACK_PAGES     4          /* 16KB per core, from the allocator */

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
static volatile int cpu_online[MAX_CPUS] = { 1 }; /* the BSP is trivially online */

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

    hal_enable_nx(); /* before this core loads any address space that uses the NX bit */

    int cpu = hal_cpu_id();
    hal_gdt_load_ap(cpu, (uint64_t)__builtin_frame_address(0));
    hal_idt_load_ap();
    hal_lapic_enable();

    *(volatile uint64_t *)AP_BOOT_FLAG_ADDR = (uint64_t)(cpu + 1);

    /* Phase 10 (remainder): this core is no longer a spectator. It joins
     * the actor scheduler -- see core/actor.c's scheduler_start_ap() --
     * and from then on runs actors in parallel with the BSP. (Until
     * Milestone 12's follow-up it only counted in a busy loop to prove
     * it was alive; that proof is now the whole demo running on both.) */
    cpu_online[cpu] = 1;
    scheduler_start_ap();
}

/* Brings up every Application Processor the firmware lists (up to
 * MAX_CPUS-1 of them), ONE AT A TIME, and returns how many responded.
 *
 * One at a time because the trampoline is a single shared blob with a
 * single mailbox: it takes its stack top from AP_STACK_PTR_ADDR. The
 * old broadcast INIT-SIPI woke every other core at once onto one fixed
 * stack, which is why `-smp 3` used to corrupt itself and hang; now the
 * BSP hands each core its own 16KB stack (allocated here, so this must
 * run after memory_init()), wakes just that core by APIC ID, and waits
 * for it to check in before touching the mailbox again. A core that
 * never answers is skipped, not waited on forever -- e.g. -smp 1 just
 * has an empty list, the honest expected outcome rather than a bug.
 *
 * Core index == APIC ID (see cpu.c); an ID >= MAX_CPUS can't be given a
 * slot and is reported and left asleep. */
int hal_smp_boot_aps(void) {
    hal_map_lapic_mmio();
    hal_lapic_enable(); /* the BSP's own -- needed to SEND the IPIs below */

    uint8_t ids[64];
    int found = hal_acpi_find_cpus(ids, 64);
    if (found == 0) {
        /* No usable ACPI: assume the classic two-core case rather than
         * guess further. */
        ids[0] = 0;
        ids[1] = 1;
        found = 2;
    }

    uint8_t *src = ap_trampoline_blob;
    uint8_t *dst = (uint8_t *)AP_TRAMPOLINE_ADDR;
    uint64_t len = (uint64_t)(ap_trampoline_blob_end - ap_trampoline_blob);
    for (uint64_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    *(volatile uint64_t *)AP_ENTRY_PTR_ADDR = (uint64_t)ap_entry_c;

    int bsp = hal_cpu_id();
    int started = 0;
    for (int n = 0; n < found; n++) {
        int id = ids[n];
        if (id == bsp) {
            continue;
        }
        if (id >= MAX_CPUS) {
            hal_console_write("SMP: core with APIC ID ");
            hal_console_write_dec64((uint64_t)id);
            hal_console_write(" is beyond MAX_CPUS; left asleep.\n");
            continue;
        }
        uint8_t *stack = (uint8_t *)alloc_dma_pages(AP_STACK_PAGES);
        if (!stack) {
            break;
        }
        *(volatile uint64_t *)AP_STACK_PTR_ADDR = (uint64_t)(stack + AP_STACK_PAGES * 4096);
        *(volatile uint64_t *)AP_BOOT_FLAG_ADDR = 0;

        hal_lapic_wake_cpu((uint8_t)id, AP_TRAMPOLINE_PAGE);

        int up = 0;
        for (volatile uint32_t spin = 0; spin < 0x2000000; spin++) {
            if (*(volatile uint64_t *)AP_BOOT_FLAG_ADDR != 0) {
                up = 1;
                break;
            }
        }
        if (up) {
            started++;
        } else {
            hal_console_write("SMP: core ");
            hal_console_write_dec64((uint64_t)id);
            hal_console_write(" did not respond.\n");
        }
    }
    return started;
}
