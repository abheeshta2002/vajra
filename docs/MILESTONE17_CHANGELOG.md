# Milestone 17 — A persistent filesystem namespace over the object store

## What this is

Roadmap Phase 17. Phase 8's object store (`core/storage.c`) stays exactly
what it always was — capability-addressed, not path-addressed: an actor
still needs `CAP_READ_OBJECT`/`CAP_WRITE_OBJECT`/`CAP_PROMOTE_OBJECT` naming
a specific object id to touch its contents, full stop. This milestone adds
a naming layer ON TOP: a real, on-disk directory mapping human-readable
names to object ids, so `main.c`'s object table (previously rebuilt from
scratch, in RAM only, every single boot — see the milestone below's own
"What's included") now survives between separate `qemu-system-x86_64`
launches against the same `disk.img`, not just within one boot.

## What's included

- **A real on-disk directory** (`core/storage.c`, `DIRECTORY_LBA = 90`,
  one sector): magic + count + up to `MAX_OBJECTS` fixed 32-byte entries
  (in_use, trust, size, 16-byte name), serialized/deserialized with
  explicit byte-level layout (not a raw struct write) so the format
  doesn't depend on compiler struct padding — the same reasoning
  `core/loader.c`'s header decode already uses. Written after every
  mutation (`directory_save()`), read once at boot (`directory_load()`).
- **Idempotent boot-time object creation.** `storage_create_object()` —
  `kernel_main`'s own unconditional entry point — now checks for an
  existing object of that name first and returns its id instead of
  creating a duplicate. Necessary the moment persistence became real:
  without it, a second boot against an already-formatted disk would
  recreate `payload.bin`/`suspicious.bin`/`hello.bin` as NEW objects,
  both exhausting `MAX_OBJECTS` (8) after a few reboots and breaking
  every fixed object-id `#define` `main.c`'s demo pipeline assumes.
- **A strict, syscall-reachable create** (`storage_create_named()`,
  `SYS_CREATE_NAME`) — the actual gap-closer `storage.h`'s own comment
  used to describe ("no runtime-reachable way to create one yet").
  Unlike the idempotent kernel-only entry point above, this one refuses
  a name collision outright rather than silently handing back someone
  else's id.
- **Four new syscalls**, all in `hal.h`/`syscall.c`:
  - `SYS_LOOKUP_NAME` — no capability required at all. An id is public
    knowledge, like a phone book entry; the capability to act on what it
    points at is still a separate, explicit grant (Phase 17's own
    "resolved design constraint" in `docs/ROADMAP.md`).
  - `SYS_LIST_OBJECTS` — gated behind a new `CAP_LIST_NAMES(0)`,
    deliberately distinct from lookup: enumerating every name that
    EXISTS is a different authority than looking up one you already
    know (§3 invariant 2 — no ambient visibility), the same reasoning
    Phase 19's `CAP_INTROSPECT` will use for `ps`.
  - `SYS_RENAME_OBJECT` / `SYS_DELETE_NAME` — gated behind two new
    per-object capabilities, `CAP_RENAME_OBJECT`/`CAP_DELETE_OBJECT`,
    deliberately NOT a reuse of `CAP_WRITE_OBJECT`: renaming/deleting
    changes the namespace binding, not the object's contents — Phase
    17's own "distinct from, layered above" design.
  - `SYS_CREATE_NAME` auto-grants the creating actor
    `CAP_READ_OBJECT`+`CAP_WRITE_OBJECT`+`CAP_RENAME_OBJECT`+
    `CAP_DELETE_OBJECT` for what it just made — the same "creator gets
    natural authority over what it created" pattern `SYS_SPAWN`'s own
    `CAP_SEND`/`CAP_TERMINATE` auto-grant already establishes.
- **Slot reuse, not just growth.** `storage_delete()` frees a slot for a
  later create to reuse (`alloc_object()`'s own free-slot scan before
  ever growing `object_count`), so create/delete churn stays bounded by
  `MAX_OBJECTS` regardless of how many objects have existed over time.
- **A new demo actor** (`actor_namer`, slot 13 — 14 ring-3 actors total
  now) exercising the full lifecycle: looks up `payload.bin` BY NAME for
  the first time anywhere in this codebase (previously only ever
  referenced by its hardcoded `PAYLOAD_OBJECT_ID`), lists the namespace,
  creates `notes.txt`, renames it to `todo.txt`, deletes it, and confirms
  the lookup then fails.

## Verified, not assumed

- A clean single-instance boot: all three pre-existing demo objects
  found by name, the namespace listed correctly (`payload.bin`,
  `suspicious.bin`, `hello.bin`, in that order), create/rename/delete/
  confirm-gone all succeeded, and the rest of the existing 13-actor demo
  ran exactly as before — nothing regressed.
- **The actual persistence claim, not just the on-disk format compiling**:
  booted the SAME already-built `disk.img` a second time, unmodified. The
  second boot's `Namer` still found `payload.bin` at id 0 (not a fresh
  duplicate) and reused id 3 for `notes.txt` (the slot the first boot's
  own delete had freed) — real evidence the directory round-tripped
  through disk correctly and the idempotent-create guard did its job,
  not merely that the write path didn't crash.

## Known follow-ups for the next phase

- Persistence is between separate QEMU launches against one `disk.img`,
  not between builds — `tools/build-c.ps1` still regenerates `disk.img`
  from scratch every build, unchanged from every earlier milestone.
- `DIRECTORY_LBA = 90` now has roughly 6.5 sectors of margin before the
  kernel image (currently ~83.5 sectors) would reach it — worth
  revisiting together with `OBJECT_DATA_BASE_LBA` if the kernel grows
  much further, the same "watch the fixed low-memory/disk layout
  boundaries" discipline this project has needed several times before
  (see Milestone 13's and 16's own changelogs).
- No hierarchical paths yet (`/a/b/c`) — flat names only, matching what
  `MAX_OBJECTS = 8` actually needs today. A real directory tree is
  future work once something needs nesting.
- No console/name locking beyond what already existed — the interleaved
  boot log above (`Namer`'s own lines threading through other actors'
  output) is the same pre-existing, harmless console-output-locking gap
  Milestone 4's own changelog already noted, not a new bug.
- `storage_get_trust()` predates this milestone and is now partially
  redundant with `storage_get_by_index()`'s own trust field — left as-is,
  not worth merging until something actually needs the merge.
