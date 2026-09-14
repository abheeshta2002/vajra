# Vajra C Rewrite — Milestone 10: Storage — Capability-Addressed Objects

## What this is

A real block device driver, and a capability-addressed object store on
top of it — not a POSIX-style path filesystem. This is the roadmap's
Phase 8. An actor holds a capability to `payload.bin` specifically
(`CAP_READ_OBJECT`, `CAP_WRITE_OBJECT`, `CAP_PROMOTE_OBJECT` naming an
object id), never blanket "the filesystem" (philosophy §9). Objects
also carry a trust state as first-class metadata
(`OBJ_UNTRUSTED → OBJ_QUARANTINED → OBJ_ANALYZED → OBJ_TRUSTED`), laid
in now even though the real scanning-pipeline milestone is still
ahead — the authority *shape* of that pipeline is what this milestone
delivers, not real malware analysis.

## What's included

- **`src/hal/x86_64/ata.c`** (new) — a minimal synchronous ATA PIO
  driver, primary bus, master drive, 28-bit LBA: `hal_disk_read()`/
  `hal_disk_write()`. Chosen over virtio-blk (the roadmap's other
  named option) for this milestone specifically because it needs no
  PCI enumeration or virtqueue negotiation — the same disk boot.asm
  already reads the kernel image from via BIOS INT 13h, just accessed
  from long mode, where INT 13h no longer exists.
- **`src/include/vajra/storage.h`** / **`src/core/storage.c`** (new) —
  the object store: a fixed table of objects (name, disk location,
  size, trust level), each backed by its own dedicated sectors.
  100% portable, like `core/memory.c`'s relationship to `e820.c` —
  it only calls `hal_disk_read()`/`hal_disk_write()`, plain sector
  numbers and byte buffers, no x86-specific format anywhere. Has no
  idea actors or capabilities exist at all; `storage_write()` always
  resets an object's trust to `OBJ_UNTRUSTED` (new content invalidates
  any prior trust decision, unconditionally — philosophy §7).
- **`src/hal/x86_64/syscall.c`** (updated) — `SYS_OBJECT_READ`/
  `SYS_OBJECT_WRITE`/`SYS_OBJECT_PROMOTE`. The capability check happens
  HERE, not in storage.c, via a new `actor_current_has_cap()`
  (`core/actor.c`) — keeping the object store reusable without
  dragging the actor system in as a dependency, the same separation
  `hal/x86_64/syscall.c` already keeps between "what a syscall is
  allowed to do" and "what the subsystem underneath actually does".
- **`src/include/vajra/actor.h`** (updated) — `CAP_READ_OBJECT`,
  `CAP_WRITE_OBJECT`, `CAP_PROMOTE_OBJECT` (the last one blanket,
  target 0, same convention as `CAP_SPAWN`: no per-object grant scheme
  for "may promote" yet). `MAX_ACTORS` raised 10→13.
- **`src/core/main.c`** (updated) — three new actors playing the roles
  philosophy §7/§8 describes: `actor_downloader` (writes untrusted
  data, holds only `CAP_WRITE_OBJECT`), `actor_scanner` (the "security
  actor" — reads, inspects, and is the only actor holding
  `CAP_PROMOTE_OBJECT`), `actor_reader` (an ordinary consumer, only
  `CAP_READ_OBJECT` — relies on Scanner's decision, cannot override
  it). Sequenced by message passing, not polling: Downloader signals
  Scanner when the write has landed, Scanner signals Reader once
  promotion is done.

## Verified

- Booted and observed real persistence through the driver, not just
  in-memory state: `actor_downloader` writes `"hello from disk!"` via
  `hal_disk_write()`; `actor_reader`, running as a completely separate
  actor and syscall context afterward, reads it back via a fresh
  `hal_disk_read()` (storage.c keeps no in-memory copy of object
  *content*, only metadata) and gets the identical bytes.
- The full trust pipeline: object starts `OBJ_UNTRUSTED` the moment
  it's written, Scanner advances it one level at a time
  (`OBJ_QUARANTINED → OBJ_ANALYZED → OBJ_TRUSTED`), each step printed
  and confirmed before Reader is ever signaled to proceed.
- Capability enforcement on all three object operations, deliberately
  triggered rather than assumed: `actor_intruder`, holding none of
  `CAP_READ_OBJECT`/`CAP_WRITE_OBJECT`/`CAP_PROMOTE_OBJECT` for the
  payload object, attempts all three anyway, knowing the object's id
  perfectly well. All three denied.
- All previous milestones' demos (preemption, isolation, ring 3,
  mailboxes, capability delegation, ghost actors) continue running
  unmodified alongside the new one.

## A genuine new failure mode, only found by booting

`user_write_trust()`'s `switch` statement on the trust level compiled
to a jump table — Clang's standard codegen for a small dense switch,
placing that table (an array of code addresses) in ordinary `.rodata`.
Milestone 6 established that data referenced *from* `.user_text` never
needs to move, because the only code that ever dereferences a pointer
an actor passes to a syscall is the kernel's own handler, running at
CPL 0. A jump table breaks that reasoning: it's read directly by
ring-3 code itself, with no kernel mediation, to pick the switch's
branch target. Reproduced immediately as a ring-3 `#PF` (error code 5)
on an address a few bytes past `__user_text_end`. Fixed by rewriting
it as an if/else chain, which compiles to plain CMP/JE sequences —
data-free, entirely within `.user_text` like the rest of the function.
Worth remembering for any future `.user_text` code: not just string
literals are safe by default — anything the compiler might synthesize
as an implicit data table (jump tables now; a future case might be
something like an FP constant pool) needs the same scrutiny.

## Known follow-ups for the next milestone

- **Synchronous, polling disk I/O.** Every read/write blocks the
  entire system (interrupts already off for the syscall it runs
  inside of) until the hardware responds — negligible under QEMU's
  emulated disk, a real limit on real hardware. An interrupt/DMA-driven
  driver is future work once that matters.
- **No on-disk object catalog.** Object metadata (name → disk
  location) lives in kernel RAM, rebuilt fresh every boot by whatever
  `storage_create_object()` calls `kernel_main` makes — objects don't
  survive a reboot yet. This milestone is about the capability-
  addressed access model, not on-disk format design.
- **No runtime object creation.** `storage_create_object()` is kernel-
  setup-only, like `actor_spawn()`'s own raw primitive was before
  Milestone 9's `actor_spawn_child()`. A `SYS_OBJECT_CREATE` (itself
  presumably capability-gated) is natural future work, now that
  ghost actors already show the pattern for turning a kernel-only
  primitive into a checked syscall.
- **Fixed object size (2KB) and count (8).** Simple, not scalable;
  fine for a demo.
- Console output still isn't preemption-safe (Milestone 4's follow-up,
  unchanged).
