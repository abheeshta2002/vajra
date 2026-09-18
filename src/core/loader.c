#include "vajra/hal.h"
#include "vajra/storage.h"
#include "vajra/memory.h"
#include "vajra/actor.h"
#include "vajra/loader.h"

/* ------------------------------------------------------------------
 * Roadmap Phase 16: the program loader. See loader.h for the format
 * and the layering reasoning (this file, like core/storage.c and
 * core/net.c before it, has no idea actors or capabilities exist --
 * that check happens one layer up, in hal/x86_64/syscall.c, and inside
 * core/actor.c's own actor_spawn_program_child() for the parts of the
 * check that only need actor-internal state).
 *
 * A whole object's worth of bytes has to be staged somewhere before
 * it can be copied into the freshly allocated program pages -- this
 * scratch buffer matches core/storage.c's own OBJECT_MAX_BYTES exactly
 * (32KB), a plain static .bss array rather than a stack buffer, same
 * reasoning as storage.c's own: this runs on whichever actor's kernel
 * stack happens to be active, and 32KB is far more than that stack
 * should be asked to spare. Safe as shared state for the same reason
 * every other static scratch buffer in this codebase is: interrupts
 * stay disabled for the whole syscall this runs inside of.
 * ---------------------------------------------------------------- */

#define LOADER_SCRATCH_BYTES (4 * 512) /* must match storage.c's OBJECT_MAX_BYTES */
static uint8_t scratch[LOADER_SCRATCH_BYTES];

int loader_spawn_program(int object_id) {
    int n = storage_read(object_id, scratch, sizeof(scratch));
    if (n < (int)sizeof(struct program_header)) {
        return -1; /* too short to even hold the header */
    }

    struct program_header hdr;
    const uint8_t *p = scratch;
    hdr.magic        = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    hdr.entry_offset = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    hdr.code_size    = (uint32_t)p[8] | ((uint32_t)p[9] << 8) | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);

    if (hdr.magic != PROGRAM_MAGIC) {
        return -1; /* not a program image at all -- e.g. one of the demo's own text objects */
    }
    if (hdr.code_size == 0 || hdr.code_size > PROGRAM_WINDOW_MAX) {
        return -1; /* doesn't fit the per-actor program window */
    }
    if ((uint32_t)n < sizeof(struct program_header) + hdr.code_size) {
        return -1; /* object is shorter than the header claims -- truncated/corrupt */
    }
    if (hdr.entry_offset >= hdr.code_size) {
        return -1; /* entry point outside the code that was actually loaded */
    }

    /* alloc_dma_pages(), not alloc_pages_contig() -- this memory is
     * written from inside this syscall, while the CALLING actor's own
     * restricted CR3 is still active (syscalls don't switch CR3), the
     * exact same reasoning hal/x86_64/virtio_net.c's own DMA buffers
     * already follow (see include/vajra/memory.h's own comment).
     * alloc_pages_contig() can return a physical page inside the
     * 1MB-2MB actor-private window, present only in whichever actor
     * happens to own THAT exact page -- not the calling actor here,
     * not the newly spawned one either until hal_address_space_map_
     * program() runs afterward. A first version used
     * alloc_pages_contig() and genuinely page-faulted (#PF, not-
     * present supervisor write) the first time this actually ran,
     * confirmed by CR2 landing inside that exact window -- the same
     * bug class Milestone 14's own changelog already documented once,
     * just hitting new code that repeated it. */
    int pages = (int)((hdr.code_size + 4095) / 4096);
    void *phys = alloc_dma_pages(pages);
    if (!phys) {
        return -1;
    }

    uint8_t *dst = (uint8_t *)phys;
    const uint8_t *src = scratch + sizeof(struct program_header);
    for (uint32_t i = 0; i < hdr.code_size; i++) {
        dst[i] = src[i];
    }

    return actor_spawn_program_child((uint64_t)phys, (uint64_t)pages * 4096, hdr.entry_offset);
}
