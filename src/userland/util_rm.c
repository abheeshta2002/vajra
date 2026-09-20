#include "util.h"

/* rm NAME: delete. Needs CAP_DELETE_OBJECT for that one object. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    int id = user_lookup_name(args);
    if (args[0] == 0) {
        user_write("usage: rm <name>\n");
    } else if (id < 0) {
        user_write("rm: no such file\n");
    } else if (user_delete_name(id) == 0) {
        user_write("rm: removed\n");
    } else {
        user_write("rm: refused (no delete authority over that)\n");
    }
    user_exit();
}
