#ifndef VAJRA_MEMORY_H
#define VAJRA_MEMORY_H

#include <stdint.h>

void memory_init(void);

/* Returns a pointer to a freshly zeroed 4KB page, or NULL if out of
 * memory. */
void *alloc_page(void);

/* Returns a page (from alloc_page()) to the allocator so a later
 * alloc_page() call can hand it out again. */
void free_page(void *addr);

/* Returns `count` PHYSICALLY CONTIGUOUS, freshly zeroed 4KB pages, or
 * NULL if that many consecutive free pages can't be found. Unlike
 * alloc_page(), consecutive calls to alloc_page() were never a
 * guarantee of contiguity (only true by accident of the bump
 * allocator's current scan order) -- this exists because
 * hal/x86_64/virtio_net.c's virtqueues are a hardware-defined layout
 * the device reads via ONE physical base address, so the descriptor
 * table, available ring, and used ring genuinely have to sit in one
 * contiguous region, not just three independently-valid pages. */
void *alloc_pages_contig(int count);

/* Highest address of usable RAM detected (i.e. the "top" of the
 * range starting at 0) -- matches the assembly kernel's mem_top
 * semantic. Divide by 1MB for a human-readable total. */
uint64_t memory_get_total_bytes(void);

#endif
