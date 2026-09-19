# Milestone 18 — Input devices & a real interactive shell

## What this is

Roadmap Phase 18, built at full scope (not split into a follow-up this time):
a PS/2 keyboard driver, a CMOS RTC driver, a text-mode escape-code renderer,
and a genuinely interactive shell actor that reads from the keyboard and
drives everything Phases 16-17 already built (the program loader, the
namespace) from real human input for the first time. Everything before this
milestone was either output-only (console) or block storage; this is the
kernel's first INPUT device.

## What's included

- **PS/2 keyboard driver** (`hal/x86_64/keyboard.c`), interrupt-driven
  (IRQ1 -> vector 33, `pic.c`/`interrupts.c`/`isr_stubs.asm`): scancode
  Set 1, US QWERTY, shift tracked. Non-blocking `hal_keyboard_poll()` /
  `SYS_KEY_READ` — the same bounded-poll-and-yield shape `SYS_NET_RECEIVE`
  already established, rather than a new "block until a key arrives"
  scheduler primitive for one device.
- **CMOS RTC driver** (`hal/x86_64/rtc.c`), polled on demand — `SYS_RTC_READ`.
- **A small ANSI/VT100-subset escape parser inside `hal_console_putchar()`**
  (`hal/x86_64/console.c`): clear+home, cursor position, 16-color SGR,
  backspace-as-cursor-back. Reachable through plain, still-UNGATED
  `SYS_WRITE` — no new syscall needed for rendering, and every pre-Phase-18
  actor's console output keeps working completely unchanged.
- **`CAP_CONSOLE`**, gating `SYS_KEY_READ` alone — deliberately NOT
  `SYS_WRITE` (see below). The shell is the only actor granted it.
- **A real interactive shell** (`core/main.c`'s `actor_shell()`): line
  editing with backspace, a colored prompt, and built-ins `help`, `ls`,
  `run <name>`, `echo`, `date`, `clear`, `count`, `pipe`, `jobs`, `stop`,
  `kill`, `exit`.
- **Job control's two-tier model, both tiers genuinely exercised**: `stop
  <slot>` sends an ordinary `MSG_PLEASE_STOP` message (tier 1 — the target
  may check it at its own safe points, or not); `kill <slot>` is
  `SYS_TERMINATE` (tier 2 — unconditional, capability-gated). `actor_slow_counter`
  (the `count` built-in's target) is a long-enough-running actor to make
  both demonstrable against something still alive, unlike `hello.bin`.
- **Pipes as ordinary mailboxes, not a new mechanism** (the `pipe`
  built-in): the shell spawns a sink then a source, delegates the
  `CAP_SEND`-to-sink capability its own spawn of sink already auto-granted
  it (plain `SYS_GRANT`, Phase 6), and sends the source an init message
  naming the sink's slot — the literal mechanism a real pipe is built
  from, using zero new syscalls.
- **Per-actor spawn quotas** (`core/actor.c`'s new `spawn_quota` field,
  `actor_set_spawn_quota()`): the shell alone gets a raised quota (6, vs.
  the ordinary default of 2) for its open-ended interactive spawning,
  without changing what quota means for every other actor.

## Real bugs found, verified by actually booting and typing, not assumed

1. **A hang from a `.bss`/fixed-address collision — the fourth time this
   exact bug class has hit this project.** The kernel image grew past
   `DIRECTORY_LBA=90` (Milestone 17's own on-disk directory sector) to
   47985 bytes, and separately `.bss` grew past the boot stack's fixed top
   (`0x88000`, `hal/x86_64/start.asm`) — confirmed via the established
   ELF-relink + `llvm-nm`/`llvm-size` technique (`__bss_end = 0x88c0c`,
   past the stack top by 3084 bytes), not guessed. Unlike the three
   earlier instances of this bug, shrinking the newly-added structures
   back down wasn't enough this time (~7KB overrun vs. ~1KB reclaimable
   from array sizes) — most of the growth was genuine new code/string
   size shifting where `.bss` STARTS, not one runaway buffer. Fixed by
   relocating the boot stack into the previously-unused 32KB gap between
   it and the page tables (`0x88000` -> `0x8F000`), giving real headroom
   for ordinary future growth instead of repeating a same-size fix that
   had already stopped scaling. `OBJECT_DATA_BASE_LBA`/`DIRECTORY_LBA`
   moved past `KERNEL_SECTORS=120`'s own hard read-cap for the same
   reason — clear of the boot loader's actual ceiling, not just today's
   kernel size.
2. **A real, correctness-affecting regression, caught by re-running the
   existing demo, not assumed safe.** An initial fix bumped
   `MAX_SPAWNS_PER_ACTOR` globally (2 -> 6) to give the shell room for its
   `count`/`pipe`/`run` built-ins — which broke Coordinator's own existing
   security demonstration (`main.c`), whose whole point is proving a 3rd
   spawn is denied at quota 2: the boot trace showed `SECURITY FAILURE:
   3rd spawn succeeded!` where it should show a denial. Fixed properly, not
   patched around: `spawn_quota` became a per-actor field (defaulted to
   `MAX_SPAWNS_PER_ACTOR`), with a new `actor_set_spawn_quota()` raising it
   for the shell alone. `MAX_SPAWNS_PER_ACTOR` itself stays at 2.
3. **Actor-slot exhaustion, the same resource-ceiling bug class in a
   different resource.** With `MAX_ACTORS` still at 16 and the shell now
   the first actor in this project's history that's permanently alive for
   an entire session (blocked in its own keyboard-read loop, never
   exiting), it silently used up the one slot of slack the rest of the
   scripted demo's dynamic spawns depended on — a real `Scanner: spawn
   failed!` in the boot trace, not assumed. Fixed by raising `MAX_ACTORS`
   by exactly 1, to 17 (+16KB `.bss`, comfortably inside the headroom fix
   #1 just freed), with the sizing rationale spelled out in `actor.h`'s own
   comment rather than picked arbitrarily.
4. **A real ring-3 `.rodata` dereference — the exact pitfall
   `user_write_trust()`'s own comment already documented, hit again in
   new code.** The shell's first command-dispatch helper,
   `shell_str_eq(cmd, "help")`, directly compared bytes of a string
   LITERAL from `.user_text` (ring-3) code. `.rodata` is supervisor-only,
   same as `.text`, except `.user_text` — confirmed by an actual `#PF`
   (CPL3, error code 0x5) triggered by literally typing `help` into a
   running shell via QEMU's monitor `sendkey`, not by inspection. Fixed
   by replacing it with `shell_cmd_is()`, which compares against
   individually-passed `char` PARAMETERS (compiled to immediates at the
   call site, never a `.rodata` blob) instead of a string literal —
   the same "avoid a lookup table, use scalar immediates" technique
   `user_write_trust()`'s if/else chain already established, generalized
   to string comparison.

## Verified, not assumed

- A clean single-instance boot with all four fixes applied, the full
  pre-existing 13-actor scripted demo still producing identical, correct
  output (including Coordinator's 3rd-spawn-denied proof), and the shell's
  colored prompt appearing and then genuinely blocking on keyboard input.
- **Real keystrokes, injected via QEMU's monitor `sendkey` (not typed by a
  human, but real hardware-level scancode injection through QEMU's own
  emulated PS/2 controller, not a simulation) against a running instance**:
  `help` echoed and printed the command list; `date` printed the actual
  live CMOS clock value (matched the real wall-clock date); `ls` listed
  the namespace with correct, up-to-date trust states; `count` spawned a
  real background job that ran to completion, visible in `jobs` while
  alive; `kill` was exercised against an already-dead slot and correctly
  reported denial rather than crashing.
- This is the first milestone in the project verified this way — by
  actually driving the running kernel's own input device, not just
  reading its serial output.

## Known follow-ups for the next phase

- No true foreground-blocking job control (`run`'s prompt returns
  immediately; there's no "wait for this job to finish" primitive) —
  would need a way for a spawned actor to learn its spawner's slot
  without an initial message (loaded programs have no such message,
  unlike kernel-linked ghost actors), deliberately scoped out rather than
  built in a rush.
- The escape-sequence parser's state spans multiple `hal_console_putchar()`
  calls but the console lock is still only held per-character — safe today
  because only the shell emits escape codes, not safe once more than one
  actor drives the screen concurrently (see `console.c`'s own comment).
- Still no filesystem path hierarchy (Phase 17's own known scope) — `run
  <name>`/`ls` work over the flat namespace only.
- `.bss` headroom after this milestone's fix is real but not unlimited —
  see `actor.h`'s own sizing note before raising `MAX_ACTORS` again
  without re-measuring.
