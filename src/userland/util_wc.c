#include "util.h"

/* wc FILE: lines, words and bytes. Needs read on FILE (or its input pipe). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int lines = 0, words = 0, in_word = 0;
    for (int i = 0; i < len; i++) {
        if (t[i] == '\n') { lines++; }
        int sp = (t[i] == ' ' || t[i] == '\n' || t[i] == '\t');
        if (!sp && !in_word) { words++; }
        in_word = !sp;
    }
    user_write_int(lines); user_write(" lines  ");
    user_write_int(words); user_write(" words  ");
    user_write_int(len); user_write(" bytes  ");
    user_write(file);
    user_write("\n");
    user_exit();
}
