# Vajra

[![Two-instance networking verification](https://github.com/abheeshta2002/vajra/actions/workflows/network-test.yml/badge.svg)](https://github.com/abheeshta2002/vajra/actions/workflows/network-test.yml)

A from-scratch x86-64 kernel built around one idea: **your devices — phone,
laptop, whatever else — should behave like one connected fabric of
computation, not several unrelated boxes.** A program's *display* and a
program's *execution* aren't the same thing and don't have to live on the
same device. Everything is an isolated **actor** talking to other actors
through **messages**, whether those actors are on the same core, a
different core, or a different device entirely — the caller never needs to
know which.

Capabilities are what make that safe to do across devices you don't all
trust equally: an actor starts with *no* authority and can only act on
something it's been explicitly, narrowly granted.

Read `docs/PHILOSOPHY.md` for the full statement — it's short on purpose.

## Status

Built incrementally, each layer proven working before the next is added —
see `docs/ROADMAP.md` for the full, honest picture (including what's
*not* done yet) and `docs/MILESTONE*_CHANGELOG.md` for how each piece was
actually verified, bugs and all.

| Phase | What | Status |
|---|---|---|
| 1–9 | Preemptive scheduling, virtual memory, ring 3, syscalls, message-passing IPC, capabilities, actor lifecycle, object storage, a quarantine/trust pipeline | ✅ Done |
| 10 | SMP (multicore) | ✅ Done — up to 16 cores run actors in parallel (cores found from the firmware's ACPI table and woken one at a time, per-core scheduler loops and preemption ticks, a big kernel lock). The desktop's **Cores** app shows every core live, the kernel-lock contention, and measures the speedup (≈3.5x of 4 on 4 cores, 0.7x on 1) |
| 11 | AArch64 port | ⬜ Not started |
| 12 | **Networking as part of the actor fabric** | ✅ Done — capability-gated actor-to-actor messaging, addressing, real remote identity (device + remote actor slot), and a genuine ACK-and-retry reliability primitive, all verified across two separate QEMU instances |
| 13 | Distributed actors & the personal fabric | 🟡 13a done — one device asks a peer to run a program, which the peer spawns under its own local authority (zero capabilities cross the wire), verified across two real QEMU instances in CI. True live migration (13b) still needs payload fragmentation and Phase 29 |
| 14–15 | Adaptive scheduling, heterogeneous compute / AI assistance | ⬜ Not started |
| 16 | A real program loader & userland runtime | ✅ Done — loads and runs a genuinely separately-compiled program, verified on both Windows and Linux QEMU |
| 17 | A persistent filesystem namespace over the object store | ✅ Done — real on-disk name→id directory, verified by rebooting the same disk image twice |
| 18 | Input devices, an interactive shell, and a text-mode desktop | ✅ Done — keyboard + RTC + PS/2 mouse drivers, a real shell, job control, pipes-as-mailboxes, and a real desktop compositor (icons, taskbar, apps menu, click-to-focus) — see `docs/DESKTOP_DESIGN.md`. Seven apps, including a **Security Lab** (attack the kernel from the keyboard and watch each attack get contained) , a **Fabric** pane and a **Cores** view |
| 19–20 | Standard utilities, package installs (CLI-OS parity — usability, not the thesis) | ⬜ Not started |
| 21 | A bounded POSIX compatibility shim & text browser | ⬜ Not started (exploratory) |
| 22 | VajraLang — an actor-native language | 🟡 v0 — a real lexer/parser/AST/compiler for a small calculator language, verified end-to-end (`tools/vajrac.ps1`, `src/userland/calc.vj`); not yet actor-native syntax or self-hosted |
| 23–27 | **Hardening**: fault containment, safe syscall pointers, capability identity, resource lifecycle, W^X + loader trust | ✅ Done — a code review found several of `docs/PHILOSOPHY.md` §3's invariants were violated by shipped code (e.g. a ring-3 page fault used to halt the whole kernel); all five verified in QEMU (two — capability generations, program-pool release — by full regression + code review rather than a live race-prone repro; see `docs/ROADMAP.md`'s own notes on those two) |
| 28 | Real parallel execution — folding SMP into the actor scheduler | ⬜ Not started |
| 29 | An authenticated fabric — device identity before remote capabilities | ⬜ Not started |
| 30 | Self-hosting — VajraLang, an editor, and a real actor heap, all running inside Vajra | ⬜ Not started |
| 31 | Adversarial demo — deliberately hostile code, proving the blast radius | ✅ Done (v1) — the Security Lab's **adversary** (key `a`) runs 12 attacks, some with real delegated capabilities, and reports 0 breaches; it found and closed a missing per-actor object quota. Compiling it from inside Vajra waits on Phase 30 |
| 32 | Day-to-day usability | ⬜ Not started |

Phases are built in dependency order, not importance order — Phase 12 was
deliberately pulled ahead of finishing Phase 10/11, and Phase 16 ahead of
finishing Phase 10/11/13, because each was closer to the actual point of
the project (or, for 16, to making the OS usable at all) than what it
skipped past. See `docs/ROADMAP.md` for the full phase-by-phase reasoning,
including why Phases 23–27 were inserted ahead of Phase 28 rather than
appended after it.

## Building it

Requires **NASM**, **QEMU**, and **clang + ld.lld** (LLVM) on `PATH`.
MSYS2's mingw-w64 toolchain does *not* work here — see
`tools/build-c.ps1`'s own comment for why.

```powershell
pwsh tools/build-c.ps1
qemu-system-x86_64 -drive file="build/disk.img",format=raw,if=ide
```

`tools/run.ps1` builds and boots in one step (`-NoBuild`, `-Headless -Seconds N -SerialLog file`,
`-Smp N`; see its header).

The build script is plain PowerShell and runs unchanged on Windows or
Linux (`pwsh`) — that's also how CI builds and boots it, on Ubuntu, with
no Windows-only dependency anywhere in the pipeline.

## Layout

```
src/
  boards/pc-bios/   BIOS-specific boot loader + linker script
  hal/x86_64/       everything architecture-specific: interrupts, paging,
                     the syscall gate, drivers (ATA, virtio-net, PCI, APIC,
                     PS/2 keyboard + mouse, the desktop compositor)
  core/             portable kernel: scheduler, actors, capabilities,
                     memory policy, storage, networking protocol, loader
  userland/         genuinely separate programs (not linked into kernel.bin),
                     the minimal runtime they link against, and VajraLang
                     source (src/userland/calc.vj)
tools/              the PowerShell build pipeline, plus vajrac.ps1 —
                     VajraLang's own compiler (lexer/parser/AST/codegen)
docs/               philosophy, roadmap, and one changelog per milestone
```

`core/` never touches hardware directly — a future `hal/aarch64/` port is
supposed to make it compile and run unchanged. See `docs/architecture.md`
and `docs/folder-structure.md` for more.

## VajraLang

A small language built specifically for Vajra (Phase 22): a real lexer,
recursive-descent parser, and AST, targeting C as its codegen backend so
it goes through the same proven `clang` + `ld.lld` + loader pipeline as
any hand-written program. Today it's a calculator (`let`, `print`,
`+ - * /`, precedence, variables) — see `src/userland/calc.vj` for the
source and `tools/vajrac.ps1` for the compiler. It runs on the host, not
yet inside Vajra itself; self-hosting is Phase 30.

## Working with Claude on this repo

`CLAUDE.md` at the repo root is a short, auto-loaded state snapshot (not
a changelog) — current branch, what's done, the exact next task, and
known issues that shouldn't be skipped past. It's meant to be overwritten
at each checkpoint, not appended to; `docs/ROADMAP.md` and the milestone
changelogs are the permanent record.

## Why the changelogs read like incident reports

They are, on purpose. Each `docs/MILESTONEn_CHANGELOG.md` documents real
bugs found by actually booting the thing — a triple fault, a page fault
crossing a device boundary for the first time, a race condition only a
second physical core could expose — not just the feature that shipped.
That's deliberate: a bug found and written up is worth more than a clean
summary that hides how it was actually verified.
