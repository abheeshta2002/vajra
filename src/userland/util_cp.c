#include "util.h"

/* cp SRC DST: needs CAP_READ_OBJECT for SRC and CAP_CREATE_OBJECT (both
 * delegated for this command). It creates DST itself, so it -- not the
 * shell -- holds authority over the new object; the shell's user-data
 * capability covers it too. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *dst = util_split(args);
    if (args[0] == 0 || dst[0] == 0) {
        user_write("usage: cp <from> <to>\n");
        user_exit();
    }
    int src = user_lookup_name(args);
    if (src < 0) {
        user_write("cp: no such file\n");
        user_exit();
    }
    char buf[UTIL_OBJ_MAX];
    int n = user_object_read(src, buf, UTIL_OBJ_MAX);
    if (n < 0) {
        user_write("cp: permission denied (no read authority over the source)\n");
        user_exit();
    }
    int id = user_create_name(dst);
    if (id < 0) {
        user_write("cp: cannot create the copy (name taken, or creation quota spent)\n");
        user_exit();
    }
    if (user_object_write(id, buf, (uint32_t)n) < 0) {
        user_write("cp: write failed\n");
    } else {
        user_write("cp: copied ");
        user_write_int(n);
        user_write(" bytes\n");
    }
    user_exit();
}
