#include "util.h"

/* seq LAST  |  seq FIRST LAST: the numbers, one per line. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *last = util_last_word(args);
    int hi = util_atoi(last);
    int lo = args[0] ? util_atoi(args) : 1;
    int count = 0;
    for (int v = lo; v <= hi && count < 2000; v++, count++) {
        user_write_int(v);
        user_write("\n");
    }
    user_exit();
}
