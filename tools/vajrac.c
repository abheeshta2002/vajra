/* ------------------------------------------------------------------
 * vajrac -- the VajraLang compiler.
 *
 * A genuine front end (lexer, recursive-descent parser, a small AST)
 * for a tiny calculator language, targeting C as its backend: codegen
 * emits a .c file written against src/userland/runtime.h's own
 * syscall wrappers, which then goes through the EXACT SAME clang +
 * ld.lld + program_header pipeline tools/build-c.ps1 already uses for
 * src/userland/hello.c -- the real, already-verified path from a flat
 * binary to a loaded, running Vajra actor (include/vajra/loader.h).
 * Targeting C instead of hand-emitted x86-64 machine code is a
 * completely ordinary compiler architecture (early C++, Nim, and
 * plenty of production languages do exactly this) -- it means this
 * front end doesn't have to reinvent register allocation or
 * instruction selection to get a real, working VajraLang program
 * running on real (emulated) hardware.
 *
 * Language, v0:
 *   program    := statement*
 *   statement  := "let" IDENT "=" expr ";"
 *               | "print" expr ";"
 *   expr       := term (("+" | "-") term)*
 *   term       := unary (("*" | "/") unary)*
 *   unary      := "-" unary | primary
 *   primary    := NUMBER | IDENT | "(" expr ")"
 *
 * Integers only (Vajra's calculator programs use plain `long long`
 * arithmetic -- see runtime.c's own user_write_int()). Variables are
 * plain C locals in the generated _start(): no real symbol table
 * needed at this scale, just a check that a name was declared with
 * `let` before it's read, so a typo is a compile error here, not a
 * silent zero at runtime.
 *
 * This host tool builds and runs on the DEVELOPMENT machine (plain
 * native clang, not the -target x86_64-elf freestanding flags
 * tools/build-c.ps1 uses for anything that ends up running inside
 * Vajra itself) -- it's a compiler FOR Vajra, not a program that runs
 * ON Vajra. Self-hosting (moving vajrac itself into a Vajra actor) is
 * real future work, not attempted here.
 * ---------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Lexer ---- */

typedef enum {
    TOK_NUM, TOK_IDENT, TOK_LET, TOK_PRINT,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH,
    TOK_LPAREN, TOK_RPAREN, TOK_EQUALS, TOK_SEMI, TOK_EOF
} TokKind;

typedef struct {
    TokKind kind;
    long long num;
    char ident[64];
    int line;
} Token;

static const char *src;
static int src_pos;
static int src_line = 1;

static void lex_skip_ws(void) {
    for (;;) {
        char c = src[src_pos];
        if (c == '\n') { src_line++; src_pos++; }
        else if (c == ' ' || c == '\t' || c == '\r') { src_pos++; }
        else if (c == '#') { while (src[src_pos] && src[src_pos] != '\n') { src_pos++; } }
        else { break; }
    }
}

static Token lex_next(void) {
    lex_skip_ws();
    Token t;
    t.line = src_line;
    char c = src[src_pos];

    if (c == '\0') { t.kind = TOK_EOF; return t; }

    if (isdigit((unsigned char)c)) {
        long long v = 0;
        while (isdigit((unsigned char)src[src_pos])) {
            v = v * 10 + (src[src_pos] - '0');
            src_pos++;
        }
        t.kind = TOK_NUM;
        t.num = v;
        return t;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        int i = 0;
        while (isalnum((unsigned char)src[src_pos]) || src[src_pos] == '_') {
            if (i < 63) { t.ident[i++] = src[src_pos]; }
            src_pos++;
        }
        t.ident[i] = 0;
        if (strcmp(t.ident, "let") == 0) { t.kind = TOK_LET; }
        else if (strcmp(t.ident, "print") == 0) { t.kind = TOK_PRINT; }
        else { t.kind = TOK_IDENT; }
        return t;
    }

    src_pos++;
    switch (c) {
        case '+': t.kind = TOK_PLUS; return t;
        case '-': t.kind = TOK_MINUS; return t;
        case '*': t.kind = TOK_STAR; return t;
        case '/': t.kind = TOK_SLASH; return t;
        case '(': t.kind = TOK_LPAREN; return t;
        case ')': t.kind = TOK_RPAREN; return t;
        case '=': t.kind = TOK_EQUALS; return t;
        case ';': t.kind = TOK_SEMI; return t;
        default:
            fprintf(stderr, "vajrac: line %d: unexpected character '%c'\n", src_line, c);
            exit(1);
    }
    return t;
}

/* ---- AST ---- */

typedef enum { NK_NUM, NK_VAR, NK_NEG, NK_BIN } NodeKind;

typedef struct Node {
    NodeKind kind;
    long long num;        /* NK_NUM */
    char var[64];          /* NK_VAR */
    char op;                /* NK_BIN: '+','-','*','/' */
    struct Node *lhs, *rhs; /* NK_BIN: both; NK_NEG: lhs only */
} Node;

typedef enum { SK_LET, SK_PRINT } StmtKind;

typedef struct Stmt {
    StmtKind kind;
    char var[64];   /* SK_LET */
    Node *expr;
    struct Stmt *next;
} Stmt;

static Node *node_new(NodeKind k) {
    Node *n = calloc(1, sizeof(Node));
    n->kind = k;
    return n;
}

/* ---- Parser (recursive descent, one token of lookahead) ---- */

static Token cur;
static char declared[64][64];
static int declared_count;

static void advance(void) { cur = lex_next(); }

static void expect(TokKind k, const char *what) {
    if (cur.kind != k) {
        fprintf(stderr, "vajrac: line %d: expected %s\n", cur.line, what);
        exit(1);
    }
}

static int is_declared(const char *name) {
    for (int i = 0; i < declared_count; i++) {
        if (strcmp(declared[i], name) == 0) { return 1; }
    }
    return 0;
}

static Node *parse_expr(void);

static Node *parse_primary(void) {
    if (cur.kind == TOK_NUM) {
        Node *n = node_new(NK_NUM);
        n->num = cur.num;
        advance();
        return n;
    }
    if (cur.kind == TOK_IDENT) {
        if (!is_declared(cur.ident)) {
            fprintf(stderr, "vajrac: line %d: '%s' used before it was declared with 'let'\n",
                    cur.line, cur.ident);
            exit(1);
        }
        Node *n = node_new(NK_VAR);
        strncpy(n->var, cur.ident, sizeof(n->var) - 1);
        advance();
        return n;
    }
    if (cur.kind == TOK_LPAREN) {
        advance();
        Node *n = parse_expr();
        expect(TOK_RPAREN, "')'");
        advance();
        return n;
    }
    fprintf(stderr, "vajrac: line %d: expected a number, a variable, or '('\n", cur.line);
    exit(1);
}

static Node *parse_unary(void) {
    if (cur.kind == TOK_MINUS) {
        advance();
        Node *n = node_new(NK_NEG);
        n->lhs = parse_unary();
        return n;
    }
    return parse_primary();
}

static Node *parse_term(void) {
    Node *n = parse_unary();
    while (cur.kind == TOK_STAR || cur.kind == TOK_SLASH) {
        char op = (cur.kind == TOK_STAR) ? '*' : '/';
        advance();
        Node *rhs = parse_unary();
        Node *bin = node_new(NK_BIN);
        bin->op = op;
        bin->lhs = n;
        bin->rhs = rhs;
        n = bin;
    }
    return n;
}

static Node *parse_expr(void) {
    Node *n = parse_term();
    while (cur.kind == TOK_PLUS || cur.kind == TOK_MINUS) {
        char op = (cur.kind == TOK_PLUS) ? '+' : '-';
        advance();
        Node *rhs = parse_term();
        Node *bin = node_new(NK_BIN);
        bin->op = op;
        bin->lhs = n;
        bin->rhs = rhs;
        n = bin;
    }
    return n;
}

static Stmt *parse_program(void) {
    Stmt *head = NULL, *tail = NULL;
    advance();
    while (cur.kind != TOK_EOF) {
        Stmt *s = calloc(1, sizeof(Stmt));
        if (cur.kind == TOK_LET) {
            advance();
            expect(TOK_IDENT, "a variable name");
            strncpy(s->var, cur.ident, sizeof(s->var) - 1);
            s->kind = SK_LET;
            advance();
            expect(TOK_EQUALS, "'='");
            advance();
            s->expr = parse_expr();
            expect(TOK_SEMI, "';'");
            advance();
            if (declared_count < 64) {
                strncpy(declared[declared_count++], s->var, 63);
            }
        } else if (cur.kind == TOK_PRINT) {
            advance();
            s->kind = SK_PRINT;
            s->expr = parse_expr();
            expect(TOK_SEMI, "';'");
            advance();
        } else {
            fprintf(stderr, "vajrac: line %d: expected 'let' or 'print'\n", cur.line);
            exit(1);
        }
        if (!head) { head = tail = s; } else { tail->next = s; tail = s; }
    }
    return head;
}

/* ---- Codegen: AST -> a C expression string, then a full _start() ---- */

static void gen_expr(FILE *out, Node *n) {
    switch (n->kind) {
        case NK_NUM: fprintf(out, "%lldLL", n->num); return;
        case NK_VAR: fprintf(out, "%s", n->var); return;
        case NK_NEG:
            fprintf(out, "(-");
            gen_expr(out, n->lhs);
            fprintf(out, ")");
            return;
        case NK_BIN:
            fprintf(out, "(");
            gen_expr(out, n->lhs);
            fprintf(out, " %c ", n->op);
            gen_expr(out, n->rhs);
            fprintf(out, ")");
            return;
    }
}

static void codegen(FILE *out, Stmt *program) {
    fprintf(out, "/* Generated by tools/vajrac.c -- do not edit by hand. */\n");
    fprintf(out, "#include \"runtime.h\"\n\n");
    fprintf(out, "__attribute__((section(\".text.start\")))\n");
    fprintf(out, "void _start(void) {\n");
    for (Stmt *s = program; s; s = s->next) {
        if (s->kind == SK_LET) {
            fprintf(out, "    long long %s = ", s->var);
            gen_expr(out, s->expr);
            fprintf(out, ";\n");
        } else {
            fprintf(out, "    user_write_int(");
            gen_expr(out, s->expr);
            fprintf(out, ");\n    user_write(\"\\n\");\n");
        }
    }
    fprintf(out, "    user_exit();\n}\n");
}

/* ---- Driver ---- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "vajrac: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: vajrac <input.vj> <output.c>\n");
        return 1;
    }
    src = read_file(argv[1]);
    src_pos = 0;

    Stmt *program = parse_program();

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "vajrac: cannot write '%s'\n", argv[2]); return 1; }
    codegen(out, program);
    fclose(out);

    fprintf(stderr, "vajrac: compiled '%s' -> '%s'\n", argv[1], argv[2]);
    return 0;
}
