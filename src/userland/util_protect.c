#include "util.h"

/* protect NAME: make a file read-only. Needs write authority over the file. It guards against accidents -- anyone
 * holding write authority can undo it. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int id = user_lookup_name(file);
    if (id < 0) { user_write("no such file\n"); user_exit(); }
    user_write(user_object_protect(id, 1) < 0 ? "refused (no write authority over that)\n" : "now protected (read-only)\n");
    user_exit();
}
