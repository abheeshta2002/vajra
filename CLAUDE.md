# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. **Phases 23-27 (the full hardening track) all DONE.**
Phase 13a (remote spawn) code implemented and pushed; its CI check is
gated behind two CI-only toolchain bugs (both below): the panic (fixed,
confirmed) and a truncated-kernel-image bug (fixed, awaiting CI).
Working tree clean, `working` pushed.

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

**Second CI-only bug (loader failing, blank names, empty object
contents) — ROOT CAUSE FOUND, fix pushed, awaiting CI confirmation.**
Symptoms seen once the kernel booted far enough: `[Loader] failed to
load and spawn calc.bin`/hello.bin, `[Namer] create failed!`,
`[2]  (TRUSTED)` (blank name), `suspicious.bin` reading back `""`
while `payload.bin` was fine. Cause: the boot loader read exactly
`KERNEL_SECTORS=120` sectors (61,440 B) in one shot and silently
stopped; CI's Debian clang emits ~4KB more code than this dev
machine's LLVM, so CI's `kernel.bin` was 62,188 B (from CI's own
`__bss_start=0x2f2ec`) — everything past `0x2f000` (late `.rodata`
string literals like `"hello.bin"`/`"BADSTUFF payload"`/`"notes.txt"`,
and all of `.data`) loaded as zeros. Data-dependent because only
literals that happened to land past the cutoff broke. Fix (`boot.asm`
fix #6): kernel read is now 4 chunks x 64 sectors (`KERNEL_SECTORS`
256, 128KB), `core/storage.c`'s `DIRECTORY_LBA`/`OBJECT_DATA_BASE_LBA`
moved to 260/270 (past the range), and `tools/build-c.ps1` now READS
`KERNEL_SECTORS` from boot.asm and FAILS the build if `kernel.bin`
exceeds it (warns >80%). Verified locally: a temporarily bloated
67,068 B kernel (over the old cap) boots with names/contents intact;
the old 120 cap + that kernel makes the build refuse it; padding
reverted, clean regression. **Not yet confirmed on CI** — check the
next run; then Phase 13a's own `Verify Phase 13a` step is the real
question again.

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

**Known bugs**: mouse sensitivity untuned (no divisor on CELL_FRAC,
mouse.c) — Phase 32. Apps always maximized, no real windows — Phase 32
(labeled cut, not a bug).

**Non-negotiable** (don't relitigate without asking): actor/capability/
message-passing model (PHILOSOPHY.md §2/§3); no pixel graphics, text
CP437-only desktop (§5); not chasing Windows/Linux breadth (§5,
reconfirmed this session — user's own priorities are messaging/
migration/parallelism/self-hosting/usability, not driver/app breadth).

**Verification standard**: every fix proven by deliberately triggering
the failure in QEMU (serial log or screendump), not by review alone
(§3.7). Full-boot regression check: `-smp 2` + full scripted demo +
shell + calc.bin's 5 printed results, all present.
