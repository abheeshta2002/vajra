# Vajra — state (read this, then docs/ROADMAP.md and docs/PHILOSOPHY.md if depth is needed)

Auto-loaded every session — keep this SHORT. Facts and pointers only;
reasoning/design lives in ROADMAP.md/PHILOSOPHY.md, don't duplicate it
here. Overwrite stale lines, don't append — git history is the log.

**Branch**: `working`, push after every commit (standing instruction).

**State**: Phase 18 (desktop) done. Phase 22 (VajraLang) v0 done,
host-side only. Phases 23-26 (fault containment, safe user memory,
generation handles, resource lifecycle) DONE. Working tree clean,
`working` pushed.

**Next task**: Phase 27 — W^X + loader trust gate (loaded-program pages
go RW at load, RX after; loader_spawn_program() must require
OBJ_TRUSTED, not just non-OBJ_REJECTED). Not started. **Last hardening
phase before Phase 28+ (real SMP) is safe to start.**

**Known security issues (why Phase 27 exists), verified against source**:
1. ~~interrupts.c exception_handler had no CS, panicked on ANY ring-3
   fault~~ — FIXED Phase 23: CS now passed, CPL3 fault terminates just
   the actor (verified: hello.bin null-deref'd, kernel kept running
   and scheduling other actors — see ROADMAP.md Phase 23).
2. ~~No copy_from_user/range check in syscall.c~~ — FIXED Phase 24:
   actor_current_owns_range()/actor_current_may_read_range() (actor.c)
   validate every syscall pointer arg against the calling actor's own
   stack/program window (writes) or that plus the kernel's low image
   (reads, where built-in actors' string literals live) before it's
   ever dereferenced (verified: wild pointer + foreign-window pointer
   both refused, no panic — see ROADMAP.md Phase 24).
3. ~~actor.c:138 capabilities were bare {op,target} ints, no
   generation~~ — FIXED Phase 25: struct capability gained target_gen,
   checked against actors[]'/objects[]' own generation counters
   (bumped on every respawn/recreate) via current_generation_of()
   (actor.c). NOT reproduced live this session -- the natural repro
   (Worker 2 reusing Worker 1's freed slot) races against the rest of
   the demo for that slot; verified by full regression + code review
   instead. See ROADMAP.md Phase 25's own note before trusting this
   blind — a deterministic repro is still owed.
4. ~~loader.c:70-81 leaked alloc_dma_pages() on spawn failure;
   PROGRAM_POOL_SIZE=2 entries never freed on actor death~~ — FIXED
   Phase 26: loader.c frees pages on every failure path;
   hal_address_space_release_program() (paging.c) frees a dead actor's
   pool entry from reap_dead_actors() (actor.c). NOT reproduced live
   (actor_program_loader is quota-capped at 2 spawns, same as
   Phase 25's item — see ROADMAP.md Phase 26's own note).
5. `storage.c:352` storage_read refuses only OBJ_REJECTED; loader.c
   never checks trust — executable == loadable today.
6. `paging.c:246` loaded-program pages are RWX, no NX.

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
