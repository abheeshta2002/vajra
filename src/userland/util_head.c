#include "util.h"

/* head [N] FILE: the first N lines (default 10). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int want = args[0] ? util_atoi(args) : 10;
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int start = 0, printed = 0;
    for (int i = 0; i <= len && printed < want; i++) {
        if (i == len || t[i] == '\n') {
            if (i > start || i < len) { util_put_line(t + start, i - start); printed++; }
            start = i + 1;
        }
    }
    user_exit();
}
