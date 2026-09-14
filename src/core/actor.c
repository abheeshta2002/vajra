#include "vajra/hal.h"
#include "vajra/memory.h"
#include "vajra/actor.h"

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
    ACTOR_BLOCKED /* waiting on actor_receive() with an empty mailbox */
} actor_state_t;

#define MAILBOX_CAPACITY 8
/* Scanner (core/main.c) is the actor that needs the most: 6 capabilities
 * granted at boot (2x CAP_READ_OBJECT, CAP_PROMOTE_OBJECT, CAP_SPAWN, 2x
 * CAP_SEND) plus CAP_SEND+CAP_TERMINATE auto-granted per sandboxed
 * inspector it spawns (actor_spawn_child() below) -- 2 inspectors for
 * this demo, so 6 + 2*2 = 10. There's no reclamation of a dead child's
 * now-useless caps (a real limitation -- see actor_spawn_child()'s own
 * comment), so this needs headroom above the exact minimum, not just
 * meet it. */
#define MAX_CAPS_PER_ACTOR 12
#define MAX_SPAWNS_PER_ACTOR 2 /* fork-bomb guard -- see actor_spawn_child() */

/* One unit of authority: the right to perform `op` on/toward `target`.
 * op == 0 marks an empty slot -- CAP_SEND is defined as 1 in actor.h
 * specifically so a zeroed struct (every actor starts this way) reads
 * as "holds nothing," not as an accidental capability. */
struct capability {
    int op;
    int target;
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
    int spawn_count; /* actors created via actor_spawn_child() so far -- see MAX_SPAWNS_PER_ACTOR */
};

#define ACTOR_STACK_SIZE 4096  /* one page; plenty for now, revisit when actors do more */

static struct actor actors[MAX_ACTORS];
static uint64_t scheduler_rsp;   /* the boot context's own saved stack, once we hand off to actors */
static int current_actor = -1;

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
        for (int j = 0; j < MAX_CAPS_PER_ACTOR; j++) {
            actors[i].caps[j].op = 0;
            actors[i].caps[j].target = 0;
        }
    }
    current_actor = -1;
}

static int actor_has_cap(int slot, int op, int target) {
    for (int i = 0; i < MAX_CAPS_PER_ACTOR; i++) {
        if (actors[slot].caps[i].op == op && actors[slot].caps[i].target == target) {
            return 1;
        }
    }
    return 0;
}

static int actor_add_cap(int slot, int op, int target) {
    for (int i = 0; i < MAX_CAPS_PER_ACTOR; i++) {
        if (actors[slot].caps[i].op == 0) {
            actors[slot].caps[i].op = op;
            actors[slot].caps[i].target = target;
            return 0;
        }
    }
    return -1; /* table full */
}

int actor_grant(int dest, int op, int target) {
    if (dest < 0 || dest >= MAX_ACTORS) {
        return -1;
    }
    return actor_add_cap(dest, op, target);
}

int actor_current_has_cap(int op, int target) {
    if (current_actor < 0) {
        return 0;
    }
    return actor_has_cap(current_actor, op, target);
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
        actors[i].spawn_count = 0;
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

int actor_spawn_child(void (*entry)(void)) {
    hal_disable_interrupts();

    if (!actor_has_cap(current_actor, CAP_SPAWN, 0)) {
        hal_enable_interrupts();
        return -1; /* not authorized to spawn at all -- see this file's top comment */
    }

    if (actors[current_actor].spawn_count >= MAX_SPAWNS_PER_ACTOR) {
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

    /* target cannot be ACTOR_RUNNING: on a single core, the only
     * RUNNING actor is ever current_actor, already excluded above.
     * Whatever target was doing (including suspended mid-syscall on
     * its own kernel stack) is simply abandoned -- schedule_next()
     * will never pick a DEAD actor again, and reap_dead_actors()
     * reclaims its user stack the same way any other exit does. */
    actors[target].state = ACTOR_DEAD;

    hal_enable_interrupts();
    return 0;
}

/* Frees the stack page of any actor that has exited, except
 * current_actor -- whichever actor is calling schedule_next() right
 * now might be the one that just called actor_exit(), and it is still
 * running on its own stack until the hal_context_switch() call below
 * actually moves off it. Any OTHER dead actor's stack is guaranteed
 * unused (nothing has run on it since it died) and safe to free here. */
static void reap_dead_actors(void) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        if (i == current_actor) {
            continue;
        }
        if (actors[i].state == ACTOR_DEAD && actors[i].stack_page) {
            free_page(actors[i].stack_page);
            actors[i].stack_page = 0;
        }
    }
}

/* Finds the next READY/RUNNING actor after 'start' (round robin) and
 * switches to it, saving the current context into *save_into first.
 * If nothing is left to run, halts.
 *
 * Disables interrupts for the whole decision + switch, so a timer
 * tick can never fire in the middle of mutating actors[]/
 * current_actor (whether this call came from a cooperative yield/exit
 * or from the timer ISR itself, where they're already disabled and
 * this is a harmless no-op). Re-enabled once this exact call
 * genuinely resumes -- which, for an already-running actor, is right
 * here after hal_context_switch() returns; a brand new actor instead
 * gets its interrupts re-enabled by actor_trampoline(), since its
 * fake frame's `ret` never actually returns into this function. */
static void schedule_next(uint64_t *save_into) {
    hal_disable_interrupts();

    reap_dead_actors();

    int start = (current_actor < 0) ? 0 : current_actor;
    int next = -1;

    for (int i = 1; i <= MAX_ACTORS; i++) {
        int idx = (start + i) % MAX_ACTORS;
        if (actors[idx].state == ACTOR_READY || actors[idx].state == ACTOR_RUNNING) {
            next = idx;
            break;
        }
    }

    if (next == -1) {
        hal_console_write("\nScheduler: no runnable actors left.\n");
        hal_halt_forever();
    }

    if (next == current_actor) {
        /* The only runnable actor is the one calling schedule_next()
         * right now -- e.g. it yielded but nothing else is READY.
         * There is nothing to switch to: actors[next].rsp was last
         * written when this actor was originally switched INTO (its
         * spawn-time fake frame, or an earlier suspend point) and was
         * never updated since, because nothing has looked at it again
         * until this very call. Calling hal_context_switch anyway
         * would save the current (correct, live) position into
         * *save_into and then immediately load rsp from that stale
         * value instead -- jumping back into memory this actor's own
         * stack has long since overwritten with real data, landing on
         * garbage instead of a valid return address. Simply returning
         * makes yield-with-nothing-else-ready a no-op, which is the
         * correct behavior anyway. */
        actors[next].state = ACTOR_RUNNING;
        hal_enable_interrupts();
        return;
    }

    current_actor = next;
    actors[next].state = ACTOR_RUNNING;
    hal_set_kernel_stack(next); /* TSS.RSP0 -- see this file's top comment */
    hal_context_switch(save_into, actors[next].rsp, actors[next].cr3);

    /* Reached only once something later switches back to the actor
     * that made this exact call -- see this function's own comment. */
    hal_enable_interrupts();
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
    schedule_next(&scheduler_rsp);
    /* Not expected to return: the boot context's own state, saved
     * into scheduler_rsp just above, is never switched back to in
     * this design -- once handed off, execution lives entirely among
     * the actors until schedule_next() finds none left and halts. */
}
