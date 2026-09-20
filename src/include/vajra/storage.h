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
 * Roadmap Phase 17 added a real on-disk directory (name -> {id,
 * trust, size}, one dedicated sector -- see core/storage.c's
 * DIRECTORY_LBA) so the namespace survives between separate
 * `qemu-system-x86_64` launches against the SAME disk.img, not just
 * within one boot. storage_create_object() (kernel-only, used by
 * kernel_main's own fixed demo objects) is idempotent by name for
 * exactly this reason: a second boot against a disk that already has
 * "payload.bin" must reuse its existing id, not create a duplicate
 * and eventually exhaust MAX_OBJECTS. storage_create_named() is the
 * new, syscall-reachable, STRICT counterpart (SYS_CREATE_NAME) --
 * fails on a name collision instead of handing back someone else's
 * id. Naming operations (lookup/list/rename/delete) are layered
 * ABOVE the object capability model, not a replacement for it: a
 * lookup returns an id and nothing else, never a capability -- see
 * hal.h's SYS_LOOKUP_NAME and docs/ROADMAP.md's Phase 17 "resolved
 * design constraint".
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

/* Roadmap Phase 17: the runtime-reachable, strict create -- unlike
 * storage_create_object() above, fails (-1) if `name` already names a
 * live object rather than returning its id. See core/storage.c's own
 * comment on why the two need different collision policies. Also
 * fails if the object table is full. */
int storage_create_named(const char *name);

/* Returns the object id for `name`, or -1 if no live object has it.
 * No capability semantics here at all -- see this file's own top
 * comment; the syscall layer (hal/x86_64/syscall.c) enforces the "no
 * capability required" policy by simply not checking one. */
int storage_lookup_by_name(const char *name);

/* Phase 19: 1 if `id` is a live USER-domain object -- one created at
 * runtime through storage_create_named() (SYS_CREATE_NAME, and package
 * installs), 0 for anything kernel_main seeded through
 * storage_create_object() (system objects, the utility programs) or an
 * invalid id. Persisted. CAP_USER_DATA's scope is exactly these. */
int storage_is_user_object(int id);

/* Size in bytes of object `id`, or -1 if it is not live. */
int storage_size(int id);

/* Roadmap Phase 25: the current generation of id `id` (bumped every
 * time this id is handed out, storage_create_object()/
 * storage_create_named(), including the first time), or -1 if `id` is
 * out of range. core/actor.c's actor_has_cap() compares a capability's
 * recorded generation against this to refuse one whose target id was
 * deleted and reused since the grant. */
int storage_object_generation(int id);

/* Fills in the `nth` LIVE object in the namespace (0-based, in id
 * order, gaps from storage_delete() skipped automatically) --
 * name_out must be at least 16 bytes. Returns 1 and fills the
 * out-params on success, 0 once `nth` runs past how many objects
 * actually exist (the caller's signal to stop enumerating). */
int storage_get_by_index(int nth, char *name_out, int *id_out, int *trust_out, uint32_t *size_out);

/* Renames object `id` to `new_name`. Returns 0 on success, -1 if id
 * is invalid or `new_name` is already taken by a DIFFERENT live
 * object (renaming to its own current name is a harmless no-op). */
int storage_rename(int id, const char *new_name);

/* Removes object `id` from the namespace, freeing its slot for reuse
 * by a later create. Does not zero its on-disk data sectors -- see
 * this function's own comment in core/storage.c for why that's safe.
 * Returns 0 on success, -1 if id is invalid. */
int storage_delete(int id);

/* Reads up to `len` bytes starting at byte `off`; returns the bytes read
 * (0 at or past the end), or -1 (bad id, or REJECTED). */
int storage_read_at(int id, uint32_t off, void *buf, uint32_t len);

/* Writes `len` bytes at `off`, growing the object (a gap is zero-filled).
 * len == 0 truncates to `off`. Resets trust and bumps the modified time.
 * Returns bytes written, or -1 (bad id, past the size limit, read-only). */
int storage_write_at(int id, uint32_t off, const void *buf, uint32_t len);

/* Sets an object's flags (OBJ_FLAG_READONLY). 0 on success, -1 if bad id. */
int storage_set_flags(int id, int flags);

/* Fills the metadata storage_get_by_index() does not carry. */
int storage_get_meta(int id, uint32_t *created, uint32_t *modified, int *flags);

/* The largest a single object can be, in bytes. */
#define STORAGE_OBJECT_MAX 8192

#endif
