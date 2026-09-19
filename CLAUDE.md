# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. **Phases 23-27 (the full hardening track) all DONE.**
Phase 13a (remote spawn, see below) implemented, PUSHED, awaiting CI
result. Working tree clean, `working` pushed.

**Next task**: check `network-test.yml`'s CI run for the just-pushed
build-c.ps1 fix (below) — confirm the "Build Vajra" step is finally
green again, THEN check the new "Verify Phase 13a" step specifically.
Mark ROADMAP.md's Phase 13a "implemented, awaiting CI verification" as
DONE once it's actually green, or fix and re-push if not. THEN Phase
28 — real parallel execution (folding SMP into the actor scheduler + a
formal audit classifying every kernel global as CPU-local/actor-local/
immutable/spinlock-protected/atomic/intentionally-shared). Not started.

**CI was silently broken from VajraLang onward, just fixed**: checking
Phase 13a's CI run (this session, via curl+GitHub API — no `gh` CLI on
this machine) found every workflow run since commit 75affd1 ("Add
VajraLang") had FAILED at the "Build Vajra" step itself, Ubuntu-side —
including every Phase 23-27 commit. Nobody had checked CI status
during any of that hardening work; verification that whole time was
Windows-only local QEMU boots. Cause: tools/build-c.ps1 hardcoded
`powershell -File ...` to invoke tools/vajrac.ps1 as a child process --
Windows PowerShell 5.1's binary name, which doesn't exist on Ubuntu
(only `pwsh`, PowerShell Core, is installed there). Fixed: picks
`pwsh` if present, falls back to `powershell` otherwise
($PwshExe near build-c.ps1's own top). Verified locally (this machine
only has `powershell`, exercises the fallback path) and full -smp 2
regression stayed clean. NOT yet confirmed on CI as of this write --
check the next push's run.

**Phase 13a — remote spawn (docs/ROADMAP.md's own section)**:
`actor_network_peer` (core/main.c) extended with `MSG_NET_SPAWN_
REQUEST`/`MSG_NET_SPAWN_REPLY` — after the existing HELLO/ACK/PING
handshake, each of the two symmetric instances asks the OTHER to run
`hello.bin`. No new wire format, no new syscall — rides on the
existing `{type,data}` transport + `SYS_SPAWN_PROGRAM`. Invariant-4-
safe without Phase 29: the spawned actor gets ZERO capabilities: the
ONLY new grants are `NETWORK_PEER_SLOT`'s own local `CAP_SPAWN`/
`CAP_READ_OBJECT` (kernel_main), letting IT decide whether to honor a
request — nothing crosses the wire but the ask. Known simplification:
program named by a raw object id both instances share only because
they booted the same seeded demo in the same order — real name/hash
lookup and actual code transfer (needs wire-payload fragmentation,
currently 8 bytes of `data` per message) are follow-up work, not this
slice. **Could not verify locally**: this machine's Windows QEMU
(11.1.0) stalls indefinitely right after "IDT installed." whenever
`-device virtio-net-pci` is attached at all, single OR two-instance,
confirmed this session — unrelated to this change (same stall with an
unmodified build). Relying on the existing Ubuntu CI workflow instead.

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
