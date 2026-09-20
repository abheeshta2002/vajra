#include "util.h"

/* expr A OP B: integer arithmetic with + - * / %  (write * as x or * -- the shell passes it through). */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    char *rest = util_split(args);
    char *b = util_split(rest);
    long long x = util_atoi(args);
    long long y = util_atoi(b);
    char op = rest[0];
    long long r = 0;
    int ok = 1;
    if (op == '+') { r = x + y; }
    else if (op == '-') { r = x - y; }
    else if (op == '*' || op == 'x') { r = x * y; }
    else if (op == '/') { if (y == 0) { ok = 0; } else { r = x / y; } }
    else if (op == '%') { if (y == 0) { ok = 0; } else { r = x % y; } }
    else { ok = 0; }
    if (!ok) { user_write("usage: expr <a> <+ - * / %> <b>   (no division by zero)\n"); user_exit(); }
    user_write_int(r);
    user_write("\n");
    user_exit();
}
