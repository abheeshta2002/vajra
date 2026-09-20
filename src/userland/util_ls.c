#include "util.h"

/* ls [DIR]: the contents of one directory -- files, and sub-directories shown
 * with a trailing '/'. The shell has resolved DIR to a full name (or "" for
 * the root), so every object whose name starts with it and has no further '/'
 * in the rest is a direct child. Columns: modified time, size, trust, domain,
 * read-only flag, name. Needs CAP_LIST_NAMES (delegated for this command only). */
static int has_prefix(const char *name, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (name[i] != prefix[i]) { return 0; }
        i++;
    }
    return 1;
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
    int plen = 0;
    while (args[plen]) { plen++; }
    struct object_info oi;
    int rc = 0;
    int shown = 0;
    for (int i = 0;; i++) {
        rc = user_list_objects(i, &oi);
        if (rc != 1) { break; }
        if (!has_prefix(oi.name, args)) { continue; }
        const char *rest = oi.name + plen;
        if (rest[0] == 0) { continue; }          /* the directory's own marker */
        int slash = -1;
        int n = 0;
        while (rest[n]) {
            if (rest[n] == '/' && slash < 0) { slash = n; }
            n++;
        }
        int is_dir = (slash >= 0 && slash == n - 1);
        if (slash >= 0 && !is_dir) { continue; } /* deeper down: shown by its own directory */
        util_write_datetime(oi.modified);
        pad_num(oi.size_bytes, 7);
        user_write("  ");
        if (oi.trust == 3)      { user_write("trusted   "); }
        else if (oi.trust == 4) { user_write("REJECTED  "); }
        else                    { user_write("untrusted "); }
        user_write(oi.user ? "user  " : "system");
        user_write(oi.flags & 1 ? " ro  " : "     ");
        user_write(rest);
        user_write("\n");
        shown++;
    }
    if (rc < 0) {
        user_write("ls: not permitted (no listing capability)\n");
    } else if (shown == 0) {
        user_write("(empty)\n");
    }
    user_exit();
}
