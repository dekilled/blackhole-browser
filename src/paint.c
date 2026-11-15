/* Painting, hit testing and anchors for the box tree. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "layout_internal.h"

typedef struct {
    Canvas       *c;
    const Node   *skip_bg; /* element whose background was propagated to the canvas */
    float         vy0, vy1;
} PC;

static void roman(char *out, int n, bool upper) {
    static const struct { int v; const char *u, *l; } t[] = {{1000, "M", "m"}, {900, "CM", "cm"}, {500, "D", "d"}, {400, "CD", "cd"},
        {100, "C", "c"}, {90, "XC", "xc"}, {50, "L", "l"}, {40, "XL", "xl"}, {10, "X", "x"}, {9, "IX", "ix"}, {5, "V", "v"}, {4, "IV", "iv"}, {1, "I", "i"}};
    out[0] = 0;
    if (n <= 0 || n > 3999) { sprintf(out, "%d", n); return; }
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        while (n >= t[i].v) { strcat(out, upper ? t[i].u : t[i].l); n -= t[i].v; }
}

static void marker_text(const Box *b, char *out, size_t cap) {
    int n = b->ordinal;
    switch (b->style->list_style) {
    case LS_DISC: snprintf(out, cap, "\xE2\x80\xA2"); break;
    case LS_CIRCLE: snprintf(out, cap, "\xE2\x97\xA6"); break;
    case LS_SQUARE: snprintf(out, cap, "\xE2\x96\xAA"); break;
    case LS_DECIMAL: snprintf(out, cap, "%d.", n); break;
    case LS_LOWER_ALPHA: case LS_UPPER_ALPHA: {
        char base = b->style->list_style == LS_LOWER_ALPHA ? 'a' : 'A';
        if (n >= 1 && n <= 26) snprintf(out, cap, "%c.", (char)(base + n - 1));
        else snprintf(out, cap, "%d.", n);
        break;
    }
    case LS_LOWER_ROMAN: case LS_UPPER_ROMAN: {
        char r[32];
        roman(r, n, b->style->list_style == LS_UPPER_ROMAN);
        snprintf(out, cap, "%s.", r);
        break;
    }
    default: out[0] = 0;
    }
}

static uint32_t side_color(const Style *st, int i) { return (st->border_color_cur >> i) & 1 ? st->color : st->border_color[i]; }

static void paint_borders(PC *pc, const Style *st, const float bw[4], float x, float y, float w, float h, bool left, bool right) {
    Canvas *c = pc->c;
    for (int i = 0; i < 4; i++) {
        float t = bw[i];
        if ((i == 3 && !left) || (i == 1 && !right)) t = 0;
        if (t <= 0 || st->border_style[i] == BS_NONE) continue;
        uint32_t col = side_color(st, i);
        if (!(col >> 24)) continue;
        float rx, ry, rw, rh;
        bool  horiz = i == 0 || i == 2;
        switch (i) {
        case 0: rx = x; ry = y; rw = w; rh = t; break;
        case 2: rx = x; ry = y + h - t; rw = w; rh = t; break;
        case 3: rx = x; ry = y + bw[0]; rw = t; rh = h - bw[0] - bw[2]; break;
        default: rx = x + w - t; ry = y + bw[0]; rw = t; rh = h - bw[0] - bw[2]; break;
        }
        uint8_t bs = st->border_style[i];
        if (bs == BS_DASHED || bs == BS_DOTTED) {
            float seg = bs == BS_DASHED ? t * 3 : t, gap = bs == BS_DASHED ? t * 2 : t;
            float len = horiz ? rw : rh;
            for (float p = 0; p < len; p += seg + gap) {
                float l = fminf(seg, len - p);
                if (horiz) canvas_fill_rect(c, rx + p, ry, l, rh, col);
                else canvas_fill_rect(c, rx, ry + p, rw, l, col);
            }
        } else if (bs == BS_DOUBLE && t >= 3) {
            float l = floorf(t / 3);
            if (horiz) {
                canvas_fill_rect(c, rx, ry, rw, l, col);
                canvas_fill_rect(c, rx, ry + rh - l, rw, l, col);
            } else {
                canvas_fill_rect(c, rx, ry, l, rh, col);
                canvas_fill_rect(c, rx + rw - l, ry, l, rh, col);
            }
        } else {
            canvas_fill_rect(c, rx, ry, rw, rh, col);
        }
    }
}

static void draw_label(PC *pc, const Box *b, float bx, float by) {
    const Style *st = b->style;
    BhFont        *f = layout_style_font(st);
    float        cx = bx + b->cx, cy = by + b->cy;
    float        ch = b->h - (b->p[0] + b->p[2] + b->bw[0] + b->bw[2]);
    float        lh = font_line_gap_height(f);
    float        base = cy + (ch - lh) / 2 + font_ascent(f) + (lh - font_ascent(f) - font_descent(f)) / 2;
    int          old[4];
    canvas_set_clip(pc->c, (int)floorf(cx), (int)floorf(cy), (int)ceilf(b->cw), (int)ceilf(ch) + 1, old);
    const char *label = b->label ? b->label : "";
    switch (b->repl) {
    case RP_BUTTONINPUT: {
        float w = font_text_width(f, label, strlen(label), 0);
        canvas_draw_text(pc->c, f, cx + (b->cw - w) / 2, base, label, strlen(label), st->color, 0);
        break;
    }
    case RP_TEXTINPUT: {
        const char *v = dom_attr(b->node, "value");
        bool        ph = !(v && *v);
        uint32_t    col = ph ? 0xFF757575u : st->color;
        canvas_draw_text(pc->c, f, cx, base, label, strlen(label), col, 0);
        break;
    }
    case RP_SELECT: {
        canvas_draw_text(pc->c, f, cx, base, label, strlen(label), st->color, 0);
        canvas_set_clip(pc->c, (int)bx, (int)by, (int)b->w, (int)b->h, NULL);
        const char *arrow = "\xE2\x96\xBE";
        canvas_draw_text(pc->c, f, bx + b->w - b->bw[1] - b->p[1] - 12, base, arrow, strlen(arrow), 0xFF444444u, 0);
        break;
    }
    case RP_TEXTAREA: {
        const char *s = label;
        float       y = cy + font_ascent(f);
        while (*s) {
            const char *nl = strchr(s, '\n');
            size_t      n = nl ? (size_t)(nl - s) : strlen(s);
            canvas_draw_text(pc->c, f, cx, y, s, n, st->color, 0);
            y += lh;
            if (!nl) break;
            s = nl + 1;
        }
        break;
    }
    default: break;
    }
    canvas_restore_clip(pc->c, old);
}

static void paint_replaced(PC *pc, const Box *b, float bx, float by) {
    Canvas *c = pc->c;
    float   cx = bx + b->cx, cy = by + b->cy;
    float   cw = b->cw, ch = b->h - (b->p[0] + b->p[2] + b->bw[0] + b->bw[2]);
    switch (b->repl) {
    case RP_IMG:
        if (b->image) {
            canvas_draw_image(c, cx, cy, cw, ch, b->image->px, b->image->w, b->image->h);
        } else if (b->label && *b->label) {
            BhFont *f = layout_style_font(b->style);
            int   old[4];
            canvas_set_clip(c, (int)cx, (int)cy, (int)ceilf(cw), (int)ceilf(ch), old);
            canvas_draw_text(c, f, cx + 2, cy + font_ascent(f) + 1, b->label, strlen(b->label), 0xFF666666u, 0);
            canvas_restore_clip(c, old);
        }
        break;
    case RP_TEXTINPUT: case RP_BUTTONINPUT: case RP_SELECT: case RP_TEXTAREA: draw_label(pc, b, bx, by); break;
    case RP_CHECK: {
        canvas_fill_rect(c, cx, cy, 13, 13, 0xFF767676u);
        canvas_fill_rect(c, cx + 1, cy + 1, 11, 11, 0xFFFFFFFFu);
        if (dom_attr(b->node, "checked")) {
            BhFont *f = font_get(FONT_SANS, true, false, 13);
            canvas_draw_text(c, f, cx + 1, cy + 11, "\xE2\x9C\x93", 3, 0xFF1A1A1Au, 0);
        }
        break;
    }
    case RP_RADIO: {
        canvas_fill_ellipse(c, cx + 6.5f, cy + 6.5f, 6.5f, 6.5f, 0xFF767676u);
        canvas_fill_ellipse(c, cx + 6.5f, cy + 6.5f, 5.5f, 5.5f, 0xFFFFFFFFu);
        if (dom_attr(b->node, "checked")) canvas_fill_ellipse(c, cx + 6.5f, cy + 6.5f, 3.2f, 3.2f, 0xFF1A1A1Au);
        break;
    }
    case RP_FRAME: {
        const char *tag = b->node->tag;
        bool        media = !strcmp(tag, "video") || !strcmp(tag, "audio");
        canvas_fill_rect(c, cx, cy, cw, ch, media ? 0xFF111111u : 0xFFF4F4F4u);
        BhFont *f = font_get(FONT_SANS, false, false, 13);
        if (b->label && cw > 20) {
            int old[4];
            canvas_set_clip(c, (int)cx, (int)cy, (int)ceilf(cw), (int)ceilf(ch), old);
            canvas_draw_text(c, f, cx + 8, cy + 18, b->label, strlen(b->label), media ? 0xFFDDDDDDu : 0xFF888888u, 0);
            canvas_restore_clip(c, old);
        }
        break;
    }
    default: break;
    }
}

static void paint_box(PC *pc, const Box *b, float ox, float oy, int depth);

static void paint_frags(PC *pc, const Box *b, float bx, float by, int depth) {
    Canvas *c = pc->c;
    for (int i = 0; i < b->nspans; i++) {
        const Span  *sp = &b->spans[i];
        const Style *st = sp->style;
        if (st->visibility_hidden) continue;
        float x = bx + sp->x, y = by + sp->y;
        if (y > pc->vy1 || y + sp->h < pc->vy0) continue;
        if (st->bg >> 24) canvas_fill_rect(c, x, y, sp->w, sp->h, st->bg);
        float bw[4];
        for (int k = 0; k < 4; k++) bw[k] = st->border_style[k] != BS_NONE ? st->border_w[k] : 0;
        if (bw[0] || bw[1] || bw[2] || bw[3]) paint_borders(pc, st, bw, x, y, sp->w, sp->h, sp->first, sp->last);
    }
    for (int i = 0; i < b->nfrags; i++) {
        const Frag *f = &b->frags[i];
        if (f->box) {
            paint_box(pc, f->box, bx, by, depth + 1);
            continue;
        }
        const Style *st = f->style;
        if (st->visibility_hidden) continue;
        float base = floorf(by + f->y + 0.5f);
        if (base - st->font_size * 1.2f > pc->vy1 || base + st->font_size * 0.5f < pc->vy0) continue;
        canvas_draw_text(c, f->font, bx + f->x, base, f->text, (size_t)f->len, st->color, st->letter_spacing);
        float thick = fmaxf(1.0f, floorf(st->font_size / 14.0f + 0.5f));
        if (st->underline) canvas_fill_rect(c, bx + f->x, base + fmaxf(1.0f, floorf(st->font_size * 0.1f)), f->w, thick, st->color);
        if (st->line_through) canvas_fill_rect(c, bx + f->x, base - floorf(st->font_size * 0.3f), f->w, thick, st->color);
    }
}

static void paint_box(PC *pc, const Box *b, float ox, float oy, int depth) {
    if (depth > 400) return;
    const Style *st = b->style;
    float        bx = ox + b->x, by = oy + b->y;
    if (st->opacity < 0.01f && b->kind != BK_ANON) return;
    bool visible = !st->visibility_hidden;
    bool box_vis = by <= pc->vy1 && by + b->h >= pc->vy0;
    if (b->kind != BK_ANON && visible && box_vis) {
        if ((st->bg >> 24) && b->node != pc->skip_bg) canvas_fill_rect(pc->c, bx, by, b->w, b->h, st->bg);
        if (b->bw[0] || b->bw[1] || b->bw[2] || b->bw[3]) paint_borders(pc, st, b->bw, bx, by, b->w, b->h, true, true);
        if (b->kind == BK_REPLACED) paint_replaced(pc, b, bx, by);
        if (st->display == DISP_LIST_ITEM && st->list_style != LS_NONE && b->kind == BK_BLOCK) {
            char mt[48];
            marker_text(b, mt, sizeof mt);
            BhFont *f = layout_style_font(st);
            float base = b->baseline >= 0 ? by + b->baseline : by + b->cy + font_ascent(f);
            float w = font_text_width(f, mt, strlen(mt), 0);
            float gap = st->list_style <= LS_SQUARE ? st->font_size * 0.35f : st->font_size * 0.3f;
            canvas_draw_text(pc->c, f, bx + b->cx - gap - w, floorf(base + 0.5f), mt, strlen(mt), st->color, 0);
        }
    }
    int  old[4];
    bool clip = st->overflow != OV_VISIBLE && b->kind != BK_ANON && b->kind != BK_REPLACED;
    if (clip) canvas_set_clip(pc->c, (int)floorf(bx + b->bw[3]), (int)floorf(by + b->bw[0]),
                              (int)ceilf(b->w - b->bw[1] - b->bw[3]), (int)ceilf(b->h - b->bw[0] - b->bw[2]), old);
    if (b->nfrags || b->nspans) paint_frags(pc, b, bx, by, depth);
    for (const Box *c = b->first; c; c = c->next) paint_box(pc, c, bx, by, depth + 1);
    if (clip) canvas_restore_clip(pc->c, old);
}

void paint_document(const LayoutDoc *d, Canvas *c, int x, int y, int w, int h, float scroll_y) {
    int old[4];
    canvas_set_clip(c, x, y, w, h, old);
    canvas_fill_rect(c, (float)x, (float)y, (float)w, (float)h, d->canvas_bg);
    if (d->root) {
        PC pc = {c, NULL, (float)y, (float)(y + h)}; /* visible range, in canvas coordinates like `by` */
        const Node *html = d->root->node;
        if (html->style && (html->style->bg >> 24)) pc.skip_bg = html;
        else {
            Node *body = dom_find((Node *)html, "body");
            if (body && body->style && (body->style->bg >> 24)) pc.skip_bg = body;
        }
        paint_box(&pc, d->root, (float)x, (float)y - scroll_y, 0);
    }
    canvas_restore_clip(c, old);
}

/* ================================================================ hit test === */
static const Node *link_ancestor(const Node *n) {
    for (; n; n = n->parent)
        if (dom_is(n, "a") && dom_attr(n, "href")) return n;
    return NULL;
}

static bool hit_box(const Box *b, float ox, float oy, float x, float y, HitResult *out, int depth) {
    if (depth > 400) return false;
    float bx = ox + b->x, by = oy + b->y;
    if (x < bx || x >= bx + b->w || y < by || y >= by + b->h) {
        /* children may overflow, but we only support the common contained case */
        return false;
    }
    for (int i = b->nfrags - 1; i >= 0; i--) {
        const Frag *f = &b->frags[i];
        if (f->box) {
            if (hit_box(f->box, bx, by, x, y, out, depth + 1)) {
                if (!out->link) out->link = f->link;
                return true;
            }
        } else {
            float a = f->font ? font_ascent(f->font) : 12, dsc = f->font ? font_descent(f->font) : 4;
            if (x >= bx + f->x && x < bx + f->x + f->w && y >= by + f->y - a && y < by + f->y + dsc) {
                out->node = b->node;
                out->link = f->link ? f->link : link_ancestor(b->node);
                return true;
            }
        }
    }
    /* children paint in order, so the last one containing the point is on top */
    HitResult best = {NULL, NULL};
    bool      found = false;
    for (const Box *c = b->first; c; c = c->next) {
        HitResult tmp = {NULL, NULL};
        if (hit_box(c, bx, by, x, y, &tmp, depth + 1)) {
            best = tmp;
            found = true;
        }
    }
    if (found) {
        *out = best;
        return true;
    }
    out->node = b->node;
    out->link = link_ancestor(b->node);
    return true;
}

bool hit_test_document(const LayoutDoc *d, float x, float y, HitResult *out) {
    out->node = NULL;
    out->link = NULL;
    if (!d->root) return false;
    return hit_box(d->root, 0, 0, x, y, out, 0);
}

/* ================================================================ anchors ==== */
static const Node *find_by_id(const Node *root, const char *id) {
    const Node *n = root->first_child;
    while (n) {
        if (n->type == NODE_ELEMENT) {
            const char *v = dom_attr(n, "id");
            if (v && !strcmp(v, id)) return n;
            if (dom_is(n, "a")) {
                v = dom_attr(n, "name");
                if (v && !strcmp(v, id)) return n;
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) return NULL;
        n = n->next;
    }
    return NULL;
}

static bool is_ancestor_or_self(const Node *a, const Node *t) {
    for (; t; t = t->parent)
        if (t == a) return true;
    return false;
}

static void best_box(const Box *b, float ox, float oy, const Node *target, float *best_y, int depth) {
    if (depth > 400) return;
    float by = oy + b->y;
    if (b->kind != BK_ANON && b->node && is_ancestor_or_self(b->node, target)) *best_y = by;
    for (const Box *c = b->first; c; c = c->next) best_box(c, ox + b->x, by, target, best_y, depth + 1);
}

float find_anchor_y(const LayoutDoc *d, const char *id) {
    if (!d->root || !id || !*id) return -1;
    const Node *t = find_by_id(d->doc, id);
    if (!t) return -1;
    float y = -1;
    best_box(d->root, 0, 0, t, &y, 0);
    return y;
}
