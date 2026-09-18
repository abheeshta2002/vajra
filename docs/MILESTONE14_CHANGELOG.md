# Vajra C Rewrite — Milestone 14: Actor Messages Cross a Device Boundary

## What this is

Roadmap Phase 12, the piece after Milestone 13's raw driver: proving
an ACTOR — not `kernel_main` poking the HAL directly — can send and
receive a message across a genuine device boundary, capability-gated,
the same way every other authority in this kernel is. This is the
first milestone where `docs/PHILOSOPHY.md` §1's actual thesis (devices
behaving like one connected fabric) has real code behind it, not just
a raw driver.

Deliberately minimal, the same way Milestone 13 deferred routing:
no addressing scheme (every message is broadcast to the local link),
no remote actor identity (a reply means "some peer heard me", not
"actor X on device Y heard me"), no retries or reliability beyond one
attempt. Real transport semantics are explicitly still ahead.

## What's included

- **`src/include/vajra/net.h`** / **`src/core/net.c`** (new) — a
  minimal, from-scratch wire protocol carried in Ethernet frames under
  EtherType `0x88B5` (one of the two values IEEE reserves for "local
  experimental" use, safely distinguishable from ARP/IPv4 sharing the
  same wire). `net_init()`, `net_send_message()`,
  `net_poll_receive_message()` — portable core logic sitting on top of
  Milestone 13's `hal_net_send`/`hal_net_poll_receive`.
- **`src/include/vajra/actor.h`** (updated) — `CAP_NET`: blanket, like
  `CAP_SPAWN`, since there's exactly one network device and no
  per-peer addressing scheme yet to scope a grant against.
- **`src/include/vajra/hal.h`** / **`src/hal/x86_64/syscall.c`**
  (updated) — `SYS_NET_SEND`/`SYS_NET_RECEIVE`, capability-checked
  against `CAP_NET`, thin pass-throughs to `core/net.c`.
- **`src/core/main.c`** (updated) — `actor_network_peer()`: the
  demonstration this milestone is actually about. Symmetric by
  design — the exact same, unmodified actor runs on every instance:
  broadcast a HELLO, listen for one from a peer, reply once if heard.
  Two separate booted instances running this same code, with neither
  one hardcoded as "the sender," is the actual verification. Banner
  bumped to Milestone 14; the demo now runs 12 ring-3 actors.

## A real bug, found only by booting an actor into this path

`hal_net_send()`'s buffers (`tx_buf`, `rx_bufs[]`, the virtqueues
themselves) were allocated via `alloc_page()`/`alloc_pages_contig()` —
Milestone 13's own choice, and it worked fine there because
`net_arp_demo()` ran entirely inside `kernel_main`, under the boot CR3
that identity-maps everything. Those two allocators hand out memory
from `MEM_BASE` (1MB) upward, which is exactly the actor-private
1MB-2MB window `hal/x86_64/paging.c` deliberately maps ONLY for
whichever specific actor owns that exact page — not a general-purpose
kernel heap.

The instant `actor_network_peer` (a real ring-3 actor, with its own
restricted CR3) called `SYS_NET_SEND` for the first time, this
produced a genuine page fault (`#PF`, error code 2 — supervisor-mode
write, not-present) at the very first write into `tx_buf`, whose
address landed at `0x10E000` — squarely inside that private window,
mapped for nobody in particular. Confirmed by booting and reading the
fault's own `CR2`/`RIP`, then cross-referencing an `nm` symbol dump to
find the code was inside `hal_net_send`, not by inspection first.

Fixed with a new allocator primitive: **`alloc_dma_pages()`**
(`core/memory.c`, declared in `include/vajra/memory.h`) — identical
scan logic to `alloc_pages_contig()`, but starting its search at 2MB
instead of 1MB, i.e. from the "commons" region `paging.c` maps
identically (supervisor-only) in *every* actor's address space. All
three of `virtio_net.c`'s allocation sites (the two virtqueues, the
RX buffer pool, the TX buffer) now use it instead. This is exactly the
right home for it anyway: actor code should never be able to touch
these buffers directly regardless of CR3, only through the
capability-gated syscall boundary — supervisor-only was always the
correct permission, the bug was only ever about *reachability*, not
about who should be allowed to write there.

## Verified

- **Capability enforcement and graceful behavior, within a single
  instance**: `actor_network_peer` broadcasts a HELLO, listens across
  10 bounded attempts, and correctly reports `"no peer heard from
  within the listening window (single-instance run?)"` when run alone
  — the honest, expected outcome, not a hang or a crash. The full
  12-actor demo (including the entire storage/quarantine pipeline)
  continues to complete and halt cleanly alongside it.
- **The underlying HAL layer, against real functioning hardware**:
  re-confirmed the Milestone 13 ARP round trip still works correctly
  end-to-end after the `alloc_dma_pages()` change — a genuine
  send/receive cycle through the same driver these new syscalls sit
  on top of.
- **A live two-instance exchange (Peer A genuinely receiving Peer B's
  HELLO and vice versa)** — now verified for real, via
  `.github/workflows/network-test.yml` on GitHub Actions (Ubuntu,
  apt-packaged QEMU), after a long detour finding and fixing three
  separate, genuinely unrelated problems in turn:
  1. **A real local-machine blocker, not a Vajra bug**: on this
     project's own Windows development machine, `qemu-system-x86_64.exe`
     itself hard-crashed (`0xc0000005`, access violation, faulting in
     an unnamed/dynamically-generated code region — a JIT/TCG-related
     crash) every time the `-netdev socket` backend was used, confirmed
     via Windows Event Viewer, while `-netdev user` (SLIRP) never
     crashed. This QEMU/Windows-build issue is why two-instance testing
     moved to CI at all; it says nothing about `core/net.c` or
     `actor_network_peer()`.
  2. **A disk-image lock conflict**: the CI workflow's first version
     pointed both QEMU instances at the same `build/disk.img`; QEMU's
     default exclusive write lock on a raw `-drive` image made the
     second instance fail outright (`Failed to get "write" lock`).
     Fixed by giving each peer its own copy of the image.
  3. **The actual root cause, found only after the above two were
     eliminated**: `hal/x86_64/console.c`'s `hal_console_putchar()`
     never wrote to COM1 in the shipped code — every previous manual
     verification throughout this project had only ever worked because
     of a temporary debug line added before testing and reverted
     after. This made every CI run's serial log come back empty,
     which was first (wrongly) read as a boot failure. `-d
     int,cpu_reset,guest_errors` (the same diagnostic technique this
     project's own Milestone 1 changelog used once before) proved the
     kernel was actually running perfectly the whole time — real
     syscalls, real timer interrupts, zero faults — the serial log was
     empty because nothing was ever written to it, independent of
     whether the kernel worked. Fixed by making the COM1 mirror
     permanent (see that file's own comment).
  4. **A genuine, if narrower, demo-level bug**, visible only once (3)
     was fixed and real boot traces could be seen: `actor_network_peer`
     timed its listening window in raw busy-spin loop iterations, not
     real time. Under TCG emulation that window closed in well under a
     second even across 20 attempts — far shorter than the multi-second
     gap between when two independently-launched QEMU processes
     actually reach that point in their own boot. No amount of
     periodic re-broadcasting helped, because the two windows simply
     never coincided in wall-clock time; both instances' own boot
     traces showed flawless execution on both sides, yet neither ever
     heard the other. Fixed by substantially widening the window and
     shrinking the CI workflow's artificial start-time stagger between
     the two instances (the `listen=` socket binds at QEMU's own
     device-init, not guest boot, so it never needed multiple seconds
     of margin there).

  None of these four were the same bug, and none of the first three
  were actually inside the code this milestone added — but all four
  had to be found and fixed, in order, before genuine cross-instance
  verification was possible at all.

## Known follow-ups for the next milestone

- **No addressing scheme** — every message is a broadcast; targeting a
  specific peer needs real addressing (Phase 12's own next step).
- **No remote actor identity** — a received message can't be attributed
  to a specific remote actor, only "some peer." Needed before Phase 13
  (distributed actors) can build on top of this.
- **No reliability** — one send, no retry, no acknowledgment beyond
  this milestone's own HELLO/HELLO_ACK demo pattern.
