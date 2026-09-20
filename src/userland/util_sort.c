#include "util.h"

/* sort [-r] FILE: the lines in byte order (or reversed with -r). Up to 500 lines. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int reverse = (args[0] == '-' && args[1] == 'r');
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    char **line = (char **)user_heap_grow(1);
    if (!line) { user_write("out of memory\n"); user_exit(); }
    int n = 0, start = 0;
    for (int i = 0; i <= len && n < 500; i++) {
        if (i == len || t[i] == '\n') {
            if (i > start || i < len) { t[i] = 0; line[n++] = t + start; }
            start = i + 1;
        }
    }
    for (int i = 1; i < n; i++) {           /* insertion sort */
        char *key = line[i];
        int j = i - 1;
        for (;;) {
            if (j < 0) { break; }
            const char *a = line[j];
            const char *b = key;
            while (*a && *a == *b) { a++; b++; }
            int cmp = (unsigned char)*a - (unsigned char)*b;
            if (reverse ? cmp >= 0 : cmp <= 0) { break; }
            line[j + 1] = line[j];
            j--;
        }
        line[j + 1] = key;
    }
    for (int i = 0; i < n; i++) {
        int l = 0;
        while (line[i][l]) { l++; }
        util_put_line(line[i], l);
    }
    user_exit();
}
