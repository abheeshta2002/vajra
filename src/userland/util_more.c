#include "util.h"

/* more FILE: shows a file a screenful at a time. Space or Enter for the next page, q to quit.
 * Needs read on FILE and the keyboard (delegated for this command). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *file = util_last_word(args);
    int len;
    char *t = util_slurp(file, &len);
    if (!t) { user_exit(); }
    int start = 0, shown = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            if (i > start || i < len) {
                util_put_line(t + start, i - start);
                shown++;
                if (shown % 20 == 0 && i < len) {
                    user_write("-- more (space/Enter: next page, q: quit) --");
                    int c;
                    while ((c = user_key_read()) < 0) { user_sleep(1); }
                    user_write("\n");
                    if (c == 'q' || c == 'Q' || c == 27) { break; }
                }
            }
            start = i + 1;
        }
    }
    user_exit();
}
