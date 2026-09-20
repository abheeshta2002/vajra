#ifndef VAJRA_ACTOR_H
#define VAJRA_ACTOR_H

#include <stdint.h>

/* Deliberately NOT raised for Phase 16, despite adding a 13th fixed
 * demo actor -- tried bumping this to 24 first, and it genuinely
 * triple-faulted: each per-actor address-space slot costs 5 page
 * tables at the time (hal/x86_64/paging.c's as_pml4/pdpt/pd/pt0/pt1,
 * the 5th new for Phase 16's program window) = 20KB of kernel .bss per
 * slot, and 24 slots' worth pushed .bss past the fixed low-memory
 * addresses (0x90000+) boot.asm's own page tables and Milestone 12's
 * AP trampoline structures live at -- confirmed by a real triple
 * fault, CR2 landing exactly on BOOT_PDPT_PHYS_BASE (paging.c), the
 * same ".bss growth silently swallowing fixed structures" bug class
 * Milestone 13's own changelog already hit once. as_pt1 was later
 * redesigned into a small shared pool (paging.c, Milestone 16's own
 * follow-up), dropping the real per-slot cost to 4 page tables = 16KB.
 *
 * Raised by exactly 1, to 17, for Phase 18 (Milestone 18): the new
 * interactive shell actor (core/main.c's actor_shell()) is the first
 * actor in this project's history that's permanently alive for an
 * entire session (blocked in its own keyboard-read loop, never
 * exiting) rather than finishing a scripted task -- it silently used
 * up the one slot of slack the rest of the scripted demo's dynamic
 * spawns (Coordinator's workers, Scanner's inspectors) depended on,
 * confirmed by a real "Scanner: spawn failed!" in the boot trace, not
 * assumed. +1 slot = +16KB .bss (four [MAX_ACTORS][512] page-table
 * arrays), comfortably inside the ~25KB of headroom start.asm's own
 * Milestone-18 boot-stack relocation just freed up -- see that file's
 * own comment. (Historical -- see the next paragraph for why that
 * headroom worry no longer applies.)
 *
 * Raised again, 17 -> 19, for the Security Lab (docs/ROADMAP.md's
 * front-end-per-feature rule), and then to 24 for Phase 10's Cores app
 * once the ceiling below stopped being a ceiling. The ".bss headroom"
 * worry above is obsolete for the boot-time page tables (boot.asm's
 * structural fix moved those BELOW the kernel), but .bss still had a
 * hard limit -- it must end below 0x9F000 (BIOS EBDA / VGA window) --
 * and each slot cost ~20KB of it (four page-table arrays + a kernel
 * stack + the actor struct). 24 slots that way ended at 0xB5000 and
 * booted into a #PF inside the APIC setup, with no build error;
 * tools/build-c.ps1 now checks it. The fix that lifts the cap: the four
 * per-slot page tables (16KB of the 20KB) are allocated from the
 * page allocator on first use instead of living in .bss (see
 * hal/x86_64/paging.c), leaving only the 4KB kernel stack + the actor
 * struct per slot in .bss. */
#define MAX_ACTORS 24

/* Capability operations an actor can hold authority over. Adding a
 * new kind of authority later means adding a CAP_* constant here, not
 * redesigning how capabilities are held or checked.
 *   CAP_SEND(target)          -- may message actor slot `target`.
 *   CAP_SPAWN(0)              -- may create new actors at all (target
 *                                is unused/0: this authorizes spawning
 *                                in general, not spawning anything
 *                                specific -- there's no registry of
 *                                "spawnable roles" yet for a target to
 *                                name).
 *   CAP_TERMINATE(target)     -- may forcibly end actor slot `target`.
 *   CAP_READ_OBJECT(id)       -- may read storage object `id`
 *                                (core/storage.c).
 *   CAP_WRITE_OBJECT(id)      -- may replace object `id`'s contents.
 *   CAP_PROMOTE_OBJECT(0)     -- may advance any object's trust level,
 *                                OR reject it outright (blanket, like
 *                                CAP_SPAWN -- there's no per-object
 *                                grant scheme for this one either).
 *                                One capability for both sides of the
 *                                same verdict -- see hal.h's
 *                                SYS_OBJECT_PROMOTE/SYS_OBJECT_REJECT.
 *   CAP_NET(0)                -- may send/receive over the network at
 *                                all (blanket, like CAP_SPAWN -- there
 *                                is exactly one network device and no
 *                                per-peer addressing scheme yet for a
 *                                target to name). See hal.h's
 *                                SYS_NET_SEND/SYS_NET_RECEIVE and
 *                                core/net.c.
 *   CAP_LIST_NAMES(0)         -- may enumerate every name in the
 *                                namespace (SYS_LIST_OBJECTS). Blanket,
 *                                like CAP_SPAWN -- and deliberately its
 *                                own authority, distinct from
 *                                CAP_READ_OBJECT: looking up a name you
 *                                already know is not gated at all
 *                                (SYS_LOOKUP_NAME -- an id is public
 *                                knowledge), but discovering every name
 *                                that EXISTS is a different kind of
 *                                visibility (§3 invariant 2 -- no
 *                                ambient authority), the same reasoning
 *                                Phase 19's CAP_INTROSPECT will use for
 *                                `ps`.
 *   CAP_CREATE_OBJECT(0)      -- may create a new named object
 *                                (SYS_CREATE_NAME). Blanket, like
 *                                CAP_SPAWN -- creating objects is
 *                                scarce (MAX_OBJECTS), not per-target
 *                                authority.
 *   CAP_RENAME_OBJECT(id)     -- may rename object `id`
 *                                (SYS_RENAME_OBJECT). Deliberately its
 *                                own capability, not a reuse of
 *                                CAP_WRITE_OBJECT: renaming changes the
 *                                NAMESPACE binding, not the object's
 *                                contents -- roadmap Phase 17's
 *                                "distinct from, layered above" design.
 *                                Auto-granted to whoever creates an
 *                                object via SYS_CREATE_NAME, the same
 *                                "creator gets natural authority over
 *                                what it created" pattern SYS_SPAWN's
 *                                own CAP_SEND/CAP_TERMINATE auto-grant
 *                                already establishes.
 *   CAP_DELETE_OBJECT(id)     -- may remove object `id` from the
 *                                namespace (SYS_DELETE_NAME). Same
 *                                auto-grant treatment as
 *                                CAP_RENAME_OBJECT above.
 *   CAP_CONSOLE(0)             -- may read the keyboard
 *                                (SYS_KEY_READ). Blanket, like
 *                                CAP_SPAWN -- there is exactly one
 *                                keyboard and no per-window addressing
 *                                scheme (roadmap Phase 18 is
 *                                deliberately NOT a windowing system --
 *                                see docs/ROADMAP.md's own "text-mode
 *                                TUI" scoping note). Gates INPUT only:
 *                                plain SYS_WRITE stays ungated exactly
 *                                as it always has, so every actor from
 *                                earlier milestones keeps working
 *                                unchanged -- see hal.h's SYS_KEY_READ
 *                                comment for why the two need different
 *                                policies. */
#define CAP_SEND           1
#define CAP_SPAWN          2
#define CAP_TERMINATE      3
#define CAP_READ_OBJECT    4
#define CAP_WRITE_OBJECT   5
#define CAP_PROMOTE_OBJECT 6
#define CAP_NET            7
#define CAP_LIST_NAMES     8
#define CAP_CREATE_OBJECT  9
#define CAP_RENAME_OBJECT  10
#define CAP_DELETE_OBJECT  11
#define CAP_CONSOLE        12
#define CAP_USER_DATA      14 /* Phase 19: read/write/rename/delete authority over EVERY user-domain
                                  object (storage_is_user_object(): created at runtime or installed as a
                                  package -- never the kernel-seeded system objects or the utilities).
                                  A scoped domain capability, not ambient authority: only the shell holds
                                  it (the user's agent), and it hands each utility just the single-object
                                  capability that command needs, by ordinary delegation. Persistent
                                  because the "user" flag is, unlike per-object capabilities, which are
                                  lost at reboot. Blanket op (target 0). */
#define CAP_INTROSPECT     15 /* Phase 19: may read the state of actor slot `target` (SYS_ACTOR_INFO).
                                  Auto-granted to a spawner for each of its children, like CAP_SEND/
                                  CAP_TERMINATE -- so by default `ps` shows only your own descendants,
                                  never a global process table (visibility is a capability). Delegable. */
#define CAP_INSTALL_PACKAGE 13 /* Phase 20: may stage a catalog package (SYS_PKG_STAGE) and deliver a
                                  verdict on the objects it staged (SYS_PKG_VERDICT) -- and NOTHING
                                  else. Deliberately not CAP_PROMOTE_OBJECT (blanket, unscoped): the
                                  kernel refuses a verdict on any object the installer did not stage,
                                  so holding this can never promote an arbitrary object. Blanket op
                                  (target 0), like CAP_SPAWN. */

/* A message as it travels through a mailbox. Deliberately minimal --
 * a fixed-size inline payload, no reference field yet (larger
 * payloads should go through an object reference once the object
 * system exists, not grow this struct) and no support for payloads
 * larger than one word. */
struct message {
    uint64_t type;
    uint64_t sender; /* filled in by the kernel from the actual caller of
                         actor_send() -- never trusted from the sender itself,
                         since actor identity must not be something a sender
                         can fake (message-passing philosophy §4) */
    uint64_t data;
};

void scheduler_init(void);

/* The RAW spawn primitive: unconditionally creates a new actor
 * running entry(), no capability check, no quota. Returns its slot
 * index, or -1 if there's no free slot or memory for its stack.
 * Callable from kernel_main (before the scheduler starts, interrupts
 * still disabled entirely) or from actor_spawn_child() below (from
 * syscall context, where interrupts are already disabled by the
 * syscall gate itself) -- both callers already run with interrupts
 * off, so this needs no locking of its own. Not capability-gated
 * itself because kernel_main's own initial setup calls need to be
 * unconditional; actor code reaches spawning only through
 * actor_spawn_child(). */
int actor_spawn(void (*entry)(void));

/* The capability-checked, quota-limited spawn actor code actually
 * uses (via SYS_SPAWN) -- the roadmap's "ghost actor" primitive.
 * Requires the CALLING actor to hold CAP_SPAWN and be under its spawn
 * quota (MAX_SPAWNS_PER_ACTOR, core/actor.c); not every actor should
 * be able to spawn arbitrarily, and none should be able to fork-bomb
 * the system. On success, auto-grants the natural parent/child
 * relationship a spawned worker needs: the caller receives
 * CAP_SEND+CAP_TERMINATE for the new actor, and the new actor
 * receives CAP_SEND for the caller, so it can report results back
 * without any extra setup. Returns the new actor's slot index, or -1
 * if denied, quota-exceeded, or actor_spawn() itself failed. */
int actor_spawn_child(void (*entry)(void));

/* Roadmap Phase 16: the raw and capability-checked spawn-from-loaded-
 * program primitives -- exactly actor_spawn()/actor_spawn_child()'s
 * own relationship, except entry is computed from PROGRAM_VBASE +
 * entry_offset (include/vajra/loader.h) instead of being a kernel-
 * linked function pointer, and hal_address_space_map_program() adds
 * [phys_base, phys_base+phys_size) to the new actor's address space.
 * actor_spawn_program() is unchecked -- only core/loader.c should call
 * it, after it has already validated phys_size itself.
 * actor_spawn_program_child() is what SYS_SPAWN_PROGRAM actually
 * reaches (via loader_spawn_program()): same CAP_SPAWN + quota +
 * parent/child auto-grant as actor_spawn_child(). Both return the new
 * actor's slot, or -1 on any failure. */
int actor_spawn_program(uint64_t phys_base, uint64_t phys_size, uint32_t entry_offset);
int actor_spawn_program_child(uint64_t phys_base, uint64_t phys_size, uint32_t entry_offset);

/* Forcibly ends actor slot `target`, which must not be the calling
 * actor (use actor_exit()/SYS_EXIT to end yourself). Requires the
 * caller to hold a CAP_TERMINATE capability for target. Returns 0 on
 * success, -1 if denied or target is invalid/already dead/is the
 * caller. Whatever target was doing is simply abandoned -- its stack
 * is reclaimed the same way any other dead actor's is (see
 * reap_dead_actors in core/actor.c). */
int actor_terminate(int target);

/* Voluntarily gives up the CPU; the calling actor stays READY and
 * will run again once every other ready actor has had a turn. */
void actor_yield(void);

/* Terminates the calling actor permanently. Never returns. */
void actor_exit(void);

/* Starts running actors round-robin. Never returns to its caller --
 * once every actor has exited, it halts the machine. */
void scheduler_start(void);

/* Phase 10 (remainder): the second core joins the scheduler. */
void scheduler_start_ap(void);
void actor_sleep(uint64_t ticks);
void actor_check_pending_kill(void);
void actor_core_status(int cpu, int *running_slot, uint64_t *switches, uint64_t *idle_ticks);

/* Sends a message to actor slot `dest`'s mailbox. Returns 0 on
 * success, or -1 if: the calling actor doesn't hold a CAP_SEND
 * capability for dest (knowing an actor's slot index is no longer
 * enough -- message-passing philosophy §3/§4: actor identity is not
 * authority); dest is out of range or DEAD; or dest's mailbox is
 * already full -- backpressure: the sender is told immediately rather
 * than the message being silently dropped or the sender being
 * blocked (§16). */
int actor_send(int dest, uint64_t type, uint64_t data);

/* Receives the next message for the CALLING actor into *out, blocking
 * (suspending this actor so others can run) for as long as its
 * mailbox is empty. Always succeeds once it returns -- there is no
 * "mailbox empty" result, only "not yet". */
void actor_receive(struct message *out);

/* Kernel-only: unconditionally grants actor `dest` a capability for
 * `op` over `target` -- used during setup (kernel_main, before the
 * scheduler starts) to hand out each actor's initial, minimal
 * authority. Actors start holding nothing; this is the only way to
 * create a capability from nothing, which is exactly why it isn't
 * exposed as a syscall actor code can call. Returns 0 on success, -1
 * if dest is out of range or its capability table is already full. */
int actor_grant(int dest, int op, int target);

/* Kernel-only: raises (or lowers) actor `slot`'s own spawn quota above
 * the ordinary MAX_SPAWNS_PER_ACTOR default (core/actor.c) -- used
 * once, for the interactive shell alone (roadmap Phase 18), so its
 * genuinely open-ended spawning needs don't change what every other
 * actor's quota means (Coordinator's own demo depends on the default
 * staying exactly 2 -- see core/actor.c's own comment). Returns 0 on
 * success, -1 if slot is out of range. */
int actor_set_spawn_quota(int slot, int quota);

/* Phase 31: per-actor cap on how many storage objects it may create
 * (SYS_CREATE_NAME). actor_create_allowed() is checked before creating,
 * actor_note_create() after a success; actor_set_create_quota() is the
 * kernel-only override (kernel_main). */
int actor_create_allowed(void);
void actor_note_create(void);
int actor_set_create_quota(int slot, int quota);

/* Kernel-only: assigns actor `slot` to console pane `win` (hal.h's
 * CONSOLE_WIN_*) -- roadmap Phase 18 (revised), called once for the
 * shell alone so its prompt has a pane the scripted demo's own flood
 * of output can never touch. Every actor defaults to CONSOLE_WIN_LOG.
 * Returns 0 on success, -1 if slot is out of range. */
int actor_set_window(int slot, int win);

/* The CALLING actor's own window assignment, or CONSOLE_WIN_LOG if
 * called outside any actor's context -- used by the SYS_WRITE syscall
 * handler (hal/x86_64/syscall.c) to route output to the right pane. */
int actor_current_window(void);

/* Delegates a COPY of a capability the CALLING actor already holds to
 * actor slot `dest`. Fails (-1) if the caller doesn't hold {op,
 * target} itself -- delegation can only pass along authority already
 * held, never manufacture new authority (kernel security invariant:
 * capabilities cannot be forged by user code). Also fails if dest is
 * out of range or its capability table is full. */
int actor_delegate(int dest, int op, int target);

/* Returns 1 if the CALLING actor holds a capability for {op, target},
 * 0 otherwise (including if called outside any actor's context).
 * Exists so modules that mediate access to something other than
 * actors themselves -- e.g. hal/x86_64/syscall.c gating
 * SYS_OBJECT_READ/WRITE/PROMOTE against core/storage.c's objects --
 * can enforce capabilities without needing to know actors[]' internal
 * layout, or storage.c needing to know actors exist at all. */
int actor_current_has_cap(int op, int target);

/* The currently-running actor's own slot index, or -1 if called
 * outside any actor's context (kernel_main, before the scheduler
 * starts). Needed by core/net.c to stamp outgoing network messages
 * with a real, kernel-trusted sender identity -- the same "never
 * trust the sender to say who it is" rule struct message's own
 * sender field already follows locally. */
int actor_current_slot(void);

/* Phase 19: SYS_ACTOR_INFO's backing call. Fills *out for actor `slot`
 * if the CALLER holds CAP_INTROSPECT for it; -1 otherwise. */
struct actor_info;
int actor_get_info(int slot, struct actor_info *out);

/* Phase 23 made faults survivable; these make them COUNTABLE.
 * actor_note_fault() is called by hal/x86_64/interrupts.c each time it
 * terminates a ring-3 actor for a CPU fault; actor_fault_count() is what
 * SYS_FAULT_COUNT returns. */
void actor_note_fault(void);
int actor_fault_count(void);

/* Roadmap Phase 24: true only if [addr, addr+len) lies entirely inside
 * memory this actor's OWN address space actually maps present+user --
 * its stack, or (if it's a loaded program) its own PROGRAM_VBASE
 * window. hal/x86_64/syscall.c must call this before dereferencing ANY
 * pointer argument a syscall received from ring 3; see actor.c's own
 * comment for exactly what it closes off. len == 0 is always true
 * (nothing to check). */
int actor_current_owns_range(uint64_t addr, uint64_t len);

/* Roadmap Phase 24: the permissive counterpart to
 * actor_current_owns_range(), for syscall arguments the kernel only
 * READS (a string to print, a buffer to copy INTO an object) rather
 * than writes into. Allows everything owns_range() allows, plus the
 * kernel's own low image (below 1MB) -- where every built-in demo
 * actor's string literals actually live; see actor.c's own comment for
 * why that's safe to read but must never be a valid WRITE target. */
int actor_current_may_read_range(uint64_t addr, uint64_t len);

#endif
