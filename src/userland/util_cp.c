#include "util.h"

/* cp SRC DST: needs CAP_READ_OBJECT for SRC and CAP_CREATE_OBJECT (both
 * delegated for this command). It creates DST itself, so it -- not the
 * shell -- holds authority over the new object; the shell's user-data
 * capability covers it too. Copies in pieces, so any size works. */
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
    char probe[1];
    if (user_object_read_at(src, 0, probe, 1) < 0) {
        user_write("cp: permission denied (no read authority over the source)\n");
        user_exit();
    }
    int id = user_create_name(dst);
    if (id < 0) {
        user_write("cp: cannot create the copy (name taken, too long, or creation quota spent)\n");
        user_exit();
    }
    char buf[UTIL_CHUNK];
    uint32_t off = 0;
    for (;;) {
        int n = user_object_read_at(src, off, buf, UTIL_CHUNK);
        if (n <= 0) {
            break;
        }
        if (user_object_write_at(id, off, buf, (uint32_t)n) < 0) {
            user_write("cp: write failed\n");
            user_exit();
        }
        off += (uint32_t)n;
    }
    user_write("cp: copied ");
    user_write_int(off);
    user_write(" bytes\n");
    user_exit();
}
