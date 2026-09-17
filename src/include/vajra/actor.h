#ifndef VAJRA_ACTOR_H
#define VAJRA_ACTOR_H

#include <stdint.h>

#define MAX_ACTORS 16

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
 *                                core/net.c. */
#define CAP_SEND           1
#define CAP_SPAWN          2
#define CAP_TERMINATE      3
#define CAP_READ_OBJECT    4
#define CAP_WRITE_OBJECT   5
#define CAP_PROMOTE_OBJECT 6
#define CAP_NET            7

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

#endif
