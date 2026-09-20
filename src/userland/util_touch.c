#include "util.h"

/* touch NAME: create an empty file, or update the modified time of an existing one.
 * Needs write on the file, or the create capability when it does not exist yet. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    if (file[0] == 0) { user_write("usage: touch <name>\n"); user_exit(); }
    int id = user_lookup_name(file);
    if (id < 0) {
        id = user_create_name(file);
        user_write(id < 0 ? "touch: cannot create it (name too long, quota spent, or no space)\n" : "touch: created\n");
        user_exit();
    }
    int size = 0;
    char buf[256];
    for (;;) {
        int n = user_object_read_at(id, (uint32_t)size, buf, 256);
        if (n <= 0) { break; }
        size += n;
    }
    user_write(user_object_write_at(id, (uint32_t)size, buf, 0) < 0 ? "touch: refused (no write authority, or read-only)\n" : "touch: updated\n");
    user_exit();
}
