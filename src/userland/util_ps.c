#include "util.h"

/* ps: the actors this program was given introspection authority over --
 * by default only the shell's own descendants (see CAP_INTROSPECT). There
 * is no global process table to read: a slot you hold no capability for
 * simply is not visible. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct actor_info ai;
    int shown = 0;
    for (int slot = 0; slot < MAX_ACTORS; slot++) {
        if (user_actor_info(slot, &ai) != 0) {
            continue;
        }
        if (shown == 0) {
            user_write("slot  state\n");
        }
        shown++;
        user_write("  ");
        user_write_int(slot);
        user_write(slot < 10 ? "   " : "  ");
        if (ai.state == 0)      { user_write("dead\n"); }
        else if (ai.state == 1) { user_write("ready\n"); }
        else if (ai.state == 2) { user_write("running\n"); }
        else if (ai.state == 3) { user_write("waiting for a message\n"); }
        else                    { user_write("sleeping\n"); }
    }
    if (shown == 0) {
        user_write("(no background jobs you may see -- start one with 'count' or 'pipe')\n");
    }
    user_exit();
}
