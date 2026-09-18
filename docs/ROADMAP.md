# Vajra Roadmap

See `docs/PHILOSOPHY.md` first — it's the canonical, short statement
of what Vajra is and why; this document is the specific, evolving path
to it, and gets checked against that document, not the other way
around.

**The thesis, restated in one line**: Vajra's own devices — phone,
laptop, whatever else — behave like one connected fabric of
computation, where a program's display and a program's execution don't
have to live on the same machine, and capabilities are what make that
safe to do across devices you don't all trust equally. Phases 12
(networking) and 13 (distributed actors) are that thesis directly;
everything else — SMP, storage, the CLI environment in Phases 16-20 —
exists to make Phase 12/13 possible or usable, not to compete with it
for priority.

## How to read this

Each phase lists **what gets built**, **why it has to come now**
(usually: it's a prerequisite for something later, or it's a gap that
will otherwise cause real bugs), and **which philosophy principle it
serves** (§-numbers below refer to `docs/PHILOSOPHY.md`'s current,
short section list for Phases 16 onward; citations on Phases 1-15,
written before that document's overhaul, are kept exactly as they were
— historical record of what motivated each phase at the time, not
re-numbered to match the new document). Phases are
ordered by hard dependency, not by importance — capabilities are more
central to Vajra's identity than SMP, but you cannot build a capability
system on top of a kernel that has no user/kernel memory separation
yet.

Discipline carried over from the project's own rules (unchanged, just
restated here so it travels with the roadmap — canonical version now
in `docs/PHILOSOPHY.md` §6):

- One architectural step at a time. Don't jump ahead of the current
  phase because a later one seems more interesting.
- Every milestone must actually **boot and be observed** in QEMU, not
  just compile.
- A fix isn't proven until the failure it prevents was deliberately
  triggered, not just the success path demonstrated.
- Prototype shortcuts (e.g. "everything is supervisor-accessible for
  now") must be labeled as shortcuts, not mistaken for the final
  design.
- A checkout at milestone N must build and demonstrate everything
  through N. Nothing silently half-finished.

## Where things actually stand right now

Done and verified booting: VGA console, IDT with 7 handled vectors
(0, 6, 8 with a dedicated IST stack, 13, 14, the timer at 32, and the
syscall gate at 0x80/DPL=3), a bitmap physical page allocator sized
from real E820 data, a preemptive round-robin actor scheduler,
per-actor address spaces (Milestone 5 — each actor's own memory is
genuinely unmapped from every other actor's page tables), actor code
running at genuine CPL 3 with a real syscall boundary (Milestone 6),
bounded per-actor mailboxes with blocking receive and backpressure
(Milestone 7), a kernel-enforced, delegable capability table gating
who may message whom (Milestone 8), capability-checked, quota-limited
actor spawning/termination at runtime — the ghost-actor pattern
(Milestone 9), a capability-addressed object store on a real ATA
disk driver, with trust-state metadata and end-to-end persistence
verified through actual disk I/O, not in-memory state (Milestone 10),
a full quarantine/security pipeline — sandboxed, narrowly-capable
inspector actors doing the actual risky inspection, and a genuine
reject path (a new terminal `OBJ_REJECTED` trust state) that the
kernel enforces against a reader even when it legitimately holds the
read capability (Milestone 11), and — for the first time, more than
one physical core running at all — a genuinely woken, independently
executing second CPU core (a real INIT-SIPI-SIPI bring-up sequence,
not a simulation), proven to run in true parallel with the BSP, though
not yet participating in the actor scheduler at all (Milestone 12),
and — the actual thesis (`docs/PHILOSOPHY.md` §1), pulled ahead of
Phase 10's remainder and Phase 11 by explicit direction — a real
virtio-net-pci driver found via a from-scratch PCI scanner (Milestone
13), with a capability-gated actor-level transport on top of it
(`SYS_NET_SEND`/`SYS_NET_RECEIVE`, `CAP_NET`) proven by an actual
ring-3 actor, not just `kernel_main`, crossing a device boundary
(Milestone 14) — verified within a single instance (capability
enforcement, graceful timeout, the underlying HAL round trip all
confirmed), and since verified for real across two genuinely separate
QEMU instances too, via GitHub Actions CI (blocked for a while on this
project's own Windows dev machine by QEMU's `-netdev socket` backend
itself crashing there — a tooling issue external to Vajra — and then,
once moved to CI, by two more unrelated bugs: a shared-disk-image lock
conflict, and the kernel never actually writing to COM1 in shipped
code, which made every CI serial log look like a boot failure when the
kernel was in fact running perfectly; see
`docs/MILESTONE14_CHANGELOG.md`'s "Verified" section for the full
account, including the actual demo-timing bug that remained once both
of those were fixed). Milestone 15 then closed Phase 12's remaining
scope — addressing, remote actor identity, and a real ACK-and-retry
reliability primitive, all likewise verified across two separate
instances, not just compiled (`docs/MILESTONE15_CHANGELOG.md`).
Isolation, the privilege boundary, mailbox backpressure, both
capability soundness properties (can't use authority you lack; can't
delegate authority you lack either), both spawn/terminate guards (no
capability; quota exceeded), all three object-capability checks
(read/write/promote), the reject-then-read-denied path, a genuine
cross-core data race (unsynchronized console output, Milestone 12), a
genuine round-trip network packet (Milestone 13), and a capability
page-fault caught only once an actor (not kernel_main) exercised the
network path (Milestone 14) were each verified by deliberately
triggering the failure or exercising the real hardware path, not just
assumed from the design. Actor memory capped to a fixed 1MB-2MB window
(Phase 3's own note), no capability revocation or reclamation yet
(Milestone 8's own follow-up, sharpened by Milestone 11's
capability-table-exhaustion bug), spawning limited to pre-linked
`.user_text` workers (no loader yet), no on-disk object catalog,
runtime object creation, or multi-stage scanning yet (Milestone
10/11's own follow-ups), the actor scheduler itself is still
single-core/BSP-only despite the second core now existing (Milestone
12's own follow-up — see Phase 10), networking has no addressing
scheme, no remote actor identity, and no reliability yet (Milestone
14's own follow-ups), x86-64 only, and the syscall surface is
write/yield/exit/send/receive/grant/spawn/terminate/object-read/
object-write/object-promote/object-reject/net-send/net-receive — no
pointer validation on syscall arguments.

---

## Phase 1 — Make the current foundation actually solid — DONE

*Nothing new conceptually; closed gaps that would otherwise corrupt
every phase built on top.*

- **Fixed the page-table/allocator mismatch.** boot.asm now identity-
  maps the first 256MB (matching `memory.c`'s `MEM_CAP`) instead of
  just the first 2MB, so `alloc_page()` can never hand out a page that
  isn't actually mapped.
- **Guard pages under actor stacks**, allocated as a physically
  contiguous guard+stack pair (`alloc_pages_contig()`) and unmapped at
  the page-table level (`hal_unmap_page()`, `src/hal/x86_64/paging.c`)
  so a stack overflow raises a real #PF instead of silently corrupting
  whatever sits below it.
- **Dedicated double-fault stack (IST1)**, via a kernel-owned GDT/TSS
  (`src/hal/x86_64/gdt.c`) — both proven approaches taken from
  `legacy-asm`'s V0.33.
- **`free_page()`** plus a reap step in the scheduler that reclaims a
  dead actor's stack once nothing could still be running on it.

Two bugs turned up only once this was actually booted and observed
(not just compiled), both fixed as part of this phase rather than
deferred:

- Splitting a huge page to carve out a guard page was allocating its
  new page table from the same general-purpose pool actor stacks come
  from, so it could — and did — land on the exact page a later actor
  needed as its own guard page. Fixed by giving page-table splits a
  separate, fixed static pool (`pt_pool` in `paging.c`) that can never
  compete with actor memory for placement.
- That same static pool, at its first (larger) size, pushed the
  kernel's `.bss` out far enough to collide with the page tables
  themselves at their old address (`0x8000`–`0xB000`) — start.asm's
  `.bss`-zeroing loop was overwriting the live page tables out from
  under CR3. Fixed by relocating the page tables to `0x90000`+, above
  the boot stack instead of immediately after the kernel image, so
  kernel/`.bss` growth and page-table placement can no longer collide
  short of the kernel approaching half a megabyte.
- A latent scheduler bug, present since M3 but never triggered there
  (the original 3-actor demo always finishes all three in lockstep):
  `actor_yield()` broke if called while the yielding actor was the
  *only* runnable one, because `schedule_next()` would read its own
  stale, long-since-overwritten suspend point instead of recognizing
  there was nothing to switch to. Fixed by making that case a no-op.

*Philosophy: §32 (don't mistake a prototype compromise for final
design), §14 (fault containment starts with not corrupting memory).*

## Phase 2 — Preemptive scheduling — DONE (Milestone 4)

- PIT timer (100Hz) + remapped 8259 PIC, IRQ0 dispatched straight to
  `actor_yield()` from `exception_handler`. Reuses `hal_context_switch()`
  unchanged, exactly as intended when it was written in M3.
- `hal_enable_interrupts()` is finally called for real (`main.c`, right
  before `scheduler_start()`).
- A misbehaving actor that never yields can no longer stall the
  system — M3's explicitly named follow-up. Deliberately proven, not
  just assumed: `actor_greedy` (`main.c`) never calls `actor_yield()`
  at all, and actors 1-3 still fully interleave and finish all their
  ticks *during* its busy-loop — see `docs/MILESTONE4_CHANGELOG.md`.
- Turned out to need more than just wiring up a timer: preemption
  means `schedule_next()`'s state mutations can happen at any point
  actor code is running, not just at explicit yield/exit calls, so
  every entry point touching `actors[]`/`current_actor` now disables
  interrupts for its critical section. A new `actor_trampoline()`
  handles the one case that doesn't resume through the normal path (a
  freshly spawned actor's fake frame `ret`s straight into application
  code) by re-enabling interrupts there explicitly.
- Known follow-up surfaced by testing, not yet fixed: console output
  has no locking, so two actors preempted mid-`hal_console_write()`
  can interleave characters. A scheduling bug this is not — actor
  state stayed correct throughout — but worth a lock before console
  output is relied on for more than demos.

*Philosophy: §12 (parallelism as default), §26 (resilience — one
actor hanging shouldn't hang the OS).*

## Phase 3 — Virtual memory & per-actor address spaces — DONE (Milestone 5)

This is the load-bearing phase: almost nothing about real isolation
is possible before it.

- **Per-actor CR3** — each actor gets its own address space
  (`hal_address_space_create()`, `src/hal/x86_64/paging.c`) instead of
  sharing one flat map, matching what `legacy-asm` already reached
  (`actor_cr3` table). `hal_context_switch()` switches CR3 and RSP in
  the same instruction sequence so they can never disagree.
- Kernel commons marked supervisor-only; each actor's own memory
  marked user-accessible within that actor's own mapping only (not yet
  enforced — that needs ring 3, Phase 4 — but already the correct bit
  to carry). This is where "all memory is temporarily accessible"
  (the prototype state through Milestone 4) gets replaced by real
  invariant list items 1–3 in philosophy §33 — genuinely, not just
  nominally: verified by making one actor deliberately write to
  another's known private stack address and confirming it faults
  (`CR2` matching exactly), not merely reading the code and assuming
  it would.
- Turned out to obsolete Milestone 4's guard-page mechanism entirely —
  once every other address in an actor's private window is unmapped,
  not just the one page below its stack, a dedicated guard page is
  redundant — and to break the physical allocator's free list, which
  had been storing its "next free page" pointer inside the freed page
  itself; freeing a page from a *different* actor's now-restricted
  address space can no longer write there. See
  `docs/MILESTONE5_CHANGELOG.md`.
- Real page-table management stayed narrowly scoped rather than fully
  general: `hal_address_space_create()` only supports actor memory
  within a fixed 1MB-2MB window, matching what the physical allocator
  and this actor count actually need today. A general map/unmap
  primitive across arbitrary ranges is deferred until something
  actually needs one.

*Philosophy: §5 (true isolation), §33 invariants 1–3.*

## Phase 4 — Ring 3 and the syscall boundary — DONE (Milestone 6)

- Actors now execute at genuine CPL 3, not ring 0. The only way back
  into the kernel is `int 0x80` (vector 0x80, DPL=3) — every other
  kernel function is unreachable from ring 3 by construction, living
  in supervisor-only memory ring-3 code cannot even fetch instructions
  from. `SYS_WRITE`/`SYS_YIELD`/`SYS_EXIT` are the whole syscall
  surface so far.
- TSS.RSP0 set up for ring 3→0 stack switching — turned out to need
  one dedicated kernel stack *per actor*, not one shared stack:
  multiple actors can be suspended mid-syscall simultaneously (the
  same reason schedule_next() already had to be interrupt-safe in
  Phase 2), and every privilege-changing transition starts fresh at
  RSP0 rather than continuing a previous one. See
  `docs/MILESTONE6_CHANGELOG.md`.
- This is where "kernel small and authoritative, policy outside it"
  (§31) stops being aspirational: before this, everything ran at CPL 0
  already, so there was no boundary to enforce, only a convention that
  nothing happened to violate. Verified, not assumed: deliberately
  executing `hlt` (a privileged instruction) from ring-3 actor code
  raised `#GP` immediately — the CPU refuses it regardless of what the
  code asks for.
- A genuine page-table hierarchy bug (leaf-level permissions overridden
  by non-leaf entries lacking the same bit) was only found by booting
  and observing a reproducible ring-3 `#PF` on the very first
  instruction fetch, not by reading the code — worth remembering
  before trusting a paging change on inspection alone in later phases.

*Philosophy: §31 (small authoritative kernel), §33 invariant 1.*

## Phase 5 — Message passing (IPC) — DONE (Milestone 7)

Now that actors have real address-space boundaries, give them a way
to talk without touching each other's memory.

- Per-actor mailbox: bounded queue (capacity 8), kernel-owned.
- `actor_send()`/`actor_receive()` and matching `SYS_SEND`/
  `SYS_RECEIVE` syscalls. `struct message` carries type, a kernel-
  filled (never sender-supplied) sender slot, and one inline data
  word — the full header from message-passing §5 (correlation ID,
  capability/context, priority, deadline) is explicitly future work,
  not yet needed by anything that exists.
- Mailbox-driven scheduling: `ACTOR_BLOCKED` on empty mailbox, woken
  to `ACTOR_READY` on message arrival — reuses `schedule_next()`
  unchanged, the same way Phase 2's preemption reused
  `hal_context_switch()` unchanged.
- Bounded mailboxes with a rejection/backpressure path from day one,
  verified by deliberately overflowing one rather than assumed from
  the code: sends past capacity are rejected outright, never silently
  dropped or blocking the sender.
- Large-payload-via-reference (message-passing §12/§17) explicitly
  deferred: there's no object/capability system yet for a reference to
  refer into, so building one now would mean designing it twice. Noted
  as a Phase 6 dependency instead.
- A recurring bug class caught a second time: growing `MAX_ACTORS`
  (4→6) grew `.bss` enough to swallow the E820 map at its old fixed
  address (`0x20000`), silently truncating detected RAM to a 16MB
  fallback — the same failure mode, and the same fix (relocate past
  the growth), as Phase 1's page-table collision. See
  `docs/MILESTONE7_CHANGELOG.md` for the general lesson.

*Philosophy: message-passing §1, §5, §8, §9, §16, §17; core §3.*

## Phase 6 — Capabilities — DONE (Milestone 8)

- A per-actor capability table, kernel-enforced. A capability names an
  operation + target (`CAP_SEND` + an actor slot is the only pair that
  exists so far — no object/storage system yet for any other operation
  to apply to), not just an ID — actor identity and authority stay
  explicitly distinct (message-passing §4).
- `actor_send()` from Phase 5 is now capability-checked: knowing an
  actor's slot index stops being sufficient to message it. Verified,
  not assumed: an actor holding no capabilities was deliberately made
  to attempt a send anyway, and denied.
- Capability derivation/delegation: `actor_delegate()` lets an actor
  pass a copy of a capability it holds to another actor, and — also
  deliberately verified — cannot pass along authority it doesn't
  itself hold. Revocation not yet implemented; noted as a Milestone 8
  follow-up rather than skipped silently.
- This is the point where philosophy §33's invariants 4–5 (no forging
  capabilities, kernel-mediated IPC) become real rather than
  aspirational, since Phase 4 already put a syscall gate in place to
  enforce them at.

*Philosophy: §4 (capability-based security), §33 invariants 4–5.*

## Phase 7 — Ghost actors & actor lifecycle — DONE (Milestone 9)

- `SYS_SPAWN`/`SYS_TERMINATE` as capability-checked
  (`CAP_SPAWN`/`CAP_TERMINATE`), quota-limited operations
  (`MAX_SPAWNS_PER_ACTOR`) — §26 of the message-passing section: not
  every actor should be able to spawn or kill arbitrarily, verified by
  deliberately exceeding the quota and attempting both syscalls
  without the capability, not just assumed enforced.
- Short-lived worker actors created with a narrow, explicit
  capability set (exactly what spawning auto-grants: `CAP_SEND` back
  to the spawner, nothing else), doing one task, then destroyed --
  either by exiting themselves after replying, or forcibly by their
  spawner — the concrete "ghost actor" pattern from philosophy §6.
- Surfaced two real bugs neither obvious from reading the code alone:
  a capability-leak-across-reused-slots bug (a fresh actor could have
  silently inherited a dead predecessor's authority) caught by re-
  reading the spawn path, and an address-space assumption in
  `alloc_page()`'s page-zeroing step that only broke once spawning
  actually happened at runtime instead of once, at boot, under a CR3
  that mapped everything. See `docs/MILESTONE9_CHANGELOG.md`.
- Genuinely usable for the first time here: e.g. a file-parsing or
  hashing worker spawned per request instead of living inside a
  monolithic service — demonstrated with a trivial compute-and-reply
  worker, since there's no file/object system yet for a more realistic
  task.

*Philosophy: §6 (ghost actors), message-passing §27.*

## Phase 8 — Storage: objects, not a raw filesystem — DONE (Milestone 10)

- A block device driver behind the HAL boundary
  (`hal/x86_64/ata.c` — ATA PIO chosen over virtio-blk for this
  milestone specifically: no PCI enumeration or virtqueue negotiation
  needed), exposed as a portable, already-generically-shaped interface
  (`hal_disk_read()`/`hal_disk_write()` — no separate translation step
  the way E820 needed one, since sector numbers and byte buffers carry
  no x86-specific format to begin with).
- An **object store** (`core/storage.c`), not a POSIX-style path
  filesystem: actors hold capabilities to specific objects
  (`CAP_READ_OBJECT`/`CAP_WRITE_OBJECT`/`CAP_PROMOTE_OBJECT` naming an
  object id), never blanket filesystem access. This is a deliberate
  architectural choice, not a missing feature — see philosophy §9.
  Verified, not assumed: an actor holding none of the three attempted
  all three against an object it knew the id of, and was denied every
  time.
- Storage states as first-class metadata: `OBJ_UNTRUSTED` →
  `OBJ_QUARANTINED` → `OBJ_ANALYZED` → `OBJ_TRUSTED` (§9). Writing an
  object always resets it to `OBJ_UNTRUSTED`, unconditionally — new
  content invalidates any prior trust decision (§7).
- A jump-table-in-`.rodata` failure mode for `.user_text` code, not
  anticipated when Milestone 6 established "only code needs moving,
  data referenced from it doesn't" — a `switch` statement is a case
  where ring-3 code itself reads compiler-synthesized data directly,
  with no kernel mediation, unlike a syscall argument pointer. See
  `docs/MILESTONE10_CHANGELOG.md`.

*Philosophy: §9 (trusted storage), §33 invariant 6.*

## Phase 9 — The quarantine/security pipeline — DONE (Milestone 11)

- **A reject path, not just promote.** `storage_reject()`
  (`core/storage.c`) marks an object `OBJ_REJECTED` — a new terminal
  trust state distinct from `OBJ_TRUSTED`, gated by the same
  `CAP_PROMOTE_OBJECT` capability via a new `SYS_OBJECT_REJECT`
  syscall (one verdict, two outcomes, not a separate grant). Once
  rejected, `storage_read()` refuses the object outright — enforced by
  the kernel, not convention, and verified against a reader that
  legitimately holds `CAP_READ_OBJECT` for the object: the capability
  alone is not enough.
- **Sandboxed execution.** Scanner no longer inspects content inline;
  it spawns a short-lived `actor_inspector()` ghost actor per object
  (Phase 7's primitive) and delegates it *only* a `CAP_READ_OBJECT` for
  that one object — nothing else, not even automatically inherited.
  A compromised inspector could not write, promote, reject, touch any
  other object, or message anyone but Scanner (§33 invariant 8, by
  construction rather than convention).
- Two objects now flow through the demo pipeline end to end:
  `payload.bin` (benign, promoted `OBJ_UNTRUSTED` →
  `OBJ_QUARANTINED` → `OBJ_ANALYZED` → `OBJ_TRUSTED`, read
  successfully) and `suspicious.bin` (contains a `"BAD"` marker the
  inspector flags, rejected, and denied to Reader on the second read
  attempt).
- Two genuine bugs surfaced only by booting and observing, neither
  obvious from reading the code:
  - A mailbox message-ordering race: Scanner's single mailbox carries
    both `MSG_DOWNLOAD_DONE` signals from Downloader and verdict
    replies from whichever inspector it just spawned, and
    `actor_receive()` is strict FIFO with no type/sender filtering. A
    second `DONE` signal arriving while Scanner was still waiting on
    the first object's verdict was silently consumed as that verdict —
    the suspicious object was never scanned at all. Fixed with an
    explicit ack: Downloader now blocks until Scanner has *fully*
    finished one object (inspector spawned, delegated, verdict
    received, promoted/rejected) before writing and signaling the
    next, so the out-of-order message can no longer exist in the first
    place.
  - `MAX_CAPS_PER_ACTOR` (8) was too small for Scanner's real load: 6
    capabilities granted at boot plus `CAP_SEND`+`CAP_TERMINATE`
    auto-granted per spawned inspector (Phase 7's own auto-grant), and
    nothing reclaims a dead inspector's now-useless entries — so by
    the second inspector, Scanner's table was full and the auto-grant
    silently failed to register, leaving Scanner unable to message an
    inspector it had just successfully spawned. Fixed by raising the
    cap table to 12 (with headroom, not just the exact minimum) —
    capability-table reclamation on actor death is a real follow-up,
    not fixed here.
- Still deferred, as scoped: multiple scanner stages in series (one
  trivial marker check remains sufficient to demonstrate the authority
  structure), and a real ingest path — Phase 12 (networking) territory.

*Philosophy: §7, §8, §33 invariant 6–8.*

## Phase 10 — SMP (multicore) — PARTIALLY DONE (Milestone 12: bring-up)

Split deliberately, the same way Phase 3 (address spaces) shipped
before Phase 4 (ring 3): this milestone did the hardest, most novel,
most bug-prone piece — waking a second physical core at all and
proving it's genuinely running in parallel — without also redesigning
the actor scheduler for concurrent multi-core access in the same
milestone.

**Its remainder is still open, not done** — see "Still not done" below
— and Phase 12 was deliberately pulled ahead of finishing it (this
roadmap's own dependency-not-importance rule, applied literally: none
of Phase 12's networking work needs a generalized per-core scheduler).

- **AP bring-up trampoline** (`hal/x86_64/ap_trampoline.asm`), woken
  via a real INIT-SIPI-SIPI sequence through a new local APIC driver
  (`hal/x86_64/apic.c`) — drawing on `legacy-asm/kernel/kernel.asm`'s
  own proven `setup_smp` for the exact IPI magic values, though the C
  rewrite's trampoline is considerably more complete (a full 16-bit →
  32-bit → 64-bit transition into real, linked C code via a mailbox
  hand-off, not just an increment-and-halt stub).
- **Genuine parallelism, verified, not assumed**: the AP's own
  30-million-iteration counting loop consistently finishes and prints
  its result in the MIDDLE of the BSP's entire actor demo trace, never
  before or after it — only possible if both cores are actually
  executing at the same wall-clock time on independent hardware.
- **A real bug, deliberately triggered then fixed**: this is the first
  time in the C rewrite two cores can race on shared mutable state at
  all. `hal/x86_64/console.c`'s cursor state had been unlocked (a known
  Milestone-4 follow-up, harmless on one core) since Milestone 4;
  booted without a fix first, the AP's and BSP's console output came
  back genuinely torn together mid-word, not just interleaved lines.
  Fixed with this kernel's first cross-core synchronization primitive
  (`include/vajra/spinlock.h`, a `lock cmpxchg` spinlock), verified by
  reconstructing both original messages byte-for-byte from the (still
  visually interleaved by design — see the changelog) log.
- **Still not done**: per-core scheduler run queues / per-core actor
  state. `core/actor.c`'s scheduler remains entirely BSP-only — the AP
  never spawns, runs, or touches a single actor yet. Message passing
  (Phase 5) already gives the right abstraction for cross-core
  communication once this happens; that part of the phase's own
  original framing still holds. Needs: a per-core `current_actor`, the
  new spinlock applied to `actors[]` (currently `cli`-only, insufficient
  across cores), the AP's own local-APIC preemption timer (PIT/8259
  IRQ0 only ever reaches the BSP), and extending
  `hal_address_space_create()` so actor-context code can reach the
  LAPIC. Also still fixed at exactly one AP — discovering N cores needs
  an ACPI MADT walk, not hardcoded bring-up.

*Philosophy: §12 (parallel computing as first-class).*

## Phase 11 — AArch64 port — NOT STARTED

**Deliberately skipped ahead of, not forgotten.** Phase 12 (below) was
pulled forward of both this phase and Phase 10's own remainder by
explicit direction, once it became clear neither is a hard dependency
of networking — see Phase 12's own dependency note. Still real,
still needed eventually: the HAL boundary hasn't actually been tested
against a second architecture yet, so every "HAL/core separation stays
real" claim up to Milestone 13 is unverified in the one way that would
actually prove it.

- This is the actual test of whether the HAL boundary (`docs/`
  addendum A2/A3) was drawn correctly: implement
  `hal/aarch64/{start,context_switch,interrupts,paging,...}` and
  `boards/qemu-virt-arm/`, and `core/` — scheduler, actors,
  capabilities, memory policy, IPC — should compile and run
  **unchanged**.
- Any change required in `core/` to make this port work is itself a
  bug in the phases above (a HAL leak), not a normal part of porting.

*Philosophy: addendum A1–A3 — the entire reason for the C rewrite.*

## Phase 12 — Networking as part of the actor fabric — FLAGSHIP, DONE (Milestones 13-15)

**Built ahead of Phase 10's remainder and Phase 11 by explicit
direction** (see both phases' own notes above) — the roadmap's own
"ordered by hard dependency, not by importance" rule applies literally
here: neither blocks a network driver or raw frame I/O, and this is
the actual thesis (`docs/PHILOSOPHY.md` §1). Phase 10's remainder and
Phase 11 remain real, open, not-yet-done work — this is a deliberate
reordering, not a silent drop.

**This and Phase 13 are the actual thesis of the project — see
`docs/PHILOSOPHY.md` §1 and §4. Everything else on this roadmap,
including the CLI environment in Phases 16-20, is in service of these
two, not a parallel goal of equal weight.**

- **DONE (Milestone 13): a network driver (virtio-net-pci under QEMU)
  behind the HAL boundary** (`hal/x86_64/pci.c`, `virtio_net.c`) —
  raw Ethernet frame TX/RX only, verified with a genuine ARP round
  trip against QEMU's own gateway, not a loopback. No IP/UDP/TCP, no
  capability gating, not reachable from actor code yet — see
  `docs/MILESTONE13_CHANGELOG.md`. Also surfaced and fixed a real
  low-memory collision (kernel `.bss` growth silently swallowing the
  boot stack and Milestone 12's AP trampoline addresses) — the same
  recurring bug class as this project's earlier page-table/E820-map
  relocations, one structure later.
- **DONE (Milestone 14): a capability-gated actor-level transport** —
  `SYS_NET_SEND`/`SYS_NET_RECEIVE` (`CAP_NET`), a minimal from-scratch
  wire protocol (`core/net.c`, EtherType `0x88B5`), and
  `actor_network_peer()`: the same, unmodified actor running on every
  instance, broadcasting a HELLO and replying to one it hears — the
  first milestone where an ACTOR, not `kernel_main`, crosses a device
  boundary. See `docs/MILESTONE14_CHANGELOG.md`. Also found and fixed
  a real bug: the driver's DMA buffers were allocated from the
  actor-private 1MB-2MB window (fine when only `kernel_main` touched
  them under the boot CR3 in Milestone 13, a genuine page fault the
  instant a real actor's own restricted CR3 called in) — fixed with a
  new `alloc_dma_pages()` allocator drawing from the 2MB+ commons
  region instead.
- **DONE: verified across two genuinely separate QEMU instances**, not
  just within one — capability enforcement, graceful timeout behavior,
  the underlying HAL round trip, and now a real cross-device
  HELLO/HELLO_ACK exchange are all confirmed correct, via
  `.github/workflows/network-test.yml` on GitHub Actions. Getting here
  meant finding and fixing three unrelated problems in sequence: QEMU's
  `-netdev socket` backend itself hard-crashing on this project's
  Windows dev machine (confirmed via Windows Event Viewer — a tooling
  issue external to Vajra, the reason this moved to CI at all); a
  shared-disk-image lock conflict in the CI workflow's first version;
  and, once both of those were out of the way, the kernel never
  actually writing to COM1 in shipped code — which made every CI run's
  serial log look like a boot failure when `-d int,cpu_reset` proved
  the kernel was running flawlessly the entire time. The genuine bug
  that remained after all three — `actor_network_peer`'s listening
  window measured in busy-spin iterations rather than real time, too
  short to overlap the two instances' actual start-time skew — is now
  fixed too. See the changelog's own full account.
- **DONE (Milestone 15): addressing, remote actor identity, and
  reliability** — `net_send_message_to()`/`SYS_NET_SEND_TO` unicasts
  to a specific device's MAC instead of broadcasting; every received
  message now carries the sending actor's own local slot
  (kernel-stamped, never actor-supplied) plus the Ethernet header's
  own source MAC, so origin is "actor N on device MAC," not just "some
  peer"; `net_send_message_reliable_to()`/`SYS_NET_SEND_RELIABLE`
  retransmits until a real ACK comes back from that exact device or
  gives up, with every received DATA frame auto-ACKed transparently on
  the receiving end. All three verified for real across two separate
  QEMU instances on CI, not just compiled — see
  `docs/MILESTONE15_CHANGELOG.md`.
- **Still open**: still no way to name a specific remote ACTOR, only a
  device (a reply means "some actor on device Y," not an address the
  fabric can route to directly) — that's Phase 13's remote capability
  delegation story. Authenticated channels between Vajra instances
  remain the next point cryptographic identity becomes necessary
  rather than deferred.
- The concrete near-term deliverable this unlocks: a **remote-display
  / follow-me session** (`docs/PHILOSOPHY.md` §4) — a display actor on
  whatever device you're looking at subscribes to a stream of
  frame-update messages from wherever a program is actually executing.
  Execution doesn't move; only the display and input do. This is the
  realistic, buildable version of "the device is just a display."

*Philosophy: §1, §4 (this document's own).*

## Phase 13 — Distributed actors & the personal fabric — FLAGSHIP

- Remote capability delegation that cannot exceed local authority
  (`docs/PHILOSOPHY.md` §3 invariant 4: crossing a device boundary can
  only preserve or narrow authority, never widen it).
- **True actor migration** (`docs/PHILOSOPHY.md` §4): the actor
  itself — memory, capability table, identity — moves to a new device
  and keeps executing there natively, not just its display. Genuinely
  hard, and the invariant above is what makes it safe to attempt at
  all: the receiving device must never end up with more authority than
  the migrated actor actually needs there.
- Multi-device pairing under one user identity — the "personal fabric"
  §1 describes, not a login/account system borrowed from elsewhere.
- Speculative/redundant execution and checkpointing belong here or
  later — long-term, not urgent.

*Philosophy: §1, §3 invariant 4, §4 (this document's own) — the actual
reason this project exists.*

## Phase 14 — Adaptive scheduling

- Only tractable once there's real telemetry to feed it: mailbox
  queue lengths and message rates (Phase 5), per-core load (Phase
  10), and locality data (Phase 12–13).
- Start with heuristics (§13 is explicit: hill-climbing and
  queueing-theory style rules before any ML), using message
  telemetry to decide actor placement — message-passing §24 ties this
  directly to the IPC layer built in Phase 5.

*Philosophy: §13, message-passing §24–25.*

## Phase 15 — Heterogeneous compute (GPU/NPU) & AI assistance

- GPU/NPU work dispatched as messages to device-owning actors (§24),
  not a bolted-on separate subsystem — a `COMPUTE_REQUEST` message
  naming an operation + capability-authorized object references
  (message-passing §11) is the same shape whether it runs on CPU,
  GPU, or NPU.
- AI assistance (diagnostics, placement suggestions, security
  analysis) plugs in as an actor that *proposes* through the same
  capability-mediated interfaces everything else uses — never a
  bypass of kernel security (§20, §33 invariant 9).

*Philosophy: §20, §21, §24.*

---

## Phases 16-20 — CLI-OS parity (supporting, not the thesis)

**These phases exist to make Phase 12/13 (the actual flagship) usable
day-to-day, and to make Vajra livable while that work happens — they
are not a competing goal of equal weight.** The aim is that Vajra can
genuinely be *used* for real tasks from a text console — open a file,
run a program, install new software — with capabilities no less
complete than any major OS's command-line environment. No GUI, no
windowing, no desktop shell: text-mode only (`docs/PHILOSOPHY.md` §5).

Numbered to continue the sequence without disturbing the existing
Milestone changelogs' references to Phases 1-15, but their DEPENDENCY
relationship to 11-15 is genuinely loose: Phase 16 only needs Phases
1-9 (actors, capabilities, storage) plus new HAL drivers, and could
run before Phase 11 (AArch64) or Phase 14-15 without penalty. Phase 20
needs Phase 12 (networking) for the "download a package" path
specifically, and Phase 21 needs both Phase 16 and Phase 12. Phase 22
is fully optional and depends only on Phase 16. Treat 16-22's
numbering as sequencing among themselves, not as lower priority than
11/14/15 — see this roadmap's own "ordered by hard dependency, not by
importance" note at the top. Priority-wise, all of 16-22 sit strictly
below Phase 12/13 (the flagship — see this section's own opening note).

### Phase 16 — A real program loader & minimal userland runtime — DONE (Milestone 16)

The single biggest gap standing between what exists today and
"install and run new software": every actor right now is a
pre-linked C function baked into the kernel image at build time —
`SYS_SPAWN` takes a function pointer to something the kernel already
knows about, not a path to a program. There is no loader, no binary
format, no relocation, no concept of a program that didn't exist when
the kernel was compiled.

- **DONE: a loadable executable format** (`include/vajra/loader.h`) —
  a small custom header (magic + entry offset + code size), not full
  ELF, matching the same "more machinery than this kernel needs yet"
  reasoning `link.ld` already applies to `kernel.bin` itself.
  `SYS_SPAWN_PROGRAM`/`core/loader.c` reads a storage object, validates
  it, and instantiates it into a FRESH actor address space at spawn
  time.
- **DONE: a second, separate per-actor memory window** for loaded
  program code+data (`hal/x86_64/paging.c`, `PROGRAM_VBASE` at 256MB),
  additive to Phase 3's existing 1MB-2MB stack window rather than an
  enlargement of it — lower risk, and every actor that predates this
  milestone keeps working exactly as it did.
- **DONE: a minimal userland runtime** (`src/userland/runtime.c`) —
  wraps the syscall boundary into a small, documented ABI a genuinely
  separate program links against, replacing the ad hoc
  `user_write()`-style wrappers hand-written per demo actor.
- **DONE: verified by loading and running a genuinely
  separately-compiled "hello world"** (`src/userland/hello.c`) — its
  own link (`program.ld`), never part of `kernel.bin`'s own C sources,
  embedded as opaque bytes (the same `incbin` technique the AP
  trampoline already uses) and loaded through the real
  `SYS_SPAWN_PROGRAM` syscall path by a new demo actor, not simulated
  by extending the existing one. Its own `user_write()` call reaching
  the console is the proof, on both local Windows QEMU and Linux CI.
- Four real bugs found getting a clean boot, all the same "kernel
  `.bss` growth silently swallowing fixed low-memory structures" class
  this project has hit before (Milestone 13's own changelog), or the
  actor-private-window-vs-commons class from Milestone 14's — see
  `docs/MILESTONE16_CHANGELOG.md` for the full account.

### Phase 17 — A filesystem namespace over the object store

Phase 8's object store stays exactly what it is — capability-addressed,
not path-addressed — this phase adds a naming layer ON TOP, not a
replacement. A path is how a human or a shell finds an object; a
capability is still what's required to touch its contents once found.
Conflating the two would quietly undo Phase 8's whole point.

- A directory/naming service (itself likely an actor) resolving
  human-readable paths to object ids, with its own persistent metadata
  on the same ATA disk.
- Create/list/rename/delete operations on names, distinct from (and
  layered above) Phase 8's `CAP_READ_OBJECT`/`CAP_WRITE_OBJECT`/
  `CAP_PROMOTE_OBJECT` on the objects those names point at.
- **Resolved design constraint**: a path lookup returns an object id
  and nothing else — never a capability. An id is public knowledge
  (like a phone book entry); the capability to act on what it points
  at is still a separate, explicit grant (`actor_delegate()`, unchanged
  from Phase 6). A namespace lookup that handed back usable access
  would forge authority from knowledge, which
  `docs/PHILOSOPHY.md` §3 invariant 3 rules out directly.

### Phase 18 — Input devices & an interactive shell

- A keyboard driver (PS/2, matching QEMU's default machine) behind the
  HAL boundary — the first HAL input device this kernel has ever had;
  everything so far has been output-only (console) or block-storage.
- A real-time clock driver, needed for anything resembling a
  `ls -l`-style timestamp.
- A shell actor: line editing, command parsing, environment variables,
  a working directory (Phase 17), argument passing into a loaded
  program (Phase 16).
- Pipes and I/O redirection between programs, built as a genuinely
  natural extension of the existing bounded-mailbox message passing
  (Phase 5) — a pipe is just another mailbox with a different actor on
  each end, not a new mechanism.
- Job control: running a program in the foreground vs. background.
- **Resolved design constraint**: "interrupt a running program" is two
  distinct tiers, both already native to the model, not a new async
  signal mechanism. A graceful request ("please stop") is an ordinary
  message the target actor checks at its own safe points and may
  ignore — ordinary mailbox delivery (Phase 5), nothing new. A hard
  stop is the shell's existing capability-mediated `SYS_TERMINATE`
  (Phase 7). No forced control-flow injection into another actor is
  ever added — that would be a new kind of non-consensual cross-actor
  interference the model has never had.

### Phase 19 — A standard utility set

The classic minimal CLI toolkit, each one a genuinely separate loaded
program (Phase 16) — this is what actually proves Phase 16 works for
real software, not just one hand-built "hello world": `ls`, `cat`,
`cp`, `mv`, `rm`, `echo`, a basic text search, a basic text editor,
`ps` and `kill`.

- `ps`/`kill` need the shell to be able to enumerate and act on live
  actors by name/id at all — a capability-gated introspection syscall
  that doesn't exist yet, not just a UI over `SYS_TERMINATE`.
- **Resolved design constraint**: visibility is a capability
  (`CAP_INTROSPECT(target)`), not ambient. By default `ps` shows only
  an actor's own descendants — the same parent/child relationship
  spawning already auto-grants for `CAP_SEND`/`CAP_TERMINATE` (Phase
  7) — never a global process table. A literal Unix-style "see
  everything" `ps` would be ambient authority
  (`docs/PHILOSOPHY.md` §3 invariant 2) and is deliberately not the
  default; a supervisor actor CAN be granted broader
  `CAP_INTROSPECT` explicitly, the same way Scanner is granted
  `CAP_PROMOTE_OBJECT` in the existing demo.
- Deliberately NOT a POSIX-compatibility layer — these tools speak
  Vajra's own object/capability model directly (an `ls` lists names in
  Phase 17's namespace and their trust state from Phase 8, which a
  literal Unix `ls` has no equivalent of), not a translation shim over
  someone else's semantics.

### Phase 20 — Package installation, using quarantine for real

Installing new software becomes: write a new object, and run it
through Milestone 11's existing quarantine pipeline (untrusted →
sandboxed inspection → promoted or permanently rejected) before it is
ever loadable at all (Phase 16) — a capability-native answer to "can I
trust this program," not a bolted-on afterthought or a signature
check reimplemented from scratch.

- Installing from local disk needs only Phases 16-19.
- Installing from a remote source (the literal "download a package"
  path) additionally needs Phase 12 (networking) — the one place this
  phase range depends on something outside 16-19 itself.
- **Resolved design constraint**: install authority is a dedicated,
  narrowly-scoped `CAP_INSTALL_PACKAGE`, granted only to whatever
  actor legitimately plays "installer" — never a reuse of the existing
  demo's blanket `CAP_PROMOTE_OBJECT` (Milestone 11), which has no
  per-object scoping and was never meant to double as real install
  policy.

*Philosophy: `docs/PHILOSOPHY.md` §3 invariants 3 and 5 (same trust
model Phase 9 established, now the actual install path instead of a
fixed demo object).*

### Phase 21 — A bounded POSIX compatibility shim & a text browser (exploratory)

Not a Vajra-native goal — a deliberately separate, scoped porting
project sitting on top of Phase 16, for the specific case of running
existing open-source software rather than reimplementing it.

- A minimal libc-equivalent translation layer: a ported program's
  `open()`/`read()`/`socket()`/`connect()` calls route internally
  through Vajra's own capability-gated object store and networking
  actor (Phase 12) — the ported code behaves as if it has a normal
  POSIX environment, but every boundary crossing is still a real
  capability check underneath: the shim never grants a ported program
  authority the capability model wouldn't otherwise allow
  (`docs/PHILOSOPHY.md` §5's "compatibility shim, not a kernel goal"
  note).
- A small, portable TLS library (BearSSL/mbedTLS-class, built for
  exactly this kind of constrained port) for HTTPS.
- Target: a text-mode browser (Lynx/w3m-class) — not a rendering
  engine, and explicitly not Dillo/NetSurf/Chromium/Ladybird-class,
  which assume a graphical surface Vajra doesn't have and hasn't been
  asked to build. Fetching pages and files works; modern JS-heavy
  sites do not render as designed, and that's a permanent property of
  this path, not a "not yet."

*Philosophy: §5 (this document's own "not binary-compatible, only
through a bounded shim" note).*

### Phase 22 — An actor-native language (optional)

Not required — once Phase 16 exists, ordinary C via the same
`clang -target x86_64-elf` toolchain that builds the kernel itself
already lets you write programs for Vajra. This phase is a genuine
bonus, only worth doing if it's done right: a language where actors,
`send`, and capabilities are first-class syntax (in the spirit of
Erlang/Pony), so the language itself enforces the model's discipline
instead of a C program having to remember to call the right wrappers.
A generic language that merely targets Vajra's ABI wouldn't be worth
building; one that makes the model impossible to accidentally violate
would be.

*Philosophy: §2 (this document's own model) — a language whose syntax
is a direct expression of it, not a separate concern bolted on.*

---

## Explicit non-goals for the foreseeable future

The permanent statement of these now lives in `docs/PHILOSOPHY.md` §5
— this section is today's specific scope, checked against that
document, not a replacement for it:

- **No GUI, no windowing, no desktop shell** — by explicit direction,
  not by omission. Phases 16-22 target a genuinely usable CLI
  environment with capabilities no less complete than any major OS's
  command-line side; a graphical surface is a deliberately separate,
  not-yet-scoped question, not an implied "eventually."
- **Not a security product** — the capability model exists to make
  Phase 12/13's fabric safe across devices of mixed trust, not as a
  goal competing with the fabric for priority. See
  `docs/PHILOSOPHY.md` §1 and §5.
- Not aiming at mass daily-driver deployment replacing
  Windows/Android/Linux, and not aiming to match their driver
  catalogs or hardware breadth — a scale problem, not a design one.
- Not binary-compatible with anything natively; running existing
  POSIX software is only ever through Phase 21's explicit, bounded
  compatibility shim, never a kernel-level goal.
- Not aiming at defense-grade or safety-certified use — that needs
  organizational certification/formal verification work that is a
  separate effort from kernel architecture.

The honest goal: a genuine, working exploration of the actor/
capability/message-passing model as the foundation for a real,
multi-device personal computing fabric (`docs/PHILOSOPHY.md` §1), with
real engineering discipline, usable day-to-day from a text console,
on modest but real hardware targets.
