#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Reads the E820 memory map boot.asm left at a fixed physical
 * address and translates it into the portable hal_memory_region
 * format. This is the only file that knows the E820 format itself --
 * core/memory.c never sees a raw E820 entry, only the generic
 * (base, length) pairs, matching the HAL boundary this project keeps
 * everything else on.
 *
 * On-disk layout at E820_MAP_BASE (see boot.asm's own comment for
 * the full story of why this exists and how it's captured):
 *   dword magic ('E820' = 0x45383230, 0 if the BIOS call failed)
 *   dword entry_count
 *   entry_count * 24-byte entries: {u64 base, u64 length, u32 type, u32 attr}
 *   (type == 1 means usable RAM; anything else is reserved/unusable)
 * ---------------------------------------------------------------- */

#define E820_MAP_BASE 0x0C000 /* must match boot.asm's ES=0x0C00 -- see its own "structural fix"
                                  comment: moved below the kernel's own 0x20000 load address,
                                  permanently clear of kernel .bss growth (which only grows
                                  upward from there), instead of another same-budget nudge. */
#define E820_MAGIC    0x45383230u
#define E820_TYPE_USABLE 1

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attr;
} __attribute__((packed));

int hal_get_memory_map(struct hal_memory_region *out, int max_regions) {
    volatile uint32_t *magic_ptr = (volatile uint32_t *)(uintptr_t)E820_MAP_BASE;
    volatile uint32_t *count_ptr = (volatile uint32_t *)(uintptr_t)(E820_MAP_BASE + 4);

    if (*magic_ptr != E820_MAGIC) {
        return 0; /* boot.asm's BIOS call failed or found no extensions -- no valid map */
    }

    uint32_t entry_count = *count_ptr;
    if (entry_count == 0) {
        return 0;
    }

    const struct e820_entry *entries =
        (const struct e820_entry *)(uintptr_t)(E820_MAP_BASE + 8);

    int written = 0;
    for (uint32_t i = 0; i < entry_count && written < max_regions; i++) {
        if (entries[i].type == E820_TYPE_USABLE) {
            out[written].base   = entries[i].base;
            out[written].length = entries[i].length;
            written++;
        }
    }

    return written;
}
