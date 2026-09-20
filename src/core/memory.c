#include "vajra/hal.h"
#include "vajra/memory.h"

/* ------------------------------------------------------------------
 * Physical memory manager.
 *
 * This is the direct C equivalent of the assembly kernel's V0.30
 * memory manager: a bitmap-based page allocator built from whatever
 * usable-RAM regions the HAL reports, rather than assuming a fixed
 * size. Same design decisions carried over deliberately:
 *
 *   - Every page defaults to "unusable" in the bitmap; only pages
 *     inside a genuine usable region get cleared to free. This
 *     correctly handles reserved holes in the memory map instead of
 *     assuming one contiguous usable range.
 *   - A safety cap (256MB) bounds the bitmap to a reasonable fixed
 *     size for now -- lifting it later is a one-line change once
 *     there's a reason to.
 *   - A conservative fallback range (1MB-16MB) is used if the HAL
 *     can't provide a valid memory map at all.
 *
 * This file is 100% portable: it never touches E820 or any other
 * x86-specific format directly. It only calls hal_get_memory_map(),
 * which each architecture's HAL implements in its own way (E820 on
 * x86 BIOS, a device tree on most ARM boards, etc.) -- this exact
 * file should work unchanged once a future hal/aarch64/ exists.
 * ---------------------------------------------------------------- */

#define MEM_BASE        0x100000ULL     /* 1MB -- nothing below this is ever handed out */
#define MEM_CAP         0x10000000ULL   /* 256MB safety cap, matching the assembly kernel's V0.30 choice */
#define PAGE_SIZE       4096ULL
#define BITMAP_SIZE     8192            /* bytes; covers (MEM_CAP-MEM_BASE)/PAGE_SIZE/8 with margin */
#define FALLBACK_TOP    0x1000000ULL    /* 16MB, used only if the HAL gives us no valid map at all */

static uint8_t bitmap[BITMAP_SIZE];
static uint64_t next_free_page;
static uint64_t mem_top;

/* Freed pages go here for O(1) reuse instead of being lost -- the
 * bitmap bit for a freed page is deliberately left SET (see
 * free_page()), so this list is the only place alloc_page() can find
 * it again.
 *
 * This is a plain array in kernel .bss, not an intrusive linked list
 * stored inside the freed pages themselves (the more obvious design,
 * and what this used to be): since per-actor address spaces (see
 * hal/x86_64/paging.c), a freed page might belong to an actor whose
 * private mapping isn't even present in the address space that's
 * currently active when free_page() runs -- e.g. reap_dead_actors()
 * freeing actor B's stack while actor C is the one currently
 * scheduled and only actor C's own memory is mapped. Writing a "next"
 * pointer into the freed page's own memory in that situation would
 * fault. This array lives in the kernel commons instead, which stays
 * identity-mapped and accessible from every address space. 32 is
 * generous for how many pages this milestone's actor demo ever frees
 * at once; a page is simply leaked (not lost track of, just never
 * reused) if the list is ever full. */
#define FREE_LIST_CAPACITY 32
static uint64_t free_list[FREE_LIST_CAPACITY];
static int free_list_count = 0;

static inline void bitmap_set_used(uint64_t page_index) {
    bitmap[page_index / 8] |= (uint8_t)(1u << (page_index % 8));
}

static inline void bitmap_clear_used(uint64_t page_index) {
    bitmap[page_index / 8] &= (uint8_t)~(1u << (page_index % 8));
}

static inline int bitmap_is_used(uint64_t page_index) {
    return (bitmap[page_index / 8] & (uint8_t)(1u << (page_index % 8))) != 0;
}

/* Marks every whole page in [base, end) as free. Both are aligned
 * inward to page boundaries first, so a region that isn't page-
 * aligned never causes an adjacent page to be incorrectly marked
 * free. */
static void mark_range_free(uint64_t base, uint64_t end) {
    base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    end  = end & ~(PAGE_SIZE - 1);
    if (base >= end) {
        return;
    }
    for (uint64_t addr = base; addr < end; addr += PAGE_SIZE) {
        bitmap_clear_used((addr - MEM_BASE) / PAGE_SIZE);
    }
}

void memory_init(void) {
    for (int i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF; /* default: everything unusable */
    }

    next_free_page = MEM_BASE;
    mem_top = FALLBACK_TOP;

    struct hal_memory_region regions[32];
    int count = hal_get_memory_map(regions, 32);

    if (count == 0) {
        mark_range_free(MEM_BASE, FALLBACK_TOP);
        return;
    }

    uint64_t highest_usable_end = 0;

    for (int i = 0; i < count; i++) {
        uint64_t base = regions[i].base;
        uint64_t end  = base + regions[i].length;

        if (base < MEM_BASE) {
            base = MEM_BASE;
        }
        if (end > MEM_CAP) {
            end = MEM_CAP;
        }
        if (base >= end) {
            continue; /* nothing usable left in this region after clipping */
        }

        if (end > highest_usable_end) {
            highest_usable_end = end;
        }
        mark_range_free(base, end);
    }

    if (highest_usable_end <= MEM_BASE) {
        /* Nothing usable found above 1MB -- fall back rather than
         * trust an empty/garbage map. */
        mark_range_free(MEM_BASE, FALLBACK_TOP);
        mem_top = FALLBACK_TOP;
    } else {
        mem_top = highest_usable_end;
    }
}

/* A thin wrapper around hal_zero_page(), not a plain pointer-and-loop:
 * a freshly allocated page isn't guaranteed to be mapped in whichever
 * address space happens to be active right now (true ever since
 * actor_spawn_child() made alloc_page() callable at runtime, from
 * inside a syscall, with some actor's own restricted CR3 loaded --
 * see hal_zero_page()'s own comment for the full story). */
static void zero_page(uint64_t addr) {
    hal_zero_page((void *)addr);
}

void *alloc_page(void) {
    if (free_list_count > 0) {
        uint64_t addr = free_list[--free_list_count];
        zero_page(addr);
        return (void *)addr;
    }

    while (next_free_page < mem_top) {
        uint64_t page_index = (next_free_page - MEM_BASE) / PAGE_SIZE;

        if (!bitmap_is_used(page_index)) {
            bitmap_set_used(page_index);
            uint64_t addr = next_free_page;
            next_free_page += PAGE_SIZE;

            /* Hand out zeroed memory, matching the assembly kernel's
             * alloc_page behavior. */
            zero_page(addr);

            return (void *)addr;
        }

        next_free_page += PAGE_SIZE;
    }

    return 0; /* out of memory */
}

/* Returns a page to the allocator for reuse. The bitmap bit is
 * deliberately left set -- freed pages are tracked exclusively via
 * free_list[], not by clearing their bitmap bit, so a page can never
 * be simultaneously "on the free list" and "found by the bump-scan in
 * alloc_page()" through two different paths. Deliberately never
 * touches *addr itself -- see free_list's own comment for why. */
void free_page(void *addr) {
    if (!addr) {
        return;
    }
    if (free_list_count < FREE_LIST_CAPACITY) {
        free_list[free_list_count++] = (uint64_t)addr;
    }
}

/* Returns a page that came from alloc_dma_pages()/alloc_pages_contig() to the
 * allocator. NOT free_page(): that pushes onto the free list alloc_page()
 * serves ordinary actor stacks from, and a page from the >=2MB commons region
 * handed out as a stack lies outside the 1MB-2MB private window every actor
 * address space is built around -- the next spawn would fail. This just
 * clears the page's bitmap bit so the contiguous scanner can hand it out again. */
void free_dma_page(void *addr) {
    uint64_t a = (uint64_t)addr;
    if (a < MEM_BASE) {
        return;
    }
    bitmap_clear_used((a - MEM_BASE) / PAGE_SIZE);
}

/* Shared scan loop for alloc_pages_contig()/alloc_dma_pages() below --
 * identical logic, different starting point in the same bitmap. */
static void *find_and_claim_contig(uint64_t search_start, int count) {
    if (count <= 0) {
        return 0;
    }

    for (uint64_t start = search_start; start + (uint64_t)count * PAGE_SIZE <= mem_top; start += PAGE_SIZE) {
        int all_free = 1;
        for (int i = 0; i < count; i++) {
            uint64_t page_index = (start - MEM_BASE) / PAGE_SIZE + (uint64_t)i;
            if (bitmap_is_used(page_index)) {
                all_free = 0;
                break;
            }
        }
        if (!all_free) {
            continue;
        }

        for (int i = 0; i < count; i++) {
            uint64_t page_index = (start - MEM_BASE) / PAGE_SIZE + (uint64_t)i;
            bitmap_set_used(page_index);
        }
        for (int i = 0; i < count; i++) {
            zero_page(start + (uint64_t)i * PAGE_SIZE);
        }
        return (void *)start;
    }

    return 0; /* no run of `count` consecutive free pages found */
}

void *alloc_pages_contig(int count) {
    return find_and_claim_contig(MEM_BASE, count);
}

/* 0x200000 (2MB) -- PRIVATE_WINDOW_END in hal/x86_64/paging.c, the
 * boundary where the actor-private window ends and the commons (huge-
 * page-mapped, identical and supervisor-only in every address space)
 * resumes. See this function's own declaration (include/vajra/
 * memory.h) for why DMA buffers need to start their search here
 * instead of at MEM_BASE. */
void *alloc_dma_pages(int count) {
    return find_and_claim_contig(0x200000ULL, count);
}

uint64_t memory_get_total_bytes(void) {
    return mem_top;
}
