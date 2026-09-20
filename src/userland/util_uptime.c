#include "util.h"

/* uptime: how long the machine has been running. Needs the console capability (delegated). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct kernel_stats ks;
    if (user_kernel_stats(&ks) != 0) { user_write("uptime: not permitted\n"); user_exit(); }
    long long s = (long long)(ks.ticks / 100);
    user_write("up "); user_write_int(s / 3600); user_write("h ");
    user_write_int((s / 60) % 60); user_write("m "); user_write_int(s % 60); user_write("s\n");
    user_exit();
}
