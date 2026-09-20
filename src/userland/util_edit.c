#include "util.h"

/* edit NAME: a deliberately small line editor. Type lines; a line holding
 * only "." saves and quits; ESC quits without saving; backspace works.
 * Needs the keyboard (CAP_CONSOLE, delegated) and WRITE+READ on the file --
 * or CAP_CREATE_OBJECT when the file does not exist yet. */
UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    if (args[0] == 0) {
        user_write("usage: edit <name>\n");
        user_exit();
    }
    char buf[UTIL_OBJ_MAX + 1];
    int n = 0;
    int id = user_lookup_name(args);
    if (id >= 0) {
        n = user_object_read(id, buf, UTIL_OBJ_MAX);
        if (n < 0) {
            user_write("edit: permission denied (not given authority over that file)\n");
            user_exit();
        }
        util_sanitize(buf, n);
    } else {
        id = user_create_name(args);
        if (id < 0) {
            user_write("edit: cannot create that file (creation quota spent?)\n");
            user_exit();
        }
    }
    user_write("-- editing '");
    user_write(args);
    user_write("': type text; a line with only '.' saves, ESC cancels --\n");
    buf[n] = 0;
    user_write(buf);
    if (n > 0 && buf[n - 1] != '\n') {
        buf[n++] = '\n';
        user_write("\n");
    }
    for (;;) {
        int c = user_key_read();
        if (c < 0) {
            user_sleep(1);
            continue;
        }
        if (c == 27) {
            user_write("edit: cancelled, nothing saved\n");
            user_exit();
        }
        if (c == '\n' || c == '\r') {
            int ls = n;
            while (ls > 0 && buf[ls - 1] != '\n') { ls--; }
            if (n - ls == 1 && buf[ls] == '.') {
                n = ls; /* drop the terminating "." line */
                user_write("\n");
                break;
            }
            if (n < UTIL_OBJ_MAX) { buf[n++] = '\n'; }
            user_write("\n");
        } else if (c == '\b' || c == 0x7F) {
            if (n > 0 && buf[n - 1] != '\n') {
                n--;
                user_write("\b \b");
            }
        } else if (c >= 0x20 && c < 0x7F && n < UTIL_OBJ_MAX - 1) {
            char echo[2];
            echo[0] = (char)c;
            echo[1] = 0;
            buf[n++] = (char)c;
            user_write(echo);
        }
    }
    if (user_object_write(id, buf, (uint32_t)n) < 0) {
        user_write("edit: could not save (no write authority?)\n");
    } else {
        user_write("edit: saved ");
        user_write_int(n);
        user_write(" bytes\n");
    }
    user_exit();
}
