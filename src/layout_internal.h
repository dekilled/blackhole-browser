/* Internal structures shared by layout.c and paint.c. */
#ifndef BH_LAYOUT_INTERNAL_H
#define BH_LAYOUT_INTERNAL_H

#include "layout.h"

#define LAYOUT_INF 1e9f

typedef struct Box Box;

typedef enum { IT_WORD, IT_SPACE, IT_BREAK, IT_OPEN, IT_CLOSE, IT_ATOM } ItemKind;

typedef struct {
    uint8_t      kind;
    bool         brk_before; /* a line break is allowed before this item */
    bool         preserved;  /* IT_SPACE: not collapsible (pre / pre-wrap) */
    const Style *style;
    const Node  *link;
    BhFont        *font;
    const char  *text;
    int          len; /* bytes for IT_WORD, count for IT_SPACE */
    float        w;
    Box         *box;
} Item;

typedef struct {
    float        x, y, w; /* y is the baseline; all relative to the owning box's border box */
    const char  *text;
    int          len;
    const Style *style;
    BhFont        *font;
    const Node  *link;
    Box         *box; /* atomic inline (inline-block, image, control) */
} Frag;

typedef struct {
    float        x, y, w, h;
    const Style *style;
    bool         first, last; /* whether the left/right border and padding belong to this piece */
} Span;

typedef enum { BK_BLOCK, BK_ANON, BK_REPLACED } BoxKind;
typedef enum {
    RP_NONE, RP_IMG, RP_TEXTINPUT, RP_BUTTONINPUT, RP_CHECK, RP_RADIO, RP_SELECT, RP_TEXTAREA, RP_FRAME, RP_EMPTY
} ReplKind;

struct Box {
    const Node  *node;
    const Style *style;
    BoxKind      kind;
    ReplKind     repl;
    Box         *parent, *first, *last, *next;
    float        x, y, w, h;   /* border box, relative to the parent's border box */
    float        cx, cy, cw;   /* content box origin inside the border box, and width */
    float        m[4], p[4], bw[4]; /* used margins, paddings, border widths (top right bottom left) */
    float        mt_eff, mb_eff;   /* margins as seen by the parent after collapsing */
    bool         bfc;              /* establishes a block formatting context: margins do not collapse through */
    bool         atom;             /* atomic inline-level box */
    const Node **inl;              /* inline content roots when inline_ctx */
    int          ninl;
    bool         inline_ctx;
    Item        *items;
    int          nitems;
    bool         items_built;
    Frag        *frags;
    int          nfrags, capfrags;
    Span        *spans;
    int          nspans, capspans;
    float        baseline; /* first line baseline from the border-box top, or -1 */
    float        min_w, max_w; /* border-box intrinsic widths */
    bool         measured;
    int          ordinal; /* list items */
    const char  *label;   /* replaced content text (alt, value, option text) */
    Image       *image;
    const Node  *atom_link;
    Box         *abs_first, *abs_last, *abs_next; /* absolutely positioned descendants anchored to this box */
    bool         fixed;                       /* position: fixed (viewport relative) */
};

struct LayoutDoc {
    Arena         arena;
    Box          *root;
    Node         *doc;
    float         w, h;
    float         vw, vh;
    ImageResolver resolver;
    void         *user;
    uint32_t      canvas_bg;
};

BhFont *layout_style_font(const Style *s);

/* paint.c */
void paint_document(const LayoutDoc *d, Canvas *c, int x, int y, int w, int h, float scroll_y);
bool hit_test_document(const LayoutDoc *d, float x, float y, float scroll_y, HitResult *out);
float find_anchor_y(const LayoutDoc *d, const char *id);

#endif
