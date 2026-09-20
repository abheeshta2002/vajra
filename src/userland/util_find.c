#include "util.h"

/* find TEXT: every name containing TEXT, anywhere in the namespace. Needs the listing capability. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    int plen = 0;
    while (args[plen]) { plen++; }
    struct object_info oi;
    int rc = 0, hits = 0;
    for (int i = 0;; i++) {
        rc = user_list_objects(i, &oi);
        if (rc != 1) { break; }
        int n = 0;
        while (oi.name[n]) { n++; }
        for (int j = 0; j + plen <= n; j++) {
            int k = 0;
            while (k < plen && oi.name[j + k] == args[k]) { k++; }
            if (k == plen) {
                user_write(oi.name); user_write("\n");
                hits++;
                break;
            }
        }
    }
    if (rc < 0) { user_write("find: not permitted (no listing capability)\n"); }
    else if (hits == 0) { user_write("(nothing found)\n"); }
    user_exit();
}
