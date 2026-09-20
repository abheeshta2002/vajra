#include "util.h"

/* nl FILE: the lines, numbered. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int start = 0, n = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            if (i > start || i < len) {
                n++;
                char num[8];
                int k = 0;
                for (int v = n; v > 0 && k < 6; v /= 10) { num[k++] = (char)('0' + v % 10); }
                for (int pad = k; pad < 5; pad++) { user_write(" "); }
                while (k > 0) { char one[2]; one[0] = num[--k]; one[1] = 0; user_write(one); }
                user_write("  ");
                util_put_line(t + start, i - start);
            }
            start = i + 1;
        }
    }
    user_exit();
}
