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

/* Like alloc_pages_contig(), but returns memory from the "commons"
 * region (2MB and above) instead of the 1MB-2MB actor-private window
 * -- i.e. memory that stays mapped (supervisor-only) in EVERY actor's
 * own address space, not just whichever actor happens to own that
 * exact page (see hal/x86_64/paging.c's own comment on the 1MB-2MB
 * window's whole point). Needed by hal/x86_64/virtio_net.c's DMA
 * buffers: they're touched from inside a syscall while some actor's
 * own restricted CR3 is active (Milestone 14's actor_network_peer,
 * not just kernel_main's own earlier one-off ARP demo), and a buffer
 * living in the private window would simply not be present in that
 * CR3 at all -- confirmed by an actual page fault (not a review-time
 * guess) the first time an actor, rather than kernel_main itself,
 * called into the network driver. */
void *alloc_dma_pages(int count);

/* Highest address of usable RAM detected (i.e. the "top" of the
 * range starting at 0) -- matches the assembly kernel's mem_top
 * semantic. Divide by 1MB for a human-readable total. */
uint64_t memory_get_total_bytes(void);

#endif
