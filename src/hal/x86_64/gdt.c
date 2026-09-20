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
#define GDT_ENTRIES         (8 + 2 * (MAX_CPUS - 1)) /* + one 16-byte TSS descriptor per extra core */
#define GDT_TSS_SELECTOR    0x20
#define GDT_USER_DATA_SEL   0x33 /* entry 6 (0x30) | RPL 3 */
#define GDT_USER_CODE_SEL   0x3B /* entry 7 (0x38) | RPL 3 */
#define GDT_AP_TSS_BASE_SEL 0x40 /* core 1's TSS descriptor (entry 8); core N's is +16*(N-1) -- see hal_gdt_load_ap() */

#define DF_STACK_SIZE 4096
#define KERNEL_STACK_SIZE 4096

static uint64_t gdt[GDT_ENTRIES];
static struct gdt_ptr gdtp;
/* Phase 10 (completion): one TSS and one #DF stack PER CORE, indexed by
 * core number (cpu 0 = the BSP). They used to be `tss` and `tss_ap` --
 * exactly one extra core's worth. */
static struct tss64 tss_cpu[MAX_CPUS];
static uint8_t df_stack_cpu[MAX_CPUS][DF_STACK_SIZE] __attribute__((aligned(16)));

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
        tss_cpu[0].ist[i] = 0;
    }
    tss_cpu[0].ist[0] = (uint64_t)&df_stack_cpu[0][DF_STACK_SIZE]; /* IST1, stacks grow down */
    tss_cpu[0].iomap_base = sizeof(tss_cpu[0]);
    hal_set_kernel_stack(0); /* a real value before the first actor runs, rather than RSP0=0 */

    uint64_t base  = (uint64_t)&tss_cpu[0];
    uint32_t limit = sizeof(tss_cpu[0]) - 1;

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
    /* Phase 10: one TSS per core -- a syscall/interrupt arriving on
     * core N lands on core N's own TSS.RSP0, which must name the
     * kernel stack of the actor THAT core is running. */
    struct tss64 *t = &tss_cpu[hal_cpu_id()];
    t->rsp0 = (uint64_t)&kernel_stacks[slot][KERNEL_STACK_SIZE];
}

void *hal_get_kernel_stack_top(int slot) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return 0;
    }
    return &kernel_stacks[slot][KERNEL_STACK_SIZE];
}

/* Activates a non-boot core's own TSS descriptor -- the counterpart to
 * hal_gdt_init() for cores 1..MAX_CPUS-1, called once per core from
 * hal/x86_64/smp.c's ap_entry_c(). Every core shares the one gdt[] table
 * itself but needs its own TSS descriptor in it (`ltr` marks the
 * descriptor busy, so two cores can't share one), at entry 8+2*(cpu-1).
 * Each core writes only its OWN two entries, so no lock is needed.
 *
 * Unlike hal_gdt_init()'s first call (replacing the bootloader's OWN,
 * different GDT), this core's segment registers already hold selector
 * values (0x10 data, 0x18 code, set by ap_trampoline.asm's own
 * temporary GDT) that describe byte-for-byte identical descriptors in
 * THIS table -- gdt[1..3] here matches ap_trampoline.asm's gdt_start
 * exactly -- so no segment reload is needed, only pointing GDTR here
 * and activating this core's own TSS selector. */
void hal_gdt_load_ap(int cpu, uint64_t rsp0) {
    if (cpu < 1 || cpu >= MAX_CPUS) {
        return;
    }
    struct tss64 *t = &tss_cpu[cpu];
    for (int i = 0; i < 7; i++) {
        t->ist[i] = 0;
    }
    t->rsp0 = rsp0;
    t->ist[0] = (uint64_t)&df_stack_cpu[cpu][DF_STACK_SIZE]; /* IST1: the #DF gate uses it */
    t->iomap_base = sizeof(*t);

    uint64_t base  = (uint64_t)t;
    uint32_t limit = sizeof(*t) - 1;

    uint64_t low = 0;
    low |= (limit & 0xFFFFULL);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;
    low |= ((uint64_t)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;
    uint64_t high = (base >> 32) & 0xFFFFFFFFULL;

    int idx = 8 + 2 * (cpu - 1);
    gdt[idx] = low;
    gdt[idx + 1] = high;

    __asm__ __volatile__("lgdt %0" : : "m"(gdtp));

    uint16_t tr = (uint16_t)(GDT_AP_TSS_BASE_SEL + 16 * (cpu - 1));
    __asm__ __volatile__("ltr %0" : : "r"(tr));
}
