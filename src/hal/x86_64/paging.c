#include "vajra/hal.h"
#include "vajra/actor.h"

/* ------------------------------------------------------------------
 * Per-actor address spaces.
 *
 * boot.asm identity-maps the first 256MB using 128 2MB huge pages in
 * a single page directory at physical 0x92000 (PML4 at 0x90000 ->
 * PDPT at 0x91000 -> this PD). That "boot" mapping remains exactly as
 * it was and keeps serving as the CR3 in effect from early boot up
 * until the first actor is spawned -- and, just as importantly, as
 * the template every actor's own address space copies its shared
 * "commons" mapping from below.
 *
 * The design: everything from 0-1MB (MEM_BASE in core/memory.c -- the
 * kernel image, its .bss, IDT/GDT/TSS, this file's own page-table
 * pools, the VGA buffer) is identical and present in EVERY address
 * space, because kernel code needs to keep reaching it no matter
 * which actor's CR3 happens to be loaded. Almost all of it is also
 * supervisor-only -- except .user_text (link.ld), the one range of
 * kernel-image code that ring-3 actor code is allowed to fetch
 * instructions from at all: actor entry points and the tiny
 * hal_syscall() wrapper they call to reach the kernel (see hal.h).
 * Everything else the kernel does stays completely unreachable from
 * CPL 3, by construction -- there's no way to call a supervisor-only
 * function directly, only to `int 0x80` into the one gate that
 * explicitly allows it.
 *
 * Data referenced FROM .user_text (string literals, etc.) does NOT
 * need to move anywhere: taking an address is unrestricted at any
 * privilege level, and the only code that ever dereferences a pointer
 * an actor passes to a syscall is the kernel's own handler, running
 * at CPL 0 -- where the page's U/S bit is irrelevant. Only *executing*
 * instructions from a page is gated by CPL, so only .text needs
 * splitting.
 *
 * Everything from 2MB-256MB is also identical in every address space
 * -- copied directly from boot.asm's own huge-page entries, which
 * need no translation since a huge-page PD entry encodes its whole
 * mapping in one 8-byte descriptor. The 1MB-2MB range in between is
 * where actor memory actually lives (core/memory.c's allocator starts
 * handing out pages at MEM_BASE = 1MB), and THAT range is where each
 * actor's address space genuinely differs: it maps ONLY that actor's
 * own pages there, present and user-accessible, and leaves every
 * other address in that entire 1MB window unmapped -- including every
 * other actor's own stack. An actor can no longer even address
 * another actor's memory, let alone corrupt it, and a stack overflow
 * of any size faults immediately instead of being merely caught by
 * one adjacent guard page.
 *
 * This assumes actor memory never needs more than the 1MB-2MB window
 * -- true today (a handful of 4KB stacks) but a real limit: revisit
 * before actors need more per-actor memory than that.
 * ---------------------------------------------------------------- */

#define PAGE_DIR_PHYS_BASE 0x92000ULL
#define PAGE_SIZE_4K        4096ULL
#define PAGE_SIZE_2M        0x200000ULL
#define PD_ENTRIES          512
#define PT_ENTRIES          512
#define COMMONS_END         0x100000ULL /* 1MB -- must match core/memory.c's MEM_BASE */
#define PRIVATE_WINDOW_END  0x200000ULL /* 2MB -- see this file's own limitation note above */

/* One fixed, statically-reserved address space per actor SLOT index
 * (not per spawn) -- MAX_ACTORS never changes at runtime, so there's
 * no need for dynamic page-table allocation/freeing here. Spawning a
 * new actor into a previously-dead slot simply rebuilds that slot's
 * tables from scratch; safe even now that spawning can happen at
 * runtime (Milestone 9's actor_spawn_child()) because interrupts stay
 * disabled for the whole syscall that triggers it, so nothing else
 * can be running concurrently to observe a half-built table. */
static uint64_t as_pml4[MAX_ACTORS][512] __attribute__((aligned(4096)));
static uint64_t as_pdpt[MAX_ACTORS][512] __attribute__((aligned(4096)));
static uint64_t as_pd  [MAX_ACTORS][512] __attribute__((aligned(4096)));
static uint64_t as_pt0 [MAX_ACTORS][512] __attribute__((aligned(4096))); /* covers 0-2MB */

/* link.ld-defined bounds of the one part of ordinary kernel-image
 * .text that ring-3 code is allowed to execute from -- see this
 * file's top comment. */
extern uint8_t __user_text_start[];
extern uint8_t __user_text_end[];

uint64_t hal_address_space_create(int slot, uint64_t private_base, uint64_t private_size) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return 0;
    }
    if (private_base < COMMONS_END || private_base + private_size > PRIVATE_WINDOW_END) {
        return 0; /* outside the 1MB-2MB window this design supports -- see top comment */
    }

    uint64_t *pml4 = as_pml4[slot];
    uint64_t *pdpt = as_pdpt[slot];
    uint64_t *pd   = as_pd[slot];
    uint64_t *pt0  = as_pt0[slot];

    for (int i = 0; i < PT_ENTRIES; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
        pd[i]   = 0;
        pt0[i]  = 0;
    }

    /* 0-1MB: commons, identity-mapped, present in every address space
     * -- except .user_text, every bit of it is supervisor-only, since
     * it's the kernel itself and every address space needs kernel
     * code/data to keep working regardless of which actor is active. */
    uint64_t user_text_start = (uint64_t)__user_text_start;
    uint64_t user_text_end   = (uint64_t)__user_text_end;
    for (uint64_t addr = 0; addr < COMMONS_END; addr += PAGE_SIZE_4K) {
        int is_user_text = (addr >= user_text_start && addr < user_text_end);
        pt0[addr / PAGE_SIZE_4K] = addr | (is_user_text ? 0x7 : 0x3);
    }

    /* 1MB-2MB: private to this actor. Everything defaults to not-
     * present (the zeroing above); only the caller's own pages get
     * mapped, present + writable + user-accessible -- this actor's
     * ring-3 code runs on this very stack, so it has to be able to
     * reach it. No other actor's memory in this same window is mapped
     * here at all, regardless of how physically close together the
     * allocator packed them. */
    for (uint64_t addr = private_base; addr < private_base + private_size; addr += PAGE_SIZE_4K) {
        pt0[addr / PAGE_SIZE_4K] = addr | 0x7;
    }

    /* Structural entries (PML4/PDPT/PD pointing to another table, as
     * opposed to a leaf mapping a page directly) carry the user bit
     * too, here and below -- x86 permissions are the AND of every
     * level walked, not just the final PTE. Without this, pt0's own
     * user-accessible entries above would be silently overridden back
     * to supervisor-only by this non-leaf entry lacking the same bit,
     * which is exactly what happened the first time this was written
     * (confirmed by a ring-3 #PF, error code 5 -- protection violation
     * on a user-mode fetch of a page that was, in isolation, correctly
     * marked present + user). The bit being set here doesn't grant
     * blanket ring-3 access to everything under pd[0] -- pt0's own
     * per-page bits (0x3 vs 0x7 above) still decide that. */
    pd[0] = ((uint64_t)pt0) | 0x7;

    /* 2MB-256MB: commons again, copied straight from boot.asm's own
     * page directory. Huge-page PD entries need no indirection to
     * share -- the whole mapping lives in the 8-byte descriptor
     * itself, so copying the value is copying the mapping. These stay
     * exactly as boot.asm set them (no user bit), so this range
     * remains supervisor-only regardless of pdpt[0]/pml4[0] below. */
    volatile uint64_t *boot_pd = (volatile uint64_t *)PAGE_DIR_PHYS_BASE;
    for (int i = 1; i < PD_ENTRIES; i++) {
        pd[i] = boot_pd[i];
    }

    pdpt[0] = ((uint64_t)pd) | 0x7;
    pml4[0] = ((uint64_t)pdpt) | 0x7;

    return (uint64_t)pml4;
}

/* The boot-time PML4 (see this file's top comment) -- identity-maps
 * the full 0-256MB range unconditionally, unlike any individual
 * actor's own address space, which only ever maps its own 1MB-2MB
 * pages. Kept around after boot specifically so hal_zero_page() below
 * has a CR3 that's guaranteed to see any physical address handed to
 * it, no matter which actor's restricted view happens to be active. */
#define BOOT_PML4_PHYS_BASE 0x90000ULL

void hal_zero_page(void *phys_addr) {
    uint64_t saved_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));

    /* Skip the switch entirely on the common path (kernel_main's own
     * initial spawns, still running under the boot CR3 itself) -- a
     * CR3 write flushes the TLB even when the value doesn't change,
     * so this isn't just a style preference. */
    if (saved_cr3 != BOOT_PML4_PHYS_BASE) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(BOOT_PML4_PHYS_BASE) : "memory");
    }

    uint64_t *p = (uint64_t *)phys_addr;
    for (uint64_t i = 0; i < PAGE_SIZE_4K / 8; i++) {
        p[i] = 0;
    }

    if (saved_cr3 != BOOT_PML4_PHYS_BASE) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
    }
}

/* ------------------------------------------------------------------
 * One extra mapping for SMP bring-up (hal/x86_64/apic.c, smp.c): the
 * local APIC's registers live at a fixed physical address (0xFEE00000)
 * far outside the 0-256MB this file's own boot-inherited identity map
 * covers, so both the BSP (to send the wake-up IPIs) and the AP (to
 * enable its own local APIC once it's running) need it reachable.
 *
 * Added directly to the BOOT page tables (0x90000-0x92FFF), not any
 * per-actor address space: every place that touches the LAPIC in this
 * milestone runs under CR3=boot PML4 the entire time -- the BSP,
 * before the first actor is ever spawned, and the AP, which (see
 * hal/x86_64/smp.c's own comment) does not yet participate in the
 * per-actor scheduler and so never loads a different CR3 at all. A
 * future milestone that lets actor-context code (e.g. a per-core timer
 * ISR firing while an actor's own restricted CR3 is active) touch the
 * LAPIC would need this same mapping added to
 * hal_address_space_create() too -- not needed yet, so not done yet.
 * ---------------------------------------------------------------- */
#define BOOT_PDPT_PHYS_BASE 0x91000ULL
#define LAPIC_MMIO_PHYS     0xFEE00000ULL

static uint64_t lapic_pd[PD_ENTRIES] __attribute__((aligned(4096)));
static uint64_t lapic_pt[PT_ENTRIES] __attribute__((aligned(4096)));

void hal_map_lapic_mmio(void) {
    for (int i = 0; i < PD_ENTRIES; i++) {
        lapic_pd[i] = 0;
    }
    for (int i = 0; i < PT_ENTRIES; i++) {
        lapic_pt[i] = 0;
    }

    /* LAPIC_MMIO_PHYS is architecturally page-aligned (it happens to
     * be 2MB-aligned too, but nothing here assumes that beyond what
     * the arithmetic itself computes). */
    int pt_index = (int)((LAPIC_MMIO_PHYS % PAGE_SIZE_2M) / PAGE_SIZE_4K);
    lapic_pt[pt_index] = LAPIC_MMIO_PHYS | 0x3; /* present, writable, supervisor-only --
                                                    actor code has no business anywhere near
                                                    this, and never can: it's outside every
                                                    per-actor address space entirely. */

    int pd_index = (int)((LAPIC_MMIO_PHYS % (PAGE_SIZE_2M * PD_ENTRIES)) / PAGE_SIZE_2M);
    lapic_pd[pd_index] = ((uint64_t)lapic_pt) | 0x3;

    int pdpt_index = (int)(LAPIC_MMIO_PHYS / (PAGE_SIZE_2M * PD_ENTRIES));
    volatile uint64_t *boot_pdpt = (volatile uint64_t *)BOOT_PDPT_PHYS_BASE;
    boot_pdpt[pdpt_index] = ((uint64_t)lapic_pd) | 0x3;

    /* A CR3 reload forces a full TLB flush. Adding a previously
     * not-present mapping can't have anything stale cached for it, so
     * this is belt-and-suspenders, not a fix for an observed bug --
     * cheap enough to keep the doubt from ever coming up. */
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
