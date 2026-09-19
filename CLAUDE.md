# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. **Phases 23-27 (the full hardening track) all DONE.**
Phase 13a (remote spawn) code implemented and pushed, verification
blocked behind a MUCH bigger, just-discovered problem (below). Working
tree clean, `working` pushed.

**URGENT, next task: a real KERNEL PANIC on CI's Ubuntu build,
deterministic, unrelated to networking.** Checking Phase 13a's CI run
found CI's own "Build Vajra" step had been silently failing since
VajraLang landed (below) — fixed that, and the VERY NEXT run then hit
`KERNEL PANIC — Vector: 0x6 (#UD, invalid opcode), RIP: 0x49000` on
EVERY boot on that runner: the single-instance sanity check (no
networking device attached at all) AND both two-instance peers, all
three byte-identical, immediately when the scheduler starts running
actors, before any actor prints a single line. Fully deterministic,
not a race, not networking-related — this means Vajra may not
correctly boot at all on a genuinely different toolchain/QEMU
combination (CI: QEMU 8.2.2 + Ubuntu's apt clang/lld/nasm; this dev
machine: QEMU 11.1.0 + a separate LLVM install), which every local
regression this whole session was blind to.

A local build's OWN symbol table happened to put 0x49000 exactly at
`as_pml4[2]` (`hal/x86_64/paging.c`'s per-actor PAGE TABLE pool) — i.e.
that specific actor's own CR3 VALUE, not a code address — suggesting a
CR3-used-as-jump-target bug somewhere in the context-switch path
(`hal/x86_64/context_switch.asm` and `core/actor.c`'s fake-frame setup
were both re-read this session and look internally consistent, so if
this hypothesis is right the bug is subtler than a simple push/pop
mismatch). **This address correlation is UNVERIFIED for CI's own
build** — symbol layout is toolchain-specific, this was local-only
reasoning, not proof. `tools/build-c.ps1` now also builds
`build/kernel_debug.elf` (same objects, real ELF with symbols — never
booted, previously only existed as an untracked ad hoc file) and
`network-test.yml` has a new diagnostic step dumping its sorted symbol
table + disassembly around 0x49000 into the job summary specifically
to confirm or refute this on CI's OWN build. **Next action: push
(already done, commit pending — check), read that diagnostic output,
confirm what's actually at CI's 0x49000, then find and fix the real
bug.** Do not attempt a fix before seeing that output — the address
may not even mean the same thing on CI's build.

**CI's build step was ALSO broken (separate, now-fixed issue)**: every
workflow run since commit 75affd1 ("Add VajraLang") had failed at
"Build Vajra" itself, Ubuntu-side — including every Phase 23-27
commit; nobody had checked CI status during any of that work, relying
on Windows-only local QEMU boots throughout. Cause: `tools/build-c.ps1`
hardcoded `powershell -File ...` to invoke `tools/vajrac.ps1` —
Windows PowerShell 5.1's binary name, absent on Ubuntu (`pwsh` only).
Fixed (`$PwshExe` detection, commit 17c6535) and **confirmed on CI**:
that run's "Build Vajra" step succeeded. This part is genuinely done —
the panic above is a SEPARATE, deeper problem it uncovered.

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
