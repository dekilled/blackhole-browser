/* JavaScript lexer + recursive-descent parser (ES2022 subset, see docs/JS_ENGINE.md). */
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "js.h"

#define MAX_DEPTH 300

typedef enum { T_EOF, T_NUM, T_BIGINT, T_STR, T_TEMPLATE, T_IDENT, T_KEYWORD, T_PUNCT, T_PRIVATE } TokType;

typedef struct {
    TokType     type;
    const char *s, *s2; /* identifier/keyword/string text; regex flags */
    size_t      len;
    double      num;
    char        p[5]; /* punctuator */
    bool        nl;   /* a line terminator precedes this token */
    int         line, col;
    size_t      start;
} Tok;

typedef struct {
    Arena      *a;
    const char *src;
    size_t      len, pos;
    int         line;
    size_t      line_start;
    Tok         tok;
    jmp_buf     jb;
    char        err[256];
    int         eline, ecol;
    int         depth, nodes;
    bool        in_async, in_gen, in_func;
} P;

/* ================================================================== errors == */
static void fail_at(P *p, int line, int col, const char *fmt, ...) __attribute__((noreturn, format(printf, 4, 5)));
static void fail_at(P *p, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
    p->eline = line;
    p->ecol = col;
    longjmp(p->jb, 1);
}

#define FAIL(p, ...) fail_at((p), (p)->tok.line, (p)->tok.col, __VA_ARGS__)
#define LEXFAIL(p, ...) fail_at((p), (p)->line, (int)((p)->pos - (p)->line_start) + 1, __VA_ARGS__)

/* ================================================================== lexer ==== */
static bool id_start(unsigned char c) { return is_alpha(c) || c == '$' || c == '_' || c >= 0x80; }
static bool id_part(unsigned char c) { return id_start(c) || is_digit(c); }

static const char *KEYWORDS[] = {"break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else",
                                 "enum", "export", "extends", "false", "finally", "for", "function", "if", "import", "in",
                                 "instanceof", "new", "null", "return", "super", "switch", "this", "throw", "true", "try", "typeof",
                                 "var", "void", "while", "with", NULL};

static bool is_keyword(const char *s, size_t n) {
    for (int i = 0; KEYWORDS[i]; i++)
        if (strlen(KEYWORDS[i]) == n && !memcmp(KEYWORDS[i], s, n)) return true;
    return false;
}

static void newline(P *p, size_t after) {
    p->line++;
    p->line_start = after;
}

static void skip_ws(P *p, bool *nl) {
    const unsigned char *s = (const unsigned char *)p->src;
    if (p->pos == 0 && p->len >= 2 && s[0] == '#' && s[1] == '!') while (p->pos < p->len && s[p->pos] != '\n') p->pos++;
    while (p->pos < p->len) {
        unsigned char c = s[p->pos];
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') p->pos++;
        else if (c == '\n') { p->pos++; newline(p, p->pos); *nl = true; }
        else if (c == '\r') { p->pos++; if (p->pos < p->len && s[p->pos] == '\n') p->pos++; newline(p, p->pos); *nl = true; }
        else if (c == '/' && p->pos + 1 < p->len && s[p->pos + 1] == '/') {
            while (p->pos < p->len && s[p->pos] != '\n' && s[p->pos] != '\r') p->pos++;
        } else if (c == '/' && p->pos + 1 < p->len && s[p->pos + 1] == '*') {
            p->pos += 2;
            for (;;) {
                if (p->pos + 1 >= p->len) LEXFAIL(p, "comentário /* não terminado");
                if (s[p->pos] == '*' && s[p->pos + 1] == '/') { p->pos += 2; break; }
                if (s[p->pos] == '\n') { p->pos++; newline(p, p->pos); *nl = true; }
                else if (s[p->pos] == '\r') { p->pos++; if (s[p->pos] == '\n') p->pos++; newline(p, p->pos); *nl = true; }
                else p->pos++;
            }
        } else if (c == 0xC2 && p->pos + 1 < p->len && s[p->pos + 1] == 0xA0) p->pos += 2;                      /* NBSP */
        else if (c == 0xEF && p->pos + 2 < p->len && s[p->pos + 1] == 0xBB && s[p->pos + 2] == 0xBF) p->pos += 3; /* BOM */
        else if (c == 0xE2 && p->pos + 2 < p->len && s[p->pos + 1] == 0x80 && (s[p->pos + 2] == 0xA8 || s[p->pos + 2] == 0xA9)) {
            p->pos += 3; newline(p, p->pos); *nl = true; /* U+2028/2029 */
        } else if (c == 0xE2 && p->pos + 2 < p->len && s[p->pos + 1] == 0x80 && ((s[p->pos + 2] >= 0x80 && s[p->pos + 2] <= 0x8A) || s[p->pos + 2] == 0xAF)) p->pos += 3;
        else if (c == 0xE3 && p->pos + 2 < p->len && s[p->pos + 1] == 0x80 && s[p->pos + 2] == 0x80) p->pos += 3;
        else break;
    }
}

/* Reads one escape sequence (p->pos is at the backslash) and appends its UTF-8 value. */
static void read_escape(P *p, Buf *b, bool tmpl) {
    const char *s = p->src;
    p->pos++;
    if (p->pos >= p->len) LEXFAIL(p, "escape incompleto");
    char c = s[p->pos++];
    switch (c) {
    case 'n': buf_putc(b, '\n'); return;
    case 't': buf_putc(b, '\t'); return;
    case 'r': buf_putc(b, '\r'); return;
    case 'b': buf_putc(b, '\b'); return;
    case 'f': buf_putc(b, '\f'); return;
    case 'v': buf_putc(b, '\v'); return;
    case '\r': if (p->pos < p->len && s[p->pos] == '\n') p->pos++; newline(p, p->pos); return;
    case '\n': newline(p, p->pos); return;
    case 'x': {
        int h1 = p->pos + 1 < p->len ? hex_val((unsigned char)s[p->pos]) : -1, h2 = p->pos + 1 < p->len ? hex_val((unsigned char)s[p->pos + 1]) : -1;
        if (h1 < 0 || h2 < 0) { if (tmpl) { buf_putc(b, 'x'); return; } LEXFAIL(p, "escape \\x inválido"); }
        p->pos += 2;
        buf_put_utf8(b, (uint32_t)(h1 * 16 + h2));
        return;
    }
    case 'u': {
        uint32_t cp = 0;
        if (p->pos < p->len && s[p->pos] == '{') {
            p->pos++;
            int nd = 0;
            while (p->pos < p->len && hex_val((unsigned char)s[p->pos]) >= 0) { cp = cp * 16 + (uint32_t)hex_val((unsigned char)s[p->pos++]); nd++; if (cp > 0x10FFFF) LEXFAIL(p, "escape \\u{} fora do intervalo"); }
            if (!nd || p->pos >= p->len || s[p->pos] != '}') { if (tmpl) { buf_putc(b, 'u'); return; } LEXFAIL(p, "escape \\u{} inválido"); }
            p->pos++;
        } else {
            for (int i = 0; i < 4; i++) {
                int h = p->pos < p->len ? hex_val((unsigned char)s[p->pos]) : -1;
                if (h < 0) { if (tmpl) { buf_putc(b, 'u'); return; } LEXFAIL(p, "escape \\u inválido"); }
                cp = cp * 16 + (uint32_t)h;
                p->pos++;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && p->pos + 5 < p->len && s[p->pos] == '\\' && s[p->pos + 1] == 'u') { /* surrogate pair */
                uint32_t lo = 0;
                bool     ok = true;
                for (int i = 0; i < 4; i++) { int h = hex_val((unsigned char)s[p->pos + 2 + (size_t)i]); if (h < 0) { ok = false; break; } lo = lo * 16 + (uint32_t)h; }
                if (ok && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p->pos += 6; }
            }
        }
        buf_put_utf8(b, cp); /* lone surrogates become U+FFFD (limitation: strings are UTF-8) */
        return;
    }
    default:
        if (c >= '0' && c <= '7') { /* \0 and legacy octal */
            int v = c - '0', k = 1;
            while (k < 3 && p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '7' && v * 8 + (s[p->pos] - '0') < 256) { v = v * 8 + (s[p->pos++] - '0'); k++; }
            buf_put_utf8(b, (uint32_t)v);
            return;
        }
        buf_putc(b, c); /* \' \" \\ and any other char stand for themselves */
    }
}

static void lex_number(P *p, Tok *t) {
    const char *s = p->src;
    size_t      st = p->pos;
    t->type = T_NUM;
    if (s[p->pos] == '0' && p->pos + 1 < p->len && strchr("xXoObB", s[p->pos + 1])) {
        int base = (s[p->pos + 1] | 32) == 'x' ? 16 : (s[p->pos + 1] | 32) == 'o' ? 8 : 2;
        p->pos += 2;
        double v = 0;
        int    nd = 0;
        while (p->pos < p->len) {
            if (s[p->pos] == '_') { p->pos++; continue; }
            int d = hex_val((unsigned char)s[p->pos]);
            if (d < 0 || d >= base) break;
            v = v * base + d;
            nd++;
            p->pos++;
        }
        if (!nd) LEXFAIL(p, "número inválido");
        t->num = v;
    } else {
        Buf b;
        buf_init(&b);
        while (p->pos < p->len && (is_digit((unsigned char)s[p->pos]) || s[p->pos] == '_')) { if (s[p->pos] != '_') buf_putc(&b, s[p->pos]); p->pos++; }
        if (p->pos < p->len && s[p->pos] == '.') {
            buf_putc(&b, '.');
            p->pos++;
            while (p->pos < p->len && (is_digit((unsigned char)s[p->pos]) || s[p->pos] == '_')) { if (s[p->pos] != '_') buf_putc(&b, s[p->pos]); p->pos++; }
        }
        if (p->pos < p->len && (s[p->pos] | 32) == 'e') {
            size_t save = p->pos;
            Buf    e;
            buf_init(&e);
            buf_putc(&e, 'e');
            p->pos++;
            if (p->pos < p->len && (s[p->pos] == '+' || s[p->pos] == '-')) buf_putc(&e, s[p->pos++]);
            size_t nd = 0;
            while (p->pos < p->len && is_digit((unsigned char)s[p->pos])) { buf_putc(&e, s[p->pos++]); nd++; }
            if (!nd) { p->pos = save; buf_free(&e); }
            else { buf_append(&b, e.data, e.len); buf_free(&e); }
        }
        t->num = b.len ? strtod(b.data, NULL) : 0;
        buf_free(&b);
    }
    if (p->pos < p->len && s[p->pos] == 'n') { t->type = T_BIGINT; p->pos++; }
    if (p->pos < p->len && id_start((unsigned char)s[p->pos]) && s[p->pos] != '\\') LEXFAIL(p, "identificador logo após número");
    (void)st;
}

static const char *PUNCT4[] = {">>>=", NULL};
static const char *PUNCT3[] = {"===", "!==", "**=", "<<=", ">>=", ">>>", "...", "&&=", "||=", "?\?=", NULL};
static const char *PUNCT2[] = {"=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**", NULL};

static void lex(P *p, Tok *t) {
    memset(t, 0, sizeof *t);
    bool nl = false;
    skip_ws(p, &nl);
    t->nl = nl;
    t->start = p->pos;
    t->line = p->line;
    t->col = (int)(p->pos - p->line_start) + 1;
    if (p->pos >= p->len) { t->type = T_EOF; return; }
    const char *s = p->src;
    unsigned char c = (unsigned char)s[p->pos];

    if (id_start(c) || c == '\\') {
        Buf b;
        buf_init(&b);
        bool esc = false;
        while (p->pos < p->len) {
            unsigned char d = (unsigned char)s[p->pos];
            if (d == '\\' && p->pos + 1 < p->len && s[p->pos + 1] == 'u') { /* \uXXXX in identifiers */
                esc = true;
                read_escape(p, &b, false);
            } else if (id_part(d)) { buf_putc(&b, (char)d); p->pos++; }
            else break;
        }
        if (!b.len) LEXFAIL(p, "caractere inesperado '\\\\'");
        t->s = arena_strndup(p->a, b.data, b.len);
        t->len = b.len;
        t->type = (!esc && is_keyword(b.data, b.len)) ? T_KEYWORD : T_IDENT;
        buf_free(&b);
        return;
    }
    if (is_digit(c) || (c == '.' && p->pos + 1 < p->len && is_digit((unsigned char)s[p->pos + 1]))) { lex_number(p, t); return; }
    if (c == '"' || c == '\'') {
        char quote = (char)c;
        Buf  b;
        buf_init(&b);
        p->pos++;
        for (;;) {
            if (p->pos >= p->len) LEXFAIL(p, "string não terminada");
            char d = s[p->pos];
            if (d == quote) { p->pos++; break; }
            if (d == '\n' || d == '\r') LEXFAIL(p, "quebra de linha dentro de string");
            if (d == '\\') read_escape(p, &b, false);
            else { buf_putc(&b, d); p->pos++; }
        }
        t->type = T_STR;
        t->s = arena_strndup(p->a, b.data ? b.data : "", b.len);
        t->len = b.len;
        buf_free(&b);
        return;
    }
    if (c == '`') { p->pos++; t->type = T_TEMPLATE; return; }
    if (c == '#' && p->pos + 1 < p->len && id_start((unsigned char)s[p->pos + 1])) {
        size_t st = p->pos++;
        while (p->pos < p->len && id_part((unsigned char)s[p->pos])) p->pos++;
        t->type = T_PRIVATE;
        t->s = arena_strndup(p->a, s + st, p->pos - st);
        t->len = p->pos - st;
        return;
    }
    /* punctuators: longest match */
    t->type = T_PUNCT;
    for (int n = 4; n >= 1; n--) {
        if (p->pos + (size_t)n > p->len) continue;
        const char **tbl = n == 4 ? PUNCT4 : n == 3 ? PUNCT3 : n == 2 ? PUNCT2 : NULL;
        bool         hit = false;
        if (tbl) {
            for (int i = 0; tbl[i]; i++) if (!memcmp(s + p->pos, tbl[i], (size_t)n)) { hit = true; break; }
            if (hit && n == 2 && !memcmp(s + p->pos, "?.", 2) && p->pos + 2 < p->len && is_digit((unsigned char)s[p->pos + 2])) hit = false; /* a?.5:b */
        } else hit = strchr("{}()[];,<>+-*/%&|^!~?:=.@", (char)c) != NULL;
        if (hit) {
            memcpy(t->p, s + p->pos, (size_t)n);
            t->p[n] = 0;
            p->pos += (size_t)n;
            return;
        }
    }
    LEXFAIL(p, "caractere inesperado '%c'", (c >= 32 && c < 127) ? (char)c : '?');
}

static void next(P *p) { lex(p, &p->tok); }

static Tok peek(P *p) {
    size_t pos = p->pos, ls = p->line_start;
    int    line = p->line;
    Tok    t;
    lex(p, &t);
    p->pos = pos;
    p->line = line;
    p->line_start = ls;
    return t;
}

/* ================================================================ helpers ==== */
static bool is_p(const P *p, const char *s) { return p->tok.type == T_PUNCT && !strcmp(p->tok.p, s); }
static bool is_kw(const P *p, const char *s) { return p->tok.type == T_KEYWORD && !strcmp(p->tok.s, s); }
static bool is_id(const P *p, const char *s) { return p->tok.type == T_IDENT && !strcmp(p->tok.s, s); }
static bool tok_p(const Tok *t, const char *s) { return t->type == T_PUNCT && !strcmp(t->p, s); }

static bool eat_p(P *p, const char *s) {
    if (!is_p(p, s)) return false;
    next(p);
    return true;
}
static void expect_p(P *p, const char *s) {
    if (!is_p(p, s)) FAIL(p, "esperado '%s'", s);
    next(p);
}

static JsNode *mk(P *p, int type) {
    JsNode *n = arena_alloc(p->a, sizeof *n);
    n->type = (uint8_t)type;
    n->line = p->tok.line;
    n->col = p->tok.col;
    p->nodes++;
    return n;
}
static JsNode *mk_like(P *p, int type, const JsNode *at) {
    JsNode *n = mk(p, type);
    n->line = at->line;
    n->col = at->col;
    return n;
}

typedef struct {
    JsNode **v;
    int      n, cap;
} NV;

static void nv_push(P *p, NV *l, JsNode *n) {
    if (l->n == l->cap) {
        int      nc = l->cap ? l->cap * 2 : 4;
        JsNode **nv = arena_alloc(p->a, sizeof(JsNode *) * (size_t)nc);
        if (l->n) memcpy(nv, l->v, sizeof(JsNode *) * (size_t)l->n);
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = n;
}
static void nv_set(NV *l, JsNode *node) {
    node->list = l->v;
    node->n = l->n;
}

static void enter(P *p) {
    if (++p->depth > MAX_DEPTH) FAIL(p, "código aninhado demais (limite %d)", MAX_DEPTH);
}
static void leave(P *p) { p->depth--; }

static void consume_semicolon(P *p) {
    if (eat_p(p, ";")) return;
    if (is_p(p, "}") || p->tok.type == T_EOF || p->tok.nl) return;
    FAIL(p, "esperado ';' (token inesperado)");
}

static JsNode *parse_assign(P *p, bool no_in);
static JsNode *parse_expr(P *p, bool no_in);
static JsNode *parse_statement(P *p);
static JsNode *parse_unary(P *p);
static JsNode *parse_function_rest(P *p, JsNode *fn);
static JsNode *parse_class(P *p, bool decl);
static JsNode *parse_binding_target(P *p);
static JsNode *parse_binding_element(P *p);
static JsNode *parse_block(P *p);

static JsNode *to_pattern(P *p, JsNode *n) {
    switch (n->type) {
    case J_IDENT: case J_MEMBER: case J_INDEX: case J_ASSIGN_PAT: case J_ARRAY_PAT: case J_OBJECT_PAT: return n;
    case J_ARRAY:
        n->type = J_ARRAY_PAT;
        for (int i = 0; i < n->n; i++) {
            JsNode *e = n->list[i];
            if (e->type == J_HOLE) continue;
            if (e->type == J_SPREAD) { e->type = J_REST; e->a = to_pattern(p, e->a); }
            else n->list[i] = to_pattern(p, e);
        }
        return n;
    case J_OBJECT:
        n->type = J_OBJECT_PAT;
        for (int i = 0; i < n->n; i++) {
            JsNode *e = n->list[i];
            if (e->type == J_SPREAD) { e->type = J_REST; e->a = to_pattern(p, e->a); }
            else e->b = to_pattern(p, e->b);
        }
        return n;
    case J_ASSIGN:
        if (strcmp(n->str, "=") != 0) break;
        n->type = J_ASSIGN_PAT;
        n->a = to_pattern(p, n->a);
        return n;
    default: break;
    }
    fail_at(p, n->line, n->col, "alvo de atribuição inválido");
}

/* ================================================================ templates == */
static JsNode *parse_template(P *p) {
    JsNode *t = mk(p, J_TEMPLATE);
    NV      l = {0};
    for (;;) {
        Buf  b;
        bool end = false;
        buf_init(&b);
        for (;;) {
            if (p->pos >= p->len) LEXFAIL(p, "template literal não terminado");
            char c = p->src[p->pos];
            if (c == '`') { p->pos++; end = true; break; }
            if (c == '$' && p->pos + 1 < p->len && p->src[p->pos + 1] == '{') { p->pos += 2; break; }
            if (c == '\\') { read_escape(p, &b, true); continue; }
            if (c == '\r') { p->pos++; if (p->pos < p->len && p->src[p->pos] == '\n') p->pos++; newline(p, p->pos); buf_putc(&b, '\n'); continue; }
            if (c == '\n') { p->pos++; newline(p, p->pos); buf_putc(&b, '\n'); continue; }
            buf_putc(&b, c);
            p->pos++;
        }
        JsNode *s = mk(p, J_STR);
        s->str = arena_strndup(p->a, b.data ? b.data : "", b.len);
        s->slen = b.len;
        buf_free(&b);
        nv_push(p, &l, s);
        if (end) break;
        next(p);
        enter(p);
        JsNode *e = parse_expr(p, false);
        leave(p);
        nv_push(p, &l, e);
        if (!is_p(p, "}")) FAIL(p, "esperado '}' ao fechar ${...}");
        /* do not call next(): p->pos is already right after the '}' */
    }
    nv_set(&l, t);
    next(p);
    return t;
}

/* ============================================================== literals ===== */
static JsNode *ident_node(P *p, const Tok *t) {
    JsNode *n = mk(p, J_IDENT);
    n->str = t->s;
    n->slen = t->len;
    return n;
}

/* property name: identifier-name (keywords allowed), string, number, [computed], #private */
static JsNode *parse_prop_name(P *p, bool *computed) {
    *computed = false;
    JsNode *n;
    switch (p->tok.type) {
    case T_IDENT: case T_KEYWORD: n = ident_node(p, &p->tok); next(p); return n;
    case T_STR: n = mk(p, J_STR); n->str = p->tok.s; n->slen = p->tok.len; next(p); return n;
    case T_NUM: case T_BIGINT: n = mk(p, J_NUM); n->num = p->tok.num; next(p); return n;
    case T_PRIVATE: n = mk(p, J_PRIVATE); n->str = p->tok.s; n->slen = p->tok.len; next(p); return n;
    case T_PUNCT:
        if (is_p(p, "[")) {
            next(p);
            n = parse_assign(p, false);
            expect_p(p, "]");
            *computed = true;
            return n;
        }
        /* fallthrough */
    default: FAIL(p, "nome de propriedade esperado");
    }
}

static bool peek_starts_key(P *p) { /* after get/set/async/static: does a property key follow (not , : ( } = ;)? */
    Tok t = peek(p);
    if (t.type == T_EOF) return false;
    if (t.type == T_PUNCT && (!strcmp(t.p, ",") || !strcmp(t.p, ":") || !strcmp(t.p, "(") || !strcmp(t.p, "}") || !strcmp(t.p, "=") || !strcmp(t.p, ";"))) return false;
    return true;
}

static JsNode *parse_method(P *p, JsNode *prop, uint32_t fflags) {
    JsNode *fn = mk(p, J_FUNC);
    fn->flags = fflags | F_METHOD;
    prop->b = parse_function_rest(p, fn);
    return prop;
}

static JsNode *parse_object(P *p) {
    JsNode *o = mk(p, J_OBJECT);
    NV      l = {0};
    next(p);
    while (!is_p(p, "}")) {
        if (is_p(p, "...")) {
            JsNode *s = mk(p, J_SPREAD);
            next(p);
            s->a = parse_assign(p, false);
            nv_push(p, &l, s);
        } else {
            JsNode  *prop = mk(p, J_PROP);
            uint32_t ff = 0;
            if (is_id(p, "async") && peek_starts_key(p) && !peek(p).nl) { ff |= F_ASYNC; next(p); }
            if (is_p(p, "*")) { ff |= F_GENERATOR; next(p); }
            if ((is_id(p, "get") || is_id(p, "set")) && !ff && peek_starts_key(p)) { prop->flags |= is_id(p, "get") ? F_GETTER : F_SETTER; next(p); }
            bool computed;
            Tok  keytok = p->tok;
            prop->a = parse_prop_name(p, &computed);
            if (computed) prop->flags |= F_COMPUTED;
            if (is_p(p, "(")) {
                parse_method(p, prop, ff);
                prop->flags |= F_METHOD;
            } else if (ff || (prop->flags & (F_GETTER | F_SETTER))) {
                FAIL(p, "esperado '(' após o nome do método");
            } else if (eat_p(p, ":")) {
                prop->b = parse_assign(p, false);
            } else if (keytok.type == T_IDENT && !computed) { /* shorthand, maybe with a default (cover grammar) */
                prop->flags |= F_SHORTHAND;
                JsNode *id = ident_node(p, &keytok);
                id->line = prop->a->line;
                id->col = prop->a->col;
                if (is_p(p, "=")) {
                    next(p);
                    JsNode *ap = mk_like(p, J_ASSIGN_PAT, id);
                    ap->a = id;
                    ap->b = parse_assign(p, false);
                    prop->b = ap;
                } else prop->b = id;
            } else FAIL(p, "esperado ':' após a chave da propriedade");
            nv_push(p, &l, prop);
        }
        if (!is_p(p, "}")) expect_p(p, ",");
    }
    next(p);
    nv_set(&l, o);
    return o;
}

static JsNode *parse_array(P *p) {
    JsNode *a = mk(p, J_ARRAY);
    NV      l = {0};
    next(p);
    while (!is_p(p, "]")) {
        if (is_p(p, ",")) { nv_push(p, &l, mk(p, J_HOLE)); next(p); continue; }
        if (is_p(p, "...")) {
            JsNode *s = mk(p, J_SPREAD);
            next(p);
            s->a = parse_assign(p, false);
            nv_push(p, &l, s);
        } else nv_push(p, &l, parse_assign(p, false));
        if (!is_p(p, "]")) expect_p(p, ",");
    }
    next(p);
    nv_set(&l, a);
    return a;
}

/* ================================================================= patterns == */
static JsNode *parse_binding_target(P *p) {
    if (p->tok.type == T_IDENT) {
        JsNode *n = ident_node(p, &p->tok);
        next(p);
        return n;
    }
    if (is_p(p, "[")) {
        JsNode *a = mk(p, J_ARRAY_PAT);
        NV      l = {0};
        next(p);
        while (!is_p(p, "]")) {
            if (is_p(p, ",")) { nv_push(p, &l, mk(p, J_HOLE)); next(p); continue; }
            if (is_p(p, "...")) {
                JsNode *r = mk(p, J_REST);
                next(p);
                r->a = parse_binding_target(p);
                nv_push(p, &l, r);
            } else nv_push(p, &l, parse_binding_element(p));
            if (!is_p(p, "]")) expect_p(p, ",");
        }
        next(p);
        nv_set(&l, a);
        return a;
    }
    if (is_p(p, "{")) {
        JsNode *o = mk(p, J_OBJECT_PAT);
        NV      l = {0};
        next(p);
        while (!is_p(p, "}")) {
            if (is_p(p, "...")) {
                JsNode *r = mk(p, J_REST);
                next(p);
                r->a = parse_binding_target(p);
                nv_push(p, &l, r);
            } else {
                JsNode *prop = mk(p, J_PROP);
                bool    computed;
                Tok     keytok = p->tok;
                prop->a = parse_prop_name(p, &computed);
                if (computed) prop->flags |= F_COMPUTED;
                if (eat_p(p, ":")) prop->b = parse_binding_element(p);
                else if (keytok.type == T_IDENT && !computed) {
                    prop->flags |= F_SHORTHAND;
                    JsNode *id = ident_node(p, &keytok);
                    if (is_p(p, "=")) {
                        next(p);
                        JsNode *ap = mk_like(p, J_ASSIGN_PAT, id);
                        ap->a = id;
                        ap->b = parse_assign(p, false);
                        prop->b = ap;
                    } else prop->b = id;
                } else FAIL(p, "padrão de objeto inválido");
                nv_push(p, &l, prop);
            }
            if (!is_p(p, "}")) expect_p(p, ",");
        }
        next(p);
        nv_set(&l, o);
        return o;
    }
    FAIL(p, "nome de variável ou padrão esperado");
}

static JsNode *parse_binding_element(P *p) {
    JsNode *t = parse_binding_target(p);
    if (is_p(p, "=")) {
        JsNode *ap = mk_like(p, J_ASSIGN_PAT, t);
        next(p);
        ap->a = t;
        ap->b = parse_assign(p, false);
        return ap;
    }
    return t;
}

/* ================================================================ functions == */
static JsNode *parse_function_rest(P *p, JsNode *fn) {
    bool sa = p->in_async, sg = p->in_gen, sf = p->in_func;
    p->in_async = (fn->flags & F_ASYNC) != 0;
    p->in_gen = (fn->flags & F_GENERATOR) != 0;
    p->in_func = true;
    NV params = {0};
    expect_p(p, "(");
    while (!is_p(p, ")")) {
        if (is_p(p, "...")) {
            JsNode *r = mk(p, J_REST);
            next(p);
            r->a = parse_binding_target(p);
            nv_push(p, &params, r);
        } else nv_push(p, &params, parse_binding_element(p));
        if (!is_p(p, ")")) expect_p(p, ",");
    }
    next(p);
    nv_set(&params, fn);
    fn->a = parse_block(p);
    p->in_async = sa;
    p->in_gen = sg;
    p->in_func = sf;
    return fn;
}

static JsNode *parse_function(P *p, uint32_t flags, bool decl) {
    JsNode *fn = mk(p, J_FUNC);
    fn->flags = flags | (decl ? F_FUNC_DECL : 0);
    next(p); /* 'function' */
    if (eat_p(p, "*")) fn->flags |= F_GENERATOR;
    if (p->tok.type == T_IDENT) {
        fn->str = p->tok.s;
        fn->slen = p->tok.len;
        next(p);
    } else if (decl) FAIL(p, "nome da função esperado");
    return parse_function_rest(p, fn);
}

static JsNode *finish_arrow(P *p, NV *params, bool is_async, const JsNode *at) {
    if (p->tok.nl) FAIL(p, "quebra de linha antes de '=>'");
    next(p); /* => */
    JsNode *fn = mk_like(p, J_FUNC, at);
    fn->flags = F_ARROW | (is_async ? F_ASYNC : 0);
    nv_set(params, fn);
    bool sa = p->in_async, sg = p->in_gen, sf = p->in_func;
    p->in_async = is_async;
    p->in_gen = false;
    p->in_func = true;
    if (is_p(p, "{")) fn->a = parse_block(p);
    else {
        fn->flags |= F_EXPR_BODY;
        fn->a = parse_assign(p, false);
    }
    p->in_async = sa;
    p->in_gen = sg;
    p->in_func = sf;
    return fn;
}

/* ================================================================== classes === */
static JsNode *parse_class(P *p, bool decl) {
    JsNode *c = mk(p, J_CLASS);
    if (decl) c->flags |= F_FUNC_DECL;
    next(p); /* class */
    if (p->tok.type == T_IDENT && !is_id(p, "extends")) {
        c->a = ident_node(p, &p->tok);
        next(p);
    } else if (decl) FAIL(p, "nome da classe esperado");
    if (is_kw(p, "extends")) {
        next(p);
        c->b = parse_unary(p); /* LeftHandSideExpression */
    }
    expect_p(p, "{");
    NV l = {0};
    while (!is_p(p, "}")) {
        if (eat_p(p, ";")) continue;
        JsNode  *m = mk(p, J_PROP);
        uint32_t ff = 0;
        if (is_id(p, "static")) {
            Tok t = peek(p);
            if (tok_p(&t, "{")) { /* static initialization block */
                next(p);
                m->flags = F_STATIC | F_STATIC_BLOCK;
                bool sf = p->in_func;
                p->in_func = true;
                m->b = parse_block(p);
                p->in_func = sf;
                nv_push(p, &l, m);
                continue;
            }
            if (peek_starts_key(p)) { m->flags |= F_STATIC; next(p); }
        }
        if (is_id(p, "async") && peek_starts_key(p) && !peek(p).nl) { ff |= F_ASYNC; next(p); }
        if (is_p(p, "*")) { ff |= F_GENERATOR; next(p); }
        if ((is_id(p, "get") || is_id(p, "set")) && !ff && peek_starts_key(p)) { m->flags |= is_id(p, "get") ? F_GETTER : F_SETTER; next(p); }
        bool computed;
        m->a = parse_prop_name(p, &computed);
        if (computed) m->flags |= F_COMPUTED;
        if (is_p(p, "(")) {
            parse_method(p, m, ff);
            m->flags |= F_METHOD;
        } else {
            if (ff || (m->flags & (F_GETTER | F_SETTER))) FAIL(p, "esperado '(' após o nome do método");
            m->flags |= F_FIELD;
            if (eat_p(p, "=")) {
                bool sf = p->in_func, sa = p->in_async, sg = p->in_gen;
                p->in_func = true; p->in_async = false; p->in_gen = false;
                m->b = parse_assign(p, false);
                p->in_func = sf; p->in_async = sa; p->in_gen = sg;
            }
            consume_semicolon(p);
        }
        nv_push(p, &l, m);
    }
    next(p);
    nv_set(&l, c);
    return c;
}

/* ============================================================== expressions == */
static void parse_arguments(P *p, NV *args) {
    expect_p(p, "(");
    while (!is_p(p, ")")) {
        if (is_p(p, "...")) {
            JsNode *s = mk(p, J_SPREAD);
            next(p);
            s->a = parse_assign(p, false);
            nv_push(p, args, s);
        } else nv_push(p, args, parse_assign(p, false));
        if (!is_p(p, ")")) expect_p(p, ",");
    }
    next(p);
}

static JsNode *parse_regex(P *p) {
    JsNode *n = mk(p, J_REGEX);
    size_t  i = p->tok.start + 1;
    bool    in_class = false;
    size_t  st = i;
    for (;; i++) {
        if (i >= p->len || p->src[i] == '\n' || p->src[i] == '\r') FAIL(p, "expressão regular não terminada");
        char c = p->src[i];
        if (c == '\\') { i++; continue; }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) break;
    }
    n->str = arena_strndup(p->a, p->src + st, i - st);
    n->slen = i - st;
    i++;
    size_t fs = i;
    while (i < p->len && id_part((unsigned char)p->src[i])) i++;
    n->str2 = arena_strndup(p->a, p->src + fs, i - fs);
    p->pos = i;
    next(p);
    return n;
}

static JsNode *parse_paren_or_arrow(P *p) {
    JsNode *at = mk(p, J_SEQ);
    next(p); /* ( */
    NV   items = {0};
    bool has_rest = false, trailing = false;
    while (!is_p(p, ")")) {
        trailing = false;
        if (is_p(p, "...")) {
            JsNode *r = mk(p, J_REST);
            next(p);
            r->a = parse_binding_target(p);
            nv_push(p, &items, r);
            has_rest = true;
            if (!is_p(p, ")")) FAIL(p, "o parâmetro rest deve ser o último");
            break;
        }
        enter(p);
        nv_push(p, &items, parse_assign(p, false));
        leave(p);
        if (is_p(p, ",")) { next(p); trailing = true; }
        else if (!is_p(p, ")")) FAIL(p, "esperado ',' ou ')'");
    }
    expect_p(p, ")");
    if (is_p(p, "=>") ) {
        for (int i = 0; i < items.n; i++) if (items.v[i]->type != J_REST) items.v[i] = to_pattern(p, items.v[i]);
        return finish_arrow(p, &items, false, at);
    }
    if (items.n == 0 || has_rest || trailing) FAIL(p, "expressão entre parênteses inválida");
    if (items.n == 1) {
        items.v[0]->flags |= F_PAREN;
        return items.v[0];
    }
    nv_set(&items, at);
    return at;
}

static JsNode *parse_primary(P *p) {
    JsNode *n;
    switch (p->tok.type) {
    case T_NUM: n = mk(p, J_NUM); n->num = p->tok.num; next(p); return n;
    case T_BIGINT: n = mk(p, J_NUM); n->num = p->tok.num; n->flags |= F_BIGINT; next(p); return n;
    case T_STR: n = mk(p, J_STR); n->str = p->tok.s; n->slen = p->tok.len; next(p); return n;
    case T_TEMPLATE: return parse_template(p);
    case T_PRIVATE: n = mk(p, J_PRIVATE); n->str = p->tok.s; n->slen = p->tok.len; next(p); return n; /* #x in obj */
    case T_IDENT: {
        Tok id = p->tok;
        if (is_id(p, "async")) {
            Tok t = peek(p);
            if (!t.nl && t.type == T_KEYWORD && !strcmp(t.s, "function")) {
                next(p);
                return parse_function(p, F_ASYNC, false);
            }
            if (!t.nl && t.type == T_IDENT) { /* async x => ... */
                size_t pos = p->pos, ls = p->line_start;
                int    line = p->line;
                Tok    save = p->tok;
                next(p);
                Tok param = p->tok;
                Tok after = peek(p);
                if (tok_p(&after, "=>")) {
                    JsNode *at = ident_node(p, &id);
                    next(p);
                    NV ps = {0};
                    nv_push(p, &ps, ident_node(p, &param));
                    return finish_arrow(p, &ps, true, at);
                }
                p->pos = pos; p->line = line; p->line_start = ls; p->tok = save;
            }
            if (tok_p(&t, "(") && !t.nl) { /* async(...) => ... or a call to a function named async */
                JsNode *at = ident_node(p, &id);
                next(p);
                NV args = {0};
                parse_arguments(p, &args);
                if (is_p(p, "=>") && !p->tok.nl) {
                    for (int i = 0; i < args.n; i++) {
                        if (args.v[i]->type == J_SPREAD) { args.v[i]->type = J_REST; args.v[i]->a = to_pattern(p, args.v[i]->a); }
                        else args.v[i] = to_pattern(p, args.v[i]);
                    }
                    return finish_arrow(p, &args, true, at);
                }
                JsNode *call = mk_like(p, J_CALL, at);
                call->a = at;
                nv_set(&args, call);
                return call;
            }
        }
        Tok after = peek(p);
        if (tok_p(&after, "=>") && !after.nl) { /* x => ... */
            JsNode *at = ident_node(p, &id);
            next(p);
            NV ps = {0};
            nv_push(p, &ps, at);
            return finish_arrow(p, &ps, false, at);
        }
        n = ident_node(p, &id);
        next(p);
        return n;
    }
    case T_KEYWORD: {
        const char *k = p->tok.s;
        if (!strcmp(k, "function")) return parse_function(p, 0, false);
        if (!strcmp(k, "class")) return parse_class(p, false);
        if (!strcmp(k, "this")) { n = mk(p, J_THIS); next(p); return n; }
        if (!strcmp(k, "null")) { n = mk(p, J_NULL); next(p); return n; }
        if (!strcmp(k, "true")) { n = mk(p, J_TRUE); next(p); return n; }
        if (!strcmp(k, "false")) { n = mk(p, J_FALSE); next(p); return n; }
        FAIL(p, "token inesperado '%s'", k);
    }
    case T_PUNCT:
        if (is_p(p, "(")) return parse_paren_or_arrow(p);
        if (is_p(p, "[")) return parse_array(p);
        if (is_p(p, "{")) return parse_object(p);
        if (is_p(p, "/") || is_p(p, "/=")) return parse_regex(p);
        FAIL(p, "token inesperado '%s'", p->tok.p);
    case T_EOF: FAIL(p, "fim inesperado do código");
    }
    FAIL(p, "token inesperado");
}

static JsNode *name_after_dot(P *p) {
    if (p->tok.type == T_IDENT || p->tok.type == T_KEYWORD) { JsNode *n = ident_node(p, &p->tok); next(p); return n; }
    if (p->tok.type == T_PRIVATE) { JsNode *n = mk(p, J_PRIVATE); n->str = p->tok.s; n->slen = p->tok.len; next(p); return n; }
    FAIL(p, "nome de propriedade esperado após '.'");
}

/* member/call chain. allow_call=false is used for the callee of `new`. */
static JsNode *parse_chain(P *p, JsNode *obj, bool allow_call) {
    bool chain = false;
    for (;;) {
        enter(p);
        JsNode *n;
        if (is_p(p, ".")) {
            next(p);
            n = mk_like(p, J_MEMBER, obj);
            n->a = obj;
            n->b = name_after_dot(p);
        } else if (is_p(p, "?.")) {
            chain = true;
            next(p);
            if (is_p(p, "(")) {
                n = mk_like(p, J_CALL, obj);
                n->flags |= F_OPTIONAL;
                n->a = obj;
                NV args = {0};
                parse_arguments(p, &args);
                nv_set(&args, n);
            } else if (is_p(p, "[")) {
                next(p);
                n = mk_like(p, J_INDEX, obj);
                n->flags |= F_OPTIONAL;
                n->a = obj;
                n->b = parse_expr(p, false);
                expect_p(p, "]");
            } else {
                n = mk_like(p, J_MEMBER, obj);
                n->flags |= F_OPTIONAL;
                n->a = obj;
                n->b = name_after_dot(p);
            }
        } else if (is_p(p, "[")) {
            next(p);
            n = mk_like(p, J_INDEX, obj);
            n->a = obj;
            n->b = parse_expr(p, false);
            expect_p(p, "]");
        } else if (allow_call && is_p(p, "(")) {
            n = mk_like(p, J_CALL, obj);
            n->a = obj;
            NV args = {0};
            parse_arguments(p, &args);
            nv_set(&args, n);
        } else if (p->tok.type == T_TEMPLATE) {
            if (chain) FAIL(p, "template literal não pode seguir '?.'");
            n = mk_like(p, J_TAGGED, obj);
            n->a = obj;
            n->b = parse_template(p);
        } else {
            leave(p);
            break;
        }
        leave(p);
        obj = n;
    }
    if (chain) {
        JsNode *w = mk_like(p, J_OPTCHAIN, obj);
        w->a = obj;
        return w;
    }
    return obj;
}

static JsNode *parse_new_expr(P *p) {
    JsNode *n = mk(p, J_NEW);
    next(p); /* new */
    if (is_p(p, ".")) { /* new.target */
        next(p);
        if (!is_id(p, "target")) FAIL(p, "esperado 'new.target'");
        next(p);
        n->type = J_NEWTARGET;
        return n;
    }
    enter(p);
    JsNode *callee;
    if (is_kw(p, "new")) callee = parse_new_expr(p);
    else callee = parse_chain(p, parse_primary(p), false);
    leave(p);
    n->a = callee;
    if (is_p(p, "(")) {
        NV args = {0};
        parse_arguments(p, &args);
        nv_set(&args, n);
    }
    return n;
}

static JsNode *parse_lhs(P *p) {
    JsNode *base;
    if (is_kw(p, "new")) base = parse_new_expr(p);
    else if (is_kw(p, "super")) {
        base = mk(p, J_SUPER);
        next(p);
        if (!is_p(p, "(") && !is_p(p, ".") && !is_p(p, "[")) FAIL(p, "'super' só pode ser usado como super(...), super.x ou super[x]");
    } else if (is_kw(p, "import")) {
        Tok id = p->tok;
        next(p);
        if (!is_p(p, "(") && !is_p(p, ".")) FAIL(p, "declarações 'import' (módulos) ainda não são suportadas");
        base = ident_node(p, &id);
    } else base = parse_primary(p);
    if (base->type == J_FUNC && (base->flags & F_ARROW) && !(base->flags & F_PAREN)) return base; /* `x => x` cannot be called directly, `(x => x)()` can */
    return parse_chain(p, base, true);
}

static const char *op_str(P *p) { return p->tok.type == T_PUNCT ? arena_strdup(p->a, p->tok.p) : p->tok.s; }

static void check_target(P *p, const JsNode *n) {
    if ((n->type == J_IDENT || n->type == J_MEMBER || n->type == J_INDEX) && !(n->flags & F_OPTIONAL)) return;
    /* Web compatibility (Annex B): `f() = 1` and `f()++` are syntactically valid in sloppy scripts and only throw a
     * ReferenceError when executed. The interpreter must raise it at run time. */
    if (n->type == J_CALL && !(n->flags & F_OPTIONAL)) return;
    fail_at(p, n->line, n->col, "alvo de atribuição inválido");
}

static JsNode *parse_unary(P *p) {
    enter(p);
    JsNode *n;
    if (p->tok.type == T_PUNCT && (is_p(p, "!") || is_p(p, "~") || is_p(p, "+") || is_p(p, "-"))) {
        n = mk(p, J_UNARY);
        n->str = op_str(p);
        next(p);
        n->a = parse_unary(p);
    } else if (is_p(p, "++") || is_p(p, "--")) {
        n = mk(p, J_UPDATE);
        n->str = op_str(p);
        n->flags |= F_PREFIX;
        next(p);
        n->a = parse_unary(p);
        check_target(p, n->a);
    } else if (is_kw(p, "typeof") || is_kw(p, "void") || is_kw(p, "delete")) {
        n = mk(p, J_UNARY);
        n->str = p->tok.s;
        next(p);
        n->a = parse_unary(p);
    } else if (is_id(p, "await") && p->in_async) {
        n = mk(p, J_AWAIT);
        next(p);
        n->a = parse_unary(p);
    } else {
        n = parse_lhs(p);
        if ((is_p(p, "++") || is_p(p, "--")) && !p->tok.nl) {
            JsNode *u = mk_like(p, J_UPDATE, n);
            u->str = op_str(p);
            check_target(p, n);
            u->a = n;
            next(p);
            n = u;
        }
    }
    leave(p);
    return n;
}

static int binprec(const P *p, bool no_in) {
    if (p->tok.type == T_KEYWORD) {
        if (!strcmp(p->tok.s, "instanceof")) return 8;
        if (!strcmp(p->tok.s, "in") && !no_in) return 8;
        return -1;
    }
    if (p->tok.type != T_PUNCT) return -1;
    static const struct { const char *op; int prec; } tbl[] = {
        {"??", 1}, {"||", 2}, {"&&", 3}, {"|", 4}, {"^", 5}, {"&", 6}, {"==", 7}, {"!=", 7}, {"===", 7}, {"!==", 7},
        {"<", 8}, {">", 8}, {"<=", 8}, {">=", 8}, {"<<", 9}, {">>", 9}, {">>>", 9}, {"+", 10}, {"-", 10},
        {"*", 11}, {"/", 11}, {"%", 11}, {"**", 12},
    };
    for (size_t i = 0; i < sizeof tbl / sizeof *tbl; i++) if (!strcmp(p->tok.p, tbl[i].op)) return tbl[i].prec;
    return -1;
}

static JsNode *parse_binary(P *p, int minprec, bool no_in) {
    JsNode *left = parse_unary(p);
    for (;;) {
        int prec = binprec(p, no_in);
        if (prec < 0 || prec < minprec) break;
        const char *op = op_str(p);
        next(p);
        enter(p);
        JsNode *right = parse_binary(p, !strcmp(op, "**") ? prec : prec + 1, no_in);
        leave(p);
        bool    logical = !strcmp(op, "&&") || !strcmp(op, "||") || !strcmp(op, "??");
        if (logical) { /* `a ?? b || c` needs parentheses */
            bool nullish = !strcmp(op, "??");
            const JsNode *opnds[2] = {left, right};
            for (int k = 0; k < 2; k++)
                if (opnds[k]->type == J_LOGICAL && !(opnds[k]->flags & F_PAREN) && ((!strcmp(opnds[k]->str, "??")) != nullish))
                    fail_at(p, opnds[k]->line, opnds[k]->col, "não é permitido misturar o operador de coalescência nula com || ou && sem parênteses");
        }
        JsNode *n = mk_like(p, logical ? J_LOGICAL : J_BINARY, left);
        n->str = op;
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static JsNode *parse_conditional(P *p, bool no_in) {
    JsNode *c = parse_binary(p, 0, no_in);
    if (!is_p(p, "?")) return c;
    next(p);
    JsNode *n = mk_like(p, J_COND, c);
    n->a = c;
    enter(p);
    n->b = parse_assign(p, false);
    expect_p(p, ":");
    n->c = parse_assign(p, no_in);
    leave(p);
    return n;
}

static bool is_assign_op(const char *s) {
    static const char *ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "?\?=", NULL};
    for (int i = 0; ops[i]; i++) if (!strcmp(s, ops[i])) return true;
    return false;
}

static JsNode *parse_assign(P *p, bool no_in) {
    enter(p);
    if (is_id(p, "yield") && p->in_gen) {
        JsNode *y = mk(p, J_YIELD);
        next(p);
        if (!p->tok.nl && !is_p(p, ")") && !is_p(p, "]") && !is_p(p, "}") && !is_p(p, ",") && !is_p(p, ";") && !is_p(p, ":") &&
            p->tok.type != T_EOF && !is_kw(p, "in") && !is_id(p, "of")) {
            if (eat_p(p, "*")) y->flags |= F_DELEGATE;
            y->a = parse_assign(p, no_in);
        }
        leave(p);
        return y;
    }
    JsNode *left = parse_conditional(p, no_in);
    if (p->tok.type == T_PUNCT && is_assign_op(p->tok.p)) {
        const char *op = op_str(p);
        if (!strcmp(op, "=") && (left->type == J_ARRAY || left->type == J_OBJECT) && !(left->flags & F_PAREN)) left = to_pattern(p, left);
        else check_target(p, left);
        next(p);
        JsNode *n = mk_like(p, J_ASSIGN, left);
        n->str = op;
        n->a = left;
        n->b = parse_assign(p, no_in);
        left = n;
    }
    leave(p);
    return left;
}

static JsNode *parse_expr(P *p, bool no_in) {
    JsNode *first = parse_assign(p, no_in);
    if (!is_p(p, ",")) return first;
    JsNode *seq = mk_like(p, J_SEQ, first);
    NV      l = {0};
    nv_push(p, &l, first);
    while (eat_p(p, ",")) nv_push(p, &l, parse_assign(p, no_in));
    nv_set(&l, seq);
    return seq;
}

/* ================================================================ statements == */
static JsNode *parse_block(P *p) {
    JsNode *b = mk(p, J_BLOCK);
    NV      l = {0};
    expect_p(p, "{");
    while (!is_p(p, "}")) {
        if (p->tok.type == T_EOF) FAIL(p, "esperado '}' (fim do código)");
        nv_push(p, &l, parse_statement(p));
    }
    next(p);
    nv_set(&l, b);
    return b;
}

static JsNode *parse_var(P *p, bool no_in, bool for_head) {
    JsNode *v = mk(p, J_VAR);
    v->str = is_kw(p, "var") ? "var" : is_kw(p, "const") ? "const" : "let";
    next(p);
    NV l = {0};
    do {
        JsNode *d = mk(p, J_DECL);
        d->a = parse_binding_target(p);
        if (eat_p(p, "=")) d->b = parse_assign(p, no_in);
        else if (v->str[0] == 'c' && !for_head) FAIL(p, "declaração 'const' precisa de inicializador");
        else if (d->a->type != J_IDENT && !for_head) FAIL(p, "padrão de desestruturação precisa de inicializador");
        nv_push(p, &l, d);
    } while (eat_p(p, ","));
    nv_set(&l, v);
    return v;
}

static bool let_is_decl(P *p) {
    if (!is_id(p, "let")) return false;
    Tok t = peek(p);
    return (t.type == T_IDENT && strcmp(t.s, "of") != 0) || tok_p(&t, "[") || tok_p(&t, "{") || (t.type == T_IDENT && !strcmp(t.s, "of") && false);
}

static JsNode *parse_for(P *p) {
    JsNode *f = mk(p, J_FOR);
    next(p); /* for */
    if (is_id(p, "await") && p->in_async) { f->flags |= F_AWAIT; next(p); }
    expect_p(p, "(");
    JsNode *init = NULL;
    if (!is_p(p, ";")) {
        if (is_kw(p, "var") || is_kw(p, "const") || let_is_decl(p)) init = parse_var(p, true, true);
        else init = parse_expr(p, true);
        bool is_of = is_id(p, "of"), is_in = is_kw(p, "in");
        if (is_of || is_in) {
            f->type = is_of ? J_FOROF : J_FORIN;
            if (init->type != J_VAR) init = to_pattern(p, init);
            else if (init->n != 1) FAIL(p, "apenas uma variável é permitida em for-%s", is_of ? "of" : "in");
            f->a = init;
            next(p);
            f->b = is_of ? parse_assign(p, false) : parse_expr(p, false);
            expect_p(p, ")");
            enter(p);
            f->d = parse_statement(p);
            leave(p);
            return f;
        }
    }
    f->a = init;
    expect_p(p, ";");
    if (!is_p(p, ";")) f->b = parse_expr(p, false);
    expect_p(p, ";");
    if (!is_p(p, ")")) f->c = parse_expr(p, false);
    expect_p(p, ")");
    enter(p);
    f->d = parse_statement(p);
    leave(p);
    return f;
}

static JsNode *parse_statement(P *p) {
    enter(p);
    JsNode *n = NULL;
    switch (p->tok.type) {
    case T_PUNCT:
        if (is_p(p, "{")) { n = parse_block(p); goto done; }
        if (is_p(p, ";")) { n = mk(p, J_EMPTY); next(p); goto done; }
        break;
    case T_KEYWORD: {
        const char *k = p->tok.s;
        if (!strcmp(k, "var") || !strcmp(k, "const")) { n = parse_var(p, false, false); consume_semicolon(p); goto done; }
        if (!strcmp(k, "function")) { n = parse_function(p, 0, true); goto done; }
        if (!strcmp(k, "class")) { n = parse_class(p, true); goto done; }
        if (!strcmp(k, "if")) {
            n = mk(p, J_IF);
            next(p);
            expect_p(p, "(");
            n->a = parse_expr(p, false);
            expect_p(p, ")");
            n->b = parse_statement(p);
            if (is_kw(p, "else")) { next(p); n->c = parse_statement(p); }
            goto done;
        }
        if (!strcmp(k, "for")) { n = parse_for(p); goto done; }
        if (!strcmp(k, "while")) {
            n = mk(p, J_WHILE);
            next(p);
            expect_p(p, "(");
            n->a = parse_expr(p, false);
            expect_p(p, ")");
            n->d = parse_statement(p);
            goto done;
        }
        if (!strcmp(k, "do")) {
            n = mk(p, J_DOWHILE);
            next(p);
            n->d = parse_statement(p);
            if (!is_kw(p, "while")) FAIL(p, "esperado 'while' após o corpo do do-while");
            next(p);
            expect_p(p, "(");
            n->a = parse_expr(p, false);
            expect_p(p, ")");
            eat_p(p, ";");
            goto done;
        }
        if (!strcmp(k, "return")) {
            if (!p->in_func) FAIL(p, "'return' fora de função");
            n = mk(p, J_RETURN);
            next(p);
            if (!is_p(p, ";") && !is_p(p, "}") && p->tok.type != T_EOF && !p->tok.nl) n->a = parse_expr(p, false);
            consume_semicolon(p);
            goto done;
        }
        if (!strcmp(k, "break") || !strcmp(k, "continue")) {
            n = mk(p, k[0] == 'b' ? J_BREAK : J_CONTINUE);
            next(p);
            if (p->tok.type == T_IDENT && !p->tok.nl) { n->str = p->tok.s; next(p); }
            consume_semicolon(p);
            goto done;
        }
        if (!strcmp(k, "throw")) {
            n = mk(p, J_THROW);
            next(p);
            if (p->tok.nl) FAIL(p, "quebra de linha após 'throw'");
            n->a = parse_expr(p, false);
            consume_semicolon(p);
            goto done;
        }
        if (!strcmp(k, "try")) {
            n = mk(p, J_TRY);
            next(p);
            n->a = parse_block(p);
            if (is_kw(p, "catch")) {
                next(p);
                if (eat_p(p, "(")) {
                    n->b = parse_binding_target(p);
                    expect_p(p, ")");
                }
                n->c = parse_block(p);
            }
            if (is_kw(p, "finally")) { next(p); n->d = parse_block(p); }
            if (!n->c && !n->d) FAIL(p, "'try' precisa de 'catch' ou 'finally'");
            goto done;
        }
        if (!strcmp(k, "switch")) {
            n = mk(p, J_SWITCH);
            next(p);
            expect_p(p, "(");
            n->a = parse_expr(p, false);
            expect_p(p, ")");
            expect_p(p, "{");
            NV cases = {0};
            while (!is_p(p, "}")) {
                JsNode *c = mk(p, J_CASE);
                if (is_kw(p, "case")) { next(p); c->a = parse_expr(p, false); }
                else if (is_kw(p, "default")) next(p);
                else FAIL(p, "esperado 'case' ou 'default'");
                expect_p(p, ":");
                NV body = {0};
                while (!is_p(p, "}") && !is_kw(p, "case") && !is_kw(p, "default")) {
                    if (p->tok.type == T_EOF) FAIL(p, "esperado '}' (fim do código)");
                    nv_push(p, &body, parse_statement(p));
                }
                nv_set(&body, c);
                nv_push(p, &cases, c);
            }
            next(p);
            nv_set(&cases, n);
            goto done;
        }
        if (!strcmp(k, "debugger")) { n = mk(p, J_DEBUGGER); next(p); consume_semicolon(p); goto done; }
        if (!strcmp(k, "with")) FAIL(p, "'with' não é suportado");
        if (!strcmp(k, "import")) {
            Tok t = peek(p);
            if (!tok_p(&t, "(") && !tok_p(&t, ".")) FAIL(p, "declarações 'import' (módulos) ainda não são suportadas");
        }
        if (!strcmp(k, "export")) FAIL(p, "declarações 'export' (módulos) ainda não são suportadas");
        break;
    }
    case T_IDENT: {
        if (let_is_decl(p)) { n = parse_var(p, false, false); consume_semicolon(p); goto done; }
        if (is_id(p, "async")) {
            Tok t = peek(p);
            if (t.type == T_KEYWORD && !strcmp(t.s, "function") && !t.nl) { next(p); n = parse_function(p, F_ASYNC, true); goto done; }
        }
        Tok t = peek(p);
        if (tok_p(&t, ":")) {
            n = mk(p, J_LABEL);
            n->str = p->tok.s;
            next(p);
            next(p);
            n->a = parse_statement(p);
            goto done;
        }
        break;
    }
    default: break;
    }
    n = mk(p, J_EXPR);
    n->a = parse_expr(p, false);
    consume_semicolon(p);
done:
    leave(p);
    return n;
}

void js_parse(Arena *a, const char *src, size_t len, JsParseResult *out) {
    memset(out, 0, sizeof *out);
    P *p = xcalloc(1, sizeof *p);
    p->a = a;
    p->src = src;
    p->len = len;
    p->line = 1;
    if (setjmp(p->jb)) {
        snprintf(out->error, sizeof out->error, "%s", p->err);
        out->line = p->eline;
        out->col = p->ecol;
        free(p);
        return;
    }
    next(p);
    JsNode *prog = mk(p, J_PROGRAM);
    NV      l = {0};
    while (p->tok.type != T_EOF) nv_push(p, &l, parse_statement(p));
    nv_set(&l, prog);
    out->program = prog;
    out->nodes = p->nodes;
    free(p);
}

/* ================================================================== dump ===== */
static const char *NAMES[J__COUNT] = {
    [J_NUM] = "num", [J_STR] = "str", [J_TEMPLATE] = "template", [J_TAGGED] = "tagged", [J_REGEX] = "regex", [J_IDENT] = "id",
    [J_THIS] = "this", [J_NULL] = "null", [J_TRUE] = "true", [J_FALSE] = "false", [J_SUPER] = "super", [J_NEWTARGET] = "new.target",
    [J_ARRAY] = "array", [J_OBJECT] = "object", [J_PROP] = "prop", [J_FUNC] = "func", [J_CLASS] = "class", [J_UNARY] = "unary",
    [J_UPDATE] = "update", [J_BINARY] = "binary", [J_LOGICAL] = "logical", [J_ASSIGN] = "assign", [J_COND] = "cond", [J_CALL] = "call",
    [J_NEW] = "new", [J_MEMBER] = "member", [J_INDEX] = "index", [J_OPTCHAIN] = "optchain", [J_SEQ] = "seq", [J_SPREAD] = "spread",
    [J_AWAIT] = "await", [J_YIELD] = "yield", [J_PRIVATE] = "private", [J_HOLE] = "hole", [J_ARRAY_PAT] = "array-pat",
    [J_OBJECT_PAT] = "object-pat", [J_ASSIGN_PAT] = "default", [J_REST] = "rest", [J_PROGRAM] = "program", [J_VAR] = "var",
    [J_DECL] = "decl", [J_EXPR] = "expr", [J_BLOCK] = "block", [J_IF] = "if", [J_FOR] = "for", [J_FORIN] = "for-in", [J_FOROF] = "for-of",
    [J_WHILE] = "while", [J_DOWHILE] = "do-while", [J_RETURN] = "return", [J_THROW] = "throw", [J_BREAK] = "break",
    [J_CONTINUE] = "continue", [J_TRY] = "try", [J_SWITCH] = "switch", [J_CASE] = "case", [J_LABEL] = "label", [J_EMPTY] = "empty",
    [J_DEBUGGER] = "debugger",
};

static void dump_rec(const JsNode *n, Buf *o, int depth) {
    if (!n) return;
    if (depth > 2000) { buf_puts(o, "..."); return; }
    buf_putc(o, '(');
    buf_puts(o, NAMES[n->type] ? NAMES[n->type] : "?");
    if (n->type == J_NUM) buf_printf(o, " %.15g%s", n->num, (n->flags & F_BIGINT) ? "n" : "");
    else if (n->type == J_STR) { buf_puts(o, " \""); buf_append(o, n->str, n->slen); buf_putc(o, '"'); }
    else if (n->type == J_REGEX) buf_printf(o, " /%s/%s", n->str, n->str2 ? n->str2 : "");
    else if (n->str) buf_printf(o, " %s", n->str);
    if (n->flags & F_OPTIONAL) buf_puts(o, " ?.");
    if (n->flags & F_ARROW) buf_puts(o, " =>");
    if (n->flags & F_ASYNC) buf_puts(o, " async");
    if (n->flags & F_GENERATOR) buf_puts(o, " *");
    if (n->flags & F_COMPUTED) buf_puts(o, " [computed]");
    if (n->flags & F_STATIC) buf_puts(o, " static");
    if (n->flags & F_GETTER) buf_puts(o, " get");
    if (n->flags & F_SETTER) buf_puts(o, " set");
    if (n->flags & F_PREFIX) buf_puts(o, " prefix");
    if (n->flags & F_AWAIT) buf_puts(o, " await");
    if (n->flags & F_DELEGATE) buf_puts(o, " delegate");
    JsNode *kids[4] = {n->a, n->b, n->c, n->d};
    for (int i = 0; i < 4; i++) if (kids[i]) { buf_putc(o, ' '); dump_rec(kids[i], o, depth + 1); }
    for (int i = 0; i < n->n; i++) { buf_putc(o, ' '); dump_rec(n->list[i], o, depth + 1); }
    buf_putc(o, ')');
}

void js_dump(const JsNode *n, Buf *out) { dump_rec(n, out, 0); }
