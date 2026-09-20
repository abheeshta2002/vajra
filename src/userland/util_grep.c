#include "util.h"

/* grep PATTERN FILE: print the lines of FILE containing PATTERN, reading the
 * file in pieces (a line is examined up to its first 79 characters). Needs
 * CAP_READ_OBJECT for that one object. */
#define GREP_LINE 80

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);   /* everything before the file is the pattern */
    if (args[0] == 0 || file[0] == 0) {
        user_write("usage: grep <text> <file>\n");
        user_exit();
    }
    int id = user_lookup_name(file);
    if (id < 0) {
        user_write("grep: no such file\n");
        user_exit();
    }
    int plen = 0;
    while (args[plen]) { plen++; }
    char chunk[UTIL_CHUNK];
    char line[GREP_LINE];
    int llen = 0;
    int hits = 0;
    uint32_t off = 0;
    for (;;) {
        int n = user_object_read_at(id, off, chunk, UTIL_CHUNK);
        if (n < 0) {
            user_write("grep: permission denied\n");
            user_exit();
        }
        int done = (n == 0);
        for (int i = 0; i <= n; i++) {
            int end_of_line;
            char c = 0;
            if (i == n) {
                end_of_line = done; /* end of file terminates the last line */
            } else {
                c = chunk[i];
                end_of_line = (c == '\n');
            }
            if (end_of_line) {
                line[llen] = 0;
                int found = 0;
                for (int j = 0; j + plen <= llen && !found; j++) {
                    int k = 0;
                    while (k < plen && line[j + k] == args[k]) { k++; }
                    if (k == plen) { found = 1; }
                }
                if (found && llen > 0) {
                    user_write(line);
                    user_write("\n");
                    hits++;
                }
                llen = 0;
            } else if (i < n) {
                if (llen < GREP_LINE - 1) {
                    line[llen++] = (c >= 0x20 && c <= 0x7E) ? c : '.';
                }
            }
        }
        if (done) { break; }
        off += (uint32_t)n;
    }
    if (hits == 0) {
        user_write("(no matching lines)\n");
    }
    user_exit();
}
