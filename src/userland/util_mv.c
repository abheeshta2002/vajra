#include "util.h"

/* mv FROM TO: rename. Needs CAP_RENAME_OBJECT for that one object. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *to = util_split(args);
    int id = user_lookup_name(args);
    if (args[0] == 0 || to[0] == 0) {
        user_write("usage: mv <from> <to>\n");
    } else if (id < 0) {
        user_write("mv: no such file\n");
    } else if (user_rename_object(id, to) == 0) {
        user_write("mv: renamed\n");
    } else {
        user_write("mv: refused (no rename authority over that, or the new name is taken)\n");
    }
    user_exit();
}
