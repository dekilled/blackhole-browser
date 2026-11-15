/* Box tree construction and layout: block, inline (line breaking), replaced elements, tables. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "layout_internal.h"

#define MAX_BOX_DEPTH 180
#define MAX_CB 64

typedef struct {
    Arena        *a;
    float         vw, vh;
    ImageResolver res;
    void         *user;
    Box          *cb_stack[MAX_CB];
    int           ncb;
    Box          *root;
    struct FloatList *fl;       /* float list of the block formatting context being laid out */
    float         fl_ox, fl_oy; /* content origin of the current block, in that list's coordinates */
    float         child_dx, child_dy; /* estimated offset of the next child inside its parent's content box */
} LC;

typedef struct {
    float x, y, w, h; /* margin box, in list coordinates */
    bool  left;
} FloatBox;

typedef struct FloatList {
    FloatBox *v;
    int       n, cap;
    float     last_top;
} FloatList;

#define VPUSH(a, arr, n, cap, T, val)                                                  \
    do {                                                                               \
        if ((n) == (cap)) {                                                            \
            int nc_ = (cap) ? (cap) * 2 : 8;                                           \
            T  *na_ = arena_alloc((a), sizeof(T) * (size_t)nc_);                       \
            if (n) memcpy(na_, (arr), sizeof(T) * (size_t)(n));                        \
            (arr) = na_;                                                               \
            (cap) = nc_;                                                               \
        }                                                                              \
        (arr)[(n)++] = (val);                                                          \
    } while (0)

/* ================================================================ helpers === */
BhFont *layout_style_font(const Style *s) {
    static const int fam[] = {FONT_SANS, FONT_SERIF, FONT_MONO};
    return font_get(fam[s->font_family % 3], s->font_weight >= 600, s->italic, s->font_size);
}

static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }

static void line_metrics(const Style *st, BhFont *f, float *asc, float *desc) {
    float a = font_ascent(f), d = font_descent(f);
    float lh = st->line_height < 0 ? font_line_gap_height(f) : (st->lh_is_mul ? st->line_height * st->font_size : st->line_height);
    float leading = lh - (a + d);
    *asc = a + leading / 2;
    *desc = d + leading / 2;
}

static float collapse_margins(float a, float b) {
    if (a >= 0 && b >= 0) return fmaxf_(a, b);
    if (a < 0 && b < 0) return fminf_(a, b);
    return a + b;
}

static void resolve_box_model(Box *b, float cb_w) {
    const Style *st = b->style;
    for (int i = 0; i < 4; i++) {
        b->m[i] = st->margin[i].is_auto ? 0 : len_resolve(&st->margin[i], cb_w, 0);
        b->p[i] = fmaxf_(0, len_resolve(&st->padding[i], cb_w, 0));
        b->bw[i] = st->border_style[i] != BS_NONE ? st->border_w[i] : 0;
    }
}

static bool is_positioned_out(const Style *s) { return s->position == POS_ABSOLUTE || s->position == POS_FIXED; }
static bool is_flex_container(const Style *st) { return st->display == DISP_FLEX || st->display == DISP_INLINE_FLEX; }

static Box *new_box(LC *lc, const Node *n, const Style *st, BoxKind kind) {
    Box *b = arena_alloc(lc->a, sizeof *b);
    b->node = n;
    b->style = st;
    b->kind = kind;
    b->baseline = -1;
    return b;
}

static void box_append(Box *parent, Box *child) {
    child->parent = parent;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

/* ============================================================ replaced ==== */
static ReplKind repl_kind_for(const Node *n) {
    const char *t = n->tag;
    if (!strcmp(t, "img")) return RP_IMG;
    if (!strcmp(t, "input")) {
        const char *ty = dom_attr(n, "type");
        if (!ty) return RP_TEXTINPUT;
        if (!strcasecmp(ty, "checkbox")) return RP_CHECK;
        if (!strcasecmp(ty, "radio")) return RP_RADIO;
        if (!strcasecmp(ty, "submit") || !strcasecmp(ty, "button") || !strcasecmp(ty, "reset") ||
            !strcasecmp(ty, "image") || !strcasecmp(ty, "file"))
            return RP_BUTTONINPUT;
        if (!strcasecmp(ty, "hidden")) return RP_EMPTY;
        return RP_TEXTINPUT;
    }
    if (!strcmp(t, "select")) return RP_SELECT;
    if (!strcmp(t, "textarea")) return RP_TEXTAREA;
    if (!strcmp(t, "iframe") || !strcmp(t, "video") || !strcmp(t, "audio") || !strcmp(t, "canvas") ||
        !strcmp(t, "embed") || !strcmp(t, "object"))
        return RP_FRAME;
    if (!strcmp(t, "svg") || !strcmp(t, "math") || !strcmp(t, "progress") || !strcmp(t, "meter")) return RP_EMPTY;
    return RP_NONE;
}

static float attr_px(const Node *n, const char *name, float dflt) {
    const char *v = dom_attr(n, name);
    if (!v) return dflt;
    char *e;
    float f = strtof(v, &e);
    if (e == v || f < 0 || strchr(e, '%')) return dflt;
    return f;
}

static const char *first_option_text(Arena *a, const Node *sel) {
    const Node *first = NULL, *chosen = NULL;
    for (const Node *n = sel->first_child; n; n = n->next) {
        const Node *opt = NULL;
        if (dom_is(n, "option")) opt = n;
        else if (dom_is(n, "optgroup")) {
            for (const Node *c = n->first_child; c; c = c->next)
                if (dom_is(c, "option")) { opt = c; if (!first) first = c; if (dom_attr(c, "selected")) chosen = c; }
            continue;
        }
        if (opt) { if (!first) first = opt; if (dom_attr(opt, "selected")) chosen = opt; }
    }
    const Node *o = chosen ? chosen : first;
    if (!o) return "";
    char *t = dom_text_content(o);
    const char *r = arena_strdup(a, str_trim(t));
    free(t);
    return r;
}

static void size_replaced(LC *lc, Box *b, float avail_w) {
    const Style *st = b->style;
    BhFont        *f = layout_style_font(st);
    float        nw = 0, nh = 0; /* natural content size */
    bool         have_ratio = false;
    resolve_box_model(b, avail_w);
    float hpad = b->p[1] + b->p[3] + b->bw[1] + b->bw[3], vpad = b->p[0] + b->p[2] + b->bw[0] + b->bw[2];
    float lh = font_line_gap_height(f);
    switch (b->repl) {
    case RP_IMG: {
        b->image = lc->res ? lc->res(lc->user, b->node) : NULL;
        if (b->image && b->image->w > 0 && b->image->h > 0) {
            nw = (float)b->image->w;
            nh = (float)b->image->h;
            have_ratio = true;
        } else {
            const char *alt = dom_attr(b->node, "alt");
            b->label = alt ? alt : "";
            float aw = attr_px(b->node, "width", -1), ah = attr_px(b->node, "height", -1);
            if (aw >= 0 && ah >= 0) { nw = aw; nh = ah; }
            else if (alt && *alt) { nw = font_text_width(f, alt, strlen(alt), 0) + 4; nh = lh + 2; }
        }
        break;
    }
    case RP_TEXTINPUT: {
        float chars = attr_px(b->node, "size", 20);
        nw = chars * st->font_size * 0.55f;
        nh = lh;
        const char *v = dom_attr(b->node, "value");
        const char *ph = dom_attr(b->node, "placeholder");
        b->label = v && *v ? v : (ph ? ph : "");
        const char *ty = dom_attr(b->node, "type");
        if (ty && !strcasecmp(ty, "password") && v && *v) {
            size_t n = strlen(v);
            char  *dots = arena_alloc(lc->a, n * 3 + 1);
            for (size_t i = 0; i < n; i++) memcpy(dots + i * 3, "\xE2\x80\xA2", 3);
            b->label = dots;
        }
        break;
    }
    case RP_BUTTONINPUT: {
        const char *v = dom_attr(b->node, "value");
        const char *ty = dom_attr(b->node, "type");
        if (!v || !*v) v = ty && !strcasecmp(ty, "reset") ? "Reset" : ty && !strcasecmp(ty, "file") ? "Choose file" : "Submit";
        b->label = v;
        nw = font_text_width(f, v, strlen(v), 0);
        nh = lh;
        break;
    }
    case RP_CHECK:
    case RP_RADIO: nw = nh = 13; break;
    case RP_SELECT: {
        b->label = first_option_text(lc->a, b->node);
        nw = font_text_width(f, b->label, strlen(b->label), 0) + 22;
        if (nw < 60) nw = 60;
        nh = lh;
        break;
    }
    case RP_TEXTAREA: {
        float cols = attr_px(b->node, "cols", 20), rows = attr_px(b->node, "rows", 2);
        nw = cols * st->font_size * 0.55f;
        nh = rows * lh;
        char *t = dom_text_content(b->node);
        b->label = arena_strdup(lc->a, t);
        free(t);
        break;
    }
    case RP_FRAME:
        nw = attr_px(b->node, "width", 300);
        nh = attr_px(b->node, "height", 150);
        b->label = dom_attr(b->node, "title") ? dom_attr(b->node, "title") : b->node->tag;
        break;
    case RP_EMPTY:
        nw = attr_px(b->node, "width", 0);
        nh = attr_px(b->node, "height", 0);
        break;
    case RP_NONE: break;
    }
    float cw = nw, ch = nh;
    bool  wauto = st->width.is_auto, hauto = st->height.is_auto;
    if (!wauto) {
        cw = len_resolve(&st->width, avail_w, nw);
        if (st->box_sizing_border) cw -= hpad;
    }
    if (!hauto) {
        ch = st->height.pct != 0 ? nh : len_resolve(&st->height, 0, nh);
        if (st->box_sizing_border && st->height.pct == 0) ch -= vpad;
    } else if (!wauto && have_ratio && nw > 0) {
        ch = cw * nh / nw;
    } else if (wauto && !hauto && have_ratio && nh > 0) {
        cw = ch * nw / nh;
    }
    if (!st->max_width.is_auto) {
        float mw = len_resolve(&st->max_width, avail_w, LAYOUT_INF);
        if (st->box_sizing_border) mw -= hpad;
        if (cw > mw && mw >= 0) {
            if (hauto && cw > 0) ch = ch * mw / cw;
            cw = mw;
        }
    }
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    b->w = cw + hpad;
    b->h = ch + vpad;
    b->cx = b->bw[3] + b->p[3];
    b->cy = b->bw[0] + b->p[0];
    b->cw = cw;
    b->baseline = (b->repl == RP_TEXTINPUT || b->repl == RP_BUTTONINPUT || b->repl == RP_SELECT)
                      ? b->cy + (ch - lh) / 2 + font_ascent(f) + (lh - font_ascent(f) - font_descent(f)) / 2
                      : -1;
}

/* ===================================================== forward declarations == */
static void  layout_block(LC *lc, Box *b, float cb_w, float forced_bw);
static void  layout_atom(LC *lc, Box *a, float avail);
static void  measure_box(LC *lc, Box *b, float *minw, float *maxw);
static float layout_inline(LC *lc, Box *b, float avail, bool emit, float *extent);
static void  build_children(LC *lc, Box *parent, const Node *node, int depth);

/* ================================================================= items ==== */
typedef struct {
    Item *v;
    int   n, cap;
} ItemList;

static void il_push(LC *lc, ItemList *l, Item it) { VPUSH(lc->a, l->v, l->n, l->cap, Item, it); }

static bool is_cjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}

static const char *transform_text(LC *lc, const char *s, size_t n, int tt, size_t *outn) {
    Buf  b;
    buf_init(&b);
    const char *end = s + n;
    bool        start = true;
    while (s < end) {
        uint32_t cp;
        s += utf8_decode(s, end, &cp);
        uint32_t r = cp;
        if (tt == TT_UPPER) r = (uint32_t)towupper((wint_t)cp);
        else if (tt == TT_LOWER) r = (uint32_t)towlower((wint_t)cp);
        else if (tt == TT_CAP && start && iswalpha((wint_t)cp)) r = (uint32_t)towupper((wint_t)cp);
        start = false;
        buf_put_utf8(&b, r);
    }
    const char *res = arena_strndup(lc->a, b.data ? b.data : "", b.len);
    *outn = b.len;
    buf_free(&b);
    return res;
}

static void emit_word(LC *lc, ItemList *L, const Style *st, BhFont *font, const Node *link, const char *s, size_t n, bool brk) {
    if (!n) return;
    Item it;
    memset(&it, 0, sizeof it);
    it.kind = IT_WORD;
    it.style = st;
    it.font = font;
    it.link = link;
    it.brk_before = brk;
    if (st->text_transform != TT_NONE) s = transform_text(lc, s, n, st->text_transform, &n);
    it.text = s;
    it.len = (int)n;
    it.w = font_text_width(font, s, n, st->letter_spacing);
    il_push(lc, L, it);
}

static void emit_space(LC *lc, ItemList *L, const Style *st, BhFont *font, const Node *link, int count, bool preserved) {
    Item it;
    memset(&it, 0, sizeof it);
    it.kind = IT_SPACE;
    it.style = st;
    it.font = font;
    it.link = link;
    it.len = count;
    it.preserved = preserved;
    it.w = (font_char_width(font, ' ') + st->letter_spacing) * (float)count;
    il_push(lc, L, it);
}

static void collect_text(LC *lc, ItemList *L, const Node *tn, const Style *st, const Node *link) {
    BhFont       *font = layout_style_font(st);
    const char *s = tn->text, *end = s + strlen(s);
    int         ws = st->white_space;
    bool        collapse = ws == WS_NORMAL || ws == WS_NOWRAP || ws == WS_PRE_LINE;
    bool        keep_nl = ws == WS_PRE || ws == WS_PRE_WRAP || ws == WS_PRE_LINE;
    const char *wstart = NULL;
    bool        next_brk = false;
    /* a newline immediately after <pre> is not content */
    if (ws == WS_PRE && tn->parent && dom_is(tn->parent, "pre") && tn->parent->first_child == tn && *s == '\n') s++;

#define FLUSH_WORD(upto)                                                                  \
    do {                                                                                  \
        if (wstart) {                                                                     \
            emit_word(lc, L, st, font, link, wstart, (size_t)((upto) - wstart), next_brk); \
            wstart = NULL;                                                                \
            next_brk = false;                                                             \
        }                                                                                 \
    } while (0)

    while (s < end) {
        uint32_t    cp;
        int         k = utf8_decode(s, end, &cp);
        bool        is_nl = cp == '\n';
        bool        is_ws = cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f' || cp == '\n';
        if (collapse && is_ws && !(is_nl && keep_nl)) {
            FLUSH_WORD(s);
            if (!(L->n && L->v[L->n - 1].kind == IT_SPACE && L->v[L->n - 1].style == st)) emit_space(lc, L, st, font, link, 1, false);
            s += k;
            continue;
        }
        if (is_nl && keep_nl) {
            FLUSH_WORD(s);
            Item it;
            memset(&it, 0, sizeof it);
            it.kind = IT_BREAK;
            it.style = st;
            it.font = font;
            il_push(lc, L, it);
            s += k;
            continue;
        }
        if (cp == '\r') { FLUSH_WORD(s); s += k; continue; }
        if (!collapse && (cp == ' ' || cp == '\t')) {
            FLUSH_WORD(s);
            int count = 0;
            while (s < end && (*s == ' ' || *s == '\t')) {
                count += *s == '\t' ? 8 : 1;
                s++;
            }
            emit_space(lc, L, st, font, link, count, true);
            continue;
        }
        if (cp == 0xAD) { /* soft hyphen: invisible break opportunity */
            FLUSH_WORD(s);
            next_brk = true;
            s += k;
            continue;
        }
        if (is_cjk(cp)) {
            FLUSH_WORD(s);
            emit_word(lc, L, st, font, link, s, (size_t)k, true);
            s += k;
            continue;
        }
        if (!wstart) wstart = s;
        s += k;
        if (cp == '-' && s < end && is_alpha((unsigned char)*s) && wstart < s - 1) { /* break after an inner hyphen */
            FLUSH_WORD(s);
            next_brk = true;
        }
    }
    FLUSH_WORD(end);
#undef FLUSH_WORD
}

static bool needs_atom(const Node *n, const Style *st) {
    return repl_kind_for(n) != RP_NONE || st->display == DISP_INLINE_BLOCK || st->display == DISP_INLINE_FLEX;
}


/* ---- absolutely positioned boxes: attached to their containing block ---- */
static Box *build_block_box(LC *lc, const Node *n, int depth);

static void abs_attach(Box *cb, Box *a) {
    a->abs_next = NULL;
    /* keep the list ordered by z-index (stable) so painting order is correct */
    if (!cb->abs_first) { cb->abs_first = cb->abs_last = a; return; }
    if (a->style->z_index >= cb->abs_last->style->z_index) { cb->abs_last->abs_next = a; cb->abs_last = a; return; }
    Box **pp = &cb->abs_first;
    while (*pp && (*pp)->style->z_index <= a->style->z_index) pp = &(*pp)->abs_next;
    a->abs_next = *pp;
    *pp = a;
    if (!a->abs_next) cb->abs_last = a;
}

/* during tree building: use the explicit stack */
static void register_abs_build(LC *lc, const Node *n, int depth) {
    Box *a = build_block_box(lc, n, depth + 1);
    a->bfc = true;
    Box *cb = lc->root;
    if (n->style->position == POS_ABSOLUTE)
        for (int i = lc->ncb - 1; i >= 0; i--) { cb = lc->cb_stack[i]; break; }
    a->fixed = n->style->position == POS_FIXED;
    if (cb) abs_attach(cb, a);
}

/* at item-collection time: walk the finished box tree upwards */
static void register_abs_late(LC *lc, Box *owner, const Node *n, int depth) {
    Box *a = build_block_box(lc, n, depth + 1);
    a->bfc = true;
    a->fixed = n->style->position == POS_FIXED;
    Box *cb = lc->root;
    if (!a->fixed)
        for (Box *p = owner; p; p = p->parent)
            if (p->style->position != POS_STATIC && p->kind != BK_ANON) { cb = p; break; }
    if (cb) abs_attach(cb, a);
}

static void collect_node(LC *lc, Box *owner, ItemList *L, const Node *n, const Node *link, int depth) {
    if (depth > MAX_BOX_DEPTH) return;
    if (n->type == NODE_TEXT) {
        if (n->style) collect_text(lc, L, n, n->style, link);
        return;
    }
    if (n->type != NODE_ELEMENT || !n->style) return;
    const Style *st = n->style;
    if (st->display == DISP_NONE) return;
    if (is_positioned_out(st)) { register_abs_late(lc, owner, n, depth); return; }
    if (!strcmp(n->tag, "br")) {
        Item it;
        memset(&it, 0, sizeof it);
        it.kind = IT_BREAK;
        it.style = st;
        it.font = layout_style_font(st);
        il_push(lc, L, it);
        return;
    }
    if (st->display == DISP_CONTENTS) {
        for (const Node *c = n->first_child; c; c = c->next) collect_node(lc, owner, L, c, link, depth + 1);
        return;
    }
    if (needs_atom(n, st)) {
        Box *a = build_block_box(lc, n, depth + 1);
        a->atom = true;
        a->bfc = true;
        a->atom_link = link;
        Item it;
        memset(&it, 0, sizeof it);
        it.kind = IT_ATOM;
        it.style = st;
        it.box = a;
        it.link = link;
        il_push(lc, L, it);
        return;
    }
    const Node *nlink = (!strcmp(n->tag, "a") && dom_attr(n, "href")) ? n : link;
    Item        open;
    memset(&open, 0, sizeof open);
    open.kind = IT_OPEN;
    open.style = st;
    open.link = nlink;
    il_push(lc, L, open);
    for (const Node *c = n->first_child; c; c = c->next) collect_node(lc, owner, L, c, nlink, depth + 1);
    open.kind = IT_CLOSE;
    il_push(lc, L, open);
}

static void ensure_items(LC *lc, Box *b) {
    if (b->items_built) return;
    b->items_built = true;
    ItemList L = {0};
    for (int i = 0; i < b->ninl; i++) {
        const Node *n = b->inl[i];
        const Node *link = NULL;
        for (const Node *p = n->parent; p; p = p->parent) /* inherited link context for text directly under <a> boxes */
            if (dom_is(p, "a") && dom_attr(p, "href")) { link = p; break; }
        collect_node(lc, b, &L, n, link, 0);
    }
    b->items = L.v;
    b->nitems = L.n;
}

/* ============================================================ inline layout == */
typedef struct {
    const Style *style;
    float        start_x;
    bool         first;
} OpenSpan;

typedef struct {
    const Style *style;
    float        x0, x1;
    bool         first, last;
} LineSpan;

#define MAX_OPEN 64
#define MAX_LSPANS 128

typedef struct {
    LC       *lc;
    Box      *b;
    float     avail;
    bool      emit;
    bool      wrap;
    float     y, cur_x, max_ext;
    int       frag0;
    bool      has_content, pending_space;
    float     space_w;
    const Style *space_style;
    BhFont     *space_font;
    const Node *space_link;
    bool      first_line;
    float     indent;
    OpenSpan  open[MAX_OPEN];
    int       nopen;
    LineSpan  lspans[MAX_LSPANS];
    int       nlspans;
    bool      run_active;
    Buf       run;
    const Style *run_style;
    const Node *run_link;
    BhFont     *run_font;
    float     run_x, run_w;
    float     asc, desc;
    int       nlines;
    FloatList *fl;
    float     fox, foy, line_left, line_right, strut_h;
} LS;

static void flush_run(LS *ls) {
    if (!ls->run_active) return;
    if (ls->emit && ls->run.len) {
        Frag f;
        memset(&f, 0, sizeof f);
        f.x = ls->run_x;
        f.w = ls->run_w;
        f.text = arena_strndup(ls->lc->a, ls->run.data, ls->run.len);
        f.len = (int)ls->run.len;
        f.style = ls->run_style;
        f.font = ls->run_font;
        f.link = ls->run_link;
        VPUSH(ls->lc->a, ls->b->frags, ls->b->nfrags, ls->b->capfrags, Frag, f);
    }
    buf_clear(&ls->run);
    ls->run_active = false;
}

static void add_text(LS *ls, const char *text, int len, float w, const Style *st, BhFont *font, const Node *link) {
    if (ls->run_active && (ls->run_style != st || ls->run_link != link || ls->run_font != font)) flush_run(ls);
    if (!ls->run_active) {
        ls->run_active = true;
        ls->run_style = st;
        ls->run_link = link;
        ls->run_font = font;
        ls->run_x = ls->cur_x;
        ls->run_w = 0;
    }
    if (ls->emit) buf_append(&ls->run, text, (size_t)len);
    else ls->run.len = 1;
    ls->run_w += w;
    ls->cur_x += w;
    float a, d;
    line_metrics(st, font, &a, &d);
    if (st->vertical_align == VA_SUB) d += st->font_size * 0.2f;
    if (st->vertical_align == VA_SUPER) a += st->font_size * 0.35f;
    ls->asc = fmaxf_(ls->asc, a);
    ls->desc = fmaxf_(ls->desc, d);
    ls->has_content = true;
}

static void line_bounds(LS *ls) {
    ls->line_left = 0;
    ls->line_right = ls->avail;
    if (!ls->fl || !ls->fl->n) return;
    float top = ls->foy + ls->y, bot = top + (ls->strut_h > 1 ? ls->strut_h : 1);
    for (int i = 0; i < ls->fl->n; i++) {
        const FloatBox *f = &ls->fl->v[i];
        if (f->y >= bot || f->y + f->h <= top) continue;
        if (f->left) { if (f->x + f->w - ls->fox > ls->line_left) ls->line_left = f->x + f->w - ls->fox; }
        else if (f->x - ls->fox < ls->line_right) ls->line_right = f->x - ls->fox;
    }
    if (ls->line_right < ls->line_left) ls->line_right = ls->line_left;
}

static void finalize_line(LS *ls, bool force) {
    flush_run(ls);
    if (!ls->has_content && !force) {
        line_bounds(ls);
        ls->cur_x = ls->line_left;
        ls->first_line = false;
        for (int i = 0; i < ls->nopen; i++) {
            ls->open[i].start_x = 0;
            ls->open[i].first = false;
        }
        ls->nlspans = 0;
        return;
    }
    Box *b = ls->b;
    BhFont *sf = layout_style_font(b->style);
    float sa, sd;
    line_metrics(b->style, sf, &sa, &sd);
    float asc = fmaxf_(sa, ls->asc), desc = fmaxf_(sd, ls->desc);
    ls->max_ext = fmaxf_(ls->max_ext, ls->cur_x);

    /* close the spans still open on this line */
    for (int i = 0; i < ls->nopen && ls->nlspans < MAX_LSPANS; i++) {
        LineSpan *sp = &ls->lspans[ls->nlspans++];
        sp->style = ls->open[i].style;
        sp->x0 = ls->open[i].start_x;
        sp->x1 = ls->cur_x;
        sp->first = ls->open[i].first;
        sp->last = false;
    }
    if (ls->emit) {
        float baseline = ls->y + asc;
        float free_w = ls->line_right - ls->cur_x;
        float dx = 0;
        if (free_w > 0) {
            if (b->style->text_align == TA_CENTER) dx = free_w / 2;
            else if (b->style->text_align == TA_RIGHT) dx = free_w;
        }
        for (int i = ls->frag0; i < b->nfrags; i++) {
            Frag *f = &b->frags[i];
            f->x += dx + b->cx;
            if (f->box) {
                Box *a = f->box;
                float boff = a->baseline >= 0 ? a->baseline : a->h;
                a->x = f->x + a->m[3];
                a->y = b->cy + baseline - boff;
                f->y = b->cy + baseline;
            } else {
                float shift = 0;
                if (f->style->vertical_align == VA_SUB) shift = f->style->font_size * 0.2f;
                else if (f->style->vertical_align == VA_SUPER) shift = -f->style->font_size * 0.35f;
                f->y = b->cy + baseline + shift;
            }
        }
        for (int i = 0; i < ls->nlspans; i++) {
            LineSpan    *sp = &ls->lspans[i];
            const Style *st = sp->style;
            BhFont        *f = layout_style_font(st);
            float        pt = fmaxf_(0, len_resolve(&st->padding[0], ls->avail, 0)), pb = fmaxf_(0, len_resolve(&st->padding[2], ls->avail, 0));
            float        bt = st->border_style[0] != BS_NONE ? st->border_w[0] : 0, bb = st->border_style[2] != BS_NONE ? st->border_w[2] : 0;
            Span         s;
            s.style = st;
            s.x = sp->x0 + dx + b->cx;
            s.w = sp->x1 - sp->x0;
            s.y = b->cy + baseline - font_ascent(f) - pt - bt;
            s.h = font_ascent(f) + font_descent(f) + pt + pb + bt + bb;
            s.first = sp->first;
            s.last = sp->last;
            if (s.w > 0) VPUSH(ls->lc->a, b->spans, b->nspans, b->capspans, Span, s);
        }
        if (ls->nlines == 0) b->baseline = b->cy + baseline;
    }
    ls->y += asc + desc;
    ls->nlines++;
    line_bounds(ls);
    ls->cur_x = ls->line_left;
    ls->first_line = false;
    ls->has_content = false;
    ls->pending_space = false;
    ls->asc = ls->desc = 0;
    ls->frag0 = ls->b->nfrags;
    ls->nlspans = 0;
    for (int i = 0; i < ls->nopen; i++) {
        ls->open[i].start_x = ls->line_left;
        ls->open[i].first = false;
    }
}

static float item_open_w(const Style *st, float avail) {
    return fmaxf_(0, len_resolve(&st->margin[3], avail, 0)) * (st->margin[3].is_auto ? 0 : 1) +
           (st->border_style[3] != BS_NONE ? st->border_w[3] : 0) + fmaxf_(0, len_resolve(&st->padding[3], avail, 0));
}
static float item_close_w(const Style *st, float avail) {
    return (st->margin[1].is_auto ? 0 : len_resolve(&st->margin[1], avail, 0)) +
           (st->border_style[1] != BS_NONE ? st->border_w[1] : 0) + fmaxf_(0, len_resolve(&st->padding[1], avail, 0));
}

static float atom_width(LC *lc, Box *a, float avail, bool emit) {
    if (emit) layout_atom(lc, a, avail);
    else {
        float mn, mx;
        resolve_box_model(a, avail);
        measure_box(lc, a, &mn, &mx);
        a->w = mx;
    }
    return a->w + a->m[1] + a->m[3];
}

static float layout_inline(LC *lc, Box *b, float avail, bool emit, float *extent) {
    ensure_items(lc, b);
    LS *ls = xcalloc(1, sizeof *ls);
    ls->lc = lc;
    ls->b = b;
    ls->avail = avail;
    ls->emit = emit;
    ls->wrap = b->style->white_space == WS_NORMAL || b->style->white_space == WS_PRE_WRAP || b->style->white_space == WS_PRE_LINE;
    ls->first_line = true;
    buf_init(&ls->run);
    if (emit) {
        b->frags = NULL; b->nfrags = b->capfrags = 0;
        b->spans = NULL; b->nspans = b->capspans = 0;
    }
    float indent = len_resolve(&(Length){b->style->text_indent, 0, false}, avail, 0);
    {
        BhFont *sf0 = layout_style_font(b->style);
        float sa0, sd0;
        line_metrics(b->style, sf0, &sa0, &sd0);
        ls->strut_h = sa0 + sd0;
    }
    ls->fl = emit ? lc->fl : NULL;
    ls->fox = lc->fl_ox;
    ls->foy = lc->fl_oy;
    line_bounds(ls);
    ls->cur_x = ls->line_left + indent;

    int n = b->nitems;
    for (int i = 0; i < n;) {
        Item *it = &b->items[i];
        if (it->kind == IT_BREAK) {
            finalize_line(ls, true);
            i++;
            continue;
        }
        if (it->kind == IT_SPACE) {
            if (it->preserved || ls->has_content) {
                if (it->preserved && !ls->has_content) {
                    /* leading preserved spaces are real */
                    add_text(ls, "                                ", it->len > 32 ? 32 : it->len, it->w, it->style, it->font, it->link);
                } else {
                    ls->pending_space = true;
                    ls->space_w = it->w;
                    ls->space_style = it->style;
                    ls->space_font = it->font;
                    ls->space_link = it->link;
                }
            }
            i++;
            continue;
        }
        /* a group of items that cannot be separated */
        int   j = i + 1;
        while (j < n && b->items[j].kind != IT_SPACE && b->items[j].kind != IT_BREAK && !b->items[j].brk_before) j++;
        float gw = 0;
        for (int k = i; k < j; k++) {
            Item *g = &b->items[k];
            switch (g->kind) {
            case IT_WORD: gw += g->w; break;
            case IT_OPEN: gw += item_open_w(g->style, avail); break;
            case IT_CLOSE: gw += item_close_w(g->style, avail); break;
            case IT_ATOM: gw += atom_width(lc, g->box, avail, emit); break;
            default: break;
            }
        }
        float need = gw + (ls->has_content && ls->pending_space ? ls->space_w : 0);
        if (ls->fl && ls->fl->n && ls->wrap && !ls->has_content && need <= avail && ls->cur_x + need > ls->line_right + 0.5f) {
            /* the line is squeezed by floats: slide down to the first float bottom and retry */
            float top = ls->foy + ls->y, nb = LAYOUT_INF;
            for (int fi = 0; fi < ls->fl->n; fi++) {
                const FloatBox *f = &ls->fl->v[fi];
                if (f->y < top + ls->strut_h && f->y + f->h > top && f->y + f->h < nb) nb = f->y + f->h;
            }
            if (nb < LAYOUT_INF && nb > top) {
                ls->y = nb - ls->foy;
                line_bounds(ls);
                ls->cur_x = ls->line_left;
                continue;
            }
        }
        if (ls->wrap && ls->has_content && ls->cur_x + need > ls->line_right + 0.5f) finalize_line(ls, false);
        if (ls->has_content && ls->pending_space) {
            add_text(ls, " ", 1, ls->space_w, ls->space_style, ls->space_font, ls->space_link);
        }
        ls->pending_space = false;
        for (int k = i; k < j; k++) {
            Item *g = &b->items[k];
            switch (g->kind) {
            case IT_WORD: add_text(ls, g->text, g->len, g->w, g->style, g->font, g->link); break;
            case IT_OPEN: {
                flush_run(ls);
                float ml = g->style->margin[3].is_auto ? 0 : len_resolve(&g->style->margin[3], avail, 0);
                ls->cur_x += ml;
                if (ls->nopen < MAX_OPEN) {
                    ls->open[ls->nopen].style = g->style;
                    ls->open[ls->nopen].start_x = ls->cur_x;
                    ls->open[ls->nopen].first = true;
                    ls->nopen++;
                }
                ls->cur_x += item_open_w(g->style, avail) - ml;
                break;
            }
            case IT_CLOSE: {
                flush_run(ls);
                float cw = item_close_w(g->style, avail);
                float mr = g->style->margin[1].is_auto ? 0 : len_resolve(&g->style->margin[1], avail, 0);
                ls->cur_x += cw - mr;
                if (ls->nopen > 0) {
                    OpenSpan *o = &ls->open[--ls->nopen];
                    if (ls->nlspans < MAX_LSPANS) {
                        LineSpan *sp = &ls->lspans[ls->nlspans++];
                        sp->style = o->style;
                        sp->x0 = o->start_x;
                        sp->x1 = ls->cur_x;
                        sp->first = o->first;
                        sp->last = true;
                    }
                }
                ls->cur_x += mr;
                break;
            }
            case IT_ATOM: {
                flush_run(ls);
                Box *a = g->box;
                float w = a->w + a->m[1] + a->m[3];
                if (emit) {
                    Frag f;
                    memset(&f, 0, sizeof f);
                    f.x = ls->cur_x;
                    f.w = w;
                    f.box = a;
                    f.style = g->style;
                    f.link = g->link;
                    VPUSH(lc->a, b->frags, b->nfrags, b->capfrags, Frag, f);
                }
                ls->cur_x += w;
                float boff = a->baseline >= 0 ? a->baseline : a->h;
                ls->asc = fmaxf_(ls->asc, boff + a->m[0]);
                ls->desc = fmaxf_(ls->desc, a->h - boff + a->m[2]);
                ls->has_content = true;
                break;
            }
            default: break;
            }
        }
        i = j;
    }
    if (ls->has_content) finalize_line(ls, false);
    float h = ls->y;
    if (extent) *extent = ls->max_ext;
    buf_free(&ls->run);
    free(ls);
    return h;
}

/* =================================================================== tables == */
typedef struct {
    Box  *box;
    int   row, col, cspan, rspan;
    float min, max, pref, pct;
} Cell;

static int int_attr(const Node *n, const char *name, int dflt, int lo, int hi) {
    const char *v = dom_attr(n, name);
    if (!v) return dflt;
    int x = atoi(v);
    return x < lo ? lo : x > hi ? hi : x;
}

#define MAX_COLS 256
#define MAX_ROWS 20000

typedef struct {
    Box **rows;
    int   nrows, caprows;
    Box  *caption;
} TableRows;

static void gather_rows(LC *lc, Box *tb, TableRows *tr) {
    for (Box *c = tb->first; c; c = c->next) {
        int d = c->style->display;
        if (d == DISP_TABLE_CAPTION) { if (!tr->caption) tr->caption = c; }
        else if (d == DISP_TABLE_ROW) { if (tr->nrows < MAX_ROWS) VPUSH(lc->a, tr->rows, tr->nrows, tr->caprows, Box *, c); }
        else if (d == DISP_TABLE_ROW_GROUP) {
            for (Box *r = c->first; r; r = r->next)
                if (r->style->display == DISP_TABLE_ROW && tr->nrows < MAX_ROWS) VPUSH(lc->a, tr->rows, tr->nrows, tr->caprows, Box *, r);
        }
    }
}

/* Lays out a table box. Returns content height; computes b->w / cw. */
static float layout_table_content(LC *lc, Box *tb, float avail_content_w, bool fixed_w, bool measure, float *w_out, float *min_out) {
    TableRows tr = {0};
    gather_rows(lc, tb, &tr);
    const Style *st = tb->style;
    float spacing = st->border_collapse ? 0 : st->border_spacing;

    /* place cells in the grid */
    Cell *cells = NULL;
    int   ncells = 0, capcells = 0;
    int   ncols = 0;
    int   span_left[MAX_COLS];
    memset(span_left, 0, sizeof span_left);
    for (int r = 0; r < tr.nrows; r++) {
        int col = 0;
        for (Box *c = tr.rows[r]->first; c; c = c->next) {
            if (c->style->display != DISP_TABLE_CELL) continue;
            while (col < MAX_COLS && span_left[col] > 0) col++;
            if (col >= MAX_COLS) break;
            Cell ce;
            memset(&ce, 0, sizeof ce);
            ce.box = c;
            ce.row = r;
            ce.col = col;
            ce.cspan = int_attr(c->node, "colspan", 1, 1, 64);
            ce.rspan = int_attr(c->node, "rowspan", 1, 1, 256);
            if (ce.col + ce.cspan > MAX_COLS) ce.cspan = MAX_COLS - ce.col;
            for (int k = 0; k < ce.cspan; k++) span_left[ce.col + k] = ce.rspan;
            col += ce.cspan;
            if (col > ncols) ncols = col;
            VPUSH(lc->a, cells, ncells, capcells, Cell, ce);
        }
        for (int k = 0; k < MAX_COLS; k++)
            if (span_left[k] > 0) span_left[k]--;
    }
    float *colmin = arena_alloc(lc->a, sizeof(float) * (size_t)(ncols + 1));
    float *colmax = arena_alloc(lc->a, sizeof(float) * (size_t)(ncols + 1));
    float *colpct = arena_alloc(lc->a, sizeof(float) * (size_t)(ncols + 1));
    float *colw = arena_alloc(lc->a, sizeof(float) * (size_t)(ncols + 1));

    for (int i = 0; i < ncells; i++) {
        Cell *ce = &cells[i];
        float mn, mx;
        resolve_box_model(ce->box, avail_content_w);
        measure_box(lc, ce->box, &mn, &mx);
        const Length *wl = &ce->box->style->width;
        ce->pref = -1;
        ce->pct = 0;
        if (!wl->is_auto) {
            if (wl->pct > 0 && wl->px == 0) ce->pct = wl->pct;
            else if (wl->pct == 0) {
                float hpad = ce->box->p[1] + ce->box->p[3] + ce->box->bw[1] + ce->box->bw[3];
                ce->pref = wl->px + (ce->box->style->box_sizing_border ? 0 : hpad);
            }
        }
        ce->min = mn;
        ce->max = ce->pref >= 0 ? fmaxf_(ce->pref, mn) : mx;
        if (ce->pref >= 0) ce->min = fmaxf_(mn, fminf_(ce->pref, mx));
    }
    /* single-column cells first, then spanning cells */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < ncells; i++) {
            Cell *ce = &cells[i];
            if ((ce->cspan > 1) != (pass == 1)) continue;
            if (pass == 0) {
                colmin[ce->col] = fmaxf_(colmin[ce->col], ce->min);
                colmax[ce->col] = fmaxf_(colmax[ce->col], ce->max);
                colpct[ce->col] = fmaxf_(colpct[ce->col], ce->pct);
            } else {
                float smin = 0, smax = 0;
                for (int k = 0; k < ce->cspan; k++) { smin += colmin[ce->col + k]; smax += colmax[ce->col + k]; }
                smin += spacing * (float)(ce->cspan - 1);
                smax += spacing * (float)(ce->cspan - 1);
                if (ce->min > smin) for (int k = 0; k < ce->cspan; k++) colmin[ce->col + k] += (ce->min - smin) / (float)ce->cspan;
                if (ce->max > smax) for (int k = 0; k < ce->cspan; k++) colmax[ce->col + k] += (ce->max - smax) / (float)ce->cspan;
                if (ce->pct > 0) for (int k = 0; k < ce->cspan; k++) colpct[ce->col + k] = fmaxf_(colpct[ce->col + k], ce->pct / (float)ce->cspan);
            }
        }
    for (int c = 0; c < ncols; c++) if (colmax[c] < colmin[c]) colmax[c] = colmin[c];

    float sum_min = 0, sum_max = 0;
    for (int c = 0; c < ncols; c++) { sum_min += colmin[c]; sum_max += colmax[c]; }
    float spacing_total = spacing * (float)(ncols + 1);
    float natural = sum_max + spacing_total;
    float target;
    if (measure) {
        if (w_out) *w_out = natural;                       /* max-content */
        if (min_out) *min_out = sum_min + spacing_total;   /* min-content */
        return 0;
    }
    if (fixed_w) target = avail_content_w;
    else target = fminf_(natural, avail_content_w);
    if (target < sum_min + spacing_total) target = sum_min + spacing_total;
    float total = target - spacing_total;
    /* distribute */
    float pct_sum = 0;
    for (int c = 0; c < ncols; c++) pct_sum += colpct[c];
    float remaining = total;
    bool *fixed_col = arena_alloc(lc->a, sizeof(bool) * (size_t)(ncols + 1));
    if (pct_sum > 0 && (fixed_w || total > 0)) {
        float scale = pct_sum > 1 ? 1 / pct_sum : 1;
        for (int c = 0; c < ncols; c++)
            if (colpct[c] > 0) {
                colw[c] = fmaxf_(colmin[c], colpct[c] * scale * total);
                remaining -= colw[c];
                fixed_col[c] = true;
            }
    }
    float fmin = 0, fmax = 0;
    for (int c = 0; c < ncols; c++) if (!fixed_col[c]) { fmin += colmin[c]; fmax += colmax[c]; }
    for (int c = 0; c < ncols; c++) {
        if (fixed_col[c]) continue;
        if (remaining >= fmax) {
            colw[c] = colmax[c] + (fixed_w && fmax > 0 ? (remaining - fmax) * colmax[c] / fmax : (fixed_w && fmax == 0 ? 0 : 0));
        } else if (remaining <= fmin || fmax <= fmin) {
            colw[c] = colmin[c];
        } else {
            colw[c] = colmin[c] + (colmax[c] - colmin[c]) * (remaining - fmin) / (fmax - fmin);
        }
    }
    if (fixed_w && fmax == 0) { /* all columns empty: share evenly */
        int nf = 0;
        for (int c = 0; c < ncols; c++) if (!fixed_col[c]) nf++;
        for (int c = 0; c < ncols; c++) if (!fixed_col[c] && nf) colw[c] = remaining / (float)nf;
    }
    float used = spacing_total;
    for (int c = 0; c < ncols; c++) used += colw[c];

    /* column x positions */
    float *colx = arena_alloc(lc->a, sizeof(float) * (size_t)(ncols + 1));
    float x = spacing;
    for (int c = 0; c < ncols; c++) { colx[c] = x; x += colw[c] + spacing; }

    /* caption */
    float y = 0;
    if (tr.caption) {
        layout_block(lc, tr.caption, used, -1);
        tr.caption->x = tb->cx + tr.caption->m[3];
        tr.caption->y = tb->cy + tr.caption->mt_eff;
        y += tr.caption->mt_eff + tr.caption->h + tr.caption->mb_eff;
    }
    y += spacing;

    /* lay out cells */
    float *rowh = arena_alloc(lc->a, sizeof(float) * (size_t)(tr.nrows + 1));
    for (int i = 0; i < ncells; i++) {
        Cell *ce = &cells[i];
        float w = spacing * (float)(ce->cspan - 1);
        for (int k = 0; k < ce->cspan; k++) w += colw[ce->col + k];
        layout_block(lc, ce->box, w, w);
        if (ce->rspan == 1) rowh[ce->row] = fmaxf_(rowh[ce->row], ce->box->h);
    }
    for (int i = 0; i < ncells; i++) { /* rowspan cells enlarge the last row they cover if needed */
        Cell *ce = &cells[i];
        if (ce->rspan <= 1) continue;
        int last = ce->row + ce->rspan - 1;
        if (last >= tr.nrows) last = tr.nrows - 1;
        float have = spacing * (float)(last - ce->row);
        for (int r = ce->row; r <= last; r++) have += rowh[r];
        if (ce->box->h > have) rowh[last] += ce->box->h - have;
    }
    float *rowy = arena_alloc(lc->a, sizeof(float) * (size_t)(tr.nrows + 1));
    for (int r = 0; r < tr.nrows; r++) {
        rowy[r] = y;
        Box *row = tr.rows[r];
        row->x = tb->cx;
        row->y = tb->cy + y;
        row->w = used;
        row->h = rowh[r];
        row->cx = row->cy = 0;
        y += rowh[r] + spacing;
    }
    for (int i = 0; i < ncells; i++) {
        Cell *ce = &cells[i];
        Box  *cb = ce->box;
        int   last = ce->row + ce->rspan - 1;
        if (last >= tr.nrows) last = tr.nrows - 1;
        float span_h = rowy[last] + rowh[last] - rowy[ce->row];
        float off = 0;
        if (cb->style->vertical_align == VA_BOTTOM) off = span_h - cb->h;
        else if (cb->style->vertical_align != VA_TOP) off = (span_h - cb->h) / 2;
        if (off < 0) off = 0;
        cb->x = colx[ce->col];
        cb->y = off;
        if (ce->rspan > 1) cb->h = span_h; /* background fills all spanned rows */
    }
    /* row groups: transparent containers positioned at the table origin */
    for (Box *c = tb->first; c; c = c->next)
        if (c->style->display == DISP_TABLE_ROW_GROUP) { c->x = c->y = 0; c->w = used; c->h = 0; }
    if (w_out) *w_out = used;
    return y;
}


/* position: relative shifts the box after normal layout, without affecting siblings */
static void apply_relative(Box *c, float cb_w, float cb_h) {
    const Style *st = c->style;
    if (st->position != POS_RELATIVE) return;
    float dx = 0, dy = 0;
    if (!st->offset[3].is_auto) dx = len_resolve(&st->offset[3], cb_w, 0);
    else if (!st->offset[1].is_auto) dx = -len_resolve(&st->offset[1], cb_w, 0);
    if (!st->offset[0].is_auto) dy = len_resolve(&st->offset[0], cb_h, 0);
    else if (!st->offset[2].is_auto) dy = -len_resolve(&st->offset[2], cb_h, 0);
    c->x += dx;
    c->y += dy;
}

/* Lays out the absolutely positioned boxes anchored to cb. cb's own geometry must be final. */
static void layout_abs_children(LC *lc, Box *cb) {
    bool  is_root = cb == lc->root;
    float pw = is_root ? lc->vw : cb->w - cb->bw[1] - cb->bw[3];
    float ph = is_root ? lc->vh : cb->h - cb->bw[0] - cb->bw[2];
    float ox = is_root ? 0 : cb->bw[3], oy = is_root ? 0 : cb->bw[0];
    for (Box *a = cb->abs_first; a; a = a->abs_next) {
        const Style *st = a->style;
        float cw = a->fixed ? lc->vw : pw, chh = a->fixed ? lc->vh : ph;
        resolve_box_model(a, cw);
        bool  l = !st->offset[3].is_auto, r = !st->offset[1].is_auto, t = !st->offset[0].is_auto, b = !st->offset[2].is_auto;
        float left = l ? len_resolve(&st->offset[3], cw, 0) : 0, right = r ? len_resolve(&st->offset[1], cw, 0) : 0;
        float top = t ? len_resolve(&st->offset[0], chh, 0) : 0, bottom = b ? len_resolve(&st->offset[2], chh, 0) : 0;
        float hpad = a->p[1] + a->p[3] + a->bw[1] + a->bw[3];
        if (a->kind == BK_REPLACED) {
            layout_block(lc, a, cw, -1);
        } else {
            float forced;
            if (!st->width.is_auto) forced = len_resolve(&st->width, cw, 0) + (st->box_sizing_border ? 0 : hpad);
            else if (l && r) forced = cw - left - right - a->m[1] - a->m[3];
            else {
                float mn, mx;
                measure_box(lc, a, &mn, &mx);
                float room = cw - (l ? left : 0) - (r ? right : 0) - a->m[1] - a->m[3];
                forced = fminf_(mx, fmaxf_(mn, room));
            }
            if (forced < hpad) forced = hpad;
            layout_block(lc, a, cw, forced);
            if (!st->height.is_auto && st->height.pct == 0) { /* height set explicitly */ }
            else if (t && b && st->height.is_auto) {
                float want = chh - top - bottom - a->m[0] - a->m[2];
                if (want > a->h) a->h = want;
            }
        }
        float x = l ? left + a->m[3] : (r ? cw - right - a->m[1] - a->w : a->m[3]);
        float y = t ? top + a->m[0] : (b ? chh - bottom - a->m[2] - a->h : a->m[0]);
        a->x = ox + x;
        a->y = oy + y;
        a->mt_eff = a->m[0];
        a->mb_eff = a->m[2];
    }
}

/* ================================================================== flexbox === */
typedef struct {
    Box  *b;
    float base, minv, maxv, size;   /* main axis, border-box */
    float ma, mb, ca, cb;           /* main/cross margins (start,end) */
    bool  auto_ma, auto_mb, auto_ca, auto_cb;
    bool  frozen;
    float cross;                    /* outer cross size */
    float cross_inner;              /* border-box cross size */
    float grow, shrink;
    int   line;
} FItem;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void flex_resolve(FItem *it, int n, float target, float gap) {
    /* target = available main size for this line (border-box + margins + gaps) */
    float used_gaps = gap * (float)(n > 0 ? n - 1 : 0);
    for (int i = 0; i < n; i++) it[i].frozen = false, it[i].size = it[i].base;
    for (int iter = 0; iter < n + 2; iter++) {
        float sum_frozen = 0, sum_unfrozen_base = 0, grow_sum = 0, shrink_scaled = 0;
        int   unfrozen = 0;
        for (int i = 0; i < n; i++) {
            float margins = it[i].ma + it[i].mb;
            if (it[i].frozen) sum_frozen += it[i].size + margins;
            else {
                sum_unfrozen_base += it[i].base + margins;
                grow_sum += it[i].grow;
                shrink_scaled += it[i].shrink * it[i].base;
                unfrozen++;
            }
        }
        if (!unfrozen) break;
        float free_space = target - used_gaps - sum_frozen - sum_unfrozen_base;
        for (int i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            float sz = it[i].base;
            if (free_space > 0 && grow_sum > 0) sz += free_space * it[i].grow / (grow_sum < 1 ? 1 : grow_sum) * (grow_sum < 1 ? grow_sum : 1) ;
            else if (free_space < 0 && shrink_scaled > 0) sz += free_space * (it[i].shrink * it[i].base) / shrink_scaled;
            it[i].size = sz;
        }
        /* clamp and freeze violators */
        float total_violation = 0;
        for (int i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            float c = clampf(it[i].size, it[i].minv, it[i].maxv);
            total_violation += c - it[i].size;
            it[i].size = c;
        }
        bool any = false;
        for (int i = 0; i < n; i++) {
            if (it[i].frozen) continue;
            bool freeze = total_violation == 0 ? true : total_violation > 0 ? it[i].size <= it[i].minv + 0.001f : it[i].size >= it[i].maxv - 0.001f;
            /* items that hit their bound keep the clamped value; the rest are re-distributed */
            if (freeze) { it[i].frozen = true; any = true; }
        }
        if (total_violation == 0 || !any) break;
    }
}

static float layout_flex(LC *lc, Box *c, float W, float def_h, float min_h, float max_h) {
    const Style *st = c->style;
    bool  row = st->flex_dir == FD_ROW || st->flex_dir == FD_ROW_REV;
    bool  rev = st->flex_dir == FD_ROW_REV || st->flex_dir == FD_COL_REV;
    float main_gap = row ? st->col_gap : st->row_gap, cross_gap = row ? st->row_gap : st->col_gap;
    int   n = 0;
    for (Box *b = c->first; b; b = b->next) n++;
    if (n == 0) return 0;
    FItem *it = arena_alloc(lc->a, sizeof(FItem) * (size_t)n);
    Box  **order = arena_alloc(lc->a, sizeof(Box *) * (size_t)n);
    {
        int i = 0;
        for (Box *b = c->first; b; b = b->next) order[i++] = b;
        if (rev) for (int a = 0, z = n - 1; a < z; a++, z--) { Box *t = order[a]; order[a] = order[z]; order[z] = t; }
    }
    float main_avail = row ? W : def_h; /* column: <0 means indefinite */
    for (int i = 0; i < n; i++) {
        Box *b = order[i];
        const Style *is = b->style;
        FItem *f = &it[i];
        f->b = b;
        b->bfc = true;
        if (b->kind == BK_REPLACED) { size_replaced(lc, b, W); } else resolve_box_model(b, W);
        resolve_box_model(b, W);
        float hpad = b->p[1] + b->p[3] + b->bw[1] + b->bw[3], vpad = b->p[0] + b->p[2] + b->bw[0] + b->bw[2];
        f->auto_ma = is->margin[row ? 3 : 0].is_auto; f->auto_mb = is->margin[row ? 1 : 2].is_auto;
        f->auto_ca = is->margin[row ? 0 : 3].is_auto; f->auto_cb = is->margin[row ? 2 : 1].is_auto;
        f->ma = f->auto_ma ? 0 : b->m[row ? 3 : 0]; f->mb = f->auto_mb ? 0 : b->m[row ? 1 : 2];
        f->ca = f->auto_ca ? 0 : b->m[row ? 0 : 3]; f->cb = f->auto_cb ? 0 : b->m[row ? 2 : 1];
        f->grow = is->flex_grow;
        f->shrink = is->flex_shrink;
        float mn, mx;
        measure_box(lc, b, &mn, &mx);
        if (row) {
            float pad = is->box_sizing_border ? 0 : hpad;
            if (!is->flex_basis.is_auto) f->base = len_resolve(&is->flex_basis, W, 0) + pad;
            else if (!is->width.is_auto) f->base = len_resolve(&is->width, W, 0) + pad;
            else f->base = mx;
            f->minv = !is->min_width.is_auto ? len_resolve(&is->min_width, W, 0) + pad : (is->overflow != OV_VISIBLE ? 0 : fminf_(mn, f->base > 0 ? fmaxf_(f->base, 0) + 0 : mn));
            if (is->width.is_auto && is->flex_basis.is_auto) f->minv = !is->min_width.is_auto ? f->minv : mn;
            f->maxv = !is->max_width.is_auto ? len_resolve(&is->max_width, W, LAYOUT_INF) + pad : LAYOUT_INF;
            if (f->base < hpad) f->base = hpad;
            if (f->minv < hpad) f->minv = hpad;
        } else {
            /* column: need the content height at the final cross width */
            float cw = W - f->ca - f->cb;
            bool stretch = (is->align_self == AI_AUTO ? st->align_items : is->align_self) == AI_STRETCH;
            if (!stretch || !is->width.is_auto) {
                float bw_ = !is->width.is_auto ? len_resolve(&is->width, W, 0) + (is->box_sizing_border ? 0 : hpad) : fminf_(mx, cw);
                cw = bw_;
            }
            if (b->kind == BK_REPLACED) { size_replaced(lc, b, W); }
            else layout_block(lc, b, W, cw);
            float pad = is->box_sizing_border ? 0 : vpad;
            f->cross_inner = b->w;
            f->base = !is->flex_basis.is_auto ? len_resolve(&is->flex_basis, 0, 0) + pad : (!is->height.is_auto && is->height.pct == 0 ? is->height.px + pad : b->h);
            f->minv = !is->min_height.is_auto && is->min_height.pct == 0 ? is->min_height.px + pad : (is->height.is_auto ? b->h * 0 + (is->flex_basis.is_auto ? b->h : 0) : 0);
            f->maxv = !is->max_height.is_auto && is->max_height.pct == 0 ? is->max_height.px + pad : LAYOUT_INF;
            if (f->base < vpad) f->base = vpad;
            if (f->minv < vpad) f->minv = vpad;
        }
        if (f->minv > f->maxv) f->minv = f->maxv;
    }

    /* split into lines */
    int  nlines = 1;
    bool wrap = st->flex_wrap && row;
    it[0].line = 0;
    if (wrap) {
        float acc = 0;
        for (int i = 0; i < n; i++) {
            float outer = clampf(it[i].base, it[i].minv, it[i].maxv) + it[i].ma + it[i].mb;
            float need = outer + (acc > 0 ? main_gap : 0);
            if (acc > 0 && acc + need > W + 0.01f) { nlines++; acc = 0; need = outer; }
            it[i].line = nlines - 1;
            acc += need;
        }
    }
    float *line_cross = arena_alloc(lc->a, sizeof(float) * (size_t)nlines);
    int    start = 0;
    float  total_cross = 0;
    for (int ln = 0; ln < nlines; ln++) {
        int end = start;
        while (end < n && it[end].line == ln) end++;
        int cnt = end - start;
        FItem *L = &it[start];
        /* main sizes */
        float target = main_avail;
        if (target < 0) { /* indefinite column: no growing/shrinking, use hypothetical sizes */
            for (int i = 0; i < cnt; i++) L[i].size = clampf(L[i].base, L[i].minv, L[i].maxv);
        } else {
            flex_resolve(L, cnt, target, main_gap);
        }
        /* lay items out at their final main size and measure the cross size */
        float lcross = 0;
        for (int i = 0; i < cnt; i++) {
            Box *b = L[i].b;
            const Style *is = b->style;
            if (row) {
                if (b->kind == BK_REPLACED) { size_replaced(lc, b, W); if (is->width.is_auto) b->w = L[i].size; }
                else layout_block(lc, b, W, L[i].size);
                L[i].cross_inner = b->h;
                L[i].cross = b->h + L[i].ca + L[i].cb;
            } else {
                L[i].cross = L[i].cross_inner + L[i].ca + L[i].cb;
            }
            lcross = fmaxf_(lcross, L[i].cross);
        }
        if (nlines == 1) {
            if (row) {
                if (def_h >= 0) lcross = def_h;
                else lcross = clampf(lcross, min_h > 0 ? min_h : 0, max_h >= 0 ? max_h : LAYOUT_INF);
            } else lcross = W; /* column: the cross axis is the container width */
        }
        line_cross[ln] = lcross;
        total_cross += lcross;
        /* main axis distribution */
        float used = main_gap * (float)(cnt > 0 ? cnt - 1 : 0);
        int   autos = 0;
        for (int i = 0; i < cnt; i++) {
            used += L[i].size + L[i].ma + L[i].mb;
            autos += (L[i].auto_ma ? 1 : 0) + (L[i].auto_mb ? 1 : 0);
        }
        float avail_main = main_avail >= 0 ? main_avail : used;
        float freeS = fmaxf_(0, avail_main - used);
        float lead = 0, between = main_gap;
        float auto_share = 0;
        if (autos) auto_share = freeS / (float)autos;
        else switch (st->justify) {
            case JC_END: lead = freeS; break;
            case JC_CENTER: lead = freeS / 2; break;
            case JC_BETWEEN: if (cnt > 1) between += freeS / (float)(cnt - 1); break;
            case JC_AROUND: lead = freeS / (float)cnt / 2; between += freeS / (float)cnt; break;
            case JC_EVENLY: lead = freeS / (float)(cnt + 1); between += freeS / (float)(cnt + 1); break;
            default: break;
        }
        float pos = lead;
        float ly = total_cross - lcross + cross_gap * (float)ln; /* cross offset of this line */
        for (int i = 0; i < cnt; i++) {
            Box *b = L[i].b;
            const Style *is = b->style;
            if (L[i].auto_ma) L[i].ma = auto_share;
            if (L[i].auto_mb) L[i].mb = auto_share;
            float main_pos = pos + L[i].ma;
            /* cross alignment */
            uint8_t al = is->align_self == AI_AUTO ? st->align_items : is->align_self;
            float   cross_avail = lcross - L[i].ca - L[i].cb;
            float   coff = L[i].ca;
            if (L[i].auto_ca || L[i].auto_cb) {
                float fs = fmaxf_(0, lcross - L[i].cross_inner - L[i].ca - L[i].cb);
                int   na = (L[i].auto_ca ? 1 : 0) + (L[i].auto_cb ? 1 : 0);
                if (L[i].auto_ca) coff += fs / (float)na;
            } else if (al == AI_END) coff += cross_avail - L[i].cross_inner;
            else if (al == AI_CENTER) coff += (cross_avail - L[i].cross_inner) / 2;
            else if (al == AI_STRETCH || al == AI_AUTO) {
                bool auto_cross = row ? (is->height.is_auto || is->height.pct != 0) : is->width.is_auto;
                if (auto_cross && b->kind != BK_REPLACED) {
                    if (row) { b->h = fmaxf_(b->h, cross_avail); L[i].cross_inner = b->h; }
                    else if (cross_avail > b->w) { /* column: re-lay out wider so content reflows */
                        layout_block(lc, b, W, cross_avail);
                        L[i].cross_inner = b->w;
                    }
                }
            }
            if (row) {
                b->x = c->cx + main_pos;
                b->y = c->cy + ly + coff;
                if (L[i].size > b->w && b->kind == BK_REPLACED) b->w = L[i].size;
            } else {
                b->x = c->cx + ly + coff;
                b->y = c->cy + main_pos;
                b->h = L[i].size; /* the main size is the height */
            }
            b->mt_eff = b->m[0];
            b->mb_eff = b->m[2];
            apply_relative(b, W, 0);
            pos = main_pos + L[i].size + L[i].mb + between;
        }
        start = end;
    }
    if (row) return total_cross + cross_gap * (float)(nlines - 1);
    /* column: height = main extent */
    float extent = 0;
    for (int i = 0; i < n; i++) extent = fmaxf_(extent, it[i].b->y - c->cy + it[i].b->h + it[i].mb);
    if (def_h >= 0) extent = fmaxf_(extent, def_h);
    return extent;
}


/* ================================================================== floats ===== */
static float fl_clear_bottom(const FloatList *fl, int mask) {
    float m = -LAYOUT_INF;
    if (fl)
        for (int i = 0; i < fl->n; i++)
            if ((fl->v[i].left && (mask & 1)) || (!fl->v[i].left && (mask & 2))) m = fmaxf_(m, fl->v[i].y + fl->v[i].h);
    return m;
}

/* Lays out a floated child and registers it. y_in_parent is where the float would start in the parent's
 * content space. Returns the float's bottom edge in the parent's content space. */
static float place_float(LC *lc, Box *b, Box *c, float content_w, float y_in_parent) {
    FloatList *fl = lc->fl;
    resolve_box_model(c, content_w);
    float forced;
    const Style *cs = c->style;
    float hpad = c->p[1] + c->p[3] + c->bw[1] + c->bw[3];
    if (c->kind == BK_REPLACED) forced = -1;
    else if (!cs->width.is_auto) forced = len_resolve(&cs->width, content_w, 0) + (cs->box_sizing_border ? 0 : hpad);
    else {
        float mn, mx;
        measure_box(lc, c, &mn, &mx);
        forced = fminf_(mx, fmaxf_(mn, content_w - c->m[1] - c->m[3]));
    }
    lc->child_dx = c->m[3];
    lc->child_dy = y_in_parent + c->m[0];
    layout_block(lc, c, content_w, forced);
    float fw = c->w + c->m[1] + c->m[3], fh = c->h + c->m[0] + c->m[2];
    bool  left = cs->float_ == FL_LEFT;
    float pox = lc->fl_ox, poy = lc->fl_oy;
    float y = poy + y_in_parent;
    if (y < fl->last_top) y = fl->last_top;
    float x = pox;
    for (int guard = 0; guard < 64; guard++) {
        float L = pox, R = pox + content_w, nb = LAYOUT_INF;
        bool  overlap = false;
        for (int i = 0; i < fl->n; i++) {
            const FloatBox *f = &fl->v[i];
            if (f->y >= y + fh || f->y + f->h <= y) continue;
            overlap = true;
            if (f->left) L = fmaxf_(L, f->x + f->w); else R = fminf_(R, f->x);
            if (f->y + f->h < nb) nb = f->y + f->h;
        }
        if (!overlap || fw <= R - L + 0.01f) { x = left ? L : R - fw; break; }
        if (nb >= LAYOUT_INF) { x = left ? L : R - fw; break; }
        y = nb;
    }
    FloatBox fb = {x, y, fw, fh, left};
    VPUSH(lc->a, fl->v, fl->n, fl->cap, FloatBox, fb);
    fl->last_top = y;
    c->x = b->cx + (x - pox) + c->m[3];
    c->y = b->cy + (y - poy) + c->m[0];
    c->mt_eff = c->m[0];
    c->mb_eff = c->m[2];
    return (y - poy) + fh;
}

/* =============================================================== block layout == */
static float resolve_content_min(const Style *st, const Length *l, float cb_w, float hpad, float dflt) {
    if (l->is_auto) return dflt;
    float v = len_resolve(l, cb_w, dflt);
    if (st->box_sizing_border) v -= hpad;
    return v;
}

static void layout_block_inner(LC *lc, Box *b, float cb_w, float forced_bw);
static void layout_block_inner(LC *lc, Box *b, float cb_w, float forced_bw) {
    const Style *st = b->style;
    if (b->kind == BK_REPLACED) {
        size_replaced(lc, b, cb_w);
        b->mt_eff = b->m[0];
        b->mb_eff = b->m[2];
        if (st->margin[3].is_auto || st->margin[1].is_auto) {
            float rem = fmaxf_(0, cb_w - b->w - (st->margin[3].is_auto ? 0 : b->m[3]) - (st->margin[1].is_auto ? 0 : b->m[1]));
            if (st->margin[3].is_auto && st->margin[1].is_auto) b->m[3] = b->m[1] = rem / 2;
            else if (st->margin[3].is_auto) b->m[3] = rem;
            else b->m[1] = rem;
        }
        return;
    }
    resolve_box_model(b, cb_w);
    float hpad = b->p[1] + b->p[3] + b->bw[1] + b->bw[3];
    float vpad = b->p[0] + b->p[2] + b->bw[0] + b->bw[2];
    bool  is_table = st->display == DISP_TABLE;
    float content_w;
    bool  width_fixed = false;

    if (forced_bw >= 0) {
        content_w = forced_bw - hpad;
        width_fixed = true;
    } else if (!st->width.is_auto) {
        content_w = len_resolve(&st->width, cb_w, 0);
        if (is_table) content_w -= hpad; /* tables: width covers the border box */
        else if (st->box_sizing_border) content_w -= hpad;
        width_fixed = true;
    } else {
        content_w = cb_w - b->m[1] - b->m[3] - hpad;
    }
    if (!is_table || width_fixed) {
        float minw = resolve_content_min(st, &st->min_width, cb_w, hpad, 0);
        float maxw = resolve_content_min(st, &st->max_width, cb_w, hpad, LAYOUT_INF);
        if (forced_bw < 0) {
            if (content_w > maxw) { content_w = maxw; width_fixed = true; }
            if (content_w < minw) { content_w = minw; width_fixed = true; }
        }
    }
    if (content_w < 0) content_w = 0;

    b->cx = b->bw[3] + b->p[3];
    b->cy = b->bw[0] + b->p[0];
    float content_h = 0;

    if (is_table) {
        float natural;
        layout_table_content(lc, b, 0, false, true, &natural, NULL);
        float avail = cb_w - b->m[1] - b->m[3] - hpad;
        float tw = width_fixed ? content_w : fminf_(natural, avail);
        if (tw < 0) tw = 0;
        float used_w = 0;
        content_h = layout_table_content(lc, b, tw, width_fixed, false, &used_w, NULL);
        content_w = used_w;
        if (forced_bw < 0 && (st->margin[3].is_auto || st->margin[1].is_auto)) {
            float rem = fmaxf_(0, cb_w - (content_w + hpad) - (st->margin[3].is_auto ? 0 : b->m[3]) - (st->margin[1].is_auto ? 0 : b->m[1]));
            if (st->margin[3].is_auto && st->margin[1].is_auto) b->m[3] = b->m[1] = rem / 2;
            else if (st->margin[3].is_auto) b->m[3] = rem;
            else b->m[1] = rem;
        }
        b->mt_eff = b->m[0];
        b->mb_eff = b->m[2];
        b->cw = content_w;
        b->w = content_w + hpad;
        b->h = content_h + vpad;
        if (!st->height.is_auto && st->height.pct == 0 && st->height.px > b->h) b->h = st->height.px;
        if (b->abs_first) layout_abs_children(lc, b);
        return;
    }

    /* auto margins when the width is constrained */
    if (forced_bw < 0 && (st->margin[3].is_auto || st->margin[1].is_auto) && (width_fixed || content_w + hpad < cb_w)) {
        float rem = fmaxf_(0, cb_w - (content_w + hpad) - (st->margin[3].is_auto ? 0 : b->m[3]) - (st->margin[1].is_auto ? 0 : b->m[1]));
        if (st->margin[3].is_auto && st->margin[1].is_auto) b->m[3] = b->m[1] = rem / 2;
        else if (st->margin[3].is_auto) b->m[3] = rem;
        else b->m[1] = rem;
    }
    b->cw = content_w;
    b->w = content_w + hpad;

    float mt = b->m[0], mb = b->m[2];
    if (is_flex_container(st) && !b->inline_ctx) {
        float def_h = -1, min_h = 0, max_h = -1;
        if (!st->height.is_auto && st->height.pct == 0) def_h = st->height.px - (st->box_sizing_border ? vpad : 0);
        if (!st->min_height.is_auto && st->min_height.pct == 0) min_h = st->min_height.px - (st->box_sizing_border ? vpad : 0);
        if (!st->max_height.is_auto && st->max_height.pct == 0) max_h = st->max_height.px - (st->box_sizing_border ? vpad : 0);
        bool column = st->flex_dir == FD_COL || st->flex_dir == FD_COL_REV;
        content_h = layout_flex(lc, b, content_w, def_h, min_h, max_h);
        /* auto height + min-height: the free space only exists once the content size is known, so redo
         * the main-axis distribution with the min-height as the definite size */
        if (column && def_h < 0 && min_h > content_h) content_h = layout_flex(lc, b, content_w, min_h, min_h, max_h);
    } else if (b->inline_ctx) {
        content_h = layout_inline(lc, b, content_w, true, NULL);
    } else {
        bool  collapse_top = !b->bfc && b->p[0] == 0 && b->bw[0] == 0;
        float cursor = 0, prev_mb = 0;
        bool  first = true;
        float floats_bottom = 0;
        float poy_ = lc->fl_oy;
        for (Box *c = b->first; c; c = c->next) {
            if (c->style->float_ != FL_NONE && c->kind != BK_ANON) {
                float fb = place_float(lc, b, c, content_w, cursor + (first ? 0 : (prev_mb > 0 ? prev_mb : 0)));
                if (fb > floats_bottom) floats_bottom = fb;
                continue;
            }
            float clear_need = -LAYOUT_INF;
            if (c->style->clear) clear_need = fl_clear_bottom(lc->fl, c->style->clear) - poy_;
            float mt_est = c->style->margin[0].is_auto ? 0 : len_resolve(&c->style->margin[0], content_w, 0);
            float est_dy = first ? (collapse_top ? 0 : mt_est) : cursor + collapse_margins(prev_mb, mt_est);
            if (est_dy < clear_need) est_dy = clear_need;
            lc->child_dy = est_dy;
            lc->child_dx = c->style->margin[3].is_auto ? 0 : len_resolve(&c->style->margin[3], content_w, 0);
            layout_block(lc, c, content_w, -1);
            float cmt = c->mt_eff, cy_;
            if (first) {
                if (collapse_top) {
                    mt = collapse_margins(b->m[0], cmt);
                    cy_ = 0;
                } else cy_ = cmt;
            } else {
                cy_ = cursor + collapse_margins(prev_mb, cmt);
            }
            if (cy_ < clear_need) { cy_ = clear_need; if (first && collapse_top) mt = b->m[0]; }
            c->y = b->cy + cy_;
            c->x = b->cx + c->m[3];
            apply_relative(c, content_w, 0);
            cursor = cy_ + c->h;
            prev_mb = c->mb_eff;
            first = false;
            if (b->baseline < 0 && c->baseline >= 0) b->baseline = c->y + c->baseline;
        }
        bool collapse_bottom = !b->bfc && b->p[2] == 0 && b->bw[2] == 0 && st->height.is_auto;
        if (!first) {
            if (collapse_bottom) {
                mb = collapse_margins(b->m[2], prev_mb);
                content_h = cursor;
            } else content_h = cursor + prev_mb;
        }
        if (b->bfc && floats_bottom > content_h) content_h = floats_bottom; /* a BFC root contains its floats */
    }
    float h = content_h;
    if (!st->height.is_auto && st->height.pct == 0) {
        h = st->height.px;
        if (st->box_sizing_border) h -= vpad;
    }
    if (!st->min_height.is_auto && st->min_height.pct == 0) {
        float mh = st->min_height.px - (st->box_sizing_border ? vpad : 0);
        if (h < mh) h = mh;
    }
    if (!st->max_height.is_auto && st->max_height.pct == 0) {
        float mh = st->max_height.px - (st->box_sizing_border ? vpad : 0);
        if (h > mh) h = mh;
    }
    if (h < 0) h = 0;
    b->h = h + vpad;
    b->mt_eff = mt;
    b->mb_eff = mb;
    if (b->abs_first) layout_abs_children(lc, b);
}

static void layout_block(LC *lc, Box *b, float cb_w, float forced_bw) {
    FloatList *sfl = lc->fl;
    float      sox = lc->fl_ox, soy = lc->fl_oy;
    if (b->kind != BK_REPLACED) resolve_box_model(b, cb_w);
    if (b->bfc || !lc->fl) {
        lc->fl = arena_alloc(lc->a, sizeof(FloatList));
        lc->fl_ox = lc->fl_oy = 0;
    } else {
        lc->fl_ox = sox + lc->child_dx + b->bw[3] + b->p[3];
        lc->fl_oy = soy + lc->child_dy + b->bw[0] + b->p[0];
    }
    layout_block_inner(lc, b, cb_w, forced_bw);
    lc->fl = sfl;
    lc->fl_ox = sox;
    lc->fl_oy = soy;
}

static void layout_atom(LC *lc, Box *a, float avail) {
    if (a->kind == BK_REPLACED) {
        size_replaced(lc, a, avail);
        return;
    }
    resolve_box_model(a, avail);
    const Style *st = a->style;
    float        hpad = a->p[1] + a->p[3] + a->bw[1] + a->bw[3];
    float        forced;
    if (!st->width.is_auto) {
        forced = len_resolve(&st->width, avail, 0) + (st->box_sizing_border ? 0 : hpad);
    } else {
        float mn, mx;
        measure_box(lc, a, &mn, &mx);
        float room = avail - a->m[1] - a->m[3];
        forced = fminf_(mx, fmaxf_(mn, room));
    }
    layout_block(lc, a, avail, forced);
}

static void measure_box(LC *lc, Box *b, float *minw, float *maxw) {
    if (b->measured) {
        *minw = b->min_w;
        *maxw = b->max_w;
        return;
    }
    const Style *st = b->style;
    float mn = 0, mx = 0;
    if (b->kind == BK_REPLACED) {
        size_replaced(lc, b, lc->vw);
        mn = mx = b->w;
    } else {
        resolve_box_model(b, 0);
        float hpad = b->p[1] + b->p[3] + b->bw[1] + b->bw[3];
        if (st->display == DISP_TABLE) {
            float natural = 0, tmin = 0;
            layout_table_content(lc, b, 0, false, true, &natural, &tmin);
            mx = natural + hpad;
            mn = tmin + hpad;
        } else if (is_flex_container(st) && !b->inline_ctx) {
            bool row = st->flex_dir == FD_ROW || st->flex_dir == FD_ROW_REV;
            float gap = row ? st->col_gap : st->row_gap;
            int   cnt = 0;
            for (Box *c = b->first; c; c = c->next) {
                float cmn, cmx;
                resolve_box_model(c, 0);
                measure_box(lc, c, &cmn, &cmx);
                float cm = (c->style->margin[3].is_auto ? 0 : c->m[3]) + (c->style->margin[1].is_auto ? 0 : c->m[1]);
                if (row) {
                    mx += cmx + cm;
                    mn = st->flex_wrap ? fmaxf_(mn, cmn + cm) : mn + cmn + cm;
                } else {
                    mx = fmaxf_(mx, cmx + cm);
                    mn = fmaxf_(mn, cmn + cm);
                }
                cnt++;
            }
            if (row && cnt > 1) { mx += gap * (float)(cnt - 1); if (!st->flex_wrap) mn += gap * (float)(cnt - 1); }
            mn += hpad;
            mx += hpad;
        } else if (b->inline_ctx) {
            float ext = 0;
            layout_inline(lc, b, LAYOUT_INF, false, &ext);
            mx = ext + hpad;
            layout_inline(lc, b, 0, false, &ext);
            mn = ext + hpad;
        } else {
            for (Box *c = b->first; c; c = c->next) {
                float cmn, cmx;
                resolve_box_model(c, 0);
                measure_box(lc, c, &cmn, &cmx);
                float cm = (c->style->margin[3].is_auto ? 0 : c->m[3]) + (c->style->margin[1].is_auto ? 0 : c->m[1]);
                mn = fmaxf_(mn, cmn + cm);
                mx = fmaxf_(mx, cmx + cm);
            }
            mn += hpad;
            mx += hpad;
        }
        if (!st->width.is_auto && st->width.pct == 0) {
            float w = st->width.px + (st->box_sizing_border ? 0 : hpad);
            mn = mx = w;
        }
        if (!st->min_width.is_auto && st->min_width.pct == 0) {
            float w = st->min_width.px + (st->box_sizing_border ? 0 : hpad);
            if (mn < w) mn = w;
            if (mx < w) mx = w;
        }
        if (!st->max_width.is_auto && st->max_width.pct == 0) {
            float w = st->max_width.px + (st->box_sizing_border ? 0 : hpad);
            if (mn > w) mn = w;
            if (mx > w) mx = w;
        }
    }
    b->min_w = mn;
    b->max_w = mx;
    b->measured = true;
    *minw = mn;
    *maxw = mx;
}

/* ========================================================== box tree building == */
static bool is_block_level(const Node *n, int depth) {
    if (n->type != NODE_ELEMENT || !n->style) return false;
    const Style *st = n->style;
    if (is_positioned_out(st)) return false;
    switch (st->display) {
    case DISP_BLOCK: case DISP_LIST_ITEM: case DISP_TABLE: case DISP_FLEX: case DISP_TABLE_ROW: case DISP_TABLE_CELL:
    case DISP_TABLE_ROW_GROUP: case DISP_TABLE_CAPTION:
        return true;
    case DISP_INLINE:
        if (repl_kind_for(n) != RP_NONE || depth > 6) return false;
        for (const Node *c = n->first_child; c; c = c->next)
            if (is_block_level(c, depth + 1)) return true;
        return false;
    default: return false;
    }
}

static int list_ordinal(const Node *li) {
    int ord = 1;
    const char *start = li->parent ? dom_attr(li->parent, "start") : NULL;
    if (start) ord = atoi(start);
    for (const Node *p = li->prev; p; p = p->prev)
        if (p->type == NODE_ELEMENT && p->style && p->style->display == DISP_LIST_ITEM) ord++;
    const char *v = dom_attr(li, "value");
    if (v) ord = atoi(v);
    return ord;
}

static Box *build_block_box(LC *lc, const Node *n, int depth) {
    Box *b = new_box(lc, n, n->style, BK_BLOCK);
    ReplKind rk = repl_kind_for(n);
    if (rk != RP_NONE) {
        b->kind = BK_REPLACED;
        b->repl = rk;
        return b;
    }
    if (n->style->display == DISP_LIST_ITEM) b->ordinal = list_ordinal(n);
    const Style *st = n->style;
    b->bfc = st->float_ != FL_NONE || st->overflow != OV_VISIBLE || st->display == DISP_TABLE_CELL || st->display == DISP_INLINE_BLOCK ||
             st->display == DISP_FLEX || st->display == DISP_INLINE_FLEX || st->display == DISP_TABLE_CAPTION;
    bool positioned = st->position != POS_STATIC || !n->parent || n->parent->type == NODE_DOCUMENT;
    if (positioned && lc->ncb < MAX_CB) lc->cb_stack[lc->ncb++] = b;
    if (depth < MAX_BOX_DEPTH) build_children(lc, b, n, depth + 1);
    if (positioned && lc->ncb > 0 && lc->cb_stack[lc->ncb - 1] == b) lc->ncb--;
    return b;
}

static bool text_is_blank(const Node *n) {
    for (const char *p = n->text; *p; p++)
        if (!is_space((unsigned char)*p)) return false;
    return true;
}

typedef struct {
    const Node **v;
    int          n, cap;
} NodeList;

static void flatten_children(LC *lc, NodeList *out, const Node *node, int depth, int box_depth) {
    if (depth > 12) return;
    for (const Node *c = node->first_child; c; c = c->next) {
        if (c->type == NODE_TEXT) {
            VPUSH(lc->a, out->v, out->n, out->cap, const Node *, c);
        } else if (c->type == NODE_ELEMENT && c->style) {
            if (c->style->display == DISP_NONE) continue;
            if (is_positioned_out(c->style)) {
                if (box_depth < MAX_BOX_DEPTH) register_abs_build(lc, c, box_depth);
                continue;
            }
            if (c->style->display == DISP_CONTENTS && repl_kind_for(c) == RP_NONE) {
                flatten_children(lc, out, c, depth + 1, box_depth);
                continue;
            }
            VPUSH(lc->a, out->v, out->n, out->cap, const Node *, c);
        }
    }
}

/* Anonymous boxes inherit text properties but have no box decoration of their own. */
static const Style *make_anon_style(LC *lc, const Style *parent) {
    Style *a = arena_alloc(lc->a, sizeof *a);
    *a = *parent;
    a->display = DISP_BLOCK;
    for (int i = 0; i < 4; i++) {
        a->margin[i] = (Length){0, 0, false};
        a->padding[i] = (Length){0, 0, false};
        a->border_style[i] = BS_NONE;
        a->border_w[i] = 0;
    }
    a->width = a->height = a->min_width = a->max_width = a->min_height = a->max_height = (Length){0, 0, true};
    a->bg = 0;
    a->overflow = OV_VISIBLE;
    a->position = POS_STATIC;
    a->float_ = FL_NONE;
    a->text_indent = parent->text_indent;
    return a;
}

static void build_children(LC *lc, Box *parent, const Node *node, int depth) {
    NodeList L = {0};
    flatten_children(lc, &L, node, 0, depth);
    if (node->style && is_flex_container(node->style) && parent->kind != BK_ANON) {
        /* every in-flow child of a flex container is a flex item (text runs become anonymous items) */
        for (int i = 0; i < L.n; i++) {
            const Node *c = L.v[i];
            if (c->type == NODE_TEXT) {
                if (text_is_blank(c)) continue;
                Box *anon = new_box(lc, node, make_anon_style(lc, node->style), BK_ANON);
                anon->inline_ctx = true;
                anon->inl = L.v + i;
                anon->ninl = 1;
                box_append(parent, anon);
            } else {
                box_append(parent, build_block_box(lc, c, depth));
            }
        }
        return;
    }
    bool has_block = false;
    for (int i = 0; i < L.n && !has_block; i++)
        if (is_block_level(L.v[i], 0)) has_block = true;
    if (!has_block) {
        if (L.n) {
            parent->inline_ctx = true;
            parent->inl = L.v;
            parent->ninl = L.n;
        }
        return;
    }
    int i = 0;
    while (i < L.n) {
        if (is_block_level(L.v[i], 0)) {
            Box *c = build_block_box(lc, L.v[i], depth);
            box_append(parent, c);
            i++;
            continue;
        }
        int  j = i;
        bool any = false;
        while (j < L.n && !is_block_level(L.v[j], 0)) {
            if (L.v[j]->type != NODE_TEXT || !text_is_blank(L.v[j])) any = true;
            j++;
        }
        if (any) {
            Box *anon = new_box(lc, node, make_anon_style(lc, node->style), BK_ANON);
            anon->inline_ctx = true;
            anon->inl = L.v + i;
            anon->ninl = j - i;
            box_append(parent, anon);
        }
        i = j;
    }
}

/* ================================================================ public API == */
LayoutDoc *layout_create(Node *doc, float vw, float vh, ImageResolver res, void *user) {
    LayoutDoc *d = xcalloc(1, sizeof *d);
    d->doc = doc;
    d->vw = vw;
    d->vh = vh;
    d->resolver = res;
    d->user = user;
    LC lc;
    memset(&lc, 0, sizeof lc);
    lc.a = &d->arena;
    lc.vw = vw;
    lc.vh = vh;
    lc.res = res;
    lc.user = user;

    Node *html = NULL;
    for (Node *c = doc->first_child; c; c = c->next)
        if (dom_is(c, "html")) { html = c; break; }
    if (!html || !html->style) {
        d->canvas_bg = 0xFFFFFFFF;
        return d;
    }
    Box *root = new_box(&lc, html, html->style, BK_BLOCK);
    lc.root = root;
    root->bfc = true;
    root->style = html->style;
    d->root = root;
    if (html->style->display == DISP_LIST_ITEM) root->ordinal = 1;
    lc.cb_stack[lc.ncb++] = root;
    build_children(&lc, root, html, 1);
    lc.ncb = 0;
    layout_block(&lc, root, vw, -1);
    root->x = root->m[3];
    root->y = root->mt_eff;
    d->w = root->x + root->w + root->m[1];
    d->h = root->y + root->h + root->mb_eff;

    /* canvas background: html's, else body's */
    uint32_t bg = html->style->bg;
    if (!(bg >> 24)) {
        Node *body = dom_find(html, "body");
        if (body && body->style) bg = body->style->bg;
    }
    d->canvas_bg = (bg >> 24) ? (0xFF000000u | (bg & 0xFFFFFF)) : 0xFFFFFFFF;
    return d;
}

void  layout_free(LayoutDoc *d) {
    if (!d) return;
    arena_free(&d->arena);
    free(d);
}
float layout_doc_height(const LayoutDoc *d) { return d->h; }
float layout_doc_width(const LayoutDoc *d) { return d->w; }
void  layout_paint(const LayoutDoc *d, Canvas *c, int x, int y, int w, int h, float scroll_y) { paint_document(d, c, x, y, w, h, scroll_y); }
bool  layout_hit_test(const LayoutDoc *d, float x, float y, float scroll_y, HitResult *out) { return hit_test_document(d, x, y, scroll_y, out); }
float layout_find_anchor(const LayoutDoc *d, const char *id) { return find_anchor_y(d, id); }
