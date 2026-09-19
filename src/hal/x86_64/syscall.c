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

/* ------------------------------------------------------------------
 * Roadmap Phase 24: every syscall pointer argument below is an ACTOR
 * virtual address, walked at CPL 0 (syscalls don't switch CR3) -- and
 * CPL 0 ignores the U/S bit, so a raw cast-and-dereference (this
 * file's own approach before this phase) lets a1/a2/a3 name ANY mapped
 * kernel address, not just this actor's own memory. Every dereference
 * below now goes through core/actor.c's actor_current_owns_range() (a
 * WRITE target: only this actor's own stack or program window is ever
 * legal) or actor_current_may_read_range() (a READ source: the same,
 * plus the kernel's own low image below 1MB, where the built-in demo
 * actors' own string literals genuinely live) -- see actor.c's own
 * comment for the full reasoning behind the two different bounds.
 *
 * copy_user_string() is the NUL-terminated-string counterpart to a
 * flat range check: length isn't known up front, so it walks one byte
 * at a time, checking read-permission of exactly the byte it's about
 * to read -- the same bound a well-formed string's own NUL would stop
 * it at, so a legitimately short string near the edge of its owning
 * window is never penalized for the window being small. Static .bss
 * scratch, not a stack buffer, same reasoning as core/loader.c's/
 * core/storage.c's own scratch: this runs on whichever actor's kernel
 * stack happens to be active, and interrupts stay disabled for the
 * whole syscall. */
#define SAFE_STRING_MAX 512
static char safe_string_buf[SAFE_STRING_MAX];

static int copy_user_string(uint64_t user_ptr, char *out, int out_capacity) {
    int i = 0;
    for (; i < out_capacity - 1; i++) {
        uint64_t addr = user_ptr + (uint64_t)i;
        if (!actor_current_may_read_range(addr, 1)) {
            return -1; /* ran off memory this actor is allowed to read before finding NUL */
        }
        char c = *(const char *)addr;
        out[i] = c;
        if (c == '\0') {
            return i; /* length, excluding the NUL */
        }
    }
    return -1; /* no NUL within out_capacity -- treat exactly like an ownership failure */
}

uint64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (num) {
        case SYS_WRITE:
            /* Roadmap Phase 18 (revised): routes to the calling
             * actor's own console pane first -- see
             * hal/x86_64/console.c's own top comment for why this
             * exists (the shell's prompt was otherwise invisible,
             * buried under the scripted demo's shared-screen flood). */
            if (copy_user_string(a1, safe_string_buf, sizeof(safe_string_buf)) < 0) {
                return (uint64_t)-1;
            }
            hal_console_set_window(actor_current_window());
            hal_console_write(safe_string_buf);
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
            if (!actor_current_owns_range(a1, sizeof(struct message))) {
                return (uint64_t)-1;
            }
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
            /* a1 = object id, a2 = buf*, a3 = buf len. Capability check
             * happens HERE, not in storage.c, which has no idea actors
             * or capabilities exist at all (see its own top comment). */
            if (!actor_current_has_cap(CAP_READ_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            if (!actor_current_owns_range(a2, a3)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_read((int)a1, (void *)a2, (uint32_t)a3);

        case SYS_OBJECT_WRITE:
            /* a2 is a READ source (the kernel copies FROM it into the
             * object store) -- may_read_range, not owns_range: several
             * built-in demo actors write a kernel .rodata string
             * literal straight into an object (core/main.c). */
            if (!actor_current_has_cap(CAP_WRITE_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            if (!actor_current_may_read_range(a2, a3)) {
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
            if (!actor_current_owns_range(a1, sizeof(struct net_message))) {
                return (uint64_t)-1;
            }
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

        case SYS_LOOKUP_NAME:
            /* No capability check, deliberately -- see hal.h's own comment. */
            if (copy_user_string(a1, safe_string_buf, sizeof(safe_string_buf)) < 0) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_lookup_by_name(safe_string_buf);

        case SYS_LIST_OBJECTS: {
            if (!actor_current_has_cap(CAP_LIST_NAMES, 0)) {
                return (uint64_t)-1;
            }
            if (!actor_current_owns_range(a2, sizeof(struct object_info))) {
                return (uint64_t)-1;
            }
            struct object_info *out = (struct object_info *)a2;
            int id = 0, trust = 0;
            uint32_t size = 0;
            char name_buf[16];
            int rc = storage_get_by_index((int)a1, name_buf, &id, &trust, &size);
            if (rc != 1) {
                return (uint64_t)0;
            }
            out->id = id;
            out->trust = trust;
            out->size_bytes = size;
            for (int i = 0; i < 16; i++) {
                out->name[i] = name_buf[i];
            }
            return (uint64_t)1;
        }

        case SYS_CREATE_NAME: {
            if (!actor_current_has_cap(CAP_CREATE_OBJECT, 0)) {
                return (uint64_t)-1;
            }
            if (copy_user_string(a1, safe_string_buf, sizeof(safe_string_buf)) < 0) {
                return (uint64_t)-1;
            }
            int id = storage_create_named(safe_string_buf);
            if (id < 0) {
                return (uint64_t)-1;
            }
            /* Creator gets natural authority over what it created --
             * same pattern as SYS_SPAWN's own CAP_SEND/CAP_TERMINATE
             * auto-grant (core/actor.c). Granted here, not in
             * core/storage.c, which has no idea actors or capabilities
             * exist at all (see its own top comment). */
            int slot = actor_current_slot();
            actor_grant(slot, CAP_READ_OBJECT, id);
            actor_grant(slot, CAP_WRITE_OBJECT, id);
            actor_grant(slot, CAP_RENAME_OBJECT, id);
            actor_grant(slot, CAP_DELETE_OBJECT, id);
            return (uint64_t)id;
        }

        case SYS_RENAME_OBJECT:
            if (!actor_current_has_cap(CAP_RENAME_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            if (copy_user_string(a2, safe_string_buf, sizeof(safe_string_buf)) < 0) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_rename((int)a1, safe_string_buf);

        case SYS_DELETE_NAME:
            if (!actor_current_has_cap(CAP_DELETE_OBJECT, (int)a1)) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)storage_delete((int)a1);

        case SYS_KEY_READ: {
            /* No args. See hal.h's own comment on why this alone,
             * unlike SYS_WRITE, is capability-gated. */
            if (!actor_current_has_cap(CAP_CONSOLE, 0)) {
                return (uint64_t)-1;
            }
            int key = hal_keyboard_poll();
            /* Still drain the hardware buffer above even when denying
             * below -- otherwise keystrokes typed while this actor's
             * own app isn't the focused one on screen would queue up
             * and all flood in at once the moment it regains focus,
             * instead of simply being what they look like: nothing
             * typed while you weren't looking at that app. */
            if (!hal_console_is_focused(actor_current_window())) {
                return (uint64_t)-1;
            }
            return (uint64_t)(int64_t)key;
        }

        case SYS_RTC_READ:
            /* No capability check -- see hal.h's own comment. */
            if (!actor_current_owns_range(a1, sizeof(struct rtc_time))) {
                return (uint64_t)-1;
            }
            hal_rtc_read((struct rtc_time *)a1);
            return 0;

        case SYS_MOUSE_READ: {
            /* Same gating as SYS_KEY_READ -- see hal.h's own comment
             * on why the two share CAP_CONSOLE rather than a new cap. */
            if (!actor_current_has_cap(CAP_CONSOLE, 0)) {
                return (uint64_t)-1;
            }
            if (!actor_current_owns_range(a1, sizeof(struct mouse_state))) {
                return (uint64_t)-1;
            }
            struct mouse_state *out = (struct mouse_state *)a1;
            int col, row, buttons;
            int r = hal_mouse_poll(&col, &row, &buttons);
            if (r < 0) {
                return (uint64_t)-1;
            }
            out->col = col;
            out->row = row;
            out->buttons = buttons;
            hal_console_mouse_update(col, row, buttons);
            return 0;
        }

        default:
            return (uint64_t)-1;
    }
}
