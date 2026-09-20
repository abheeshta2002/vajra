#include "util.h"

/* cal: this month's calendar, from the CMOS clock. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    struct rtc_time t;
    user_rtc_read(&t);
    static const char *const names[12] = { "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December" };
    int y = t.year, m = t.month, d = t.day;
    int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) { dim[1] = 29; }
    static const int tt[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = m < 3 ? y - 1 : y;
    int first = (yy + yy / 4 - yy / 100 + yy / 400 + tt[m - 1] + 1) % 7;   /* 0 = Sunday */
    user_write("   "); user_write(names[m - 1]); user_write(" "); user_write_int(y); user_write("\n");
    user_write("Su Mo Tu We Th Fr Sa\n");
    for (int i = 0; i < first; i++) { user_write("   "); }
    for (int day = 1; day <= dim[m - 1]; day++) {
        if (day < 10) { user_write(" "); }
        if (day == d) { user_write("\x1b[30;47m"); }
        user_write_int(day);
        if (day == d) { user_write("\x1b[0m"); }
        user_write(" ");
        if ((first + day) % 7 == 0) { user_write("\n"); }
    }
    user_write("\n");
    user_exit();
}
