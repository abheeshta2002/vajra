#include "util.h"

/* rev FILE: each line written backwards. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            if (i > start || i < len) {
                int a = start, b = i - 1;
                while (a < b) { char c = t[a]; t[a] = t[b]; t[b] = c; a++; b--; }
                util_put_line(t + start, i - start);
            }
            start = i + 1;
        }
    }
    user_exit();
}
