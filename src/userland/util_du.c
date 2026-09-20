#include "util.h"

/* du: how full the store is -- objects and bytes, split into user files and system objects.
 * Needs the listing capability. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct object_info oi;
    int rc = 0, users = 0, sys = 0;
    long long ubytes = 0, sbytes = 0;
    for (int i = 0;; i++) {
        rc = user_list_objects(i, &oi);
        if (rc != 1) { break; }
        if (oi.user) { users++; ubytes += oi.size_bytes; } else { sys++; sbytes += oi.size_bytes; }
    }
    if (rc < 0) { user_write("du: not permitted (no listing capability)\n"); user_exit(); }
    user_write("  user files:      "); user_write_int(users); user_write(" objects, "); user_write_int(ubytes); user_write(" bytes\n");
    user_write("  system objects:  "); user_write_int(sys); user_write(" objects, "); user_write_int(sbytes); user_write(" bytes\n");
    user_write("  free slots:      "); user_write_int(96 - users - sys); user_write(" of 96 (each up to 12288 bytes)\n");
    user_exit();
}
