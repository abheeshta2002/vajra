#include "util.h"

/* cat NAME: print a file, in pieces (any size up to the object limit).
 * Needs CAP_READ_OBJECT for that one object. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    int id = user_lookup_name(args);
    if (id < 0) {
        user_write("cat: no such file\n");
    } else {
        char buf[UTIL_CHUNK + 1];
        uint32_t off = 0;
        int last = '\n';
        for (;;) {
            int n = user_object_read_at(id, off, buf, UTIL_CHUNK);
            if (n < 0) {
                user_write("cat: permission denied (I was not given read authority over that)\n");
                user_exit();
            }
            if (n == 0) {
                break;
            }
            util_sanitize(buf, n);
            buf[n] = 0;
            user_write(buf);
            last = buf[n - 1];
            off += (uint32_t)n;
        }
        if (last != '\n') { user_write("\n"); }
    }
    user_exit();
}
