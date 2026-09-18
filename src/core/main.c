#include "vajra/hal.h"
#include "vajra/memory.h"
#include "vajra/actor.h"
#include "vajra/storage.h"
#include "vajra/net.h"

/* ------------------------------------------------------------------
 * Scheduler demonstration: twelve statically-spawned actors, plus
 * (dynamically, at runtime) up to two ghost-actor workers from
 * Coordinator and two sandboxed inspectors from Scanner. Four
 * (one/two/three/greedy) each print a few
 * messages with a yield in between (except actor_greedy, which never
 * yields at all -- see its own comment), proving the scheduler
 * actually interleaves execution rather than just running one actor to
 * completion. Three more (mailbox_receiver/mailbox_sender/intruder)
 * demonstrate message passing and capability enforcement. One
 * (coordinator, plus the worker(s) it spawns) demonstrates the full
 * ghost-actor lifecycle. Three more (downloader/scanner/reader, plus
 * the sandboxed actor_inspector() Scanner spawns per object)
 * demonstrate the storage pipeline: untrusted data written, inspected
 * by a narrowly-capable sandboxed actor, and either promoted through
 * trust levels for an ordinary reader to rely on, or permanently
 * rejected -- with that rejection enforced by the kernel itself, not
 * just convention, even against a reader that legitimately holds the
 * read capability. The last one (network_peer, Milestone 14) is the
 * actual thesis: a capability-gated actor sending and receiving a
 * message across a genuine device boundary -- see its own comment.
 * Kept as a working demonstration (same reasoning as V0.30's sysinfo
 * command staying in the shipped assembly kernel) rather than stripped
 * after verification, since it doesn't crash or destabilize anything.
 *
 * Everything below marked __attribute__((section(".user_text"))) runs
 * at CPL 3 (ring 3) -- see core/actor.c's actor_trampoline() and
 * hal/x86_64/paging.c's own comment on why only .text needs this
 * treatment, not the string literals these functions reference. That
 * means none of it can call hal_console_write(), actor_yield(), or
 * actor_exit() directly anymore: those are ordinary kernel functions,
 * living in supervisor-only memory, unreachable from ring 3 by
 * construction. user_write()/user_yield()/user_exit() below are the
 * only way back into the kernel from here -- thin wrappers around
 * hal_syscall(), which is itself the one part of the syscall boundary
 * that has to sit on the ring-3 side of the line (see
 * hal/x86_64/syscall_invoke.c).
 * ---------------------------------------------------------------- */

__attribute__((section(".user_text")))
static void user_write(const char *str) {
    hal_syscall(SYS_WRITE, (uint64_t)str, 0, 0);
}

/* No SYS_WRITE_DEC syscall exists, deliberately -- formatting a
 * number is pure computation, not a privileged operation, so it
 * belongs on the user side: this builds the string entirely in
 * ring 3, on the calling actor's own stack, and only crosses into the
 * kernel once, via user_write(), to actually print it. */
__attribute__((section(".user_text")))
static void user_write_dec64(uint64_t value) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    user_write(&buf[i]);
}

__attribute__((section(".user_text")))
static void user_yield(void) {
    hal_syscall(SYS_YIELD, 0, 0, 0);
}

__attribute__((section(".user_text")))
static void user_exit(void) {
    hal_syscall(SYS_EXIT, 0, 0, 0);
}

__attribute__((section(".user_text")))
static int user_send(int dest, uint64_t type, uint64_t data) {
    return (int)hal_syscall(SYS_SEND, (uint64_t)dest, type, data);
}

__attribute__((section(".user_text")))
static void user_receive(struct message *out) {
    hal_syscall(SYS_RECEIVE, (uint64_t)out, 0, 0);
}

__attribute__((section(".user_text")))
static int user_grant(int dest, int op, int target) {
    return (int)hal_syscall(SYS_GRANT, (uint64_t)dest, (uint64_t)op, (uint64_t)target);
}

__attribute__((section(".user_text")))
static int user_spawn(void (*entry)(void)) {
    return (int)hal_syscall(SYS_SPAWN, (uint64_t)entry, 0, 0);
}

__attribute__((section(".user_text")))
static int user_terminate(int target) {
    return (int)hal_syscall(SYS_TERMINATE, (uint64_t)target, 0, 0);
}

__attribute__((section(".user_text")))
static int user_object_read(int id, void *buf, uint32_t len) {
    return (int)hal_syscall(SYS_OBJECT_READ, (uint64_t)id, (uint64_t)buf, (uint64_t)len);
}

__attribute__((section(".user_text")))
static int user_object_write(int id, const void *buf, uint32_t len) {
    return (int)hal_syscall(SYS_OBJECT_WRITE, (uint64_t)id, (uint64_t)buf, (uint64_t)len);
}

__attribute__((section(".user_text")))
static int user_object_promote(int id) {
    return (int)hal_syscall(SYS_OBJECT_PROMOTE, (uint64_t)id, 0, 0);
}

__attribute__((section(".user_text")))
static int user_object_reject(int id) {
    return (int)hal_syscall(SYS_OBJECT_REJECT, (uint64_t)id, 0, 0);
}

__attribute__((section(".user_text")))
static int user_net_send(uint64_t type, uint64_t data) {
    return (int)hal_syscall(SYS_NET_SEND, type, data, 0);
}

__attribute__((section(".user_text")))
static int user_net_receive(struct net_message *out, uint32_t max_spins) {
    return (int)hal_syscall(SYS_NET_RECEIVE, (uint64_t)out, (uint64_t)max_spins, 0);
}

/* Formats a trust level as text entirely in ring 3 (same reasoning as
 * user_write_dec64() -- pure computation, no reason to spend a
 * syscall on it). Prints nothing further for an unrecognized value
 * rather than guessing -- there shouldn't ever be one, but silently
 * mislabeling a trust level would be worse than saying nothing.
 *
 * Deliberately if/else, not switch: a switch over these four values
 * is exactly the kind of thing a compiler turns into a jump table --
 * a small array of code addresses the CPU itself reads directly to
 * pick a branch target. That table would land in ordinary .rodata,
 * not .user_text, and unlike a string pointer this function hands to
 * a syscall (dereferenced by the KERNEL, at CPL 0, where the U/S bit
 * is irrelevant -- see paging.c's own comment), a jump table is read
 * by ring-3 code directly, with no kernel mediation in between. Confirmed
 * by exactly this: a ring-3 #PF, error code 5, on an address a few
 * bytes past __user_text_end. An if/else chain compiles to ordinary
 * CMP/JE instructions -- data-free, and entirely within .user_text
 * like the rest of this function's own code. */
__attribute__((section(".user_text")))
static void user_write_trust(int trust) {
    if (trust == OBJ_UNTRUSTED) {
        user_write("UNTRUSTED");
    } else if (trust == OBJ_QUARANTINED) {
        user_write("QUARANTINED");
    } else if (trust == OBJ_ANALYZED) {
        user_write("ANALYZED");
    } else if (trust == OBJ_TRUSTED) {
        user_write("TRUSTED");
    } else if (trust == OBJ_REJECTED) {
        user_write("REJECTED");
    }
}

__attribute__((section(".user_text")))
static void actor_one(void) {
    for (int i = 0; i < 3; i++) {
        user_write("[Actor 1] tick ");
        user_write_dec64((uint64_t)i);
        user_write("\n");
        user_yield();
    }
    user_exit();
}

__attribute__((section(".user_text")))
static void actor_two(void) {
    for (int i = 0; i < 3; i++) {
        user_write("[Actor 2] tick ");
        user_write_dec64((uint64_t)i);
        user_write("\n");
        user_yield();
    }
    user_exit();
}

__attribute__((section(".user_text")))
static void actor_three(void) {
    for (int i = 0; i < 3; i++) {
        user_write("[Actor 3] tick ");
        user_write_dec64((uint64_t)i);
        user_write("\n");
        user_yield();
    }
    user_exit();
}

/* Proves preemption is real rather than just compiled: this actor
 * never calls user_yield() at all, only user_exit() at the very end,
 * after several long busy-loops. Under a purely cooperative scheduler
 * this would have starved actors 1-3 forever -- once given the CPU,
 * nothing would ever take it back. With the timer driving preemption,
 * they still get to run (and finish all their ticks) while this actor
 * is busy-looping, proving the timer is actually interrupting code
 * that never asks to be interrupted -- and, since Milestone 6, doing
 * so from ring 3 exactly the same as it would from ring 0: the timer
 * doesn't care what privilege level it interrupts. */
__attribute__((section(".user_text")))
static void actor_greedy(void) {
    user_write("[Greedy] starting (never calls user_yield)\n");

    volatile uint64_t counter = 0;
    for (int spin = 0; spin < 5; spin++) {
        for (uint64_t i = 0; i < 2000000; i++) {
            counter++;
        }
        user_write("[Greedy] spin ");
        user_write_dec64((uint64_t)spin);
        user_write(" done (still never yielded)\n");
    }

    user_write("[Greedy] exiting\n");
    user_exit();
}

/* actor_spawn() below fills slots in call order, so these actors
 * always land at these slots -- hardcoded here rather than
 * discovered, since there's no actor directory/naming yet (a later
 * milestone). Dynamically spawned actors (the ghost-actor workers
 * further down) are deliberately NOT given fixed slots this way --
 * actor_spawn_child()'s return value is used instead, which is the
 * correct approach once spawning happens at runtime rather than in a
 * fixed boot-time sequence. */
#define MAILBOX_RECEIVER_SLOT 4
#define MAILBOX_SENDER_SLOT   5
#define INTRUDER_SLOT         6
#define COORDINATOR_SLOT      7
#define DOWNLOADER_SLOT       8
#define SCANNER_SLOT          9
#define READER_SLOT           10
#define NETWORK_PEER_SLOT     11

/* Message types the ghost-actor demo (actor_worker/actor_coordinator)
 * uses over actor_send()/actor_receive(). Arbitrary application-level
 * values -- the kernel doesn't interpret message type at all. */
#define TASK_SQUARE 1
#define TASK_RESULT 2

/* Message types the storage pipeline demo
 * (downloader/scanner/reader/inspector) uses to sequence itself -- see
 * actor_downloader()'s own comment for why sequencing is needed at
 * all. Also arbitrary application-level values; m.data carries the
 * relevant object id on all four. */
#define MSG_DOWNLOAD_DONE 1
#define MSG_SCAN_DONE     2
#define TASK_INSPECT      3 /* Scanner -> Inspector: please examine this object id */
#define MSG_CHECK_PASS    4 /* Inspector -> Scanner: looked clean */
#define MSG_CHECK_FAIL    5 /* Inspector -> Scanner: found the bad marker */

/* storage_create_object() calls in kernel_main() run in a fixed
 * order, so these object ids are as predictable as the actor slots
 * above -- same reasoning, same caveat (no object directory yet). */
#define PAYLOAD_OBJECT_ID    0 /* benign -- Downloader writes it, Scanner promotes it to
                                   OBJ_TRUSTED, Reader reads the final trusted content */
#define SUSPICIOUS_OBJECT_ID 1 /* contains a "BAD" marker -- Scanner rejects it instead,
                                   and Reader's attempt to read it is then refused even
                                   though Reader legitimately holds CAP_READ_OBJECT for it --
                                   see storage_read()'s own comment on why that's a second,
                                   independent gate, not a capability failure */

/* Demonstrates message passing, and specifically the harder of the
 * two possible orderings to get right: this actor calls
 * user_receive() with an EMPTY mailbox as its very first action, so
 * it blocks immediately (ACTOR_BLOCKED) before actor_mailbox_sender
 * has sent anything at all. If actor_send()'s wake-on-full-mailbox
 * logic (core/actor.c) were broken, this actor would simply never run
 * again -- there would be nothing left to make schedule_next() ever
 * pick it. Getting a message back at all is the proof the wake path
 * works, not just that messages can be enqueued.
 *
 * Receives twice, not once: the first message comes from Sender
 * (using the capability kernel_main granted it directly), the second
 * from Intruder (using a capability Sender delegates to it at
 * runtime) -- see actor_mailbox_sender() and actor_intruder(). */
__attribute__((section(".user_text")))
static void actor_mailbox_receiver(void) {
    for (int i = 0; i < 2; i++) {
        user_write("[Receiver] waiting for a message...\n");

        struct message msg;
        user_receive(&msg);

        user_write("[Receiver] got a message: type=");
        user_write_dec64(msg.type);
        user_write(" sender=");
        user_write_dec64(msg.sender);
        user_write(" data=");
        user_write_dec64(msg.data);
        user_write("\n");
    }

    user_exit();
}

/* kernel_main() grants this actor -- ALONE -- a CAP_SEND capability
 * for Receiver before the scheduler starts (principle of least
 * privilege: nothing gets authority it wasn't explicitly given).
 * After using it once, this actor delegates a COPY of that same
 * capability to Intruder, which starts holding nothing at all --
 * demonstrating that authority can be passed along deliberately, not
 * just handed out once at spawn time. */
__attribute__((section(".user_text")))
static void actor_mailbox_sender(void) {
    user_write("[Sender] ticking a few times before sending...\n");
    for (int i = 0; i < 3; i++) {
        user_yield();
    }

    user_write("[Sender] sending to Receiver (using its granted capability)\n");
    if (user_send(MAILBOX_RECEIVER_SLOT, 42, 1234) != 0) {
        user_write("[Sender] send failed!\n");
    }

    user_write("[Sender] delegating its send capability to Intruder\n");
    if (user_grant(INTRUDER_SLOT, CAP_SEND, MAILBOX_RECEIVER_SLOT) != 0) {
        user_write("[Sender] delegation failed!\n");
    }

    user_write("[Sender] exiting\n");
    user_exit();
}

/* Forward-declared: actor_intruder() below references it (to prove a
 * spawn attempt is denied) before its own definition further down,
 * next to actor_coordinator() which actually uses it legitimately. */
static void actor_worker(void);

/* Proves capability checking is real, not just present in the code:
 * this actor starts holding NO capabilities at all (kernel_main never
 * grants it any), so its first attempt to message Receiver -- knowing
 * its slot index perfectly well -- must be denied. Only after Sender
 * voluntarily delegates its own capability does the second, identical
 * attempt succeed. If actor_send()'s capability check (core/actor.c)
 * were missing or broken, attempt 1 would succeed instead of being
 * denied -- that's the specific failure this actor exists to catch. */
__attribute__((section(".user_text")))
static void actor_intruder(void) {
    user_write("[Intruder] attempt 1: sending to Receiver with no capability...\n");
    int rc = user_send(MAILBOX_RECEIVER_SLOT, 7, 111);
    user_write(rc == 0
        ? "[Intruder] SECURITY FAILURE: send succeeded with no capability!\n"
        : "[Intruder] denied, as expected (no capability held)\n");

    for (int i = 0; i < 5; i++) {
        user_yield();
    }

    user_write("[Intruder] attempt 2: sending to Receiver after Sender delegated...\n");
    rc = user_send(MAILBOX_RECEIVER_SLOT, 7, 222);
    user_write(rc == 0
        ? "[Intruder] send succeeded (capability correctly delegated)\n"
        : "[Intruder] still denied (delegation did not work)\n");

    /* Same proof, for the two Milestone 8 additions: Intruder holds no
     * CAP_SPAWN and no CAP_TERMINATE either, so both must be denied
     * regardless of how well it knows actor_worker's address or
     * Receiver's slot number. */
    user_write("[Intruder] attempt: spawning an actor with no CAP_SPAWN...\n");
    int spawned = user_spawn(actor_worker);
    user_write(spawned >= 0
        ? "[Intruder] SECURITY FAILURE: spawn succeeded with no CAP_SPAWN!\n"
        : "[Intruder] spawn denied, as expected (no CAP_SPAWN held)\n");

    user_write("[Intruder] attempt: terminating Receiver with no CAP_TERMINATE...\n");
    rc = user_terminate(MAILBOX_RECEIVER_SLOT);
    user_write(rc == 0
        ? "[Intruder] SECURITY FAILURE: terminate succeeded with no capability!\n"
        : "[Intruder] terminate denied, as expected (no CAP_TERMINATE held)\n");

    /* Same proof again, for Milestone 10's storage objects: Intruder
     * holds no CAP_READ_OBJECT/CAP_WRITE_OBJECT/CAP_PROMOTE_OBJECT for
     * the payload object either, so all three must be denied
     * regardless of it knowing the object's id perfectly well. */
    char buf[8];
    user_write("[Intruder] attempt: reading the payload object with no capability...\n");
    int orc = user_object_read(PAYLOAD_OBJECT_ID, buf, sizeof(buf));
    user_write(orc >= 0
        ? "[Intruder] SECURITY FAILURE: object read succeeded with no capability!\n"
        : "[Intruder] object read denied, as expected\n");

    user_write("[Intruder] attempt: overwriting the payload object with no capability...\n");
    orc = user_object_write(PAYLOAD_OBJECT_ID, "hacked", 6);
    user_write(orc >= 0
        ? "[Intruder] SECURITY FAILURE: object write succeeded with no capability!\n"
        : "[Intruder] object write denied, as expected\n");

    user_write("[Intruder] attempt: promoting the payload object with no capability...\n");
    orc = user_object_promote(PAYLOAD_OBJECT_ID);
    user_write(orc >= 0
        ? "[Intruder] SECURITY FAILURE: object promote succeeded with no capability!\n"
        : "[Intruder] object promote denied, as expected\n");

    user_exit();
}

/* A ghost actor in the sense philosophy §6 describes: created for one
 * narrowly scoped task, given only the authority that spawning it
 * automatically confers (CAP_SEND back to its spawner -- nothing
 * else), and gone once the task is done. Waits for exactly one task
 * message, computes a trivial result, replies, and exits itself --
 * the "self-cleaning" half of the lifecycle. actor_coordinator()
 * below demonstrates the other half, forcible termination, on a
 * second worker instead of waiting for it to finish. */
__attribute__((section(".user_text")))
static void actor_worker(void) {
    user_write("[Worker] waiting for a task...\n");

    struct message task;
    user_receive(&task);

    uint64_t result = task.data * task.data;
    user_write("[Worker] squaring ");
    user_write_dec64(task.data);
    user_write(" -> ");
    user_write_dec64(result);
    user_write(", replying to spawner\n");

    /* task.sender is the kernel-filled slot of whoever sent this task
     * -- reliable because it's how CAP_SEND back to the spawner was
     * addressed at spawn time (core/actor.c's actor_spawn_child()),
     * so it's always the actor this worker is authorized to reply to. */
    user_send((int)task.sender, TASK_RESULT, result);

    user_write("[Worker] done, exiting\n");
    user_exit();
}

/* kernel_main() grants this actor -- ALONE -- CAP_SPAWN. Demonstrates
 * the full ghost-actor lifecycle from philosophy §6: create a worker,
 * give it a task, get the result back, then either let it clean
 * itself up (Worker 1) or forcibly terminate it (Worker 2) -- plus
 * the quota guard (MAX_SPAWNS_PER_ACTOR in core/actor.c) refusing a
 * 3rd spawn outright, verified here rather than merely assumed. */
__attribute__((section(".user_text")))
static void actor_coordinator(void) {
    user_write("[Coordinator] spawning Worker 1...\n");
    int w1 = user_spawn(actor_worker);
    if (w1 < 0) {
        user_write("[Coordinator] spawn failed!\n");
        user_exit();
    }

    user_write("[Coordinator] sending Worker 1 a task\n");
    user_send(w1, TASK_SQUARE, 7);

    struct message result;
    user_receive(&result);
    user_write("[Coordinator] Worker 1 replied: ");
    user_write_dec64(result.data);
    user_write(" (7 squared; it will now exit on its own)\n");

    user_write("[Coordinator] spawning Worker 2...\n");
    int w2 = user_spawn(actor_worker);
    if (w2 < 0) {
        user_write("[Coordinator] spawn failed!\n");
        user_exit();
    }

    user_write("[Coordinator] sending Worker 2 a task, then terminating it directly\n");
    user_send(w2, TASK_SQUARE, 9);
    user_yield();
    user_yield();
    user_write(user_terminate(w2) == 0
        ? "[Coordinator] Worker 2 terminated\n"
        : "[Coordinator] terminate failed!\n");

    user_write("[Coordinator] attempting a 3rd spawn (quota is 2 per actor)...\n");
    int w3 = user_spawn(actor_worker);
    user_write(w3 >= 0
        ? "[Coordinator] SECURITY FAILURE: 3rd spawn succeeded!\n"
        : "[Coordinator] denied, as expected (spawn quota exceeded)\n");

    user_write("[Coordinator] exiting\n");
    user_exit();
}

/* Demonstrates the storage pipeline philosophy §7/§8 describes:
 * downloaded data lands untrusted, gets inspected in isolation, and
 * only then becomes something a normal reader can rely on -- or gets
 * turned away permanently. This actor plays the "download" role --
 * kernel_main() grants it CAP_WRITE_OBJECT for BOTH objects and
 * CAP_SEND to Scanner, nothing else. Writing always resets an
 * object's trust to OBJ_UNTRUSTED (core/storage.c), so there's no
 * separate step needed to mark this as untrusted; it's the
 * unavoidable consequence of writing at all.
 *
 * Signals Scanner by message rather than Scanner just polling the
 * object, for the same reason actor_receive() blocks instead of
 * spinning: there is no other way to know "the write actually landed
 * on disk" than being told, and a message is the mechanism this
 * kernel already has for exactly that.
 *
 * Waits for Scanner's ack after the FIRST signal before sending the
 * second -- not a courtesy, a correctness requirement. Scanner's
 * mailbox also receives verdict replies from the sandboxed inspectors
 * it spawns (see actor_scan_object()); if this actor's second
 * MSG_DOWNLOAD_DONE arrived while Scanner was still blocked waiting
 * for the FIRST object's inspector verdict, Scanner's receive() for
 * that verdict would have no way to tell the two apart and could
 * consume the wrong message -- exactly what happened the first time
 * this was written: the suspicious object was silently never scanned
 * at all, its DOWNLOAD_DONE signal misread as the benign object's
 * inspector verdict. Waiting for the ack guarantees Scanner has fully
 * finished one object -- including its inspector's reply -- before
 * this actor is even able to make a second message exist for it to
 * misinterpret. */
__attribute__((section(".user_text")))
static void actor_downloader(void) {
    user_write("[Downloader] writing payload object (marks it OBJ_UNTRUSTED)\n");
    user_object_write(PAYLOAD_OBJECT_ID, "hello from disk!", 17);
    user_send(SCANNER_SLOT, MSG_DOWNLOAD_DONE, (uint64_t)PAYLOAD_OBJECT_ID);

    struct message ack;
    user_receive(&ack); /* Scanner has fully finished the payload object now -- see above */

    user_write("[Downloader] writing suspicious object (marks it OBJ_UNTRUSTED)\n");
    user_object_write(SUSPICIOUS_OBJECT_ID, "BADSTUFF payload", 16);
    user_send(SCANNER_SLOT, MSG_DOWNLOAD_DONE, (uint64_t)SUSPICIOUS_OBJECT_ID);

    user_exit();
}

/* The actual "sandboxed execution" from philosophy §8 (and roadmap
 * Phase 9): a short-lived ghost actor Scanner spawns per object,
 * given ONLY a delegated CAP_READ_OBJECT for that one object -- not
 * inherited automatically, explicitly handed down by Scanner (see
 * actor_scan_object() below). If this actor were compromised by
 * whatever it's examining, it could not write, promote, reject,
 * touch any other object, or message anyone but Scanner (its only
 * auto-granted capability, from being spawned at all) -- a small
 * blast radius by construction (§33 invariant 8), not by convention.
 *
 * The "analysis" itself is deliberately trivial (one fixed marker) --
 * this milestone is about the authority structure sandboxing gives a
 * real scanner, not about writing one. */
__attribute__((section(".user_text")))
static void actor_inspector(void) {
    struct message task;
    user_receive(&task); /* {type=TASK_INSPECT, data=object id} */
    int id = (int)task.data;

    char buf[32];
    int n = user_object_read(id, buf, sizeof(buf) - 1);
    if (n < 0) {
        n = 0;
    }
    buf[n] = 0;

    user_write("[Inspector] examining object ");
    user_write_dec64((uint64_t)id);
    user_write(": \"");
    user_write(buf);
    user_write("\"\n");

    int bad = 0;
    for (int i = 0; buf[i] && buf[i + 1] && buf[i + 2]; i++) {
        if (buf[i] == 'B' && buf[i + 1] == 'A' && buf[i + 2] == 'D') {
            bad = 1;
            break;
        }
    }

    user_send((int)task.sender, bad ? MSG_CHECK_FAIL : MSG_CHECK_PASS, (uint64_t)id);
    user_exit();
}

/* Plays the "security actor" role from philosophy §8: the only actor
 * (besides Downloader/Reader's own narrow rights) authorized to touch
 * either object at all -- specifically the only one kernel_main()
 * grants CAP_PROMOTE_OBJECT (which also gates rejection -- see
 * actor.h's own comment) and, new this milestone, CAP_SPAWN, so it
 * can hand the actual inspection off to a sandboxed actor_inspector()
 * rather than examining untrusted content inline, itself. Spawning
 * costs one of Scanner's two spawn-quota slots per object -- exactly
 * fits MAX_SPAWNS_PER_ACTOR (core/actor.c) for the two objects this
 * demo scans, deliberately, not by accident. */
__attribute__((section(".user_text")))
static void actor_scan_object(int id) {
    user_write("[Scanner] spawning a sandboxed inspector for object ");
    user_write_dec64((uint64_t)id);
    user_write("\n");

    int w = user_spawn(actor_inspector);
    if (w < 0) {
        user_write("[Scanner] spawn failed!\n");
        return;
    }

    /* Delegated, not inherited: the inspector gets read access to
     * THIS object and nothing else, granted explicitly from a
     * capability Scanner itself holds (actor_delegate() -- see
     * core/actor.c -- refuses to pass along authority the delegator
     * doesn't have, so this only works because kernel_main() already
     * granted Scanner CAP_READ_OBJECT for both objects). */
    user_grant(w, CAP_READ_OBJECT, id);
    user_send(w, TASK_INSPECT, (uint64_t)id);

    struct message verdict;
    user_receive(&verdict);

    if (verdict.type == MSG_CHECK_FAIL) {
        user_write("[Scanner] object ");
        user_write_dec64((uint64_t)id);
        user_write(" FAILED inspection -- rejecting\n");
        user_object_reject(id);
    } else {
        user_write("[Scanner] object ");
        user_write_dec64((uint64_t)id);
        user_write(" passed inspection -- promoting\n");
        for (int i = 0; i < 3; i++) {
            int trust = user_object_promote(id);
            user_write("[Scanner] promoted to ");
            user_write_trust(trust);
            user_write("\n");
        }
    }

    user_send(READER_SLOT, MSG_SCAN_DONE, (uint64_t)id);

    /* Tells Downloader it's now safe to write and signal the next
     * object -- see actor_downloader()'s own comment for why this is
     * load-bearing, not just a courtesy. */
    user_send(DOWNLOADER_SLOT, MSG_SCAN_DONE, (uint64_t)id);
}

__attribute__((section(".user_text")))
static void actor_scanner(void) {
    for (int i = 0; i < 2; i++) {
        struct message m;
        user_receive(&m); /* blocks until Downloader signals MSG_DOWNLOAD_DONE for an object */
        actor_scan_object((int)m.data);
    }
    user_exit();
}

/* An ordinary consumer: holds CAP_READ_OBJECT for BOTH objects, but
 * neither CAP_WRITE_OBJECT nor CAP_PROMOTE_OBJECT for either -- it
 * can rely on what Scanner decided, not override it. Waits for
 * Scanner's signal before reading either object at all, so what it
 * reads is always Scanner's FINAL verdict, never a half-scanned
 * object. The suspicious object's read is expected to fail: holding
 * CAP_READ_OBJECT is not the same as the object being readable --
 * see storage_read()'s own comment on why OBJ_REJECTED is a second,
 * independent gate the capability alone does not clear. */
__attribute__((section(".user_text")))
static void actor_reader(void) {
    for (int i = 0; i < 2; i++) {
        struct message m;
        user_receive(&m); /* blocks until Scanner signals MSG_SCAN_DONE for an object */
        int id = (int)m.data;

        char buf[32];
        int n = user_object_read(id, buf, sizeof(buf) - 1);

        user_write("[Reader] object ");
        user_write_dec64((uint64_t)id);
        if (n < 0) {
            user_write(": read refused (rejected -- capability alone wasn't enough)\n");
        } else {
            buf[n] = 0;
            user_write(": trusted content \"");
            user_write(buf);
            user_write("\"\n");
        }
    }

    user_exit();
}

/* Roadmap Phase 12 (Milestone 14): the actual thesis, one step
 * further than Milestone 13's raw driver demo -- an ACTOR sending and
 * receiving a message across a genuine device boundary, capability-
 * gated (CAP_NET), not just kernel_main poking the HAL directly.
 * Symmetric by design: the exact same actor, unmodified, runs on
 * every Vajra instance -- each one broadcasts a HELLO, listens for
 * one from a peer, and replies once if it hears one. Two separate
 * booted instances joined by a real (if QEMU-emulated) network link
 * running this same code is the actual verification: neither instance
 * is "the sender" or "the receiver", proving the symmetry is real, not
 * arranged.
 *
 * Deliberately minimal: no addressing (broadcasts, and accepts
 * anything using the protocol -- see core/net.c), no remote actor
 * identity (a reply just means "some peer heard me", not "actor X on
 * device Y heard me"). Real transport semantics (addressing, remote
 * actor identity, guaranteed reliability) are explicitly Phase 12's
 * next step, not this milestone's -- what's here is a deliberately
 * simple, honest exception: the HELLO broadcast repeats periodically
 * while listening, not just once. A single-shot broadcast plus a
 * short listen window turned out to be genuinely too fragile for two
 * independently-scheduled instances to reliably find each other with
 * (confirmed by booting two real, separate QEMU instances: both sides
 * ran their entire demo correctly -- proven by their own boot traces,
 * real syscalls, zero faults -- but neither ever happened to be
 * listening at the exact moment the other's one-shot broadcast
 * arrived, purely because each instance's own preceding actors take a
 * slightly different amount of time to run before reaching this one).
 * Repeating the broadcast is an application-level persistence choice,
 * not a transport-level guarantee -- it doesn't add acknowledgments,
 * ordering, or addressing, just more chances to be heard.
 *
 * The listening window itself first shipped as 10, then 20, attempts
 * of a 2,000,000-spin poll (hal_net_poll_receive()'s busy-wait, see
 * hal/x86_64/virtio_net.c) -- both turned out to still be far too
 * short in *real* time on a genuinely separate second QEMU instance.
 * A spin count isn't a duration: it's however long that many bare
 * volatile-memory-compare loop iterations take under TCG emulation,
 * which is a few milliseconds at most -- multiplied by even 20
 * attempts, the whole window closes in well under a second. Two
 * independently-launched QEMU processes on shared CI hardware don't
 * start that precisely together (the CI workflow itself staggers them
 * by a second or more so the `listen=` side's socket is bound before
 * the `connect=` side tries), so neither instance's sub-second window
 * ever coincided with the other's, no matter how many times each
 * re-broadcast -- confirmed by two full, fault-free boot traces
 * (-d int,cpu_reset showing perfect execution end to end) that still
 * both independently gave up. Fixed by scaling the spin count up by
 * 25x and the attempt count up by 3x, so the window is comfortably
 * wider than realistic multi-second start-time skew between two
 * separate processes, not just wider than measurement noise. */
#define MSG_NET_HELLO     1
#define MSG_NET_HELLO_ACK 2

__attribute__((section(".user_text")))
static void actor_network_peer(void) {
    int rc = user_net_send(MSG_NET_HELLO, 0xC0FFEE);
    if (rc != 0) {
        user_write("[Net] no network capability or no device -- nothing to do\n");
        user_exit();
    }
    user_write("[Net] broadcast HELLO, listening for a peer...\n");

    int heard_ack = 0;
    for (int attempt = 0; attempt < 60 && !heard_ack; attempt++) {
        if (attempt > 0 && (attempt % 4) == 0) {
            /* Re-broadcast -- see this function's own top comment. */
            user_net_send(MSG_NET_HELLO, 0xC0FFEE);
        }

        struct net_message msg;
        int got = user_net_receive(&msg, 50000000);
        if (got != 1) {
            continue; /* nothing this attempt -- keep listening, bounded by the loop itself */
        }

        if (msg.type == MSG_NET_HELLO) {
            user_write("[Net] heard a HELLO from a peer (data=");
            user_write_dec64(msg.data);
            user_write(") -- replying\n");
            user_net_send(MSG_NET_HELLO_ACK, 0xBEEF);
        } else if (msg.type == MSG_NET_HELLO_ACK) {
            user_write("[Net] heard a HELLO_ACK from a peer (data=");
            user_write_dec64(msg.data);
            user_write(") -- genuine cross-device actor communication confirmed\n");
            heard_ack = 1;
        }
    }

    if (!heard_ack) {
        user_write("[Net] no peer heard from within the listening window (single-instance run?)\n");
    }
    user_exit();
}

/* Roadmap Phase 12 (Milestone 13): the raw HAL network driver's first
 * exercise, the same way hal_disk_read/write were first called
 * directly from kernel_main before core/storage.c ever existed. Prints
 * this device's MAC, sends one broadcast ARP request ("who has
 * 10.0.2.2?" -- QEMU's default usermode-networking gateway, which
 * always answers ARP for itself), and polls for the reply -- a
 * genuine round trip over real (emulated) hardware, not a loopback or
 * a simulation. Deliberately raw Ethernet framing built by hand: no
 * IP/UDP/TCP stack exists yet, only hal_net_send()/hal_net_poll_receive()
 * (hal/x86_64/virtio_net.c). */
static void write_hex_byte(uint8_t b) {
    const char *digits = "0123456789ABCDEF";
    hal_console_putchar(digits[(b >> 4) & 0xF]);
    hal_console_putchar(digits[b & 0xF]);
}

static void write_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        write_hex_byte(mac[i]);
        if (i != 5) {
            hal_console_putchar(':');
        }
    }
}

static int net_arp_demo(void) {
    if (net_init() != 0) {
        hal_console_write("Net: no virtio-net-pci device found (QEMU started without -device virtio-net-pci?).\n");
        return 0;
    }

    uint8_t mac[6];
    hal_net_get_mac(mac);
    hal_console_write("Net: virtio-net online, MAC ");
    write_mac(mac);
    hal_console_write("\n");

    uint8_t frame[42];
    for (int i = 0; i < 6; i++) {
        frame[i] = 0xFF; /* Ethernet broadcast destination */
    }
    for (int i = 0; i < 6; i++) {
        frame[6 + i] = mac[i]; /* Ethernet source */
    }
    frame[12] = 0x08;
    frame[13] = 0x06; /* ethertype: ARP */

    uint8_t *arp = frame + 14;
    arp[0] = 0x00; arp[1] = 0x01; /* hardware type: Ethernet */
    arp[2] = 0x08; arp[3] = 0x00; /* protocol type: IPv4 */
    arp[4] = 6;                   /* hardware address length */
    arp[5] = 4;                   /* protocol address length */
    arp[6] = 0x00; arp[7] = 0x01; /* opcode: request */
    for (int i = 0; i < 6; i++) {
        arp[8 + i] = mac[i]; /* sender MAC */
    }
    arp[14] = 10; arp[15] = 0; arp[16] = 2; arp[17] = 15;   /* sender IP: 10.0.2.15 (claimed) */
    for (int i = 0; i < 6; i++) {
        arp[18 + i] = 0x00; /* target MAC: unknown, that's the question */
    }
    arp[24] = 10; arp[25] = 0; arp[26] = 2; arp[27] = 2;    /* target IP: 10.0.2.2 (QEMU's gateway) */

    hal_console_write("Net: sending ARP request -- who has 10.0.2.2?\n");
    if (hal_net_send(frame, sizeof(frame)) != 0) {
        hal_console_write("Net: send failed.\n");
        return 1; /* device is online, this one send just failed -- still usable below */
    }

    uint8_t reply[64];
    int n = hal_net_poll_receive(reply, sizeof(reply), 20000000);
    if (n < 0) {
        hal_console_write("Net: a reply arrived but didn't fit the receive buffer.\n");
    } else if (n == 0) {
        hal_console_write("Net: no ARP reply received (timed out).\n");
    } else if (n >= 42 && reply[12] == 0x08 && reply[13] == 0x06 && reply[20] == 0x00 && reply[21] == 0x02) {
        hal_console_write("Net: ARP reply -- 10.0.2.2 is at ");
        write_mac(&reply[22]);
        hal_console_write(" -- a genuine round trip over real virtio hardware.\n");
    } else {
        hal_console_write("Net: received a frame, but not the expected ARP reply (");
        hal_console_write_dec64((uint64_t)n);
        hal_console_write(" bytes).\n");
    }
    return 1;
}

/* This is the real entry point into the portable core -- everything
 * it calls is a HAL function, nothing here is x86-specific. On a
 * future AArch64 port, this exact file should compile and run
 * unchanged once src/hal/aarch64/ implements the same hal_*
 * functions. Runs entirely in ring 0 (unlike the actor functions
 * above): it's the kernel's own setup code, never actor code. */
void kernel_main(void) {
    hal_console_init();
    hal_console_write("VAJRA OS (C rewrite) - Milestone 14\n");
    hal_console_write("Console + IDT + exception handling online.\n");

    hal_interrupts_init();
    hal_console_write("IDT installed.\n");

    /* Roadmap Phase 10: wake a second physical core and let it run
     * genuinely in parallel with everything below -- see
     * hal/x86_64/smp.c's own top comment for exactly what this does
     * and doesn't yet mean. Deliberately placed here, before
     * scheduler_start(): the AP's own demo work (see smp.c) needs to
     * overlap the BSP's entire actor demo in wall-clock time for the
     * "genuinely concurrent, not just sequential" proof to mean
     * anything, and this is the earliest point the GDT/IDT it depends
     * on are both ready. */
    if (hal_smp_boot_ap()) {
        hal_console_write("SMP: AP core online.\n");
    } else {
        hal_console_write("SMP: no AP responded (single-CPU run?).\n");
    }

    memory_init();
    hal_console_write("Memory manager online. Detected RAM: ");
    hal_console_write_dec64(memory_get_total_bytes() / (1024 * 1024));
    hal_console_write(" MB\n");

    /* Roadmap Phase 12: the first piece of the fabric -- see
     * net_arp_demo()'s own comment. Needs memory_init() (virtqueues
     * are DMA memory, allocated via alloc_pages_contig()) but nothing
     * else below it. Also brings core/net.c online (net_init()) for
     * actor_network_peer below, whether or not a device turned out to
     * be present -- net_send_message()/net_poll_receive_message()
     * both fail cleanly if it isn't. */
    net_arp_demo();

    hal_pic_remap();
    hal_timer_init(100);
    hal_console_write("PIC remapped, timer at 100 Hz.\n");

    storage_init();
    scheduler_init();
    actor_spawn(actor_one);
    actor_spawn(actor_two);
    actor_spawn(actor_three);
    actor_spawn(actor_greedy);
    actor_spawn(actor_mailbox_receiver); /* must land at MAILBOX_RECEIVER_SLOT -- see its #define */
    actor_spawn(actor_mailbox_sender);   /* must land at MAILBOX_SENDER_SLOT */
    actor_spawn(actor_intruder);         /* must land at INTRUDER_SLOT -- starts with NO capabilities */
    actor_spawn(actor_coordinator);      /* must land at COORDINATOR_SLOT */
    actor_spawn(actor_downloader);       /* must land at DOWNLOADER_SLOT */
    actor_spawn(actor_scanner);          /* must land at SCANNER_SLOT */
    actor_spawn(actor_reader);           /* must land at READER_SLOT */
    actor_spawn(actor_network_peer);     /* must land at NETWORK_PEER_SLOT */

    int payload_id    = storage_create_object("payload.bin");    /* must be PAYLOAD_OBJECT_ID */
    int suspicious_id = storage_create_object("suspicious.bin"); /* must be SUSPICIOUS_OBJECT_ID */
    hal_console_write("Storage online. Created objects 'payload.bin' (id ");
    hal_console_write_dec64((uint64_t)payload_id);
    hal_console_write(") and 'suspicious.bin' (id ");
    hal_console_write_dec64((uint64_t)suspicious_id);
    hal_console_write(").\n");

    /* The only capabilities granted at setup time: Sender may send to
     * Receiver, Coordinator may spawn ghost actors, and the storage
     * pipeline actors get exactly the narrow rights their role needs
     * -- Downloader can write both objects but never read or promote
     * either; Scanner can read+promote+spawn (to sandbox the actual
     * inspection -- see actor_scan_object()) but never write; Reader
     * can only read. Everyone else -- Intruder very much included --
     * starts with nothing, per the principle of least privilege.
     * Whatever Coordinator's or Scanner's spawned children can do
     * beyond that comes from actor_spawn_child()'s own auto-grants
     * (core/actor.c) or Scanner's own explicit delegation, not from
     * here. */
    actor_grant(MAILBOX_SENDER_SLOT, CAP_SEND, MAILBOX_RECEIVER_SLOT);
    actor_grant(COORDINATOR_SLOT, CAP_SPAWN, 0);

    actor_grant(DOWNLOADER_SLOT, CAP_WRITE_OBJECT, PAYLOAD_OBJECT_ID);
    actor_grant(DOWNLOADER_SLOT, CAP_WRITE_OBJECT, SUSPICIOUS_OBJECT_ID);
    actor_grant(DOWNLOADER_SLOT, CAP_SEND, SCANNER_SLOT);

    actor_grant(SCANNER_SLOT, CAP_READ_OBJECT, PAYLOAD_OBJECT_ID);
    actor_grant(SCANNER_SLOT, CAP_READ_OBJECT, SUSPICIOUS_OBJECT_ID);
    actor_grant(SCANNER_SLOT, CAP_PROMOTE_OBJECT, 0);
    actor_grant(SCANNER_SLOT, CAP_SPAWN, 0);
    actor_grant(SCANNER_SLOT, CAP_SEND, READER_SLOT);
    actor_grant(SCANNER_SLOT, CAP_SEND, DOWNLOADER_SLOT); /* the ack actor_downloader() waits on */

    actor_grant(READER_SLOT, CAP_READ_OBJECT, PAYLOAD_OBJECT_ID);
    actor_grant(READER_SLOT, CAP_READ_OBJECT, SUSPICIOUS_OBJECT_ID);

    actor_grant(NETWORK_PEER_SLOT, CAP_NET, 0);

    hal_console_write("\nStarting preemptive scheduler with 12 ring-3 actors...\n\n");

    hal_enable_interrupts();
    scheduler_start();
}
