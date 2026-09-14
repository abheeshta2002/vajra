# Vajra C Rewrite — Milestone 9: Ghost Actors & the Actor Lifecycle

## What this is

Actors can now create actors, at runtime, not just at boot. This is
the roadmap's Phase 7 — the "ghost actor" pattern from philosophy §6:
a short-lived worker, created for one narrowly scoped task, given only
the authority spawning it automatically confers, doing its job, and
gone. `SYS_SPAWN` and `SYS_TERMINATE` are the whole surface, both
capability-checked (`CAP_SPAWN`, `CAP_TERMINATE`) and, for spawning,
quota-limited — not every actor should be able to spawn arbitrarily,
and none should be able to fork-bomb the system.

## What's included

- **`src/include/vajra/actor.h`** (updated) — `CAP_SPAWN`,
  `CAP_TERMINATE`; `actor_spawn()` split into the raw, unconditional
  primitive (kernel-setup-only, as before) and `actor_spawn_child()`
  (capability-checked, quota-limited, what `SYS_SPAWN` actually calls);
  `actor_terminate()`. `MAX_ACTORS` raised 7→10.
- **`src/core/actor.c`** (updated) — `actor_spawn_child()` requires
  `CAP_SPAWN` and a spawn count under `MAX_SPAWNS_PER_ACTOR` (2), then
  auto-grants the natural parent/child relationship a worker needs:
  the spawner gets `CAP_SEND`+`CAP_TERMINATE` for the child, the child
  gets `CAP_SEND` back to its spawner — no separate grant dance per
  worker. `actor_terminate()` forcibly ends another actor (never the
  caller itself — `SYS_EXIT` is for that), reusing the existing
  DEAD-state + `reap_dead_actors()` cleanup path unchanged.
- **`src/hal/x86_64/syscall.c`** / **`hal.h`** (updated) —
  `SYS_SPAWN`/`SYS_TERMINATE`, thin pass-throughs.
- **`src/core/main.c`** (updated) — `actor_worker` (a ghost actor:
  waits for one task, replies, exits itself) and `actor_coordinator`
  (spawns two workers, demonstrating both halves of cleanup — one
  self-exits after replying, one is forcibly terminated — then a
  3rd spawn attempt to prove the quota). `actor_intruder` extended
  with two more denied attempts (spawn, terminate).

## Two real bugs, both found before they could bite in the field

- **Capability leakage across a reused slot.** `actor_spawn()` reuses
  a DEAD actor's slot for a new one, but never cleared that slot's
  capability table or spawn count — both were only zeroed once, at
  boot, by `scheduler_init()`. A freshly spawned actor would have
  silently inherited whatever authority the slot's *previous*
  occupant still held. Caught by re-reading the spawn path while
  adding quota tracking, not by a failing boot — the existing demo
  never happened to reuse a slot in a way that would have exposed it.
  Fixed by clearing both on every spawn, not just at boot.
- **`alloc_page()`'s zeroing step assumed the wrong address space.**
  This one *did* surface immediately on first boot: `actor_worker`'s
  spawn page-faulted (`#PF`, error code 2 — supervisor-mode write,
  not-present) the moment `actor_spawn()` tried to zero its freshly
  allocated stack. Root cause: `alloc_page()` used to run exclusively
  during `kernel_main`'s setup, under the boot CR3, which identity-
  maps everything — so writing to a brand-new page always just
  worked. Now that spawning can happen at runtime, from inside a
  syscall, the CR3 actually active is the *calling* actor's own
  deliberately restricted one, which never maps a page that doesn't
  exist yet for a child that hasn't been created. Fixed with a new
  HAL primitive, `hal_zero_page()` (`hal/x86_64/paging.c`): it
  temporarily switches to the always-complete boot CR3 for the one
  write, then switches back to whatever was active — transparent to
  everything else, since interrupts stay disabled for the whole
  syscall this happens inside of.

## Verified

Three failure modes, all now permanently built into the demo itself
rather than needing separate temporary test code, because the
ghost-actor story naturally exercises all of them:

- **Spawning without authority.** `actor_intruder` holds no
  `CAP_SPAWN` and attempts `SYS_SPAWN` anyway, knowing
  `actor_worker`'s address perfectly well. Denied.
- **Terminating without authority.** Same actor, no `CAP_TERMINATE`,
  attempts to end Receiver. Denied.
- **Exceeding the spawn quota.** `actor_coordinator`, which *does*
  hold `CAP_SPAWN`, successfully spawns two workers (its quota), then
  a third attempt is refused outright — not queued, not silently
  capped, refused.
- The legitimate path end-to-end: Worker 1 receives a task, computes,
  replies, and self-terminates; Worker 2 receives a task, and is
  forcibly ended by Coordinator instead, independent of whether it had
  already replied.
- All previous milestones' demos (preemption, isolation, ring 3,
  mailboxes, capability delegation) continue running unmodified
  alongside the new one.

## Known follow-ups for the next milestone

- **No revocation, still** (Milestone 8's follow-up, unchanged) — a
  spawned worker's granted capabilities live for its lifetime.
- **`SYS_SPAWN` only instantiates pre-linked `.user_text` functions.**
  There's still no loader; a "ghost actor" here is a known worker
  chosen from the kernel image, not arbitrary code. Matches the
  project's own incremental discipline (§40) rather than a shortcut.
- **Quota is a flat constant (2), not configurable per actor.** Fine
  for a demo; a real policy would probably vary it by role.
- Console output still isn't preemption-safe (Milestone 4's follow-up,
  unchanged).
