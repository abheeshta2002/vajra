# Vajra C Rewrite — Milestone 13: The First Network Packet

## What this is

Roadmap Phase 12, deliberately pulled ahead of Phase 10's own
remainder and Phase 11 (AArch64) by explicit direction — see
`docs/ROADMAP.md`'s Phase 10/11/12 entries for why neither blocks
networking, and `docs/PHILOSOPHY.md` for why this phase, not those, is
the actual thesis. This milestone is the raw HAL driver only — a real
virtio-net-pci device, found via a from-scratch PCI scanner, sending
and receiving genuine raw Ethernet frames — with no IP/UDP/TCP stack
and no capability gating yet, the same "driver first, syscall surface
later" shape Phase 8 (storage) took before Phase 10/11's object store
existed.

## What's included

- **`src/hal/x86_64/pci.c`** (new) — minimal PCI configuration-space
  access (I/O ports 0xCF8/0xCFC): scans bus 0, function 0 for a
  vendor/device match, enables I/O space + bus mastering on a match,
  and walks the PCI capability list to detect MSI-X (needed to compute
  the correct device-config offset for legacy virtio devices — see
  below). This is the first time this kernel has had to *find* a
  device rather than being handed a fixed, well-known port range.
- **`src/hal/x86_64/virtio_net.c`** (new) — a minimal legacy
  virtio-net-pci driver: negotiates zero optional features (keeping
  `virtio_net_hdr` a fixed 10 bytes and every buffer a single,
  ungrouped descriptor — the simplest *correct* configuration, not a
  shortcut), builds RX/TX virtqueues sized generically from whatever
  `QUEUE_NUM` the device actually reports, posts 8 RX buffers, and
  exposes `hal_net_init()`/`hal_net_get_mac()`/`hal_net_send()`/
  `hal_net_poll_receive()`.
- **`src/include/vajra/memory.h`** / **`src/core/memory.c`** (updated)
  — `alloc_pages_contig(count)`: a genuine new primitive, not just a
  wrapper. Virtio's virtqueues are a hardware-defined layout the
  device addresses via ONE physical base, so the descriptor table,
  available ring, and used ring have to sit in one physically
  contiguous region — something `alloc_page()`'s single-page bump
  allocator was never asked to guarantee before.
- **`src/core/main.c`** (updated) — `net_arp_demo()`: prints the
  device's MAC, hand-builds a broadcast ARP request ("who has
  10.0.2.2?" — QEMU's default usermode-networking gateway), sends it,
  and polls for the reply. Banner bumped to Milestone 13.
- **`src/include/vajra/hal.h`** (updated) — PCI and network HAL
  declarations.
- **`tools/build-c.ps1`** (updated) — `pci.c`/`virtio_net.c` added to
  the build.

## Two real bugs, both found only by booting

- **The kernel outgrew its own boot loader.** Adding the PCI scanner
  and virtio-net driver pushed `kernel.bin` past 50 sectors (28924
  bytes) for the first time — and 0x7C00 (the boot sector's own load
  address) is a hard ceiling on how far anything loaded at the old
  0x1000 could grow before a read started overwriting the boot
  sector's own code while it was still executing (the exact failure
  mode an earlier milestone already had to debug once). Fixed by
  moving the kernel's load address to 0x20000 and raising
  `KERNEL_SECTORS` to 120 — see `src/boards/pc-bios/boot.asm`'s own
  "fix #3" comment for the full reasoning.
- **That relocation silently created a second, worse collision.**
  Moving the kernel's base pushed every linked address up by the same
  amount — including `.bss`, which turned out to be dominated by
  Milestone 5's per-actor page-table arrays (~256KB) and now reaches
  ~0x80a0c. That's well past where `hal/x86_64/start.asm`'s boot stack
  (0x80000) and `hal/x86_64/smp.c`'s AP trampoline (0x70000, from
  Milestone 12) still lived — both silently swallowed by live kernel
  `.bss`. Confirmed by an actual hang, not caught by inspection first:
  boot got exactly to `hal_map_lapic_mmio()` and stopped with zero
  further output — narrowed down via temporary breadcrumb prints and
  an `nm` symbol dump, which showed `lapic_pd`/`lapic_pt` (also new
  this milestone) landing at 0x7e000/0x7f000, squarely inside what had
  become both the AP-trampoline region *and* the boot stack's own
  range. The zero-fill loop for those arrays was overwriting live
  return addresses on the stack out from under itself. Fixed by
  relocating the boot stack to 0x88000 and the AP trampoline/mailbox/
  stack to 0x96000/0x96FF0/0x96FF8/0x99000 — both chosen with real
  headroom this time, and kept below 0xA0000 (the conventional PC
  VGA/BIOS-shadow memory hole this kernel has never tested writing
  into) rather than risking untested territory. This is the same
  recurring bug class as the page-table and E820-map relocations
  earlier in this project's history, one structure later — there is
  still no general mechanism preventing a fourth collision next time
  something grows, which is worth remembering before trusting a
  low-memory-adjacent change on inspection alone.

## Verified

- **A genuine round trip over real (emulated) hardware**, not a
  loopback or simulation: booted with `-netdev user,id=n0 -device
  virtio-net-pci,netdev=n0`, the kernel printed its own MAC, sent a
  broadcast ARP request, and received a real reply from QEMU's SLIRP
  gateway (`52:55:0A:00:02:02` for `10.0.2.2`) — proof both TX and RX
  paths work against actual (emulated) hardware, not just that the
  driver initializes without crashing.
- **The honest negative case**: booted without a virtio-net-pci
  device attached, `hal_net_init()` correctly reports none found and
  the rest of the kernel proceeds unaffected — no hang, no crash.
- **SMP and networking together**: booted with both `-smp 2` and
  `-device virtio-net-pci`, confirming the AP-trampoline relocation
  didn't disturb Milestone 12's bring-up and the two new drivers don't
  interfere with each other.
- All previous milestones' demos (preemption, isolation, ring 3,
  mailboxes, capability delegation, ghost-actor lifecycle, the full
  storage quarantine/reject pipeline, SMP bring-up) continue running
  correctly.

## Known follow-ups for the next milestone

- **No IP/UDP/TCP stack, no capability gating, not reachable from
  actor code at all** — this milestone is the raw HAL driver only, the
  same two-step shape storage took (Phase 8 driver, Phase 9/10 object
  store + syscalls on top). A real transport and syscall surface is
  explicitly the next piece of Phase 12.
- **Still exactly one recognized PCI device, bus 0/function 0 only** —
  no bridge traversal, no multi-function cards.
- **The low-memory collision-avoidance problem remains structural**,
  not solved generally — see the second bug above.
