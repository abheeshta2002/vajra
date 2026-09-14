# Vajra C Rewrite — Milestone 8: Capabilities

## What this is

`actor_send()` now checks *authority*, not just existence. Knowing an
actor's slot index used to be enough to message it (Milestone 7); it
no longer is. This is the roadmap's Phase 6: a per-actor capability
table, kernel-granted at setup time, delegable at runtime, and never
forgeable — the point where philosophy §33's invariants 4–5 ("no
forging capabilities", "IPC is kernel-mediated") become something
actually checked on every send, not just a design intention.

Deliberately narrow in scope: `CAP_SEND` is the only capability
operation that exists, because there's no object/storage system yet
for any other operation ("read object X") to apply to. Adding one
later means adding a `CAP_*` constant, not redesigning how
capabilities are held or checked.

## What's included

- **`src/include/vajra/actor.h`** (updated) — `CAP_SEND`, and
  declarations for `actor_grant()` (kernel-only, unconditional —
  creates a capability from nothing, which is why it's not a syscall)
  and `actor_delegate()` (checks the caller already holds what it's
  trying to pass along). `MAX_ACTORS` raised 6→7 for the new demo
  actor.
- **`src/core/actor.c`** (updated) — a fixed-size per-actor capability
  table (`struct capability { op; target; }`), `actor_has_cap()`/
  `actor_add_cap()` internals, and `actor_send()` now requires
  `actor_has_cap(current_actor, CAP_SEND, dest)` before anything else.
- **`src/hal/x86_64/syscall.c`** / **`hal.h`** (updated) — `SYS_GRANT`,
  a thin pass-through to `actor_delegate()`.
- **`src/core/main.c`** (updated) — `actor_mailbox_receiver` now
  receives twice instead of once; `actor_mailbox_sender` delegates its
  capability to a new `actor_intruder` after using it; `actor_intruder`
  starts holding nothing and proves both halves of the boundary.

## Verified

Two failure modes a capability system must prevent, both deliberately
triggered rather than assumed fixed:

- **Using authority you don't have.** `actor_intruder` is never
  granted anything by `kernel_main` and attempts to message Receiver
  anyway, knowing its slot index perfectly well. Result: denied. Only
  after `actor_mailbox_sender` — which *was* granted `CAP_SEND` for
  Receiver at setup — voluntarily delegates a copy to Intruder does
  the identical second attempt succeed, and Receiver gets exactly the
  message sent (`sender=6`, Intruder's real slot, kernel-filled).
- **Manufacturing authority via delegation.** Intruder, still holding
  nothing at that point in the run, was temporarily made to attempt
  delegating `CAP_SEND` onward to a third party. Result: denied —
  `actor_delegate()` correctly refused to pass along authority the
  caller doesn't itself hold. Test code removed after confirming;
  absent from the shipped diff.
- All four Milestone 6 demo actors continue running unmodified
  alongside the capability demo.

## Known follow-ups for the next milestone

- **No revocation.** Once granted or delegated, a capability lives for
  the actor's lifetime — there's no way to take it back. Worth adding
  once something actually depends on revoking mid-run authority.
- **`actor_grant()` is the only bootstrap mechanism**, called from
  `kernel_main` before the scheduler starts (same constraint
  `actor_spawn()` already has). Actors granting *initial* capabilities
  to other actors they spawn (not just delegating what they hold)
  needs `SYS_SPAWN` first (still not implemented — Milestone 7's own
  follow-up, unchanged).
- Capabilities are checked but not yet the vehicle for large payloads
  or object references — still deferred to when an object/storage
  system exists (Milestone 7's own follow-up, unchanged).
- Console output still isn't preemption-safe (Milestone 4's follow-up,
  unchanged).
