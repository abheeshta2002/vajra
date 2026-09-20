#include "vajra/hal.h"
#include "vajra/memory.h"
#include "vajra/actor.h"
#include "vajra/loader.h"
#include "vajra/storage.h"

/* ------------------------------------------------------------------
 * Cooperative round-robin scheduler.
 *
 * This is the first genuinely portable design piece beyond boot
 * infrastructure -- everything here works purely in terms of saved
 * stack pointers via hal_context_switch(), the one architecture-
 * specific primitive this needs. The scheduling policy itself (round
 * robin), the actor table, spawn/yield/exit -- none of it is
 * x86-specific, and this file should work unchanged on a future
 * AArch64 port once hal_context_switch has an ARM implementation.
 *
 * Now preemptive: hal/x86_64/interrupts.c calls actor_yield() itself
 * from the timer IRQ, on top of the exact same hal_context_switch
 * primitive and the exact same schedule_next() actors call
 * voluntarily -- nothing here needed to be thrown away to add that,
 * as expected when this was first written.
 *
 * Preemption means schedule_next()'s state mutations (actors[],
 * current_actor) can now happen at any point actor code is running,
 * not only at explicit yield/exit call sites -- so every entry point
 * that touches them (actor_yield, actor_exit, schedule_next itself)
 * disables interrupts first and re-enables them only once execution
 * genuinely resumes, making each switch atomic with respect to the
 * timer. A freshly spawned actor is the one case that doesn't reach
 * that "resume" point through schedule_next() at all -- its fake
 * frame's `ret` lands directly in application code the first time --
 * so actor_trampoline() exists purely to re-enable interrupts there
 * too before running the actor's real entry point.
 *
 * Now also isolated: each actor gets its own address space (its own
 * CR3, built by hal_address_space_create() -- see hal/x86_64/paging.c)
 * whose only actor-specific mapping is that actor's own stack. No
 * actor's page tables contain any OTHER actor's memory at all, so one
 * actor cannot read, corrupt, or even address another's stack.
 * hal_context_switch() switches CR3 alongside RSP in one atomic step,
 * so "which stack is active" and "which address space is active" can
 * never disagree.
 *
 * And now genuinely privileged: actor code runs at CPL 3 (ring 3),
 * not CPL 0 -- actor_trampoline() drops it there via
 * hal_enter_user_mode() the first time it ever runs. This is the
 * hardware boundary the isolation above was still missing: a CPL 3
 * actor cannot load a different CR3, execute `cli`/`hlt`, or call any
 * ordinary kernel function directly (kernel code is supervisor-only
 * memory, unreachable from ring 3 by construction) no matter how
 * deliberately it tries. The ONLY way back into the kernel is the
 * syscall gate (`int 0x80`, vector 0x80, DPL=3 -- see
 * hal/x86_64/interrupts.c and hal.h's SYS_* constants). Every actor
 * also gets its own dedicated kernel stack (hal_set_kernel_stack(),
 * TSS.RSP0) separate from its user stack: whenever a syscall or
 * interrupt arrives while an actor is running in ring 3, the CPU
 * switches to that stack automatically, and since MULTIPLE actors can
 * be suspended mid-syscall at once (exactly like being suspended
 * mid-yield always could), one shared kernel stack would let one
 * actor's syscall silently corrupt another's saved state.
 *
 * And now actors can talk to each other: actor_send()/actor_receive()
 * give each actor a bounded mailbox instead of unrestricted shared
 * memory (there isn't any to share, post-Milestone-5, but the point
 * stands architecturally -- see the project's message-passing
 * philosophy §1). A full mailbox rejects the sender immediately
 * (§16: unbounded sending must not be free), and receiving with an
 * empty mailbox suspends the calling actor (ACTOR_BLOCKED) exactly
 * like a normal yield, waking back to READY the moment a message
 * arrives -- reusing schedule_next() and hal_context_switch()
 * unchanged, same as every other suspension this scheduler already
 * knows how to do.
 *
 * And now capability-checked: knowing a slot index is no longer
 * sufficient to message it. actor_send() requires the CALLER to hold
 * a CAP_SEND capability naming that exact destination -- actor
 * identity and authority are explicitly different things (message-
 * passing philosophy §3/§4). Every actor starts holding nothing;
 * actor_grant() is the only way to create a capability from nothing,
 * and it's kernel-only (called from kernel_main, never exposed as a
 * syscall), so the initial authority handed to each actor is always
 * an explicit, auditable decision, never a default. actor_delegate()
 * lets an actor pass a copy of a capability it already holds to
 * another actor -- deliberately unable to manufacture authority
 * beyond what the delegator itself has, which is the one soundness
 * property a capability system cannot compromise on.
 *
 * And now actors can spawn actors: actor_spawn_child() (the roadmap's
 * "ghost actor" primitive, philosophy §6) is what SYS_SPAWN calls --
 * capability-checked (CAP_SPAWN) and quota-limited
 * (MAX_SPAWNS_PER_ACTOR), unlike the raw actor_spawn() kernel_main
 * uses for its own unconditional initial setup. Spawning
 * auto-grants the natural relationship a worker and its creator need
 * -- the spawner can message and terminate what it created, and the
 * new actor can message its spawner back -- without a fresh grant
 * dance for every worker. actor_terminate() is the other half:
 * capability-checked (CAP_TERMINATE) forcible termination of another
 * actor, for cleaning up a worker that won't exit itself.
 * ---------------------------------------------------------------- */

typedef enum {
    ACTOR_DEAD = 0,
    ACTOR_READY,
    ACTOR_RUNNING,
    ACTOR_BLOCKED, /* waiting on actor_receive() with an empty mailbox */
    ACTOR_SLEEPING /* waiting for hal_ticks() to reach wake_tick -- see actor_sleep() */
} actor_state_t;

#define MAILBOX_CAPACITY 8
/* Roadmap Phase 18 (Milestone 18) raised the capability-table size for
 * the new interactive shell actor (core/main.c's actor_shell()), which
 * spawns across an open-ended interactive session (the `count`/`pipe`
 * built-ins) rather than a small fixed number of workers per scripted
 * demo run the way Scanner/Coordinator do -- each spawn auto-grants 2
 * more capabilities (CAP_SEND+CAP_TERMINATE) for the new actor.
 * Scanner's own old ceiling (6 static grants + 2 inspectors x 2 = 10,
 * needing headroom to 12) stays valid but is no longer the tightest
 * case: the shell's 4 static grants (CAP_CONSOLE, CAP_SPAWN,
 * CAP_LIST_NAMES, CAP_READ_OBJECT) + up to 6 spawns x 2 = 16 needed;
 * 20 kept for real headroom, since there's still no capability-table
 * reclamation on actor death (Milestone 8's own long-open follow-up).
 *
 * MAX_SPAWNS_PER_ACTOR itself stays at the original 2, deliberately --
 * NOT raised globally. Coordinator's own existing demo (core/main.c)
 * explicitly proves a 3rd spawn is denied at quota 2; raising this
 * constant broke that proof outright (a real regression caught by
 * actually re-running the boot trace, not assumed safe). The shell
 * instead gets its own PER-ACTOR override -- see spawn_quota below and
 * actor_set_spawn_quota() -- so its genuinely different, open-ended
 * spawning needs don't change what every other actor's quota means. */
#define MAX_CAPS_PER_ACTOR 20
#define MAX_CREATES_PER_ACTOR 2 /* Phase 31: objects one actor may ever create, unless raised */
#define MAX_SPAWNS_PER_ACTOR 2 /* fork-bomb guard, and Coordinator's own demo default -- see
                                   actor_spawn_child() and spawn_quota's own comment below */

/* One unit of authority: the right to perform `op` on/toward `target`.
 * op == 0 marks an empty slot -- CAP_SEND is defined as 1 in actor.h
 * specifically so a zeroed struct (every actor starts this way) reads
 * as "holds nothing," not as an accidental capability. */
struct capability {
    int op;
    int target;
    int target_gen; /* roadmap Phase 25: the target's generation at the moment this capability was
                        granted -- 0 for a blanket capability (target itself is the placeholder 0,
                        not a real actor/object identity, e.g. CAP_SPAWN). actor_has_cap() refuses
                        a match unless this still equals the target's CURRENT generation, so a
                        capability recorded against an earlier occupant of a reused actor slot or
                        object id can never silently apply to whatever replaced it. */
};

struct actor {
    uint64_t rsp;
    uint64_t cr3; /* this actor's own private address space -- see hal_address_space_create() */
    actor_state_t state;
    void *stack_page; /* this actor's ring-3 USER stack; freed on reap once DEAD (see
                          reap_dead_actors) -- the separate ring-0 kernel stack used for
                          syscalls/interrupts is static, per-slot, and never freed; see
                          hal_get_kernel_stack_top()/hal_set_kernel_stack() */
    void (*entry)(void); /* read by actor_trampoline() the first time this actor ever runs */
    struct message mailbox[MAILBOX_CAPACITY]; /* ring buffer; see actor_send()/actor_receive() */
    int mailbox_head;
    int mailbox_count;
    struct capability caps[MAX_CAPS_PER_ACTOR]; /* see actor_grant()/actor_delegate() */
    int spawn_count; /* actors created via actor_spawn_child() so far -- see spawn_quota */
    int spawn_quota; /* roadmap Phase 18: per-actor override of MAX_SPAWNS_PER_ACTOR, defaulted
                         to it for every actor (scheduler_init()/actor_spawn()) and raised only
                         for the shell (actor_set_spawn_quota(), called once from kernel_main) --
                         see MAX_CAPS_PER_ACTOR's own comment for why a global raise wasn't the
                         right fix. */
    int create_count; /* Phase 31: objects created via SYS_CREATE_NAME so far -- see create_quota */
    int create_quota; /* Phase 31: how many objects this actor may ever create. The store is tiny and
                          shared (MAX_OBJECTS), so an actor holding CAP_CREATE_OBJECT could otherwise
                          fill it and starve everyone else -- found by the adversary campaign (Security
                          Lab 'a'). Same lifetime-count shape as spawn_quota; default
                          MAX_CREATES_PER_ACTOR, raised per actor by actor_set_create_quota(). */
    int window; /* roadmap Phase 18 (revised): which console pane (hal.h's CONSOLE_WIN_*) this
                    actor's SYS_WRITE output lands in. Defaults to CONSOLE_WIN_LOG for every
                    actor (scheduler_init()/actor_spawn()); only the shell gets raised to
                    CONSOLE_WIN_SHELL, via actor_set_window() -- same kernel-only-override
                    convention as spawn_quota above. */
    int generation; /* roadmap Phase 25: bumped every time actor_spawn() hands this SLOT out --
                        including the first spawn ever, so generation 0 never means "a real
                        actor," the same convention storage.c's own object generation follows.
                        See actor_has_cap()'s own comment. */
    uint64_t wake_tick; /* ACTOR_SLEEPING only: the hal_ticks() value at which it becomes READY */
    int caps_reclaimed; /* Phase 10 follow-up: set once reap_dead_actors() has cleared every OTHER
                            actor's CAP_SEND/CAP_TERMINATE entries that named this (now dead)
                            slot -- 1 for a slot that has never held an actor, 0 from spawn until
                            its actor dies and is reaped. See reclaim_caps_for(). */
    int kill_pending; /* Phase 10: actor_terminate() found this actor RUNNING on ANOTHER core --
                          it can't be torn down under that core's feet, so it's flagged and
                          exits itself at its next kernel entry (actor_check_pending_kill()),
                          which its core's timer tick guarantees within one period. */
    uint64_t program_size; /* roadmap Phase 24: 0 for an ordinary actor: this one owns no memory
                               at PROGRAM_VBASE at all. Set by actor_spawn_program() once
                               hal_address_space_map_program() succeeds -- the ONLY other range
                               (besides its own stack, stack_page/ACTOR_STACK_SIZE) a syscall
                               pointer argument from this actor is allowed to name; see
                               actor_current_owns_range() below. Reset to 0 on every fresh spawn
                               so a slot reused by a plain actor_spawn() doesn't inherit a
                               previous occupant's loaded-program window. */
};

#define ACTOR_STACK_SIZE 4096  /* one page; plenty for now, revisit when actors do more */

static struct actor actors[MAX_ACTORS];

/* Phase 10 (remainder): every core runs actors, so "the running actor"
 * and "the scheduler's saved context" are per-core. `current_actor`
 * stays a name (it is used all over this file) but is now an lvalue
 * into the executing core's own slot. Everything below runs under the
 * kernel lock (hal/x86_64/cpu.c), which is what makes reading another
 * core's slot -- as the reaper and terminate do -- safe. */
static int cur_slot[MAX_CPUS] = { -1, -1, -1, -1 };
#define current_actor (cur_slot[hal_cpu_id()])
static uint64_t sched_rsp[MAX_CPUS];   /* each core's scheduler context, saved while an actor runs */
static int last_pick[MAX_CPUS];        /* round-robin cursor, per core */
static uint64_t core_switches[MAX_CPUS]; /* actors this core has switched into (for the Cores app) */
static uint64_t core_idle_ticks[MAX_CPUS]; /* times this core found nothing to run and halted */
static volatile int sched_go = 0;      /* set by the BSP once actors exist; the AP waits on it */

/* The kernel lock keeps interrupts' critical sections exclusive across
 * cores now, so the enable calls that used to end every critical
 * section below would only open a preemption window INSIDE kernel code
 * (with the lock held). Interrupt state is restored by iretq on the way
 * back to ring 3 instead. */
#define hal_enable_interrupts() ((void)0)

/* The `ret` target baked into every actor's fake initial frame,
 * instead of its real entry point directly. Runs once, in ring 0 (the
 * fake frame lands here via a plain `ret`, not a privilege-changing
 * iretq), on this actor's own dedicated kernel stack -- and its whole
 * job is to leave ring 0 immediately: hal_enter_user_mode() drops to
 * ring 3 and lands at the actor's real entry() with its own user
 * stack, and never returns here. From that point on, this actor's
 * kernel stack is only ever touched again by a syscall or interrupt
 * arriving while it's running in ring 3 (see this file's top comment)
 * -- never by application code directly.
 *
 * (An already-running actor, by contrast, resumes via
 * hal_context_switch() returning into schedule_next(), which
 * re-enables interrupts once execution genuinely gets back there --
 * this function is only ever reached the FIRST time a given actor
 * runs, which is why it's the one place that has to think about
 * dropping to ring 3 at all.) */
static void actor_trampoline(void) {
    struct actor *self = &actors[current_actor];
    /* Reached from this core's scheduler loop, which holds the kernel
     * lock; going to ring 3 is where a core lets go of it. (A resumed
     * actor releases it on its way out of its own syscall/ISR frame
     * instead -- see hal_kernel_leave()'s callers.) */
    hal_kernel_leave(1);
    hal_enter_user_mode((uint64_t)self->entry, (uint64_t)self->stack_page + ACTOR_STACK_SIZE);
}

void scheduler_init(void) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        actors[i].state = ACTOR_DEAD;
        actors[i].rsp = 0;
        actors[i].cr3 = 0;
        actors[i].stack_page = 0;
        actors[i].entry = 0;
        actors[i].mailbox_head = 0;
        actors[i].mailbox_count = 0;
        actors[i].spawn_count = 0;
        actors[i].spawn_quota = MAX_SPAWNS_PER_ACTOR;
        actors[i].create_count = 0;
        actors[i].create_quota = MAX_CREATES_PER_ACTOR;
        actors[i].window = CONSOLE_WIN_LOG;
        actors[i].kill_pending = 0;
        actors[i].caps_reclaimed = 1; /* nothing ever lived here, so no capability names it */
        for (int j = 0; j < MAX_CAPS_PER_ACTOR; j++) {
            actors[i].caps[j].op = 0;
            actors[i].caps[j].target = 0;
        }
    }
    for (int c = 0; c < MAX_CPUS; c++) {
        cur_slot[c] = -1;
    }
}

/* Roadmap Phase 25: the CURRENT generation of `target` under `op`'s
 * own namespace -- actor slot for the two actor-targeted ops, object
 * id for the object-targeted ones, and a fixed 0 for every blanket op
 * (CAP_SPAWN etc.), whose target is always the placeholder 0, not a
 * real identity. Shared by actor_add_cap() (recording what generation
 * a new capability was granted against) and actor_has_cap() (checking
 * a stored capability against what's actually there NOW) -- the same
 * lookup on both sides is what makes a stale capability start failing
 * the instant the slot/id it named gets reused, not just eventually. */
static int current_generation_of(int op, int target) {
    switch (op) {
        case CAP_SEND:
        case CAP_TERMINATE:
            if (target < 0 || target >= MAX_ACTORS) {
                return -1; /* never matches a real capability's recorded generation */
            }
            return actors[target].generation;

        case CAP_READ_OBJECT:
        case CAP_WRITE_OBJECT:
        case CAP_RENAME_OBJECT:
        case CAP_DELETE_OBJECT:
            return storage_object_generation(target); /* -1 for an out-of-range id, same reasoning */

        default:
            return 0; /* blanket op -- target is the placeholder 0, not a real identity */
    }
}

static int actor_has_cap(int slot, int op, int target) {
    int current_gen = current_generation_of(op, target);
    for (int i = 0; i < MAX_CAPS_PER_ACTOR; i++) {
        if (actors[slot].caps[i].op == op && actors[slot].caps[i].target == target &&
            actors[slot].caps[i].target_gen == current_gen) {
            return 1;
        }
    }
    return 0;
}

static int actor_add_cap(int slot, int op, int target) {
    int target_gen = current_generation_of(op, target);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MAX_CAPS_PER_ACTOR; i++) {
            if (actors[slot].caps[i].op == 0) {
                actors[slot].caps[i].op = op;
                actors[slot].caps[i].target = target;
                actors[slot].caps[i].target_gen = target_gen;
                return 0;
            }
        }
        if (pass == 1) {
            break;
        }
        /* Table full: sweep entries whose target has since been reused (a
         * deleted object's id handed to someone else). actor_has_cap()
         * already refuses them -- they can never work again -- so they are
         * pure dead weight, and without the sweep a few create/delete
         * cycles would exhaust the table for good. Swept only when needed,
         * never eagerly, so the generation check itself stays what stops
         * a stale capability. */
        for (int i = 0; i < MAX_CAPS_PER_ACTOR; i++) {
            if (actors[slot].caps[i].op != 0 &&
                actors[slot].caps[i].target_gen !=
                    current_generation_of(actors[slot].caps[i].op, actors[slot].caps[i].target)) {
                actors[slot].caps[i].op = 0;
                actors[slot].caps[i].target = 0;
            }
        }
    }
    return -1; /* table full of live capabilities */
}

int actor_grant(int dest, int op, int target) {
    if (dest < 0 || dest >= MAX_ACTORS) {
        return -1;
    }
    return actor_add_cap(dest, op, target);
}

/* Phase 31: the object-creation quota (see create_quota above). The
 * syscall handler asks first, creates, and counts only a success, so a
 * name collision doesn't burn quota. */
int actor_create_allowed(void) {
    return actors[current_actor].create_count < actors[current_actor].create_quota;
}

void actor_note_create(void) {
    actors[current_actor].create_count++;
}

/* Kernel-only, same convention as actor_set_spawn_quota(). */
int actor_set_create_quota(int slot, int quota) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return -1;
    }
    actors[slot].create_quota = quota;
    return 0;
}

/* Kernel-only, unconditional -- same convention as actor_grant(),
 * called once from kernel_main to raise the shell's own spawn quota
 * above the ordinary default. See spawn_quota's own comment (struct
 * actor above) and MAX_CAPS_PER_ACTOR's comment for why this is a
 * per-actor override rather than a global constant change. Returns 0
 * on success, -1 if slot is out of range. */
int actor_set_spawn_quota(int slot, int quota) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return -1;
    }
    actors[slot].spawn_quota = quota;
    return 0;
}

/* Kernel-only, unconditional -- same convention as
 * actor_set_spawn_quota() above, called once from kernel_main to give
 * the shell its own console pane. Returns 0 on success, -1 if slot is
 * out of range. */
int actor_set_window(int slot, int win) {
    if (slot < 0 || slot >= MAX_ACTORS) {
        return -1;
    }
    actors[slot].window = win;
    return 0;
}

/* The CALLING actor's own window assignment (hal.h's CONSOLE_WIN_*) --
 * CONSOLE_WIN_LOG if called outside any actor's context (kernel_main,
 * before the scheduler starts), matching every actor's own default. */
int actor_current_window(void) {
    if (current_actor < 0) {
        return CONSOLE_WIN_LOG;
    }
    return actors[current_actor].window;
}

int actor_current_has_cap(int op, int target) {
    if (current_actor < 0) {
        return 0;
    }
    return actor_has_cap(current_actor, op, target);
}

int actor_current_slot(void) {
    return current_actor;
}

static int faults_contained = 0;
void actor_note_fault(void) {
    faults_contained++;
}
int actor_fault_count(void) {
    return faults_contained;
}

/* Roadmap Phase 24: the syscall boundary's own bounds checks. Every
 * syscall pointer argument is an ACTOR virtual address, walked at CPL 0
 * (syscalls don't switch CR3) -- and CPL 0 ignores the U/S bit, so
 * without this, a pointer into 0-1MB/2MB-256MB "commons"
 * (hal/x86_64/paging.c's own comment: identity-mapped and PRESENT in
 * every address space, marked supervisor-only only because ring-3
 * *code* is barred from it, not ring-0 *data* accesses) reads/writes
 * arbitrary kernel memory: the actors[] array itself, page tables,
 * anything.
 *
 * Two checks, not one, because the two directions carry very different
 * risk. When the kernel is about to WRITE into a syscall's pointer
 * argument (SYS_RECEIVE, SYS_OBJECT_READ's dest, SYS_RTC_READ, ...),
 * anything other than this actor's OWN memory is real corruption --
 * of another actor's state, or of the kernel itself. actor_current_
 * owns_range() is that strict check: only the two ranges genuinely
 * private to this actor, its own stack (stack_page/ACTOR_STACK_SIZE)
 * and, if it's a loaded program, its own code+data window
 * (PROGRAM_VBASE/program_size) -- exactly what hal_address_space_
 * create()/hal_address_space_map_program() ever mapped present+user
 * for it.
 *
 * When the kernel is about to READ a syscall's pointer argument
 * (SYS_WRITE's string, SYS_OBJECT_WRITE's source buffer, ...), the
 * risk is narrower -- disclosure and a wild-pointer crash, not
 * corruption -- and the kernel's own low image (link.ld: 0x20000 up to
 * well under 1MB, .text/.rodata/.bss) is where every built-in demo
 * actor's own string literals genuinely live (core/actor.c's own
 * top-of-file comment: taking their address is unrestricted at any
 * privilege level; this file's paging.c counterpart is what decided
 * .rodata never needed to move into a per-actor window). Refusing
 * reads there would break every one of those calls for no real safety
 * gain -- nothing actor-private lives below 1MB, only the kernel
 * image. actor_current_may_read_range() allows that plus everything
 * owns_range() already allows; it still refuses another actor's
 * private 1MB-2MB slot (the one place real cross-actor secrets live)
 * and anything past 2MB that isn't this actor's own program window
 * (where an unmapped hole would #PF the kernel outright). */
int actor_current_owns_range(uint64_t addr, uint64_t len) {
    if (current_actor < 0) {
        return 0;
    }
    if (len == 0) {
        return 1; /* nothing to read or write */
    }
    uint64_t end = addr + len;
    if (end < addr) {
        return 0; /* overflow -- addr+len wrapped past UINT64_MAX */
    }

    struct actor *a = &actors[current_actor];

    uint64_t stack_base = (uint64_t)a->stack_page;
    if (addr >= stack_base && end <= stack_base + ACTOR_STACK_SIZE) {
        return 1;
    }

    if (a->program_size != 0 && addr >= PROGRAM_VBASE && end <= PROGRAM_VBASE + a->program_size) {
        return 1;
    }

    return 0;
}

#define KERNEL_IMAGE_END 0x100000ULL /* 1MB -- must match core/memory.c's MEM_BASE and
                                         hal/x86_64/paging.c's COMMONS_END: everything below
                                         here is the kernel image itself (link.ld), never another
                                         actor's private memory (that only ever starts AT this
                                         address) -- see actor_current_may_read_range()'s own
                                         comment above. */

int actor_current_may_read_range(uint64_t addr, uint64_t len) {
    if (current_actor < 0) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }
    uint64_t end = addr + len;
    if (end < addr) {
        return 0; /* overflow */
    }

    if (addr < KERNEL_IMAGE_END && end <= KERNEL_IMAGE_END) {
        return 1; /* the kernel image itself -- .rodata string literals live here, see above */
    }

    return actor_current_owns_range(addr, len);
}

int actor_delegate(int dest, int op, int target) {
    if (dest < 0 || dest >= MAX_ACTORS) {
        return -1;
    }

    hal_disable_interrupts();

    if (!actor_has_cap(current_actor, op, target)) {
        hal_enable_interrupts();
        return -1; /* cannot delegate authority you don't hold -- see this file's top comment */
    }

    int rc = actor_add_cap(dest, op, target);

    hal_enable_interrupts();
    return rc;
}

int actor_spawn(void (*entry)(void)) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        if (actors[i].state != ACTOR_DEAD) {
            continue;
        }

        void *stack_mem = alloc_page();
        if (!stack_mem) {
            return -1;
        }

        /* This actor's own private address space: its stack is the
         * only thing mapped anywhere in the 1MB-2MB window (see
         * hal_address_space_create()'s own comment), so a stack
         * overflow of any size faults immediately -- there's no
         * separate physical guard page to arrange here the way a
         * single shared address space would have needed, because
         * every OTHER address in that window, including every other
         * actor's own stack, is simply not present in this one. */
        uint64_t cr3 = hal_address_space_create(i, (uint64_t)stack_mem, ACTOR_STACK_SIZE);
        if (!cr3) {
            /* Refuse to spawn an actor with no address space rather
             * than silently give it the wrong one. */
            free_page(stack_mem);
            return -1;
        }

        /* The fake frame below is built on this actor's dedicated
         * KERNEL stack (hal_get_kernel_stack_top), not its user stack
         * (stack_mem) -- actor_trampoline() runs in ring 0, and this
         * is also the same stack every future syscall/interrupt this
         * actor takes from ring 3 will land on (see this file's top
         * comment and hal_set_kernel_stack's own). stack_mem is purely
         * the ring-3 user stack now, handed to hal_enter_user_mode()
         * by actor_trampoline() once it's ready to drop to ring 3. */
        uint64_t *sp = (uint64_t *)hal_get_kernel_stack_top(i);

        /* Build a fake "previously suspended" frame matching exactly
         * what hal_context_switch's own push sequence produces, so
         * its pop sequence + ret lands on actor_trampoline() the first
         * time this actor is switched to. See context_switch.asm's
         * comment for the full explanation, and actor_trampoline's own
         * comment for why it targets the trampoline instead of entry
         * directly. */
        *(--sp) = (uint64_t)actor_trampoline; /* return address for the final `ret` */
        *(--sp) = 0; /* rbx */
        *(--sp) = 0; /* rbp */
        *(--sp) = 0; /* r12 */
        *(--sp) = 0; /* r13 */
        *(--sp) = 0; /* r14 */
        *(--sp) = 0; /* r15 */

        actors[i].rsp = (uint64_t)sp;
        actors[i].cr3 = cr3;
        actors[i].stack_page = stack_mem;
        actors[i].entry = entry;
        actors[i].mailbox_head = 0;
        actors[i].mailbox_count = 0;
        actors[i].create_count = 0;
        actors[i].create_quota = MAX_CREATES_PER_ACTOR;
        actors[i].spawn_count = 0;
        actors[i].spawn_quota = MAX_SPAWNS_PER_ACTOR; /* a fresh occupant of a reused slot gets
                                                           the ordinary default, never inherits
                                                           a predecessor's raised quota */
        actors[i].window = CONSOLE_WIN_LOG; /* same reasoning -- a fresh occupant never inherits
                                                a predecessor's window assignment */
        actors[i].program_size = 0; /* same reasoning -- a fresh occupant never inherits a
                                        predecessor's loaded-program window */
        actors[i].kill_pending = 0;
        actors[i].caps_reclaimed = 0;
        actors[i].generation++; /* Phase 25: every hand-out of this slot, first included -- see
                                    struct actor's own comment */
        /* This slot may be reused from a previous, now-DEAD occupant
         * (scheduler_init() only zeroes capabilities once, at boot) --
         * without this, a freshly spawned actor would silently inherit
         * whatever authority the slot's previous occupant happened to
         * still hold, a real capability leak across actor identities. */
        for (int j = 0; j < MAX_CAPS_PER_ACTOR; j++) {
            actors[i].caps[j].op = 0;
            actors[i].caps[j].target = 0;
        }
        actors[i].state = ACTOR_READY;
        return i;
    }

    return -1;
}

/* Roadmap Phase 16: the raw primitive, unconditional -- exactly the
 * same relationship to actor_spawn_program_child() below that
 * actor_spawn() has to actor_spawn_child() above, and shares that same
 * function for everything except the address space: entry is computed
 * as a real address inside the newly loaded program (PROGRAM_VBASE +
 * entry_offset, include/vajra/loader.h), not a kernel-linked function
 * pointer, and hal_address_space_map_program() adds that program's
 * memory to the new actor's address space on top of the ordinary stack
 * actor_spawn() already gives it. Callable only from core/loader.c,
 * which has already validated phys_size against PROGRAM_WINDOW_MAX --
 * the cleanup path below exists for defense in depth, not because
 * that validated caller is expected to trigger it. */
int actor_spawn_program(uint64_t phys_base, uint64_t phys_size, uint32_t entry_offset) {
    void (*entry)(void) = (void (*)(void))(PROGRAM_VBASE + entry_offset);

    int slot = actor_spawn(entry);
    if (slot < 0) {
        return -1;
    }

    if (hal_address_space_map_program(slot, phys_base, phys_size) != 0) {
        actors[slot].state = ACTOR_DEAD; /* stack_page reclaimed by reap_dead_actors(), same
                                             path any other dead actor's stack already takes */
        return -1;
    }

    actors[slot].program_size = phys_size; /* Phase 24: now a legal target for this actor's own
                                                syscall pointer args, [PROGRAM_VBASE, +phys_size) --
                                                see actor_current_owns_range() */

    return slot;
}

/* The capability-checked, quota-limited counterpart callers actually
 * reach (via core/loader.c, itself reached through SYS_SPAWN_PROGRAM)
 * -- identical CAP_SPAWN + quota + parent/child auto-grant logic to
 * actor_spawn_child() above, just calling actor_spawn_program() instead
 * of actor_spawn(). Deliberately duplicated rather than shared through
 * one more layer of indirection: the two checked wrappers are five
 * lines of genuinely identical logic around two DIFFERENT raw calls,
 * not a case where extracting a helper would remove real duplication
 * versus just renaming it. */
int actor_spawn_program_child(uint64_t phys_base, uint64_t phys_size, uint32_t entry_offset) {
    hal_disable_interrupts();

    if (!actor_has_cap(current_actor, CAP_SPAWN, 0)) {
        hal_enable_interrupts();
        return -1;
    }

    if (actors[current_actor].spawn_count >= actors[current_actor].spawn_quota) {
        hal_enable_interrupts();
        return -1;
    }

    int spawner = current_actor;
    int child = actor_spawn_program(phys_base, phys_size, entry_offset);
    if (child < 0) {
        hal_enable_interrupts();
        return -1;
    }

    actors[spawner].spawn_count++;
    actor_add_cap(spawner, CAP_SEND, child);
    actor_add_cap(spawner, CAP_TERMINATE, child);
    actor_add_cap(child, CAP_SEND, spawner);

    hal_enable_interrupts();
    return child;
}

int actor_spawn_child(void (*entry)(void)) {
    hal_disable_interrupts();

    if (!actor_has_cap(current_actor, CAP_SPAWN, 0)) {
        hal_enable_interrupts();
        return -1; /* not authorized to spawn at all -- see this file's top comment */
    }

    if (actors[current_actor].spawn_count >= actors[current_actor].spawn_quota) {
        hal_enable_interrupts();
        return -1; /* quota exceeded -- fork-bomb guard */
    }

    int spawner = current_actor;
    int child = actor_spawn(entry);
    if (child < 0) {
        hal_enable_interrupts();
        return -1;
    }

    actors[spawner].spawn_count++;

    /* The natural parent/child relationship a ghost actor and its
     * creator need -- see this file's top comment. actor_add_cap()
     * failing here (a full table) is not treated as spawn failure:
     * the child already exists and is running: better a worker with
     * slightly less convenience than one silently leaked. */
    actor_add_cap(spawner, CAP_SEND, child);
    actor_add_cap(spawner, CAP_TERMINATE, child);
    actor_add_cap(child, CAP_SEND, spawner);

    hal_enable_interrupts();
    return child;
}

int actor_terminate(int target) {
    if (target < 0 || target >= MAX_ACTORS || target == current_actor) {
        return -1; /* use actor_exit()/SYS_EXIT to end yourself */
    }

    hal_disable_interrupts();

    if (!actor_has_cap(current_actor, CAP_TERMINATE, target)) {
        hal_enable_interrupts();
        return -1; /* not authorized -- see this file's top comment */
    }

    /* Whatever target was doing (including suspended mid-syscall on
     * its own kernel stack) is simply abandoned -- the scheduler
     * never picks a DEAD actor again, and reap_dead_actors()
     * reclaims its user stack the same way any other exit does.
     *
     * Phase 10: target CAN be ACTOR_RUNNING now -- on the other core,
     * in ring 3 (we hold the kernel lock, so it isn't in the kernel).
     * Freeing its stack or address space under a core that is
     * executing on them would crash it, so it's only flagged; it
     * exits itself at its next kernel entry (its core's next tick at
     * the latest). Success is reported either way -- the kill is
     * decided, just not yet carried out. */
    if (actors[target].state == ACTOR_RUNNING) {
        actors[target].kill_pending = 1;
    } else {
        actors[target].state = ACTOR_DEAD;
    }

    hal_enable_interrupts();
    return 0;
}

/* Frees the stack page of any actor that has exited, except
 * current_actor -- whichever actor is calling schedule_next() right
 * now might be the one that just called actor_exit(), and it is still
 * running on its own stack until the hal_context_switch() call below
 * actually moves off it. Any OTHER dead actor's stack is guaranteed
 * unused (nothing has run on it since it died) and safe to free here. */
static int actor_running_on_any_core(int slot) {
    for (int c = 0; c < MAX_CPUS; c++) {
        if (cur_slot[c] == slot) {
            return 1;
        }
    }
    return 0;
}

/* Capability tables have no other reclamation (Milestone 8's long-open
 * follow-up): every spawn adds a CAP_SEND and a CAP_TERMINATE to the
 * spawner, and a table holds MAX_CAPS_PER_ACTOR entries, so a parent
 * that spawns and reaps children in a loop -- the Cores app's burners
 * and kill-test spinners -- silently ran out after ~9 children and its
 * later terminates were refused for want of a capability it had been
 * granted (found by the kill test: "29 launched, 9 killed"). A
 * capability naming a dead actor is already useless (a send fails, and
 * the generation check retires it the moment the slot is reused), so
 * dropping it once the actor is reaped removes nothing anyone could
 * still use -- it only makes room. */
static void reclaim_caps_for(int dead_slot) {
    for (int a = 0; a < MAX_ACTORS; a++) {
        for (int j = 0; j < MAX_CAPS_PER_ACTOR; j++) {
            struct capability *c = &actors[a].caps[j];
            if ((c->op == CAP_SEND || c->op == CAP_TERMINATE) && c->target == dead_slot) {
                c->op = 0;
                c->target = 0;
                c->target_gen = 0;
            }
        }
    }
}

static void reap_dead_actors(void) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        if (actor_running_on_any_core(i)) {
            continue;
        }
        if (actors[i].state == ACTOR_DEAD && !actors[i].caps_reclaimed) {
            reclaim_caps_for(i);
            actors[i].caps_reclaimed = 1;
        }
        if (actors[i].state == ACTOR_DEAD && actors[i].stack_page) {
            free_page(actors[i].stack_page);
            actors[i].stack_page = 0;
        }
        if (actors[i].state == ACTOR_DEAD && actors[i].program_size != 0) {
            /* Roadmap Phase 26: the as_pt1 program-window pool entry
             * (hal/x86_64/paging.c, PROGRAM_POOL_SIZE == 2) this actor
             * held, if any, freed the same instant its ordinary stack
             * page is -- see hal_address_space_release_program()'s own
             * comment. program_size == 0 is the same "already handled"
             * sentinel stack_page's own 0-check above uses, so this
             * never double-releases a slot that already gave its entry
             * back. */
            hal_address_space_release_program(i);
            actors[i].program_size = 0;
        }
    }
}

/* Phase 10: each core runs a scheduler LOOP on its own stack (the BSP's
 * boot stack, the AP's boot stack), and actors never switch directly
 * into one another -- they switch back to their core's loop, which
 * picks the next READY actor. That costs a second context switch per
 * reschedule but buys three things a direct actor-to-actor switch
 * can't give a multicore kernel: (1) there is somewhere to IDLE when
 * nothing is runnable (an actor that just blocked must not keep
 * sitting on its own kernel stack -- another core may wake and resume
 * it at any moment); (2) a dead actor is reaped from a stack that is
 * not its own; (3) an actor is only ever resumed by a core that took
 * it off the run queue under the kernel lock, so two cores can never
 * run one actor.
 *
 * Entered with the kernel lock held and interrupts off. The lock is
 * held across every switch (it belongs to the core, not to an actor)
 * and dropped only when a core goes to ring 3 or idles. */
static void scheduler_loop(void) {
    int cpu = hal_cpu_id();
    int warned_empty = 0;

    for (;;) {
        reap_dead_actors();

        uint64_t now = hal_ticks();
        for (int i = 0; i < MAX_ACTORS; i++) {
            if (actors[i].state == ACTOR_SLEEPING && now >= actors[i].wake_tick) {
                actors[i].state = ACTOR_READY;
            }
        }

        int next = -1;
        int live = 0;
        for (int i = 0; i < MAX_ACTORS; i++) {
            if (actors[i].state != ACTOR_DEAD) {
                live++;
            }
        }
        for (int i = 1; i <= MAX_ACTORS; i++) {
            int idx = (last_pick[cpu] + i) % MAX_ACTORS;
            if (actors[idx].state == ACTOR_READY) {
                next = idx;
                break;
            }
        }

        if (next == -1) {
            if (live == 0 && !warned_empty) {
                warned_empty = 1;
                hal_console_write("\nScheduler: no runnable actors left.\n");
            }
            /* Nothing for THIS core: let go of the kernel lock so the
             * other core (and this core's own interrupts) can make
             * progress, sleep until the next tick or wake-up, then look
             * again. sti;hlt is atomic with respect to the interrupt
             * window, so a tick can't slip between them and be missed. */
            core_idle_ticks[cpu]++;
            hal_kernel_leave(1);
            __asm__ __volatile__("sti; hlt; cli" : : : "memory");
            hal_kernel_enter();
            continue;
        }

        if (cpu != 0 && core_switches[cpu] == 0) {
            /* The first time a non-boot core ever runs an actor: say so
             * (also what CI greps to prove the second core really is
             * scheduling on real hardware, not just booted). */
            hal_console_begin_window(CONSOLE_WIN_LOG);
            hal_console_write("[AP core ");
            hal_console_write_dec64((uint64_t)cpu);
            hal_console_write("] running its first actor: slot ");
            hal_console_write_dec64((uint64_t)next);
            hal_console_write("\n");
            hal_console_end_window();
        }

        last_pick[cpu] = next;
        cur_slot[cpu] = next;
        actors[next].state = ACTOR_RUNNING;
        hal_set_kernel_stack(next); /* this core's TSS.RSP0 -- see this file's top comment */
        core_switches[cpu]++;
        hal_context_switch(&sched_rsp[cpu], actors[next].rsp, actors[next].cr3);

        /* Back here once an actor running on THIS core switched to the
         * scheduler (yield / block / exit / preempt). */
        cur_slot[cpu] = -1;
    }
}

/* An actor giving up its core: saves its own context into *save_into
 * (actors[i].rsp, set by every caller) and resumes this core's
 * scheduler loop. Returns only when some core's loop later switches
 * back into this actor -- possibly a DIFFERENT core than the one it
 * left, which is fine: everything the actor needs (kernel stack, page
 * tables) is per-actor, and the kernel lock is held by whichever core
 * resumed it. Interrupt state is deliberately not touched on resume;
 * iretq restores the actor's own on its way back to ring 3. */
static void schedule_next(uint64_t *save_into) {
    hal_disable_interrupts();
    hal_context_switch(save_into, sched_rsp[hal_cpu_id()], hal_kernel_cr3());
}

/* Blocks the calling actor for `ticks` timer ticks (10ms each). What an
 * input-polling actor should do between polls instead of yielding: a
 * yielding poll loop is a core-and-kernel-lock-hogging busy loop, which
 * under two cores starved real work (a boot-time demo that took 15-100+
 * seconds depending on luck) and stopped either core from ever idling. */
void actor_sleep(uint64_t ticks) {
    if (current_actor < 0) {
        return;
    }
    hal_disable_interrupts();
    actors[current_actor].wake_tick = hal_ticks() + (ticks ? ticks : 1);
    actors[current_actor].state = ACTOR_SLEEPING;
    schedule_next(&actors[current_actor].rsp);
}

void actor_yield(void) {
    if (current_actor < 0) {
        return;
    }
    /* Disabled here too, not just inside schedule_next(): a timer
     * tick landing between this state write and schedule_next()'s own
     * call to actor_yield() -- via the same current_actor -- would
     * otherwise silently flip this actor back to READY the instant
     * schedule_next() sets state itself. Harmless race for yield
     * (READY either way), but actor_exit() below has the same
     * ordering and there DEAD must never lose to READY. */
    hal_disable_interrupts();
    actors[current_actor].state = ACTOR_READY;
    schedule_next(&actors[current_actor].rsp);
}

void actor_exit(void) {
    /* See actor_yield()'s comment: without this, a timer tick between
     * these two lines would call actor_yield() on our behalf (via
     * exception_handler -> actor_yield(), still referring to this
     * same current_actor), which unconditionally sets state back to
     * READY -- resurrecting an actor that just exited. */
    hal_disable_interrupts();
    actors[current_actor].state = ACTOR_DEAD;
    schedule_next(&actors[current_actor].rsp); /* rsp written here is never read again */
}

int actor_send(int dest, uint64_t type, uint64_t data) {
    if (dest < 0 || dest >= MAX_ACTORS) {
        return -1;
    }

    hal_disable_interrupts();

    if (!actor_has_cap(current_actor, CAP_SEND, dest)) {
        hal_enable_interrupts();
        return -1; /* no capability -- see this file's top comment */
    }

    if (actors[dest].state == ACTOR_DEAD) {
        hal_enable_interrupts();
        return -1;
    }

    if (actors[dest].mailbox_count >= MAILBOX_CAPACITY) {
        hal_enable_interrupts();
        return -1; /* backpressure -- reject rather than block the sender or drop silently */
    }

    int tail = (actors[dest].mailbox_head + actors[dest].mailbox_count) % MAILBOX_CAPACITY;
    actors[dest].mailbox[tail].type   = type;
    actors[dest].mailbox[tail].sender = (uint64_t)current_actor;
    actors[dest].mailbox[tail].data   = data;
    actors[dest].mailbox_count++;

    if (actors[dest].state == ACTOR_BLOCKED) {
        actors[dest].state = ACTOR_READY; /* wake it -- see this file's top comment */
    }

    hal_enable_interrupts();
    return 0;
}

void actor_receive(struct message *out) {
    hal_disable_interrupts();

    while (actors[current_actor].mailbox_count == 0) {
        actors[current_actor].state = ACTOR_BLOCKED;
        schedule_next(&actors[current_actor].rsp);
        /* schedule_next() re-enabled interrupts once this actor actually
         * resumed (see its own comment) -- re-disable before touching
         * shared state again to recheck the loop condition below. */
        hal_disable_interrupts();
    }

    int head = actors[current_actor].mailbox_head;
    *out = actors[current_actor].mailbox[head];
    actors[current_actor].mailbox_head = (head + 1) % MAILBOX_CAPACITY;
    actors[current_actor].mailbox_count--;

    hal_enable_interrupts();
}

void scheduler_start(void) {
    /* The boot context becomes the BSP's scheduler loop and never
     * returns. It takes the kernel lock first (the loop's invariant),
     * then lets the AP in -- everything it will need (actors, address
     * spaces, capabilities) already exists. */
    hal_disable_interrupts();
    hal_kernel_enter();
    sched_go = 1;
    scheduler_loop();
}

/* The AP's counterpart: waits for the BSP to finish setting the system
 * up, starts this core's own preemption tick, and becomes its
 * scheduler loop. Called from hal/x86_64/smp.c's ap_entry_c() on the
 * AP's own boot stack; never returns. */
void scheduler_start_ap(void) {
    while (!sched_go) {
        __asm__ __volatile__("pause" : : : "memory");
    }
    hal_disable_interrupts();
    hal_kernel_enter();
    hal_lapic_timer_start(LAPIC_TIMER_VECTOR, 0x100000);
    hal_console_begin_window(CONSOLE_WIN_LOG);
    hal_console_write("[AP core ");
    hal_console_write_dec64((uint64_t)hal_cpu_id());
    hal_console_write("] joined the scheduler\n");
    hal_console_end_window();
    scheduler_loop();
}

/* Called at every kernel entry: an actor flagged by actor_terminate()
 * while it was running on another core dies here, at the first point
 * it is back in the kernel and safe to tear down. */
void actor_check_pending_kill(void) {
    int c = current_actor;
    if (c >= 0 && actors[c].kill_pending) {
        actors[c].kill_pending = 0;
        hal_console_begin_window(CONSOLE_WIN_LOG);
        hal_console_write("\n[actor ");
        hal_console_write_dec64((uint64_t)c);
        hal_console_write(" killed at its next kernel entry -- it was running on another core]\n");
        hal_console_end_window();
        actor_exit(); /* never returns */
    }
}

/* Per-core view for the Cores app (SYS_CORE_INFO): which actor slot the
 * given core is executing right now (-1 = idle) and how many actors it
 * has switched into / times it found nothing to run. */
void actor_core_status(int cpu, int *running_slot, uint64_t *switches, uint64_t *idle_ticks) {
    if (cpu < 0 || cpu >= MAX_CPUS) {
        *running_slot = -1;
        *switches = 0;
        *idle_ticks = 0;
        return;
    }
    *running_slot = cur_slot[cpu];
    *switches = core_switches[cpu];
    *idle_ticks = core_idle_ticks[cpu];
}
