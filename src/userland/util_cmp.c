#include "util.h"

/* cmp A B: are two files identical? Says where they first differ. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *b_name = util_split(args);   /* the shell passes exactly "A B" */
    char *a_name = args;
    int la, lb;
    char *a = util_slurp(a_name, &la);
    if (!a) { user_exit(); }
    char *b = util_slurp(b_name, &lb);
    if (!b) { user_exit(); }
    int n = la < lb ? la : lb;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            user_write("differ at byte "); user_write_int(i); user_write("\n");
            user_exit();
        }
    }
    if (la != lb) { user_write("one is longer: "); user_write_int(la); user_write(" vs "); user_write_int(lb); user_write(" bytes\n"); }
    else { user_write("identical\n"); }
    user_exit();
}
