#include "vajra/hal.h"
#include "vajra/actor.h"

/* ------------------------------------------------------------------
 * Kernel-owned GDT + TSS.
 *
 * Originally built (Milestone 4) just to give the double-fault
 * handler a dedicated stack (IST1) instead of whatever RSP happened
 * to be active when it fired -- see the IST1 setup below for why that
 * matters. Milestone 5's per-actor address spaces needed nothing more
 * from this file. Milestone 6 (ring 3) needs two more things, both
 * here: ring-3 code/data descriptors (GDT_USER_DATA/GDT_USER_CODE),
 * and a real TSS.RSP0 -- until now the TSS existed only for IST1 and
 * RSP0 was left at 0, which was fine while every gate ran at CPL 0
 * already and never triggered a privilege-level change.
 *
 * boot.asm loads a bootstrap GDT (null, 32-bit code, data, 64-bit
 * code) just to get into long mode; it has no TSS descriptor and
 * nothing after boot ever expected to need one. Rather than patch
 * live boot-sector memory at a hardcoded offset, the kernel builds
 * and loads its own GDT here, replicating the first four entries
 * byte-for-byte (so selectors 0x10/0x18 already in use -- e.g. by
 * idt_set_gate's IDT entries -- keep meaning exactly what they did).
 * ---------------------------------------------------------------- */

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* null, 32-bit code, data, 64-bit code, TSS lo, TSS hi, user data, user
 * code, AP TSS lo, AP TSS hi. */
#define GDT_ENTRIES         10
#define GDT_TSS_SELECTOR    0x20
#define GDT_USER_DATA_SEL   0x33 /* entry 6 (0x30) | RPL 3 */
#define GDT_USER_CODE_SEL   0x3B /* entry 7 (0x38) | RPL 3 */
#define GDT_AP_TSS_SELECTOR 0x40 /* entry 8 -- see hal_gdt_load_ap() */

#define DF_STACK_SIZE 4096
#define KERNEL_STACK_SIZE 4096

static uint64_t gdt[GDT_ENTRIES];
static struct gdt_ptr gdtp;
static struct tss64 tss;
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));

/* The AP's own TSS (Milestone 12/Phase 10) -- a task register can only
 * ever point at ONE TSS descriptor at a time per core, and loading one
 * (`ltr`) marks that exact descriptor "busy" in hardware; two cores
 * both pointing TR at the SAME descriptor would have the second `ltr`
 * fault (#GP) on an already-busy descriptor. Each core needs its own
 * descriptor and its own backing struct, even though both can -- and
 * do here -- share the one physical gdt[] table itself. */
static struct tss64 tss_ap;

/* One fixed, dedicated ring-0 stack per actor SLOT index (same "one
 * static slot per actor slot, not per spawn" pattern as paging.c's
 * per-actor page tables, for the same reason: MAX_ACTORS never
 * changes at runtime, and actor_spawn() only ever runs before any
 * actor is active). This is what TSS.RSP0 points at while that actor
 * is the one currently scheduled -- see hal_set_kernel_stack() and,
 * for why one shared stack across all actors would be wrong, hal.h's
 * own comment on hal_set_kernel_stack(). */
static uint8_t kernel_stacks[MAX_ACTORS][KERNEL_STACK_SIZE] __attribute__((aligned(16)));

void hal_gdt_init(void) {
    gdt[0] = 0x0000000000000000ULL; /* null */
    gdt[1] = 0x00CF9A000000FFFFULL; /* 32-bit code -- unused past boot, kept for parity */
    gdt[2] = 0x00CF92000000FFFFULL; /* data (selector 0x10) */
    gdt[3] = 0x00AF9A000000FFFFULL; /* 64-bit code (selector 0x18) */
    gdt[6] = 0x00CFF2000000FFFFULL; /* user data, DPL=3 (selector 0x30|3 = 0x33) */
    gdt[7] = 0x00AFFA000000FFFFULL; /* user 64-bit code, DPL=3 (selector 0x38|3 = 0x3B) */

    for (int i = 0; i < 7; i++) {
        tss.ist[i] = 0;
    }
    tss.ist[0] = (uint64_t)&df_stack[DF_STACK_SIZE]; /* IST1, stacks grow down */
    tss.iomap_base = sizeof(tss);
    hal_set_kernel_stack(0); /* a real value before the first actor runs, rather than RSP0=0 */

    uint64_t base  = (uint64_t)&tss;
    uint32_t limit = sizeof(tss) - 1;

    uint64_t low = 0;
    low |= (limit & 0xFFFFULL);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;                        /* present, type 0x9 = 64-bit TSS (available) */
    low |= ((uint64_t)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;
    uint64_t high = (base >> 32) & 0xFFFFFFFFULL;

    gdt[4] = low;
    gdt[5] = high;

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)&gdt[0];

    __asm__ __volatile__("lgdt %0" : : "m"(gdtp));

    /* Reload CS via a far return (the only way to reload it in long
     * mode) and the data segment registers, so the CPU is actually
     * running off the new table rather than just pointing GDTR at it. */
    __asm__ __volatile__(
        "pushq $0x18\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        : : : "rax", "memory");

    uint16_t tr = GDT_TSS_SELECTOR;
    __asm__ __volatile__("ltr %0" : : "r"(tr));
}

void hal_set_kernel_stack(int slot) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return;
    }
    tss.rsp0 = (uint64_t)&kernel_stacks[slot][KERNEL_STACK_SIZE];
}

void *hal_get_kernel_stack_top(int slot) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return 0;
    }
    return &kernel_stacks[slot][KERNEL_STACK_SIZE];
}

/* Activates the AP's own TSS descriptor -- the AP-side counterpart to
 * hal_gdt_init(), called once from hal/x86_64/smp.c's ap_entry_c().
 * rsp0 isn't actually exercised yet (the AP doesn't run ring-3 actor
 * code in this milestone -- see smp.c's own top comment), but leaving
 * it valid rather than zero is what a real TSS should look like, not
 * a shortcut worth a comment of its own.
 *
 * Unlike hal_gdt_init()'s first call (replacing the bootloader's OWN,
 * different GDT), this core's segment registers already hold selector
 * values (0x10 data, 0x18 code, set by ap_trampoline.asm's own
 * temporary GDT) that describe byte-for-byte identical descriptors in
 * THIS table -- gdt[1..3] here matches ap_trampoline.asm's gdt_start
 * exactly -- so no segment reload is needed, only pointing GDTR here
 * and activating this core's own TSS selector. */
void hal_gdt_load_ap(uint64_t rsp0) {
    for (int i = 0; i < 7; i++) {
        tss_ap.ist[i] = 0;
    }
    tss_ap.rsp0 = rsp0;
    tss_ap.iomap_base = sizeof(tss_ap);

    uint64_t base  = (uint64_t)&tss_ap;
    uint32_t limit = sizeof(tss_ap) - 1;

    uint64_t low = 0;
    low |= (limit & 0xFFFFULL);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;
    low |= ((uint64_t)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;
    uint64_t high = (base >> 32) & 0xFFFFFFFFULL;

    gdt[8] = low;
    gdt[9] = high;

    __asm__ __volatile__("lgdt %0" : : "m"(gdtp));

    uint16_t tr = GDT_AP_TSS_SELECTOR;
    __asm__ __volatile__("ltr %0" : : "r"(tr));
}
