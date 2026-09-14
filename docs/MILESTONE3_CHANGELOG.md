# Vajra C Rewrite — Milestone 3: Actors & Cooperative Scheduler

## What this is

The first genuine design piece beyond boot infrastructure: real
multitasking. Scoped deliberately as **cooperative** scheduling first
(actors voluntarily yield via `actor_yield()`), not timer-driven
preemption — lower risk than jumping straight to interrupt-driven
context switching, and it builds on the exact same primitive a future
preemptive scheduler would need, so nothing here gets thrown away
when that's added.

## What's included

- **`src/hal/x86_64/context_switch.asm`** (new) — the one genuinely
  architecture-specific piece: a minimal cooperative context switch
  (save callee-saved registers, swap stack pointers, `ret` into
  whatever was suspended there). Classic, well-understood technique.
- **`src/core/actor.c`** (new) — the actual scheduler: actor table,
  round-robin policy, spawn/yield/exit. 100% portable — it only calls
  `hal_context_switch()`, so this exact file should work unchanged on
  a future AArch64 port once that HAL function exists there.
- **`src/include/vajra/actor.h`** (new) — the portable interface.
- **`src/core/main.c`** (updated) — spawns three demo actors that each
  print a few messages with a yield in between, proving interleaving
  actually happens.

## Verified

- Booted cleanly with **correct interleaved output**: actors 2, 3, 1
  (in that round-robin order — actor 2 goes first as a natural
  consequence of how "current actor" starts at slot 0 internally, not
  a bug) each completed exactly 3 ticks in consistent rotation, then
  the scheduler correctly detected no runnable actors left and halted
  gracefully with a clear message — rather than crashing or hanging.
- Checked the exception log for the full run: zero exceptions fired,
  confirming the hand-built initial stack frames (the trickiest part
  of this milestone — constructing a fake "previously suspended"
  context for an actor that's never actually run) are laid out
  correctly on the first attempt.
- This demo is being **kept in the shipped code**, not stripped after
  verification — same reasoning as the assembly kernel's V0.30
  `sysinfo` command staying permanently: it demonstrates a real,
  working feature and doesn't destabilize anything, unlike the
  deliberate-crash tests used to verify Milestones 1 and 2.

## Known follow-ups for the next milestone

- Still cooperative, not preemptive — a genuinely misbehaving actor
  that never yields would stall the whole system. Timer-driven
  preemption (reusing this same `hal_context_switch` primitive,
  triggered from the timer interrupt handler instead of a direct
  call) is the natural next step.
- Actor stacks are a single 4KB page with no guard page yet — the
  direct equivalent of the assembly kernel's V0.33 guarded-stacks work
  is a reasonable target once actors do enough that overflow becomes
  a real risk.
- No message-passing or capabilities yet — actors currently can't
  communicate with each other at all. That's the next major design
  piece after preemption.
