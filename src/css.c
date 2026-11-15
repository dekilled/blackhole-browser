#include "css.h"
#include "diag.h"

#include <stdlib.h>
#include <string.h>

#define MAX_NEST 8
#define MAX_PARTS 16
#define MAX_RULES 400000

/* ================================================================ props ==== */
static const struct {
    const char *name;
    PropId      id;
} prop_table[] = {
#define X(id, name) {name, PROP_##id},
    PROP_LIST(X)
#undef X
};

PropId prop_lookup(const char *name) {
    for (size_t i = 0; i < sizeof prop_table / sizeof *prop_table; i++)
        if (!strcmp(prop_table[i].name, name)) return prop_table[i].id;
    return PROP_UNKNOWN;
}

/* =============================================================== scanning == */
static char *strip_comments(const char *src, size_t len) {
    char  *out = xmalloc(len + 1);
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
            const char *e = memmem(src + i + 2, len - i - 2, "*/", 2);
            i = e ? (size_t)(e - src) + 2 : len;
            out[o++] = ' ';
        } else if (src[i] == '"' || src[i] == '\'') { /* copy strings verbatim */
            char q = src[i];
            out[o++] = src[i++];
            while (i < len && src[i] != q) {
                if (src[i] == '\\' && i + 1 < len) out[o++] = src[i++];
                out[o++] = src[i++];
            }
            if (i < len) out[o++] = src[i++];
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = 0;
    return out;
}

/* s points just after '{'. Returns pointer just after the matching '}'. */
static const char *skip_block(const char *s, const char *end) {
    int depth = 1;
    while (s < end && depth > 0) {
        char c = *s;
        if (c == '"' || c == '\'') {
            s++;
            while (s < end && *s != c) {
                if (*s == '\\' && s + 1 < end) s++;
                s++;
            }
        } else if (c == '{') depth++;
        else if (c == '}') depth--;
        s++;
    }
    return s > end ? end : s;
}

/* Finds the ';' or '{' ending a prelude at nesting level 0. */
static const char *find_prelude_end(const char *s, const char *end) {
    int paren = 0;
    while (s < end) {
        char c = *s;
        if (c == '"' || c == '\'') {
            s++;
            while (s < end && *s != c) {
                if (*s == '\\' && s + 1 < end) s++;
                s++;
            }
        } else if (c == '(' || c == '[') paren++;
        else if ((c == ')' || c == ']') && paren > 0) paren--;
        else if (paren == 0 && (c == ';' || c == '{' || c == '}')) return s;
        s++;
    }
    return end;
}

static const char *skip_ws(const char *s, const char *end) {
    while (s < end && is_space((unsigned char)*s)) s++;
    return s;
}

static bool ident_char(unsigned char c) { return is_alpha(c) || is_digit(c) || c == '-' || c == '_' || c >= 0x80; }

/* Parses an identifier with CSS escapes into out. Returns bytes consumed. */
static size_t parse_ident(const char *s, const char *end, Buf *out) {
    const char *start = s;
    while (s < end) {
        unsigned char c = (unsigned char)*s;
        if (ident_char(c)) {
            buf_putc(out, (char)c);
            s++;
        } else if (c == '\\' && s + 1 < end) {
            s++;
            if (hex_val((unsigned char)*s) >= 0) {
                uint32_t v = 0;
                int      k = 0;
                while (s < end && k < 6 && hex_val((unsigned char)*s) >= 0) {
                    v = v * 16 + (uint32_t)hex_val((unsigned char)*s);
                    s++;
                    k++;
                }
                if (s < end && is_space((unsigned char)*s)) s++;
                buf_put_utf8(out, v ? v : 0xFFFD);
            } else {
                buf_putc(out, *s++);
            }
        } else {
            break;
        }
    }
    return (size_t)(s - start);
}

/* ============================================================= selectors === */
typedef struct {
    const char *s, *end;
    Arena      *a;
} Cur;

static const char *intern_buf(Arena *a, Buf *b) {
    const char *r = arena_strndup(a, b->data ? b->data : "", b->len);
    buf_free(b);
    return r;
}

static bool parse_nth(const char *s, size_t n, int *a, int *b) {
    char *t = xstrndup(s, n);
    char *w = t;
    for (char *r = t; *r; r++)
        if (!is_space((unsigned char)*r)) *w++ = (char)to_lower((unsigned char)*r);
    *w = 0;
    bool ok = true;
    if (!strcmp(t, "odd")) { *a = 2; *b = 1; }
    else if (!strcmp(t, "even")) { *a = 2; *b = 0; }
    else {
        char *np = strchr(t, 'n');
        if (np) {
            *np = 0;
            if (!*t || !strcmp(t, "+")) *a = 1;
            else if (!strcmp(t, "-")) *a = -1;
            else *a = atoi(t);
            *b = *(np + 1) ? atoi(np + 1) : 0;
        } else {
            *a = 0;
            *b = atoi(t);
            if (!*t) ok = false;
        }
    }
    free(t);
    return ok;
}

static bool parse_compound(Cur *c, Compound *out, bool allow_not);

static bool parse_attr_sel(Cur *c, AttrSel *as) {
    /* c->s is just after '[' */
    const char *s = skip_ws(c->s, c->end);
    Buf         name;
    buf_init(&name);
    size_t k = parse_ident(s, c->end, &name);
    if (!k) { buf_free(&name); return false; }
    s += k;
    str_lower(name.data);
    as->name = intern_buf(c->a, &name);
    s = skip_ws(s, c->end);
    if (s < c->end && *s == ']') {
        as->op = 0;
        c->s = s + 1;
        return true;
    }
    char op = *s;
    if (op == '=') { s++; }
    else if ((op == '~' || op == '|' || op == '^' || op == '$' || op == '*') && s + 1 < c->end && s[1] == '=') { s += 2; }
    else return false;
    as->op = op;
    s = skip_ws(s, c->end);
    Buf val;
    buf_init(&val);
    if (s < c->end && (*s == '"' || *s == '\'')) {
        char q = *s++;
        while (s < c->end && *s != q) {
            if (*s == '\\' && s + 1 < c->end) s++;
            buf_putc(&val, *s++);
        }
        if (s < c->end) s++;
    } else {
        size_t k2 = parse_ident(s, c->end, &val);
        if (!k2) { /* numbers etc. */
            while (s < c->end && *s != ']' && !is_space((unsigned char)*s)) buf_putc(&val, *s++);
        } else s += k2;
    }
    as->value = intern_buf(c->a, &val);
    s = skip_ws(s, c->end);
    if (s < c->end && (*s == 'i' || *s == 'I')) {
        as->icase = true;
        s = skip_ws(s + 1, c->end);
    }
    if (s >= c->end || *s != ']') return false;
    c->s = s + 1;
    return true;
}

static bool parse_compound(Cur *c, Compound *out, bool allow_not) {
    memset(out, 0, sizeof *out);
    const char *classes[16];
    int         ncl = 0;
    AttrSel     attrs[8];
    int         nat = 0;
    Compound   *nots[4];
    int         nnot = 0;
    Compound   *anys[8];
    int         nany = 0;
    bool        any_zero = false;
    bool        any = false;
    const char *s = c->s;

    /* type selector / universal */
    if (s < c->end && *s == '*') {
        s++;
        any = true;
    } else if (s < c->end && (ident_char((unsigned char)*s) || *s == '\\') && !is_digit((unsigned char)*s)) {
        Buf b;
        buf_init(&b);
        s += parse_ident(s, c->end, &b);
        str_lower(b.data);
        out->tag = intern_buf(c->a, &b);
        any = true;
    }
    while (s < c->end) {
        char ch = *s;
        if (ch == '#') {
            Buf b;
            buf_init(&b);
            size_t k = parse_ident(s + 1, c->end, &b);
            if (!k) { buf_free(&b); return false; }
            s += 1 + k;
            out->id = intern_buf(c->a, &b);
            any = true;
        } else if (ch == '.') {
            Buf b;
            buf_init(&b);
            size_t k = parse_ident(s + 1, c->end, &b);
            if (!k || ncl >= 16) { buf_free(&b); return false; }
            s += 1 + k;
            classes[ncl++] = intern_buf(c->a, &b);
            any = true;
        } else if (ch == '[') {
            if (nat >= 8) return false;
            Cur sub = {s + 1, c->end, c->a};
            memset(&attrs[nat], 0, sizeof attrs[nat]);
            if (!parse_attr_sel(&sub, &attrs[nat])) return false;
            nat++;
            s = sub.s;
            any = true;
        } else if (ch == ':') {
            bool dbl = s + 1 < c->end && s[1] == ':';
            Buf b;
            buf_init(&b);
            size_t k = parse_ident(s + 1 + (dbl ? 1 : 0), c->end, &b);
            if (dbl) { /* pseudo-element: only ::before and ::after are supported */
                if (!k) { buf_free(&b); return false; }
                str_lower(b.data);
                int pe = !strcmp(b.data, "before") ? 1 : !strcmp(b.data, "after") ? 2 : 0;
                buf_free(&b);
                if (!pe || !allow_not) return false;
                out->pseudo_el = (uint8_t)pe;
                s += 2 + k;
                any = true;
                continue;
            }
            if (!k) { buf_free(&b); return false; }
            s += 1 + k;
            str_lower(b.data);
            const char *name = b.data;
            bool        fn = s < c->end && *s == '(';
            const char *arg = NULL;
            size_t      arglen = 0;
            if (fn) {
                int         depth = 1;
                const char *as = s + 1, *e = as;
                while (e < c->end && depth > 0) {
                    if (*e == '(') depth++;
                    else if (*e == ')') depth--;
                    e++;
                }
                if (depth != 0) { buf_free(&b); return false; }
                arg = as;
                arglen = (size_t)(e - 1 - as);
                s = e;
            }
            bool ok = true;
            if (!fn) {
                if (!strcmp(name, "first-child")) out->pseudo |= PC_FIRST_CHILD;
                else if (!strcmp(name, "last-child")) out->pseudo |= PC_LAST_CHILD;
                else if (!strcmp(name, "only-child")) out->pseudo |= PC_ONLY_CHILD;
                else if (!strcmp(name, "first-of-type")) out->pseudo |= PC_FIRST_OF_TYPE;
                else if (!strcmp(name, "last-of-type")) out->pseudo |= PC_LAST_OF_TYPE;
                else if (!strcmp(name, "only-of-type")) out->pseudo |= PC_ONLY_OF_TYPE;
                else if (!strcmp(name, "link") || !strcmp(name, "any-link")) out->pseudo |= PC_LINK;
                else if (!strcmp(name, "hover")) out->pseudo |= PC_HOVER;
                else if (!strcmp(name, "root")) out->pseudo |= PC_ROOT;
                else if (!strcmp(name, "empty")) out->pseudo |= PC_EMPTY;
                else if (!strcmp(name, "checked")) out->pseudo |= PC_CHECKED;
                else if (!strcmp(name, "disabled")) out->pseudo |= PC_DISABLED;
                else if (!strcmp(name, "enabled")) out->pseudo |= PC_ENABLED;
                else if (!strcmp(name, "visited") || !strcmp(name, "active") || !strcmp(name, "focus") ||
                         !strcmp(name, "focus-within") || !strcmp(name, "focus-visible") || !strcmp(name, "target"))
                    out->pseudo |= PC_NEVER;
                else if (!strcmp(name, "before") || !strcmp(name, "after")) {
                    if (allow_not) out->pseudo_el = !strcmp(name, "before") ? 1 : 2;
                    else ok = false;
                } else if (!strcmp(name, "first-line") || !strcmp(name, "first-letter"))
                    ok = false;
                else ok = false;
            } else {
                if (!strcmp(name, "nth-child") || !strcmp(name, "nth-last-child") || !strcmp(name, "nth-of-type") ||
                    !strcmp(name, "nth-last-of-type")) {
                    out->has_nth = true;
                    out->nth_of_type = strstr(name, "of-type") != NULL;
                    out->nth_last = strstr(name, "last") != NULL;
                    ok = parse_nth(arg, arglen, &out->nth_a, &out->nth_b);
                } else if ((!strcmp(name, "is") || !strcmp(name, "where") || !strcmp(name, "matches") || !strcmp(name, "-webkit-any") ||
                            !strcmp(name, "-moz-any")) && allow_not && nany == 0) {
                    /* comma separated list of compound selectors */
                    ok = true;
                    const char *seg = arg, *argend = arg + arglen;
                    int         paren = 0;
                    for (const char *q = arg;; q++) {
                        bool at_end = q >= argend;
                        if (!at_end) {
                            if (*q == '(' || *q == '[') paren++;
                            else if ((*q == ')' || *q == ']') && paren > 0) paren--;
                        }
                        if (at_end || (*q == ',' && paren == 0)) {
                            Cur sub = {seg, q, c->a};
                            sub.s = skip_ws(sub.s, sub.end);
                            Compound *nc = arena_alloc(c->a, sizeof *nc);
                            if (nany >= 8 || !parse_compound(&sub, nc, false) || skip_ws(sub.s, sub.end) != sub.end) { ok = false; break; }
                            anys[nany++] = nc;
                            seg = q + 1;
                            if (at_end) break;
                        }
                    }
                    any_zero = !strcmp(name, "where");
                    if (!ok) nany = 0;
                } else if (!strcmp(name, "not") && allow_not && nnot < 4) {
                    Cur   sub = {arg, arg + arglen, c->a};
                    sub.s = skip_ws(sub.s, sub.end);
                    Compound *nc = arena_alloc(c->a, sizeof *nc);
                    ok = parse_compound(&sub, nc, false) && skip_ws(sub.s, sub.end) == sub.end;
                    if (ok) nots[nnot++] = nc;
                } else ok = false;
            }
            buf_free(&b);
            if (!ok) return false;
            any = true;
        } else {
            break;
        }
    }
    c->s = s;
    if (!any) return false;
    if (ncl) {
        out->classes = arena_alloc(c->a, sizeof(char *) * (size_t)ncl);
        memcpy(out->classes, classes, sizeof(char *) * (size_t)ncl);
        out->nclasses = ncl;
    }
    if (nat) {
        out->attrs = arena_alloc(c->a, sizeof(AttrSel) * (size_t)nat);
        memcpy(out->attrs, attrs, sizeof(AttrSel) * (size_t)nat);
        out->nattrs = nat;
    }
    if (nany) {
        out->anys = arena_alloc(c->a, sizeof(Compound *) * (size_t)nany);
        memcpy(out->anys, anys, sizeof(Compound *) * (size_t)nany);
        out->nany = nany;
        out->any_zero = any_zero;
    }
    if (nnot) {
        out->nots = arena_alloc(c->a, sizeof(Compound *) * (size_t)nnot);
        memcpy(out->nots, nots, sizeof(Compound *) * (size_t)nnot);
        out->nnots = nnot;
    }
    return true;
}

static uint32_t compound_specificity(const Compound *c) {
    uint32_t a = c->id ? 1 : 0;
    uint32_t b = (uint32_t)c->nclasses + (uint32_t)c->nattrs + (c->has_nth ? 1u : 0u);
    for (uint32_t m = c->pseudo; m; m &= m - 1) b++;
    uint32_t t = (c->tag ? 1 : 0) + (c->pseudo_el ? 1 : 0);
    for (int i = 0; i < c->nnots; i++) {
        uint32_t s = compound_specificity(c->nots[i]);
        a += s >> 16;
        b += (s >> 8) & 255;
        t += s & 255;
    }
    if (c->nany && !c->any_zero) { /* :is() takes the specificity of its most specific argument */
        uint32_t best = 0;
        for (int i = 0; i < c->nany; i++) { uint32_t sp = compound_specificity(c->anys[i]); if (sp > best) best = sp; }
        a += best >> 16;
        b += (best >> 8) & 255;
        t += best & 255;
    }
    return (a << 16) | ((b > 255 ? 255 : b) << 8) | (t > 255 ? 255 : t);
}

static ComplexSel *parse_complex(const char *s, const char *end, Arena *a) {
    Compound parts[MAX_PARTS];
    char     combs[MAX_PARTS];
    int      n = 0;
    Cur      c = {s, end, a};
    c.s = skip_ws(c.s, end);
    uint32_t spec = 0;
    for (;;) {
        if (n >= MAX_PARTS) return NULL;
        if (!parse_compound(&c, &parts[n], true)) return NULL;
        uint32_t cs = compound_specificity(&parts[n]);
        spec += cs; /* component-wise addition without overflow for sane selectors */
        n++;
        const char *after = c.s;
        const char *p = skip_ws(after, end);
        if (p == end) break;
        char comb = ' ';
        if (*p == '>' || *p == '+' || *p == '~') {
            comb = *p;
            p = skip_ws(p + 1, end);
            if (p == end) return NULL;
        } else if (p == after) {
            return NULL; /* garbage right after a compound */
        }
        combs[n - 1] = comb;
        c.s = p;
    }
    for (int i = 0; i + 1 < n; i++) if (parts[i].pseudo_el) return NULL;
    ComplexSel *cs = arena_alloc(a, sizeof *cs);
    cs->pseudo_el = parts[n - 1].pseudo_el;
    cs->parts = arena_alloc(a, sizeof(Compound) * (size_t)n);
    memcpy(cs->parts, parts, sizeof(Compound) * (size_t)n);
    cs->combs = arena_alloc(a, (size_t)n);
    if (n > 1) memcpy(cs->combs, combs, (size_t)(n - 1));
    cs->n = n;
    cs->specificity = spec;
    return cs;
}

/* Splits on top-level commas. Returns NULL if any selector is invalid (whole rule is dropped). */
static ComplexSel **parse_selector_list(const char *s, const char *end, Arena *a, int *count) {
    ComplexSel **list = NULL;
    int          n = 0, cap = 0;
    const char  *start = s;
    int          paren = 0;
    for (const char *p = s;; p++) {
        bool at_end = p >= end;
        if (!at_end) {
            if (*p == '(' || *p == '[') paren++;
            else if ((*p == ')' || *p == ']') && paren > 0) paren--;
            else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (p < end && *p != q) {
                    if (*p == '\\') p++;
                    p++;
                }
            }
        }
        if (at_end || (*p == ',' && paren == 0)) {
            ComplexSel *cs = parse_complex(start, p, a);
            if (!cs) return NULL;
            if (n == cap) {
                int          nc = cap ? cap * 2 : 4;
                ComplexSel **nl = arena_alloc(a, sizeof(ComplexSel *) * (size_t)nc);
                if (n) memcpy(nl, list, sizeof(ComplexSel *) * (size_t)n);
                list = nl;
                cap = nc;
            }
            list[n++] = cs;
            start = p + 1;
            if (at_end) break;
        }
    }
    *count = n;
    return list;
}

/* ================================================================ matching == */
static bool has_class(const Node *n, const char *cls) {
    const char *v = dom_attr(n, "class");
    if (!v) return false;
    size_t cl = strlen(cls);
    while (*v) {
        while (is_space((unsigned char)*v)) v++;
        const char *t = v;
        while (*v && !is_space((unsigned char)*v)) v++;
        if ((size_t)(v - t) == cl && !strncmp(t, cls, cl)) return true;
    }
    return false;
}

static const Node *prev_el(const Node *n) {
    for (n = n->prev; n && (n->type != NODE_ELEMENT || (n->flags & NODE_PSEUDO)); n = n->prev) {}
    return n;
}
static const Node *next_el(const Node *n) {
    for (n = n->next; n && (n->type != NODE_ELEMENT || (n->flags & NODE_PSEUDO)); n = n->next) {}
    return n;
}

static bool attr_match(const AttrSel *as, const Node *n) {
    const char *v = dom_attr(n, as->name);
    if (!v) return false;
    if (!as->op) return true;
    size_t vl = strlen(v), al = strlen(as->value);
    int (*cmp)(const char *, const char *, size_t) = as->icase ? strncasecmp : strncmp;
    switch (as->op) {
    case '=': return vl == al && cmp(v, as->value, al) == 0;
    case '^': return al > 0 && vl >= al && cmp(v, as->value, al) == 0;
    case '$': return al > 0 && vl >= al && cmp(v + vl - al, as->value, al) == 0;
    case '*': return al > 0 && (as->icase ? strcasestr(v, as->value) : strstr(v, as->value)) != NULL;
    case '|': return vl >= al && cmp(v, as->value, al) == 0 && (vl == al || v[al] == '-');
    case '~': {
        if (al == 0) return false;
        const char *p = v;
        while (*p) {
            while (is_space((unsigned char)*p)) p++;
            const char *t = p;
            while (*p && !is_space((unsigned char)*p)) p++;
            if ((size_t)(p - t) == al && cmp(t, as->value, al) == 0) return true;
        }
        return false;
    }
    }
    return false;
}

static bool nth_ok(int a, int b, int idx) {
    if (a == 0) return idx == b;
    int d = idx - b;
    return d % a == 0 && d / a >= 0;
}

static bool match_compound(const Compound *c, const Node *n) {
    if (n->type != NODE_ELEMENT) return false;
    if (c->tag && strcmp(c->tag, n->tag) != 0) return false;
    if (c->id) {
        const char *id = dom_attr(n, "id");
        if (!id || strcmp(id, c->id) != 0) return false;
    }
    for (int i = 0; i < c->nclasses; i++)
        if (!has_class(n, c->classes[i])) return false;
    for (int i = 0; i < c->nattrs; i++)
        if (!attr_match(&c->attrs[i], n)) return false;
    uint32_t ps = c->pseudo;
    if (ps) {
        if (ps & PC_NEVER) return false;
        if ((ps & PC_FIRST_CHILD) && prev_el(n)) return false;
        if ((ps & PC_LAST_CHILD) && next_el(n)) return false;
        if ((ps & PC_ONLY_CHILD) && (prev_el(n) || next_el(n))) return false;
        if (ps & (PC_FIRST_OF_TYPE | PC_LAST_OF_TYPE | PC_ONLY_OF_TYPE)) {
            bool before = false, after = false;
            for (const Node *s = prev_el(n); s; s = prev_el(s)) if (!strcmp(s->tag, n->tag)) { before = true; break; }
            for (const Node *s = next_el(n); s; s = next_el(s)) if (!strcmp(s->tag, n->tag)) { after = true; break; }
            if ((ps & PC_FIRST_OF_TYPE) && before) return false;
            if ((ps & PC_LAST_OF_TYPE) && after) return false;
            if ((ps & PC_ONLY_OF_TYPE) && (before || after)) return false;
        }
        if ((ps & PC_LINK) && !((!strcmp(n->tag, "a") || !strcmp(n->tag, "area")) && dom_attr(n, "href"))) return false;
        if ((ps & PC_HOVER) && !(n->flags & NODE_HOVER)) return false;
        if ((ps & PC_ROOT) && !(n->parent && n->parent->type == NODE_DOCUMENT)) return false;
        if ((ps & PC_EMPTY) && n->first_child) return false;
        if ((ps & PC_CHECKED) && !dom_attr(n, "checked")) return false;
        if ((ps & PC_DISABLED) && !dom_attr(n, "disabled")) return false;
        if ((ps & PC_ENABLED) && dom_attr(n, "disabled")) return false;
    }
    if (c->has_nth) {
        int idx = 1;
        if (!c->nth_last) {
            for (const Node *s = prev_el(n); s; s = prev_el(s))
                if (!c->nth_of_type || !strcmp(s->tag, n->tag)) idx++;
        } else {
            for (const Node *s = next_el(n); s; s = next_el(s))
                if (!c->nth_of_type || !strcmp(s->tag, n->tag)) idx++;
        }
        if (!nth_ok(c->nth_a, c->nth_b, idx)) return false;
    }
    if (c->nany) {
        bool any = false;
        for (int i = 0; i < c->nany && !any; i++) any = match_compound(c->anys[i], n);
        if (!any) return false;
    }
    for (int i = 0; i < c->nnots; i++)
        if (match_compound(c->nots[i], n)) return false;
    return true;
}

static bool match_complex_at(const ComplexSel *s, int i, const Node *n) {
    if (!match_compound(&s->parts[i], n)) return false;
    if (i == 0) return true;
    switch (s->combs[i - 1]) {
    case '>': {
        const Node *p = n->parent;
        return p && p->type == NODE_ELEMENT && match_complex_at(s, i - 1, p);
    }
    case ' ':
        for (const Node *p = n->parent; p && p->type == NODE_ELEMENT; p = p->parent)
            if (match_complex_at(s, i - 1, p)) return true;
        return false;
    case '+': {
        const Node *p = prev_el(n);
        return p && match_complex_at(s, i - 1, p);
    }
    case '~':
        for (const Node *p = prev_el(n); p; p = prev_el(p))
            if (match_complex_at(s, i - 1, p)) return true;
        return false;
    }
    return false;
}

long css_rule_match(const Rule *r, const Node *n, int pseudo_el) {
    long best = -1;
    for (int i = 0; i < r->nsels; i++) {
        const ComplexSel *s = r->sels[i];
        if (s->pseudo_el != pseudo_el) continue;
        if ((long)s->specificity > best && match_complex_at(s, s->n - 1, n)) best = (long)s->specificity;
    }
    return best;
}

/* ============================================================ media queries == */
enum { MF_TRUE, MF_FALSE, MF_MIN_W, MF_MAX_W, MF_MIN_H, MF_MAX_H, MF_PORTRAIT, MF_LANDSCAPE };

static bool query_matches(const MediaQuery *q, const Viewport *vp) {
    bool ok = q->media_ok;
    for (int i = 0; ok && i < q->nfeatures; i++) {
        const MediaFeature *f = &q->features[i];
        switch (f->kind) {
        case MF_TRUE: break;
        case MF_FALSE: ok = false; break;
        case MF_MIN_W: ok = vp->vw >= f->value; break;
        case MF_MAX_W: ok = vp->vw <= f->value; break;
        case MF_MIN_H: ok = vp->vh >= f->value; break;
        case MF_MAX_H: ok = vp->vh <= f->value; break;
        case MF_PORTRAIT: ok = vp->vh >= vp->vw; break;
        case MF_LANDSCAPE: ok = vp->vw > vp->vh; break;
        }
    }
    return q->negate ? !ok : ok;
}

bool media_matches(const MediaCond *m, const Viewport *vp) {
    for (; m; m = m->outer) {
        if (!m->queries) continue;
        bool any = false;
        for (const MediaQuery *q = m->queries; q; q = q->next)
            if (query_matches(q, vp)) { any = true; break; }
        if (!any) return false;
    }
    return true;
}

static MediaCond *parse_media_cond(Arena *a, const char *s, size_t n, MediaCond *outer);

bool css_media_attr_matches(const char *media, const Viewport *vp) {
    if (!media) return true;
    const char *m0 = media;
    while (is_space((unsigned char)*m0)) m0++;
    if (!*m0) return true;
    Arena      a = {0};
    MediaCond *m = parse_media_cond(&a, media, strlen(media), NULL);
    bool       r = media_matches(m, vp);
    arena_free(&a);
    return r;
}

static float parse_media_len(const char *s) {
    char  *endp;
    float v = strtof(s, &endp);
    if (!strncasecmp(endp, "em", 2) || !strncasecmp(endp, "rem", 3)) v *= 16;
    return v;
}

static void add_feature(Arena *a, MediaFeature **arr, int *n, int kind, float v) {
    MediaFeature *na = arena_alloc(a, sizeof(MediaFeature) * (size_t)(*n + 1));
    if (*n) memcpy(na, *arr, sizeof(MediaFeature) * (size_t)*n);
    na[*n].kind = kind;
    na[*n].value = v;
    *arr = na;
    (*n)++;
}

static MediaQuery *parse_one_query(Arena *a, char *s) {
    MediaQuery *q = arena_alloc(a, sizeof *q);
    q->media_ok = true;
    char *p = str_trim(s);
    if (!*p) return q;
    if (str_istarts_with(p, "not ")) { q->negate = true; p = str_trim(p + 4); }
    else if (str_istarts_with(p, "only ")) p = str_trim(p + 5);
    while (*p) {
        p = str_trim(p);
        if (*p == '(') {
            char *e = strchr(p, ')');
            if (!e) { add_feature(a, &q->features, &q->nfeatures, MF_FALSE, 0); break; }
            *e = 0;
            char *inner = str_trim(p + 1);
            str_lower(inner);
            char *colon = strchr(inner, ':');
            if (colon) {
                *colon = 0;
                char *name = str_trim(inner), *val = str_trim(colon + 1);
                if (!strcmp(name, "min-width")) add_feature(a, &q->features, &q->nfeatures, MF_MIN_W, parse_media_len(val));
                else if (!strcmp(name, "max-width")) add_feature(a, &q->features, &q->nfeatures, MF_MAX_W, parse_media_len(val));
                else if (!strcmp(name, "min-height")) add_feature(a, &q->features, &q->nfeatures, MF_MIN_H, parse_media_len(val));
                else if (!strcmp(name, "max-height")) add_feature(a, &q->features, &q->nfeatures, MF_MAX_H, parse_media_len(val));
                else if (!strcmp(name, "orientation"))
                    add_feature(a, &q->features, &q->nfeatures, !strcmp(val, "portrait") ? MF_PORTRAIT : MF_LANDSCAPE, 0);
                else if (!strcmp(name, "prefers-color-scheme"))
                    add_feature(a, &q->features, &q->nfeatures, !strcmp(val, "light") ? MF_TRUE : MF_FALSE, 0);
                else if (!strcmp(name, "hover") || !strcmp(name, "any-hover") || !strcmp(name, "pointer") ||
                         !strcmp(name, "any-pointer"))
                    add_feature(a, &q->features, &q->nfeatures,
                                (!strcmp(val, "hover") || !strcmp(val, "fine")) ? MF_TRUE : MF_FALSE, 0);
                else if (!strcmp(name, "prefers-reduced-motion"))
                    add_feature(a, &q->features, &q->nfeatures, !strcmp(val, "no-preference") ? MF_TRUE : MF_FALSE, 0);
                else add_feature(a, &q->features, &q->nfeatures, MF_FALSE, 0);
            } else if (strstr(inner, "width") || strstr(inner, "height")) { /* range syntax: (width >= 600px) */
                bool  w = strstr(inner, "width") != NULL;
                char *op = strpbrk(inner, "<>");
                if (op) {
                    bool  gt = *op == '>';
                    bool  eq = op[1] == '=';
                    float v = parse_media_len(str_trim(op + (eq ? 2 : 1)));
                    bool  var_left = op > strstr(inner, w ? "width" : "height");
                    bool  is_min = var_left ? gt : !gt;
                    add_feature(a, &q->features, &q->nfeatures,
                                w ? (is_min ? MF_MIN_W : MF_MAX_W) : (is_min ? MF_MIN_H : MF_MAX_H), v);
                } else add_feature(a, &q->features, &q->nfeatures, MF_TRUE, 0);
            } else if (!strcmp(inner, "color") || !strcmp(inner, "hover") || !strcmp(inner, "pointer")) {
                add_feature(a, &q->features, &q->nfeatures, MF_TRUE, 0);
            } else add_feature(a, &q->features, &q->nfeatures, MF_FALSE, 0);
            p = e + 1;
        } else {
            char *e = p;
            while (*e && !is_space((unsigned char)*e) && *e != '(') e++;
            char saved = *e;
            *e = 0;
            if (!strcasecmp(p, "and")) {
            } else if (!strcasecmp(p, "screen") || !strcasecmp(p, "all")) {
            } else if (!strcasecmp(p, "print") || !strcasecmp(p, "speech") || !strcasecmp(p, "tty") ||
                       !strcasecmp(p, "projection") || !strcasecmp(p, "handheld")) {
                q->media_ok = false;
            } else {
                add_feature(a, &q->features, &q->nfeatures, MF_FALSE, 0);
            }
            *e = saved;
            p = e;
        }
    }
    return q;
}

static MediaCond *parse_media_cond(Arena *a, const char *s, size_t n, MediaCond *outer) {
    MediaCond *m = arena_alloc(a, sizeof *m);
    m->outer = outer;
    char       *copy = xstrndup(s, n);
    MediaQuery *tail = NULL;
    char       *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        MediaQuery *q = parse_one_query(a, tok);
        if (tail) tail->next = q;
        else m->queries = q;
        tail = q;
    }
    free(copy);
    return m;
}

/* ============================================================ declarations === */
static Decl *parse_decls_range(Arena *a, const char *s, const char *end) {
    Decl  *head = NULL, **tail = &head;
    while (s < end) {
        /* find end of declaration at top level */
        const char *p = s;
        int         paren = 0;
        bool        nested = false;
        while (p < end) {
            char c = *p;
            if (c == '"' || c == '\'') {
                p++;
                while (p < end && *p != c) {
                    if (*p == '\\' && p + 1 < end) p++;
                    p++;
                }
            } else if (c == '(') paren++;
            else if (c == ')' && paren > 0) paren--;
            else if (c == '{') {
                nested = true;
                p = skip_block(p + 1, end);
                continue;
            } else if (c == ';' && paren == 0) break;
            p++;
        }
        const char *seg_end = p;
        if (!nested) {
            const char *colon = memchr(s, ':', (size_t)(seg_end - s));
            if (colon) {
                char *name = xstrndup(s, (size_t)(colon - s));
                char *val = xstrndup(colon + 1, (size_t)(seg_end - colon - 1));
                char *nt = str_trim(name);
                char *vt = str_trim(val);
                bool  important = false;
                size_t vl = strlen(vt);
                /* "!important" with optional whitespace after '!' */
                char *bang = strrchr(vt, '!');
                if (bang) {
                    char *w = str_trim(bang + 1);
                    if (!strcasecmp(w, "important")) {
                        important = true;
                        *bang = 0;
                        vt = str_trim(vt);
                        vl = strlen(vt);
                    }
                }
                if (*nt && vl) {
                    bool   custom = nt[0] == '-' && nt[1] == '-';
                    PropId id = PROP_UNKNOWN;
                    if (custom) id = PROP_CUSTOM;
                    else {
                        str_lower(nt);
                        id = prop_lookup(nt);
                    }
                    if (id == PROP_UNKNOWN && nt[0] != '-') diag_note("css-propriedade-ignorada", nt);
                    if (id != PROP_UNKNOWN) {
                        Decl *d = arena_alloc(a, sizeof *d);
                        d->prop = id;
                        d->name = custom ? arena_strdup(a, nt) : NULL;
                        d->value = arena_strndup(a, vt, vl);
                        d->important = important;
                        *tail = d;
                        tail = &d->next;
                    }
                }
                free(name);
                free(val);
            }
        }
        s = seg_end < end ? seg_end + 1 : end;
    }
    return head;
}

Decl *css_parse_decls(Arena *a, const char *src, size_t len) {
    char *clean = strip_comments(src, len);
    Decl *d = parse_decls_range(a, clean, clean + strlen(clean));
    free(clean);
    return d;
}

/* ============================================================== stylesheet === */
static void add_rule(Stylesheet *sh, ComplexSel **sels, int nsels, Decl *decls, MediaCond *media) {
    if (sh->nrules >= MAX_RULES) return;
    if (sh->nrules == sh->caprules) {
        sh->caprules = sh->caprules ? sh->caprules * 2 : 64;
        sh->rules = xrealloc(sh->rules, sizeof(Rule) * (size_t)sh->caprules);
    }
    Rule *r = &sh->rules[sh->nrules];
    r->sels = sels;
    r->nsels = nsels;
    r->decls = decls;
    r->media = media;
    r->order = sh->nrules++;
}

static char *extract_import_url(const char *p, const char *end) {
    p = skip_ws(p, end);
    if (end - p >= 4 && !strncasecmp(p, "url(", 4)) p += 4;
    p = skip_ws(p, end);
    char q = 0;
    if (p < end && (*p == '"' || *p == '\'')) q = *p++;
    const char *s = p;
    while (p < end && (q ? *p != q : (*p != ')' && !is_space((unsigned char)*p)))) p++;
    if (p == s) return NULL;
    return xstrndup(s, (size_t)(p - s));
}

static void parse_rules(Stylesheet *sh, const char *s, const char *end, MediaCond *media, int depth) {
    while (s < end) {
        s = skip_ws(s, end);
        if (s >= end) break;
        if (s[0] == '<' && s + 3 < end && !strncmp(s, "<!--", 4)) { s += 4; continue; }
        if (s[0] == '-' && s + 2 < end && !strncmp(s, "-->", 3)) { s += 3; continue; }
        if (*s == '}') { s++; continue; }
        if (*s == '@') {
            const char *ns = s + 1;
            Buf         name;
            buf_init(&name);
            ns += parse_ident(ns, end, &name);
            str_lower(name.data ? name.data : (char *)"");
            const char *pe = find_prelude_end(ns, end);
            const char *prelude = ns, *prelude_end = pe;
            bool        has_block = pe < end && *pe == '{';
            const char *block = has_block ? pe + 1 : NULL;
            const char *block_end = has_block ? skip_block(block, end) : NULL;
            const char *inner_end = has_block ? (block_end > block && block_end[-1] == '}' ? block_end - 1 : block_end) : NULL;
            const char *an = name.data ? name.data : "";
            if (!strcmp(an, "import") && !has_block) {
                char *url = extract_import_url(prelude, prelude_end);
                if (url && sh->nimports < 16) {
                    /* skip imports restricted to print etc.: crude check on the tail */
                    sh->imports = xrealloc(sh->imports, sizeof(char *) * (size_t)(sh->nimports + 1));
                    sh->imports[sh->nimports++] = url;
                } else free(url);
                s = pe < end ? pe + 1 : end;
            } else if (!strcmp(an, "media") && has_block) {
                if (depth < MAX_NEST) {
                    MediaCond *m = parse_media_cond(&sh->arena, prelude, (size_t)(prelude_end - prelude), media);
                    parse_rules(sh, block, inner_end, m, depth + 1);
                }
                s = block_end;
            } else if ((!strcmp(an, "supports") || !strcmp(an, "layer") || !strcmp(an, "document") ||
                        !strcmp(an, "-moz-document")) && has_block) {
                if (depth < MAX_NEST) parse_rules(sh, block, inner_end, media, depth + 1);
                s = block_end;
            } else {
                if (strcmp(an, "charset") && strcmp(an, "namespace")) { char tmp[40]; snprintf(tmp, sizeof tmp, "@%s", an); diag_note("css-at-rule-ignorada", tmp); }
                /* @font-face, @keyframes, @page, @charset, @namespace, @layer a, b; ... */
                s = has_block ? block_end : (pe < end ? pe + 1 : end);
            }
            buf_free(&name);
            continue;
        }
        /* qualified rule */
        const char *pe = find_prelude_end(s, end);
        if (pe >= end || *pe != '{') { /* malformed: skip to next ';' */
            s = pe < end ? pe + 1 : end;
            continue;
        }
        const char *block = pe + 1;
        const char *block_end = skip_block(block, end);
        const char *inner_end = block_end > block && block_end[-1] == '}' ? block_end - 1 : block_end;
        int          nsels = 0;
        ComplexSel **sels = parse_selector_list(s, pe, &sh->arena, &nsels);
        if (!sels && diag_on) {
            char tmp[100];
            size_t n = (size_t)(pe - s) < sizeof tmp - 1 ? (size_t)(pe - s) : sizeof tmp - 1;
            memcpy(tmp, s, n);
            tmp[n] = 0;
            char *t = str_trim(tmp);
            if (strstr(t, "::-webkit-") || strstr(t, "::-moz-") || strstr(t, ":-moz-") || strstr(t, ":-ms-") || strstr(t, "::-ms-") ||
                strstr(t, ":-o-") || strstr(t, "::placeholder") || strstr(t, "::view-transition") || strstr(t, "::selection") || strstr(t, "::marker") ||
                strstr(t, "::backdrop") || strstr(t, ":popover-open") || strstr(t, "::file-selector"))
                diag_note("css-seletor-descartado", "(pseudo-elementos de navegador: ::-webkit-*, ::placeholder, ::selection... sem prejuizo visual)");
            else if (strstr(t, ":has("))
                diag_note("css-seletor-descartado", "(seletores com :has() - nao suportado)");
            else diag_note("css-seletor-descartado", t);
        }
        if (sels && nsels > 0) {
            Decl *d = parse_decls_range(&sh->arena, block, inner_end);
            if (d) add_rule(sh, sels, nsels, d, media);
        }
        s = block_end;
    }
}

Stylesheet *css_parse(const char *src, size_t len, int origin) {
    Stylesheet *sh = xcalloc(1, sizeof *sh);
    sh->origin = origin;
    char *clean = strip_comments(src, len);
    parse_rules(sh, clean, clean + strlen(clean), NULL, 0);
    free(clean);
    return sh;
}

void css_free(Stylesheet *s) {
    if (!s) return;
    for (int i = 0; i < s->nimports; i++) free(s->imports[i]);
    free(s->imports);
    free(s->rules);
    arena_free(&s->arena);
    free(s);
}
