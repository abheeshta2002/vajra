#include "util.h"

/* diff A B: the lines that differ, compared line by line (not a minimal edit script:
 * an inserted line makes every later line differ). */
static int next_line(const char *t, int len, int *pos, int *start) {
    if (*pos >= len) { return 0; }
    *start = *pos;
    while (*pos < len && t[*pos] != '\n') { (*pos)++; }
    int l = *pos - *start;
    if (*pos < len) { (*pos)++; }
    return l + 1;   /* length+1 so an empty line still counts as a line */
}

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
    int pa = 0, pb = 0, line = 0, diffs = 0;
    for (;;) {
        int sa = 0, sb = 0;
        int ea = next_line(a, la, &pa, &sa);
        int eb = next_line(b, lb, &pb, &sb);
        if (!ea && !eb) { break; }
        line++;
        int same = (ea == eb);
        for (int k = 0; same && k < ea - 1; k++) { if (a[sa + k] != b[sb + k]) { same = 0; } }
        if (!same) {
            diffs++;
            user_write("line "); user_write_int(line); user_write(":\n");
            if (ea) { user_write("< "); util_put_line(a + sa, ea - 1); }
            if (eb) { user_write("> "); util_put_line(b + sb, eb - 1); }
        }
    }
    if (diffs == 0) { user_write("no differences\n"); }
    user_exit();
}
