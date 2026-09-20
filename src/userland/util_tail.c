#include "util.h"

/* tail [N] FILE: the last N lines (default 10). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int want = args[0] ? util_atoi(args) : 10;
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int total = 0;
    for (int i = 0; i < len; i++) { if (t[i] == '\n') { total++; } }
    if (len > 0 && t[len - 1] != '\n') { total++; }
    int skip = total > want ? total - want : 0;
    int start = 0, line = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            if ((i > start || i < len) && line >= skip) { util_put_line(t + start, i - start); }
            if (i > start || i < len) { line++; }
            start = i + 1;
        }
    }
    user_exit();
}
