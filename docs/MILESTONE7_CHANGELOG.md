# Vajra C Rewrite — Milestone 7: Message Passing

## What this is

Actors can now talk to each other. This is the roadmap's Phase 5:
each actor gets a bounded mailbox instead of unrestricted shared
memory (there isn't any to share, post-Milestone-5, but the point
stands architecturally — see the project's message-passing philosophy
§1). `actor_send()`/`actor_receive()` and the matching
`SYS_SEND`/`SYS_RECEIVE` syscalls are the whole surface.

Deliberately not yet capability-mediated — any actor can send to any
other actor slot, addressed by plain index. Message-passing philosophy
§3 already names capability-mediated sending as the correct eventual
design; this milestone is the honest step before that exists, not a
skipped one. Roadmap Phase 6 is capabilities.

## What's included

- **`src/include/vajra/actor.h`** (updated) — `struct message` (type,
  kernel-filled sender, one word of inline data — no reference/object
  field yet, since there's no object system to reference into; large
  payloads are explicitly deferred to when one exists, not stuffed
  into this struct), and the `actor_send()`/`actor_receive()`
  declarations. `MAX_ACTORS` raised from 4 to 6 for the two new demo
  actors.
- **`src/core/actor.c`** (updated) — a new `ACTOR_BLOCKED` state
  (skipped by `schedule_next()`'s round-robin scan, exactly like
  `ACTOR_DEAD` already was — no scheduler changes needed beyond adding
  the state itself); a fixed-capacity ring buffer per actor;
  `actor_send()` (rejects a full mailbox rather than blocking the
  sender or dropping the message — message-passing philosophy §16)
  and `actor_receive()` (blocks by setting `ACTOR_BLOCKED` and calling
  the existing `schedule_next()`, reusing the exact same suspend/
  resume machinery every other actor state transition already uses).
- **`src/hal/x86_64/syscall.c`** (updated) — `SYS_SEND`/`SYS_RECEIVE`
  cases, thin pass-throughs to the `core/actor.c` functions.
- **`src/core/main.c`** (updated) — two new demo actors,
  `actor_mailbox_receiver` and `actor_mailbox_sender`, alongside the
  existing four.

## Verified

- Booted and observed: `actor_mailbox_receiver` calls `user_receive()`
  as its very first action, with an empty mailbox, and correctly
  blocks; `actor_mailbox_sender` ticks a few times, then sends;
  `actor_mailbox_receiver` wakes and prints the exact message sent
  (`type=42 sender=5 data=1234`) — `sender=5` matching the sender's
  real slot index, confirmed kernel-filled rather than trusted from
  the caller. This is deliberately the harder of the two possible
  orderings (receive-before-send, not send-before-receive): if
  `actor_send()`'s wake-on-full-mailbox logic were broken, this actor
  would simply never run again.
- Backpressure was also deliberately triggered, not assumed: sender
  temporarily made to send well past `MAILBOX_CAPACITY` (8) into a
  mailbox nothing was draining. Result: sends succeeded until the
  mailbox filled, then were rejected outright — never silently
  dropped, and the sender was never itself blocked waiting for room.
  Test code removed after confirming.
- All four Milestone 6 demo actors (including `actor_greedy`, still
  never yielding) continue running correctly, unmodified, alongside
  the two new ones.

## A recurring bug class, caught the same way twice now

Booting this milestone's larger `MAX_ACTORS` (4→6) reintroduced,
almost exactly, the bug Milestone 1 first hit and fixed: growing
`.bss` (per-actor static tables in `paging.c`/`gdt.c` got bigger)
pushed its end address past another fixed low-memory structure —
this time the E820 map at `0x20000`, not the page tables at the old
`0x8000`. `start.asm`'s `.bss`-zeroing loop wiped it before
`memory_init()` ever read it, silently falling back to a conservative
16MB instead of the real ~127MB — caught only by noticing the boot log
said something different than every previous milestone's, not by
reading any changed code (`boot.asm`/`e820.c`/`memory.c` were
untouched by this milestone's actual changes). Fixed by moving the
E820 map to `0x94000`, past the page tables at `0x90000-0x93000`, the
same relocation strategy Milestone 1 used for the page tables
themselves.

The general lesson, worth carrying into every future milestone that
adds static kernel state: **any fixed low-memory address is at risk
the moment `.bss` grows past it**, and `.bss` growth is easy to miss
precisely because it costs nothing on disk. `hal_get_memory_map()`
returning a suspiciously round fallback number, silently, is the
signature to watch for.

## Known follow-ups for the next milestone

- **No capability check on `actor_send()`.** Any actor can message any
  other by slot index. Roadmap Phase 6 (capabilities) is what fixes
  this — message-passing philosophy §3/§4 already describe the
  target design.
- **No object/reference system**, so `struct message`'s payload is
  necessarily inline and tiny (one word). Larger payloads wait for
  that system to exist, per message-passing philosophy §12/§17.
- **No `SYS_SPAWN`** — actors still can't create other actors
  (`actor_spawn()` remains kernel-setup-only). Needed before ghost
  actors (roadmap Phase 6/7).
- Console output still isn't preemption-safe (Milestone 4's own
  follow-up, unchanged).
