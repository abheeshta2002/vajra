#include "util.h"

/* stat NAME: everything the store knows about one object. Needs the listing capability. */
static void date(uint32_t t) { util_write_datetime(t); }

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct object_info oi;
    int rc = 0;
    for (int i = 0;; i++) {
        rc = user_list_objects(i, &oi);
        if (rc != 1) { break; }
        if (util_streq(oi.name, args)) {
            user_write("  name:     "); user_write(oi.name); user_write("\n");
            user_write("  id:       "); user_write_int(oi.id); user_write("\n");
            user_write("  size:     "); user_write_int(oi.size_bytes); user_write(" bytes\n");
            user_write("  trust:    ");
            user_write(oi.trust == 3 ? "trusted" : oi.trust == 4 ? "REJECTED" : oi.trust == 2 ? "analyzed" :
                       oi.trust == 1 ? "quarantined" : "untrusted");
            user_write("\n");
            user_write("  domain:   "); user_write(oi.user ? "user (the shell may change it)" : "system"); user_write("\n");
            user_write("  flags:    "); user_write(oi.flags & 1 ? "read-only" : "writable"); user_write("\n");
            user_write("  created:  "); date(oi.created); user_write("\n");
            user_write("  modified: "); date(oi.modified); user_write("\n");
            user_exit();
        }
    }
    user_write(rc < 0 ? "stat: not permitted (no listing capability)\n" : "stat: no such file\n");
    user_exit();
}
