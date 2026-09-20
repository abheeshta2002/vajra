#include "util.h"

/* strings FILE: every run of four or more printable characters. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int start = -1;
    for (int i = 0; i <= len; i++) {
        int printable = (i < len && t[i] >= 0x20 && t[i] <= 0x7E);
        if (printable && start < 0) { start = i; }
        if (!printable && start >= 0) {
            if (i - start >= 4) { util_put_line(t + start, i - start); }
            start = -1;
        }
    }
    user_exit();
}
