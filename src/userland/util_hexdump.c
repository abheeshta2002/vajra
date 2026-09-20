#include "util.h"

/* hexdump FILE: offset, sixteen bytes in hex, and the text they spell. */
static void hex(char *out, unsigned v, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        unsigned d = v & 15u;
        out[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
}

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    for (int off = 0; off < len; off += 16) {
        char row[80];
        int n = 0;
        hex(row, (unsigned)off, 6);
        n = 6;
        row[n++] = ' '; row[n++] = ' ';
        for (int i = 0; i < 16; i++) {
            if (off + i < len) { hex(row + n, (unsigned char)t[off + i], 2); n += 2; }
            else { row[n++] = ' '; row[n++] = ' '; }
            row[n++] = ' ';
        }
        row[n++] = ' ';
        for (int i = 0; i < 16 && off + i < len; i++) {
            char c = t[off + i];
            row[n++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
        }
        row[n] = 0;
        user_write(row);
        user_write("\n");
    }
    user_exit();
}
