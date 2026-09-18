#include "vajra/hal.h"
#include "vajra/storage.h"

/* ------------------------------------------------------------------
 * The object store implementation -- see storage.h for the design.
 * 100% portable: it only calls hal_disk_read()/hal_disk_write()
 * (plain sector numbers and byte buffers, no x86-specific format
 * involved at all -- unlike E820, block I/O is already shaped
 * generically, so there's no separate translation step the way
 * hal/x86_64/e820.c exists for core/memory.c). This file has no idea
 * actors or capabilities exist; that check happens one layer up, in
 * hal/x86_64/syscall.c, keeping this module reusable for anything
 * that might need object storage later without dragging the actor
 * system in as a dependency.
 * ---------------------------------------------------------------- */

#define MAX_OBJECTS          8
/* Tried bumping this to 64 sectors (32KB) for Phase 16's loaded
 * programs first -- unnecessary and genuinely harmful: the actual
 * "hello world" program (src/userland/) compiles to 251 bytes total,
 * and the extra 30KB this and core/loader.c's own matching scratch
 * buffer added to kernel .bss pushed __bss_end (0xa220c) past the
 * fixed low addresses (0x90000+) boot.asm's own page tables and the
 * AP trampoline live at -- confirmed by a real triple fault, the same
 * ".bss swallowing fixed structures" bug class as actor.h's own
 * MAX_ACTORS note. 8 sectors (4KB) is still 2x Milestone 14's original
 * 2KB, real headroom for this milestone's actual program without
 * repeating that mistake -- revisit together with core/loader.c's
 * LOADER_SCRATCH_BYTES (must match) if a genuinely bigger program
 * ever needs it, not preemptively. */
#define SECTORS_PER_OBJECT   4
#define OBJECT_MAX_BYTES     (SECTORS_PER_OBJECT * 512)
#define OBJECT_DATA_BASE_LBA 100                         /* clear of the boot sector + kernel image --
                                                              see tools/build-c.ps1's KERNEL_SECTORS cap */

struct object {
    int in_use;
    char name[16];
    uint64_t lba;
    uint32_t size_bytes;
    obj_trust_t trust;
};

static struct object objects[MAX_OBJECTS];
static int object_count = 0;

/* Sector-aligned scratch space for staging a whole object's worth of
 * data through hal_disk_read()/hal_disk_write(), which only ever
 * moves whole 512-byte sectors. Static, not stack-allocated: this is
 * called from syscall context on whichever actor's kernel stack is
 * active, and OBJECT_MAX_BYTES is larger than that stack should be
 * asked to spare. Safe as plain shared state for the same reason
 * every other static scratch buffer in this codebase is: interrupts
 * stay disabled for the whole syscall this runs inside of, so there
 * is never more than one caller here at a time. */
static uint8_t scratch[OBJECT_MAX_BYTES];

void storage_init(void) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        objects[i].in_use = 0;
        objects[i].name[0] = 0;
        objects[i].lba = 0;
        objects[i].size_bytes = 0;
        objects[i].trust = OBJ_UNTRUSTED;
    }
    object_count = 0;
}

int storage_create_object(const char *name) {
    if (object_count >= MAX_OBJECTS) {
        return -1;
    }

    int id = object_count++;
    int i = 0;
    for (; i < (int)sizeof(objects[id].name) - 1 && name[i]; i++) {
        objects[id].name[i] = name[i];
    }
    objects[id].name[i] = 0;
    objects[id].lba = OBJECT_DATA_BASE_LBA + (uint64_t)id * SECTORS_PER_OBJECT;
    objects[id].size_bytes = 0;
    objects[id].trust = OBJ_UNTRUSTED;
    objects[id].in_use = 1;
    return id;
}

int storage_read(int id, void *buf, uint32_t buf_len) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    if (objects[id].trust == OBJ_REJECTED) {
        return -1; /* a real dead end, not just advisory -- see this function's own comment */
    }

    if (hal_disk_read(objects[id].lba, SECTORS_PER_OBJECT, scratch) != 0) {
        return -1;
    }

    uint32_t n = objects[id].size_bytes;
    if (n > buf_len) {
        n = buf_len;
    }
    for (uint32_t i = 0; i < n; i++) {
        ((uint8_t *)buf)[i] = scratch[i];
    }
    return (int)n;
}

int storage_write(int id, const void *buf, uint32_t len) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    if (len > OBJECT_MAX_BYTES) {
        return -1;
    }

    for (uint32_t i = 0; i < OBJECT_MAX_BYTES; i++) {
        scratch[i] = (i < len) ? ((const uint8_t *)buf)[i] : 0;
    }

    if (hal_disk_write(objects[id].lba, SECTORS_PER_OBJECT, scratch) != 0) {
        return -1;
    }

    objects[id].size_bytes = len;
    objects[id].trust = OBJ_UNTRUSTED; /* new content invalidates any prior trust decision */
    return (int)len;
}

int storage_promote(int id) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    /* Explicit about both terminal cases rather than relying on
     * OBJ_REJECTED's enum ordering to fall out of a single
     * `>= OBJ_TRUSTED` comparison -- correct either way today, but
     * the intent (two DIFFERENT reasons promotion stops) shouldn't
     * depend on remembering which constant sorts where. */
    if (objects[id].trust == OBJ_TRUSTED || objects[id].trust == OBJ_REJECTED) {
        return -1;
    }
    objects[id].trust = (obj_trust_t)(objects[id].trust + 1);
    return (int)objects[id].trust;
}

int storage_reject(int id) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    objects[id].trust = OBJ_REJECTED;
    return 0;
}

int storage_get_trust(int id) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    return (int)objects[id].trust;
}
