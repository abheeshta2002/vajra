# Vajra C Rewrite — Milestone 2: Physical Memory Manager

## What this is

The C equivalent of the assembly kernel's V0.30: a real physical
memory manager built from the machine's actual reported RAM (via
E820), replacing any hardcoded assumption about memory size.

## What's included

- **`src/hal/x86_64/e820.c`** (new) — reads the E820 map `boot.asm`
  already leaves at a fixed address (this code was already present
  from the assembly project's V0.30 and needed no changes) and
  translates it into the portable `hal_memory_region` format defined
  in `hal.h`. This is the only file that knows the E820 format itself.
- **`src/core/memory.c`** (new) — the actual allocator: a bitmap where
  every page defaults to "unusable," and only pages inside a genuine
  usable region (from whatever `hal_get_memory_map()` returns) get
  marked free. 100% portable — it never touches E820 directly, so
  this exact file should work unchanged once a future `hal/aarch64/`
  exists and implements the same function via a device tree instead.
- **`src/include/vajra/memory.h`** (new) — the portable interface:
  `memory_init()`, `alloc_page()`, `memory_get_total_bytes()`.
- **`src/hal/x86_64/start.asm`** and **`link.ld`** (updated) — added
  proper `.bss` zeroing at startup, caught as a real structural gap
  before it caused a confusing bug: C assumes uninitialized static/
  global variables start at zero, but nothing was actually zeroing
  that memory (it isn't part of the loaded flat binary at all). Fixed
  before `memory.c`'s static bitmap array made the gap matter.
- **`src/core/main.c`** (updated) — now initializes the memory manager
  and reports detected RAM in MB.

## Verified

- Detected RAM tracks the actual machine, not a hardcoded number —
  tested exactly like the assembly project's V0.30: default QEMU RAM
  → 127 MB, `-m 64` → 63 MB, `-m 256` → 255 MB. All three numbers
  match the assembly kernel's results exactly (the ~1MB gap in each
  case is the BIOS's own reserved region, correctly excluded both
  times).
- `alloc_page()` proven directly: temporarily allocated 5 pages and
  printed their addresses — came back sequential and page-aligned
  (`0x100000`, `0x101000`, `0x102000`, `0x103000`, `0x104000`),
  confirming the bitmap allocator correctly finds and marks free
  pages. Test code removed before shipping; confirmed absent by grep.
- Regression-checked the `.bss` fix in isolation before adding any new
  static data, confirming Milestone 1's console/exception handling
  still worked unchanged.

## Known follow-ups for the next milestone

- No `free_page()` yet — matches the assembly kernel's own design at
  this stage (a pure bump allocator with a free bitmap, not general
  reuse). Worth adding once something actually needs to free memory.
- The 256MB safety cap is unchanged from the assembly kernel's choice
  — still a deliberate, easily-lifted limit, not a structural ceiling.
- Next up: actors and the scheduler — the first real design piece
  beyond boot infrastructure.
