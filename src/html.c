#include "html.h"

#include <stdlib.h>
#include <string.h>

#include "charset.h"

#define MAX_DEPTH 256
#define MAX_NODES 1500000

/* ============================================================ entities ==== */
static const char *latin1_names[96] = {
    "nbsp",  "iexcl", "cent",  "pound", "curren", "yen",   "brvbar", "sect",  "uml",   "copy",  "ordf",  "laquo",
    "not",   "shy",   "reg",   "macr",  "deg",    "plusmn", "sup2",  "sup3",  "acute", "micro", "para",  "middot",
    "cedil", "sup1",  "ordm",  "raquo", "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc", "Atilde",
    "Auml",  "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml",  "Igrave", "Iacute", "Icirc", "Iuml",
    "ETH",   "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc",
    "Uuml",  "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml",  "aring", "aelig", "ccedil",
    "egrave", "eacute", "ecirc", "euml",  "igrave", "iacute", "icirc", "iuml",  "eth",   "ntilde", "ograve", "oacute",
    "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc",  "uuml",  "yacute", "thorn", "yuml",
};

static const struct {
    const char *name;
    uint32_t    cp;
} extra_entities[] = {
    {"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},    {"apos", '\''},   {"ensp", 0x2002},
    {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C},  {"zwj", 0x200D},  {"lrm", 0x200E},   {"rlm", 0x200F},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"sbquo", 0x201A}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"dagger", 0x2020}, {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026},
    {"permil", 0x2030}, {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"euro", 0x20AC},
    {"trade", 0x2122}, {"larr", 0x2190}, {"uarr", 0x2191},  {"rarr", 0x2192}, {"darr", 0x2193},  {"harr", 0x2194},
    {"lArr", 0x21D0}, {"rArr", 0x21D2},  {"hArr", 0x21D4},  {"forall", 0x2200}, {"part", 0x2202}, {"exist", 0x2203},
    {"empty", 0x2205}, {"nabla", 0x2207}, {"isin", 0x2208}, {"sum", 0x2211},  {"minus", 0x2212}, {"radic", 0x221A},
    {"infin", 0x221E}, {"and", 0x2227},   {"or", 0x2228},   {"cap", 0x2229},  {"cup", 0x222A},   {"int", 0x222B},
    {"asymp", 0x2248}, {"ne", 0x2260},    {"equiv", 0x2261}, {"le", 0x2264},  {"ge", 0x2265},    {"loz", 0x25CA},
    {"spades", 0x2660}, {"clubs", 0x2663}, {"hearts", 0x2665}, {"diams", 0x2666}, {"check", 0x2713}, {"OElig", 0x152},
    {"oelig", 0x153}, {"Scaron", 0x160},  {"scaron", 0x161}, {"Yuml", 0x178}, {"fnof", 0x192},   {"circ", 0x2C6},
    {"tilde", 0x2DC}, {"alpha", 0x3B1},   {"beta", 0x3B2},  {"gamma", 0x3B3}, {"delta", 0x3B4},  {"pi", 0x3C0},
    {"sigma", 0x3C3}, {"omega", 0x3C9},   {"mu", 0x3BC},    {"lambda", 0x3BB}, {"Omega", 0x3A9}, {"Delta", 0x394},
};

static bool lookup_entity(const char *name, size_t n, uint32_t *cp) {
    for (int i = 0; i < 96; i++)
        if (strlen(latin1_names[i]) == n && !strncmp(latin1_names[i], name, n)) {
            *cp = 0xA0 + (uint32_t)i;
            return true;
        }
    for (size_t i = 0; i < sizeof extra_entities / sizeof *extra_entities; i++)
        if (strlen(extra_entities[i].name) == n && !strncmp(extra_entities[i].name, name, n)) {
            *cp = extra_entities[i].cp;
            return true;
        }
    return false;
}

void html_decode_entities(Buf *out, const char *s, size_t n, bool in_attr) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c != '&') {
            buf_putc(out, c);
            i++;
            continue;
        }
        size_t j = i + 1;
        if (j < n && s[j] == '#') {
            j++;
            bool hex = false;
            if (j < n && (s[j] == 'x' || s[j] == 'X')) {
                hex = true;
                j++;
            }
            size_t   ds = j;
            uint32_t v = 0;
            while (j < n && (hex ? hex_val((unsigned char)s[j]) >= 0 : is_digit((unsigned char)s[j]))) {
                if (v <= 0x10FFFF) v = v * (hex ? 16u : 10u) + (uint32_t)(hex ? hex_val((unsigned char)s[j]) : s[j] - '0');
                j++;
            }
            if (j == ds) {
                buf_putc(out, '&');
                i++;
                continue;
            }
            if (j < n && s[j] == ';') j++;
            if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
            else if (v >= 0x80 && v <= 0x9F) v = cp1252_high[v - 0x80];
            buf_put_utf8(out, v);
            i = j;
            continue;
        }
        size_t k = j;
        while (k < n && k - j < 32 && (is_alpha((unsigned char)s[k]) || is_digit((unsigned char)s[k]))) k++;
        uint32_t cp;
        if (k < n && s[k] == ';' && k > j && lookup_entity(s + j, k - j, &cp)) {
            buf_put_utf8(out, cp);
            i = k + 1;
            continue;
        }
        if (!in_attr && k > j) { /* legacy references allowed without ';' in text */
            static const char *legacy[] = {"amp", "lt", "gt", "quot", "nbsp", "copy", "reg", NULL};
            bool done = false;
            for (int l = 0; legacy[l]; l++) {
                size_t ll = strlen(legacy[l]);
                if (k - j >= ll && !strncmp(s + j, legacy[l], ll) && lookup_entity(legacy[l], ll, &cp)) {
                    buf_put_utf8(out, cp);
                    i = j + ll;
                    done = true;
                    break;
                }
            }
            if (done) continue;
        }
        buf_putc(out, '&');
        i++;
    }
}

/* ============================================================ tree builder = */
typedef struct {
    Arena *a;
    Node  *doc, *html, *head, *body;
    Node  *stack[MAX_DEPTH];
    int    sp;
    Buf    text;
    int    svg_depth;
    int    nodes;
} P;

static const char *const VOID_TAGS[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                                        "meta", "param", "source", "track", "wbr", NULL};
static const char *const HEAD_TAGS[] = {"title", "meta", "link", "style", "script", "base", "template", NULL};
static const char *const P_CLOSERS[] = {"address", "article", "aside", "blockquote", "details", "div", "dl",
                                        "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4",
                                        "h5", "h6", "header", "hgroup", "hr", "main", "menu", "nav", "ol", "p",
                                        "pre", "section", "table", "ul", "dd", "dt", "li", "center", "dir", NULL};
static const char *const RAW_TAGS[] = {"script", "style", "xmp", "iframe", "noembed", "noframes", NULL};
static const char *const RCDATA_TAGS[] = {"title", "textarea", NULL};

static bool in_list(const char *tag, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(tag, *list)) return true;
    return false;
}

static void ensure_body(P *p);
static Node *cur(P *p) { return p->sp ? p->stack[p->sp - 1] : p->doc; }

static void push(P *p, Node *n) {
    if (p->sp < MAX_DEPTH) p->stack[p->sp++] = n;
}

static Node *make_el(P *p, const char *tag) {
    p->nodes++;
    return dom_new_element(p->a, tag);
}

static void flush_text(P *p) {
    if (!p->text.len) return;
    if (!p->body) {
        bool blank = true;
        for (size_t i = 0; i < p->text.len; i++)
            if (!is_space((unsigned char)p->text.data[i])) blank = false;
        if (blank) { /* whitespace between <html>, <head>, <body> is not content */
            buf_clear(&p->text);
            return;
        }
        ensure_body(p); /* non-blank text before <body> forces a body */
    }
    Node *c;
    c = cur(p);
    if (c->last_child && c->last_child->type == NODE_TEXT) {
        /* merge adjacent text nodes */
        size_t ol = strlen(c->last_child->text);
        char  *m = arena_alloc(p->a, ol + p->text.len + 1);
        memcpy(m, c->last_child->text, ol);
        memcpy(m + ol, p->text.data, p->text.len);
        c->last_child->text = m;
    } else {
        dom_append(c, dom_new_text(p->a, p->text.data, p->text.len));
        p->nodes++;
    }
    buf_clear(&p->text);
}

static void ensure_html(P *p) {
    if (p->html) return;
    p->html = make_el(p, "html");
    dom_append(p->doc, p->html);
    push(p, p->html);
}

static void pop_to_depth(P *p, int depth) {
    if (depth < 0) depth = 0;
    if (p->sp > depth) p->sp = depth;
}

static void ensure_head(P *p) {
    ensure_html(p);
    if (p->head || p->body) return;
    p->head = make_el(p, "head");
    dom_append(p->html, p->head);
    push(p, p->head);
}

static void ensure_body(P *p) {
    ensure_html(p);
    if (p->body) return;
    /* close head if it is open */
    for (int i = 0; i < p->sp; i++)
        if (p->stack[i] == p->head) {
            pop_to_depth(p, i);
            break;
        }
    p->body = make_el(p, "body");
    dom_append(p->html, p->body);
    push(p, p->body);
}

/* Scans the open-element stack top-down. Finds the nearest element whose tag is in `targets`
 * without crossing any element in `barriers`. Returns its stack index or -1. */
static int find_in_scope(P *p, const char *const *targets, const char *const *barriers) {
    for (int i = p->sp - 1; i >= 0; i--) {
        const char *t = p->stack[i]->tag;
        if (in_list(t, targets)) return i;
        if (in_list(t, barriers)) return -1;
    }
    return -1;
}

static const char *const SCOPE_BARRIERS[] = {"html", "body", "table", "td", "th", "caption", "object", "applet",
                                              "marquee", "template", "button", NULL};
static const char *const LIST_BARRIERS[] = {"html", "body", "ul", "ol", "table", "td", "th", "menu", NULL};
static const char *const DL_BARRIERS[] = {"html", "body", "dl", "table", "td", "th", NULL};
static const char *const TABLE_BARRIERS[] = {"html", "body", NULL};

static void close_p(P *p) {
    static const char *const t[] = {"p", NULL};
    int i = find_in_scope(p, t, SCOPE_BARRIERS);
    if (i >= 0) pop_to_depth(p, i);
}

typedef struct {
    char **names, **values;
    int    n, cap;
} AttrList;

static void attrlist_add(AttrList *l, char *name, char *value) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->names[i], name)) { /* first occurrence wins */
            free(name);
            free(value);
            return;
        }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->names = xrealloc(l->names, sizeof(char *) * (size_t)l->cap);
        l->values = xrealloc(l->values, sizeof(char *) * (size_t)l->cap);
    }
    l->names[l->n] = name;
    l->values[l->n] = value;
    l->n++;
}

static void attrlist_free(AttrList *l) {
    for (int i = 0; i < l->n; i++) {
        free(l->names[i]);
        free(l->values[i]);
    }
    free(l->names);
    free(l->values);
    memset(l, 0, sizeof *l);
}

static void apply_attrs(P *p, Node *el, const AttrList *l, bool overwrite) {
    for (int i = 0; i < l->n; i++) {
        if (!overwrite && dom_attr(el, l->names[i])) continue;
        dom_set_attr(p->a, el, l->names[i], l->values[i]);
    }
}

static void start_tag(P *p, const char *tag, const AttrList *attrs, bool selfclose) {
    flush_text(p);
    if (!strcmp(tag, "html")) {
        ensure_html(p);
        apply_attrs(p, p->html, attrs, false);
        return;
    }
    if (!strcmp(tag, "head")) {
        if (!p->head && !p->body) ensure_head(p);
        return;
    }
    if (!strcmp(tag, "body")) {
        ensure_body(p);
        apply_attrs(p, p->body, attrs, false);
        return;
    }
    if (in_list(tag, HEAD_TAGS) && !p->body) ensure_head(p);
    else if (!in_list(tag, HEAD_TAGS)) ensure_body(p);

    /* ---- implied end tags ---- */
    if (in_list(tag, P_CLOSERS)) close_p(p);
    if (!strcmp(tag, "li")) {
        static const char *const t[] = {"li", NULL};
        int i = find_in_scope(p, t, LIST_BARRIERS);
        if (i >= 0) pop_to_depth(p, i);
    } else if (!strcmp(tag, "dd") || !strcmp(tag, "dt")) {
        static const char *const t[] = {"dd", "dt", NULL};
        int i = find_in_scope(p, t, DL_BARRIERS);
        if (i >= 0) pop_to_depth(p, i);
    } else if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
        Node *c = cur(p);
        if (c->type == NODE_ELEMENT && c->tag[0] == 'h' && c->tag[1] >= '1' && c->tag[1] <= '6' && !c->tag[2])
            pop_to_depth(p, p->sp - 1);
    } else if (!strcmp(tag, "option") || !strcmp(tag, "optgroup")) {
        Node *c = cur(p);
        if (dom_is(c, "option")) pop_to_depth(p, p->sp - 1);
        if (!strcmp(tag, "optgroup") && dom_is(cur(p), "optgroup")) pop_to_depth(p, p->sp - 1);
    } else if (!strcmp(tag, "a")) {
        static const char *const t[] = {"a", NULL};
        int i = find_in_scope(p, t, SCOPE_BARRIERS);
        if (i >= 0) pop_to_depth(p, i);
    } else if (!strcmp(tag, "tbody") || !strcmp(tag, "thead") || !strcmp(tag, "tfoot") || !strcmp(tag, "caption") ||
               !strcmp(tag, "colgroup")) {
        static const char *const t[] = {"table", NULL};
        int i = find_in_scope(p, t, TABLE_BARRIERS);
        if (i >= 0) pop_to_depth(p, i + 1);
    } else if (!strcmp(tag, "tr")) {
        static const char *const t[] = {"tbody", "thead", "tfoot", "table", NULL};
        int i = find_in_scope(p, t, TABLE_BARRIERS);
        if (i >= 0) pop_to_depth(p, i + 1);
        if (dom_is(cur(p), "table")) {
            Node *tb = make_el(p, "tbody");
            dom_append(cur(p), tb);
            push(p, tb);
        }
    } else if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        static const char *const t[] = {"tr", "tbody", "thead", "tfoot", "table", NULL};
        int i = find_in_scope(p, t, TABLE_BARRIERS);
        if (i >= 0) pop_to_depth(p, i + 1);
        if (dom_is(cur(p), "table")) {
            Node *tb = make_el(p, "tbody");
            dom_append(cur(p), tb);
            push(p, tb);
        }
        if (dom_is(cur(p), "tbody") || dom_is(cur(p), "thead") || dom_is(cur(p), "tfoot")) {
            Node *tr = make_el(p, "tr");
            dom_append(cur(p), tr);
            push(p, tr);
        }
    }

    if (p->nodes >= MAX_NODES) return;
    Node *el = make_el(p, tag);
    for (int i = 0; i < attrs->n; i++) dom_set_attr(p->a, el, attrs->names[i], attrs->values[i]);
    dom_append(cur(p), el);

    bool is_void = in_list(tag, VOID_TAGS);
    if (!strcmp(tag, "svg") || !strcmp(tag, "math")) p->svg_depth++;
    bool closes = is_void || (selfclose && p->svg_depth > 0);
    if (!closes) push(p, el);
    else if (!strcmp(tag, "svg")) p->svg_depth--;
}

static void end_tag(P *p, const char *tag) {
    flush_text(p);
    if (!strcmp(tag, "html") || !strcmp(tag, "body") || !strcmp(tag, "head")) {
        if (!strcmp(tag, "head") && p->head)
            for (int i = 0; i < p->sp; i++)
                if (p->stack[i] == p->head) {
                    pop_to_depth(p, i);
                    break;
                }
        return;
    }
    if (!strcmp(tag, "br")) {
        AttrList none = {0};
        start_tag(p, "br", &none, false);
        return;
    }
    if (!strcmp(tag, "p")) {
        static const char *const t[] = {"p", NULL};
        int i = find_in_scope(p, t, SCOPE_BARRIERS);
        if (i >= 0) pop_to_depth(p, i);
        else if (p->body) {
            AttrList none = {0};
            start_tag(p, "p", &none, false);
            pop_to_depth(p, p->sp - 1);
        }
        return;
    }
    bool table_ish = !strcmp(tag, "table") || !strcmp(tag, "td") || !strcmp(tag, "th") || !strcmp(tag, "tr") ||
                     !strcmp(tag, "tbody") || !strcmp(tag, "thead") || !strcmp(tag, "tfoot") || !strcmp(tag, "caption");
    for (int i = p->sp - 1; i >= 0; i--) {
        const char *t = p->stack[i]->tag;
        if (!strcmp(t, tag)) {
            if (!strcmp(tag, "svg") || !strcmp(tag, "math")) {
                if (p->svg_depth > 0) p->svg_depth--;
            }
            pop_to_depth(p, i);
            return;
        }
        if (!strcmp(t, "html") || !strcmp(t, "body")) return;
        if (!table_ish && in_list(t, SCOPE_BARRIERS) && strcmp(t, "button")) return;
    }
}

/* ============================================================ tokenizer ==== */
static bool name_char_end(char c) { return is_space((unsigned char)c) || c == '/' || c == '>'; }

static const char *find_ci(const char *s, const char *end, const char *needle) {
    size_t nl = strlen(needle);
    for (; s + nl <= end; s++)
        if (str_ieqn(s, needle, nl)) return s;
    return NULL;
}

Node *html_parse(Arena *a, const char *src, size_t len) {
    P *p = xcalloc(1, sizeof *p);
    p->a = a;
    p->doc = dom_new_document(a);
    buf_init(&p->text);
    const char *s = src, *end = src + len;

    while (s < end && p->nodes < MAX_NODES) {
        if (*s != '<') {
            const char *lt = memchr(s, '<', (size_t)(end - s));
            if (!lt) lt = end;
            html_decode_entities(&p->text, s, (size_t)(lt - s), false);
            s = lt;
            continue;
        }
        /* *s == '<' */
        if (s + 1 >= end) {
            buf_putc(&p->text, '<');
            s++;
            continue;
        }
        if (s[1] == '!') {
            if (s + 3 < end && s[2] == '-' && s[3] == '-') {
                const char *e = find_ci(s + 4, end, "-->");
                flush_text(p);
                const char *ce = e ? e : end;
                Node       *c = cur(p);
                if (p->nodes < MAX_NODES) {
                    dom_append(c, dom_new_comment(a, s + 4, (size_t)(ce - (s + 4))));
                    p->nodes++;
                }
                s = e ? e + 3 : end;
            } else {
                const char *gt = memchr(s, '>', (size_t)(end - s)); /* DOCTYPE, CDATA, bogus comments */
                s = gt ? gt + 1 : end;
            }
            continue;
        }
        if (s[1] == '?') {
            const char *gt = memchr(s, '>', (size_t)(end - s));
            s = gt ? gt + 1 : end;
            continue;
        }
        bool closing = s[1] == '/';
        const char *q = s + (closing ? 2 : 1);
        if (q >= end || !is_alpha((unsigned char)*q)) {
            if (closing) { /* </ followed by garbage: bogus comment */
                const char *gt = memchr(s, '>', (size_t)(end - s));
                s = gt ? gt + 1 : end;
            } else {
                buf_putc(&p->text, '<');
                s++;
            }
            continue;
        }
        const char *ns = q;
        while (q < end && !name_char_end(*q)) q++;
        char *tag = xstrndup(ns, (size_t)(q - ns));
        str_lower(tag);

        AttrList attrs = {0};
        bool     selfclose = false;
        for (;;) {
            while (q < end && (is_space((unsigned char)*q) || (*q == '/' && !(q + 1 < end && q[1] == '>')))) q++;
            if (q >= end) break;
            if (*q == '>') {
                q++;
                break;
            }
            if (*q == '/') { /* "/>" */
                selfclose = true;
                q += 2;
                break;
            }
            const char *an = q;
            q++; /* first char may be '=' per spec; consumed as part of name */
            while (q < end && !is_space((unsigned char)*q) && *q != '=' && *q != '>' && !(*q == '/' && q + 1 < end && q[1] == '>')) q++;
            char *name = xstrndup(an, (size_t)(q - an));
            str_lower(name);
            while (q < end && is_space((unsigned char)*q)) q++;
            Buf val;
            buf_init(&val);
            if (q < end && *q == '=') {
                q++;
                while (q < end && is_space((unsigned char)*q)) q++;
                if (q < end && (*q == '"' || *q == '\'')) {
                    char        quote = *q++;
                    const char *vs = q;
                    while (q < end && *q != quote) q++;
                    html_decode_entities(&val, vs, (size_t)(q - vs), true);
                    if (q < end) q++;
                } else {
                    const char *vs = q;
                    while (q < end && !is_space((unsigned char)*q) && *q != '>') q++;
                    html_decode_entities(&val, vs, (size_t)(q - vs), true);
                }
            }
            attrlist_add(&attrs, name, buf_steal(&val));
        }
        s = q;

        if (closing) {
            end_tag(p, tag);
        } else {
            start_tag(p, tag, &attrs, selfclose);
            bool raw = in_list(tag, RAW_TAGS), rc = in_list(tag, RCDATA_TAGS);
            if ((raw || rc) && cur(p) && dom_is(cur(p), tag)) {
                char pat[32];
                snprintf(pat, sizeof pat, "</%s", tag);
                const char *e = s;
                for (;;) {
                    e = find_ci(e, end, pat);
                    if (!e) break;
                    char after = e + strlen(pat) < end ? e[strlen(pat)] : '>';
                    if (name_char_end(after)) break;
                    e += 2;
                }
                const char *content_end = e ? e : end;
                if (content_end > s) {
                    Buf t;
                    buf_init(&t);
                    if (rc) html_decode_entities(&t, s, (size_t)(content_end - s), false);
                    else buf_append(&t, s, (size_t)(content_end - s));
                    dom_append(cur(p), dom_new_text(a, t.data, t.len));
                    p->nodes++;
                    buf_free(&t);
                }
                s = content_end;
                if (e) {
                    const char *gt = memchr(e, '>', (size_t)(end - e));
                    s = gt ? gt + 1 : end;
                }
                end_tag(p, tag);
            }
        }
        attrlist_free(&attrs);
        free(tag);
    }
    flush_text(p);
    Node *doc = p->doc;
    buf_free(&p->text);
    free(p);
    return doc;
}
