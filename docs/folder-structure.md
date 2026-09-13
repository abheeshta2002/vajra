# Vajra C Project — Folder Structure

## The rule

- **`src/core/`** — portable C. The actual OS: scheduler, capabilities,
  message-passing, VFS. Must never contain anything that only makes
  sense on one specific CPU or board. If you're tempted to add an
  `#ifdef ARCH_X86` here, that code belongs in `hal/` instead.

- **`src/hal/<arch>/`** — hardware abstraction layer, one folder per
  CPU architecture (x86_64, aarch64, ...). Entry stub, context
  switching, exception/interrupt setup, paging, timer/interrupt
  controller code. Same architecture = same folder regardless of
  which board/device it's running on.

- **`src/boards/<board>/`** — board/firmware-specific boot chain and
  memory layout (linker script). Two boards can share the same CPU
  architecture but boot completely differently (BIOS PC vs. a Pi's
  GPU firmware vs. QEMU's generic ARM "virt" machine) — that
  difference lives here, not in `hal/`.

- **`src/include/vajra/`** — shared headers, including the HAL
  interface contract itself (the function signatures `core/` calls
  and every `hal/<arch>/` must implement).

- **`build/`** — 100% generated. Never hand-edit anything here, never
  commit it (see `.gitignore`). Safe to delete entirely at any time;
  the build script recreates it.

- **`tools/`** — build scripts (`build-c.ps1`) and future tooling
  (flashing scripts, etc.).

## Current state

Only `src/boards/pc-bios/` and `src/hal/x86_64/` exist with real
content — this is the x86-first phase. `src/hal/aarch64/` and
`src/boards/qemu-virt-arm/` are placeholders for when the ARM port
starts; nothing needs to move or be renamed to add them later.
