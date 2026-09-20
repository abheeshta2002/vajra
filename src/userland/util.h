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
 * writable data. Everything mutable lives on the stack.
 * ---------------------------------------------------------------- */
#define MSG_ARG       0x80
#define MSG_ARG_END   0x81
#define UTIL_ARGS_MAX 48

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

#define UTIL_MAIN __attribute__((section(".text.start"))) void _start(void)
#define UTIL_OBJ_MAX 2048

#endif
