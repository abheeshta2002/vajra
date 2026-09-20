#include "util.h"

/* file NAME: what kind of thing a file is. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    user_write(file);
    user_write(": ");
    if (len == 0) {
        user_write("empty\n");
    } else if (len >= 12 && t[0] == 'V' && t[1] == 'A' && t[2] == 'J' && t[3] == 'R') {
        user_write("a Vajra program (");
        user_write_int(len - 12);
        user_write(" bytes of code and data)\n");
    } else {
        int text = 1, lines = 0;
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)t[i];
            if (c == '\n') { lines++; }
            else if (c != '\t' && (c < 0x20 || c > 0x7E)) { text = 0; break; }
        }
        if (text) { user_write("text, "); user_write_int(lines); user_write(" lines\n"); }
        else { user_write("binary data, "); user_write_int(len); user_write(" bytes\n"); }
    }
    user_exit();
}
