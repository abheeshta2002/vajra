# Vajra — Session Context

A living state snapshot, not a design document — `docs/PHILOSOPHY.md`
(canonical principles) and `docs/ROADMAP.md` (the phased plan, checked
against philosophy) stay the source of truth for *what Vajra is*. This
file exists so a new session can reconstruct *where things stand*
without re-deriving it from conversation history. Read this, then
`docs/ROADMAP.md`, then `docs/PHILOSOPHY.md`, before touching anything.

Update this file (and only this file) at the end of a work session, or
whenever asked to checkpoint. Overwrite stale sections rather than
appending — this is current state, not a log (the git history and
`docs/MILESTONE*_CHANGELOG.md` files are the log).

## Current milestone

Milestone 18 (Phase 18: input/shell/desktop) is functionally complete
and superseded by a full desktop rewrite. Phase 22 (VajraLang) has a
working v0. The roadmap itself was just restructured (see below) —
no code for the new phases has been written yet.

## Current branch

`working` — pushed to `origin/working` after every commit this
session (standing instruction from the user: "push everything
everytime").

## What was completed (most recent session)

- Real desktop compositor (`hal/x86_64/console.c`): icons, taskbar,
  `[ Apps ]` launcher, start menu, title bar with close button. Apps:
  System Log, Shell, Files (live, regenerated from `core/storage.c` on
  focus), About. Each app keeps its own offscreen buffer
  (`alloc_dma_pages()`), not static `.bss`.
- PS/2 mouse driver (`hal/x86_64/mouse.c`, IRQ12) + click-to-focus
  hit-testing, single-click activation (no double-click timing).
- A **sixth** recurrence of this project's `.bss`-vs-fixed-address
  collision bug — fixed structurally this time: page tables, E820 map,
  AP trampoline/stack all moved below the kernel's own `0x20000` load
  address (space free since Milestone 13), not nudged again. See
  `src/boards/pc-bios/boot.asm`'s own "structural fix" comment.
- **VajraLang v0** (`tools/vajrac.ps1`): a real lexer/parser/AST/codegen
  compiler for a small calculator language (`let`, `print`,
  `+ - * /`, precedence, unary minus, variables), targeting C as its
  backend, going through the existing `clang`+`ld.lld`+
  `program_header` pipeline. Verified end-to-end: `calc.bin` runs as a
  real ring-3 actor and prints correct results.
- `docs/ROADMAP.md` restructured: Phases 23-27 (hardening) inserted
  ahead of real-SMP/self-hosting/adversarial-demo/usability (renumbered
  28/30/31/32), plus new Phase 29 (authenticated fabric). Driven by an
  external code review (ChatGPT) whose specific claims were verified
  directly against source before being written down — see "Known
  security issues" below.

## What was verified

- Full SMP (`-smp 2`) + entire scripted capability-security demo boots
  clean, serial trace captured, unaffected by any change above.
- Desktop click-to-focus verified via QEMU monitor-injected
  `mouse_move`/click sequences + screendumps (converted PPM→PNG,
  visually confirmed correct icon/tab hit-testing and app switching).
- `calc.bin`'s output (`14, 20, -6, 14, -42`) confirmed in a real serial
  boot trace, matching hand-computed expected values.

## Known bugs

- **Mouse sensitivity is too high** — `mouse.c`'s `CELL_FRAC` scaling
  has no sensitivity divisor, untuned against a real pointer (only
  tested via QEMU-monitor-injected synthetic motion). Planned fix:
  Phase 32.
- **A spurious first mouse "click" was observed during headless
  testing** (`buttons=7` on the very first packet after enabling PS/2
  reporting) — mitigated with a `mouse_seen_clean` guard in
  `console.c` (never treat the first-ever reading as a click edge), but
  the root cause (an 8042-handshake byte-alignment artifact, best
  guess) was not fully isolated. Worth re-examining if it recurs with
  real hardware input.
- Desktop apps are always maximized, not true overlapping/movable
  windows — a deliberate, labeled scope cut (Phase 18), not an
  oversight. Real windows are Phase 32.

## Known security issues (the reason Phases 23-27 exist — do not skip to 28+ before these)

Verified directly against source, file and line, this session:

1. `hal/x86_64/interrupts.c`'s `exception_handler()` takes no CS and
   treats **every** ring-3 fault as `KERNEL PANIC` — invariant 1
   (isolation is real) is false today for any ordinary bug, not just
   malware. **Highest priority fix — Phase 23.**
2. No `copy_from_user`/range validation anywhere in
   `hal/x86_64/syscall.c` — `SYS_WRITE` and others follow raw
   actor-controlled pointers unbounded. Phase 24.
3. `core/storage.c`'s `storage_read()` (line ~352) refuses only
   `OBJ_REJECTED`; `core/loader.c` never checks trust state at all —
   "executable" and "loadable" are the same concept today. Phase 27.
4. `hal/x86_64/paging.c` (line ~246): loaded-program pages are mapped
   `present|writable|user`, no NX bit — genuinely RWX. Phase 27.
5. `core/actor.c`'s `struct capability { int op; int target; }` (line
   ~138) — no generation counter; a capability can silently apply to a
   slot's new occupant after the old one dies and the slot is reused.
   Phase 25.
6. `core/loader.c`'s `loader_spawn_program()` (lines 70-81) leaks its
   `alloc_dma_pages()` allocation if `actor_spawn_program_child()`
   fails after allocation. The program page-table pool
   (`PROGRAM_POOL_SIZE`, `hal/x86_64/paging.c`) is documented as
   permanently never freed. Phase 26.

These are NOT hypothetical — `volatile int *p = (int*)0x12345678; *p =
42;` from any ring-3 actor already halts the whole machine, today.

## Current task

None in progress. Just finished restructuring `docs/ROADMAP.md`; no
kernel code has been touched since the VajraLang commit
(`75affd1`)/desktop commit (`61f5d33`).

## Next exact task

**Phase 23 — Fault containment.** Not started. Plan (from
`docs/ROADMAP.md`):
1. `isr_common` (`hal/x86_64/isr_stubs.asm`) passes the saved CS
   (already on the CPU's exception frame) as a 4th arg to
   `exception_handler()`.
2. `exception_handler()` (`hal/x86_64/interrupts.c`) branches on CS's
   CPL bits: CPL3 origin → terminate the offending actor (reuse the
   existing Phase 7 termination path), reap, `schedule_next()`, kernel
   continues. CPL0 origin → unchanged, still `KERNEL PANIC`.
3. Verify by deliberately triggering it: a ring-3 actor dereferencing
   an invalid pointer, confirmed the kernel survives and keeps
   scheduling other actors immediately after — this becomes the first
   entry in the standing `hostile_ring3.c` test the roadmap describes.

## Files currently being modified

None — working tree is clean as of the last push.

## Tests that must pass

- Full boot serial trace: SMP (`-smp 2`) bring-up message, the entire
  scripted capability-security demo (Coordinator/Scanner/Downloader/
  Intruder/Sender/Receiver/Namer), shell prompt, `calc.bin`'s five
  printed results — all present, in the same shape as every prior
  verified boot this session.
- Any NEW hardening fix (Phase 23 onward) must be proven by
  deliberately triggering the exact failure it prevents in QEMU, not
  by code review alone (`docs/PHILOSOPHY.md` §3.7 — standing project
  rule, not new for this file).

## Architectural decisions (do not relitigate without explicit user sign-off)

- Actor / capability / message-passing is the only computation model —
  `docs/PHILOSOPHY.md` §2, non-negotiable.
- HAL/core separation is real (`core/` has zero architecture-specific
  code) — checked by the (not-yet-built) AArch64 port compiling
  `core/` unchanged.
- Programs use a small custom loader format (`include/vajra/
  loader.h`), deliberately not ELF — matches `link.ld`'s own "no more
  machinery than needed yet" reasoning.
- Desktop is a text-mode METAPHOR (CP437 glyphs, icons, taskbar,
  windows) — explicitly authorized by the user this session, but pixel
  graphics / font rendering / a graphical surface remain out of scope
  per `docs/PHILOSOPHY.md` §5.
- VajraLang's v0 compiler runs on the HOST (PowerShell — this build
  machine's clang has no host C library configured), not inside Vajra.
  Self-hosting is Phase 30, not done.
- Roadmap non-goal, reconfirmed this session: NOT chasing Windows/
  Linux driver or ecosystem breadth (USB, GPU, Wi-Fi, a large ported
  app catalog) — `docs/PHILOSOPHY.md` §5 names this explicitly, and the
  user's own stated priorities (actor messaging, migration, parallel
  computing, self-hosting, usability) don't need it either.

## Things Claude must NOT change without asking first

- `docs/PHILOSOPHY.md`'s non-negotiable invariants (§3) — every phase
  gets checked against these, they don't get relaxed to make a phase
  easier.
- The non-goals in `docs/ROADMAP.md` / `docs/PHILOSOPHY.md` §5 (no
  pixel graphics, no Windows/Linux breadth chasing, no native binary
  compatibility) — these were reconfirmed, not just inherited, this
  session.
- Git safety: never force-push, never amend a pushed commit, never
  skip hooks — standard rules, unchanged.
- Don't skip Phases 23-27 to jump to something that looks more
  interesting (Phase 28 SMP, Phase 30 self-hosting) — the whole reason
  they were inserted is that those phases make an already-broken fault
  boundary worse, not better, if built first.
