#ifndef VAJRA_UTIL_H
#define VAJRA_UTIL_H

#include "runtime.h"
#include "vajra/actor.h"
#include "vajra/hal.h"

/* ------------------------------------------------------------------
 * Phase 19: shared plumbing for the standard utilities. Each utility is
 * a genuinely separate loaded program (its own object, its own link) that
 * starts with NO authority. The shell spawns it, delegates exactly the
 * capabilities that one command needs (for `cat notes.txt`: read on that
 * one object), then sends the command line as MSG_ARG messages (8 bytes
 * each, a message carries one word) ended by MSG_ARG_END.
 *
 * Loaded programs are mapped read-only + executable (W^X): no global
 * writable data. Everything mutable lives on the stack (4 KB in all), so
 * files are processed in small pieces with the offset read/write calls.
 * ---------------------------------------------------------------- */
#define MSG_ARG       0x80
#define MSG_ARG_END   0x81
#define UTIL_ARGS_MAX 120
#define UTIL_CHUNK    256 /* must stay well under the 511-character limit of one console write */

static inline void util_read_args(char *buf, int max) {
    struct message m;
    int n = 0;
    for (;;) {
        user_receive(&m);
        if (m.type == MSG_ARG_END) {
            break;
        }
        if (m.type == MSG_ARG) {
            for (int i = 0; i < 8; i++) {
                char c = (char)(m.data >> (8 * i));
                if (c == 0) {
                    break;
                }
                if (n < max - 1) {
                    buf[n++] = c;
                }
            }
        }
    }
    buf[n] = 0;
}

/* Splits `s` at its first space; returns the second word (possibly ""). */
static inline char *util_split(char *s) {
    while (*s && *s != ' ') {
        s++;
    }
    if (*s) {
        *s = 0;
        s++;
        while (*s == ' ') {
            s++;
        }
    }
    return s;
}

/* Whatever bytes a file holds, the console must only ever see plain text:
 * a control byte (ESC starts an ANSI sequence) would let a file rewrite
 * the screen. Everything outside printable ASCII and newline becomes '.'. */
static inline void util_sanitize(char *buf, int n) {
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c != '\n' && (c < 0x20 || c > 0x7E)) {
            buf[i] = '.';
        }
    }
}

/* Writes "YYYY-MM-DD HH:MM" for a timestamp in seconds since 2000-01-01. */
static inline void util_write_datetime(uint32_t t) {
    if (t == 0) {
        user_write("----------------");
        return;
    }
    uint32_t days = t / 86400u;
    uint32_t rem = t % 86400u;
    /* civil-from-days (era arithmetic); days counted from 2000-01-01 */
    int z = (int)days + 10957 + 719468;
    int era = z / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) { y++; }
    char out[17];
    out[0] = (char)('0' + (y / 1000) % 10);
    out[1] = (char)('0' + (y / 100) % 10);
    out[2] = (char)('0' + (y / 10) % 10);
    out[3] = (char)('0' + y % 10);
    out[4] = '-';
    out[5] = (char)('0' + m / 10);
    out[6] = (char)('0' + m % 10);
    out[7] = '-';
    out[8] = (char)('0' + d / 10);
    out[9] = (char)('0' + d % 10);
    out[10] = ' ';
    out[11] = (char)('0' + (rem / 3600) / 10);
    out[12] = (char)('0' + (rem / 3600) % 10);
    out[13] = ':';
    out[14] = (char)('0' + ((rem % 3600) / 60) / 10);
    out[15] = (char)('0' + ((rem % 3600) / 60) % 10);
    out[16] = 0;
    user_write(out);
}

/* The last space-separated word of `s` (the file operand); the word is cut off `s`, so
 * whatever came before it (options, a pattern, a count) stays in `s`. A command with a
 * single word returns that word and leaves `s` empty. */
static inline char *util_last_word(char *s) {
    int n = 0;
    while (s[n]) { n++; }
    while (n > 0 && s[n - 1] == ' ') { s[--n] = 0; }
    int i = n;
    while (i > 0 && s[i - 1] != ' ') { i--; }
    if (i > 0) {
        s[i - 1] = 0;
        return s + i;
    }
    /* a single word: shift it up one place so `s` itself becomes empty and the word is still returned */
    for (int k = n; k >= 0; k--) { s[k + 1] = s[k]; }
    s[0] = 0;
    return s + 1;
}

static inline int util_atoi(const char *s) {
    int neg = 0;
    while (*s == ' ') { s++; }
    if (*s == '-') { neg = 1; s++; }
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static inline int util_streq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) { i++; }
    return a[i] == 0 && b[i] == 0;
}

/* Reads a whole file (up to the 12 KB object limit) into fresh heap pages, NUL-terminated.
 * Returns 0 after printing the reason if it cannot. `*len` gets the byte count. */
static inline char *util_slurp(const char *name, int *len) {
    int id = user_lookup_name(name);
    if (id < 0) {
        user_write("no such file: ");
        user_write(name);
        user_write("\n");
        return 0;
    }
    char *buf = (char *)user_heap_grow(4);   /* 16 KB */
    if (!buf) {
        user_write("out of memory\n");
        return 0;
    }
    int n = user_object_read_at(id, 0, buf, 12288);
    if (n < 0) {
        user_write("permission denied (not given read authority over that)\n");
        return 0;
    }
    buf[n] = 0;
    *len = n;
    return buf;
}

/* Prints a line (without its newline) plus a newline, in pieces short enough for one
 * console write, replacing any control byte with '.'. */
static inline void util_put_line(const char *s, int n) {
    char piece[200];
    while (n > 0) {
        int k = n > 190 ? 190 : n;
        for (int i = 0; i < k; i++) {
            char c = s[i];
            piece[i] = (c >= 0x20 && c <= 0x7E) ? c : ((c == '\t') ? ' ' : '.');
        }
        piece[k] = 0;
        user_write(piece);
        s += k;
        n -= k;
    }
    user_write("\n");
}

#define UTIL_MAIN __attribute__((section(".text.start"))) void _start(void)
#define UTIL_OBJ_MAX 2048   /* what a whole-file-in-memory tool (edit) can hold on a 4 KB stack */

#endif
