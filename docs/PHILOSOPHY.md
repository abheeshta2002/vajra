# Vajra Philosophy

This supersedes every earlier philosophy discussion (including
`architecture.md`'s original sketch and the unnumbered "§N" citations
scattered through the early roadmap and milestone changelogs, kept
there only as historical record of what motivated each already-shipped
phase). This is the one canonical statement, kept short on purpose —
if a design decision can't be checked against this document in under a
minute, the document has failed at its job.

## 1. What Vajra is

Vajra is a small number of your own devices — phone, laptop, whatever
else — behaving like one connected fabric of computation instead of
several unrelated boxes. A program's *display* and a program's
*execution* are not the same thing and don't have to live on the same
device. Parallel and distributed computation is the default shape of
work, not a bolted-on feature: everything is an isolated **actor**
talking to other actors through **messages**, whether those actors are
on the same core, a different core, or a different device entirely —
the caller never needs to know which.

**This is the thesis. Everything else in this document exists to make
that thesis safe and real, not to compete with it.**

## 2. The model, in one paragraph each

- **Actors.** The only unit of computation. Each one has its own
  memory, runs independently, and can be created and destroyed cheaply
  (a "ghost actor" — spawned for one narrow task, gone when it's
  done). Nothing runs outside an actor, including the kernel's own
  setup code.
- **Messages.** The only way actors interact. Bounded mailboxes,
  explicit backpressure (a full mailbox rejects a send outright — it
  never silently drops one or blocks the sender forever), and a
  message's sender identity is filled in by the kernel, never
  self-reported. No shared memory between actors, ever — not even as
  an optimization.
- **Capabilities.** The only source of authority. An actor starts with
  nothing. It can act on something — message another actor, read an
  object, spawn a child, cross a network boundary — only if it holds
  an explicit, kernel-issued capability naming that exact operation
  and target. Knowing something exists (a slot number, an object id, a
  device's address) is never itself authority to act on it.

## 3. Non-negotiable invariants

Every phase, forever, gets checked against these. A phase that can't
satisfy them isn't scoped wrong — it's the wrong phase.

1. **Isolation is real, not conventional.** One actor cannot read,
   write, or execute another's memory, by construction — not by a
   check that could be bypassed, by the memory simply not being
   addressable.
2. **No ambient authority.** Nothing is ever "trusted because of who's
   running it" (no root, no uid=0 special case). Authority is always
   an explicit, specific, revocable-in-principle grant.
3. **Capability soundness.** An actor can never use authority it
   wasn't given, and can never delegate authority it doesn't hold —
   capabilities can be passed along, never manufactured.
4. **Authority never increases at a boundary.** Crossing a core, a
   process-equivalent, or a network link to another device can only
   preserve or narrow what an actor is allowed to do — never widen it.
   This is what makes the fabric (§4) safe to use across devices you
   don't all trust equally.
5. **Small blast radius by default.** New, risky, or untrusted work
   (a downloaded file, a freshly installed program, an inspection
   task) runs in its own narrowly-capable actor, not inline in
   something already trusted.
6. **The kernel is small and mediates everything, but isn't the
   policy.** It enforces capability checks and isolation; it does not
   decide what any particular actor *should* be allowed to do — that's
   set once, deliberately, at the point a capability is granted.
7. **A guarantee isn't real until it's been broken on purpose.** Every
   invariant above gets verified by deliberately triggering the exact
   failure it exists to prevent, not by reading the code and assuming
   it holds. This has been true since Milestone 1 and does not change.

## 4. The fabric, concretely

"Your program follows you across devices" means one of two different
things, and they're not equally hard:

- **Remote display / follow-me session.** Execution stays put;
  whichever device you're looking at just streams frames and forwards
  input, the same as any other actor sending messages, just carried
  over the network instead of locally. This is the near-term,
  realistic shape of the experience.
- **True migration.** The actor itself — its memory, its capability
  table, its identity — moves to the new device and keeps running
  there natively. This is the long-term ambition, genuinely hard, and
  exactly where invariant 4 matters most: the device receiving a
  migrated actor must never end up with more authority than that actor
  actually needs there.

The closest real precedent is Plan 9 (Bell Labs): a terminal, compute,
and storage as different machines, made transparent through one
uniform way of addressing a resource whether it's local or remote.
Vajra's own message-passing is meant to do the same thing: `send(actor,
message)` looks identical whether the destination is a neighboring
core or a device across the room.

## 5. What Vajra deliberately is not

- **Not a security product.** Security isn't the destination — it's
  the reason the fabric in §4 can span devices you don't fully trust
  without either trusting them completely or not using them at all.
  Capability-worthiness is checked against "does this make the fabric
  safer to use," not pursued for its own sake.
- **Not a Windows/Linux competitor on breadth.** No attempt to match
  their driver catalogs, their software ecosystems, or their raw
  hardware support. That's a decades-and-thousands-of-contributors
  problem, not a design problem, and chasing it would prove nothing
  about the actor/capability model this project exists to explore.
- **Not a GUI.** Text console only, by explicit choice, for as long as
  this document says so. A graphical surface is a separate,
  not-yet-scoped decision, not an implied "eventually" of anything
  above.
- **Not binary-compatible with anything**, natively. Running existing
  POSIX/Win32 software is possible only through an explicit,
  bounded compatibility shim (its own scoped project, not a kernel
  goal) that translates at the boundary — the ported code never gains
  authority the capability model wouldn't otherwise grant it.

## 6. Engineering discipline

Unchanged since Milestone 1, because it has worked for twelve
milestones straight:

- One architectural step at a time. Never jump ahead of the current
  phase because a later one seems more interesting.
- Every milestone boots and is observed in QEMU, not just compiled.
- A fix isn't proven until the failure it prevents was deliberately
  triggered (§3.7) — the success path alone proves nothing.
- Prototype shortcuts are labeled as shortcuts the moment they're
  written, never discovered later as an accidental design.
- HAL/core separation stays real: `core/` never contains
  architecture-specific code, checked by the AArch64 port actually
  compiling `core/` unchanged.

## 7. Non-goals

Carried forward, unchanged in substance: no mass daily-driver
deployment claim, no defense-grade/certified-safety claim, no driver
breadth beyond what the fabric and a CLI environment actually need.
See `docs/ROADMAP.md`'s own non-goals section for the current, more
detailed list — this document states the permanent principle; the
roadmap states today's specific scope.
