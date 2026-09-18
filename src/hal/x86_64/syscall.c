#include "vajra/hal.h"
#include "vajra/actor.h"
#include "vajra/storage.h"
#include "vajra/net.h"
#include "vajra/loader.h"

/* ------------------------------------------------------------------
 * The kernel side of the syscall boundary. isr_stubs.asm's
 * syscall_common extracts the syscall number and up to 3 arguments
 * from the saved register state at the moment of `int 0x80` (RAX and
 * RDI/RSI/RDX) and calls this function; its return value goes back to
 * the ring-3 caller in RAX.
 *
 * This function itself runs at CPL 0 -- that is the entire point of
 * routing through a syscall gate. Calling ordinary kernel functions
 * like hal_console_write() or actor_yield()/actor_exit() from here is
 * completely normal: unlike the ring-3 caller two steps up the call
 * chain, this code doesn't need a syscall to reach the kernel -- it
 * already is the kernel.
 * ---------------------------------------------------------------- */

uint64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (num) {
        case SYS_WRITE:
            /* a1 is a pointer into the CALLING actor's own address
             * space (still active -- syscalls don't switch CR3), but
             * dereferencing it here is fine regardless of that page's
             * U/S bit: that bit only restricts CPL 3 accesses, and
             * this code is running at CPL 0. */
            hal_console_write((const char *)a1);
            return 0;

        case SYS_YIELD:
            actor_yield();
            return 0;

        case SYS_EXIT:
            actor_exit(); /* never returns */
            return 0;

        case SYS_SEND:
            /* a1 = dest slot, a2 = type, a3 = data. */
            return (uint64_t)actor_send((int)a1, a2, a3);

        case SYS_RECEIVE:
            /* a1 is, like SYS_WRITE's pointer, in the calling actor's
             * own (currently active) address space -- safe to write
             * into directly at CPL 0 regardless of its U/S bit. */
            actor_receive((struct message *)a1);
            return 0;

        case SYS_GRANT:
            /* a1 = dest slot, a2 = op, a3 = target. */
            return (uint64_t)actor_delegate((int)a1, (int)a2, (int)a3);

        case SYS_SPAWN:
            /* a1 = entry point, a .user_text function address the
             * caller already knew (see hal.h's own comment -- knowing
             * an address is unrestricted; actor_spawn_child() is what
             * actually gates whether the caller may act on it). */
            return (uint64_t)actor_spawn_child((void (*)(void))a1);

        case SYS_TERMINATE:
            /* a1 = target actor slot. */
            return (uint64_t)actor_terminate((int)a1);

        case SYS_OBJECT_READ:
            /* a1 = object id, a2 = buf* (caller's own memory, safe to
             * write directly at CPL 0 -- same reasoning as SYS_RECEIVE
             * above), a3 = buf len. Capability check happens HERE, not
             * in storage.c, which has no idea actors or capabilities
             * exist at all (see its own top comment). */
            if (!actor_current_has_cap(CAP_READ_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_read((int)a1, (void *)a2, (uint32_t)a3);

        case SYS_OBJECT_WRITE:
            if (!actor_current_has_cap(CAP_WRITE_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_write((int)a1, (const void *)a2, (uint32_t)a3);

        case SYS_OBJECT_PROMOTE:
            /* CAP_PROMOTE_OBJECT is blanket (target 0), like
             * CAP_SPAWN -- see actor.h's own comment. */
            if (!actor_current_has_cap(CAP_PROMOTE_OBJECT, 0)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_promote((int)a1);

        case SYS_OBJECT_REJECT:
            /* Same capability as promote -- one verdict, two outcomes. */
            if (!actor_current_has_cap(CAP_PROMOTE_OBJECT, 0)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_reject((int)a1);

        case SYS_NET_SEND:
            if (!actor_current_has_cap(CAP_NET, 0)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)net_send_message(a1, a2);

        case SYS_NET_SEND_TO: {
            if (!actor_current_has_cap(CAP_NET, 0)) {
                return (uint64_t)-1;
            }
            uint8_t mac[6];
            for (int i = 0; i < 6; i++) {
                mac[i] = (uint8_t)(a3 >> (8 * i));
            }
            return (uint64_t)(int64_t)net_send_message_to(mac, a1, a2);
        }

        case SYS_NET_SEND_RELIABLE: {
            if (!actor_current_has_cap(CAP_NET, 0)) {
                return (uint64_t)-1;
            }
            uint8_t mac[6];
            for (int i = 0; i < 6; i++) {
                mac[i] = (uint8_t)(a3 >> (8 * i));
            }
            return (uint64_t)(int64_t)net_send_message_reliable_to(mac, a1, a2);
        }

        case SYS_NET_RECEIVE: {
            if (!actor_current_has_cap(CAP_NET, 0)) {
                return (uint64_t)-1;
            }
            /* a1 is in the calling actor's own (currently active)
             * address space -- safe to write directly at CPL 0, same
             * reasoning as SYS_RECEIVE's own. */
            struct net_message *out = (struct net_message *)a1;
            uint64_t type = 0, data = 0, sender_actor = 0;
            uint8_t sender_mac[6];
            int rc = net_poll_receive_message(&type, &data, &sender_actor, sender_mac, (uint32_t)a2);
            if (rc == 1) {
                out->type = type;
                out->data = data;
                out->sender_actor = sender_actor;
                for (int i = 0; i < 6; i++) {
                    out->sender_mac[i] = sender_mac[i];
                }
            }
            return (uint64_t)(int64_t)rc;
        }

        case SYS_SPAWN_PROGRAM:
            /* a1 = storage object id. CAP_SPAWN + spawn quota are
             * checked inside actor_spawn_program_child() itself (same
             * as SYS_SPAWN's own actor_spawn_child()) -- CAP_READ_OBJECT
             * is checked HERE, not in core/loader.c, which (like
             * core/storage.c and core/net.c before it) has no idea
             * capabilities exist. Loading a program requires being
             * authorized to read the bytes that will actually execute,
             * not just spawn authority in general. */
            if (!actor_current_has_cap(CAP_READ_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)loader_spawn_program((int)a1);

        default:
            return (uint64_t)-1;
    }
}
