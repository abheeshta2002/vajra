# Vajra C Rewrite — Milestone 6: Ring 3 & the Syscall Boundary

## What this is

Actor code now genuinely executes at CPL 3 (ring 3), not CPL 0. This
is the roadmap's Phase 4, and it's what turns Milestone 5's memory
isolation into a real security boundary rather than a convention:
until now, nothing stopped deliberately malicious code from loading a
different CR3, disabling interrupts, or calling any kernel function
directly — it just never did, because it never tried. At CPL 3 those
things are no longer possible *at all*, enforced by the CPU itself,
regardless of what the code asks for.

The only way from ring 3 back into the kernel is now a syscall
(`int 0x80`, vector 0x80, DPL=3). Every other kernel function is
unreachable from actor code by construction — it lives in
supervisor-only memory that ring-3 code cannot fetch instructions
from, let alone call into.

## What's included

- **`src/hal/x86_64/gdt.c`** (updated) — two new GDT descriptors
  (user code/data, DPL=3, selectors `0x3B`/`0x33`), and a real
  `TSS.RSP0`: `hal_set_kernel_stack()`/`hal_get_kernel_stack_top()`
  manage one dedicated ring-0 stack per actor slot. This turned out to
  be necessary, not optional — see "What this needed that a first pass
  didn't account for" below.
- **`src/hal/x86_64/usermode.asm`** (new) — `hal_enter_user_mode()`:
  the standard hand-built-iretq technique for dropping from ring 0 to
  ring 3 without an ELF loader.
- **`src/hal/x86_64/isr_stubs.asm` / `interrupts.c`** (updated) — a
  parallel `syscall_common` path (vector 0x80, DPL=3) alongside the
  existing exception/IRQ path: different enough (real arguments in
  RAX/RDI/RSI/RDX, a return value in RAX) that overloading
  `exception_handler`'s signature would have been the wrong fit. IDT
  grown to the full 256 entries to fit vector 128.
- **`src/hal/x86_64/syscall.c`** (new) — the kernel-side dispatcher:
  `SYS_WRITE`, `SYS_YIELD`, `SYS_EXIT`. Runs at CPL 0, so it calls
  `hal_console_write()`/`actor_yield()`/`actor_exit()` completely
  normally.
- **`src/hal/x86_64/syscall_invoke.c`** (new) — `hal_syscall()`, the
  user-side wrapper that executes `int 0x80`. The one function that
  has to sit on the ring-3 side of the boundary despite living in
  `hal/`, marked accordingly (see below).
- **`src/boards/pc-bios/link.ld`** (updated) — a new page-aligned
  `.user_text` output section for code (only code — see
  `paging.c`'s comment on why data doesn't need to move) that must be
  fetchable from CPL 3: actor entry points, and the small syscall/
  formatting wrappers they call.
- **`src/hal/x86_64/paging.c`** (updated) — marks `.user_text`'s pages
  user-accessible within the otherwise supervisor-only kernel commons.
- **`src/core/actor.c`** (updated) — `actor_trampoline()` now calls
  `hal_enter_user_mode()` instead of calling `entry()` directly;
  `schedule_next()` calls `hal_set_kernel_stack()` before every real
  switch.
- **`src/core/main.c`** (updated) — the demo actors' direct calls to
  `hal_console_write()`/`actor_yield()`/`actor_exit()` replaced with
  `user_write()`/`user_yield()`/`user_exit()`, thin `.user_text`
  wrappers around `hal_syscall()`. A new `user_write_dec64()` formats
  numbers entirely in ring 3 — no `SYS_WRITE_DEC` needed, since
  formatting is pure computation, not a privileged operation.

## What this needed that a first pass didn't account for

Two things only became clear from actually booting and observing, not
from writing the code:

- **A page-table hierarchy bug.** The first attempt marked `.user_text`
  and each actor's stack user-accessible at the leaf (PTE) level but
  left the PML4/PDPT/PD entries pointing to them at their old
  supervisor-only value. x86 permissions are the AND of every level
  walked, not just the final PTE — so every ring-3 fetch faulted with
  `#PF`, error code `0x5` (protection violation, user-mode access),
  even though the leaf entry alone looked correct. Fixed by carrying
  the user bit on every structural entry in the path, not just the
  leaf.
- **One shared kernel stack is not enough.** The first design used a
  single TSS.RSP0 for all actors, on the reasoning that only one actor
  runs at a time on this single core. That's true, but irrelevant:
  multiple actors can be *suspended mid-syscall* simultaneously (an
  actor that yielded is sitting inside its own syscall handler's call
  chain, waiting), and every ring3→ring0 transition starts fresh at
  TSS.RSP0 rather than continuing wherever a previous one left off —
  so a second actor's syscall would silently overwrite the first
  actor's still-live saved state. Fixed with one dedicated kernel
  stack per actor slot (`gdt.c`), swapped via `hal_set_kernel_stack()`
  on every scheduler switch, exactly parallel to how CR3 is already
  swapped per actor.

## Verified

- Booted and observed: all 4 actors (including `actor_greedy`, still
  never yielding) run to completion via syscalls from ring 3, byte-for-
  byte the same interleaving behavior as Milestone 5.
- The actual claim of this milestone — that actor code runs at genuine
  CPL 3, not just "in a function called ring 3" — was deliberately
  tested, not assumed: `actor_greedy` was temporarily made to execute
  `hlt` (a privileged instruction, unconditionally `#GP` above CPL 0)
  directly. Result: immediate `#GP` (vector `0xD`), confirming the CPU
  itself refuses the instruction regardless of what the code asked
  for. Test code removed after confirming; absent from the shipped
  diff.
- The page-table hierarchy bug above was also only found this way: the
  first boot attempt faulted reproducibly on the very first
  instruction fetch inside `actor_one`, with `CR2` and `RIP` both
  pointing at an address that was, by every other check, correctly
  inside the mapped `.user_text` range.

## Known follow-ups for the next milestone

- **No pointer validation on syscall arguments.** `SYS_WRITE`'s string
  pointer is trusted completely — the kernel dereferences whatever
  address the actor passes, with no length limit and no check that it
  actually points into that actor's own memory. Harmless today only
  because nothing hands actors a reason to pass a bad pointer on
  purpose; worth hardening before actors do anything less trivial than
  print fixed strings.
- **No `SYS_SPAWN` yet.** Actors still can't create other actors —
  `actor_spawn()` remains kernel-setup-only (see its own comment).
  Needed before ghost actors (roadmap Phase 6/7).
- Still no message passing, no capabilities, single core. Unchanged
  from Milestone 5's own follow-ups except where this milestone
  specifically addressed the privilege boundary.
