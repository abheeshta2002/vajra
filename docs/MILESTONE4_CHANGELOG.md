# Vajra C Rewrite — Milestone 4: Preemptive Scheduling

## What this is

Timer-driven preemption on top of the exact same `hal_context_switch`
primitive M3's cooperative scheduler already used. A misbehaving actor
that never calls `actor_yield()` can no longer stall the whole system
— the explicitly named follow-up from M3.

Also folds in the foundation-hardening work from the roadmap's
Phase 1 (guard pages, a dedicated double-fault stack, a real physical
allocator fix, `free_page()`), which landed first since preemption
depends on the IDT/GDT groundwork it put in place.

## What's included

- **`src/hal/x86_64/pic.c`** (new) — remaps the 8259 PIC's IRQ0-15
  from their power-on default (0x08-0x0F, colliding with CPU exception
  vectors) to 0x20-0x2F, and masks everything except IRQ0. Sends EOI.
- **`src/hal/x86_64/timer.c`** (new) — configures the PIT's channel 0
  to fire IRQ0 at a given frequency (100Hz). Purely a hardware tick
  source; knows nothing about scheduling.
- **`src/hal/x86_64/interrupts.c`** (updated) — IDT grown to 48
  entries to cover the remapped IRQ range; vector 32 (timer) now
  dispatches to `actor_yield()` directly instead of the panic path.
- **`src/hal/x86_64/isr_stubs.asm`** (updated) — one line
  (`ISR_NOERR 32`) reusing the existing exception-stub macro, since a
  hardware IRQ with no CPU-pushed error code has the identical stack
  shape.
- **`src/core/actor.c`** (updated) — preemption means
  `schedule_next()`'s state mutations can now happen at any point
  actor code is running, not just at explicit yield/exit call sites.
  Every entry point that touches `actors[]`/`current_actor` now
  disables interrupts for its critical section and re-enables them
  once execution genuinely resumes. A new `actor_trampoline()`
  handles the one case that doesn't resume through the normal path: a
  freshly spawned actor's fake frame `ret`s straight into application
  code, so the trampoline exists purely to re-enable interrupts there
  before running the actor's real entry point.
- **`src/core/main.c`** (updated) — added a fourth demo actor,
  `actor_greedy`, that never calls `actor_yield()` at all.

## Verified

- Booted and observed (not just compiled) via serial-mirrored console
  output: actors 1-3 fully interleave and complete all 3 ticks each
  — entirely *during* `actor_greedy`'s busy-loop, before its first
  spin even finishes. Under the old purely cooperative scheduler this
  is impossible: once given the CPU, a never-yielding actor would keep
  it forever, and actors 1-3 would never print anything at all. This
  is the failure case Phase 2 exists to prevent, deliberately
  triggered and confirmed fixed, not just assumed from the code
  reading correct.
- After actors 1-3 exit, `actor_greedy` continues alone, repeatedly
  preempted and resumed via `schedule_next()`'s self-switch no-op path
  (added for a different reason during Phase 1, now exercised for
  real under actual preemption for the first time). No crash, no
  corruption, all 5 spins complete, clean exit, scheduler halts with
  "no runnable actors left."
- Checked the boundary explicitly: a brand-new actor that has never
  run before starts with interrupts enabled (via `actor_trampoline`),
  confirmed by `actor_one`/`two`/`three` (spawned first) all
  successfully receiving timer ticks and completing normally.

## Known follow-ups for the next milestone

- **Console output isn't preemption-safe.** One verification run
  showed two actors' `hal_console_write()` calls interleaved at the
  character level (cursor state and VGA writes have no locking). This
  is a real, expected consequence of adding preemption without adding
  any synchronization to shared kernel-side I/O — it's a console bug,
  not a scheduling bug: the actors' own execution and state remained
  entirely correct throughout. Worth a lock (even a simple
  disable-interrupts-around-the-write one) before console output is
  relied on for anything beyond demos.
- Still only one core. `legacy-asm` already has a multicore trampoline
  and per-core actor state to draw on when that milestone comes.
- Still one flat, shared identity-mapped address space — no memory
  protection between actors yet. This is the load-bearing milestone
  the roadmap's Phase 3 (virtual memory / per-actor address spaces) is
  for.
