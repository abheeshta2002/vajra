#include "util.h"

/* ls: every name in the namespace with size, trust state, domain and the
 * time it was last changed. Needs CAP_LIST_NAMES (the shell delegates it
 * for this command only). */
static void pad(const char *s, int width) {
    int n = 0;
    while (s[n]) { n++; }
    user_write(s);
    for (; n < width; n++) { user_write(" "); }
}

static void pad_num(long long v, int width) {
    int digits = 1;
    for (long long t = v; t >= 10; t /= 10) { digits++; }
    for (int i = digits; i < width; i++) { user_write(" "); }
    user_write_int(v);
}

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct object_info oi;
    int rc = 0;
    for (int i = 0;; i++) {
        rc = user_list_objects(i, &oi);
        if (rc != 1) { break; }
        pad(oi.name, 24);
        pad_num(oi.size_bytes, 6);
        user_write("  ");
        if (oi.trust == 3)      { user_write("trusted   "); }
        else if (oi.trust == 4) { user_write("REJECTED  "); }
        else                    { user_write("untrusted "); }
        user_write(oi.user ? "user  " : "system");
        user_write(oi.flags & 1 ? " ro " : "    ");
        util_write_datetime(oi.modified);
        user_write("\n");
    }
    if (rc < 0) {
        user_write("ls: not permitted (no listing capability)\n");
    }
    user_exit();
}
