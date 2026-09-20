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

## Phase 10 — SMP (multicore) — DONE (Milestone 12 bring-up, the scheduler follow-up, and up to 16 cores)

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
- **Follow-up (2026-09-20): the second core runs actors.** What was
  listed here as "still not done" is now done, verified by a test you
  can run:
  - A per-core `current_actor` and scheduler context (`core/actor.c`).
    Each core runs a scheduler *loop* on its own stack; actors switch
    back to their core's loop rather than directly into each other. That
    gives an idle place to sleep (`sti; hlt`), lets a dead actor be reaped
    from a stack that isn't its own, and guarantees only the core that
    took an actor off the run queue can run it.
  - One TSS per core (`hal_set_kernel_stack()` picks the executing core's,
    plus its own #DF stack) and one local-APIC preemption tick per
    non-boot core (vector 48; the PIT/8259 still only reaches the BSP).
  - `hal_cpu_id()` via CPUID leaf 1 (works under any actor's CR3, unlike
    an LAPIC MMIO read, and ring 3 can't corrupt it the way it could a GS
    base). The LAPIC is now mapped, supervisor-only and uncached, into
    *every* address space so a tick landing in an actor's private CR3 can
    write its EOI.
  - **A big kernel lock** (`hal/x86_64/cpu.c`, a ticket lock): every entry
    from ring 3 (syscall, IRQ, fault) takes it, released when the core
    returns to ring 3 or idles. Actors' own work runs truly in parallel;
    kernel work is serialized. The deliberate consequence: every kernel
    global that relied on "interrupts are off, so only one caller exists"
    (`storage.c`/`loader.c` scratch buffers, `safe_string_buf`, `actors[]`,
    the allocator) stays valid *unchanged*. Narrowing it is Phase 28's
    audit, not this. Ownership is per core, not per actor, and a
    watchdog reports (once, on the raw serial port) if a core waits for it
    implausibly long.
  - `actor_terminate()` on an actor that is RUNNING on the other core is
    deferred (`kill_pending`) and carried out at that actor's next kernel
    entry — freeing its stack or address space under a core that is
    executing on them would crash it.
  - Page tables are allocated (`alloc_dma_pages`) instead of living in
    `.bss`, which lifted the 19-actor ceiling: `MAX_ACTORS` is 24.
  - `SYS_SLEEP`: input-polling actors sleep instead of yield-looping. A
    yielding poll loop kept both cores and the kernel lock permanently
    busy (idle-halts stayed 0) and starved real work.
  - Capability tables reclaim CAP_SEND/CAP_TERMINATE entries naming a dead
    actor. Without it a parent that spawns and reaps children (the kill
    test) ran out after ~9 children.
- **Front end: the Cores app** (7th desktop app). Live per-core view (which
  actor each core is running, switches, idle-halts) from one atomic
  `SYS_CORE_INFO` snapshot, plus two tests you run with a key:
  - `b` — parallelism: one CPU-bound burner alone for a fixed TSC window,
    then two at once, then one again; speedup = (A+B) / best solo, and each
    burner reports which cores it touched (CPUID from ring 3). **Verified,
    including the deliberate negative control**: `-smp 2` measured 1.44x–
    1.95x across runs (burners on cores 0 and 1), `-smp 1` measured
    0.68x ("not parallel", core 1 shown offline). A single core cannot
    exceed 1.00x, so the verdict threshold is 1.25x.
  - `s` — kill test: launches never-yielding spinners and terminates each
    mid-run. 48 launched, 48 killed, every kill deferred to the target's
    next kernel entry, both cores still up.
- **Bugs this exposed, all fixed** (each found by running it, not by review):
  a deferred kill ended the actor *before* its timer IRQ's EOI was sent, so
  the 8259 kept IRQ0 "in service" and the whole machine froze with both
  cores in ring 3 (fix: EOI first, in one place, `exception_handler`); the
  AP came up with CR0.CD/NW set (caches off — invisible in QEMU, an order-
  of-magnitude slowdown on real hardware); a test-and-set lock let one core
  starve the other (now a ticket lock); a `SYS_CORE_INFO` per core sampled
  two different instants (now one syscall); CI's own log/serial mirroring
  meant a fast redraw put half a megabyte in a 30 s boot log (now 1 Hz).
- **Completion: up to 16 cores (`MAX_CPUS` 16).** The two-core version
  woke every other core at once with a broadcast INIT-SIPI onto ONE fixed
  boot stack and had a single `tss_ap`, so `-smp 3` corrupted itself and
  hung (confirmed before fixing: the log stopped at "Starting preemptive
  scheduler"). Now:
  - `hal/x86_64/acpi.c` reads the firmware's own core list (RSDP -> RSDT ->
    MADT, enabled processor-local-APIC entries only). No usable ACPI falls
    back to the classic two cores.
  - Cores are woken **one at a time** by APIC ID (`hal_lapic_wake_cpu`, INIT
    then SIPI twice), each onto its own 16KB allocator-provided stack (the
    trampoline reads its stack top from a mailbox cell), and the boot core
    waits for each to check in before touching the mailbox again.
  - One TSS, one #DF stack and one GDT descriptor per core (entry
    8+2*(cpu-1)); `hal_set_kernel_stack` indexes by core.
  - The Cores app shows every online core (wide lines up to 4 cores, two
    compact columns beyond), runs the parallelism test with one burner per
    core (capped at 6 by free actor slots) and reports efficiency as a share
    of linear; and it now shows the **kernel lock's contention live** (share
    of time held, average share of each core's time spent waiting) via a new
    `SYS_KERNEL_STATS`.
  - Verified: booted and ran actors on every core at `-smp` 1, 2, 3, 4, 8,
    12 and 16 (all 16 cores listed running distinct actors); parallelism
    test 3.5x of 4 (88%) on 4 cores, 4.2-4.6x of 6 burners on 8 and 12 cores;
    Security Lab 7/7 HELD and the kill test 16/16 on 4 cores.
- **Measured, and it changed the design work: the console was the
  bottleneck, not the lock.** The lock gauge read "held 95%, cores wait 74-86%"
  even with nearly every core idle. Cause: a write to the *focused* window
  redrew all 2000 screen cells after **every character** (each cell an MMIO
  store, a trap into QEMU's device model), and every console write happens
  inside a syscall holding the kernel lock. Fixes: redraw is deferred to the
  end of a whole write (`hal_console_flush`), and a RAM shadow of the screen
  means a redraw stores only cells that changed. Lock held fell from 95% to
  10% and waiting from 86% to 1% on 8 cores; the 8-core demo went from
  crawling to normal. (This is also exactly what Phase 28 should measure
  *before* narrowing any lock.)
- **Honest limits.**
  - Above ~8 cores the one big kernel lock, not the hardware, is what limits
    scaling: at 16 cores each core's timer tick needs the lock, and cores
    spent ~97% of their time waiting for it (efficiency 44%). Tickless idle
    (stop an idle core's timer, wake it with an IPI) and finer locks are
    Phase 28's job, and the lock gauge is how to check they worked.
  - This host has 12 logical CPUs, so 16 emulated cores are oversubscribed
    and run ~4x slower than wall-clock; correctness at 16 is verified, speed
    at 16 is not meaningful here.
  - Still open: x2APIC (needed above 255 cores; the ID from CPUID leaf 1 is
    8 bits), an IPI to wake an idle core the moment work appears (today: at its
    next tick), and a calibrated LAPIC timer (the count is a fixed guess).
  - Tooling: this Windows QEMU (11.1.0) crashes with an access violation
    (exit 0xC0000005) roughly half the time it starts, and occasionally
    mid-run; a "stalled" log is usually that, not a hang. `alive.py`-style
    harnesses that watch the process exit code tell the two apart.

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

### Phase 13a — Remote spawn: the first slice — DONE (verified on two real QEMU instances in CI)

True live migration (above) needs two things this repo doesn't have
yet: a way to ship a running actor's state at all, and (for the
capability-table half specifically) Phase 29's device authentication,
since a capability naming a LOCAL slot doesn't even mean anything on a
different device without some notion of which devices are trustworthy
enough to hand one to. Rather than block on both, this is the
tractable first step: one device can ask another to run a program, and
prove the request itself carries zero authority.

- `core/main.c`'s `actor_network_peer` — already the Phase 12 HELLO/ACK
  discovery demo, symmetric across both instances — extended with a
  new message pair, `MSG_NET_SPAWN_REQUEST`/`MSG_NET_SPAWN_REPLY`.
  Once two instances find each other (the existing handshake,
  untouched), each asks the OTHER to load-and-run `hello.bin`.
- No new wire format and no new syscall: this rides entirely on the
  existing generic `{type, data}` message transport
  (`core/net.c`, unchanged) and the existing `SYS_SPAWN_PROGRAM`
  syscall — exactly as capability- and trust-gated on the RECEIVING
  device as any local `run` already is (Phases 16/20/27).
- Why this is invariant-4-safe without Phase 29: the spawned actor
  gets **zero** capabilities, same as any fresh local spawn — nothing
  is delegated across the wire, only a request. The only reason it can
  succeed at all is that `kernel_main` already, independently, granted
  `NETWORK_PEER_SLOT` the local authority to do it
  (`CAP_SPAWN`/`CAP_READ_OBJECT` for the two demo programs). An
  untrusted peer asking for this gains nothing it couldn't already be
  refused.
- Known, labeled simplification: the program is named by a raw storage
  object id (`HELLO_PROGRAM_OBJECT_ID`, `2`), which both instances only
  share because they booted the identical seeded demo in the same
  order — id-by-convention, not a real name/hash-based lookup. A later
  pass (whenever a device needs to run something the peer doesn't
  already have) needs actual code transfer, which itself needs
  fragmentation: `core/net.c`'s current wire payload carries 8 bytes of
  `data` per message, nowhere near a program image's size.
- Verification: the existing two-instance CI workflow
  (`.github/workflows/network-test.yml`, Ubuntu-only — this repo's own
  Windows QEMU build stalls with `-device virtio-net-pci` attached at
  all, confirmed this session, unrelated to this change; local
  single-instance regression stayed clean throughout). A new step
  greps both peers' logs for `"a program I named is now genuinely
  executing on a DIFFERENT device"` — the confirmation only printed
  once a peer's own `MSG_NET_SPAWN_REPLY` reports success.

**Status as of this session's last check**: checking this run's CI
result uncovered a SEPARATE, pre-existing bug — every workflow run
since VajraLang was added (commit `75affd1`) had been failing at the
"Build Vajra" step itself on Ubuntu (`tools/build-c.ps1` hardcoded
`powershell`, which doesn't exist there; only `pwsh` does), silently,
through every Phase 23-27 commit. Nobody had checked CI during that
work. Fixed (see `tools/build-c.ps1`'s own `$PwshExe` detection) and
confirmed: the NEXT run's "Build Vajra" step succeeded.

**That same run then uncovered something much bigger**: a genuine
`KERNEL PANIC — Vector: 0x6 (#UD), RIP: 0x49000` on EVERY boot on
CI's runner — the single-instance sanity check (no networking device
at all) AND both two-instance peers, byte-identical. Fully
deterministic, not the HELLO handshake's known timing-fragility, not
networking-related at all. Never reproduced locally (CI: QEMU 8.2.2 +
Ubuntu's own clang/lld/nasm; every local regression this whole session
used QEMU 11.1.0 + a separate LLVM install on Windows) — a real
portability gap totally invisible until CI's build step itself started
working again.

**Confirmed against CI's OWN build** (via a diagnostic CI step —
`tools/build-c.ps1` now permanently produces `build/kernel_debug.elf`,
a real ELF with symbols from the same objects, never booted;
`network-test.yml` dumps its symbol table and disassembly into the job
summary): `0x49000` is exactly `as_pml4` (`hal/x86_64/paging.c`'s
per-actor page-table pool), row 0. QEMU's own `-d int` trace narrowed
WHEN more precisely than first thought — not at initial scheduler
start, but mid-syscall: `int 0x80` (CPL3→CPL0, inside `hal_syscall`,
CR3=`0x4a000` = `as_pml4[1]`, a DIFFERENT actor's own valid table)
immediately followed by the `#UD` at `0x49000`, CR3 unchanged. An
actor made a syscall, and somewhere in the kernel's own handling of
it — CR3 never switching, as designed — execution jumped to a
DIFFERENT actor's page-table array and tried to execute page-table
data as code. Reads like a corrupted return address or function
pointer inside the syscall path itself, not the context-switch/fake-
frame path first suspected (`context_switch.asm` and `core/actor.c`'s
fake-frame setup were both re-read and look internally consistent for
the ordinary case).

**Root cause, found**: raw-serial syscall tracing (temporary, since
reverted) pinned it to actor slot 1's very first syscall — `SYS_WRITE`
for `actor_two`'s `"[Actor 2] tick "` — crashing immediately as
dispatch began, before any case body ever ran. Disassembling
`syscall_handler` (via `build/kernel_debug.elf`, now a permanent
`tools/build-c.ps1` build product) showed why: clang compiles its
25-case `switch(num)` to a jump table — a `.rodata` array of absolute
case-target addresses, read and jumped through indirectly
(`jmp [table + 8*(num-1)]`) — even at the default `-O0` this project
builds with. Every link here goes straight to `ld.lld --oformat
binary`, a raw flat binary with no relocation table retained, so
every address in that table has to be fully resolved and baked in AT
LINK TIME. On this specific Ubuntu `ld.lld` build, `jump_table[0]`
(the `SYS_WRITE` entry) came out wrong — landing exactly on
`as_pml4`'s own address instead of the real case body. This repo's own
Windows LLVM build resolves the identical table correctly, which is
exactly why this was invisible locally the whole time.

**Fix**: `-fno-jump-tables` added to every clang invocation in
`tools/build-c.ps1` (see its own top-of-file comment), forcing a plain
compare-and-branch dispatch instead of a jump table — sidesteps
whatever this specific `ld.lld` build gets wrong, without needing to
know exactly what. Verified locally: `llvm-objdump` confirms the
indirect `jmp` is gone from `syscall_handler`'s own disassembly, and a
full regression boot stays clean. All temporary debug instrumentation
reverted out of `syscall.c`.

**Confirmed fixed on CI** (commit `83468c3`'s run): both the
single-instance sanity check and Peer A's full boot now run all the
way through to `[Greedy] exiting` — the HELLO/ACK exchange step and
the reliable-delivery step both pass for the first time ever. The
panic is genuinely gone.

**A second CI-only bug then showed up** (visible only once the kernel
booted that far): blank object names, `suspicious.bin` reading back
`""`, failing loader spawns. **Root cause of the names/contents: a
silently truncated kernel image.** The boot loader read exactly 120
sectors (61,440 B) and stopped; CI's Debian clang produces a ~4KB
larger image than this repo's Windows LLVM (CI: 62,188 B), so
everything past `0x2f000` — late `.rodata` literals and all `.data` —
loaded as zeros. **Fix and confirmed on CI (`fd60745`)**: `boot.asm`
fix #6 (chunked read, 256-sector cap), storage's LBAs moved past it,
and `tools/build-c.ps1` now fails the build if `kernel.bin` exceeds the
cap it reads from `boot.asm`.

**That CI log then exposed two ordinary logic bugs**, both fixed,
awaiting CI: (1) spawns failing when all 17 actor slots are momentarily
full (15 static + a Worker + an Inspector) — a timing race CI hit and
this machine didn't; now a bounded retry-with-yield
(`user_spawn_program_retry`). (2) The spawn request was handled only in
`actor_network_peer`'s drain loop, but `core/net.c` auto-ACKs any frame
from either loop, so a request landing during the HELLO handshake loop
was ACKed and then dropped; now `net_handle_spawn_msg()` is shared by
both loops. **Confirmed on CI (`d76f0a5`)**: every step of the two-instance
workflow passes, including `Verify Phase 13a` — one device asked its
peer to run `hello.bin`, the peer spawned it under its own local
authority, and the reply crossed back. Phase 13a is done. What it is
NOT: no live state moves (the program restarts from scratch on the
peer), the program is named by a shared object id, and there is no
code transfer — true migration (13b) still needs payload fragmentation
and Phase 29's authenticated fabric.

*Philosophy: §3 invariant 4, directly — the first real instance of "a
device boundary can only narrow authority" actually enforced, not just
stated.*

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
genuinely be *used* for real tasks — open a file, run a program,
install new software — with capabilities no less complete than any
major OS's command-line environment. **Update, mid-Phase-18 (explicit
user direction, not a silent scope change): a real text-mode desktop
DOES now exist** — icons, a taskbar, an apps launcher, a mouse
(`docs/DESKTOP_DESIGN.md`) — superseding this section's original
"no windowing, no desktop shell" framing. Still bound by
`docs/PHILOSOPHY.md` §5's actual constraint, which was narrower than
this section's own paraphrase of it: **text characters and CP437
glyphs only, no pixel graphics** — a real desktop METAPHOR is in
scope; a graphical rendering surface is not, and remains the one line
this project has not crossed.

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

### Phase 17 — A filesystem namespace over the object store — DONE (Milestone 17)

Phase 8's object store stays exactly what it is — capability-addressed,
not path-addressed — this phase adds a naming layer ON TOP, not a
replacement. A path is how a human or a shell finds an object; a
capability is still what's required to touch its contents once found.
Conflating the two would quietly undo Phase 8's whole point.

- **DONE: a real on-disk directory** (`core/storage.c`) — name -> {id,
  trust, size}, persisted to a dedicated sector and rebuilt at boot, so
  the namespace survives between separate QEMU launches against the
  same disk image, not just within one boot.
- **DONE: create/list/rename/delete operations on names**
  (`SYS_CREATE_NAME`/`SYS_LIST_OBJECTS`/`SYS_RENAME_OBJECT`/
  `SYS_DELETE_NAME`), distinct from (and layered above) Phase 8's
  `CAP_READ_OBJECT`/`CAP_WRITE_OBJECT`/`CAP_PROMOTE_OBJECT` on the
  objects those names point at — four new capabilities
  (`CAP_LIST_NAMES`, `CAP_CREATE_OBJECT`, `CAP_RENAME_OBJECT`,
  `CAP_DELETE_OBJECT`) gate them, kernel-enforced the same way Phase
  9's trust-state transitions already are, not actor-brokered through
  a separate naming-service actor.
- **DONE: resolved design constraint, implemented as designed**: a
  path lookup (`SYS_LOOKUP_NAME`) returns an object id and nothing
  else — never a capability, and requires no capability itself. An id
  is public knowledge (like a phone book entry); the capability to act
  on what it points at is still a separate, explicit grant. Listing
  every name that exists, unlike a lookup by a name already known, IS
  capability-gated (`CAP_LIST_NAMES`) — a genuinely different kind of
  authority (§3 invariant 2), not an oversight.
- See `docs/MILESTONE17_CHANGELOG.md` for the full account, including
  the idempotent-boot-object-creation change persistence required and
  the two-boot verification that actually proved it.

### Phase 18 — Input devices & an interactive shell — DONE (Milestone 18)

- **DONE: a keyboard driver** (`hal/x86_64/keyboard.c`, PS/2,
  interrupt-driven, IRQ1) — the first HAL input device this kernel has
  ever had; everything before was output-only (console) or
  block-storage.
- **DONE: a real-time clock driver** (`hal/x86_64/rtc.c`, CMOS).
- **DONE: a real interactive shell actor** (`core/main.c`'s
  `actor_shell()`): line editing (backspace), a colored prompt, and
  built-ins covering the namespace (Phase 17: `ls`), the loader (Phase
  16: `run <name>`), the RTC (`date`), and job control (`count`,
  `pipe`, `jobs`, `stop`, `kill`). Verified against a REAL running
  instance by injecting actual keystrokes through QEMU's own emulated
  PS/2 controller (monitor `sendkey`), not just compiled — `date`
  printed the live CMOS clock value, `ls` the live namespace state.
  Argument passing into a loaded program and a working-directory
  concept are explicitly deferred (Phase 17's namespace stayed flat by
  its own design; loaded programs have no argv slot yet) rather than
  silently dropped — see `docs/MILESTONE18_CHANGELOG.md`.
- **DONE: pipes as ordinary mailboxes, not a new mechanism** — the
  `pipe` built-in wires two spawned actors together using only
  existing primitives (`SYS_SPAWN`'s own auto-grant + `SYS_GRANT` +
  `SYS_SEND`/`SYS_RECEIVE`), zero new syscalls, concretely proving the
  line below.
- **DONE: job control**, both tiers of the resolved design constraint
  below genuinely exercised (`stop`/`kill` built-ins), plus `count`/
  `jobs` for spawning and tracking background actors. True
  foreground-blocking (`run` waiting for completion before returning to
  the prompt) is an explicit, labeled follow-up, not built this
  milestone — see the changelog's own "Known follow-ups."
- **Resolved design constraint, implemented as designed**: "interrupt a
  running program" is two distinct tiers, both already native to the
  model, not a new async signal mechanism. A graceful request ("please
  stop") is an ordinary message the target actor checks at its own safe
  points and may ignore — ordinary mailbox delivery (Phase 5), nothing
  new. A hard stop is the shell's existing capability-mediated
  `SYS_TERMINATE` (Phase 7). No forced control-flow injection into
  another actor was ever added.
- **DONE, then superseded within the same milestone by a real desktop
  — see `docs/DESKTOP_DESIGN.md` for the full account.** First shipped
  as two fixed panes (System Log / Shell); the user's own explicit
  redirect ("I want a desktop means I want a desktop... icons, click,
  open files, a basic OS in purely textual format") took it further,
  in stages:
  - **DONE: a PS/2 mouse driver** (`hal/x86_64/mouse.c`, IRQ12) and a
    software cursor — Stage 1 of the design doc, verified with real
    injected packets via QEMU's monitor.
  - **DONE: `hal/x86_64/console.c` rebuilt as a real desktop
    compositor** — a background, clickable icons, a taskbar with an
    `[ Apps ]` launcher and per-app tabs, a start-menu popup, a title
    bar with a close button. Four fixed apps today (System Log, Shell,
    Files — live, regenerated from `core/storage.c` on focus — and
    About); each keeps its own offscreen content buffer
    (`alloc_dma_pages()`, not static `.bss` — seeded a real structural
    fix, see below) so switching away and back preserves it exactly.
    Apps are always maximized (one fills the screen) rather than true
    overlapping/movable windows — a deliberate scope cut, not
    forgotten (see Phase 26 below).
  - **A sixth recurrence of this project's `.bss`-vs-fixed-address
    collision bug, fixed structurally this time**: the compositor
    pushed kernel `.bss` past the page tables' own address. Fixed by
    moving the page tables, E820 map, and AP trampoline/stack below
    the kernel's own load address permanently (space free since
    Milestone 13 moved the kernel up), rather than nudging the budget
    again — see `src/boards/pc-bios/boot.asm`'s own "structural fix"
    comment. This is the same bug class Milestone 13's changelog
    already named; it will not recur from `.bss` growth again by
    construction.
  - Still genuinely not a GUI in the sense §5 actually forbids: text
    characters and CP437 glyphs only, no pixel graphics, no font
    rendering. A desktop METAPHOR rendered in text is in scope; a
    graphical surface is not.
  - **Design correction, same as before**: `CAP_CONSOLE` gates
    `SYS_KEY_READ` (keyboard) and now also gates which app's
    `SYS_KEY_READ` calls succeed — an unfocused app's actor is denied
    keystrokes (still draining the hardware buffer so they don't queue
    up), so typing while browsing Files doesn't leak into the Shell's
    REPL once refocused.

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

### Phase 22 — An actor-native language — PARTIALLY DONE (v0, host-side)

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

- **DONE: VajraLang v0** (`tools/vajrac.ps1`, grammar in
  `src/userland/calc.vj`) — a real lexer, recursive-descent parser, and
  AST for a small calculator language (`let`, `print`, `+ - * /`,
  precedence, unary minus, variables), targeting C as its codegen
  backend (an ordinary, legitimate compiler architecture — Nim and
  early C++ do the same), which then goes through Phase 16's own
  proven `clang` + `ld.lld` + `program_header` pipeline unmodified.
  Verified via a real boot: the compiled `calc.bin` runs as a genuine
  ring-3 actor and prints exactly the right results.
- **NOT YET actor-native syntax** — v0 is a calculator with C-shaped
  expressions, not yet the `spawn`/`send`/capability-typed language
  this phase's own opening paragraph describes. That's still open
  work, layered onto the same front end once there's a reason to
  (Phase 24 below is the more urgent half of "why this matters").
- **NOT YET self-hosted** — the compiler runs on the DEVELOPMENT
  machine (PowerShell, since this build machine's clang has no host C
  library configured for a native `vajrac` binary), not inside Vajra.
  See Phase 24.

*Philosophy: §2 (this document's own model) — a language whose syntax
is a direct expression of it, not a separate concern bolted on.*

## Phases 23-27 — Hardening: containment that's actually real, not assumed

**Inserted ahead of everything below, after an external code review
(ChatGPT, reading the actual working tree — not a hypothetical audit)
found that several of Vajra's own core invariants are currently
VIOLATED by the shipped code, not just untested.** Verified directly
against source before writing this down (file/line, not paraphrase):
a CPL3 page fault genuinely reaches `KERNEL PANIC` today
(`hal/x86_64/interrupts.c`'s `exception_handler()` takes no CS and
treats every non-IRQ vector as fatal unconditionally); `SYS_WRITE`
genuinely follows an actor-controlled pointer unbounded
(`hal/x86_64/syscall.c`); `storage_read()` genuinely refuses only
`OBJ_REJECTED`, not `OBJ_UNTRUSTED`/`QUARANTINED`/`ANALYZED`
(`core/storage.c:352`, `core/loader.c` never checks trust at all);
loaded-program pages are genuinely mapped `present|writable|user` with
no NX bit anywhere (`hal/x86_64/paging.c:246`); capabilities are
genuinely bare `{op, target}` ints with no generation counter
(`core/actor.c:138`); and `loader_spawn_program()` genuinely leaks its
`alloc_dma_pages()` allocation if `actor_spawn_program_child()` fails
(`core/loader.c:70-81`).

**This is not "before Phase 31's demo."** `docs/PHILOSOPHY.md` §3
invariant 1 — "isolation is real, not conventional" — is violated
RIGHT NOW, today, by ordinary ring-3 bugs, not just deliberate hostile
code: `volatile int *p = (int *)0x12345678; *p = 42;` from any actor
already takes down the whole machine. Every "the kernel survived" claim
in every changelog so far has been true because nothing has hit a bad
pointer yet, not because the kernel enforces the isolation it claims.
These five phases close that gap, in dependency order, BEFORE Phase 28
(real SMP) and Phase 30 (self-hosting) make the problem worse — a
second core hitting the same unguarded static scratch buffers, or a
self-hosted compiler's own ordinary bugs panicking the kernel
constantly with no malice involved at all.

**Standing test, starting now, not saved for Phase 31**: each phase
below adds to one growing `hostile_ring3.c` (or its VajraLang
equivalent, once Phase 30 exists) exercising exactly the failure the
phase claims to fix — the same discipline (§3.7) this project has used
since Milestone 1, applied to security properties specifically instead
of waiting for one final demonstration:
1. read another actor's memory — Phase 23, checked off
2. write another actor's memory — Phase 23, checked off
3. read/write a kernel address via a syscall pointer arg — Phase 24, checked off
4. jump to invalid/unmapped code — Phase 23, checked off
5. invoke a syscall with a huge length — Phase 24 (overflow-guarded in
   `actor_current_owns_range()`/`actor_current_may_read_range()`; not
   separately re-run live, same guard the wild-pointer test above
   already exercises)
6. forge a capability — already denied (Phase 8), add to the suite
7. use a stale object/actor identity (act on a dead, reused slot) —
   Phase 25, fix landed but not yet reproduced live (see that phase's
   own note — the natural repro is racy against the rest of the demo)
8. run an object that was never promoted past `OBJ_UNTRUSTED` — Phase
   27, checked off (defense-in-depth: this exact attempt was also
   capability-denied; the trust gate itself is exercised every ordinary
   boot, since hello.bin/calc.bin now require it to load at all)
9. write into a loaded program's own code pages post-launch — Phase 27,
   checked off (genuine #PF, caught by Phase 23, actor terminated)
10. exhaust the program-loader pool via repeated run/exit — Phase 26,
    fix landed but not yet reproduced live (see that phase's own note)

Each item gets checked off — genuinely triggered in QEMU, kernel
observed to survive — the milestone its fix lands, not deferred.

### Phase 23 — Fault containment: a CPL3 fault kills the actor, not the kernel — DONE

The single highest-priority fix. Was: any exception from ring 3
(`#PF`, `#GP`, `#UD`, ...) reaches `exception_handler()`, which cannot
even tell where the fault came from (`isr_stubs.asm` passes vector,
error code, and RIP — never the saved CS, though the CPU's own
exception frame already contains it) and treats every non-IRQ vector as
`KERNEL PANIC` unconditionally.

- `isr_common` (`hal/x86_64/isr_stubs.asm`) passes the saved CS (already
  on the interrupt frame) as a 4th argument to `exception_handler()`.
- `exception_handler()` branches on it: CS's low 2 bits (CPL) == 3 means
  the fault originated in an actor, not the kernel. Actor-origin fault:
  terminate the offending actor (reuse Phase 7's existing termination
  path — mailbox/capability-table cleanup already exists there), reap
  its resources, `schedule_next()`, kernel continues. CPL0-origin fault:
  unchanged — genuinely a kernel bug, `KERNEL PANIC` stays exactly
  right for that case.
- Verification: `hostile_ring3.c` items 1/2/4 above, run for real,
  kernel observed still running and still scheduling OTHER actors
  immediately after.

**Verified**: `hello.bin` temporarily made to null-deref
(`*(volatile long *)0 = 1`) right after its first `user_write()`.
Serial log: `[actor 0x0000000B terminated -- fault vector
0x0000000E, RIP 0x...20]`, then calc.bin, the Worker/Coordinator/
Scanner/Namer/Intruder actors, and the shell prompt all continue
exactly as a normal boot — no panic, no halt. Full `-smp 2` regression
re-run afterward (fault reverted) also clean: AP core runs, calc.bin's
5 results print, shell reaches its prompt. Implementation:
`hal/x86_64/isr_stubs.asm` (CS as 4th arg) + `hal/x86_64/interrupts.c`
(`exception_handler` branches on CPL, calls the existing `actor_exit()`
for a CPL3-origin fault instead of panicking).

*Philosophy: §3 invariant 1, directly — this is the fix that makes the
invariant true instead of aspirational.*

### Phase 24 — Safe user memory: bounded copies, not trusted pointers — DONE

Was: `SYS_WRITE` handing `hal_console_write()` an actor-controlled
pointer and walking it to NUL was simultaneously a kernel memory read
primitive (if the address happened to be mapped) and a kernel-wide DoS
(if it wasn't) — and it wasn't the only syscall doing this
(`actor_receive()`, `storage_read()`/`storage_write()`,
`hal_rtc_read()` all took a raw `a1`/`a2`/`a3` cast straight to a
pointer).

- `actor_current_owns_range(addr, len)` (`core/actor.c`) — true only if
  `[addr, addr+len)` lies entirely inside memory this actor's address
  space actually maps present+user: its own stack, or (a loaded
  program) its own `PROGRAM_VBASE` window. Used for every syscall arg
  the kernel WRITES into (`SYS_RECEIVE`, `SYS_OBJECT_READ`'s dest,
  `SYS_NET_RECEIVE`, `SYS_LIST_OBJECTS`'s out, `SYS_RTC_READ`,
  `SYS_MOUSE_READ`) — the direction where an unchecked pointer means
  real corruption, of another actor or of the kernel itself.
- `actor_current_may_read_range(addr, len)` — the same, plus the
  kernel's own low image (below 1MB, `link.ld`): where every built-in
  demo actor's own string literals genuinely live (`paging.c`'s own
  comment — taking a `.rodata` literal's address was always
  unrestricted, only dereferencing it was ever the question). A
  first version restricted reads the same as writes and broke every
  built-in actor's `user_write("...")`/`user_object_write(id, "...",
  n)` call — reverted to this two-tier design instead of narrowing the
  design goal. Used for every syscall arg the kernel only READS
  (`SYS_WRITE`'s string, `SYS_OBJECT_WRITE`'s source buffer,
  `SYS_CREATE_NAME`/`SYS_RENAME_OBJECT`/`SYS_LOOKUP_NAME`'s name
  string via `copy_user_string()`, a bounded byte-at-a-time NUL scan
  capped at 512 bytes).
- Both refuse `addr+len` overflow (`end < addr`) before comparing
  bounds — a huge length can't wrap past the check.
- Deliberately NOT "check the pointer is below some address" (the
  critique that prompted this phase named that exact wrong answer) —
  every check is against the calling actor's OWN mapped ranges,
  looked up fresh from `actors[current_actor]` every call.

**Verified**: `hello.bin` temporarily made to call raw syscalls
directly with a wild pointer (`SYS_WRITE` at `0xDEADBEEF`) and a
foreign-window pointer (`SYS_RECEIVE` at `0x100000`, plausible-looking
but not this actor's own stack) — both refused (`-1`), no panic, no
hang, hello.bin exits normally and the rest of the boot continues.
Full `-smp 2` regression re-run afterward (test code reverted) also
clean: all built-in demo actors' string literals print correctly,
`storage_write()` of literal payloads (`"hello from disk!"`,
`"BADSTUFF payload"`) still works, calc.bin's 5 results print, shell
reaches its prompt.

*Philosophy: §3 invariant 1 again (the memory boundary is only real if
crossing it is checked, not just architecturally possible to check).*

### Phase 25 — Generation handles: actor and object identity stops being reused silently — DONE

Was: `CAP_SEND(5)` meant "whoever currently occupies slot 5" —
capability targets were bare ids (`core/actor.c`'s `struct capability
{ int op; int target; }`), and slots ARE reused once an actor dies. A
capability granted for one actor could silently start applying to a
different, unrelated one that landed in the same slot later. This is a
hard blocker at Phase 13 specifically (a remote capability naming a
slot by number has no way to know if that slot means the same thing it
did when the capability was issued) — fixed now, before more of the
system gets built assuming raw ids are stable identity.

- `struct capability` gained a third field, `target_gen` — the
  target's generation at the moment this capability was granted. Not a
  separate `ActorHandle`/`ObjectHandle` type replacing every existing
  `int` slot/id parameter throughout the codebase (that would have
  touched every syscall and every capability call site) — the
  generation is looked up and compared INTERNALLY, in `core/actor.c`
  alone, so no external signature changed.
- `struct actor` gained `generation` (bumped in `actor_spawn()` on
  every hand-out of a slot, including the first); `struct object`
  (`core/storage.c`) gained the same, bumped in `alloc_object()`
  (`storage_object_generation()` is the new accessor).
- `current_generation_of(op, target)` — one shared lookup, used by both
  `actor_add_cap()` (recording what generation a new grant was made
  against) and `actor_has_cap()` (comparing a stored capability against
  the target's CURRENT generation) — actor slot for `CAP_SEND`/
  `CAP_TERMINATE`, object id for the `CAP_*_OBJECT` ops, and a fixed 0
  for every blanket op (`CAP_SPAWN` etc., whose target is always the
  placeholder 0, not a real identity). The same lookup on both sides is
  what makes a stale capability start failing the instant the slot/id
  it named gets reused, not just eventually.
- Verification: `hostile_ring3.c` item 7 — hold a capability, let its
  target die and get reused, confirm the OLD capability no longer
  reaches the NEW occupant. **Not reproduced live this session**: the
  natural repro (Coordinator's Worker 1 exits, Worker 2 spawns into the
  same freed slot) turned out to race against every OTHER actor in the
  demo also competing for that freed slot (Scanner's dynamic
  inspectors especially) — confirmed non-deterministic across several
  runs, Worker 2 landing in a different slot each time. Verified
  instead by full regression (every existing capability check in the
  demo — Coordinator/Worker send+terminate, Namer create/rename/
  delete, Reader's promoted-object read, the quota-exceeded denial —
  still passes identically with the generation check now live, proving
  the added comparison isn't accidentally always-false) and by code
  review of `current_generation_of()`'s symmetry between grant time and
  check time. A deterministic live repro needs either a dedicated
  single-actor test harness or temporarily quieting the rest of the
  demo — worth doing before Phase 13 actually depends on this.
- **Reproduced live (Security Lab attack 9, added afterwards).** The lab
  creates an object, keeps its capabilities, deletes it, and has an
  accomplice actor create a new object that lands on the same id; then it
  tries its old capability on the stranger's object. Result: `HELD`.
  Negative control: with the generation comparison in `actor_has_cap()`
  disabled, the same attack reports `BREACH -- my stale capability read a
  stranger's object`. Only the OBJECT half is reproducible this way: an
  actor's capabilities naming a dead ACTOR are now cleared outright when
  it dies (`reclaim_caps_for()`, Phase 10), so a stale actor capability
  never survives long enough to meet a reused slot -- the generation
  check there is defence in depth. Object capabilities are not cleared,
  so the generation check is what stops them; a full capability table is
  swept of stale entries on demand (`actor_add_cap()`), otherwise a few
  create/delete cycles would exhaust it.

*Philosophy: §3 invariant 3 (capability soundness) — a capability that
can silently apply to the wrong target once a slot is reused isn't
sound, even though nothing about the grant itself was forged.*

### Phase 26 — Resource lifecycle: nothing leaks across run/exit — DONE

Was, concretely: `loader_spawn_program()` (`core/loader.c:70-81`) leaked
its `alloc_dma_pages()` allocation if `actor_spawn_program_child()`
failed after the pages were already allocated. Separately,
`hal_address_space_map_program()`'s `PROGRAM_POOL_SIZE` (2) entries
were explicitly, permanently never freed on actor death — a real,
documented limitation that was fine for a demo that loads one program
once, and a hard ceiling the moment `run`/exit repeats.

- `loader_spawn_program()` now frees the pages it allocated (one
  `free_page()` call per page — `alloc_dma_pages()` has no bulk
  counterpart, same as everywhere else in this codebase) on every
  failure path after the allocation, not just leaving them held with
  no owner.
- `hal_address_space_release_program(slot)` (`hal/x86_64/paging.c`) —
  the release half of `hal_address_space_map_program()`, marking that
  slot's `as_pt1` pool entry free again. Called from `core/actor.c`'s
  `reap_dead_actors()`, the same instant it frees a dead actor's
  ordinary stack page — gated on `program_size != 0` (Phase 24's own
  field), the same "already handled, 0 means done" sentinel
  `stack_page` itself already used, reset to 0 right after release so
  a slot's pool entry is never released twice.
- Capability table reclamation on actor death: confirmed complete, not
  just directionally right, now that Phase 25 exists — capabilities the
  dead actor HELD are overwritten the moment its slot is next
  respawned into (`actor_spawn()`'s existing per-slot reset), and
  capabilities OTHER actors hold TARGETING the dead actor are already
  invalidated the instant that slot's generation bumps on respawn
  (Phase 25's `current_generation_of()`) — no separate cleanup needed
  on the targeting side at all.
- Verification: `hostile_ring3.c` item 10 — `run`/exit the same program
  more than `PROGRAM_POOL_SIZE` times in a row, confirm it keeps
  working instead of failing once the pool's exhausted. **Not
  reproduced live this session**: the existing demo's own program
  loader (`actor_program_loader`) is quota-limited to 2 spawns (the
  ordinary `MAX_SPAWNS_PER_ACTOR` fork-bomb guard — only the shell gets
  a raised quota, `actor_set_spawn_quota()`), so a same-actor
  repeated-`run` test needs either a temporarily raised quota or
  driving it through the interactive shell (which needs scripted
  keyboard input, not attempted this session). Verified instead by full
  regression (hello.bin/calc.bin's own one-shot loads still work
  identically with the release path now live) and by code review of
  the release call's gating.
- **Reproduced live (Security Lab attack 8, added afterwards).** The lab
  runs `hello.bin` six times in a row, each after the previous one
  exited. Result: `HELD -- loaded and ran 6 of 6`. Negative control: with
  the `hal_address_space_release_program()` call in `reap_dead_actors()`
  disabled it reports `BREACH -- loaded and ran 0 of 6` (the two pool
  entries were already consumed by the demo's own hello.bin and calc.bin
  and never came back). Both attacks are also in CI ("Verify the Security
  Lab").

*Philosophy: §6 (engineering discipline) — a resource that's fine for
today's demo but silently caps real use is exactly the kind of
shortcut this document says must be labeled the moment it's written,
not discovered later as an accidental design.*

### Phase 27 — W^X, and a real trust gate between "stored" and "loadable" — DONE

Was: loaded-program pages were mapped `present|writable|user` with no
NX bit at all (`hal/x86_64/paging.c:246` — genuinely RWX), and
`loader_spawn_program()` never checked an object's trust state
(`core/storage.c`'s `storage_read()` refuses only `OBJ_REJECTED`) —
"executable" and "loadable" were the same concept, and Phase 20's
quarantine gate was a frontend convention, not an enforced boundary.

- W^X: `hal_address_space_map_program()` (`paging.c`) now maps a
  program's window `present|user|execute`, deliberately NOT writable
  (flag `0x5`, not `0x7`) — no separate RW-then-RX runtime transition
  needed, because `core/loader.c` already copies the program's bytes
  into physical memory via the commons alias entirely BEFORE this
  mapping is ever built, and neither of today's two loaded programs
  (`hello.c`, VajraLang's `calc.vj` output) has any mutable global —
  their `let`-bound values are ordinary C locals on the actor's own
  SEPARATE stack, untouched by this change. A future JIT-style codegen
  backend (VajraLang self-hosted, Phase 30) that genuinely needs to
  write code at runtime should get an explicit, narrow transition
  function added at that point — never a standing RWX default again.
- `loader_spawn_program()` (`core/loader.c`) now refuses anything short
  of `OBJ_TRUSTED` via `storage_get_trust()`, checked before even
  reading the object's bytes — moving the check from "the storage
  layer's own read primitive, applied to everyone" to "the loader's
  own policy, applied specifically to execution." `kernel_main` now
  explicitly promotes `hello.bin`/`calc.bin` through the same
  `OBJ_UNTRUSTED → QUARANTINED → ANALYZED → TRUSTED` pipeline
  Scanner/Inspector uses for downloaded content (three
  `storage_promote()` calls each) right after seeding them — the
  kernel vouching for its own built-in demo programs the same way, not
  bypassing the gate for them.
- Verification: `hostile_ring3.c` items 8/9, both genuinely triggered
  in QEMU. Item 9: `hello.bin` temporarily made to write one byte into
  its own `_start` — real `#PF` (vector 0xE), CPL3-origin, caught by
  Phase 23: `[actor 0xB terminated -- fault vector 0xE, ...]`, no
  panic, rest of the boot continues (confirming "refused" — the RX
  mapping made the write genuinely unreachable, not merely policy).
  Item 8: `hello.bin` temporarily made to `SYS_SPAWN_PROGRAM` object 0
  (`payload.bin`, `OBJ_UNTRUSTED`) directly — refused (`-1`), logged
  `[hostile 8] SYS_SPAWN_PROGRAM on an untrusted object: refused, as
  expected`, no panic. (This specific attempt was already unauthorized
  by capability too — `hello.bin`'s spawned actor holds no
  `CAP_READ_OBJECT` for object 0 — so it demonstrates defense in
  depth, not proof the trust gate is what fired in isolation; the
  trust gate itself is still exercised on every ordinary boot, since
  `hello.bin`/`calc.bin` would fail to load at all without the
  explicit promotion added above.) Both test edits reverted after
  confirming; full `-smp 2` regression re-run clean, both programs
  showing `(TRUSTED)` in the namespace listing.

*Philosophy: §3 invariants 3 and 5 (small blast radius by default) —
an untrusted object should never reach "running with a real actor's
authority" by construction, not by the installer frontend behaving
itself.*

**Follow-on fix, same phase**: a second independent review (ChatGPT,
asked to audit the whole repo after 23-27 landed) found Phase 27's W^X
work had missed one range — `.user_text` (`link.ld`, the kernel-image
code ring-3 actors are allowed to fetch instructions from: `core/main.c`'s
built-in demo actors, `hal_syscall()`'s own wrapper) was still mapped
`0x7` (present|writable|user) in `hal_address_space_create()`
(`hal/x86_64/paging.c`) — genuinely RWX, the exact same class of bug
Phase 27 fixed for the loaded-program window, just in a spot that pass
didn't touch. Fixed the same way: `0x5` (present|user|execute, no
writable bit) — safe because nothing in this codebase ever writes to
`.user_text` at runtime (grep-confirmed, no self-modifying code).
**Verified live**: `hello.bin` temporarily made to write one byte at
`__user_text_start` (found via `llvm-nm build/kernel_debug.elf`,
`0x29000` in that build) — genuine `#PF` (vector 0xE), caught by
Phase 23 (`[actor 0xB terminated -- fault vector 0xE, ...]`), no
panic, rest of the boot continues to completion. Test code reverted;
full `-smp 2` regression re-run clean afterward. The review's other
main finding — `actor_current_may_read_range()` (Phase 24) still
permits reading the ENTIRE kernel image below 1MB, not just legitimate
`.rodata` literal addresses — is a real, already-documented tradeoff
(see Phase 24's own comment in `core/actor.c`), not a new bug; closing
it properly needs a dedicated read-only user-runtime-data region
separate from the kernel image, which is a bigger structural change
than this follow-on fix. Left open, flagged here rather than fixed
silently.

---

### Phase 28 — Real parallel execution: folding SMP into the actor scheduler, and a formal audit

The gap nobody currently owns, now WIDENED by the review's own point 7:
Phase 10 (Milestone 12) proved a second physical core can be woken and
runs genuinely independent, linked C code — but it was, and remains,
DELIBERATELY excluded from `core/actor.c`'s scheduler
(`hal/x86_64/smp.c`'s own top comment: "the AP never spawns, runs, or
touches a single actor"). Every actor today, regardless of how many
cores exist, still runs on the BSP alone, round-robin — and several
kernel modules (`core/storage.c`'s `scratch[]`, `core/loader.c`'s own
copy) are correct ONLY because of that single-core assumption, stated
in their own comments as "interrupts stay disabled for the whole
syscall, therefore only one caller exists." This phase makes that
assumption false; everything relying on it has to be found first.

- **A formal SMP audit, before any scheduler change**: classify every
  kernel global as CPU-local, actor-local, immutable, spinlock-
  protected, atomic, or intentionally shared — `core/storage.c`'s and
  `core/loader.c`'s static scratch buffers are the two already known to
  need one of the first three; there are certainly others not yet
  found by name.
- A per-core `current_actor` and run queue (today: one global
  scheduler, one global `current_actor` — Phase 2's own single-core
  assumption, never revisited).
- The AP needs its own preemption timer (today it only counts a busy
  loop, per `hal/x86_64/smp.c`'s own demo) and its own kernel stack per
  actor (Phase 6's per-actor-kernel-stack design, extended per-core).
- `hal_address_space_create()`/`hal_map_lapic_mmio()` need to work from
  actor context on EITHER core, not just BSP-before-scheduling
  (`hal/x86_64/paging.c`'s own current single-core assumption).
- Verification, per this project's own standing rule (§3.7): two
  actors doing independent CPU-bound work, with timestamps proving
  genuine overlap on two cores — not just correct interleaving on one
  — AND the audited globals deliberately raced against on purpose,
  confirmed NOT to corrupt, not just assumed safe by inspection.

*Philosophy: §1 ("parallel and distributed computation is the default
shape of work, not a bolted-on feature") — today it is, literally,
bolted on: a second core exists and sits nearly idle. This phase makes
the sentence true, safely.*

> **Status note (2026-09-20):** Phase 10's follow-up already did the first
> half of this phase's plumbing — per-core `current_actor` and scheduler
> loops, per-core TSS and preemption tick, `hal_address_space_create()`
> reachable from either core (LAPIC mapped everywhere), and a two-core
> parallelism test (the Cores app's `b`). What remains here is the *audit
> and the narrowing*: classify every global, then replace the big kernel
> lock with finer ones only where measurement says it matters, and prove
> the audited globals survive being raced on purpose.

### Phase 29 — An authenticated fabric: device identity before remote capabilities

Phase 12's networking (Milestones 13-15) is genuinely solid for what it
proves — device addressing, sequence numbers, ACK, retry, tested across
separate QEMU instances — but the ACK mechanism proves only "this MAC
sent an ACK for sequence N," never "this authenticated Vajra device
authorized this ACK." Phase 13's own "remote capability delegation that
cannot exceed local authority" has nothing to authenticate the remote
end AGAINST yet — don't build distributed authority on top of
unauthenticated MAC addresses.

- Device identity and authentication, a real authenticated channel
  between two Vajra instances, THEN remote actor identity (Phase 25's
  generation handles, extended across a device boundary) and only then
  capability delegation on top.
- This is explicitly a prerequisite for Phase 13's remote-capability
  sub-bullet specifically, not a blocker on Phase 13's local-only parts
  (multi-device pairing under one identity can still be scoped/designed
  in parallel).

*Philosophy: §3 invariant 4 (authority never increases at a boundary)
— that invariant is meaningless without first knowing WHO is on the
other side of the boundary.*

### Phase 30 — Self-hosting: VajraLang, a text editor, and a real actor heap, all running inside Vajra

The actual answer to "can Vajra develop Vajra from within Vajra,"
raised directly by the user this session. Three real, currently-missing
prerequisites, not one:

- **A real per-actor heap.** Today an actor gets one fixed, small
  private window (Phase 3) — no `malloc`-equivalent, no growth. A
  compiler (even VajraLang's small one) needs dynamic allocation for
  its token stream, AST, and generated output; this is likely a bump
  allocator over a few `alloc_dma_pages()`-backed pages per actor
  first, not a general kernel allocator redesign — start minimal, the
  same discipline every phase before this one has used.
- **A text editor as a loaded Vajra program** — Phase 19's own utility
  list already names this; it becomes load-bearing here, not optional,
  since there is otherwise no way to WRITE `.vj` source from inside a
  booted Vajra at all.
- **VajraLang's own front end ported to run AS a ring-3 actor** — reads
  source from a storage object (`SYS_OBJECT_READ`), writes a compiled,
  loader-valid `.bin` to a NEW storage object (`SYS_CREATE_NAME` +
  `SYS_WRITE_OBJECT`), using the heap above instead of a host process's
  memory. The codegen target stays a real question: emitting C and
  invoking a hosted clang isn't available inside Vajra at all (there is
  no C compiler runtime here to invoke) — this phase likely needs
  VajraLang's OWN backend to finally emit x86-64 machine code bytes
  directly, rather than continuing to lean on the host toolchain
  Phase 22 currently depends on. Phase 27's W^X work applies directly
  here: freshly-compiled code populates RW, then transitions to RX
  before ever running — never standing RWX, even for code this
  compiler wrote itself.
- Verification: write a NEW `.vj` program using Vajra's own editor,
  compile it with Vajra's own compiler, and run it — all inside one
  booted instance, zero host tool invocations after boot. This is the
  actual, checkable definition of "self-hosted," not a claim.

*Philosophy: §1's thesis applied to the toolchain itself — the OS
building the OS, not just running what was built for it elsewhere.*

### Phase 31 — Adversarial demo: deliberately hostile code, and proving the blast radius

Direct user request, and a clean fit — not a detour: §3.7 ("a guarantee
isn't real until it's been broken on purpose") and §3.5 ("small blast
radius by default") are asking for exactly this, and the existing
`Intruder` demo actor already rehearses the shape of it, cooperatively
(scripted to fail gracefully, not actually trying to win). This phase
is the CULMINATION of `hostile_ring3.c` — the standing test Phases
23-27 already built up item by item, not a fresh start — run as one
complete adversarial program, and, once Phase 30 exists, written and
compiled entirely FROM INSIDE Vajra rather than handed to the loader
from the host:

- A program, written adversarially (genuinely trying to escape, not
  scripted to demonstrate failure), that attempts: reading/corrupting
  another actor's memory across the isolation boundary (§3 invariant
  1), forging or manufacturing a capability it was never granted (§3
  invariant 3), escalating authority it doesn't hold (§3 invariant 2 —
  no ambient authority to find), spawning without bound (a fork-bomb
  shape against Phase 7's quota), and exceeding its storage/object
  quota.
- The MORE interesting version, worth building toward rather than
  settling for the obvious one: not a program caught by Phase 9/20's
  quarantine pipeline at install time (the "front door"), but one
  that's ALREADY running with whatever narrow capabilities it was
  legitimately granted, and still can't do damage outside them — proving
  the capability model is real defense in depth, not just a gate that
  can be walked around once past.
- Deliverable: a real, narratable demo — "here is code that tries to
  do X, here is the exact capability check that stops it, here is what
  it could still do (nothing outside its own slot and grants)" — the
  literal blast-radius claim, shown, not asserted.

*Philosophy: §3 invariants 1/2/3/5, §3.7 directly. Also `docs/
PHILOSOPHY.md` §5's own framing — this proves the mechanism the fabric
needs to be safe; it doesn't reposition Vajra as a security product.*

### Front-end-per-feature rule — Security Lab + Fabric — DONE (2026-09-20)

Standing user instruction: every backend feature ships with a desktop
app where a person can experience it. Applied retroactively to the
hardening track and the fabric:

- **Security Lab** (`actor_lab`, core/main.c): press 1-7 to attack the
  running kernel. 1 = write into kernel memory (Phase 23), 2 = write to
  own code (Phase 27 W^X), 3 = pass the kernel a pointer into itself
  (Phase 24), 4/5 = run an untrusted vs a trusted program (Phase 27),
  6/7 = kill / message the shell with no capability. Faulting attacks
  run in a disposable hostile child; the verdict comes from the
  kernel's own fault counter (`SYS_FAULT_COUNT`), not the child's word.
  All seven verified live (keys injected through the QEMU monitor):
  7 HELD, 0 BREACH, no panic. This is also the deterministic,
  repeatable demo Phase 31 (adversarial demo) builds on.
- **Fabric**: the network peer's own pane (its `[Net]` trace, plus a
  header saying whether a device exists). Interactive resend is not
  built yet; only the no-device state was verified locally.

Bugs the front end exposed (all fixed): two CAP_CONSOLE actors ate each
other's keystrokes (`SYS_KEY_READ` drained for unfocused callers);
console output raced across cores (global `current_window`, now
`begin/end_window`); `MAX_ACTORS` can't exceed 19 (`.bss` ceiling,
now checked at build time); Coordinator's Worker-2 terminate raced
its own exit once the round-robin lengthened.

### Phase 32 — Day-to-day usability: the same "front end first" pass, applied everywhere

Direct user priority, stated plainly: not another proof-of-concept
phase, an OS they actually sit down and use. This phase has no single
new mechanism — it's the discipline that already worked once
(`docs/DESKTOP_DESIGN.md`: a full visual mockup, reviewed and approved,
BEFORE the mouse driver or compositor were written) applied to
whatever's still rough:

- True overlapping, movable windows (Phase 18's own explicit scope
  cut) — more than one app visible and usable at once, not just
  maximized-and-switched.
- The calculator (Phase 22's own verification target) and every future
  VajraLang program as real, clickable desktop icons — not only
  reachable via the boot-time scripted demo, the way `calc.bin` is
  today.
- Files gains real per-object-kind actions (`docs/DESKTOP_DESIGN.md`
  §4's own "open" design, not yet built): opening a data object shows
  its content; opening a valid program object runs it.
- Mouse feel tuned for an actual pointer (today's fixed, unscaled
  `CELL_FRAC` multiplier is tuned against nothing — a real sensitivity
  divisor, checked against a real mouse, not a QEMU-monitor-injected
  one).
- Phase 20's install flow gets a front end, not just a syscall path.

Verification for this phase is different in kind from every phase
above it, deliberately: not "does it compile and boot," but "would a
person choose to keep using this" — checked by actually using it, the
same way correctness is checked by actually breaking it (§3.7).

*Philosophy: §6 (engineering discipline), extended: usability is
verified empirically, the same standard already held for correctness.*

---

## Explicit non-goals for the foreseeable future

The permanent statement of these now lives in `docs/PHILOSOPHY.md` §5
— this section is today's specific scope, checked against that
document, not a replacement for it:

- **No pixel graphics, no font rendering, no graphical rendering
  surface** — by explicit direction, not by omission. **Updated,
  Phase 18**: this is narrower than earlier phrasing of this bullet
  ("no GUI, no windowing, no desktop shell") claimed — a text-mode
  DESKTOP METAPHOR (icons, a taskbar, windows, a mouse) is in scope and
  built, per the user's own explicit redirect; what stays out of scope
  is rendering any of it as anything other than character cells and
  CP437 glyphs.
- **Not a security product** — the capability model exists to make
  Phase 12/13's fabric safe across devices of mixed trust, not as a
  goal competing with the fabric for priority. Phases 23-27 fix real,
  currently-shipped containment gaps first (an external code review
  confirmed several of §3's invariants are violated by the working
  tree today, not just untested) BECAUSE the fabric can't be safe on a
  foundation that isn't; Phase 31's adversarial demo then proves the
  mechanism — neither repositions the project. See
  `docs/PHILOSOPHY.md` §1 and §5.
- Not aiming at mass daily-driver deployment replacing
  Windows/Android/Linux, and not aiming to match their driver
  catalogs or hardware breadth (USB, GPU, Wi-Fi, a large ported
  application ecosystem) — a scale problem, not a design one, and
  explicitly not this project's measure of "complete." Confirmed
  against the user's own stated priorities (this session): actor
  messaging, real parallelism, true migration, self-hosting, and
  day-to-day usability — Phases 13/28/30/32, hardened by 23-27 first —
  not hardware/ecosystem breadth.
- Not binary-compatible with anything natively; running existing
  POSIX software is only ever through Phase 21's explicit, bounded
  compatibility shim, never a kernel-level goal.
- Not aiming at defense-grade or safety-certified use — that needs
  organizational certification/formal verification work that is a
  separate effort from kernel architecture. Phases 23-27 and Phase 31
  are concrete proofs of specific properties, not a certification
  claim.

The honest goal: a genuine, working exploration of the actor/
capability/message-passing model as the foundation for a real,
multi-device personal computing fabric (`docs/PHILOSOPHY.md` §1), with
real engineering discipline, usable day-to-day — a text-mode
environment (desktop metaphor included) rather than a text CONSOLE
specifically — on modest but real hardware targets.

## "What does 'complete' mean for Vajra" — the answer to Phases 23-32

Not feature parity with Linux or Windows (see the non-goals above,
`docs/PHILOSOPHY.md` §5) — a checklist against THEIR breadth would
measure the wrong thing entirely. Complete, for Vajra, means:

0. **Its containment claims are actually true, not assumed.** A CPL3
   fault kills the offending actor, never the kernel; a syscall pointer
   is checked, never blindly followed; a capability can't silently
   apply to the wrong target once a slot is reused; nothing leaks
   across run/exit; loaded code is trusted before it's executable, and
   never writable while it's executable (Phases 23-27) — the
   PREREQUISITE for every claim below meaning anything at all.
1. **The thesis is real, not demonstrated in miniature.** Actors
   genuinely run in parallel across real cores (Phase 28), not just
   time-sliced on one. An actor genuinely migrates to a different
   device and keeps running (Phase 13, on an authenticated channel —
   Phase 29), not just streams its display.
2. **It builds itself.** VajraLang, an editor, and the compiler all run
   AS Vajra actors, inside a booted instance, with no host machine in
   the loop after boot (Phase 30).
3. **Its central safety claim is shown, not asserted.** A deliberately
   hostile program, written from inside Vajra, is contained exactly the
   way the capability model promises (Phase 31) — the culmination of
   the standing `hostile_ring3.c` test Phases 23-27 built incrementally,
   not a single demo assembled at the end.
4. **A person would choose to use it.** Not "it boots and the demo
   passes" — real day-to-day use, with a front end worth sitting in
   front of (Phase 32), the same discipline already proven once for the
   desktop.

Five honest, checkable bars — each one either true of a running system
or not — rather than an open-ended breadth list that could never
finish and was never the point.
