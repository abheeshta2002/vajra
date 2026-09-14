# Vajra C Rewrite — Milestone 5: Per-Actor Address Spaces

## What this is

Real memory isolation between actors: each actor now gets its own
x86-64 address space (its own CR3), whose only actor-specific mapping
is that actor's own stack. No actor's page tables contain any other
actor's memory at all — not "an unmapped guard page next to it," but
genuinely absent, anywhere in that address space. This is the
roadmap's Phase 3, described there as load-bearing: almost nothing
about real isolation is possible before it.

Not full isolation yet: everything still runs at CPL 0 (ring 0), so
there's no hardware privilege boundary stopping deliberately malicious
code from loading a different CR3 itself. What this milestone actually
delivers is a real boundary against everything else — bugs, wild
pointers, stack overflows, anything that isn't a CR3 instruction typed
on purpose. Ring 3 (roadmap Phase 4) is what turns this into a
boundary enforced even against deliberate misbehavior.

## What's included

- **`src/hal/x86_64/paging.c`** (rewritten) — `hal_address_space_create()`
  builds a complete address space per actor slot: the kernel commons
  (0-1MB and 2MB-256MB, matching `core/memory.c`'s `MEM_BASE`) identity-
  mapped and supervisor-only in every address space, plus exactly one
  actor's own pages mapped present/writable/user-accessible (the user
  bit means nothing yet at CPL 0, but is already the right bit for
  ring 3 later) in the 1MB-2MB window where actor memory lives. This
  fully replaces Milestone 4's guard-page mechanism (`hal_unmap_page`),
  which is now redundant: a stack overflow of any size faults
  immediately once every other address in that whole window is simply
  not present, not just the one page directly below the stack.
- **`src/hal/x86_64/context_switch.asm`** (updated) — `hal_context_switch`
  takes a third argument, `new_cr3`, and switches it in the same
  instruction sequence as the stack pointer, so "which stack" and
  "which address space" can never disagree.
- **`src/core/actor.c`** (updated) — each `struct actor` carries its own
  `cr3`; `actor_spawn()` calls `hal_address_space_create()` once per
  actor instead of arranging a guard page.
- **`src/core/memory.c`** (updated) — `free_page()`/`alloc_page()`'s free
  list moved from an intrusive linked list stored inside the freed
  pages themselves to a plain array in kernel commons. The old design
  broke the moment address spaces stopped being universal: freeing one
  actor's page from within a *different* actor's currently-active
  (and now deliberately restricted) address space can't write into
  memory that address space doesn't map.

## Verified

- Booted and observed: all 4 actors (including `actor_greedy`, still
  never calling `actor_yield()`) run to completion exactly as in
  Milestone 4, now under per-actor address spaces.
- The actual failure this milestone exists to prevent was deliberately
  triggered, not just assumed fixed: `actor_greedy` was temporarily
  made to write directly to `0x100000` — `actor_one`'s known private
  stack address (it's always spawned first, and `alloc_page()`'s first
  page is always `MEM_BASE`). Result: an immediate `#PF`, error code
  `0x2` (write, not-present, supervisor), `CR2 = 0x100000` — exactly
  the address it tried to touch. Confirmed the page is genuinely
  absent from `actor_greedy`'s tables, not just conventionally off-
  limits. Test code removed after confirming; absent from the shipped
  diff.
- That test also incidentally confirmed something not originally
  planned: `actor_one` had already exited (and its stack been reaped)
  long before `actor_greedy` reached the test, yet the fault still
  reproduced exactly as designed. An address space's mapping is fixed
  at spawn time and doesn't change with another actor's lifecycle —
  worth knowing going into Phase 6 (ghost actors), where address
  spaces will need to be built and torn down at runtime, not just once
  at boot.
- The cross-address-space free bug (see `memory.c` above) was itself
  only found by booting and observing, not by reasoning about the code
  in the abstract: the first attempt at this milestone paged-faulted
  reproducibly (`CR2` pointing at exactly the stack page
  `reap_dead_actors()` was freeing) the moment the third actor
  finished, before the fix was made.

## Known follow-ups for the next milestone

- **Actor memory is hard-limited to the 1MB-2MB window.** Fine for a
  handful of 4KB stacks; a real limit `hal_address_space_create()`
  will refuse once actors need more. Revisit alongside per-actor
  heaps/larger stacks.
- **Address spaces are built once at spawn and never rebuilt.** Fine
  while `actor_spawn()` only ever runs once at boot (see its own
  comment); ghost actors (roadmap Phase 6/7) will need
  `hal_address_space_create()`'s per-slot storage made properly
  reusable at runtime, and reclaimed page-table storage, not just
  reclaimed stack memory.
- Still ring 0 / one core / no message passing yet — unchanged from
  Milestone 4's own follow-ups except where this milestone specifically
  addressed memory isolation.
