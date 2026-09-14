# Vajra C Rewrite — Milestone 11: The Quarantine/Security Pipeline

## What this is

Roadmap Phase 9. Milestone 10 already had a storage pipeline shape
(download → promote → read), but its "scanner" inspected content
inline and nothing ever decided "no." This milestone adds the two
pieces philosophy §7/§8 actually require: a genuine reject path with
kernel-enforced consequences, and sandboxed execution — the risky
inspection work itself happens inside a narrowly-capable, short-lived
ghost actor, not inline in a trusted one.

## What's included

- **`src/include/vajra/storage.h`** / **`src/core/storage.c`**
  (updated) — a new terminal trust state, `OBJ_REJECTED` (distinct
  from `OBJ_TRUSTED`, not "less trusted than quarantined"), and
  `storage_reject()`. `storage_read()` now refuses an `OBJ_REJECTED`
  object outright, and `storage_promote()` treats `OBJ_REJECTED` as a
  second terminal case alongside `OBJ_TRUSTED`.
- **`src/include/vajra/hal.h`** / **`src/hal/x86_64/syscall.c`**
  (updated) — `SYS_OBJECT_REJECT`, gated by the same
  `CAP_PROMOTE_OBJECT` capability `SYS_OBJECT_PROMOTE` uses: whatever
  authority decides an object may advance is the same authority that
  decides it may not, one verdict with two outcomes rather than a
  separate grant.
- **`src/include/vajra/actor.h`** (updated) — `MAX_ACTORS` 13→16 and
  `MAX_CAPS_PER_ACTOR` 8→12 (see "bugs" below).
- **`src/core/main.c`** (updated) — a new sandboxed `actor_inspector()`
  ghost actor: waits for one task, reads the object with only the one
  `CAP_READ_OBJECT` Scanner explicitly delegated it, checks for a
  `"BAD"` marker, replies pass/fail, exits. `actor_scan_object()` is
  Scanner's new per-object routine: spawn an inspector, delegate it
  read access to exactly that object, await its verdict, then either
  reject or promote (×3, through to `OBJ_TRUSTED`) and notify both
  Reader and Downloader. `actor_downloader()` now writes two objects —
  `payload.bin` (benign) and `suspicious.bin` (contains the `"BAD"`
  marker) — and `actor_reader()` now expects a possible denial on its
  second read, not just trusted content.
- **`src/core/main.c`**, `kernel_main()` (updated) — creates
  `suspicious.bin` alongside `payload.bin`; grants Scanner
  `CAP_SPAWN` (to sandbox inspection) and `CAP_SEND` to Downloader (see
  below); banner bumped to Milestone 11.

## Two real bugs, neither obvious from reading the code alone

- **Mailbox message-ordering race.** Scanner's single mailbox carries
  two logically distinct streams: `MSG_DOWNLOAD_DONE` signals from
  Downloader, and verdict replies from whichever inspector it just
  spawned. `actor_receive()` is strict FIFO with no type/sender
  filtering. The first working version had Downloader signal both
  objects back-to-back; a boot test showed the suspicious object never
  scanned at all — Scanner processed object 0 twice instead, because
  Downloader's second `DONE` signal arrived while Scanner was still
  blocked awaiting the first object's inspector verdict, and got
  consumed as that verdict instead. Fixed with an explicit ack:
  Downloader now blocks on `user_receive()` after its first signal,
  and `actor_scan_object()` sends an explicit ack to Downloader (in
  addition to its existing signal to Reader) only once it has fully
  finished an object — so the message Downloader waits for cannot
  exist until Scanner is genuinely done with the previous one, and the
  race message can never be constructed in the first place.
- **`MAX_CAPS_PER_ACTOR` (8) was too small for Scanner's real load.**
  Even after the ordering fix, a second boot test showed the second
  inspector spawning successfully but Scanner unable to message it
  (`user_send` returned -1) — confirmed by temporary debug output
  (`w=0` slot returned, `send=-1`). Root cause: Scanner holds 6
  capabilities from boot-time grants, and `actor_spawn_child()`
  auto-grants the spawner `CAP_SEND`+`CAP_TERMINATE` for every child it
  creates — 2 more per inspector. Nothing reclaims a dead inspector's
  now-useless entries, so by the second inspector Scanner's table was
  already full (8/8) and the second auto-grant silently failed to
  register (`actor_add_cap()`'s documented, deliberate behavior on a
  full table — see `actor_spawn_child()`'s own comment). Fixed by
  raising `MAX_CAPS_PER_ACTOR` to 12, with headroom above the exact
  minimum (10) rather than just meeting it. Capability-table
  reclamation on actor death remains unfixed — a sharper version of
  Milestone 8's still-open "no revocation" follow-up.

## Verified

Both permanently built into the demo itself, not separate temporary
test code — the two-object pipeline naturally exercises both:

- **The legitimate path.** `payload.bin` written, inspected by a
  sandboxed inspector holding only a delegated `CAP_READ_OBJECT` for
  that one object, promoted `OBJ_UNTRUSTED` → `OBJ_QUARANTINED` →
  `OBJ_ANALYZED` → `OBJ_TRUSTED`, and read successfully by Reader.
- **The reject path, kernel-enforced.** `suspicious.bin`'s inspector
  finds the `"BAD"` marker and reports failure; Scanner calls
  `storage_reject()`; Reader's subsequent read attempt — despite
  legitimately holding `CAP_READ_OBJECT` for the object — is refused
  by `storage_read()` itself: `[Reader] object 1: read refused
  (rejected -- capability alone wasn't enough)`. Confirms `OBJ_REJECTED`
  is a second, independent gate, not something the capability check
  alone controls.
- Sandboxing's actual authority shape: the inspector is granted
  *nothing* automatically except `CAP_SEND` back to Scanner (the
  ordinary spawn auto-grant) plus the one object-scoped
  `CAP_READ_OBJECT` Scanner explicitly delegates — it could not write,
  promote, reject, touch the other object, or message anyone else even
  if compromised.
- All previous milestones' demos (preemption, isolation, ring 3,
  mailboxes, capability delegation, ghost-actor spawn/terminate/quota)
  continue running correctly alongside the expanded pipeline.

## Known follow-ups for the next milestone

- **No capability reclamation on actor death**, sharpened by this
  milestone's own capacity bug — a longer-running system would
  eventually exhaust any fixed `MAX_CAPS_PER_ACTOR` no matter how
  large, not just under this demo's specific load.
- **A single inspection stage** (one fixed marker check) — multiple
  scanner stages in series (file-type, hash/reputation, static
  analysis) remain deferred; this milestone was about the authority
  structure sandboxing gives a real scanner, not about writing one.
- **No real ingest path** — `actor_downloader()` still writes a fixed
  string, not bytes from an actual external source. Needs Phase 12
  (networking) or equivalent.
- Console output still isn't preemption-safe (Milestone 4's follow-up,
  unchanged).
