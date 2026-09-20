#include "util.h"

/* cores: each CPU core and the actor it is running right now. Needs the console capability (delegated). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct core_info ci[MAX_CPUS];
    if (user_core_info(MAX_CPUS, ci) != 0) { user_write("cores: not permitted\n"); user_exit(); }
    int online = 0;
    for (int i = 0; i < MAX_CPUS; i++) { if (ci[i].online) { online++; } }
    user_write_int(online); user_write(" core(s) online\n");
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!ci[i].online) { continue; }
        user_write("  core "); user_write_int(i); user_write(": ");
        if (ci[i].running_slot >= 0) { user_write("running actor "); user_write_int(ci[i].running_slot); }
        else { user_write("idle"); }
        user_write("  (switched into "); user_write_int((long long)ci[i].switches); user_write(" actors)\n");
    }
    user_exit();
}
