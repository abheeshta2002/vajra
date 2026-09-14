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

/* Highest address of usable RAM detected (i.e. the "top" of the
 * range starting at 0) -- matches the assembly kernel's mem_top
 * semantic. Divide by 1MB for a human-readable total. */
uint64_t memory_get_total_bytes(void);

#endif
