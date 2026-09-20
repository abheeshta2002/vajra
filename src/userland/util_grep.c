#include "util.h"

/* grep PATTERN FILE: print the lines of FILE containing PATTERN. Needs
 * CAP_READ_OBJECT for that one object. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_split(args);
    if (args[0] == 0 || file[0] == 0) {
        user_write("usage: grep <text> <file>\n");
        user_exit();
    }
    int id = user_lookup_name(file);
    if (id < 0) {
        user_write("grep: no such file\n");
        user_exit();
    }
    char buf[UTIL_OBJ_MAX + 1];
    int n = user_object_read(id, buf, UTIL_OBJ_MAX);
    if (n < 0) {
        user_write("grep: permission denied\n");
        user_exit();
    }
    util_sanitize(buf, n);
    buf[n] = 0;
    int plen = 0;
    while (args[plen]) { plen++; }
    int hits = 0;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            int found = 0;
            for (int j = start; j + plen <= i && !found; j++) {
                int k = 0;
                while (k < plen && buf[j + k] == args[k]) { k++; }
                if (k == plen) { found = 1; }
            }
            if (found && i > start) {
                char saved = buf[i];
                buf[i] = 0;
                user_write(buf + start);
                user_write("\n");
                buf[i] = saved;
                hits++;
            }
            start = i + 1;
        }
    }
    if (hits == 0) {
        user_write("(no matching lines)\n");
    }
    user_exit();
}
