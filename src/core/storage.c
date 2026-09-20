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

#define MAX_OBJECTS          64 /* was 8, then 14, then 28. The directory is NINE sectors (64-byte entries) */
#define DIRECTORY_SECTORS    9  /* LBAs 400-408; object data starts at 420: 8 + 64*64 = 4104 <= 4608 */
#define NAME_MAX_CHARS       39 /* names are 40 bytes on disk and in memory: 39 characters + NUL (paths live in the name) */
#define DIR_ENTRY_BYTES      64
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
#define SECTORS_PER_OBJECT   16 /* 8 KB per object (was 2 KB). 64 objects x 16 = 1024 sectors, LBA 270..1293 */
#define OBJECT_MAX_BYTES     (SECTORS_PER_OBJECT * 512)
/* Roadmap Phase 18 pushed the kernel image to ~93.7 sectors (47985
 * bytes) and, exactly as Milestone 17's own changelog warned it might,
 * collided with the OLD DIRECTORY_LBA (90) -- the same ".bss/disk
 * layout growth silently overruns a fixed low structure" bug class
 * this project has now hit several times (Milestones 1, 5, 7, 13, 16,
 * and now the disk-layout version of it here), just caught this time
 * by literally computing the new kernel size before it caused a data
 * corruption bug instead of a boot fault. Both constants below now sit
 * clear of KERNEL_SECTORS (src/boards/pc-bios/boot.asm, now 256 -- read
 * in chunks, see that file's fix #6, and enforced at build time by
 * tools/build-c.ps1) -- the boot loader's own hard ceiling on how much
 * of the disk it will ever treat as "the kernel image" -- rather than
 * clear of today's actual kernel size, so ordinary future growth up to
 * that limit can't repeat this. These were 125/130 against the old
 * 120-sector cap; moved past 256 when the cap was raised. */
#define OBJECT_DATA_BASE_LBA 420 /* was 270; moved past the raised KERNEL_SECTORS=384 and the 9-sector directory at 400 */

/* One dedicated sector holding the persistent name -> {id, trust,
 * size} directory (roadmap Phase 17), so the namespace survives
 * between separate `qemu-system-x86_64` launches against the SAME
 * already-built disk.img (not between rebuilds -- tools/build-c.ps1
 * regenerates disk.img from scratch every build, same as it always
 * has). See OBJECT_DATA_BASE_LBA's own comment for why this now sits
 * past KERNEL_SECTORS=256, not just past today's actual kernel size. */
#define DIRECTORY_LBA   400
#define DIRECTORY_MAGIC 0x33524456u /* 'VDR3': directory format v3 (64-byte entries, 39-char names, timestamps). An older disk reads as blank. */

struct object {
    int in_use;
    char name[40];
    uint32_t created;  /* seconds since 2000-01-01 (hal_rtc_epoch) */
    uint32_t modified;
    int flags;         /* OBJ_FLAG_READONLY */
    uint64_t lba;
    uint32_t size_bytes;
    obj_trust_t trust;
    int user; /* Phase 19: 1 = a user-domain object (created at runtime through SYS_CREATE_NAME or
                  installed as a package), 0 = seeded by kernel_main (system objects, utilities).
                  Persisted in the directory. An actor holding CAP_USER_DATA has read/write/rename/
                  delete authority over exactly the user-domain objects -- see core/actor.c. */
    int generation; /* roadmap Phase 25: bumped every time alloc_object() hands this id out --
                        including the FIRST time, so generation 0 never means "a real object,"
                        the same "0 reads as empty" convention actor.c's own capability op field
                        already uses. A capability recorded against an earlier generation of this
                        id must stop working once storage_delete() frees it and a later create
                        reuses the slot -- see storage_object_generation() and
                        core/actor.c's actor_add_cap()/actor_has_cap(). */
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

/* Sector-sized scratch for the directory itself -- separate from
 * `scratch` above (which is sized/used for whole-object I/O) purely
 * for clarity, not because both couldn't safely share one buffer
 * under the same single-caller-at-a-time reasoning `scratch`'s own
 * comment already gives. */
static uint8_t dir_buf[512 * DIRECTORY_SECTORS];

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Compares a stored (fixed 16-byte, NUL-padded) name against a
 * caller-supplied NUL-terminated one, at most 16 bytes -- same
 * "dereference a raw actor-supplied pointer, bounded, trusting
 * NUL-termination" convention SYS_WRITE already uses, just capped
 * far tighter here since object names are never expected to be long. */
static int name_eq(const char *stored, const char *given) {
    for (int i = 0; i < 40; i++) {
        if (stored[i] != given[i]) {
            return 0;
        }
        if (stored[i] == 0) {
            return 1;
        }
    }
    return 1;
}

/* 1 if `name` is 1..NAME_MAX_CHARS characters -- a longer name must be refused,
 * not truncated (a truncated name would never be found again by the name given). */
static int name_ok(const char *name) {
    int n = 0;
    while (name[n]) { n++; }
    return n >= 1 && n <= NAME_MAX_CHARS;
}

static int find_by_name(const char *name) {
    for (int id = 0; id < object_count; id++) {
        if (objects[id].in_use && name_eq(objects[id].name, name)) {
            return id;
        }
    }
    return -1;
}

/* Serializes objects[0..object_count) to dir_buf and writes it to
 * DIRECTORY_LBA. Called after every mutation (create/write/promote/
 * reject/rename/delete) rather than batched -- the whole directory is
 * one sector, MAX_OBJECTS is tiny (8), and "always consistent on disk
 * after any successful call returns" is a much simpler invariant to
 * keep than partial/deferred writes would be. Explicit byte-level
 * layout (not a raw struct write) so the on-disk format doesn't
 * depend on this compiler's struct padding -- same reasoning
 * core/loader.c's header decode already uses.
 * Layout: [0..3] magic, [4] object_count, then MAX_OBJECTS 32-byte
 * entries from offset 8: [0] in_use, [1] trust, [2..3] reserved,
 * [4..7] size_bytes (LE), [8..23] name (NUL-padded), [24..31] reserved. */
static void directory_save(void) {
    for (int i = 0; i < 512 * DIRECTORY_SECTORS; i++) {
        dir_buf[i] = 0;
    }
    dir_buf[0] = (uint8_t)(DIRECTORY_MAGIC);
    dir_buf[1] = (uint8_t)(DIRECTORY_MAGIC >> 8);
    dir_buf[2] = (uint8_t)(DIRECTORY_MAGIC >> 16);
    dir_buf[3] = (uint8_t)(DIRECTORY_MAGIC >> 24);
    dir_buf[4] = (uint8_t)object_count;

    /* v3 entry (64 bytes, from offset 8): [0] in_use, [1] trust, [2] user,
     * [3] flags, [4..7] size, [8..11] created, [12..15] modified,
     * [16..55] name (40 bytes, NUL-padded), [56..63] reserved. */
    for (int id = 0; id < object_count; id++) {
        int off = 8 + id * DIR_ENTRY_BYTES;
        dir_buf[off + 0] = (uint8_t)objects[id].in_use;
        dir_buf[off + 1] = (uint8_t)objects[id].trust;
        dir_buf[off + 2] = (uint8_t)objects[id].user;
        dir_buf[off + 3] = (uint8_t)objects[id].flags;
        wr32(&dir_buf[off + 4], objects[id].size_bytes);
        wr32(&dir_buf[off + 8], objects[id].created);
        wr32(&dir_buf[off + 12], objects[id].modified);
        int j = 0;
        for (; j < NAME_MAX_CHARS && objects[id].name[j]; j++) {
            dir_buf[off + 16 + j] = (uint8_t)objects[id].name[j];
        }
        dir_buf[off + 16 + j] = 0;
    }

    hal_disk_write(DIRECTORY_LBA, DIRECTORY_SECTORS, dir_buf);
}

/* Rebuilds objects[]/object_count from whatever directory_save() last
 * wrote -- called once, from storage_init(). A disk that's never been
 * through directory_save() (freshly built by tools/build-c.ps1, or
 * genuinely blank) reads back as all-zero bytes at DIRECTORY_LBA,
 * which will not match DIRECTORY_MAGIC -- treated as "nothing
 * persisted yet", not an error, so storage_init()'s own zeroing loop
 * stands as the effective starting state exactly as it did before
 * this milestone. */
static void directory_load(void) {
    if (hal_disk_read(DIRECTORY_LBA, DIRECTORY_SECTORS, dir_buf) != 0) {
        return;
    }
    uint32_t magic = (uint32_t)dir_buf[0] | ((uint32_t)dir_buf[1] << 8) |
                      ((uint32_t)dir_buf[2] << 16) | ((uint32_t)dir_buf[3] << 24);
    if (magic != DIRECTORY_MAGIC) {
        return;
    }

    int count = dir_buf[4];
    if (count > MAX_OBJECTS) {
        count = MAX_OBJECTS; /* defensive -- a corrupt on-disk count must never overrun objects[] */
    }

    for (int id = 0; id < count; id++) {
        int off = 8 + id * DIR_ENTRY_BYTES;
        objects[id].in_use = dir_buf[off + 0];
        objects[id].trust = (obj_trust_t)dir_buf[off + 1];
        objects[id].user = dir_buf[off + 2];
        objects[id].flags = dir_buf[off + 3];
        objects[id].size_bytes = rd32(&dir_buf[off + 4]);
        objects[id].created = rd32(&dir_buf[off + 8]);
        objects[id].modified = rd32(&dir_buf[off + 12]);
        int j = 0;
        for (; j < NAME_MAX_CHARS && dir_buf[off + 16 + j]; j++) {
            objects[id].name[j] = (char)dir_buf[off + 16 + j];
        }
        objects[id].name[j] = 0;
        objects[id].lba = OBJECT_DATA_BASE_LBA + (uint64_t)id * SECTORS_PER_OBJECT;
    }
    object_count = count;
}

void storage_init(void) {
    for (int i = 0; i < MAX_OBJECTS; i++) {
        objects[i].in_use = 0;
        objects[i].name[0] = 0;
        objects[i].lba = 0;
        objects[i].size_bytes = 0;
        objects[i].trust = OBJ_UNTRUSTED;
        objects[i].user = 0;
        objects[i].flags = 0;
        objects[i].created = 0;
        objects[i].modified = 0;
    }
    object_count = 0;
    directory_load();
}

/* Shared by storage_create_object() and storage_create_named() below
 * -- everything except the by-name collision policy, which differs
 * between the two (see each one's own comment). Reuses a freed slot
 * (storage_delete()) before ever growing object_count, so repeated
 * create/delete churn stays bounded by MAX_OBJECTS regardless of how
 * many objects have existed over time, not just how many exist now. */
static int alloc_object(const char *name, int user) {
    int id = -1;
    for (int i = 0; i < object_count; i++) {
        if (!objects[i].in_use) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        if (object_count >= MAX_OBJECTS) {
            return -1;
        }
        id = object_count++;
    }

    int i = 0;
    for (; i < NAME_MAX_CHARS && name[i]; i++) {
        objects[id].name[i] = name[i];
    }
    objects[id].name[i] = 0;
    objects[id].lba = OBJECT_DATA_BASE_LBA + (uint64_t)id * SECTORS_PER_OBJECT;
    objects[id].size_bytes = 0;
    objects[id].trust = OBJ_UNTRUSTED;
    objects[id].in_use = 1;
    objects[id].user = user;
    objects[id].flags = 0;
    objects[id].created = hal_rtc_epoch();
    objects[id].modified = objects[id].created;
    objects[id].generation++; /* Phase 25: every hand-out of this id, first included -- see
                                  struct object's own comment */
    directory_save();
    return id;
}

/* Kernel-only, unconditional -- kernel_main's own entry point, unlike
 * storage_create_named() below. Idempotent BY NAME as of Phase 17's
 * real persistence: kernel_main calls this unconditionally every
 * single boot for its fixed demo objects (payload.bin etc), and once
 * the directory survives a reboot (directory_load() above), those
 * names already exist by the second boot against the same disk.img.
 * Recreating them anyway would both exhaust MAX_OBJECTS after a few
 * reboots and break every fixed object-id #define this demo assumes
 * (main.c's PAYLOAD_OBJECT_ID etc, which depend on creation ORDER
 * producing the same ids every time). Returning the existing id
 * instead keeps that assumption true whether the directory was empty
 * or already populated. */
int storage_create_object(const char *name) {
    int existing = find_by_name(name);
    if (existing >= 0) {
        return existing;
    }
    return alloc_object(name, 0);
}

/* The runtime-reachable, syscall-facing entry point (SYS_CREATE_NAME)
 * -- deliberately STRICT, unlike storage_create_object() above: a
 * caller asking to create something new must be told "that name is
 * taken", not silently handed back someone else's existing object id.
 * Phase 17's actual "no runtime creation" gap-closer -- storage.h's
 * own top comment described that gap when it was still true. */
int storage_create_named(const char *name) {
    if (!name_ok(name) || find_by_name(name) >= 0) {
        return -1;
    }
    return alloc_object(name, 1);
}

/* 1 if `id` is a live user-domain object (see struct object's `user`). */
int storage_is_user_object(int id) {
    return id >= 0 && id < MAX_OBJECTS && objects[id].in_use && objects[id].user;
}

/* Returns the object id for `name`, or -1 if no live object has it --
 * SYS_LOOKUP_NAME's backing call. Deliberately no capability check
 * anywhere on this path (see hal.h/syscall.c) -- an id is public
 * knowledge, like a phone book entry; the capability to act on what
 * it points at is still a separate, explicit grant. */
int storage_lookup_by_name(const char *name) {
    return find_by_name(name);
}

/* Roadmap Phase 25: the current generation of id `id`, or -1 if `id`
 * is out of range. Deliberately does NOT check `in_use` -- a capability
 * check needs the generation of whatever LIVE object currently sits at
 * this id even to correctly refuse a stale capability against a now-
 * dead-and-not-yet-reused slot (its generation is still what it was
 * when it died; a capability from an earlier generation still won't
 * match). core/actor.c's actor_has_cap() is the only caller. */
int storage_object_generation(int id) {
    if (id < 0 || id >= MAX_OBJECTS) {
        return -1;
    }
    return objects[id].generation;
}

/* Fills in the `nth` LIVE (in_use) object in the namespace, in id
 * order -- `nth` is a position among live objects, not a raw slot
 * index, so a caller enumerating 0,1,2... never sees gaps left by an
 * earlier storage_delete() and always terminates cleanly once nth
 * exceeds how many objects actually exist. Returns 1 and fills the
 * out-params on success, 0 once nth runs past the end (the caller's
 * signal to stop). name_out must be at least 16 bytes. */
int storage_get_by_index(int nth, char *name_out, int *id_out, int *trust_out, uint32_t *size_out) {
    int seen = 0;
    for (int id = 0; id < object_count; id++) {
        if (!objects[id].in_use) {
            continue;
        }
        if (seen == nth) {
            *id_out = id;
            *trust_out = (int)objects[id].trust;
            *size_out = objects[id].size_bytes;
            int j = 0;
            for (; j < NAME_MAX_CHARS && objects[id].name[j]; j++) {
                name_out[j] = objects[id].name[j];
            }
            name_out[j] = 0;
            return 1;
        }
        seen++;
    }
    return 0;
}

/* Renames object `id` to `new_name`, refusing a collision with any
 * OTHER live object's name (renaming to its own current name is a
 * harmless no-op, not an error). Returns 0 on success, -1 if id is
 * invalid, or the name is taken by something else. */
int storage_rename(int id, const char *new_name) {
    if (!name_ok(new_name) || id < 0 || id >= object_count || !objects[id].in_use) {
        return -1;
    }
    int existing = find_by_name(new_name);
    if (existing >= 0 && existing != id) {
        return -1;
    }
    int i = 0;
    for (; i < NAME_MAX_CHARS && new_name[i]; i++) {
        objects[id].name[i] = new_name[i];
    }
    objects[id].name[i] = 0;
    directory_save();
    return 0;
}

/* Removes object `id` from the namespace -- frees its slot for reuse
 * by a later create (see alloc_object() above) and its on-disk
 * directory entry. Does NOT zero the object's own data sectors:
 * SECTORS_PER_OBJECT is tiny and gets overwritten wholesale by
 * whatever create call reuses this slot next, via storage_write()'s
 * existing REPLACES-entire-contents semantics -- there is nothing a
 * stale sector could leak to an id that doesn't exist in the
 * directory to name it. Returns 0 on success, -1 if id is invalid. */
int storage_delete(int id) {
    if (id < 0 || id >= object_count || !objects[id].in_use) {
        return -1;
    }
    objects[id].in_use = 0;
    objects[id].name[0] = 0;
    objects[id].size_bytes = 0;
    objects[id].trust = OBJ_UNTRUSTED;
    objects[id].user = 0;
    objects[id].flags = 0;
    directory_save();
    return 0;
}

int storage_read(int id, void *buf, uint32_t buf_len) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    if (objects[id].trust == OBJ_REJECTED) {
        return -1; /* a real dead end, not just advisory -- see this function's own comment */
    }

    uint32_t n = objects[id].size_bytes;
    if (n > buf_len) {
        n = buf_len;
    }
    if (n == 0) {
        return 0;
    }
    if (hal_disk_read(objects[id].lba, (int)((n + 511) / 512), scratch) != 0) {
        return -1;
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
    if (len > OBJECT_MAX_BYTES || (objects[id].flags & 1)) {
        return -1;
    }

    /* Only the sectors this length touches are written; anything past `len`
     * in the object is dead space (size_bytes is what says where the data
     * ends), so there is no need to zero the rest of the 8 KB. */
    uint32_t sectors = (len + 511) / 512;
    if (sectors == 0) {
        sectors = 1;
    }
    for (uint32_t i = 0; i < sectors * 512; i++) {
        scratch[i] = (i < len) ? ((const uint8_t *)buf)[i] : 0;
    }

    if (hal_disk_write(objects[id].lba, (int)sectors, scratch) != 0) {
        return -1;
    }

    objects[id].size_bytes = len;
    objects[id].trust = OBJ_UNTRUSTED; /* new content invalidates any prior trust decision */
    objects[id].modified = hal_rtc_epoch();
    directory_save();
    return (int)len;
}

int storage_read_at(int id, uint32_t off, void *buf, uint32_t len) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    if (objects[id].trust == OBJ_REJECTED) {
        return -1;
    }
    uint32_t size = objects[id].size_bytes;
    if (off >= size || len == 0) {
        return 0;
    }
    if (len > size - off) {
        len = size - off;
    }
    uint32_t first = off / 512;
    uint32_t last = (off + len - 1) / 512;
    if (hal_disk_read(objects[id].lba + first, (int)(last - first + 1), scratch) != 0) {
        return -1;
    }
    uint32_t skip = off - first * 512;
    for (uint32_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = scratch[skip + i];
    }
    return (int)len;
}

int storage_write_at(int id, uint32_t off, const void *buf, uint32_t len) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    if (objects[id].flags & 1) {
        return -1; /* read-only */
    }
    uint32_t size = objects[id].size_bytes;
    if (len == 0) { /* truncate to `off` (never grows) */
        if (off > size) {
            return -1;
        }
        objects[id].size_bytes = off;
        objects[id].trust = OBJ_UNTRUSTED;
        objects[id].modified = hal_rtc_epoch();
        directory_save();
        return 0;
    }
    if (off > OBJECT_MAX_BYTES || len > OBJECT_MAX_BYTES - off) {
        return -1;
    }
    uint32_t first = off / 512;
    uint32_t last = (off + len - 1) / 512;
    uint32_t nsec = last - first + 1;
    /* read-modify-write just the sectors this touches; a gap between the old
     * end and `off` must read as zeros, so blank what the disk holds there */
    if (hal_disk_read(objects[id].lba + first, (int)nsec, scratch) != 0) {
        return -1;
    }
    uint32_t base = first * 512;
    if (off > size) {
        uint32_t gap_from = (size > base) ? size - base : 0;
        for (uint32_t i = gap_from; i < off - base; i++) {
            scratch[i] = 0;
        }
    }
    for (uint32_t i = 0; i < len; i++) {
        scratch[off - base + i] = ((const uint8_t *)buf)[i];
    }
    if (hal_disk_write(objects[id].lba + first, (int)nsec, scratch) != 0) {
        return -1;
    }
    if (off + len > size) {
        objects[id].size_bytes = off + len;
    }
    objects[id].trust = OBJ_UNTRUSTED;
    objects[id].modified = hal_rtc_epoch();
    directory_save();
    return (int)len;
}

int storage_set_flags(int id, int flags) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    objects[id].flags = flags & 1;
    directory_save();
    return 0;
}

int storage_get_meta(int id, uint32_t *created, uint32_t *modified, int *flags) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    *created = objects[id].created;
    *modified = objects[id].modified;
    *flags = objects[id].flags;
    return 0;
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
    directory_save();
    return (int)objects[id].trust;
}

int storage_reject(int id) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    objects[id].trust = OBJ_REJECTED;
    directory_save();
    return 0;
}

int storage_get_trust(int id) {
    if (id < 0 || id >= MAX_OBJECTS || !objects[id].in_use) {
        return -1;
    }
    return (int)objects[id].trust;
}
