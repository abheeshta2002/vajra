#include "util.h"

/* ls: every name in the namespace with size, trust state and domain.
 * Needs CAP_LIST_NAMES (the shell delegates it for this command only). */
static void pad(const char *s, int width) {
    int n = 0;
    while (s[n]) { n++; }
    user_write(s);
    for (; n < width; n++) { user_write(" "); }
}

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct object_info oi;
    int rc = 0;
    for (int i = 0;; i++) {
        rc = user_list_objects(i, &oi);
        if (rc != 1) { break; }
        pad(oi.name, 17);
        user_write_int(oi.size_bytes);
        user_write(" bytes  ");
        if (oi.trust == 3)      { user_write("trusted   "); }
        else if (oi.trust == 4) { user_write("REJECTED  "); }
        else                    { user_write("untrusted "); }
        user_write(oi.user ? "user\n" : "system\n");
    }
    if (rc < 0) {
        user_write("ls: not permitted (no listing capability)\n");
    }
    user_exit();
}
