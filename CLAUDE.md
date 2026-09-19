# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. **Phases 23-27 (the full hardening track) all DONE.**
Phase 13a (remote spawn) code implemented and pushed, verification
blocked on CI until the toolchain bug below is confirmed fixed. Working
tree clean, `working` pushed.

**Next task: confirm the CI KERNEL PANIC is actually fixed, then check
Phase 13a's own result.** Full diagnosis (this session, real work, not
guessed): CI's Ubuntu build (QEMU 8.2.2 + Debian apt clang/lld/nasm)
hit a genuine, deterministic `KERNEL PANIC — Vector: 0x6 (#UD),
RIP: 0x49000` on every single boot — never reproduced on this dev
machine's own toolchain (QEMU 11.1.0 + a separate LLVM install). Root
cause: `syscall_handler()`'s `switch(num)` (25 cases) compiles to a
jump table even at clang's default `-O0` — a `.rodata` array of
absolute case-target addresses, read and jumped through indirectly.
Every link here goes straight to `ld.lld --oformat binary` (a raw flat
binary, not an ordinary relocatable ELF), so every address in that
table has to be fully resolved and baked in AT LINK TIME. On this
specific Ubuntu `ld.lld` build, one table entry came out wrong,
landing exactly on `hal/x86_64/paging.c`'s `as_pml4` array (confirmed
against CI's own `nm`/`objdump` output, not a local guess) — an actor
called `SYS_WRITE`, the CPU tried to execute page-table bytes as code,
`#UD`. **Fix**: `-fno-jump-tables` added to every clang invocation in
`tools/build-c.ps1` (see its own top-of-file comment for the full
story), forcing a plain compare-and-branch dispatch instead — verified
locally that this removes the indirect jump entirely (`llvm-objdump`),
and full local regression stays clean. `tools/build-c.ps1` also now
permanently builds `build/kernel_debug.elf` (a real ELF with symbols,
never booted) — this is what made confirming the bug against CI's own
build possible; keep it, useful for future debugging generally. All
temporary debug instrumentation (raw-serial syscall tracing) has been
reverted out of `syscall.c` — the fix is real code, not a diagnostic.
**NOT yet confirmed the fix actually works on CI** — check the next
run's result before treating this as closed.

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
