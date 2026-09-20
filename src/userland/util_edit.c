#include "util.h"

/* ------------------------------------------------------------------
 * edit NAME -- a full-screen text editor (the Phase 19 line editor,
 * replaced in the "usable for real work" track).
 *
 * It takes over the shell's window with ANSI cursor addressing and keeps
 * everything -- the text (up to the 12 KB object limit), an undo log, a
 * clipboard -- in its own HEAP (SYS_HEAP_GROW), since a loaded program has
 * no writable globals and only a 4 KB stack. Keys:
 *
 *   arrows / Home / End / PgUp / PgDn     move        (Ctrl-A / Ctrl-E = line start / end)
 *   type, Enter, Tab (4 spaces), Backspace, Delete    edit
 *   Ctrl-S save          Ctrl-Q quit (twice if there are unsaved changes)
 *   Ctrl-Z undo          Ctrl-Y redo
 *   Ctrl-F find          Ctrl-G find next       Ctrl-R replace all       Ctrl-T go to line
 *   Ctrl-C copy line     Ctrl-K cut line        Ctrl-V paste
 *   Ctrl-L toggle line numbers
 *
 * The clipboard is the object ".clipboard" when the shell delegated it, so
 * text also survives between editing sessions (and `cat .clipboard` shows it).
 * Needs the keyboard (CAP_CONSOLE) and READ+WRITE on the file, or
 * CAP_CREATE_OBJECT when the file does not exist yet.
 * ---------------------------------------------------------------- */

#define ED_MAX    12288     /* the object size limit */
#define ED_ROWS   22        /* text rows; row 23 is the status bar */
#define ED_WIDTH  79        /* never touch column 80: writing it would wrap */
#define CLIP_MAX  1024
#define OPS_MAX   1024

struct op {
    uint16_t pos;
    uint8_t ch;
    uint8_t kg; /* bit 7: 1 = insert, 0 = delete; low 7 bits: undo group */
};

struct ed {
    char *text;
    int len;
    int cur;
    int top;          /* first displayed line number */
    int hscroll;
    int want_col;
    int modified;
    int quit_armed;
    int gutter;       /* 1 = show line numbers */
    int id;           /* the file's object id */
    int clip_id;      /* ".clipboard" object id, or -1 */
    int clip_len;
    char *clip;
    struct op *ops;
    int nops;         /* ops [0, nops) are applied */
    int nredo;        /* ops [nops, nredo) can be redone */
    uint8_t group;
    int last_typed;   /* 1 while a run of typed characters is being grouped */
    char msg[72];
    char find[40];
    char name[28];
    uint32_t rowhash[ED_ROWS + 1];
};

/* ---- small helpers (no library, no globals) ---- */
static void ed_msg(struct ed *e, const char *m) {
    int i = 0;
    while (m[i] && i < 70) { e->msg[i] = m[i]; i++; }
    e->msg[i] = 0;
}

static int ed_ls(struct ed *e, int p) {
    while (p > 0 && e->text[p - 1] != '\n') { p--; }
    return p;
}

static int ed_le(struct ed *e, int p) {
    while (p < e->len && e->text[p] != '\n') { p++; }
    return p;
}

static int ed_line_no(struct ed *e, int p) {
    int n = 0;
    for (int i = 0; i < p; i++) {
        if (e->text[i] == '\n') { n++; }
    }
    return n;
}

static int ed_line_start(struct ed *e, int n) {
    int p = 0;
    while (n > 0) {
        while (p < e->len && e->text[p] != '\n') { p++; }
        if (p < e->len) { p++; } else { break; }
        n--;
    }
    return p;
}

static int ed_line_count(struct ed *e) {
    return ed_line_no(e, e->len) + 1;
}

/* ---- the buffer: every change goes through these two, which also log it ---- */
static void ed_push(struct ed *e, int is_insert, int pos, char ch) {
    if (e->nops >= OPS_MAX) {                     /* log full: forget the oldest quarter */
        int drop = OPS_MAX / 4;
        for (int i = 0; i + drop < e->nops; i++) { e->ops[i] = e->ops[i + drop]; }
        e->nops -= drop;
    }
    struct op *o = &e->ops[e->nops++];
    o->pos = (uint16_t)pos;
    o->ch = (uint8_t)ch;
    o->kg = (uint8_t)((is_insert ? 0x80 : 0) | (e->group & 0x7F));
    e->nredo = e->nops; /* a new edit discards the redo tail */
}

static int ed_insert_raw(struct ed *e, int pos, char ch) {
    if (e->len >= ED_MAX) { return 0; }
    for (int i = e->len; i > pos; i--) { e->text[i] = e->text[i - 1]; }
    e->text[pos] = ch;
    e->len++;
    return 1;
}

static char ed_delete_raw(struct ed *e, int pos) {
    char c = e->text[pos];
    for (int i = pos; i < e->len - 1; i++) { e->text[i] = e->text[i + 1]; }
    e->len--;
    return c;
}

static int ed_insert(struct ed *e, int pos, char ch) {
    if (!ed_insert_raw(e, pos, ch)) {
        ed_msg(e, "The file is full (12288 bytes).");
        return 0;
    }
    ed_push(e, 1, pos, ch);
    e->modified = 1;
    return 1;
}

static void ed_delete(struct ed *e, int pos) {
    char c = ed_delete_raw(e, pos);
    ed_push(e, 0, pos, c);
    e->modified = 1;
}

static void ed_new_group(struct ed *e) {
    e->group = (uint8_t)((e->group + 1) & 0x7F);
    e->last_typed = 0;
}

static void ed_undo(struct ed *e) {
    if (e->nops == 0) { ed_msg(e, "Nothing to undo."); return; }
    int g = e->ops[e->nops - 1].kg & 0x7F;
    while (e->nops > 0 && (e->ops[e->nops - 1].kg & 0x7F) == g) {
        struct op *o = &e->ops[--e->nops];
        if (o->kg & 0x80) {           /* it was an insert: remove it */
            ed_delete_raw(e, o->pos);
            e->cur = o->pos;
        } else {                       /* it was a delete: put it back */
            ed_insert_raw(e, o->pos, (char)o->ch);
            e->cur = o->pos + 1;
        }
    }
    e->modified = 1;
    e->last_typed = 0;
}

static void ed_redo(struct ed *e) {
    if (e->nops >= e->nredo) { ed_msg(e, "Nothing to redo."); return; }
    int g = e->ops[e->nops].kg & 0x7F;
    while (e->nops < e->nredo && (e->ops[e->nops].kg & 0x7F) == g) {
        struct op *o = &e->ops[e->nops++];
        if (o->kg & 0x80) {
            ed_insert_raw(e, o->pos, (char)o->ch);
            e->cur = o->pos + 1;
        } else {
            ed_delete_raw(e, o->pos);
            e->cur = o->pos;
        }
    }
    e->modified = 1;
    e->last_typed = 0;
}

/* ---- drawing ---- */
static uint32_t ed_hash(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h = (h ^ (uint8_t)s[i]) * 16777619u; }
    return h;
}

static void ed_at(char *out, int *n, int row, int col) {
    out[(*n)++] = 27;
    out[(*n)++] = '[';
    if (row >= 10) { out[(*n)++] = (char)('0' + row / 10); }
    out[(*n)++] = (char)('0' + row % 10);
    out[(*n)++] = ';';
    if (col >= 10) { out[(*n)++] = (char)('0' + col / 10); }
    out[(*n)++] = (char)('0' + col % 10);
    out[(*n)++] = 'H';
}

static void ed_status(struct ed *e, const char *prompt, const char *typed) {
    char out[128];
    int n = 0;
    ed_at(out, &n, ED_ROWS + 1, 1);
    out[n++] = 27; out[n++] = '['; out[n++] = '3'; out[n++] = '0'; out[n++] = ';';
    out[n++] = '4'; out[n++] = '7'; out[n++] = 'm';
    int start = n;
    if (prompt) {
        for (int i = 0; prompt[i] && n - start < ED_WIDTH; i++) { out[n++] = prompt[i]; }
        for (int i = 0; typed[i] && n - start < ED_WIDTH; i++) { out[n++] = typed[i]; }
    } else {
        out[n++] = ' ';
        for (int i = 0; e->name[i] && n - start < 24; i++) { out[n++] = e->name[i]; }
        if (e->modified) { out[n++] = ' '; out[n++] = '['; out[n++] = '+'; out[n++] = ']'; }
        out[n++] = ' '; out[n++] = ' ';
        int line = ed_line_no(e, e->cur) + 1;
        int col = e->cur - ed_ls(e, e->cur) + 1;
        out[n++] = 'L'; out[n++] = 'n'; out[n++] = ' ';
        char num[8];
        int k = 0;
        for (int v = line; v > 0 && k < 7; v /= 10) { num[k++] = (char)('0' + v % 10); }
        if (k == 0) { num[k++] = '0'; }
        while (k > 0) { out[n++] = num[--k]; }
        out[n++] = ','; out[n++] = ' '; out[n++] = 'C'; out[n++] = 'o'; out[n++] = 'l'; out[n++] = ' ';
        k = 0;
        for (int v = col; v > 0 && k < 7; v /= 10) { num[k++] = (char)('0' + v % 10); }
        if (k == 0) { num[k++] = '0'; }
        while (k > 0) { out[n++] = num[--k]; }
        out[n++] = ' '; out[n++] = ' ';
        for (int i = 0; e->msg[i] && n - start < ED_WIDTH; i++) { out[n++] = e->msg[i]; }
    }
    while (n - start < ED_WIDTH) { out[n++] = ' '; }
    out[n++] = 27; out[n++] = '['; out[n++] = '0'; out[n++] = 'm';
    out[n] = 0;
    user_write(out);
}

static void ed_draw(struct ed *e) {
    /* keep the cursor on screen */
    int curl = ed_line_no(e, e->cur);
    if (curl < e->top) { e->top = curl; }
    if (curl >= e->top + ED_ROWS) { e->top = curl - ED_ROWS + 1; }
    int gw = e->gutter ? 5 : 0;
    int textw = ED_WIDTH - gw;
    int col = e->cur - ed_ls(e, e->cur);
    if (col < e->hscroll) { e->hscroll = col; }
    if (col >= e->hscroll + textw) { e->hscroll = col - textw + 1; }

    int p = ed_line_start(e, e->top);
    int lines = ed_line_count(e);
    char batch[500];   /* rows changed since the last flush: one console write carries up to 5 */
    int bn = 0;
    for (int r = 0; r < ED_ROWS; r++) {
        char row[ED_WIDTH + 1];
        int ln = e->top + r;
        for (int i = 0; i < ED_WIDTH; i++) { row[i] = ' '; }
        int rowlen = ED_WIDTH;
        if (ln < lines) {
            if (gw) {
                int v = ln + 1;
                for (int k = 3; k >= 0 && v > 0; k--) { row[k] = (char)('0' + v % 10); v /= 10; }
            }
            int end = ed_le(e, p);
            for (int i = 0; i < textw; i++) {
                int q = p + e->hscroll + i;
                if (q >= end) { break; }
                char c = e->text[q];
                row[gw + i] = (c >= 0x20 && c <= 0x7E) ? c : '.';
            }
            p = end < e->len ? end + 1 : end;
        } else {
            row[0] = '~';
        }
        uint32_t h = ed_hash(row, rowlen);
        if (h != e->rowhash[r]) {
            e->rowhash[r] = h;
            if (bn + 90 > 499) { batch[bn] = 0; user_write(batch); bn = 0; }
            ed_at(batch, &bn, r + 1, 1);
            for (int i = 0; i < ED_WIDTH; i++) { batch[bn++] = row[i]; }
        }
    }
    if (bn) { batch[bn] = 0; user_write(batch); }
    ed_status(e, 0, 0);
    char cur[16];
    int n = 0;
    ed_at(cur, &n, curl - e->top + 1, gw + col - e->hscroll + 1);
    cur[n] = 0;
    user_write(cur);
}

/* ---- prompts on the status bar ---- */
static int ed_prompt(struct ed *e, const char *label, char *buf, int max) {
    int len = 0;
    buf[0] = 0;
    for (;;) {
        ed_status(e, label, buf);
        char cur[16];
        int n = 0;
        int lp = 0;
        while (label[lp]) { lp++; }
        ed_at(cur, &n, ED_ROWS + 1, lp + len + 1);
        cur[n] = 0;
        user_write(cur);
        int c = user_key_read();
        if (c < 0) { user_sleep(1); continue; }
        if (c == '\n' || c == '\r') { return 1; }
        if (c == 27) { buf[0] = 0; return 0; }
        if ((c == '\b' || c == 0x7F) && len > 0) { buf[--len] = 0; }
        else if (c >= 0x20 && c < 0x7F && len < max - 1) { buf[len++] = (char)c; buf[len] = 0; }
    }
}

/* ---- search ---- */
static int ed_match_at(struct ed *e, int p, const char *pat, int plen) {
    if (p + plen > e->len) { return 0; }
    for (int i = 0; i < plen; i++) {
        if (e->text[p + i] != pat[i]) { return 0; }
    }
    return 1;
}

static int ed_find_from(struct ed *e, int from) {
    int plen = 0;
    while (e->find[plen]) { plen++; }
    if (plen == 0) { return -1; }
    for (int pass = 0; pass < 2; pass++) {
        int start = pass == 0 ? from : 0;
        int stop = pass == 0 ? e->len : from;
        for (int p = start; p < stop; p++) {
            if (ed_match_at(e, p, e->find, plen)) { return p; }
        }
    }
    return -1;
}

static void ed_replace_all(struct ed *e, const char *with) {
    int plen = 0, wlen = 0, count = 0;
    while (e->find[plen]) { plen++; }
    while (with[wlen]) { wlen++; }
    if (plen == 0) { return; }
    ed_new_group(e);
    int p = 0;
    while (p + plen <= e->len) {
        if (ed_match_at(e, p, e->find, plen)) {
            if (e->len - plen + wlen > ED_MAX) { ed_msg(e, "Stopped: file would exceed 12288 bytes."); break; }
            for (int i = 0; i < plen; i++) { ed_delete(e, p); }
            for (int i = 0; i < wlen; i++) { ed_insert(e, p + i, with[i]); }
            p += wlen;
            count++;
        } else {
            p++;
        }
    }
    e->cur = e->cur > e->len ? e->len : e->cur;
    char m[40];
    int n = 0;
    m[n++] = 'R'; m[n++] = 'e'; m[n++] = 'p'; m[n++] = 'l'; m[n++] = 'a'; m[n++] = 'c'; m[n++] = 'e'; m[n++] = 'd'; m[n++] = ' ';
    char num[8];
    int k = 0;
    for (int v = count; v > 0 && k < 7; v /= 10) { num[k++] = (char)('0' + v % 10); }
    if (k == 0) { num[k++] = '0'; }
    while (k > 0) { m[n++] = num[--k]; }
    m[n] = 0;
    ed_msg(e, m);
    ed_new_group(e);
}

/* ---- clipboard ---- */
static void ed_clip_store(struct ed *e, int from, int to) {
    int n = to - from;
    if (n > CLIP_MAX) { n = CLIP_MAX; }
    for (int i = 0; i < n; i++) { e->clip[i] = e->text[from + i]; }
    e->clip_len = n;
    if (e->clip_id >= 0) {
        user_object_write_at(e->clip_id, 0, e->clip, (uint32_t)n);
        user_object_write_at(e->clip_id, (uint32_t)n, e->clip, 0); /* truncate to n */
    }
}

/* ---- saving ---- */
static int ed_save(struct ed *e) {
    int rc = 0;
    if (e->len > 0) {
        rc = user_object_write_at(e->id, 0, e->text, (uint32_t)e->len);
    }
    if (rc >= 0) {
        rc = user_object_write_at(e->id, (uint32_t)e->len, e->text, 0); /* truncate to the new length */
    }
    if (rc < 0) {
        ed_msg(e, "SAVE FAILED: no write authority or read-only.");
        return 0;
    }
    e->modified = 0;
    ed_msg(e, "Saved.");
    return 1;
}

UTIL_MAIN {
    char args[UTIL_ARGS_MAX];
    util_read_args(args, UTIL_ARGS_MAX);
    if (args[0] == 0) {
        user_write("usage: edit <name>\n");
        user_exit();
    }
    /* heap: the editor state, the text, the clipboard and the undo log */
    char *heap = (char *)user_heap_grow(5);
    if (!heap) {
        user_write("edit: could not get memory\n");
        user_exit();
    }
    struct ed *e = (struct ed *)heap;
    e->text = heap + 0x400;                       /* ED_MAX (+1) bytes: to 0x3401 */
    e->clip = heap + 0x3500;                      /* CLIP_MAX bytes: to 0x3900 */
    e->ops = (struct op *)(heap + 0x3900);        /* OPS_MAX * 4 bytes: to 0x4900 (5 pages = 0x5000) */
    e->len = 0; e->cur = 0; e->top = 0; e->hscroll = 0; e->want_col = 0;
    e->modified = 0; e->quit_armed = 0; e->gutter = 1;
    e->nops = 0; e->nredo = 0; e->group = 1; e->last_typed = 0;
    e->clip_len = 0; e->msg[0] = 0; e->find[0] = 0;
    for (int i = 0; i < ED_ROWS + 1; i++) { e->rowhash[i] = 0; }
    int nl = 0;
    while (args[nl] && nl < 27) { e->name[nl] = args[nl]; nl++; }
    e->name[nl] = 0;

    e->id = user_lookup_name(args);
    if (e->id >= 0) {
        int n = user_object_read_at(e->id, 0, e->text, ED_MAX);
        if (n < 0) {
            user_write("edit: permission denied (not given authority over that file)\n");
            user_exit();
        }
        e->len = n;
        ed_msg(e, "^S save ^Q quit ^Z undo ^F find ^R replace ^K cut ^V paste");
    } else {
        e->id = user_create_name(args);
        if (e->id < 0) {
            user_write("edit: cannot create that file (name too long, or creation quota spent)\n");
            user_exit();
        }
        ed_msg(e, "New file. ^S save ^Q quit ^Z undo ^F find");
    }
    e->clip_id = user_lookup_name(".clipboard");
    if (e->clip_id >= 0) {
        int n = user_object_read_at(e->clip_id, 0, e->clip, CLIP_MAX);
        if (n > 0) { e->clip_len = n; } else if (n < 0) { e->clip_id = -1; }
    }

    user_write("\x1b[0m\x1b[2J\x1b[1;1H");
    for (;;) {
        ed_draw(e);
        int c;
        while ((c = user_key_read()) < 0) { user_sleep(1); }
        e->msg[0] = 0;
        int was_quit_armed = e->quit_armed;
        e->quit_armed = 0;

        if (c == 17) {                         /* Ctrl-Q */
            if (e->modified && !was_quit_armed) {
                e->quit_armed = 1;
                ed_msg(e, "Unsaved changes! ^Q again to discard and quit.");
                continue;
            }
            break;
        } else if (c == 19) {                  /* Ctrl-S */
            ed_new_group(e);
            ed_save(e);
        } else if (c == 26) {                  /* Ctrl-Z */
            ed_undo(e);
        } else if (c == 25) {                  /* Ctrl-Y */
            ed_redo(e);
        } else if (c == 12) {                  /* Ctrl-L */
            e->gutter = !e->gutter;
            for (int i = 0; i < ED_ROWS; i++) { e->rowhash[i] = 0; }
        } else if (c == 6 || c == 7) {         /* Ctrl-F, Ctrl-G */
            if (c == 6) {
                char pat[40];
                if (!ed_prompt(e, "Find: ", pat, 40) || pat[0] == 0) { continue; }
                int i = 0;
                while (pat[i]) { e->find[i] = pat[i]; i++; }
                e->find[i] = 0;
            } else if (e->find[0] == 0) {
                ed_msg(e, "Nothing to find yet (^F).");
                continue;
            }
            int at = ed_find_from(e, c == 6 ? e->cur : e->cur + 1);
            if (at < 0) { ed_msg(e, "Not found."); }
            else { e->cur = at; ed_msg(e, "Found. ^G = next."); }
            ed_new_group(e);
        } else if (c == 18) {                  /* Ctrl-R: replace all */
            char pat[40];
            char with[40];
            if (!ed_prompt(e, "Replace: ", pat, 40) || pat[0] == 0) { continue; }
            if (!ed_prompt(e, "With: ", with, 40)) { continue; }
            int i = 0;
            while (pat[i]) { e->find[i] = pat[i]; i++; }
            e->find[i] = 0;
            ed_replace_all(e, with);
        } else if (c == 20) {                  /* Ctrl-T: go to line */
            char num[12];
            if (!ed_prompt(e, "Go to line: ", num, 12)) { continue; }
            int v = 0;
            for (int i = 0; num[i] >= '0' && num[i] <= '9'; i++) { v = v * 10 + (num[i] - '0'); }
            if (v < 1) { v = 1; }
            int lines = ed_line_count(e);
            if (v > lines) { v = lines; }
            e->cur = ed_line_start(e, v - 1);
            ed_new_group(e);
        } else if (c == 3 || c == 11) {        /* Ctrl-C copy line, Ctrl-K cut line */
            int a = ed_ls(e, e->cur);
            int b = ed_le(e, e->cur);
            if (b < e->len) { b++; }           /* include the newline */
            ed_clip_store(e, a, b);
            if (c == 11) {
                ed_new_group(e);
                for (int i = 0; i < b - a; i++) { ed_delete(e, a); }
                e->cur = a > e->len ? e->len : a;
                ed_new_group(e);
                ed_msg(e, "Cut.");
            } else {
                ed_msg(e, "Copied.");
            }
        } else if (c == 22) {                  /* Ctrl-V */
            if (e->clip_len == 0) { ed_msg(e, "The clipboard is empty."); continue; }
            ed_new_group(e);
            for (int i = 0; i < e->clip_len; i++) {
                if (!ed_insert(e, e->cur, e->clip[i])) { break; }
                e->cur++;
            }
            ed_new_group(e);
        } else if (c == KEY_LEFT) {
            if (e->cur > 0) { e->cur--; }
            ed_new_group(e);
            e->want_col = e->cur - ed_ls(e, e->cur);
        } else if (c == KEY_RIGHT) {
            if (e->cur < e->len) { e->cur++; }
            ed_new_group(e);
            e->want_col = e->cur - ed_ls(e, e->cur);
        } else if (c == KEY_HOME || c == 1) {
            e->cur = ed_ls(e, e->cur);
            e->want_col = 0;
            ed_new_group(e);
        } else if (c == KEY_END || c == 5) {
            e->cur = ed_le(e, e->cur);
            e->want_col = e->cur - ed_ls(e, e->cur);
            ed_new_group(e);
        } else if (c == KEY_UP || c == KEY_DOWN || c == KEY_PGUP || c == KEY_PGDN) {
            int steps = (c == KEY_PGUP || c == KEY_PGDN) ? ED_ROWS - 2 : 1;
            int down = (c == KEY_DOWN || c == KEY_PGDN);
            int line = ed_line_no(e, e->cur);
            int last = ed_line_count(e) - 1;
            line += down ? steps : -steps;
            if (line < 0) { line = 0; }
            if (line > last) { line = last; }
            int s = ed_line_start(e, line);
            int end = ed_le(e, s);
            e->cur = (s + e->want_col <= end) ? s + e->want_col : end;
            ed_new_group(e);
        } else if (c == KEY_DELETE) {
            if (e->cur < e->len) { ed_delete(e, e->cur); }
            ed_new_group(e);
        } else if (c == '\b' || c == 0x7F) {
            if (e->cur > 0) { e->cur--; ed_delete(e, e->cur); }
            ed_new_group(e);
            e->want_col = e->cur - ed_ls(e, e->cur);
        } else if (c == '\n' || c == '\r') {
            ed_new_group(e);
            if (ed_insert(e, e->cur, '\n')) { e->cur++; }
            ed_new_group(e);
            e->want_col = 0;
        } else if (c == '\t') {
            ed_new_group(e);
            for (int i = 0; i < 4; i++) {
                if (ed_insert(e, e->cur, ' ')) { e->cur++; }
            }
            ed_new_group(e);
            e->want_col = e->cur - ed_ls(e, e->cur);
        } else if (c >= 0x20 && c < 0x7F) {
            if (!e->last_typed) { ed_new_group(e); }
            e->last_typed = 1;
            if (ed_insert(e, e->cur, (char)c)) { e->cur++; }
            e->want_col = e->cur - ed_ls(e, e->cur);
        }
    }
    user_write("\x1b[0m\x1b[2J\x1b[1;1H");
    user_write("edit: closed '");
    user_write(args);
    user_write("'\n");
    user_exit();
}
