# Design: a mouse-driven text-mode desktop

Not yet built. This is a design document to review and redirect before any
code is written — see the conversation that produced it: the two-pane
console (Milestone 18's own fix) solved "the shell is invisible," but the
actual ask is bigger — a real desktop: a mouse cursor, clickable icons for
objects in storage, windows you can open, a file viewer. Still text-mode
only (character cells, CP437 box-drawing/block glyphs, no pixel graphics,
no `docs/PHILOSOPHY.md` §5 violation) — the historical precedent is Norton
Commander / MS-DOS Shell / Borland's Turbo Vision, not a windowing GUI
toolkit.

## What has to exist that doesn't yet

1. **A mouse driver** — no pointing device has ever been read by this
   kernel. PS/2 mouse, IRQ12, a different wire protocol from the keyboard
   (3-byte relative-motion packets, not scancodes).
2. **A real window manager** — `console.c`'s two panes are FIXED (exactly
   two, exact position, never move, never close). A desktop needs an
   open-ended-ish set of windows that can be created, given content,
   moved, and closed.
3. **Desktop icons** — a visual representation of each object in Phase
   17's namespace, positioned on screen, clickable.
4. **An "open" action per object kind** — a data object opens into a text
   viewer; a program object launches (Phase 16's loader), with its own
   window for output.
5. **Hit-testing and click semantics** — translating "mouse is at column
   X, row Y, left button transitioned down" into "this icon was clicked,"
   including double-click detection (time between two clicks in the same
   spot).

## 1. Mouse driver (`hal/x86_64/mouse.c`)

- PS/2 auxiliary device, enabled via the 8042 controller (`0x64`
  command port: `0xA8` enables the aux port, a command-byte read/modify/
  write turns on its IRQ, then `0xD4` prefix + `0xF4` to the mouse itself
  turns on data reporting) — meaningfully more setup than the keyboard,
  which QEMU/real firmware already leaves enabled.
- IRQ12 → vector 44 (`0x2C`, on the SLAVE PIC — `hal_pic_remap()`'s mask
  byte for PIC2 needs IRQ12 unmasked too, and `hal_pic_send_eoi()` already
  handles the slave-PIC EOI case for irq ≥ 8).
- Standard 3-byte packets: byte 0 (button/sign/overflow flags), byte 1
  (ΔX), byte 2 (ΔY, sign-inverted vs. screen coordinates). The driver
  accumulates these into a fixed-point cell position (`x256`, `y256` —
  sub-cell precision so movement feels smooth despite 80×25 being coarse),
  clamped to the screen, and exposes the CURRENT absolute position +
  button state, not raw deltas — a desktop needs "where is the cursor
  now," not "how far did it move."
- `hal_mouse_poll(int *col, int *row, int *buttons)` — non-blocking, same
  shape as `hal_keyboard_poll()`.
- New syscall `SYS_MOUSE_READ`, gated behind the SAME `CAP_CONSOLE` that
  already gates `SYS_KEY_READ` — both are "owns the interactive session"
  authority, not two separate resources.

**Verification problem to solve up front**: under `-display none`
(every headless boot this project has ever used for CI/scripted
verification), nothing ever generates real mouse events — there is no
windowing system on the host feeding QEMU pointer motion. Keyboard input
was verified this same way it'll have to be verified again: QEMU's
monitor has `mouse_move <dx> <dy>` / `mouse_button <mask>` HMP commands,
the mouse equivalent of the `sendkey` trick already used for Phase 18 —
inject real synthetic packets through QEMU's own emulated PS/2 mouse,
not a simulation, then screendump and look.

## 2. Generalizing the window model

`console.c` currently has exactly two hardcoded `struct console_window`
entries. This becomes:

```c
#define MAX_WINDOWS 6   /* screen is 80x25 -- there is only so much room */
struct window {
    int in_use;
    int x, y, w, h;          /* screen position + size, INCLUDING border */
    char title[24];
    uint16_t *buf;           /* own offscreen char+attr buffer, w*h cells --
                                 content is drawn into THIS, not VGA_BASE
                                 directly */
    int cx, cy;               /* local cursor within the buffer, for
                                 actors that stream text into it */
    int owner_actor;          /* which actor's SYS_WRITE targets this --
                                 -1 for none (e.g. the desktop background
                                 itself isn't "owned" by one actor) */
};
```

A **compositor pass** (run after any change — a write, a move, an open/
close, a mouse redraw tick) blits every `in_use` window's buffer onto
`VGA_BASE` in some z-order, draws its border + title, then draws the
mouse cursor glyph on top of everything, saving whatever cell was
underneath first so the NEXT compositor pass can restore it before
redrawing elsewhere — the standard software-cursor technique text-mode
programs have always used, since VGA text mode has no hardware sprite
separate from the character grid.

This is a genuine rewrite of `console.c`'s rendering path (from "every
`hal_console_putchar()` call writes straight to `VGA_BASE`" to "writes go
into a window's own buffer; a separate compositor pass decides what
`VGA_BASE` actually shows") — bigger than Milestone 18's own two-pane
patch, and needs its own careful verification (screendump, same as
Milestone 18's fix used) before anything is layered on top of it.

Window creation stays kernel-mediated, same convention as
`actor_set_window()`/`actor_set_spawn_quota()`: a new `CAP_CREATE_WINDOW`
(blanket, like `CAP_SPAWN`) gates a `SYS_WINDOW_CREATE` syscall, held only
by whichever actor plays "desktop" (either the existing shell actor,
repurposed, or a new dedicated `actor_desktop()` — open question, see
below). Auto-grant pattern reused again: creating a window for a NEWLY
SPAWNED program (see §4) hands that program's own actor slot ownership of
the window it's about to write into, the same "creator gets natural
authority over what it created" shape `SYS_SPAWN`'s own auto-grant
already establishes.

## 3. Desktop icons

Not a general file-manager grid yet — a simple, fixed-cell layout: one
icon per LIVE object in the namespace (`storage_get_by_index()`, already
built in Phase 17), positioned in reading order across a grid (e.g. every
12 columns, 3 rows apart), each rendered as a small glyph + truncated
name + a trust-state color (reusing the existing `OBJ_*` → color mapping
philosophy already established for `ls`'s trust-state text, just as color
now instead of a word).

```
struct icon {
    int object_id;
    int col, row;   /* top-left of this icon's 2-3 cell footprint */
};
```

Rebuilt each time the desktop redraws from whatever `storage_get_by_index()`
currently reports — no separate icon state to keep in sync, the namespace
IS the source of truth, matching Phase 17's own "id is public knowledge"
design constraint (an icon existing is not itself an authority grant).

## 4. Opening something

Double-click hit-tests the click position against each icon's footprint
(a released time within e.g. 400ms of the previous release, same spot →
double-click; otherwise single-click, which for this design just
highlights/selects, no other effect yet). On a double-click:

- **If the object is a valid loadable program** (`include/vajra/
  loader.h`'s magic, checked the same way `loader_spawn_program()`
  already validates it): `SYS_WINDOW_CREATE` a new window, then
  `SYS_SPAWN_PROGRAM` with that window id — the spawned actor's own
  output lands in its own window instead of the shared Log pane. Needs
  either a new syscall variant or an extra argument on the existing one;
  exact shape TBD when this stage is actually built.
- **Otherwise** (plain data): `SYS_WINDOW_CREATE` a small text-viewer
  window, `SYS_OBJECT_READ` the bytes (requires the desktop to hold
  `CAP_READ_OBJECT` for that id — same capability-gated read as
  everything else; opening an icon you have no read authority for fails
  exactly the way any other unauthorized read already does, which is
  correct, not a bug to work around), and write the content into the new
  window. Closing is a clickable "×" in the window's own border region,
  hit-tested the same way icons are.

## Open questions (need a decision before or during implementation)

- **Does the shell BECOME the desktop actor, or is `actor_desktop()` a
  new, separate one?** The shell already holds `CAP_CONSOLE`; a clean
  option is the shell process both keyboard commands AND mouse/icon
  events in one loop, so there's still exactly one actor with
  interactive-session authority. A separate desktop actor would need its
  own `CAP_CONSOLE` delegation story.
- **How much of `MAX_ACTORS`/`MAX_WINDOWS`/capability-table headroom does
  this cost**, and does it repeat the `.bss`-vs-fixed-address collision
  this project has now hit four times? Needs the same measure-first
  discipline (ELF relink + `llvm-nm`) before assuming it fits, not after.
- **Icon layout on an 80×25 screen** is genuinely cramped once there are
  more than ~6-8 objects — fine for this demo's object count (currently
  4-5), a real scroll/pagination story is future work, not solved here.

## Proposed build order

1. **DONE.** Mouse driver + a visible, movable cursor glyph over the
   EXISTING two-pane layout (no new windows yet) — smallest possible
   slice that proves real mouse input works at all, verified via QEMU
   monitor `mouse_move`/`mouse_button` + a screendump. Built as: `hal/
   x86_64/mouse.c` (8042 aux-port enable sequence, IRQ12 → vector 44,
   3-byte packet parsing into a clamped absolute cell position),
   `SYS_MOUSE_READ` (gated by the same `CAP_CONSOLE` `SYS_KEY_READ`
   already uses, exactly as proposed above), and `hal_console_draw_
   cursor()` in `console.c` (save-cell/restore-cell software cursor,
   drawn from the syscall handler on every successful poll — no
   separate compositor tick needed yet since there's still only the
   two fixed panes). The shell actor polls the mouse once per
   keystroke-loop iteration, alongside its existing keyboard poll — no
   click handling yet, this stage only proves the pointer itself is
   real. Verified: a real `mouse_move`/`mouse_button` sequence injected
   via the QEMU monitor produced a visible cursor glyph on a screendump,
   correctly clamped and composited without disturbing either pane's
   text.
2. Generalize `console.c` to the windowed-buffer + compositor model
   described in §2, with the Log and Shell panes as the only two windows
   that exist — a pure refactor, verified by confirming the screen looks
   IDENTICAL to Milestone 18's fix before adding anything new.
3. Desktop icons + click/double-click hit-testing (§3), no "open" action
   yet — just visible, selectable icons.
4. "Open" action (§4) — the text viewer first (simpler, no window-per-
   spawned-actor plumbing needed), then program launch into its own
   window.
5. Later, not part of this design's first pass: dragging windows, closing
   via a clickable ×, multiple simultaneously open windows, focus/z-order
   polish.

Each stage gets its own real verification (screendump, and for input,
injected QEMU monitor events) before the next one starts — the same
discipline `docs/ROADMAP.md`'s own rules already require project-wide,
applied here because this is exactly the kind of multi-piece build that's
produced real, only-caught-by-booting bugs every other time it was
attempted at full scope in one pass this session.
