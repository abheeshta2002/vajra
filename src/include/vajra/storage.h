#ifndef VAJRA_STORAGE_H
#define VAJRA_STORAGE_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * A capability-addressed object store, not a POSIX-style path
 * filesystem: actors hold capabilities (CAP_READ_OBJECT,
 * CAP_WRITE_OBJECT, CAP_PROMOTE_OBJECT -- see actor.h) to specific
 * OBJECT IDS, never blanket "the filesystem". There is no directory
 * tree, no path traversal, and no way to reach an object except by
 * already holding a capability naming it. This is the roadmap's
 * Phase 8 -- see docs/ROADMAP.md.
 *
 * Deliberately narrow: object metadata (name, size, disk location,
 * trust state) lives in kernel RAM, rebuilt fresh at every boot by
 * whatever storage_create_object() calls kernel_main makes -- there
 * is no on-disk catalog/directory format yet, only raw sector ranges
 * this file hands out and remembers for the current boot. A real
 * persistent catalog is future work once something needs objects to
 * survive a reboot; this milestone is about the capability-addressed
 * access model, not on-disk format design.
 * ---------------------------------------------------------------- */

typedef enum {
    OBJ_UNTRUSTED = 0, /* fresh or freshly overwritten content -- see storage_write() */
    OBJ_QUARANTINED,
    OBJ_ANALYZED,
    OBJ_TRUSTED,
    OBJ_REJECTED /* terminal -- see storage_reject(). Deliberately last in this enum
                    (not e.g. inserted before OBJ_TRUSTED): a rejected object isn't
                    "less trusted than QUARANTINED", it's a different, dead-end outcome
                    that storage_read() refuses outright regardless of capability --
                    see its own comment. */
} obj_trust_t;

void storage_init(void);

/* Kernel-only, like actor_spawn()'s own unconditional primitive --
 * called from kernel_main before the scheduler starts. Reserves a
 * fresh object backed by its own dedicated disk sectors, empty
 * (size 0) and OBJ_UNTRUSTED. Returns its object id, or -1 if the
 * object table is full. There is deliberately no runtime
 * (syscall-reachable) way to create one yet -- see this file's own
 * top comment. */
int storage_create_object(const char *name);

/* Reads up to buf_len bytes of object `id`'s current contents into
 * buf. Returns the number of bytes actually read (the object's own
 * size, capped by buf_len -- a short read, not an error, if the
 * object holds less than buf_len), or -1 if id is invalid OR the
 * object is OBJ_REJECTED -- a second, TRUST-STATE gate on top of (not
 * instead of) the capability check the syscall layer already does:
 * holding CAP_READ_OBJECT is necessary but not sufficient once
 * something has been rejected. That refusal happens here, not just as
 * an application-level convention, so a rejected object stays
 * unreadable even for an actor that legitimately holds the
 * capability. Does NOT check capabilities itself -- that's the
 * syscall layer's job (hal/x86_64/syscall.c), consistent with keeping
 * this module unaware of actors entirely. */
int storage_read(int id, void *buf, uint32_t buf_len);

/* REPLACES object `id`'s entire contents with the first len bytes of
 * buf (no partial-offset writes yet). Resets its trust to
 * OBJ_UNTRUSTED: new content invalidates whatever trust decision
 * applied to the old content, unconditionally. Returns bytes written,
 * or -1 if id is invalid, len exceeds the object's fixed on-disk
 * capacity, or the underlying disk write failed. */
int storage_write(int id, const void *buf, uint32_t len);

/* Advances object `id` one trust level
 * (OBJ_UNTRUSTED -> OBJ_QUARANTINED -> OBJ_ANALYZED -> OBJ_TRUSTED).
 * Returns the new trust level, or -1 if id is invalid, already
 * OBJ_TRUSTED (nothing further to promote to), or OBJ_REJECTED
 * (a dead end -- see storage_reject()). */
int storage_promote(int id);

/* Marks object `id` OBJ_REJECTED, unconditionally and permanently --
 * there is no path back from this state; a rejected object must be
 * overwritten (storage_write(), which resets it to OBJ_UNTRUSTED,
 * starting the trust decision over from scratch) to ever be usable
 * again. Returns 0 on success, or -1 if id is invalid. */
int storage_reject(int id);

/* Returns object `id`'s current trust level, or -1 if id is invalid. */
int storage_get_trust(int id);

#endif
