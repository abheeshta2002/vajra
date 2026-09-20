#include "util.h"

/* cat NAME: print a file. Needs CAP_READ_OBJECT for that one object. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    int id = user_lookup_name(args);
    if (id < 0) {
        user_write("cat: no such file\n");
    } else {
        char buf[UTIL_OBJ_MAX + 1];
        int n = user_object_read(id, buf, UTIL_OBJ_MAX);
        if (n < 0) {
            user_write("cat: permission denied (I was not given read authority over that)\n");
        } else {
            util_sanitize(buf, n);
            buf[n] = 0;
            user_write(buf);
            if (n > 0 && buf[n - 1] != '\n') { user_write("\n"); }
        }
    }
    user_exit();
}
