#include "util.h"

/* tree [DIR]: everything under a directory, indented by depth. The shell has
 * resolved DIR to a full name ("" = root). Depth-first: for each direct child
 * of a directory, print it and, if it is a directory itself, recurse. Needs
 * CAP_LIST_NAMES (delegated for this command only). */
static int has_prefix(const char *name, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (name[i] != prefix[i]) { return 0; }
        i++;
    }
    return 1;
}

static void walk(const char *dir, int depth) {
    int plen = 0;
    while (dir[plen]) { plen++; }
    struct object_info oi;
    for (int i = 0; user_list_objects(i, &oi) == 1; i++) {
        if (!has_prefix(oi.name, dir)) { continue; }
        const char *rest = oi.name + plen;
        if (rest[0] == 0) { continue; }
        int slash = -1;
        int n = 0;
        while (rest[n]) {
            if (rest[n] == '/' && slash < 0) { slash = n; }
            n++;
        }
        int is_dir = (slash >= 0 && slash == n - 1);
        if (slash >= 0 && !is_dir) { continue; }
        for (int d = 0; d < depth; d++) { user_write("  "); }
        user_write(rest);
        if (!is_dir) {
            user_write("  (");
            user_write_int(oi.size_bytes);
            user_write(")");
        }
        user_write("\n");
        if (is_dir && depth < 4) {
            char sub[48];
            int k = 0;
            while (oi.name[k] && k < 46) { sub[k] = oi.name[k]; k++; }
            sub[k] = 0;
            walk(sub, depth + 1);
        }
    }
}

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    user_write(args[0] ? args : "/");
    user_write("\n");
    walk(args, 1);
    user_exit();
}
