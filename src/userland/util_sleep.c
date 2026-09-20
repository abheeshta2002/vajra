#include "util.h"

/* sleep N: wait N seconds. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    int n = util_atoi(args);
    if (n < 0) { n = 0; }
    if (n > 3600) { n = 3600; }
    user_sleep((uint64_t)n * 100u);
    user_exit();
}
