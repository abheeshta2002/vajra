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
| 10 | SMP (multicore) | 🟡 Bring-up only — a second core boots and runs in parallel, not yet scheduling actors |
| 11 | AArch64 port | ⬜ Not started |
| 12 | **Networking as part of the actor fabric** | 🟢 Flagship, in progress — capability-gated actor-to-actor messaging verified across two genuinely separate QEMU instances, with real remote identity (device + remote actor slot) and addressing (unicast). No routing, no reliability guarantees yet. |
| 13 | Distributed actors & the personal fabric | ⬜ Not started — the other half of the actual thesis |
| 14–15 | Adaptive scheduling, heterogeneous compute / AI assistance | ⬜ Not started |
| 16–20 | CLI-OS parity (usability, not the thesis) | ⬜ Not started |

Phases are built in dependency order, not importance order — Phase 12 was
deliberately pulled ahead of finishing Phase 10/11 because it's closer to
the actual point of the project.

## Building it

Requires **NASM**, **QEMU**, and **clang + ld.lld** (LLVM) on `PATH`.
MSYS2's mingw-w64 toolchain does *not* work here — see
`tools/build-c.ps1`'s own comment for why.

```powershell
pwsh tools/build-c.ps1
qemu-system-x86_64 -drive file="build/disk.img",format=raw,if=ide
```

The build script is plain PowerShell and runs unchanged on Windows or
Linux (`pwsh`) — that's also how CI builds and boots it, on Ubuntu, with
no Windows-only dependency anywhere in the pipeline.

## Layout

```
src/
  boards/pc-bios/   BIOS-specific boot loader + linker script
  hal/x86_64/       everything architecture-specific: interrupts, paging,
                     the syscall gate, drivers (ATA, virtio-net, PCI, APIC)
  core/             portable kernel: scheduler, actors, capabilities,
                     memory policy, storage, networking protocol
docs/               philosophy, roadmap, and one changelog per milestone
```

`core/` never touches hardware directly — a future `hal/aarch64/` port is
supposed to make it compile and run unchanged. See `docs/architecture.md`
and `docs/folder-structure.md` for more.

## Why the changelogs read like incident reports

They are, on purpose. Each `docs/MILESTONEn_CHANGELOG.md` documents real
bugs found by actually booting the thing — a triple fault, a page fault
crossing a device boundary for the first time, a race condition only a
second physical core could expose — not just the feature that shipped.
That's deliberate: a bug found and written up is worth more than a clean
summary that hides how it was actually verified.
