#include "vajra/hal.h"
#include "vajra/memory.h"
#include "vajra/actor.h"
#include "vajra/storage.h"
#include "vajra/packages.h"
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
static void user_sleep(uint64_t ticks) {
    hal_syscall(SYS_SLEEP, ticks, 0, 0);
}

__attribute__((section(".user_text")))
static int user_actor_info(int slot, struct actor_info *out) {
    return (int)hal_syscall(SYS_ACTOR_INFO, (uint64_t)slot, (uint64_t)out, 0);
}

__attribute__((section(".user_text")))
static int user_pkg_list(int index, struct pkg_info *out) {
    return (int)hal_syscall(SYS_PKG_LIST, (uint64_t)index, (uint64_t)out, 0);
}

__attribute__((section(".user_text")))
static int user_pkg_stage(int index) {
    return (int)hal_syscall(SYS_PKG_STAGE, (uint64_t)index, 0, 0);
}

__attribute__((section(".user_text")))
static int user_pkg_verdict(int id, int pass) {
    return (int)hal_syscall(SYS_PKG_VERDICT, (uint64_t)id, (uint64_t)pass, 0);
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
#define LAB_SLOT              15 /* Security Lab -- see actor_lab() */
#define CORES_SLOT            16 /* Cores app -- see actor_cores() */
#define INSTALLER_SLOT        17 /* Phase 20 package installer -- see actor_installer() */
#define MSG_PKG_INSTALL       0x70 /* shell -> installer: data = catalog index */
#define MSG_PKG_DONE          0x71 /* installer -> shell: data = object id (>= 0), or PKG_ERR_* */
#define PKG_ERR_REJECTED      (-1) /* failed inspection -- permanently rejected */
#define PKG_ERR_PRESENT       (-2) /* an object of that name already exists */
#define PKG_ERR_BAD_INDEX     (-3)
#define PKG_ERR_NO_SPACE      (-4) /* object store full */
#define PKG_ERR_NO_INSPECTOR  (-5) /* couldn't start the sandboxed inspector */

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
    /* No yields between the send and the terminate: an earlier version
     * yielded twice here, which quietly let Worker 2 finish and exit
     * on its own before the terminate ran -- printing "terminate
     * failed!" whenever the round-robin got longer (it did, once the
     * Security Lab became a 16th actor). Terminating right away is
     * deterministic: the worker can't have run yet. */
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

/* SYS_SPAWN_PROGRAM with a bounded retry. All MAX_ACTORS (24) slots are
 * routinely full early in the demo (15 static actors + Coordinator's
 * Worker + Scanner's Inspector), so a spawn can fail purely because no
 * slot is free YET -- a transient condition that clears as soon as any
 * of those short-lived actors exits. CI's timing hit this on both
 * hello.bin and calc.bin while this dev machine's happened not to; a
 * single unretried attempt turned a race into a hard failure. Yielding
 * between attempts lets the scheduler actually run those actors. A
 * genuine refusal (no capability, untrusted object, spawn quota) fails
 * every attempt the same way and still surfaces, just after the budget. */
__attribute__((section(".user_text")))
static int user_spawn_program_retry(int object_id) {
    int slot = -1;
    for (int attempt = 0; attempt < 100; attempt++) {
        slot = user_spawn_program(object_id);
        if (slot >= 0) {
            return slot;
        }
        user_yield();
    }
    return slot;
}

/* Phase 13a's application-level handling of a peer's spawn request/
 * reply. A shared function, called from BOTH of actor_network_peer's
 * receive loops: the first version handled these only in the final
 * drain loop, but core/net.c auto-ACKs ANY decoded data frame inside
 * user_net_receive() regardless of which loop called it -- so a request
 * that arrived while this actor was still in its HELLO handshake loop
 * was ACKed (the sender's reliable send succeeded) and then silently
 * dropped, since that loop only dispatches HELLO/HELLO_ACK. Confirmed
 * by CI: the requesting peer's PING and request were both ACKed, yet
 * the receiving peer never logged the request. */
__attribute__((section(".user_text")))
static void net_handle_spawn_msg(const struct net_message *msg) {
    if (msg->type == MSG_NET_SPAWN_REQUEST) {
        user_write("[Net] peer on device ");
        user_write_mac(msg->sender_mac);
        user_write(" asked me to run object ");
        user_write_dec64(msg->data);
        user_write(" -- their request grants them nothing here; only MY OWN existing"
                   " capabilities decide whether this is allowed\n");
        int slot = user_spawn_program_retry((int)msg->data);
        if (slot >= 0) {
            user_write("[Net] spawned as my own local actor ");
            user_write_dec64((uint64_t)slot);
            user_write("\n");
        } else {
            user_write("[Net] refused (not authorized here, no free actor slot, or not a"
                       " trusted program)\n");
        }
        user_net_send_to(msg->sender_mac, MSG_NET_SPAWN_REPLY, (uint64_t)slot);
    } else if (msg->type == MSG_NET_SPAWN_REPLY) {
        if ((int64_t)msg->data >= 0) {
            user_write("[Net] the peer confirmed: my request is now running as ITS OWN"
                       " local actor ");
            user_write_dec64(msg->data);
            user_write(" -- a program I named is now genuinely executing on a DIFFERENT"
                       " device, with only the authority THAT device already had\n");
        } else {
            user_write("[Net] the peer refused my spawn request\n");
        }
    }
}

__attribute__((section(".user_text")))
static void actor_network_peer(void) {
    int rc = user_net_send(MSG_NET_HELLO, 0xC0FFEE);
    if (rc != 0) {
        user_write("[Net] no network capability or no device -- nothing to do\n");
        user_exit();
    }
    user_write("[Net] broadcast HELLO, listening for a peer...\n");

    int heard_ack = 0;
    int peer_known = 0; /* heard ANY HELLO or HELLO_ACK -- enough to address the peer later */
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
            for (int i = 0; i < 6; i++) {
                peer_mac[i] = msg.sender_mac[i];
            }
            peer_known = 1;
        } else if (msg.type == MSG_NET_HELLO_ACK) {
            user_write("[Net] heard a HELLO_ACK from actor ");
            user_write_dec64(msg.sender_actor);
            user_write(" on device ");
            user_write_mac(msg.sender_mac);
            user_write(" -- genuine cross-device actor communication confirmed\n");
            heard_ack = 1;
            peer_known = 1;
            for (int i = 0; i < 6; i++) {
                peer_mac[i] = msg.sender_mac[i];
            }
        } else {
            net_handle_spawn_msg(&msg);
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

        net_handle_spawn_msg(&msg);
        /* Anything else (a stray HELLO/HELLO_ACK/PING, e.g. from a
         * third instance sharing the link) is simply ignored -- same
         * as this loop always did before Phase 13a. */
    }

    /* The Fabric app's interactive mode: the actor no longer exits, it
     * stays as the window's owner so a person can drive the fabric by
     * hand. Keys reach it only while the Fabric window is focused.
     *   h  say HELLO again (find a peer that booted after us)
     *   p  ping the peer with a delivery guarantee
     *   r  ask the peer to run hello.bin on ITS OWN device
     * Receives use a SHORT spin then sleep: SYS_NET_RECEIVE spins inside
     * the kernel holding the big kernel lock, so a long spin here would
     * stall every other core. */
    user_write("[Net] interactive: h = say hello, p = ping the peer, r = ask the peer to run hello.bin\n");
    int have_peer = peer_known;
    for (;;) {
        int c = user_key_read();
        if (c == 'h' || c == 'H') {
            user_write("[Net] key h: broadcasting HELLO...\n");
            user_net_send(MSG_NET_HELLO, 0xC0FFEE);
        } else if (c == 'p' || c == 'P' || c == 'r' || c == 'R') {
            if (!have_peer) {
                user_write("[Net] key p/r: no peer known yet -- press h to look for one\n");
            } else if (c == 'p' || c == 'P') {
                int ok = user_net_send_reliable_to(peer_mac, MSG_NET_PING, 0xDEAD);
                user_write(ok == 0 ? "[Net] key p: PING acked by the peer -- reliable delivery confirmed\n"
                                   : "[Net] key p: PING never acked within budget\n");
            } else {
                user_write("[Net] key r: asking the peer to run 'hello.bin' on its OWN device...\n");
                int ok = user_net_send_reliable_to(peer_mac, MSG_NET_SPAWN_REQUEST,
                                                   (uint64_t)HELLO_PROGRAM_OBJECT_ID);
                user_write(ok == 0 ? "[Net] key r: request delivered (peer's own reply follows)\n"
                                   : "[Net] key r: spawn request never acked within budget\n");
            }
        }

        struct net_message msg;
        if (user_net_receive(&msg, 200000) == 1) {
            if (msg.type == MSG_NET_HELLO || msg.type == MSG_NET_HELLO_ACK) {
                for (int i = 0; i < 6; i++) {
                    peer_mac[i] = msg.sender_mac[i];
                }
                have_peer = 1;
                user_write(msg.type == MSG_NET_HELLO ? "[Net] heard a HELLO from device "
                                                     : "[Net] heard a HELLO_ACK from device ");
                user_write_mac(msg.sender_mac);
                user_write("\n");
                if (msg.type == MSG_NET_HELLO) {
                    user_net_send_to(msg.sender_mac, MSG_NET_HELLO_ACK, 0xBEEF);
                }
            } else if (msg.type == MSG_NET_PING) {
                user_write("[Net] a PING arrived from device ");
                user_write_mac(msg.sender_mac);
                user_write("\n");
            } else {
                net_handle_spawn_msg(&msg);
            }
        }
        user_sleep(2);
    }
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

    int slot = user_spawn_program_retry(HELLO_PROGRAM_OBJECT_ID);
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
    int calc_slot = user_spawn_program_retry(CALC_PROGRAM_OBJECT_ID);
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
#define SHELL_LINE_MAX 96
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
/* Moves the cursor left/right by writing backspaces / re-writing text --
 * the console has no relative cursor movement, but '\b' is non-destructive. */
__attribute__((section(".user_text")))
static void shell_back(int n) {
    for (int i = 0; i < n; i++) { user_write("\b"); }
}

__attribute__((section(".user_text")))
static void shell_spaces(int n) {
    for (int i = 0; i < n; i++) { user_write(" "); }
}

#define SHELL_HIST 6

__attribute__((section(".user_text")))
static int shell_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) { i++; }
    return a[i] == 0 && b[i] == 0;
}

/* Reads one line with real line editing: Left/Right/Home/End (also Ctrl-A /
 * Ctrl-E), Delete and Backspace at the cursor, insertion in the middle,
 * Ctrl-U to clear the line, and Up/Down through the last SHELL_HIST lines. */
__attribute__((section(".user_text")))
static int shell_read_line(char *buf, char (*hist)[SHELL_LINE_MAX], int *hist_n) {
    int len = 0;
    int pos = 0;
    int hist_pos = *hist_n; /* == *hist_n means "the line being typed" */
    for (;;) {
        struct mouse_state m;
        user_mouse_read(&m); /* non-blocking; the cursor glyph is drawn kernel-side */

        int c = user_key_read();
        if (c < 0) {
            user_sleep(1); /* not user_yield(): see SYS_SLEEP's comment in hal.h */
            continue;
        }
        if (c == '\n' || c == '\r') {
            user_write("\n");
            buf[len] = 0;
            if (len > 0 && (*hist_n == 0 || !shell_eq(hist[*hist_n - 1], buf))) {
                if (*hist_n == SHELL_HIST) {
                    for (int i = 1; i < SHELL_HIST; i++) {
                        for (int j = 0; j < SHELL_LINE_MAX; j++) { hist[i - 1][j] = hist[i][j]; }
                    }
                    (*hist_n)--;
                }
                for (int j = 0; j <= len; j++) { hist[*hist_n][j] = buf[j]; }
                (*hist_n)++;
            }
            return len;
        }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) { buf[i] = buf[i + 1]; }
                len--;
                pos--;
                user_write("\b");
                buf[len] = 0;
                user_write(buf + pos);
                user_write(" ");
                shell_back(len - pos + 1);
            }
        } else if (c == KEY_DELETE) {
            if (pos < len) {
                for (int i = pos; i < len - 1; i++) { buf[i] = buf[i + 1]; }
                len--;
                buf[len] = 0;
                user_write(buf + pos);
                user_write(" ");
                shell_back(len - pos + 1);
            }
        } else if (c == KEY_LEFT) {
            if (pos > 0) { pos--; user_write("\b"); }
        } else if (c == KEY_RIGHT) {
            if (pos < len) {
                char one[2];
                one[0] = buf[pos];
                one[1] = 0;
                user_write(one);
                pos++;
            }
        } else if (c == KEY_HOME || c == 1) {
            shell_back(pos);
            pos = 0;
        } else if (c == KEY_END || c == 5) {
            buf[len] = 0;
            user_write(buf + pos);
            pos = len;
        } else if (c == 21) { /* Ctrl-U: clear the line */
            shell_back(pos);
            shell_spaces(len);
            shell_back(len);
            len = 0;
            pos = 0;
        } else if (c == KEY_UP || c == KEY_DOWN) {
            int target = hist_pos + (c == KEY_UP ? -1 : 1);
            if (target >= 0 && target <= *hist_n) {
                if (hist_pos == *hist_n) {
                    /* leaving the line being typed: nothing to save beyond it on screen */
                }
                hist_pos = target;
                shell_back(pos);
                int newlen = 0;
                if (hist_pos < *hist_n) {
                    while (hist[hist_pos][newlen]) { buf[newlen] = hist[hist_pos][newlen]; newlen++; }
                }
                buf[newlen] = 0;
                user_write(buf);
                if (newlen < len) {
                    shell_spaces(len - newlen);
                    shell_back(len - newlen);
                }
                len = newlen;
                pos = newlen;
            }
        } else if (len < SHELL_LINE_MAX - 1 && c >= 0x20 && c < 0x7F) {
            for (int i = len; i > pos; i--) { buf[i] = buf[i - 1]; }
            buf[pos] = (char)c;
            len++;
            buf[len] = 0;
            user_write(buf + pos);
            pos++;
            shell_back(len - pos);
        }
    }
}

/* Phase 20's front end: `pkg list` and `pkg install <name>`. The shell
 * itself holds NO install authority -- it asks the installer actor (the
 * only holder of CAP_INSTALL_PACKAGE) and reports what came back. */
__attribute__((section(".user_text")))
static int pkg_streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) { i++; }
    return a[i] == 0 && b[i] == 0;
}

__attribute__((section(".user_text")))
static void shell_pkg(const char *arg) {
    char sub[SHELL_LINE_MAX];
    char name[SHELL_LINE_MAX];
    shell_split(arg, sub, name);
    struct pkg_info info;

    /* Subcommands are compared character by character: ring-3 code must never dereference a
     * string literal (it lives in the kernel image, unreadable from ring 3). */
    if (sub[0] == 'l' && sub[1] == 'i' && sub[2] == 's' && sub[3] == 't' && sub[4] == 0) {
        user_write("Package catalog:\n");
        for (int i = 0; user_pkg_list(i, &info) == 0; i++) {
            user_write("  ");
            user_write(info.name);
            int len = 0;
            while (info.name[len]) { len++; }
            for (int pad = len; pad < 12; pad++) { user_write(" "); }
            user_write_dec64((uint64_t)info.size);
            user_write(" bytes  ");
            if (info.state == PKG_INSTALLED)      { user_write("\x1b[32minstalled\x1b[0m\n"); }
            else if (info.state == PKG_REJECTED)  { user_write("\x1b[31mREJECTED (failed inspection)\x1b[0m\n"); }
            else if (info.state == PKG_UNVETTED)  { user_write("staged, not vetted\n"); }
            else                                  { user_write("not installed\n"); }
        }
    } else if (sub[0] == 'i' && sub[1] == 'n' && sub[2] == 's' && sub[3] == 't' && sub[4] == 'a' &&
               sub[5] == 'l' && sub[6] == 'l' && sub[7] == 0) {
        int index = -1;
        for (int i = 0; user_pkg_list(i, &info) == 0; i++) {
            if (pkg_streq(info.name, name)) { index = i; break; }
        }
        if (index < 0) {
            user_write("pkg: no such package (try 'pkg list')\n");
            return;
        }
        user_write("pkg: asking the installer to stage '");
        user_write(name);
        user_write("' and put it through inspection...\n");
        if (user_send(INSTALLER_SLOT, MSG_PKG_INSTALL, (uint64_t)index) != 0) {
            user_write("pkg: the installer is not available\n");
            return;
        }
        struct message m;
        do {
            user_receive(&m);
        } while (m.type != MSG_PKG_DONE);
        int r = (int)(int64_t)m.data;
        if (r >= 0) {
            user_write("\x1b[32mpkg: installed.\x1b[0m It was staged UNTRUSTED, inspected in a sandbox, then promoted -- try: run ");
            user_write(name);
            user_write("\n");
        } else if (r == PKG_ERR_REJECTED) {
            user_write("\x1b[31mpkg: REJECTED.\x1b[0m Inspection found a hostile marker; the object is permanently unusable.\n");
        } else if (r == PKG_ERR_PRESENT) {
            user_write("pkg: a package by that name is already present (see 'pkg list')\n");
        } else if (r == PKG_ERR_NO_SPACE) {
            user_write("pkg: the object store is full\n");
        } else {
            user_write("pkg: install failed (no sandbox slot)\n");
        }
    } else {
        user_write("usage: pkg list | pkg install <name>\n");
    }
}

/* ------------------------------------------------------------------
 * Phase 19: the standard utilities (ls cat cp mv rm grep edit ps), each a
 * separately loaded program from src/userland/util_*.c. The shell is the
 * user's agent: it holds CAP_USER_DATA (authority over the user's own
 * files) and CAP_CREATE_OBJECT, and for each command it hands the freshly
 * spawned, authority-less program exactly the capabilities that command
 * needs -- `cat notes.txt` gets read on notes.txt and nothing else -- then
 * sends the arguments and waits for it to end. Ask for something the shell
 * itself holds no authority over (say `cat payload.bin`) and the delegation
 * fails, so the utility runs, tries, and is refused by the kernel.
 * ---------------------------------------------------------------- */
#define MSG_UTIL_ARG     0x80
#define MSG_UTIL_ARG_END 0x81

__attribute__((section(".user_text")))
static int shell_util_kind(const char *cmd) {
    if (shell_cmd_is(cmd, 'l','s',0,0,0,0)) { return 1; }
    if (shell_cmd_is(cmd, 'c','a','t',0,0,0)) { return 2; }
    if (shell_cmd_is(cmd, 'c','p',0,0,0,0)) { return 3; }
    if (shell_cmd_is(cmd, 'm','v',0,0,0,0)) { return 4; }
    if (shell_cmd_is(cmd, 'r','m',0,0,0,0)) { return 5; }
    if (shell_cmd_is(cmd, 'g','r','e','p',0,0)) { return 6; }
    if (shell_cmd_is(cmd, 'e','d','i','t',0,0)) { return 7; }
    if (shell_cmd_is(cmd, 'p','s',0,0,0,0)) { return 8; }
    if (shell_cmd_is(cmd, 't','r','e','e',0,0)) { return 9; }
    return 0;
}

/* ------------------------------------------------------------------
 * Directories. The namespace is flat; a directory is a name convention:
 * the object "docs/" (size 0) marks a directory and "docs/notes.txt" lives
 * in it. The SHELL owns the notion of a current directory (`cwd`, "" for the
 * root, otherwise ending in '/') and resolves every path argument to a full
 * object name before a utility ever sees it -- so every utility, present
 * and future, works with paths without knowing they exist. Absolute paths
 * start with '/', "." and ".." work, and a name (path included) is at most
 * 39 characters.
 * ---------------------------------------------------------------- */

/* Copies the idx-th space-separated word of s into out (empty if there is none). */
__attribute__((section(".user_text")))
static void shell_word(const char *s, int idx, char *out, int max) {
    int i = 0;
    for (int w = 0; w <= idx; w++) {
        while (s[i] == ' ') { i++; }
        int start = i;
        while (s[i] && s[i] != ' ') { i++; }
        if (w == idx) {
            int n = 0;
            while (start + n < i && n < max - 1) { out[n] = s[start + n]; n++; }
            out[n] = 0;
            return;
        }
    }
    out[0] = 0;
}

/* Resolves `in` against `cwd` into a full object name. want_dir: the result is a
 * directory name and always ends in '/' (root resolves to ""). */
__attribute__((section(".user_text")))
static void shell_resolve(const char *cwd, const char *in, char *out, int want_dir) {
    int n = 0;
    int i = 0;
    if (in[0] == '/') {
        i = 1;
    } else {
        while (cwd[n] && n < 39) { out[n] = cwd[n]; n++; }
    }
    while (in[i]) {
        int start = i;
        while (in[i] && in[i] != '/') { i++; }
        int clen = i - start;
        int last = (in[i] == 0);
        if (in[i] == '/') { i++; }
        if (clen == 0 || (clen == 1 && in[start] == '.')) { continue; }
        if (clen == 2 && in[start] == '.' && in[start + 1] == '.') {
            if (n > 0) {
                n--;
                while (n > 0 && out[n - 1] != '/') { n--; }
            }
            continue;
        }
        for (int k = 0; k < clen && n < 39; k++) { out[n++] = in[start + k]; }
        if ((!last || want_dir) && n < 39) { out[n++] = '/'; }
    }
    out[n] = 0;
}

__attribute__((section(".user_text")))
static int shell_has_prefix(const char *name, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (name[i] != prefix[i]) { return 0; }
        i++;
    }
    return 1;
}

/* Appends src to dst (which currently holds *n chars), bounded by max. */
__attribute__((section(".user_text")))
static void shell_cat(char *dst, int *n, const char *src, int max) {
    for (int i = 0; src[i] && *n < max - 1; i++) { dst[(*n)++] = src[i]; }
    dst[*n] = 0;
}

/* cd / pwd / mkdir / rmdir. Returns 1 if `cmd` was one of them. */
__attribute__((section(".user_text")))
static int shell_dircmd(const char *cmd, const char *arg, char *cwd) {
    char full[48];
    if (shell_cmd_is(cmd, 'p','w','d',0,0,0)) {
        user_write("/");
        int n = 0;
        while (cwd[n]) { n++; }
        char shown[48];
        for (int i = 0; i < n && i < 46; i++) { shown[i] = cwd[i]; }
        if (n > 0) { n--; }              /* drop the trailing '/' */
        shown[n] = 0;
        user_write(shown);
        user_write("\n");
        return 1;
    }
    if (shell_cmd_is(cmd, 'c','d',0,0,0,0)) {
        char a[SHELL_LINE_MAX];
        shell_word(arg, 0, a, SHELL_LINE_MAX);
        shell_resolve(cwd, a, full, 1);
        if (a[0] == 0) { full[0] = 0; }  /* plain `cd` goes home to the root */
        if (full[0] != 0 && user_lookup_name(full) < 0) {
            user_write("cd: no such directory\n");
            return 1;
        }
        int n = 0;
        while (full[n]) { cwd[n] = full[n]; n++; }
        cwd[n] = 0;
        return 1;
    }
    if (shell_cmd_is(cmd, 'm','k','d','i','r',0)) {
        char a[SHELL_LINE_MAX];
        shell_word(arg, 0, a, SHELL_LINE_MAX);
        shell_resolve(cwd, a, full, 1);
        if (a[0] == 0 || full[0] == 0) { user_write("usage: mkdir <name>\n"); return 1; }
        if (user_lookup_name(full) >= 0) { user_write("mkdir: that already exists\n"); return 1; }
        /* the parent must exist: strip the last component and look it up */
        int n = 0;
        while (full[n]) { n++; }
        int p = n - 1;                   /* the trailing '/' */
        while (p > 0 && full[p - 1] != '/') { p--; }
        if (p > 0) {
            char parent[48];
            for (int i = 0; i < p; i++) { parent[i] = full[i]; }
            parent[p] = 0;
            if (user_lookup_name(parent) < 0) { user_write("mkdir: the parent directory does not exist\n"); return 1; }
        }
        if (user_create_name(full) < 0) { user_write("mkdir: could not create it (name too long, or out of space)\n"); return 1; }
        user_write("mkdir: created\n");
        return 1;
    }
    if (shell_cmd_is(cmd, 'r','m','d','i','r',0)) {
        char a[SHELL_LINE_MAX];
        shell_word(arg, 0, a, SHELL_LINE_MAX);
        shell_resolve(cwd, a, full, 1);
        int id = full[0] ? user_lookup_name(full) : -1;
        if (a[0] == 0 || id < 0) { user_write("rmdir: no such directory\n"); return 1; }
        struct object_info oi;
        for (int i = 0; user_list_objects(i, &oi) == 1; i++) {
            if (oi.id != id && shell_has_prefix(oi.name, full)) {
                user_write("rmdir: the directory is not empty\n");
                return 1;
            }
        }
        int cn = 0;
        while (cwd[cn] && cwd[cn] == full[cn]) { cn++; }
        if (full[cn] == 0) { user_write("rmdir: you are inside that directory\n"); return 1; }
        user_write(user_delete_name(id) == 0 ? "rmdir: removed\n" : "rmdir: refused\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Phase 19: the standard utilities (ls cat cp mv rm grep edit ps tree), each a
 * separately loaded program from src/userland/util_*.c. The shell is the
 * user's agent: it holds CAP_USER_DATA (authority over the user's own
 * files) and CAP_CREATE_OBJECT, and for each command it hands the freshly
 * spawned, authority-less program exactly the capabilities that command
 * needs -- `cat notes.txt` gets read on notes.txt and nothing else -- then
 * sends the arguments and waits for it to end. Ask for something the shell
 * itself holds no authority over (say `cat payload.bin`) and the delegation
 * fails, so the utility runs, tries, and is refused by the kernel.
 * ---------------------------------------------------------------- */
#define MSG_UTIL_ARG     0x80
#define MSG_UTIL_ARG_END 0x81

/* Returns 1 if `cmd` named a utility (handled here), 0 if it is not one. */
__attribute__((section(".user_text")))
static int shell_util(const char *cmd, const char *arg, const char *cwd, const int *job_slots, int job_count) {
    int kind = shell_util_kind(cmd);
    if (kind == 0) {
        return 0;
    }
    int prog = user_lookup_name(cmd);
    if (prog < 0) {
        user_write("that utility is not installed on this disk\n");
        return 1;
    }

    /* Resolve the path arguments BEFORE spawning, so a bad command line costs nothing. */
    char w1[SHELL_LINE_MAX];
    char w2[SHELL_LINE_MAX];
    char r1[48];
    char r2[48];
    char args[128];
    int an = 0;
    args[0] = 0;
    shell_word(arg, 0, w1, SHELL_LINE_MAX);
    shell_word(arg, 1, w2, SHELL_LINE_MAX);
    r1[0] = 0;
    r2[0] = 0;
    if (kind == 1 || kind == 9) {                 /* ls, tree: a directory (default: the current one) */
        shell_resolve(cwd, w1, r1, 1);
        shell_cat(args, &an, r1, 128);
    } else if (kind == 2 || kind == 5 || kind == 7) {   /* cat, rm, edit: one file */
        shell_resolve(cwd, w1, r1, 0);
        shell_cat(args, &an, r1, 128);
    } else if (kind == 3 || kind == 4) {          /* cp, mv: two files */
        shell_resolve(cwd, w1, r1, 0);
        shell_resolve(cwd, w2, r2, 0);
        shell_cat(args, &an, r1, 128);
        args[an++] = ' ';                /* a literal must never be dereferenced from ring 3 */
        args[an] = 0;
        shell_cat(args, &an, r2, 128);
    } else if (kind == 6) {                       /* grep <text> <file> */
        shell_resolve(cwd, w2, r2, 0);
        shell_cat(args, &an, w1, 128);
        args[an++] = ' ';                /* a literal must never be dereferenced from ring 3 */
        args[an] = 0;
        shell_cat(args, &an, r2, 128);
    }

    int child = user_spawn_program(prog);
    if (child < 0) {
        user_write("could not start it (no free actor slot, or the loader pool is busy)\n");
        return 1;
    }

    /* Delegate exactly what this one command needs. A refusal here (the
     * shell holds no authority over that object) is not an error: the
     * utility simply won't be able to, and says so. */
    int id1 = r1[0] ? user_lookup_name(r1) : -1;
    if (kind == 1 || kind == 9) {
        user_grant(child, CAP_LIST_NAMES, 0);
    } else if (kind == 2) {
        if (id1 >= 0) { user_grant(child, CAP_READ_OBJECT, id1); }
    } else if (kind == 3) {
        if (id1 >= 0) { user_grant(child, CAP_READ_OBJECT, id1); }
        user_grant(child, CAP_CREATE_OBJECT, 0);
    } else if (kind == 4) {
        if (id1 >= 0) { user_grant(child, CAP_RENAME_OBJECT, id1); }
    } else if (kind == 5) {
        if (id1 >= 0) { user_grant(child, CAP_DELETE_OBJECT, id1); }
    } else if (kind == 6) {
        int id2 = r2[0] ? user_lookup_name(r2) : -1;
        if (id2 >= 0) { user_grant(child, CAP_READ_OBJECT, id2); }
    } else if (kind == 7) {
        user_grant(child, CAP_CONSOLE, 0);
        if (id1 >= 0) {
            user_grant(child, CAP_READ_OBJECT, id1);
            user_grant(child, CAP_WRITE_OBJECT, id1);
        } else {
            user_grant(child, CAP_CREATE_OBJECT, 0);
        }
        /* the editor's clipboard is an ordinary user object; make sure it exists and share it */
        int clip = user_lookup_name(".clipboard");
        if (clip < 0) { clip = user_create_name(".clipboard"); }
        if (clip >= 0) {
            user_grant(child, CAP_READ_OBJECT, clip);
            user_grant(child, CAP_WRITE_OBJECT, clip);
        }
    } else if (kind == 8) {
        for (int i = 0; i < job_count; i++) {
            user_grant(child, CAP_INTROSPECT, job_slots[i]);
        }
    }

    /* The command line, 8 bytes per message (a message carries one word). */
    int len = 0;
    while (args[len] && len < 112) { len++; }
    for (int i = 0; i < len; i += 8) {
        uint64_t word = 0;
        for (int j = 0; j < 8 && i + j < len; j++) {
            word |= (uint64_t)(uint8_t)args[i + j] << (8 * j);
        }
        user_send(child, MSG_UTIL_ARG, word);
    }
    user_send(child, MSG_UTIL_ARG_END, 0);

    /* Wait for it to finish: introspection authority over one's own child
     * is what tells us it is gone. */
    struct actor_info ai;
    for (;;) {
        if (user_actor_info(child, &ai) != 0 || ai.state == 0) {
            break;
        }
        user_sleep(2);
    }
    return 1;
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
    char hist[SHELL_HIST][SHELL_LINE_MAX];
    int hist_n = 0;
    char cwd[48];   /* the current directory: "" = root, else ends in '/' */
    cwd[0] = 0;

    for (;;) {
        user_write("\x1b[36mvajra");
        if (cwd[0]) {
            user_write(":");
            user_write(cwd);
        }
        user_write("> \x1b[0m");
        shell_read_line(line, hist, &hist_n);
        shell_split(line, cmd, arg);

        if (cmd[0] == 0) {
            continue;
        } else if (shell_cmd_is(cmd, 'h','e','l','p',0,0)) {
            user_write("Commands: help run <name> echo <text> date clear\n");
            user_write("Files:    ls cat cp mv rm grep edit <name>   ps tree\n");
            user_write("Folders:  cd pwd mkdir rmdir  (paths: a/b, /abs, .., .)\n");
            user_write("          count pipe jobs stop <slot> kill <slot> exit\n");
            user_write("          pkg list | pkg install <name>\n");
        } else if (shell_cmd_is(cmd, 'p','k','g',0,0,0)) {
            shell_pkg(arg);
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
        } else if (shell_cmd_is(cmd, 'r','u','n',0,0,0)) {
            char runname[48];
            char runarg[SHELL_LINE_MAX];
            shell_word(arg, 0, runarg, SHELL_LINE_MAX);
            shell_resolve(cwd, runarg, runname, 0);
            int id = user_lookup_name(runname);
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
        } else if (shell_dircmd(cmd, arg, cwd)) {
            /* handled: cd pwd mkdir rmdir */
        } else if (!shell_util(cmd, arg, cwd, job_slots, job_count)) {
            user_write("unknown command (try 'help')\n");
        }
    }
}

/* ------------------------------------------------------------------
 * Phase 20: the package installer. The ONLY actor holding
 * CAP_INSTALL_PACKAGE. An install request is never "copy and trust":
 * stage the package as an ordinary UNTRUSTED object, have a sandboxed
 * inspector (an actor with read access to that one object and nothing
 * else) examine the bytes, and only then ask the kernel to promote or
 * permanently reject it -- and the kernel will only carry out a verdict
 * on an object this installer staged. Its output goes to the system
 * log; the shell shows the outcome.
 * ---------------------------------------------------------------- */
__attribute__((section(".user_text")))
static void actor_pkg_inspector(void) {
    struct message task;
    user_receive(&task); /* {type=TASK_INSPECT, data=object id} */
    int id = (int)task.data;
    char buf[2048];
    int n = user_object_read(id, buf, sizeof(buf));
    int bad = (n < 0); /* couldn't read it at all: not vouched for */
    for (int i = 0; i + 7 < n && !bad; i++) {
        if (buf[i] == 'B' && buf[i + 1] == 'A' && buf[i + 2] == 'D' && buf[i + 3] == 'S' &&
            buf[i + 4] == 'T' && buf[i + 5] == 'U' && buf[i + 6] == 'F' && buf[i + 7] == 'F') {
            bad = 1;
        }
    }
    user_send((int)task.sender, bad ? MSG_CHECK_FAIL : MSG_CHECK_PASS, (uint64_t)id);
    user_exit();
}

__attribute__((section(".user_text")))
static int pkg_install_one(int index, int requester) {
    struct pkg_info info;
    if (user_pkg_list(index, &info) != 0) {
        return PKG_ERR_BAD_INDEX;
    }
    int id = user_pkg_stage(index);
    if (id == -2) {
        return PKG_ERR_PRESENT;
    }
    if (id < 0) {
        return PKG_ERR_NO_SPACE;
    }
    user_write("[Installer] staged '");
    user_write(info.name);
    user_write("' as object ");
    user_write_dec64((uint64_t)id);
    user_write(" (UNTRUSTED -- the loader will not touch it yet)\n");

    int w = user_spawn(actor_pkg_inspector);
    int pass = 0;
    if (w >= 0) {
        user_grant(w, CAP_READ_OBJECT, id); /* delegated: this one object, nothing else */
        user_send(w, TASK_INSPECT, (uint64_t)id);
        struct message v;
        do {
            user_receive(&v);
        } while (v.type != MSG_CHECK_PASS && v.type != MSG_CHECK_FAIL);
        pass = (v.type == MSG_CHECK_PASS);
    }
    user_pkg_verdict(id, pass);
    if (w < 0) {
        user_write("[Installer] no sandbox slot for the inspector -- rejected '");
        user_write(info.name);
        user_write("'\n");
        return PKG_ERR_NO_INSPECTOR;
    }
    if (!pass) {
        user_write("[Installer] the inspector found the hostile marker in '");
        user_write(info.name);
        user_write("' -- REJECTED for good\n");
        return PKG_ERR_REJECTED;
    }
    user_write("[Installer] inspector says '");
    user_write(info.name);
    user_write("' is clean -- promoted to TRUSTED, installed\n");
    user_grant(requester, CAP_READ_OBJECT, id); /* let the requester run what it asked for */
    return id;
}

__attribute__((section(".user_text")))
static void actor_installer(void) {
    /* Self-check at every boot: the install authority is NARROW. Holding
     * CAP_INSTALL_PACKAGE must not let this actor promote an object it did
     * not stage -- here, the suspicious demo object. */
    int rc = user_pkg_verdict(SUSPICIOUS_OBJECT_ID, 1);
    user_write(rc < 0
        ? "[Installer] self-check: refused to promote an object I did not stage -- the install authority is narrow, OK\n"
        : "[Installer] self-check FAILED: promoted an object I did not stage!\n");
    for (;;) {
        struct message m;
        user_receive(&m);
        if (m.type != MSG_PKG_INSTALL) {
            continue;
        }
        int requester = (int)m.sender;
        int result = pkg_install_one((int)m.data, requester);
        user_send(requester, MSG_PKG_DONE, (uint64_t)(int64_t)result);
    }
}

/* ------------------------------------------------------------------
 * Security Lab (front-end-per-feature rule): an interactive app where
 * a person attacks Vajra ON PURPOSE and watches the hardening from
 * Phases 23-27 hold -- "guarantees are proven by breaking them"
 * (PHILOSOPHY.md section 3.7) made something you can do with the
 * keyboard instead of something you read about in a boot log.
 *
 * The lab itself is an ordinary ring-3 actor with almost no
 * authority (see kernel_main's grants). Attacks that end in a CPU
 * fault run in a disposable hostile child, since the fault would
 * otherwise kill the lab itself; attacks the kernel answers with a
 * refusal (-1) run in the lab directly.
 * ---------------------------------------------------------------- */
#define LAB_KERNEL_TARGET 0x30000 /* inside the kernel image (loaded at 0x20000) */

__attribute__((section(".user_text")))
static int user_fault_count(void) {
    return (int)hal_syscall(SYS_FAULT_COUNT, 0, 0, 0);
}

/* SYS_RTC_READ WRITES into whatever pointer it's given -- the textbook
 * confused-deputy target, which is why it's the one used here. */
__attribute__((section(".user_text")))
static int user_rtc_read_at(uint64_t address) {
    return (int)hal_syscall(SYS_RTC_READ, address, 0, 0);
}

/* Attack 1: write straight into the kernel's own memory. */
__attribute__((section(".user_text")))
static void actor_hostile_kernel_write(void) {
    volatile uint64_t *kernel_word = (volatile uint64_t *)LAB_KERNEL_TARGET;
    *kernel_word = 0x4841434B454421ULL;
    user_exit(); /* only reachable if the write was NOT stopped */
}

/* Attack b (NX): machine code placed in DATA memory must not run. Each writes
 * a lone `ret` (0xC3) into memory it owns and calls it. On a stack or heap
 * page that is executable, the call returns and the actor exits quietly; with
 * no-execute the fetch faults. A third child only proves the heap is really
 * writable, so the fault above can't be blamed on a broken mapping. */
__attribute__((section(".user_text")))
static void actor_hostile_exec_stack(void) {
    volatile uint8_t code[16];
    code[0] = 0xC3;
    void (*f)(void) = (void (*)(void))(uint64_t)code;
    f();
    user_exit(); /* only reachable if the stack was executable */
}

__attribute__((section(".user_text")))
static void actor_hostile_exec_heap(void) {
    uint64_t h = hal_syscall(SYS_HEAP_GROW, 1, 0, 0);
    if (h == (uint64_t)-1) {
        user_exit();
    }
    volatile uint8_t *p = (volatile uint8_t *)h;
    p[0] = 0xC3;
    void (*f)(void) = (void (*)(void))h;
    f();
    user_exit(); /* only reachable if the heap was executable */
}

__attribute__((section(".user_text")))
static void actor_heap_control(void) {
    uint64_t h = hal_syscall(SYS_HEAP_GROW, 2, 0, 0);
    if (h == (uint64_t)-1) {
        user_exit();
    }
    volatile uint32_t *p = (volatile uint32_t *)h;
    for (uint32_t i = 0; i < 2048; i++) {
        p[i] = i * 7u + 1u; /* touches both pages */
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 2048; i++) {
        sum += p[i];
    }
    (void)sum;
    user_exit();
}

/* Attack 2: modify this actor's own executable code (W^X). */
__attribute__((section(".user_text")))
static void actor_hostile_code_write(void) {
    volatile uint8_t *my_code = (volatile uint8_t *)actor_hostile_code_write;
    *my_code = 0x90;
    user_exit(); /* only reachable if the write was NOT stopped */
}

/* Attack 9's accomplice: an actor that takes over a dead object's id.
 * The lab hands it CAP_CREATE_OBJECT after spawning it (delegation --
 * the lab holds it too), it creates "lab_squat" (which lands on the id
 * the lab just freed), waits for the lab's release message, cleans up. */
__attribute__((section(".user_text")))
static void actor_squatter(void) {
    int id = -1;
    for (int i = 0; i < 300 && id < 0; i++) {
        id = user_create_name("lab_squat");
        if (id < 0) {
            user_sleep(1);
        }
    }
    struct message m;
    user_receive(&m);
    if (id >= 0) {
        user_delete_name(id);
    }
    user_exit();
}

/* ------------------------------------------------------------------
 * Phase 31: the adversary campaign (Security Lab key 'a'). Unlike attacks
 * 1-9, which each demonstrate ONE guarantee, this is a program that is
 * really trying to get out -- and several of its pieces run with genuine,
 * delegated authority (CAP_SPAWN, CAP_CREATE_OBJECT), which is the more
 * interesting claim: not "a hostile program is caught at the door" but
 * "a hostile program already INSIDE, holding real capabilities, still
 * cannot exceed them". Every line is one attempt and one verdict; the
 * kernel's own answer (refusal code, fault counter, the actor's own
 * count) decides it, never the attacker's word.
 * ---------------------------------------------------------------- */
#define MSG_ADV_GO     0x61
#define MSG_ADV_RESULT 0x62

/* Reads the page just above its own stack: nothing there is mapped in
 * this actor's address space (every other actor's memory included). */
__attribute__((section(".user_text")))
static void actor_adv_neighbor_read(void) {
    volatile uint64_t marker = 0;
    uint64_t here = (uint64_t)&marker & ~0xFFFULL;
    volatile uint64_t *neighbor = (volatile uint64_t *)(here + 0x1000);
    marker = *neighbor; /* faults here: no mapping, so no way to read anything of anyone's */
    user_exit();
}

__attribute__((section(".user_text")))
static void actor_adv_idle(void) {
    user_exit();
}

/* Fork bomb WITH genuine CAP_SPAWN (delegated by the lab): spawns until
 * the kernel says no, reports how many it got. */
__attribute__((section(".user_text")))
static void actor_adv_forkbomb(void) {
    struct message go;
    user_receive(&go); /* wait until the lab has delegated CAP_SPAWN */
    int made = 0;
    for (int i = 0; i < 50; i++) {
        if (user_spawn(actor_adv_idle) >= 0) {
            made++;
        } else {
            break;
        }
    }
    user_send(LAB_SLOT, MSG_ADV_RESULT, (uint64_t)made);
    user_exit();
}

/* Storage flood WITH genuine CAP_CREATE_OBJECT (delegated): creates
 * objects until refused, reports how many, then cleans up after itself. */
__attribute__((section(".user_text")))
static void actor_adv_flood(void) {
    struct message go;
    user_receive(&go);
    int ids[20];
    int made = 0;
    char name[5];
    name[0] = 'f'; name[1] = 'l'; name[2] = 'd'; name[4] = 0;
    for (int i = 0; i < 20; i++) {
        name[3] = (char)('a' + i);
        int id = user_create_name(name);
        if (id < 0) {
            break;
        }
        ids[made++] = id;
    }
    for (int i = 0; i < made; i++) {
        user_delete_name(ids[i]);
    }
    user_send(LAB_SLOT, MSG_ADV_RESULT, (uint64_t)made);
    user_exit();
}

__attribute__((section(".user_text")))
static void lab_menu(void) {
    user_write("\x1b[2J\x1b[1;1H");
    user_write("\x1b[37mSecurity Lab\x1b[0m -- attack Vajra. Press a key:\n\n");
    user_write("  1  Write into kernel memory            (Phase 23)\n");
    user_write("  2  Overwrite my own program code       (Phase 27, W^X)\n");
    user_write("  3  Make the kernel write for me:\n");
    user_write("     pass it a pointer into the kernel   (Phase 24)\n");
    user_write("  4  Run an UNtrusted program            (Phase 27)\n");
    user_write("  5  Run a TRUSTED program (control)     (Phase 27)\n");
    user_write("  6  Kill the shell with no permission   (capabilities)\n");
    user_write("  7  Message the shell, no capability    (capabilities)\n");
    user_write("  8  Run a program over and over: the loader\n");
    user_write("     has 2 slots, so a leak would show up    (Phase 26)\n");
    user_write("  9  Reuse a dead object's id: does my old\n");
    user_write("     capability now open someone else's?     (Phase 25)\n");
    user_write("  b  Run machine code from my own stack and\n");
    user_write("     heap (no-execute)                       (hardening)\n");
    user_write("  a  The adversary: a program that really\n");
    user_write("     tries everything at once, some of it\n");
    user_write("     with real capabilities                  (Phase 31)\n");
    user_write("  m  redraw this menu\n\n");
}

__attribute__((section(".user_text")))
static void lab_verdict(int held) {
    if (held) {
        user_write("  \x1b[32mHELD\x1b[0m -- ");
    } else {
        user_write("  \x1b[31mBREACH\x1b[0m -- ");
    }
}

__attribute__((section(".user_text")))
static void lab_footer(void) {
    user_write("  faults contained so far: ");
    user_write_dec64((uint64_t)user_fault_count());
    user_write("  (kernel + every other actor still running)\n\n");
}

/* Launches a hostile child and decides from the kernel's own fault
 * counter (not the child's word) whether the CPU stopped it. */
__attribute__((section(".user_text")))
static void lab_run_hostile(void (*entry)(void)) {
    int before = user_fault_count();
    int slot = user_spawn(entry);
    if (slot < 0) {
        user_write("  could not launch the attacker (spawn quota or no free slot)\n\n");
        return;
    }
    user_write("  attacker launched as actor ");
    user_write_dec64((uint64_t)slot);
    user_write("...\n");
    int stopped = 0;
    for (int i = 0; i < 3000 && !stopped; i++) {
        if (user_fault_count() > before) {
            stopped = 1;
        } else {
            user_yield();
        }
    }
    if (stopped) {
        lab_verdict(1);
        user_write("the CPU faulted the attacker; the kernel killed just that actor.\n");
    } else {
        lab_verdict(0);
        user_write("the attacker's write went through with no fault!\n");
    }
    lab_footer();
}

/* One attempt, one verdict. `refused` is what the kernel answered. */
__attribute__((section(".user_text")))
static void lab_attempt(const char *what, int refused, int *breaches) {
    user_write("  ");
    user_write(what);
    if (refused) {
        user_write("  \x1b[32mHELD\x1b[0m\n");
    } else {
        user_write("  \x1b[31mBREACH\x1b[0m\n");
        (*breaches)++;
    }
}

/* A counted attempt: `got` successes out of `tried`, `allowed` is the most
 * that legitimately holding the granted authority permits. */
__attribute__((section(".user_text")))
static void lab_counted(const char *what, int got, int allowed, int *breaches) {
    user_write("  ");
    user_write(what);
    user_write(" ");
    user_write_dec64((uint64_t)got);
    if (got <= allowed) {
        user_write("  \x1b[32mHELD\x1b[0m (limit ");
    } else {
        user_write("  \x1b[31mBREACH\x1b[0m (limit ");
        (*breaches)++;
    }
    user_write_dec64((uint64_t)allowed);
    user_write(")\n");
}

__attribute__((section(".user_text")))
static void lab_adversary(void) {
    int breaches = 0;
    int attempts = 0;
    char probe[8];
    user_write("\x1b[33m[a] The adversary: a hostile program, trying everything\x1b[0m\n");

    /* 1. memory: reach another actor's memory */
    {
        int before = user_fault_count();
        int slot = user_spawn(actor_adv_neighbor_read);
        int stopped = 0;
        for (int i = 0; slot >= 0 && i < 3000 && !stopped; i++) {
            if (user_fault_count() > before) { stopped = 1; } else { user_yield(); }
        }
        lab_attempt("read the page next to my own stack (no map)   ", stopped, &breaches);
        attempts++;
    }
    /* 2. forge or escalate capabilities */
    lab_attempt("grant MYSELF terminate-rights over the shell  ",
                user_grant(LAB_SLOT, CAP_TERMINATE, SHELL_SLOT) != 0, &breaches);
    lab_attempt("grant the shell a capability I do not hold    ",
                user_grant(SHELL_SLOT, CAP_PROMOTE_OBJECT, 0) != 0, &breaches);
    lab_attempt("grant a made-up capability number (99)        ",
                user_grant(LAB_SLOT, 99, 0) != 0, &breaches);
    lab_attempt("grant with a target far out of range          ",
                user_grant(LAB_SLOT, CAP_SEND, 9999) != 0, &breaches);
    lab_attempt("promote an untrusted program to trusted       ",
                user_object_promote(SUSPICIOUS_OBJECT_ID) != 0, &breaches);
    lab_attempt("install a package with no CAP_INSTALL_PACKAGE ",
                user_pkg_stage(0) < 0, &breaches);
    lab_attempt("promote an object via the install syscall     ",
                user_pkg_verdict(SUSPICIOUS_OBJECT_ID, 1) < 0, &breaches);
    attempts += 7;
    /* 3. sweeps: try every actor and every object */
    {
        int killed = 0, messaged = 0, read = 0, wrote = 0;
        for (int t = 0; t < MAX_ACTORS; t++) {
            if (t == LAB_SLOT) { continue; }
            if (user_terminate(t) == 0) { killed++; }
            if (user_send(t, MSG_PLEASE_STOP, 0) == 0) { messaged++; }
        }
        for (int o = 0; o < 64; o++) { /* every slot in the object store */
            if (user_object_read(o, probe, 8) >= 0) { read++; }
            if (user_object_write(o, "x", 1) >= 0) { wrote++; }
        }
        lab_counted("terminate every other actor, one by one:     ", killed, 0, &breaches);
        lab_counted("message every other actor, one by one:       ", messaged, 0, &breaches);
        lab_counted("write to every stored object:                ", wrote, 0, &breaches);
        lab_counted("read every stored object (I hold 2 grants):  ", read, 2, &breaches);
        attempts += 4;
    }
    /* 4. with REAL authority: quotas bound it */
    {
        int bomb = user_spawn(actor_adv_forkbomb);
        int made = -1;
        if (bomb >= 0) {
            user_grant(bomb, CAP_SPAWN, 0);
            user_send(bomb, MSG_ADV_GO, 0);
            struct message m;
            user_receive(&m);
            made = (int)m.data;
        }
        lab_counted("fork bomb, holding real CAP_SPAWN, spawned:  ", made < 0 ? 0 : made, 2, &breaches);
        attempts++;
        int flood = user_spawn(actor_adv_flood);
        int created = -1;
        if (flood >= 0) {
            user_grant(flood, CAP_CREATE_OBJECT, 0);
            user_send(flood, MSG_ADV_GO, 0);
            struct message m;
            user_receive(&m);
            created = (int)m.data;
        }
        lab_counted("storage flood, holding CAP_CREATE_OBJECT, made:", created < 0 ? 0 : created, 2, &breaches);
        attempts++;
    }
    user_write("  ");
    user_write_dec64((uint64_t)attempts);
    user_write(" attempts, ");
    user_write_dec64((uint64_t)breaches);
    user_write(" breaches. Nothing reached outside its own grants.\n");
    lab_footer();
}

__attribute__((section(".user_text")))
static void actor_lab(void) {
    lab_menu();
    for (;;) {
        int c = user_key_read();
        if (c < 0) {
            user_sleep(1);
            continue;
        }
        if (c == 'm' || c == 'M') {
            lab_menu();
        } else if (c == '1') {
            user_write("\x1b[33m[1] writing to kernel address 0x30000 from ring 3\x1b[0m\n");
            lab_run_hostile(actor_hostile_kernel_write);
        } else if (c == '2') {
            user_write("\x1b[33m[2] overwriting a byte of my own executable code\x1b[0m\n");
            lab_run_hostile(actor_hostile_code_write);
        } else if (c == '3') {
            user_write("\x1b[33m[3] SYS_RTC_READ with its output pointer aimed at the kernel\x1b[0m\n");
            int rc = user_rtc_read_at(LAB_KERNEL_TARGET);
            lab_verdict(rc < 0);
            user_write(rc < 0
                ? "the kernel checked the pointer and refused to write there.\n"
                : "the kernel wrote into its own memory on my behalf!\n");
            lab_footer();
        } else if (c == '4') {
            user_write("\x1b[33m[4] running suspicious.bin (never vetted as trusted)\x1b[0m\n");
            int slot = user_spawn_program(SUSPICIOUS_OBJECT_ID);
            lab_verdict(slot < 0);
            user_write(slot < 0
                ? "the loader refuses anything that isn't OBJ_TRUSTED.\n"
                : "an untrusted program was loaded and is running!\n");
            lab_footer();
        } else if (c == '5') {
            user_write("\x1b[33m[5] running hello.bin (vetted, OBJ_TRUSTED) -- should work\x1b[0m\n");
            int slot = user_spawn_program(HELLO_PROGRAM_OBJECT_ID);
            lab_verdict(slot >= 0);
            user_write(slot >= 0
                ? "it loaded and ran (see the Log app): the gate isn't a blanket no.\n"
                : "the trusted program was refused (quota or loader problem).\n");
            lab_footer();
        } else if (c == '6') {
            user_write("\x1b[33m[6] terminating the shell with no CAP_TERMINATE\x1b[0m\n");
            int rc = user_terminate(SHELL_SLOT);
            lab_verdict(rc != 0);
            user_write(rc != 0
                ? "no capability, no kill. The shell is untouched.\n"
                : "the shell was killed by an actor with no authority over it!\n");
            lab_footer();
        } else if (c == '7') {
            user_write("\x1b[33m[7] messaging the shell with no CAP_SEND to it\x1b[0m\n");
            int rc = user_send(SHELL_SLOT, MSG_PLEASE_STOP, 0);
            lab_verdict(rc != 0);
            user_write(rc != 0
                ? "no capability, no message. Authority is never ambient.\n"
                : "the message got through with no capability!\n");
            lab_footer();
        } else if (c == '8') {
            user_write("\x1b[33m[8] running hello.bin six times in a row (the loader has a pool of 2)\x1b[0m\n");
            int ok = 0;
            for (int run = 0; run < 6; run++) {
                int slot = -1;
                for (int t = 0; t < 100 && slot < 0; t++) {
                    slot = user_spawn_program(HELLO_PROGRAM_OBJECT_ID);
                    if (slot < 0) {
                        user_sleep(2);
                    }
                }
                if (slot < 0) {
                    break;
                }
                ok++;
                user_sleep(15); /* let it run and exit; the kernel reaps it and frees its pool entry */
            }
            lab_verdict(ok == 6);
            user_write("loaded and ran ");
            user_write_dec64((uint64_t)ok);
            user_write(" of 6");
            user_write(ok == 6
                ? " -- every exit gave its loader slot back.\n"
                : " -- the loader ran out: something leaked!\n");
            lab_footer();
        } else if (c == '9') {
            user_write("\x1b[33m[9] keep a capability to an object, let the object die, let a stranger take its id\x1b[0m\n");
            char probe[8];
            int victim = user_create_name("lab_victim");
            if (victim < 0) {
                user_write("  could not create the victim object (name taken, or no free object slot)\n\n");
                continue;
            }
            user_object_write(victim, "hi!!", 4);
            int before = user_object_read(victim, probe, 8);
            user_write("  control: with a live capability I read my own object: ");
            user_write_dec64((uint64_t)(before < 0 ? 0 : before));
            user_write(" bytes\n");
            user_delete_name(victim); /* I keep the (now stale) capabilities on purpose */
            int squat = user_spawn(actor_squatter);
            if (squat < 0) {
                user_write("  could not launch the accomplice (spawn quota or no free slot)\n\n");
                continue;
            }
            user_grant(squat, CAP_CREATE_OBJECT, 0);
            int taken = -1;
            for (int t = 0; t < 300 && taken < 0; t++) {
                taken = user_lookup_name("lab_squat");
                if (taken < 0) {
                    user_sleep(1);
                }
            }
            if (taken != victim) {
                user_write("  inconclusive: the stranger's object did not land on my old id -- try again\n");
            } else {
                user_write("  a stranger's object now lives at my old id (");
                user_write_dec64((uint64_t)taken);
                user_write(")\n");
                int rc = user_object_read(victim, probe, 8);
                lab_verdict(rc < 0);
                user_write(rc < 0
                    ? "my old capability named a dead identity; it opens nothing new.\n"
                    : "my stale capability read a stranger's object!\n");
            }
            user_send(squat, 0x60, 0); /* release the accomplice: it deletes its object and exits */
            lab_footer();
        } else if (c == 'a' || c == 'A') {
            lab_adversary();
        } else if (c == 'b' || c == 'B') {
            user_write("\x1b[33m[b] executing code from data pages\x1b[0m\n");
            /* control: the heap really works (no fault expected) */
            int before = user_fault_count();
            int cs = user_spawn(actor_heap_control);
            user_sleep(50);
            int control_ok = (cs >= 0 && user_fault_count() == before);
            user_write(control_ok ? "  control: a 2-page heap was mapped, written and read back  \x1b[32mOK\x1b[0m\n"
                                   : "  control FAILED: the heap did not work, so the tests below prove nothing\n");
            user_write("  run a `ret` placed on my STACK:\n");
            lab_run_hostile(actor_hostile_exec_stack);
            user_write("  run a `ret` placed on my HEAP:\n");
            lab_run_hostile(actor_hostile_exec_heap);
        }
    }
}

/* ------------------------------------------------------------------
 * Cores app (front-end-per-feature rule, Phase 10 remainder): a live
 * view of every CPU core -- which actor it is executing this instant,
 * how many actors it has switched into, how often it found nothing to
 * run -- plus a parallelism test you can run yourself.
 *
 * Press b: one CPU-bound "burner" actor runs ALONE for a fixed window
 * of TSC cycles and counts loop iterations (its solo throughput); then
 * K burners run at once for the same window, K = one per online core
 * (at most BURN_MAX, bounded by free actor slots). If the kernel really
 * runs actors on K cores, each keeps close to solo throughput and the
 * speedup is near K; on one core (or a scheduler that only time-slices)
 * the group shares one core's worth of work and it is near 1.0x. It
 * also reports how many distinct cores the burners touched, read with
 * CPUID from ring 3. Press s: the kill test.
 * ---------------------------------------------------------------- */
#define MSG_BURN_RESULT   0x50
#define BURN_WINDOW_TSC   200000000ULL /* cycles each burner spins for */
#define BURN_MAX          6            /* burners in the parallel run: bounded by free actor slots */
#define BURN_ITER_MASK    0xFFFFFFFFFFULL /* low 40 bits: loop count; bits 40-55: core mask */

__attribute__((section(".user_text")))
static int user_core_info(int count, struct core_info *out) {
    return (int)hal_syscall(SYS_CORE_INFO, (uint64_t)count, (uint64_t)out, 0);
}

__attribute__((section(".user_text")))
static int user_kernel_stats(struct kernel_stats *out) {
    return (int)hal_syscall(SYS_KERNEL_STATS, (uint64_t)out, 0, 0);
}

__attribute__((section(".user_text")))
static uint64_t user_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* The core executing this instruction, from CPUID leaf 1 (initial
 * APIC ID) -- unprivileged, so a burner can sample it from ring 3. */
__attribute__((section(".user_text")))
static int user_cpu_id(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (int)((ebx >> 24) & 0xF);
}

/* Spins for BURN_WINDOW_TSC cycles counting iterations, noting every
 * core it lands on, then reports (iterations | core mask << 40) to its
 * spawner. The same body serves the solo run and every parallel burner. */
__attribute__((section(".user_text")))
static void actor_burner(void) {
    uint64_t start = user_rdtsc();
    uint64_t iterations = 0;
    uint64_t core_mask = 0;
    volatile uint64_t sink = 0;
    while (user_rdtsc() - start < BURN_WINDOW_TSC) {
        for (int i = 0; i < 1000; i++) {
            sink += (uint64_t)i;
        }
        iterations++;
        core_mask |= (1ULL << user_cpu_id());
    }
    (void)sink;
    /* The spawner is the only actor this one holds a CAP_SEND to (the
     * kernel granted it at spawn) -- CORES_SLOT is the Cores app itself,
     * see kernel_main's spawn order. */
    user_send(CORES_SLOT, MSG_BURN_RESULT, (iterations & BURN_ITER_MASK) | (core_mask << 40));
    user_exit();
}

/* Never yields, never exits: only a kill ends it. That is the point --
 * terminating an actor that is RUNNING, possibly on another core, is
 * the one cross-core operation that can't just take effect (freeing
 * the stack/address space under a core that is executing on them would
 * crash it), so the kernel defers it to the target's next kernel entry
 * (actor_terminate()'s kill_pending). */
__attribute__((section(".user_text")))
static void actor_spinner(void) {
    volatile uint64_t sink = 0;
    for (;;) {
        sink++;
    }
}

__attribute__((section(".user_text")))
static void cores_write_name(int slot) {
    if (slot < 0)        { user_write("idle          "); }
    else if (slot == 0)  { user_write("Actor 1       "); }
    else if (slot == 1)  { user_write("Actor 2       "); }
    else if (slot == 2)  { user_write("Actor 3       "); }
    else if (slot == 3)  { user_write("Greedy        "); }
    else if (slot == 4)  { user_write("Receiver      "); }
    else if (slot == 5)  { user_write("Sender        "); }
    else if (slot == 6)  { user_write("Intruder      "); }
    else if (slot == 7)  { user_write("Coordinator   "); }
    else if (slot == 8)  { user_write("Downloader    "); }
    else if (slot == 9)  { user_write("Scanner       "); }
    else if (slot == 10) { user_write("Reader        "); }
    else if (slot == 11) { user_write("Network peer  "); }
    else if (slot == 12) { user_write("Program loader"); }
    else if (slot == 13) { user_write("Namer         "); }
    else if (slot == 14) { user_write("Shell         "); }
    else if (slot == 15) { user_write("Security Lab  "); }
    else if (slot == 16) { user_write("Cores app     "); }
    else if (slot == 17) { user_write("Installer     "); }
    else                 { user_write("(spawned)     "); }
}

/* Right-aligned decimal in a fixed-width field, so a redraw in place
 * always fully overwrites the previous frame. */
__attribute__((section(".user_text")))
static void cores_write_num(uint64_t v, int width) {
    uint64_t t = v;
    int digits = 1;
    while (t >= 10) { t /= 10; digits++; }
    for (int i = digits; i < width; i++) {
        user_write(" ");
    }
    user_write_dec64(v);
}

__attribute__((section(".user_text")))
static int cores_popcount(uint64_t mask) {
    int n = 0;
    while (mask) {
        n += (int)(mask & 1);
        mask >>= 1;
    }
    return n;
}

/* Everything the app remembers between frames. */
struct cores_state {
    int have_result;
    int burners;            /* how many ran in the parallel test */
    uint64_t solo;          /* best of two solo runs */
    uint64_t total;         /* sum over the parallel burners */
    uint64_t mask;          /* union of the cores they touched */
    int stress_launched;
    int stress_killed;
    /* the previous frame's kernel-lock counters, for rates */
    uint64_t prev_tsc, prev_hold, prev_wait, prev_acq, prev_ticks;
};

/* Draws the per-core table: one wide line per core while there are few,
 * two compact columns once there are many (16 cores must still leave
 * room for the test results below). Returns the first free screen row. */
__attribute__((section(".user_text")))
static int cores_draw_table(struct core_info *ci, int ncores) {
    int wide = (ncores <= 4);
    int rows = wide ? ncores : (ncores + 1) / 2;
    for (int r = 0; r < rows; r++) {
        for (int col = 0; col < (wide ? 1 : 2); col++) {
            int cpu = wide ? r : (col == 0 ? r : r + rows);
            if (cpu >= ncores) {
                user_write("                                       ");
                continue;
            }
            user_write(" core ");
            if (cpu < 10) { user_write(" "); }
            user_write_dec64((uint64_t)cpu);
            if (!ci[cpu].online) {
                user_write(wide ? "  \x1b[31moffline\x1b[0m                                                    "
                                : "  \x1b[31moffline\x1b[0m                    ");
                continue;
            }
            user_write(" \x1b[32m");
            cores_write_name(ci[cpu].running_slot);
            user_write("\x1b[0m");
            if (wide) {
                if (ci[cpu].running_slot >= 0) {
                    user_write(" slot ");
                    cores_write_num((uint64_t)ci[cpu].running_slot, 2);
                } else {
                    user_write(" slot --");
                }
                user_write("  switches");
                cores_write_num(ci[cpu].switches, 8);
                user_write("  idle");
                cores_write_num(ci[cpu].idle_ticks, 8);
            } else {
                user_write(" ");
                cores_write_num(ci[cpu].switches, 6);
            }
        }
        user_write("\n");
    }
    return 3 + rows;
}

__attribute__((section(".user_text")))
static void cores_draw(struct cores_state *st, int running_test) {
    user_write("\x1b[1;1H");
    user_write("\x1b[37mCores\x1b[0m -- every CPU core, live.  \x1b[33mb\x1b[0m parallelism test   \x1b[33ms\x1b[0m kill test\n\n");

    struct core_info ci[MAX_CPUS];
    int have_info = (user_core_info(MAX_CPUS, ci) == 0);
    int ncores = 0;
    if (have_info) {
        for (int i = 0; i < MAX_CPUS; i++) {
            if (ci[i].online) { ncores = i + 1; }
        }
    }
    if (ncores == 0) {
        ncores = 1;
    }
    int row = 3;
    if (have_info) {
        row = cores_draw_table(ci, ncores);
    }

    user_write("\x1b[");
    user_write_dec64((uint64_t)(row + 1));
    user_write(";1H");

    if (running_test) {
        user_write("  \x1b[33mtest running...\x1b[0m the burners are spinning; results appear here.                 \n");
        user_write("                                                                              \n");
        user_write("                                                                              \n");
        user_write("                                                                              \n");
        user_write("                                                                              \n");
    } else if (st->have_result) {
        user_write("  1 burner alone:  ");
        cores_write_num(st->solo, 9);
        user_write(" loops in the window                                \n");
        user_write("  ");
        user_write_dec64((uint64_t)st->burners);
        user_write(" burners at once:");
        cores_write_num(st->total, 9);
        user_write(" loops in total, on ");
        user_write_dec64((uint64_t)cores_popcount(st->mask));
        user_write(" core(s)          \n");
        uint64_t x100 = (st->solo > 0) ? (st->total * 100) / st->solo : 0;
        uint64_t ideal100 = (uint64_t)st->burners * 100;
        user_write("\n  speedup: \x1b[1;37m");
        user_write_dec64(x100 / 100);
        user_write(".");
        if (x100 % 100 < 10) { user_write("0"); }
        user_write_dec64(x100 % 100);
        user_write("x\x1b[0m of an ideal ");
        user_write_dec64((uint64_t)st->burners);
        user_write(".00x   ");
        /* One core can never exceed 1.00x. Parallel = clearly past that:
         * a quarter of each extra burner, which also absorbs host noise. */
        uint64_t threshold100 = 100 + 25 * (uint64_t)(st->burners - 1);
        if (x100 >= threshold100) {
            user_write("\x1b[32mgenuinely parallel\x1b[0m                    \n");
        } else {
            user_write("\x1b[31mnot parallel\x1b[0m                          \n");
        }
        user_write("  efficiency: ");
        user_write_dec64((x100 * 100) / (ideal100 ? ideal100 : 1));
        user_write("% of linear                                            \n");
    } else {
        user_write("  No test run yet.                                                              \n");
        user_write("                                                                              \n");
        user_write("                                                                              \n");
        user_write("                                                                              \n");
        user_write("                                                                              \n");
    }
    user_write("\x1b[");
    user_write_dec64((uint64_t)(row + 8));
    user_write(";1H  kill test (\x1b[33ms\x1b[0m): ");
    if (st->stress_launched > 0) {
        user_write_dec64((uint64_t)st->stress_launched);
        user_write(" launched, ");
        user_write_dec64((uint64_t)st->stress_killed);
        user_write(" killed -- all cores still up.                              \n");
    } else {
        user_write("launch never-yielding spinners and kill them mid-run.          \n");
    }

    /* The kernel lock, measured: share of time held, and the average share
     * of each core's time spent waiting for it, over the time since the
     * previous frame. This is the number that says whether the one big
     * lock is the bottleneck (Phase 28). */
    struct kernel_stats ks;
    if (user_kernel_stats(&ks) == 0) {
        uint64_t tsc = user_rdtsc();
        user_write("\x1b[");
        user_write_dec64((uint64_t)(row + 10));
        user_write(";1H  kernel lock: ");
        if (st->prev_tsc != 0 && tsc > st->prev_tsc) {
            uint64_t span = tsc - st->prev_tsc;
            uint64_t hold_pct = ((ks.lock_hold - st->prev_hold) * 100) / span;
            uint64_t wait_pct = ((ks.lock_wait - st->prev_wait) * 100) / (span * (uint64_t)ncores);
            user_write("held ");
            cores_write_num(hold_pct, 3);
            user_write("% of the time, waiting ");
            cores_write_num(wait_pct, 3);
            user_write("%, ");
            cores_write_num(ks.lock_acquisitions - st->prev_acq, 7);
            user_write(" takes/s  ");
        } else {
            user_write("measuring...                                       ");
        }
        user_write("\n  uptime: ");
        user_write_dec64(ks.ticks / 100);
        user_write(" s                       ");
        st->prev_tsc = tsc;
        st->prev_hold = ks.lock_hold;
        st->prev_wait = ks.lock_wait;
        st->prev_acq = ks.lock_acquisitions;
        st->prev_ticks = ks.ticks;
    }
}

/* Runs `count` burners at once and gathers their results. */
__attribute__((section(".user_text")))
static int cores_run_burners(int count, uint64_t *total, uint64_t *mask) {
    for (int i = 0; i < count; i++) {
        if (user_spawn(actor_burner) < 0) {
            for (int j = 0; j < i; j++) {
                struct message stale;
                user_receive(&stale); /* don't leave a started burner's result queued */
            }
            return -1;
        }
    }
    *total = 0;
    *mask = 0;
    for (int i = 0; i < count; i++) {
        struct message m;
        user_receive(&m);
        *total += m.data & BURN_ITER_MASK;
        *mask |= (m.data >> 40) & 0xFFFF;
    }
    return 0;
}

__attribute__((section(".user_text")))
static void actor_cores(void) {
    struct cores_state st;
    st.have_result = 0;
    st.burners = 0;
    st.solo = 0;
    st.total = 0;
    st.mask = 0;
    st.stress_launched = 0;
    st.stress_killed = 0;
    st.prev_tsc = 0;
    st.prev_hold = 0;
    st.prev_wait = 0;
    st.prev_acq = 0;
    st.prev_ticks = 0;

    user_write("\x1b[2J\x1b[1;1H");
    cores_draw(&st, 0);

    struct rtc_time now;
    user_rtc_read(&now);
    int last_second = now.seconds;
    for (;;) {
        int c = user_key_read();
        if (c == 'b' || c == 'B') {
            cores_draw(&st, 1);
            /* How many burners: one per online core, capped by the free
             * actor slots. */
            struct core_info ci[MAX_CPUS];
            int online = 0;
            if (user_core_info(MAX_CPUS, ci) == 0) {
                for (int i = 0; i < MAX_CPUS; i++) {
                    if (ci[i].online) { online++; }
                }
            }
            int k = online < 2 ? 2 : online;
            if (k > BURN_MAX) { k = BURN_MAX; }

            /* solo, then k at once, then solo again: the baseline is the
             * better solo run, so background load can only make the
             * reported speedup smaller, never inflate it. */
            uint64_t solo1 = 0, solo2 = 0, total = 0, mask = 0, ignore_mask = 0;
            if (cores_run_burners(1, &solo1, &ignore_mask) == 0 &&
                cores_run_burners(k, &total, &mask) == 0 &&
                cores_run_burners(1, &solo2, &ignore_mask) == 0) {
                st.solo = (solo1 > solo2) ? solo1 : solo2;
                st.total = total;
                st.mask = mask;
                st.burners = k;
                st.have_result = 1;
            }
            cores_draw(&st, 0);
        }
        if (c == 's' || c == 'S') {
            /* 16 rounds: launch a spinner, give it a few scheduling
             * rounds so it is very likely RUNNING (often on another
             * core), then terminate it. */
            for (int round = 0; round < 16; round++) {
                int slot = user_spawn(actor_spinner);
                if (slot < 0) {
                    continue;
                }
                st.stress_launched++;
                for (int y = 0; y < 3 + (round % 4); y++) {
                    user_yield();
                }
                if (user_terminate(slot) == 0) {
                    st.stress_killed++;
                }
                user_yield();
            }
            cores_draw(&st, 0);
        }
        /* Redraw once a second, not on a yield counter: every byte the
         * console writes is ALSO mirrored to the serial port, and a
         * fast redraw put half a megabyte of frames into a 30-second
         * boot log (and would blow past CI's step-summary size limit). */
        user_rtc_read(&now);
        if (now.seconds != last_second) {
            last_second = now.seconds;
            cores_draw(&st, 0);
        }
        user_sleep(1);
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

/* Phase 19: every standard utility's program image -- generated by tools/build-c.ps1 into
 * build/utils_blob.asm. Terminated by an all-zero entry. */
struct util_entry {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
};
extern struct util_entry util_table[];

void kernel_main(void) {
    hal_enable_nx(); /* stacks and heaps are mapped non-executable from here on */
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
    memory_init();
    hal_console_write("Memory manager online. Detected RAM: ");
    hal_console_write_dec64(memory_get_total_bytes() / (1024 * 1024));
    hal_console_write(" MB\n");

    /* Phase 10: wake every other core the firmware lists, one at a time
     * (each gets an allocator-provided stack, hence after memory_init()
     * -- see hal_smp_boot_aps()). They idle until scheduler_start(). */
    int aps = hal_smp_boot_aps();
    hal_console_write("SMP: ");
    hal_console_write_dec64((uint64_t)(aps + 1));
    hal_console_write(" core(s) online (the boot core plus ");
    hal_console_write_dec64((uint64_t)aps);
    hal_console_write(").\n");

    /* Roadmap Phase 12: the first piece of the fabric -- see
     * net_arp_demo()'s own comment. Needs memory_init() (virtqueues
     * are DMA memory, allocated via alloc_pages_contig()) but nothing
     * else below it. Also brings core/net.c online (net_init()) for
     * actor_network_peer below, whether or not a device turned out to
     * be present -- net_send_message()/net_poll_receive_message()
     * both fail cleanly if it isn't. */
    int have_net = net_arp_demo();

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

    /* begin/end (not set_window): the AP core prints to the Log window
     * concurrently, and an unsynchronized window switch here raced with it
     * -- see console.c. */
    hal_console_begin_window(CONSOLE_WIN_ABOUT);
    hal_console_write("Vajra OS\n\n");
    hal_console_write("A from-scratch x86-64 kernel built around actors,\n");
    hal_console_write("capabilities, and message passing -- no ambient\n");
    hal_console_write("authority, and nothing trusted by default.\n\n");
    hal_console_write("Memory detected: ");
    hal_console_write_dec64(memory_get_total_bytes() / (1024 * 1024));
    hal_console_write(" MB\n");
    hal_console_end_window();
    hal_console_begin_window(CONSOLE_WIN_SECURITY);
    hal_console_write("Security Lab\n\nStarting...\n");

    hal_console_end_window();
    hal_console_begin_window(CONSOLE_WIN_FABRIC);
    hal_console_write("Fabric -- actors talking across devices\n\n");
    if (have_net) {
        uint8_t fabric_mac[6];
        hal_net_get_mac(fabric_mac);
        hal_console_write("This device: ");
        write_mac(fabric_mac);
        hal_console_write("\nListening for a peer. Boot a second Vajra on the\n");
        hal_console_write("same virtual link (tools/run.ps1 -Net) and watch it\n");
        hal_console_write("find this one, ping it, and ask it to run a program.\n\n");
        hal_console_write("Keys (when this window is focused):\n");
        hal_console_write("  h  say HELLO      p  ping the peer\n");
        hal_console_write("  r  ask the peer to run hello.bin on ITS device\n\n");
    } else {
        hal_console_write("No network device on this machine.\n\n");
        hal_console_write("Boot with -device virtio-net-pci (tools/run.ps1 -Net)\n");
        hal_console_write("to bring the fabric up.\n\n");
    }
    hal_console_end_window();

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
    actor_spawn(actor_lab);              /* must land at LAB_SLOT */
    actor_spawn(actor_cores);            /* must land at CORES_SLOT */
    actor_spawn(actor_installer);        /* must land at INSTALLER_SLOT */

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

    /* Phase 19: seed the standard utilities (src/userland/util_*.c, built as
     * separate programs and listed in build/utils_blob.asm's util_table) as
     * SYSTEM objects, vouched for by the kernel through the same promotion
     * steps as hello.bin above. The shell gets read rights to each program
     * (that is what lets it load them) -- and nothing else over them. */
    int util_count = 0;
    for (int u = 0; util_table[u].name; u++) {
        int uid = storage_create_object(util_table[u].name);
        storage_write(uid, util_table[u].start, (uint32_t)(util_table[u].end - util_table[u].start));
        storage_promote(uid);
        storage_promote(uid);
        storage_promote(uid);
        actor_grant(SHELL_SLOT, CAP_READ_OBJECT, uid);
        util_count++;
    }
    hal_console_write("Loader: seeded ");
    hal_console_write_dec64((uint64_t)util_count);
    hal_console_write(" standard utilities (ls cat cp mv rm grep edit ps) as loadable programs.\n");

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
    actor_grant(SHELL_SLOT, CAP_USER_DATA, 0);      /* Phase 19: the user's own files, and only those */
    actor_grant(SHELL_SLOT, CAP_CREATE_OBJECT, 0);
    actor_set_create_quota(SHELL_SLOT, 200);
    actor_set_spawn_quota(SHELL_SLOT, 2000); /* every utility run is one spawn; was 6 */
    /* see actor.h's own comment -- a per-actor override,
                                              not a change to every other actor's quota */
    /* The Security Lab: keyboard (CAP_CONSOLE) so a person can drive it,
     * CAP_SPAWN for its disposable hostile children, and READ on exactly
     * the two program objects its trust-gate demo needs -- and
     * deliberately NOTHING else: no CAP_TERMINATE, no CAP_SEND to the
     * shell, because attacks 6 and 7 exist to show those are refused. */
    actor_grant(LAB_SLOT, CAP_CONSOLE, 0);
    actor_grant(LAB_SLOT, CAP_SPAWN, 0);
    actor_grant(LAB_SLOT, CAP_READ_OBJECT, SUSPICIOUS_OBJECT_ID);
    actor_grant(LAB_SLOT, CAP_READ_OBJECT, HELLO_PROGRAM_OBJECT_ID);
    actor_grant(LAB_SLOT, CAP_CREATE_OBJECT, 0); /* attack 9: create the object whose id gets reused */
    actor_set_create_quota(LAB_SLOT, 200);
    actor_set_spawn_quota(LAB_SLOT, 200);
    actor_set_window(LAB_SLOT, CONSOLE_WIN_SECURITY);

    /* The Cores app: keyboard + status (CAP_CONSOLE) and CAP_SPAWN for
     * the burner actors its parallelism test runs. It never touches
     * anything else. */
    actor_grant(CORES_SLOT, CAP_CONSOLE, 0);
    actor_grant(CORES_SLOT, CAP_SPAWN, 0);
    actor_set_spawn_quota(CORES_SLOT, 500); /* burners + kill-test spinners add up */
    actor_set_window(CORES_SLOT, CONSOLE_WIN_CORES);

    /* The package installer (Phase 20): the ONLY holder of
     * CAP_INSTALL_PACKAGE, CAP_SPAWN for its per-install sandboxed
     * inspectors, and a CAP_SEND to the shell to answer it. The shell in
     * turn may only ASK the installer (CAP_SEND to it) -- it gets no
     * install authority of its own. */
    actor_grant(INSTALLER_SLOT, CAP_INSTALL_PACKAGE, 0);
    actor_grant(INSTALLER_SLOT, CAP_SPAWN, 0);
    actor_grant(INSTALLER_SLOT, CAP_SEND, SHELL_SLOT);
    actor_set_spawn_quota(INSTALLER_SLOT, 100);
    actor_grant(SHELL_SLOT, CAP_SEND, INSTALLER_SLOT);
    actor_set_window(NETWORK_PEER_SLOT, CONSOLE_WIN_FABRIC); /* the Fabric app is this actor's pane */
    actor_grant(NETWORK_PEER_SLOT, CAP_CONSOLE, 0); /* the Fabric app's own keys (h/p/r); SYS_KEY_READ only serves the FOCUSED window's actor, and without
                                                       this grant every read is silently denied -- found by CI: the keys never arrived */
    actor_set_window(SHELL_SLOT, CONSOLE_WIN_SHELL); /* the shell's own pane -- see console.c's
                                                          own top comment for why this exists */

    hal_console_write("\nStarting preemptive scheduler with 18 ring-3 actors...\n\n");

    scheduler_start(); /* becomes the BSP's scheduler loop; the AP joins once this runs */
}
