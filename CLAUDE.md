# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 10 (SMP, up to 16 cores) DONE — see below. Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. **Phases 23-27 (the full hardening track) all DONE.**
**Phase 13a (remote spawn) DONE** — every step of the two-instance CI
workflow passes on commit `d76f0a5`, including `Verify Phase 13a`.
`working` pushed after every commit.

**Phase 20 (package install) DONE**: shell `pkg list|install <name>` ->
installer actor (INSTALLER_SLOT 17, sole holder of CAP_INSTALL_PACKAGE) ->
stage UNTRUSTED -> sandboxed inspector -> kernel verdict, which only works
on objects the installer staged (narrow). Catalog in core/packages.c.
MAX_OBJECTS 14. Runs in CI. **Ring-3 gotcha hit again**: comparing against a
string literal in ring 3 (`strcmp(x, "list")`) page-faults — compare chars.

**Phase 31 (adversary demo) DONE v1**: Lab key `a`, 12 attempts, all HELD;
also added a per-actor object-creation quota (`MAX_CREATES_PER_ACTOR` 2;
Lab 200) — SYS_CREATE_NAME beyond it returns -1. Runs in CI too.

**Phase 10 / SMP (done; 16 cores via MADT + one-at-a-time wake, per-core
TSS/stack)**: all cores run actors. Per-core scheduler LOOP on its own stack (actors switch to it, not
to each other); per-core TSS + LAPIC tick (vector 48); `hal_cpu_id()` =
CPUID leaf 1; **big kernel lock** (`hal/x86_64/cpu.c`, ticket lock, per
CORE not per actor — every ring-3 entry takes it, back-to-ring-3/idle
releases). Terminate of an actor RUNNING on the other core is deferred
(`kill_pending`). EOI must be sent BEFORE anything that can not return
(a deferred kill) — was a full-machine freeze. `MAX_ACTORS` 24 (page
tables now allocated, not `.bss`; `.bss` must still end < 0x9F000, build
checks). `SYS_SLEEP` replaces yield-polling. Cores app = 7th desktop app
(`b` parallelism test, `s` kill test; -smp 1 gives 0.68x as the negative
control). Console writes atomic per window (`hal_console_begin/end_window`);
`SYS_KEY_READ` only serves the focused caller. **Test harness gotcha**:
`run.ps1 -Seconds N` kills QEMU so serial-file tails are lost (logs end
mid-line) — that is NOT a hang; poll the log while QEMU runs. Local QEMU
with a TCP monitor crashes at start ~50% (exit 0xC0000005): just retry.
Console redraw is deferred (`hal_console_flush`) + shadowed: a per-char
full redraw under the kernel lock made it look 95% held. The Cores app has a
live lock gauge (`SYS_KERNEL_STATS`) — use it before touching locks. Above
~8 cores the one lock limits scaling (tickless idle + finer locks = Phase 28).
Open: x2APIC (>255), wake-IPI, LAPIC calibration. This host has 12 logical CPUs: 16 emulated cores run ~4x slow.
QEMU 11.1.0 here crashes (0xC0000005) ~50% at start, sometimes mid-run:
check the process exit code before calling anything a hang.

**Next task** (user's order: small independent phases first — done so far:
Phase 10 complete, owed fixes, 31, 20; next 19 (standard utilities), 22
(actor-native VajraLang), 32 (usability); 13b/14/28/29/30 wait on
dependencies). Phase 19 design constraint: the shell holds the user's file
authority and DELEGATES the minimum per command to each loaded utility;
programs are <=2KB objects and only 2 can be loaded at once (pool). Standing:
check the Actions run after every push (status via API; job logs need auth).

**CI KERNEL PANIC — FIXED AND CONFIRMED.** CI's Ubuntu build (QEMU
8.2.2 + Debian apt clang/lld/nasm) hit a genuine, deterministic
`KERNEL PANIC — Vector: 0x6 (#UD), RIP: 0x49000` on every single boot
— never reproduced on this dev machine's own toolchain. Root cause:
`syscall_handler()`'s `switch(num)` (25 cases) compiled to a jump
table even at clang's default `-O0`; every link here goes straight to
`ld.lld --oformat binary` (a raw flat binary, addresses fully baked in
at link time, nothing left to relocate), and this specific Ubuntu
`ld.lld` baked one table entry wrong, landing exactly on
`hal/x86_64/paging.c`'s `as_pml4` array (confirmed against CI's own
`nm`/`objdump`, not a local guess). Fixed: `-fno-jump-tables` on every
clang invocation in `tools/build-c.ps1` (see its own top comment).
**Confirmed on CI**: the run for commit `83468c3` boots all the way
through to `[Greedy] exiting` on both the sanity check and Peer A —
something that had never once happened before. `build/kernel_debug.elf`
(a real ELF with symbols, never booted) is now a permanent build
product — keep it, useful for future debugging generally.

**Second CI-only bug (blank names, empty object contents) — FIXED AND
CONFIRMED.** The boot loader read exactly `KERNEL_SECTORS=120`
sectors (61,440 B) and silently stopped; CI's Debian clang emits ~4KB
more code than this dev machine's LLVM (CI `kernel.bin` 62,188 B), so
everything past `0x2f000` (late `.rodata` literals, all `.data`) loaded
as zeros. Fix (`boot.asm` fix #6): chunked read, `KERNEL_SECTORS` 256;
`core/storage.c` LBAs moved to 260/270; `tools/build-c.ps1` reads the
cap from boot.asm and FAILS the build if `kernel.bin` exceeds it.
**Confirmed on CI (commit `fd60745`)**: `hello.bin` name, `notes.txt`
create/rename/delete, `"BADSTUFF payload"` all correct now.

**Two ordinary logic bugs that CI's log then exposed (fixed, CONFIRMED
on CI at `d76f0a5`)** — neither toolchain-related, both timing/ordering:
(1) `[Loader] failed to load and spawn` — all 17 actor slots are full
early in the demo (15 static + Coordinator's Worker + Scanner's
Inspector), so a spawn fails until any of them exits; CI's timing hit
it, this machine's didn't. Fix: `user_spawn_program_retry()` (main.c),
bounded retry with `user_yield()`, used by `actor_program_loader` and
the network peer. (2) Phase 13a's spawn request was handled ONLY in
`actor_network_peer`'s final drain loop, but `core/net.c` auto-ACKs any
frame inside `user_net_receive()` from EITHER loop — a request arriving
during the HELLO handshake loop was ACKed (sender's reliable send
succeeded) then silently dropped. Fix: `net_handle_spawn_msg()` shared
by both loops. CI peers now have distinct MACs (52:54:00:aa:00:0a / bb:00:0b), asserted
by their own CI step. That step exposed a real driver bug: virtio_net read
the MAC 4 bytes off (CI showed 34:56:01:00:FF:FF for 52:54:00:12:34:56)
because pci.c shifted the config offset when MSI-X was merely PRESENT, not
ENABLED — fixed (pci.c `pci_msix_enabled`), confirmed on CI.

**CI's build step was ALSO broken (separate, already-fixed issue,
same session)**: every workflow run since commit 75affd1 ("Add
VajraLang") had failed at "Build Vajra" itself, Ubuntu-side — nobody
had checked CI status during Phases 23-27's whole hardening track.
Cause: `tools/build-c.ps1` hardcoded `powershell -File ...` (Windows
PowerShell 5.1's binary name, absent on Ubuntu) instead of detecting
`pwsh`. Fixed and confirmed on CI (commit 17c6535) — this part is done.

**Phase 13a — remote spawn (docs/ROADMAP.md's own section, full
detail)**: `actor_network_peer` (core/main.c) extended with
`MSG_NET_SPAWN_REQUEST`/`MSG_NET_SPAWN_REPLY` — after the existing
HELLO/ACK/PING handshake, each of the two symmetric instances asks the
OTHER to run `hello.bin`. No new wire format, no new syscall — rides
on the existing `{type,data}` transport + `SYS_SPAWN_PROGRAM`.
Invariant-4-safe without Phase 29: the spawned actor gets ZERO
capabilities — the ONLY new grants are `NETWORK_PEER_SLOT`'s own local
`CAP_SPAWN`/`CAP_READ_OBJECT` (kernel_main), letting IT decide whether
to honor a request. Known simplification: program named by a raw
object id both instances share only because they booted the same
seeded demo in the same order. **Could not verify locally**: this
machine's Windows QEMU (11.1.0) stalls indefinitely right after "IDT
installed." whenever `-device virtio-net-pci` is attached at all,
single OR two-instance, confirmed this session with an unmodified
build too (unrelated to this change). Relying on CI — see "Next task"
above for exactly where that stands.

**Hardening track history (Phases 23-27, all FIXED, all verified in
QEMU unless noted)** — see ROADMAP.md for full detail per phase:
1. Fault containment (23): CS now passed to exception_handler; a CPL3
   fault terminates just that actor, not the kernel.
2. Safe user memory (24): actor_current_owns_range()/
   actor_current_may_read_range() (actor.c) validate every syscall
   pointer arg before it's dereferenced.
3. Generation handles (25): struct capability gained target_gen,
   checked against actors[]'/objects[]' generation counters
   (current_generation_of(), actor.c). NOT reproduced live — the
   natural repro races against the rest of the demo for the freed
   slot; verified by full regression + code review instead (ROADMAP.md
   Phase 25's own note — a deterministic repro is still owed).
4. Resource lifecycle (26): loader.c frees pages on every failure
   path; hal_address_space_release_program() (paging.c) frees a dead
   actor's program-pool entry from reap_dead_actors(). NOT reproduced
   live (same quota-cap reason as #3 — see ROADMAP.md Phase 26).
5. W^X + trust gate (27): loaded-program pages now present|user|
   execute, never writable (paging.c); loader_spawn_program() requires
   OBJ_TRUSTED via storage_get_trust(); kernel_main now explicitly
   promotes hello.bin/calc.bin through the real pipeline. BOTH
   verified live: a write into a loaded program's own code page
   genuinely #PFs and gets caught by Phase 23; SYS_SPAWN_PROGRAM on an
   untrusted object is refused.
6. Follow-on (still Phase 27, found by a second review after the above
   landed): `.user_text` (paging.c) was ALSO still RWX (0x7) — Phase
   27's pass only touched the loaded-program window, missed this one.
   Fixed the same way (0x5, no writable bit). Verified live: write to
   __user_text_start genuinely #PFs, caught by Phase 23, no panic.

**Known, deliberate, NOT yet fixed**: `actor_current_may_read_range()`
(Phase 24, actor.c) permits reading the ENTIRE kernel image below 1MB
via any syscall's read-direction pointer arg (SYS_WRITE's string,
etc.), not just legitimate .rodata literal addresses — flagged by the
same second review. This is the documented tradeoff Phase 24 made to
avoid breaking built-in actors' string literals (see that function's
own comment) — a real disclosure risk, not a crash risk, and properly
closing it needs a dedicated read-only user-runtime-data region
separate from the kernel image (bigger structural change, not
attempted yet). Don't treat this as fixed.

**Desktop keys**: F1-F7 focus app 0-6, F8 = bare desktop (keyboard.c ->
`hal_console_focus_app`, never buffered, so no app can fake a switch) —
the mouse can't be driven through QEMU's monitor here (cursor sticks at the
bottom-left), so F-keys are also how tests focus a window. **Fabric app keys**
(actor_network_peer stays alive after its handshake): h = hello, p = ping,
r = ask the peer to run hello.bin; CI drives them through Peer A's QEMU
monitor (five "Fabric app --" steps) — CONFIRMED green on CI at 7bae000
(local QEMU can't run virtio-net). Gotcha found there: SYS_KEY_READ needs
CAP_CONSOLE — the network actor lacked it and its keys were silently denied. ANSI parser state is now per window (a
global one let another window's write corrupt a split escape sequence).

**Known bugs**: mouse sensitivity untuned (no divisor on CELL_FRAC,
mouse.c) — Phase 32. Apps always maximized, no real windows — Phase 32
(labeled cut, not a bug).

**Standing rule (user, 2026-09-20)**: for every backend feature built,
also build a front-end app where a person can *experience* it — a
desktop app, not just a boot-log line. Shipped so far: **Security
Lab** (attack the hardening interactively; keys 1-9, `a` = the Phase 31 adversary campaign) for Phases 23-27,
**Fabric** (window for the network peer) for Phase 12/13a. Desktop is
now 7 apps (hal.h `CONSOLE_WIN_*`, console.c roster; Cores added for Phase 10).

**Non-negotiable** (don't relitigate without asking): actor/capability/
message-passing model (PHILOSOPHY.md §2/§3); no pixel graphics, text
CP437-only desktop (§5); not chasing Windows/Linux breadth (§5,
reconfirmed this session — user's own priorities are messaging/
migration/parallelism/self-hosting/usability, not driver/app breadth).

**Verification standard**: every fix proven by deliberately triggering
the failure in QEMU (serial log or screendump), not by review alone
(§3.7). Full-boot regression check: `-smp 2` + full scripted demo +
shell + calc.bin's 5 printed results, all present.
