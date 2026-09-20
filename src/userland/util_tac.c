#include "util.h"

/* tac FILE: the lines in reverse order. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int end = len;
    if (end > 0 && t[end - 1] == '\n') { end--; }
    for (int i = end; i >= 0; i--) {
        if (i == 0 || t[i - 1] == '\n') {
            util_put_line(t + i, end - i);
            end = i - 1;
            if (i == 0) { break; }
        }
    }
    user_exit();
}
