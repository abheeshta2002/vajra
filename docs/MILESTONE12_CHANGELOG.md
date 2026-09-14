# Vajra C Rewrite — Milestone 12: SMP Bring-Up

## What this is

Roadmap Phase 10, deliberately scoped to its hardest, most novel, most
bug-prone piece: waking a second physical CPU core from cold and
proving it genuinely, independently executes in parallel with the
BSP. This is the first time in the C rewrite that more than one
instruction pointer has ever existed in the machine at once —
everything before this milestone, including every preemption/isolation
guarantee from Phases 2-9, only ever had to hold up against a single
core interleaving work over time, never two cores truly running at the
same instant.

Deliberately NOT included: folding the AP into the actor/capability
scheduler. `core/actor.c`'s `actors[]`/`current_actor`/
`schedule_next()` remain entirely BSP-only — the AP never spawns, runs,
or touches a single actor in this milestone. That's real, explicitly
deferred work (per-core run queues, a per-core `current_actor`, the
AP's own preemption timer, extending `hal_address_space_create()` so
actor-context code could reach the LAPIC too), split out the same way
Phase 3 (per-actor address spaces) shipped before Phase 4 (ring 3)
rather than both at once. See `docs/ROADMAP.md`'s own Phase 10 entry
for what's now explicitly next.

## What's included

- **`src/hal/x86_64/ap_trampoline.asm`** (new) — a standalone, flat
  16-bit real-mode binary, assembled separately (`nasm -f bin`, like
  `boot.asm`) since it has to run from a fixed low physical address in
  real mode before anything resembling the kernel's own addressing
  exists on that core. Transitions 16-bit real mode → 32-bit protected
  mode → 64-bit long mode using the SAME already-built page tables the
  BSP's own boot.asm constructed (`CR3=0x90000`) — no redundant second
  identity map. Hands off into the real, linked kernel image through a
  fixed "mailbox" memory cell (`0x70FF8`) the BSP populates before
  ever triggering the wake-up, since the trampoline itself is a
  position-independent blob with no visibility into the kernel's own
  symbol table.
- **`src/hal/x86_64/ap_trampoline_blob.asm`** (new) — wraps the built
  `ap_trampoline.bin` as inert `.rodata` (`incbin`) inside the normal
  kernel image, so `smp.c` can copy it to its real runtime home
  (physical `0x70000`) with an ordinary loop.
- **`src/hal/x86_64/apic.c`** (new) — local APIC driver: per-core
  enable (`hal_lapic_enable`), reading this core's own APIC ID
  (`hal_lapic_id`), and the INIT-then-SIPI wake-up sequence
  (`hal_lapic_send_init_sipi`), reusing the exact ICR magic values
  `legacy-asm/kernel/kernel.asm`'s own proven `setup_smp` used.
- **`src/hal/x86_64/smp.c`** (new) — orchestration: `hal_smp_boot_ap()`
  maps the LAPIC's MMIO page, copies the trampoline into place, writes
  the entry-point mailbox cell, sends the wake-up IPIs, and waits
  (bounded — never hangs forever) for the AP to report itself alive.
  `ap_entry_c()` is the first C code the AP ever runs: loads its own
  TSS/GDTR/IDTR, enables its own local APIC, signals "alive", then
  runs a small demo proving genuine parallelism (see Verified below).
- **`src/hal/x86_64/paging.c`** (updated) — `hal_map_lapic_mmio()`
  adds one new mapping (physical `0xFEE00000`, the local APIC's fixed
  MMIO address, far outside boot.asm's own 0-256MB identity map) to
  the BOOT page tables specifically — not every per-actor address
  space, since every place this milestone touches the LAPIC runs under
  `CR3=boot PML4` the entire time (see file's own comment for why, and
  what a future milestone letting actor-context code reach the LAPIC
  would need to add).
- **`src/hal/x86_64/gdt.c`** (updated) — a second, genuinely distinct
  TSS (`tss_ap`) and GDT descriptor (selector `0x40`) for the AP:
  `ltr`-ing the SAME descriptor from two cores at once would fault
  (#GP) on the second, already-"busy" load. `hal_gdt_load_ap()` is the
  AP-side counterpart to `hal_gdt_init()`.
- **`src/hal/x86_64/interrupts.c`** (updated) — `hal_idt_load_ap()`:
  the IDT's *contents* are ordinary kernel commons, already visible to
  every core, but IDTR itself is per-core state that resets
  independently on each one — skipping this would leave the AP one
  exception away from a silent triple fault instead of this kernel's
  own diagnostic panic screen.
- **`src/include/vajra/spinlock.h`** (new) — this kernel's first
  genuine cross-core synchronization primitive: a `lock cmpxchg`-based
  test-and-set spinlock (`hal_spin_lock`/`hal_spin_unlock`, via clang's
  freestanding `__sync_*` builtins). Every critical section before this
  milestone got away with just disabling interrupts, because "no
  interrupts" was equivalent to "nothing else can run" on the single
  core that existed — an equivalence that breaks the instant a second
  core exists. `core/actor.c`'s own scheduler state does NOT use this
  yet (see "Deliberately NOT included" above).
- **`src/hal/x86_64/console.c`** (updated) — the actual bug fix (see
  below): `hal_console_putchar()` now holds `console_lock` for its
  whole body.
- **`src/include/vajra/hal.h`** / **`src/core/main.c`** (updated) — new
  SMP declarations; banner bumped to Milestone 12; `kernel_main()`
  calls `hal_smp_boot_ap()` right after the GDT/IDT are ready and
  reports whether an AP responded.
- **`tools/build-c.ps1`** (updated) — assembles the trampoline as its
  own flat binary before the `incbin` wrapper that needs it to already
  exist, and adds `apic.c`/`smp.c` to the normal C source list.

## A real, deliberately triggered bug: the console race SMP newly enables

`hal/x86_64/console.c` has carried a known, explicitly-documented gap
since Milestone 4: no locking around `cursor_x`/`cursor_y`. It stayed
harmless for eight milestones because every interrupt/syscall gate is
an x86 "interrupt gate" (hardware-clears IF on entry), so on a single
core a console write could never actually be interrupted by another
console write — there was only ever one instruction pointer in the
whole machine to begin with.

Booted deliberately WITHOUT the fix first, with the AP printing its own
status lines while the BSP's entire actor demo ran at the same
wall-clock time: the output came back with characters from both
sources genuinely torn together mid-word —
`[[IAP core ntruder] attempt: promoting the paylo1ad object with]
counted to  no 30000000 while the BSP's actor demo ran` — not two
intact, merely-adjacent lines, but real cursor-state corruption (note
the double space in "to  no", a lost/duplicated character, not just
visual interleaving).

Fixed by wrapping `hal_console_putchar()`'s whole body in the new
spinlock. Rebooted and re-verified: extracting just the AP's message's
characters and just the affected BSP actor's message's characters from
the resulting (still visually interleaved, by design — see below) log
reproduces BOTH messages exactly, byte-for-byte, in the correct order,
with nothing lost, duplicated, or substituted. The lock is deliberately
placed around each character, not each whole `hal_console_write()`
call (`hal_console_write_hex64()`/`_dec64()` each loop calling
`hal_console_putchar()` directly, bypassing `hal_console_write()`
entirely, so a lock only there would miss them) — the shared resource
that actually needs protecting is the cursor/VGA state, not any one
caller's idea of "one whole message". A multi-character message from
two cores can still interleave at the character level as a visual,
cosmetic result (two complete, correctly-spelled messages taking turns
character-by-character rather than corrupting each other) — this
finally closes the Milestone 4 follow-up for what it was actually
about (state corruption), not for a cosmetic ordering guarantee it
never promised.

## Verified

- **Genuine AP bring-up**, not just a claim: `[AP core 1] alive,
  running independently of the BSP` appears in the boot log under
  `-smp 2`, and — the honest, deliberately-designed-for negative case
  — `SMP: no AP responded (single-CPU run?).` appears instead under
  QEMU's default `-smp 1`, with the rest of the kernel completely
  unaffected either way (`hal_smp_boot_ap()` never hangs waiting for a
  core that doesn't exist).
- **Genuine parallelism, not just sequencing**: the AP's own
  30-million-iteration counting loop finishes and prints its result
  literally in the middle of the BSP's actor trace (e.g. between
  `[Coordinator] exiting` and `[Greedy] spin 1 done`), not before or
  after it — impossible unless both cores were actually executing at
  the same wall-clock time on independent hardware.
- **The console race**, deliberately triggered (character-level
  cursor-state corruption) and then fixed (verified byte-for-byte
  correct, per-message, even though still visually interleaved) — see
  above.
- All previous milestones' demos (preemption, isolation, ring 3,
  mailboxes, capability delegation, ghost-actor spawn/terminate/quota,
  the full storage quarantine/reject pipeline) continue running
  correctly, unaffected by the AP's existence, since it participates in
  none of that shared state.

## Known follow-ups for the next milestone

- **The AP does not run actor code.** Generalizing the scheduler for
  genuine per-core actor execution needs: a per-core `current_actor`
  (there's no per-CPU data mechanism yet — `hal_lapic_id()` is read
  fresh each time rather than cached), a real lock (this milestone's
  new `hal_spinlock_t`) around `core/actor.c`'s `actors[]` and related
  state instead of `cli`-only critical sections, the AP's own
  preemption source (its local APIC timer — untouched by this
  milestone; PIT/8259 IRQ0 only ever reaches the BSP), and extending
  `hal_address_space_create()` so actor-context code can reach the
  LAPIC (needed the moment the AP's own timer ISR has to send itself
  an EOI while some actor's restricted CR3 is active).
- **Exactly one AP.** Generalizing past a fixed second core needs
  discovering how many actually exist and their APIC IDs (an ACPI MADT
  table walk), not hardcoded bring-up of a single one.
- **Console interleaving is character-atomic, not message-atomic** —
  documented, deliberate, and explained above, not a bug, but worth
  remembering before relying on console output for anything more
  than human-readable diagnostics.
