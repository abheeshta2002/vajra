#include "util.h"

/* uniq FILE: adjacent duplicate lines collapsed to one (sort first to catch all of them). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int start = 0, prev_start = -1, prev_len = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            if (i > start || i < len) {
                int l = i - start;
                int same = (prev_start >= 0 && l == prev_len);
                for (int k = 0; same && k < l; k++) { if (t[start + k] != t[prev_start + k]) { same = 0; } }
                if (!same) { util_put_line(t + start, l); }
                prev_start = start;
                prev_len = l;
            }
            start = i + 1;
        }
    }
    user_exit();
}
