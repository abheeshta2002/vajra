# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. Roadmap just restructured (Phases 23-32). Nothing
built for 23+ yet. Working tree clean.

**Next task**: Phase 23 — fault containment. Not started.
Plan: `isr_common` (isr_stubs.asm) pass saved CS as 4th arg to
`exception_handler()` (interrupts.c) → branch on CPL: CPL3 fault =
terminate actor + reap + reschedule, kernel survives; CPL0 = panic,
unchanged. Verify by deliberately faulting a ring-3 actor in QEMU.
**Do not skip to Phase 28+ before 23-27 — they make an already-broken
fault boundary worse, not better, if built first.**

**Known security issues (why 23-27 exist), verified against source**:
1. `interrupts.c` exception_handler has no CS, panics on ANY ring-3
   fault — isolation invariant is false today, not just untested.
2. No copy_from_user/range check in syscall.c — SYS_WRITE etc. follow
   raw actor pointers unbounded.
3. `storage.c:352` storage_read refuses only OBJ_REJECTED; loader.c
   never checks trust — executable == loadable today.
4. `paging.c:246` loaded-program pages are RWX, no NX.
5. `actor.c:138` capabilities are bare {op,target} ints, no generation
   — stale cap can hit a reused slot's new occupant.
6. `loader.c:70-81` leaks alloc_dma_pages() on spawn failure;
   PROGRAM_POOL_SIZE=2 entries never freed on actor death.

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
