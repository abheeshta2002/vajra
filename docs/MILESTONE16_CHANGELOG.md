# Vajra C Rewrite — Milestone 16: A Real Program Loader

## What this is

Roadmap Phase 16, the biggest gap between this kernel and "install and
run new software." Every actor before this milestone was a pre-linked
C function baked into the kernel image at build time — `SYS_SPAWN`
took a function pointer the kernel already knew about, not a path to a
program. There was no loader, no binary format, no concept of code
that didn't exist when the kernel was compiled. This milestone closes
that gap, deliberately scoped the same way earlier phases were: a real
loader and format, not a shortcut, but not full ELF either — more
machinery than this kernel's actual needs justify yet, the same
reasoning `link.ld` already applies to `kernel.bin` itself.

## What's included

- **`include/vajra/loader.h`** (new) — the loadable executable format:
  a 12-byte header (`magic`, `entry_offset`, `code_size`) prepended to
  raw compiled bytes. `PROGRAM_VBASE` (256MB) and `PROGRAM_WINDOW_MAX`
  (2MB) are the shared constants `paging.c`, `actor.c`, and `loader.c`
  all agree on for where and how much loaded-program memory a slot
  gets.
- **`hal/x86_64/paging.c`** (updated) — `hal_address_space_map_program()`:
  a second, separate private window per actor at `PROGRAM_VBASE`, on
  top of the existing 1MB-2MB stack window Phase 3 already built. A
  new window, not an enlargement of the old one — every actor that
  predates this milestone keeps working exactly as it did. Backed by a
  small shared pool of page tables (`PROGRAM_POOL_SIZE = 2`), not one
  reserved per actor slot — see "Real bugs found" below for why that
  mattered.
- **`core/loader.c`** (new) — `loader_spawn_program()`: reads a storage
  object, validates the header, copies the code into freshly allocated
  pages, and spawns an actor whose entry point is inside the *loaded*
  code. Has no idea capabilities or actors exist beyond calling
  `actor_spawn_program_child()` — same layering discipline as
  `core/storage.c` and `core/net.c` before it.
- **`core/actor.c`** / **`include/vajra/actor.h`** (updated) —
  `actor_spawn_program()` (raw primitive, mirrors `actor_spawn()`) and
  `actor_spawn_program_child()` (capability-checked: `CAP_SPAWN` +
  spawn quota + the same parent/child auto-grant, mirrors
  `actor_spawn_child()`).
- **`SYS_SPAWN_PROGRAM`** (`hal.h`/`syscall.c`) — `a1` = storage object
  id. `CAP_SPAWN`+quota checked inside `actor_spawn_program_child()`
  itself; `CAP_READ_OBJECT` for that specific object checked here,
  since loading a program requires being authorized to read the bytes
  that will actually execute, not just spawn authority in general.
- **`src/userland/`** (new) — the minimal userland runtime
  (`runtime.c`/`.h`: a small, documented syscall ABI real programs
  link against, replacing the ad hoc `user_write()`-style wrappers
  hand-written per demo actor in `core/main.c`) and `hello.c`, the
  actual "hello world" — its own genuinely separate link
  (`program.ld`, fixed at `PROGRAM_VBASE`), never part of `kernel.bin`'s
  own C sources.
- **`tools/build-c.ps1`** (updated) — builds `hello.c`+`runtime.c` as
  their own program, prepends the program header itself (the linker
  has no way to know its own final size to embed it), and wraps the
  result as an `incbin` blob (`hal/x86_64/hello_blob.asm`) — the exact
  same technique `ap_trampoline_blob.asm` already established.
- **`core/main.c`** (updated) — seeds the embedded `hello.bin` bytes
  into a storage object at boot, and a new demo actor
  (`actor_program_loader`) calls `SYS_SPAWN_PROGRAM` on it through the
  real syscall path, proving the loader works by the loaded program's
  own `user_write()` call actually reaching the console — not by
  extending the existing demo to fake the same effect.

## Real bugs found

Four, all found by actually booting the thing, not by review — and all
the same underlying class this project has hit before: kernel `.bss`
growth silently colliding with a fixed low-memory address something
else depends on (Milestone 13's own changelog already documented this
exact failure mode once, for the AP trampoline), or the
actor-private-window-vs-commons mistake (Milestone 14's DMA buffers).
Both recurred here, in new code:

1. **`MAX_ACTORS` 16→24, to fit a 13th fixed demo actor, triple-faulted.**
   `CR2` landed exactly on `BOOT_PDPT_PHYS_BASE` — `.bss` growth (each
   actor slot now costs 5 page tables, one more than before, for the
   new program window) pushed `__bss_end` past the fixed low addresses
   `boot.asm`'s own page tables live at. Reverted: the extra actor
   never actually needed more slots, just headroom that turned out to
   be dangerous rather than free.
2. **Bumping object storage to 32KB** (`SECTORS_PER_OBJECT`, for
   headroom the actual `hello.bin` — 251 bytes — never needed) pushed
   `__bss_end` past that same `0x90000` boundary again, confirmed by
   the identical triple fault signature. Reverted to 4 sectors (2KB,
   Milestone 14's original size); real headroom without repeating the
   mistake.
3. **A full page table per actor slot for the new program window**
   (`as_pt1[MAX_ACTORS][512]`, 64KB of `.bss`) pushed `__bss_end` past
   the BSP's own boot stack top (`start.asm`'s `RSP=0x88000`) — a
   silent **hang**, not a fault: the boot stack's first pushes were
   overwriting live kernel `.bss` out from under itself, confirmed
   reproducible (not a one-off) before being diagnosed by re-linking
   as ELF and inspecting `__bss_end` directly with `llvm-nm`/`llvm-size`
   rather than guessing further. Fixed by a small shared pool of page
   tables (2 entries) instead of one reserved per actor slot — the
   loader only ever needs as many as programs actually in flight at
   once, not one per possible actor.
4. **`core/loader.c`'s own page copy used `alloc_pages_contig()`
   instead of `alloc_dma_pages()`** — the exact same mistake
   `hal/x86_64/virtio_net.c`'s DMA buffers made once before (Milestone
   14's own changelog): this code runs inside a syscall with the
   *calling* actor's own restricted CR3 still active, and
   `alloc_pages_contig()` can return a physical page inside the 1MB-2MB
   actor-private window, present only in whichever actor happens to
   own that exact page. Confirmed by a real `#PF` (present+supervisor
   write, `CR2` inside that window) the first time the loader actually
   ran. Fixed by switching to `alloc_dma_pages()`, which searches from
   the commons region (2MB+) instead.

None of these four were caught by a clean compile — each one only
showed up the first time the actual new code path genuinely ran,
matching this project's own long-standing pattern: a guarantee (or in
this case, a memory layout assumption) isn't real until it's been
exercised for real.

## Known follow-ups for the next phase

- **No relocation** — a loaded program links at a fixed address
  (`PROGRAM_VBASE`) and only one program's worth of code fits per
  actor's own window; there's no support yet for a program that needs
  to be positioned differently or for multiple independently-loaded
  modules.
- **`PROGRAM_POOL_SIZE = 2` and no reclamation** — the pool entry a
  dead actor's loaded program used is never freed for reuse. Harmless
  for this milestone's demo (loads exactly one program), a real limit
  once a later milestone needs to load and unload programs repeatedly.
- **No filesystem namespace yet** — programs are seeded into the
  object store directly at boot (the same `incbin`-then-`storage_write()`
  technique the AP trampoline uses), not installed or discovered by
  name. That's Phase 17's own job.
