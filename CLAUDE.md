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
deterministic, unrelated to networking. Partially diagnosed, root
cause still unknown.** `KERNEL PANIC — Vector: 0x6 (#UD), RIP: 0x49000`
on EVERY boot on that runner (single-instance sanity check, no
networking device at all, AND both two-instance peers, byte-identical)
— never reproduced locally (this dev machine: QEMU 11.1.0 + a separate
LLVM install; CI: QEMU 8.2.2 + Ubuntu's apt clang/lld/nasm).

**CI's OWN symbol table (not a local guess — pulled via a diagnostic
CI step, see below) confirms `0x49000 = as_pml4` exactly** (paging.c's
per-actor page-table pool, row 0). QEMU's own `-d int` trace pinpoints
WHEN more precisely than first thought: NOT at initial scheduler
start — the sequence is `int 0x80` (vector 0x80, CPL3→CPL0, inside
`hal_syscall`, CR3=`0x4a000` = `as_pml4[1]`, a DIFFERENT, valid
actor's own table) immediately followed by the `#UD` at RIP=`0x49000`,
CR3 UNCHANGED. So: an actor makes a syscall, CR3 never switches (as
designed — syscalls don't switch CR3), and SOMEWHERE during the
kernel's own handling of that syscall, execution jumps to a
DIFFERENT actor's page-table array start and tries to execute page-
table data as code. Smells like a corrupted return address or
function pointer inside the syscall path, not (as first guessed) the
context-switch/fake-frame path — `context_switch.asm` and the fake-
frame setup in `core/actor.c` were both re-read and look internally
consistent for the simple case.

**Which exact syscall is in flight when this happens is still
unknown** — `-d int` only logs interrupt/exception EVENTS, not every
instruction in between. Added a TEMPORARY diagnostic to close that
gap: `syscall_handler()`'s very first lines (`hal/x86_64/syscall.c`)
now unconditionally print `[dbg] syscall <num> from actor <slot>` to
the LOG window before dispatching. Verified locally: adds a lot of
noise (every syscall, including the shell's own idle-loop key/mouse
polling) but no functional regression, no new crash — pushed as-is.
**Next action: get the next CI run's serial log tail (the line right
before `KERNEL PANIC`) — that names the exact syscall number and
actor slot that was executing when it crashed. Once known, read that
syscall's handler and whatever it calls line by line for a stack-
corruption/wrong-pointer bug. Revert the debug print once diagnosed
(labeled TEMPORARY in the comment).** `tools/build-c.ps1` also now
permanently builds `build/kernel_debug.elf` (a real ELF with symbols
from the same objects, never booted) — keep this, it's how the
`as_pml4` correlation above was confirmed and will help with future
debugging generally, not just this bug.

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
