#include "vajra/hal.h"
#include "vajra/memory.h"
#include "vajra/actor.h"
#include "vajra/storage.h"
#include "vajra/net.h"

/* ------------------------------------------------------------------
 * Scheduler demonstration: fifteen statically-spawned actors (a 14th,
 * actor_namer, added for Phase 17's persistent name/directory layer;
 * a 15th, actor_shell, added for Phase 18's real interactive shell --
 * see each one's own comment), plus
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

/* Nibble-to-hex-char by arithmetic, not a lookup table -- a ring-3
 * function may take the ADDRESS of ordinary kernel .rodata (a string
 * literal to hand a syscall) but must never DEREFERENCE it itself
 * (hal/x86_64/paging.c's own comment): .rodata is supervisor-only,
 * same as .text, except .user_text. A `const char *digits = "0..F"`
 * table indexed here is exactly that forbidden dereference -- it
 * compiled fine and then paged-faulted (#PF, present+user-read) the
 * first time this function actually ran on real two-instance CI,
 * since a single local sanity boot never took this code path at all
 * (no peer to hear a HELLO from). user_write_dec64() above already
 * gets this right by never indexing a table either -- matched here. */
__attribute__((section(".user_text")))
static char hex_nibble(uint8_t n) {
    return (char)((n < 10) ? ('0' + n) : ('A' + (n - 10)));
}

__attribute__((section(".user_text")))
static void user_write_mac(const uint8_t mac[6]) {
    char buf[18];
    int i = 0;
    for (int b = 0; b < 6; b++) {
        buf[i++] = hex_nibble((mac[b] >> 4) & 0xF);
        buf[i++] = hex_nibble(mac[b] & 0xF);
        if (b != 5) {
            buf[i++] = ':';
        }
    }
    buf[i] = 0;
    user_write(buf);
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
static int user_spawn_program(int object_id) {
    return (int)hal_syscall(SYS_SPAWN_PROGRAM, (uint64_t)object_id, 0, 0);
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
static int user_net_send_to(const uint8_t mac[6], uint64_t type, uint64_t data) {
    uint64_t packed = 0;
    for (int i = 0; i < 6; i++) {
        packed |= ((uint64_t)mac[i]) << (8 * i);
    }
    return (int)hal_syscall(SYS_NET_SEND_TO, type, data, packed);
}

__attribute__((section(".user_text")))
static int user_net_send_reliable_to(const uint8_t mac[6], uint64_t type, uint64_t data) {
    uint64_t packed = 0;
    for (int i = 0; i < 6; i++) {
        packed |= ((uint64_t)mac[i]) << (8 * i);
    }
    return (int)hal_syscall(SYS_NET_SEND_RELIABLE, type, data, packed);
}

__attribute__((section(".user_text")))
static int user_net_receive(struct net_message *out, uint32_t max_spins) {
    return (int)hal_syscall(SYS_NET_RECEIVE, (uint64_t)out, (uint64_t)max_spins, 0);
}

__attribute__((section(".user_text")))
static int user_lookup_name(const char *name) {
    return (int)hal_syscall(SYS_LOOKUP_NAME, (uint64_t)name, 0, 0);
}

__attribute__((section(".user_text")))
static int user_list_objects(int index, struct object_info *out) {
    return (int)hal_syscall(SYS_LIST_OBJECTS, (uint64_t)index, (uint64_t)out, 0);
}

__attribute__((section(".user_text")))
static int user_create_name(const char *name) {
    return (int)hal_syscall(SYS_CREATE_NAME, (uint64_t)name, 0, 0);
}

__attribute__((section(".user_text")))
static int user_rename_object(int id, const char *new_name) {
    return (int)hal_syscall(SYS_RENAME_OBJECT, (uint64_t)id, (uint64_t)new_name, 0);
}

__attribute__((section(".user_text")))
static int user_delete_name(int id) {
    return (int)hal_syscall(SYS_DELETE_NAME, (uint64_t)id, 0, 0);
}

__attribute__((section(".user_text")))
static int user_key_read(void) {
    return (int)hal_syscall(SYS_KEY_READ, 0, 0, 0);
}

__attribute__((section(".user_text")))
static void user_rtc_read(struct rtc_time *out) {
    hal_syscall(SYS_RTC_READ, (uint64_t)out, 0, 0);
}

__attribute__((section(".user_text")))
static int user_mouse_read(struct mouse_state *out) {
    return (int)hal_syscall(SYS_MOUSE_READ, (uint64_t)out, 0, 0);
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
#define PROGRAM_LOADER_SLOT   12
#define NAMESPACE_DEMO_SLOT   13
#define SHELL_SLOT            14

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
#define MSG_NET_PING      3 /* the reliable-delivery exercise below -- never seen by this
                                function's own type dispatch, only by net.c's auto-ACK, which
                                doesn't care what type a DATA frame carries */

/* Roadmap Phase 13a: the first slice of the actual thesis (Phase 13,
 * FLAGSHIP) -- not yet true live migration (docs/ROADMAP.md's own
 * Phase 13 entry describes that as the eventual goal), but the
 * honest, buildable first step: one device can ask another to run a
 * program, and the REQUESTER gains no authority on the target device
 * at all. `data` = HELLO_PROGRAM_OBJECT_ID/CALC_PROGRAM_OBJECT_ID (a
 * storage object id both peers already have, at the same id, from
 * booting the identical seeded demo -- id-by-convention, not a real
 * name-based lookup; see this function's own reply-handling comment
 * below for why that's a known, labeled simplification, not the real
 * mechanism). No new wire format, no new syscall: this rides entirely
 * on the existing generic message transport (core/net.c) plus the
 * existing SYS_SPAWN_PROGRAM syscall, exactly as capability- and
 * trust-gated on the RECEIVING device as any local spawn already is
 * (Phases 16/20/27) -- core/net.c itself still has no idea actors,
 * capabilities, or spawning exist at all.
 *
 * Why this is invariant-4-safe (docs/PHILOSOPHY.md §3: authority can
 * only be preserved or narrowed crossing a device boundary, never
 * widened) without needing Phase 29's device authentication first: the
 * spawned actor gets ZERO capabilities, the same as any fresh
 * actor_spawn_program_child() locally -- nothing is delegated across
 * the wire, only a REQUEST. The only reason the spawn can succeed at
 * all is that the RECEIVING device already, independently, granted
 * THIS actor (NETWORK_PEER_SLOT) the local authority to do it
 * (kernel_main's own CAP_READ_OBJECT/CAP_SPAWN grants below) -- an
 * untrusted or malicious peer asking for this gains literally nothing
 * it didn't already have the power to refuse. */
#define HELLO_PROGRAM_OBJECT_ID 2 /* must match kernel_main's own storage_create_object() call
                                      order for "hello.bin" -- also used by actor_program_loader
                                      below, unchanged from its original Phase 16 meaning */
#define CALC_PROGRAM_OBJECT_ID  3
#define MSG_NET_SPAWN_REQUEST 4 /* data = a storage object id (see this block's own top comment
                                    on the id-by-convention limitation) */
#define MSG_NET_SPAWN_REPLY   5 /* data = the new actor's LOCAL slot on the replying device, or
                                    (uint64_t)-1 if refused (not authorized, pool exhausted, not
                                    OBJ_TRUSTED, ...) -- meaningless to the requester as an
                                    identity (it's a slot on a device the requester has no
                                    capability over), only as a yes/no confirmation */

__attribute__((section(".user_text")))
static void actor_network_peer(void) {
    int rc = user_net_send(MSG_NET_HELLO, 0xC0FFEE);
    if (rc != 0) {
        user_write("[Net] no network capability or no device -- nothing to do\n");
        user_exit();
    }
    user_write("[Net] broadcast HELLO, listening for a peer...\n");

    int heard_ack = 0;
    uint8_t peer_mac[6];
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
            user_write("[Net] heard a HELLO from actor ");
            user_write_dec64(msg.sender_actor);
            user_write(" on device ");
            user_write_mac(msg.sender_mac);
            user_write(" -- replying directly (addressed, not broadcast)\n");
            user_net_send_to(msg.sender_mac, MSG_NET_HELLO_ACK, 0xBEEF);
        } else if (msg.type == MSG_NET_HELLO_ACK) {
            user_write("[Net] heard a HELLO_ACK from actor ");
            user_write_dec64(msg.sender_actor);
            user_write(" on device ");
            user_write_mac(msg.sender_mac);
            user_write(" -- genuine cross-device actor communication confirmed\n");
            heard_ack = 1;
            for (int i = 0; i < 6; i++) {
                peer_mac[i] = msg.sender_mac[i];
            }
        }
    }

    if (heard_ack) {
        /* Phase 12's reliability primitive, exercised for real. This
         * side got confirmed quickly or slowly depending purely on
         * scheduling luck -- the peer that only ever REPLIED to a
         * HELLO (never got its own HELLO_ACK back) has no way to know
         * in advance whether or when a reliable PING is coming, so it
         * can't just exit the moment its own handshake loop ends: see
         * the drain phase below, which both roles now run for exactly
         * that reason -- a first version of this had the replier exit
         * immediately after its own 60-attempt budget, and lost the
         * race against a peer that took nearly that same 60-attempt
         * budget just to get ITS OWN ack, confirmed by real CI logs
         * showing the replier already gone before the pinger ever
         * sent anything. */
        user_write("[Net] sending one PING with a delivery guarantee...\n");
        int delivered = user_net_send_reliable_to(peer_mac, MSG_NET_PING, 0xDEAD);
        if (delivered == 0) {
            user_write("[Net] PING genuinely acked by the peer -- reliable delivery confirmed\n");
        } else {
            user_write("[Net] PING never acked within budget\n");
        }

        /* Phase 13a: ask the peer to run 'hello.bin' on ITS OWN
         * device. Reliable, not broadcast -- this is a request to a
         * SPECIFIC device, the same addressing PING above already
         * exercises, and the drain loop below is what actually sees
         * the peer's MSG_NET_SPAWN_REPLY once it arrives. */
        user_write("[Net] asking the peer to run 'hello.bin' on its OWN device...\n");
        int req_delivered = user_net_send_reliable_to(peer_mac, MSG_NET_SPAWN_REQUEST,
                                                        (uint64_t)HELLO_PROGRAM_OBJECT_ID);
        if (req_delivered != 0) {
            user_write("[Net] spawn request never acked within budget\n");
        }
    } else {
        user_write("[Net] no peer heard from within the listening window (single-instance run?)\n");
    }

    /* Stay reachable a while longer regardless of role above -- see
     * this function's own comment just above. Phase 13a extends this
     * from a pure discard loop into a real dispatch: a peer's
     * MSG_NET_SPAWN_REQUEST needs an actual local spawn attempt and a
     * reply, not just an auto-ACK (core/net.c's own auto-ACK already
     * handles the RELIABLE delivery side of that reliable send above;
     * this is the APPLICATION-level response on top of it, same
     * layering as MSG_NET_HELLO's own reply above). */
    for (int drain = 0; drain < 40; drain++) {
        struct net_message msg;
        int got = user_net_receive(&msg, 50000000);
        if (got != 1) {
            continue;
        }

        if (msg.type == MSG_NET_SPAWN_REQUEST) {
            user_write("[Net] peer on device ");
            user_write_mac(msg.sender_mac);
            user_write(" asked me to run object ");
            user_write_dec64(msg.data);
            user_write(" -- their request grants them nothing here; only MY OWN existing"
                       " capabilities decide whether this is allowed\n");
            int slot = user_spawn_program((int)msg.data);
            if (slot >= 0) {
                user_write("[Net] spawned as my own local actor ");
                user_write_dec64((uint64_t)slot);
                user_write("\n");
            } else {
                user_write("[Net] refused (not authorized here, pool exhausted, or not a"
                           " trusted program)\n");
            }
            user_net_send_to(msg.sender_mac, MSG_NET_SPAWN_REPLY, (uint64_t)slot);
        } else if (msg.type == MSG_NET_SPAWN_REPLY) {
            if ((int64_t)msg.data >= 0) {
                user_write("[Net] the peer confirmed: my request is now running as ITS OWN"
                           " local actor ");
                user_write_dec64(msg.data);
                user_write(" -- a program I named is now genuinely executing on a DIFFERENT"
                           " device, with only the authority THAT device already had\n");
            } else {
                user_write("[Net] the peer refused my spawn request\n");
            }
        }
        /* Anything else (a stray HELLO/HELLO_ACK/PING, e.g. from a
         * third instance sharing the link) is simply ignored -- same
         * as this loop always did before Phase 13a. */
    }
    user_exit();
}

/* Roadmap Phase 16's own verification target, run from an ordinary
 * ring-3 actor like everything else in this demo: SYS_SPAWN_PROGRAM
 * on the "hello.bin" object kernel_main seeded at boot (its bytes
 * came from a genuinely separately-compiled program -- see
 * src/userland/hello.c and tools/build-c.ps1's program-build step,
 * never part of this kernel image's own C sources). If the loader
 * works, the spawned actor's own user_write() call inside hello.c
 * prints its message through the exact same console path everything
 * else here uses -- real, visible proof it actually ran, not just
 * that SYS_SPAWN_PROGRAM returned a plausible-looking slot number.
 *
 * Moved above actor_network_peer (roadmap Phase 13a) -- that function
 * now names HELLO_PROGRAM_OBJECT_ID too, in its own spawn-request
 * exchange. */

__attribute__((section(".user_text")))
static void actor_program_loader(void) {
    user_write("[Loader] spawning the loaded 'hello' program from storage object ");
    user_write_dec64((uint64_t)HELLO_PROGRAM_OBJECT_ID);
    user_write("...\n");

    int slot = user_spawn_program(HELLO_PROGRAM_OBJECT_ID);
    if (slot < 0) {
        user_write("[Loader] failed to load and spawn the program\n");
    } else {
        user_write("[Loader] loaded program is now running as actor ");
        user_write_dec64((uint64_t)slot);
        user_write("\n");
    }

    user_write("[Loader] spawning the VajraLang-compiled 'calc' program from storage object ");
    user_write_dec64((uint64_t)CALC_PROGRAM_OBJECT_ID);
    user_write("...\n");
    int calc_slot = user_spawn_program(CALC_PROGRAM_OBJECT_ID);
    if (calc_slot < 0) {
        user_write("[Loader] failed to load and spawn calc.bin\n");
    } else {
        user_write("[Loader] calc.bin is now running as actor ");
        user_write_dec64((uint64_t)calc_slot);
        user_write("\n");
    }

    user_exit();
}

/* Roadmap Phase 17's own verification target: a persistent name ->
 * object-id directory layered over Phase 8's capability-addressed
 * object store (core/storage.c). Runs the full lifecycle a real shell
 * will eventually drive from user input -- lookup, enumerate,
 * create, rename, delete -- and shows the two access rules the
 * roadmap's own "resolved design constraint" calls for: a lookup by
 * a name you already know needs no capability at all (an id is public
 * knowledge), while enumerating every name that EXISTS is its own,
 * separately-granted authority (CAP_LIST_NAMES). The "hello.bin"/
 * "payload.bin"/"suspicious.bin" objects kernel_main seeds every boot
 * are found here by NAME for the first time, not by a hardcoded id --
 * real proof the directory is actually being consulted, not bypassed. */
__attribute__((section(".user_text")))
static void actor_namer(void) {
    int found = user_lookup_name("payload.bin");
    user_write("[Namer] lookup 'payload.bin' -> id ");
    user_write_dec64((uint64_t)found);
    user_write("\n");

    user_write("[Namer] listing the namespace:\n");
    for (int i = 0; ; i++) {
        struct object_info info;
        int rc = user_list_objects(i, &info);
        if (rc != 1) {
            break;
        }
        user_write("  [");
        user_write_dec64((uint64_t)info.id);
        user_write("] ");
        user_write(info.name);
        user_write(" (");
        user_write_trust(info.trust);
        user_write(")\n");
    }

    user_write("[Namer] creating 'notes.txt'...\n");
    int id = user_create_name("notes.txt");
    if (id < 0) {
        user_write("[Namer] create failed!\n");
        user_exit();
    }
    user_write("[Namer] created id ");
    user_write_dec64((uint64_t)id);
    user_write("\n");

    user_write("[Namer] renaming it to 'todo.txt'...\n");
    int rc = user_rename_object(id, "todo.txt");
    user_write(rc == 0 ? "[Namer] renamed\n" : "[Namer] rename failed!\n");

    user_write("[Namer] deleting it...\n");
    rc = user_delete_name(id);
    user_write(rc == 0 ? "[Namer] deleted\n" : "[Namer] delete failed!\n");

    int gone = user_lookup_name("todo.txt");
    user_write(gone < 0
        ? "[Namer] confirmed: 'todo.txt' is gone (lookup returned -1)\n"
        : "[Namer] SECURITY FAILURE: deleted name still resolves!\n");

    user_exit();
}

/* Roadmap Phase 18's own job-control demo pair, spawned by the shell's
 * `pipe` built-in below -- concrete proof of the roadmap's own
 * resolved design constraint: "a pipe is just another mailbox with a
 * different actor on each end, not a new mechanism." No new syscalls
 * or plumbing at all: the shell spawns Sink first (auto-granting
 * itself CAP_SEND+CAP_TERMINATE for it, same as every spawn), spawns
 * Source, DELEGATES its own freshly auto-granted CAP_SEND-to-Sink to
 * Source (ordinary SYS_GRANT, Phase 6), then sends Source an initial
 * message naming Sink's slot -- the same "learn your target from a
 * message, not a spawn-time argument" pattern actor_worker() already
 * uses for task.sender. */
#define MSG_PIPE_INIT 20
#define MSG_PIPE_DATA 21

__attribute__((section(".user_text")))
static void actor_pipe_sink(void) {
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        struct message m;
        user_receive(&m);
        sum += (int)m.data;
    }
    user_write("[Pipe] sink received 5 values, sum = ");
    user_write_dec64((uint64_t)sum);
    user_write("\n");
    user_exit();
}

__attribute__((section(".user_text")))
static void actor_pipe_source(void) {
    struct message init;
    user_receive(&init); /* {type=MSG_PIPE_INIT, data=sink's slot} */
    int sink = (int)init.data;
    for (int i = 1; i <= 5; i++) {
        user_send(sink, MSG_PIPE_DATA, (uint64_t)i);
    }
    user_write("[Pipe] source sent 5 values to sink\n");
    user_exit();
}

/* The shell's `count` built-in target -- a long-enough-running,
 * genuinely still-alive actor to make `jobs`/`stop`/`kill` demonstrable
 * against something real, unlike hello.bin (Phase 16), which finishes
 * almost instantly. Ticks 0..9 with a yield between each, checking its
 * OWN mailbox non-blockingly each round for the graceful-stop message
 * the shell's `stop` built-in sends -- the "tier 1" half of Phase 18's
 * two-tier interrupt model (docs/ROADMAP.md's own resolved design
 * constraint): an ordinary message the target checks at its own safe
 * points and may act on, ignore, or (as here) simply not be listening
 * for yet when `kill` (tier 2, SYS_TERMINATE) ends it directly instead. */
#define MSG_PLEASE_STOP 22

__attribute__((section(".user_text")))
static void actor_slow_counter(void) {
    for (int i = 0; i < 10; i++) {
        user_write("[Count] ");
        user_write_dec64((uint64_t)i);
        user_write("\n");
        for (int spin = 0; spin < 3; spin++) {
            user_yield();
        }
    }
    user_write("[Count] done\n");
    user_exit();
}

/* Roadmap Phase 18's own headline actor: a real interactive shell,
 * reading from the keyboard (this milestone's whole reason for
 * existing) rather than running a fixed scripted sequence like every
 * actor above. Line editing (backspace supported), a colored prompt
 * via the escape codes hal/x86_64/console.c now understands, and
 * built-ins covering the namespace (Phase 17), the loader (Phase 16),
 * the RTC, and job control (spawn/stop/kill) -- everything this
 * milestone's new HAL surface actually unlocks, driven by a real
 * human typing, not a script. */
#define SHELL_LINE_MAX 64
#define SHELL_MAX_JOBS 8

/* Deliberately NOT a string-literal comparison (`shell_str_eq(cmd,
 * "help")` was the first version of this, and it genuinely crashed --
 * a real #PF, CPL3, error code 0x5, confirmed by actually typing a
 * command into a running shell via QEMU's monitor `sendkey`, not
 * assumed). A string literal like "help" lives in .rodata, and .rodata
 * is supervisor-only, same as .text except .user_text -- exactly the
 * pitfall core/main.c's own user_write_trust() already documents and
 * avoids with an if/else chain instead of a lookup table. Six
 * individually-passed char PARAMETERS, unlike a string literal, compile
 * to immediate values at the call site (baked into the instruction
 * stream, .user_text itself), never a .rodata blob a ring-3 pointer
 * would have to dereference -- the same reasoning as an integer
 * literal, applied to text. Covers every built-in name here (longest
 * are "clear"/"count" at 5 characters); trailing unused slots pass 0. */
__attribute__((section(".user_text")))
static int shell_cmd_is(const char *cmd, char c0, char c1, char c2, char c3, char c4, char c5) {
    if (cmd[0] != c0) { return 0; }
    if (c0 == 0) { return 1; }
    if (cmd[1] != c1) { return 0; }
    if (c1 == 0) { return 1; }
    if (cmd[2] != c2) { return 0; }
    if (c2 == 0) { return 1; }
    if (cmd[3] != c3) { return 0; }
    if (c3 == 0) { return 1; }
    if (cmd[4] != c4) { return 0; }
    if (c4 == 0) { return 1; }
    if (cmd[5] != c5) { return 0; }
    if (c5 == 0) { return 1; }
    return cmd[6] == 0;
}

__attribute__((section(".user_text")))
static int shell_parse_int(const char *s) {
    int v = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    return v;
}

__attribute__((section(".user_text")))
static void shell_split(const char *line, char *cmd, char *arg) {
    int i = 0, j = 0;
    while (line[i] == ' ') { i++; }
    while (line[i] && line[i] != ' ' && j < SHELL_LINE_MAX - 1) { cmd[j++] = line[i++]; }
    cmd[j] = 0;
    while (line[i] == ' ') { i++; }
    j = 0;
    while (line[i] && j < SHELL_LINE_MAX - 1) { arg[j++] = line[i++]; }
    arg[j] = 0;
}

/* Non-blocking key-by-key line read, polling SYS_KEY_READ and yielding
 * between attempts (the same shape actor_network_peer's own polling
 * loop already established for SYS_NET_RECEIVE) -- there is no
 * "block this actor until a key arrives" scheduler primitive, and
 * building one for a single device wasn't worth it (see keyboard.c's
 * own comment). Enter or backspace get real handling; every other
 * printable ASCII byte is echoed and appended. */
__attribute__((section(".user_text")))
static int shell_read_line(char *buf) {
    int len = 0;
    for (;;) {
        struct mouse_state m;
        user_mouse_read(&m); /* docs/DESKTOP_DESIGN.md Stage 1 -- non-blocking, ignores -1
                                 (nothing new) the same way this loop already ignores a -1 from
                                 user_key_read() below; drawing the cursor glyph itself happens
                                 kernel-side (SYS_MOUSE_READ's own handler), so there's nothing
                                 further to do with a successful read here yet -- no click
                                 handling until docs/DESKTOP_DESIGN.md's later stages. */

        int c = user_key_read();
        if (c < 0) {
            user_yield();
            continue;
        }
        if (c == '\n' || c == '\r') {
            user_write("\n");
            buf[len] = 0;
            return len;
        }
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                user_write("\b \b");
            }
            continue;
        }
        if (len < SHELL_LINE_MAX - 1 && c >= 0x20 && c < 0x7F) {
            char echo[2];
            echo[0] = (char)c;
            echo[1] = 0;
            buf[len++] = (char)c;
            user_write(echo);
        }
    }
}

__attribute__((section(".user_text")))
static void actor_shell(void) {
    int job_slots[SHELL_MAX_JOBS];
    int job_count = 0;

    user_write("\x1b[36m");
    user_write("Vajra shell -- type 'help' for commands.\n");
    user_write("\x1b[0m");

    char line[SHELL_LINE_MAX];
    char cmd[SHELL_LINE_MAX];
    char arg[SHELL_LINE_MAX];

    for (;;) {
        user_write("\x1b[36mvajra> \x1b[0m");
        shell_read_line(line);
        shell_split(line, cmd, arg);

        if (cmd[0] == 0) {
            continue;
        } else if (shell_cmd_is(cmd, 'h','e','l','p',0,0)) {
            user_write("Commands: help ls run <name> echo <text> date clear\n");
            user_write("          count pipe jobs stop <slot> kill <slot> exit\n");
        } else if (shell_cmd_is(cmd, 'c','l','e','a','r',0)) {
            user_write("\x1b[2J\x1b[1;1H");
        } else if (shell_cmd_is(cmd, 'e','c','h','o',0,0)) {
            user_write(arg);
            user_write("\n");
        } else if (shell_cmd_is(cmd, 'd','a','t','e',0,0)) {
            struct rtc_time t;
            user_rtc_read(&t);
            user_write_dec64((uint64_t)t.year);
            user_write("-");
            user_write_dec64((uint64_t)t.month);
            user_write("-");
            user_write_dec64((uint64_t)t.day);
            user_write(" ");
            user_write_dec64((uint64_t)t.hours);
            user_write(":");
            user_write_dec64((uint64_t)t.minutes);
            user_write(":");
            user_write_dec64((uint64_t)t.seconds);
            user_write(" (UTC, from the CMOS RTC)\n");
        } else if (shell_cmd_is(cmd, 'l','s',0,0,0,0)) {
            for (int i = 0; ; i++) {
                struct object_info info;
                int rc = user_list_objects(i, &info);
                if (rc != 1) {
                    break;
                }
                user_write("  [");
                user_write_dec64((uint64_t)info.id);
                user_write("] ");
                user_write(info.name);
                user_write(" (");
                user_write_trust(info.trust);
                user_write(")\n");
            }
        } else if (shell_cmd_is(cmd, 'r','u','n',0,0,0)) {
            int id = user_lookup_name(arg);
            if (id < 0) {
                user_write("run: no such object\n");
            } else {
                int slot = user_spawn_program(id);
                if (slot < 0) {
                    user_write("run: failed (not a valid program, or not authorized to read it)\n");
                } else {
                    user_write("run: started as actor ");
                    user_write_dec64((uint64_t)slot);
                    user_write("\n");
                    if (job_count < SHELL_MAX_JOBS) {
                        job_slots[job_count++] = slot;
                    }
                }
            }
        } else if (shell_cmd_is(cmd, 'c','o','u','n','t',0)) {
            int slot = user_spawn(actor_slow_counter);
            if (slot < 0) {
                user_write("count: spawn failed (quota exceeded?)\n");
            } else {
                user_write("count: started as actor ");
                user_write_dec64((uint64_t)slot);
                user_write(" (background)\n");
                if (job_count < SHELL_MAX_JOBS) {
                    job_slots[job_count++] = slot;
                }
            }
        } else if (shell_cmd_is(cmd, 'p','i','p','e',0,0)) {
            int sink = user_spawn(actor_pipe_sink);
            int source = (sink >= 0) ? user_spawn(actor_pipe_source) : -1;
            if (sink < 0 || source < 0) {
                user_write("pipe: spawn failed (quota exceeded?)\n");
            } else {
                user_grant(source, CAP_SEND, sink); /* delegating the CAP_SEND-to-sink the
                                                        shell's own spawn of sink just
                                                        auto-granted it -- see this
                                                        function group's own top comment */
                user_send(source, MSG_PIPE_INIT, (uint64_t)sink);
                user_write("pipe: wired actor ");
                user_write_dec64((uint64_t)source);
                user_write(" -> actor ");
                user_write_dec64((uint64_t)sink);
                user_write("\n");
                if (job_count < SHELL_MAX_JOBS - 1) {
                    job_slots[job_count++] = sink;
                    job_slots[job_count++] = source;
                }
            }
        } else if (shell_cmd_is(cmd, 'j','o','b','s',0,0)) {
            if (job_count == 0) {
                user_write("(no jobs spawned this session)\n");
            } else {
                for (int i = 0; i < job_count; i++) {
                    user_write("  actor ");
                    user_write_dec64((uint64_t)job_slots[i]);
                    user_write("\n");
                }
            }
        } else if (shell_cmd_is(cmd, 's','t','o','p',0,0)) {
            int slot = shell_parse_int(arg);
            int rc = user_send(slot, MSG_PLEASE_STOP, 0);
            user_write(rc == 0
                ? "stop: graceful-stop message sent (target may or may not act on it)\n"
                : "stop: could not send (no CAP_SEND for that slot, or it's dead)\n");
        } else if (shell_cmd_is(cmd, 'k','i','l','l',0,0)) {
            int slot = shell_parse_int(arg);
            int rc = user_terminate(slot);
            user_write(rc == 0 ? "kill: terminated\n" : "kill: failed (no CAP_TERMINATE, or already dead)\n");
        } else if (shell_cmd_is(cmd, 'e','x','i','t',0,0)) {
            user_write("Shell exiting.\n");
            user_exit();
        } else {
            user_write("unknown command (try 'help')\n");
        }
    }
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
/* Built by tools/build-c.ps1 from src/userland/{runtime,hello}.c ->
 * build/hello.bin (header-prefixed, see include/vajra/loader.h), then
 * wrapped as inert .rodata by hal/x86_64/hello_blob.asm -- the exact
 * same technique hal/x86_64/smp.c already uses for the AP trampoline,
 * see that file's own comment for why this can't just be compiled
 * into the kernel image the normal way (it's a wholly separate link,
 * fixed at PROGRAM_VBASE, not this image's own address space at all). */
extern uint8_t hello_blob[];
extern uint8_t hello_blob_end[];

/* Built by tools/vajrac.ps1 (VajraLang -> C) then tools/build-c.ps1's
 * own userland pipeline (build/calc_gen.c -> build/calc.bin), wrapped
 * exactly like hello_blob above -- see hal/x86_64/calc_blob.asm. A
 * genuine "does OUR OWN compiler produce a real, running Vajra
 * program" proof, not just "does the loader accept hand-written C". */
extern uint8_t calc_blob[];
extern uint8_t calc_blob_end[];

void kernel_main(void) {
    hal_console_init();
    hal_console_write("VAJRA OS (C rewrite) - Milestone 18\n");
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

    /* Roadmap Phase 18: the kernel's first input device. Must come
     * after hal_pic_remap() (which is what unmasks IRQ1 now) and
     * before hal_enable_interrupts() near the end of this function --
     * keystrokes could otherwise start arriving (and being silently
     * lost, since hal_keyboard_init()'s drain hasn't run yet) before
     * the driver is ready for them. */
    hal_keyboard_init();
    hal_console_write("Keyboard online (PS/2, IRQ1).\n");

    /* docs/DESKTOP_DESIGN.md Stage 1: the second input device, same
     * ordering constraint as the keyboard just above (must follow
     * hal_pic_remap()'s IRQ12 unmask, must precede hal_enable_
     * interrupts() near the end of this function). */
    hal_mouse_init();
    hal_console_write("Mouse online (PS/2, IRQ12).\n");

    /* Switches the screen over to the real desktop for the first time
     * -- see console.c's own two-phase-init comment for why this can
     * only happen now (needs memory_init(), already run above) and
     * not from hal_console_init() itself (which ran much earlier, and
     * also doubles as the panic screen's reset). Every hal_console_
     * write() call above this point rendered as plain scrolling boot
     * text (the pre-desktop fallback); everything below renders into
     * an app's own offscreen buffer instead, invisible until that
     * app is actually focused on screen. */
    hal_console_alloc_windows();

    hal_console_set_window(CONSOLE_WIN_ABOUT);
    hal_console_write("Vajra OS\n\n");
    hal_console_write("A from-scratch x86-64 kernel built around actors,\n");
    hal_console_write("capabilities, and message passing -- no ambient\n");
    hal_console_write("authority, and nothing trusted by default.\n\n");
    hal_console_write("Memory detected: ");
    hal_console_write_dec64(memory_get_total_bytes() / (1024 * 1024));
    hal_console_write(" MB\n");
    hal_console_set_window(CONSOLE_WIN_LOG);

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
    actor_spawn(actor_program_loader);   /* must land at PROGRAM_LOADER_SLOT */
    actor_spawn(actor_namer);            /* must land at NAMESPACE_DEMO_SLOT */
    actor_spawn(actor_shell);            /* must land at SHELL_SLOT */

    int payload_id    = storage_create_object("payload.bin");    /* must be PAYLOAD_OBJECT_ID */
    int suspicious_id = storage_create_object("suspicious.bin"); /* must be SUSPICIOUS_OBJECT_ID */
    hal_console_write("Storage online. Created objects 'payload.bin' (id ");
    hal_console_write_dec64((uint64_t)payload_id);
    hal_console_write(") and 'suspicious.bin' (id ");
    hal_console_write_dec64((uint64_t)suspicious_id);
    hal_console_write(").\n");

    /* Seeds the loaded program's bytes into the object store, the same
     * way a real filesystem/install step will later (Phase 17/20) --
     * for now, kernel_main is the one place allowed to write storage
     * objects directly (it's ring 0, not going through a syscall),
     * exactly like the two storage_create_object() calls just above. */
    int hello_id = storage_create_object("hello.bin"); /* must be HELLO_PROGRAM_OBJECT_ID */
    uint32_t hello_len = (uint32_t)(hello_blob_end - hello_blob);
    storage_write(hello_id, hello_blob, hello_len);
    /* Roadmap Phase 27: storage_write() resets trust to OBJ_UNTRUSTED
     * on every write (storage.c's own comment -- new content
     * invalidates any prior trust decision), and loader_spawn_program()
     * now refuses to load anything short of OBJ_TRUSTED. This IS the
     * kernel vouching for its own built-in demo program the same way
     * Scanner/Inspector vouches for a downloaded one (Phase 20) -- three
     * storage_promote() calls, one per step of the same
     * UNTRUSTED -> QUARANTINED -> ANALYZED -> TRUSTED pipeline, not a
     * bypass of it. */
    storage_promote(hello_id);
    storage_promote(hello_id);
    storage_promote(hello_id);
    hal_console_write("Loader: seeded 'hello.bin' (id ");
    hal_console_write_dec64((uint64_t)hello_id);
    hal_console_write(", ");
    hal_console_write_dec64((uint64_t)hello_len);
    hal_console_write(" bytes) as a loadable program.\n");

    int calc_id = storage_create_object("calc.bin"); /* must be CALC_PROGRAM_OBJECT_ID */
    uint32_t calc_len = (uint32_t)(calc_blob_end - calc_blob);
    storage_write(calc_id, calc_blob, calc_len);
    storage_promote(calc_id); /* same reasoning as hello_id above */
    storage_promote(calc_id);
    storage_promote(calc_id);
    hal_console_write("Loader: seeded 'calc.bin' (id ");
    hal_console_write_dec64((uint64_t)calc_id);
    hal_console_write(", ");
    hal_console_write_dec64((uint64_t)calc_len);
    hal_console_write(" bytes) -- VajraLang-compiled, from src/userland/calc.vj.\n");

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
    /* Roadmap Phase 13a: the SAME authority PROGRAM_LOADER_SLOT already
     * holds to run hello.bin/calc.bin locally, granted here too so this
     * actor can independently decide whether to honor a peer's
     * MSG_NET_SPAWN_REQUEST -- nothing the peer sends ever grants this,
     * only kernel_main's own local decision does (see
     * actor_network_peer's own top-of-function comment). */
    actor_grant(NETWORK_PEER_SLOT, CAP_SPAWN, 0);
    actor_grant(NETWORK_PEER_SLOT, CAP_READ_OBJECT, HELLO_PROGRAM_OBJECT_ID);
    actor_grant(NETWORK_PEER_SLOT, CAP_READ_OBJECT, CALC_PROGRAM_OBJECT_ID);

    actor_grant(PROGRAM_LOADER_SLOT, CAP_SPAWN, 0);
    actor_grant(PROGRAM_LOADER_SLOT, CAP_READ_OBJECT, HELLO_PROGRAM_OBJECT_ID);
    actor_grant(PROGRAM_LOADER_SLOT, CAP_READ_OBJECT, CALC_PROGRAM_OBJECT_ID);

    actor_grant(NAMESPACE_DEMO_SLOT, CAP_LIST_NAMES, 0);
    actor_grant(NAMESPACE_DEMO_SLOT, CAP_CREATE_OBJECT, 0);

    /* The shell: CAP_CONSOLE for keyboard input (the ONLY actor that
     * gets it -- everyone else could ask, but only this one is
     * granted it, the same "only Scanner gets CAP_PROMOTE_OBJECT"
     * least-privilege shape as the rest of this demo), CAP_SPAWN for
     * `run`/`count`/`pipe`, CAP_LIST_NAMES for `ls`, and
     * CAP_READ_OBJECT for hello.bin specifically -- NOT a blanket
     * grant, so `run` only works on programs the shell was actually
     * authorized to read, exactly like every other object capability
     * in this codebase (Phase 17/19's own "visibility != authority"
     * design constraint applies to the shell too, not just to
     * background actors). */
    actor_grant(SHELL_SLOT, CAP_CONSOLE, 0);
    actor_grant(SHELL_SLOT, CAP_SPAWN, 0);
    actor_grant(SHELL_SLOT, CAP_LIST_NAMES, 0);
    actor_grant(SHELL_SLOT, CAP_READ_OBJECT, HELLO_PROGRAM_OBJECT_ID);
    actor_grant(SHELL_SLOT, CAP_READ_OBJECT, CALC_PROGRAM_OBJECT_ID); /* run calc.bin */
    actor_set_spawn_quota(SHELL_SLOT, 6); /* see actor.h's own comment -- a per-actor override,
                                              not a change to every other actor's quota */
    actor_set_window(SHELL_SLOT, CONSOLE_WIN_SHELL); /* the shell's own pane -- see console.c's
                                                          own top comment for why this exists */

    hal_console_write("\nStarting preemptive scheduler with 15 ring-3 actors...\n\n");

    hal_enable_interrupts();
    scheduler_start();
}
