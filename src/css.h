/* CSS: stylesheet parser, selectors, media queries. Computing styles lives in style.h. */
#ifndef BH_CSS_H
#define BH_CSS_H

#include "dom.h"
#include "util.h"

/* Property ids. Shorthands are expanded when applied (so var() works inside them). */
#define PROP_LIST(X)                                                                                        \
    X(DISPLAY, "display") X(COLOR, "color") X(BACKGROUND, "background") X(BACKGROUND_COLOR, "background-color") \
    X(FONT, "font") X(FONT_SIZE, "font-size") X(FONT_WEIGHT, "font-weight") X(FONT_STYLE, "font-style")       \
    X(FONT_FAMILY, "font-family") X(LINE_HEIGHT, "line-height") X(TEXT_ALIGN, "text-align")                   \
    X(TEXT_DECORATION, "text-decoration") X(TEXT_DECORATION_LINE, "text-decoration-line")                     \
    X(TEXT_TRANSFORM, "text-transform") X(WHITE_SPACE, "white-space") X(LETTER_SPACING, "letter-spacing")     \
    X(TEXT_INDENT, "text-indent") X(VERTICAL_ALIGN, "vertical-align") X(VISIBILITY, "visibility")             \
    X(MARGIN, "margin") X(MARGIN_TOP, "margin-top") X(MARGIN_RIGHT, "margin-right")                           \
    X(MARGIN_BOTTOM, "margin-bottom") X(MARGIN_LEFT, "margin-left") X(PADDING, "padding")                     \
    X(PADDING_TOP, "padding-top") X(PADDING_RIGHT, "padding-right") X(PADDING_BOTTOM, "padding-bottom")       \
    X(PADDING_LEFT, "padding-left") X(BORDER, "border") X(BORDER_TOP, "border-top")                           \
    X(BORDER_RIGHT, "border-right") X(BORDER_BOTTOM, "border-bottom") X(BORDER_LEFT, "border-left")           \
    X(BORDER_WIDTH, "border-width") X(BORDER_STYLE, "border-style") X(BORDER_COLOR, "border-color")           \
    X(BORDER_TOP_WIDTH, "border-top-width") X(BORDER_RIGHT_WIDTH, "border-right-width")                       \
    X(BORDER_BOTTOM_WIDTH, "border-bottom-width") X(BORDER_LEFT_WIDTH, "border-left-width")                   \
    X(BORDER_TOP_STYLE, "border-top-style") X(BORDER_RIGHT_STYLE, "border-right-style")                       \
    X(BORDER_BOTTOM_STYLE, "border-bottom-style") X(BORDER_LEFT_STYLE, "border-left-style")                   \
    X(BORDER_TOP_COLOR, "border-top-color") X(BORDER_RIGHT_COLOR, "border-right-color")                       \
    X(BORDER_BOTTOM_COLOR, "border-bottom-color") X(BORDER_LEFT_COLOR, "border-left-color")                   \
    X(WIDTH, "width") X(HEIGHT, "height") X(MIN_WIDTH, "min-width") X(MAX_WIDTH, "max-width")                 \
    X(MIN_HEIGHT, "min-height") X(MAX_HEIGHT, "max-height") X(LIST_STYLE, "list-style")                       \
    X(LIST_STYLE_TYPE, "list-style-type") X(BOX_SIZING, "box-sizing") X(FLOAT, "float") X(CLEAR, "clear")     \
    X(OVERFLOW, "overflow") X(POSITION, "position") X(OPACITY, "opacity")                        \
    X(BORDER_SPACING, "border-spacing") X(BORDER_COLLAPSE, "border-collapse")                                 \
    X(FLEX_DIRECTION, "flex-direction") X(FLEX_WRAP, "flex-wrap") X(FLEX_FLOW, "flex-flow")                   \
    X(JUSTIFY_CONTENT, "justify-content") X(ALIGN_ITEMS, "align-items") X(ALIGN_SELF, "align-self")           \
    X(ALIGN_CONTENT, "align-content") X(FLEX_GROW, "flex-grow") X(FLEX_SHRINK, "flex-shrink")                 \
    X(FLEX_BASIS, "flex-basis") X(FLEX, "flex") X(GAP, "gap") X(ROW_GAP, "row-gap") X(COLUMN_GAP, "column-gap") \
    X(CONTENT, "content") X(BORDER_RADIUS, "border-radius") X(BORDER_TOP_LEFT_RADIUS, "border-top-left-radius")                    \
    X(BORDER_TOP_RIGHT_RADIUS, "border-top-right-radius") X(BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius") \
    X(BORDER_BOTTOM_LEFT_RADIUS, "border-bottom-left-radius") X(BACKGROUND_IMAGE, "background-image")        \
    X(BOX_SHADOW, "box-shadow")                                                                               \
    X(OVERFLOW_X, "overflow-x") X(OVERFLOW_Y, "overflow-y")                                                   \
    X(MARGIN_INLINE_START, "margin-inline-start") X(MARGIN_INLINE_END, "margin-inline-end")                   \
    X(MARGIN_BLOCK_START, "margin-block-start") X(MARGIN_BLOCK_END, "margin-block-end")                       \
    X(MARGIN_INLINE, "margin-inline") X(MARGIN_BLOCK, "margin-block")                                         \
    X(PADDING_INLINE_START, "padding-inline-start") X(PADDING_INLINE_END, "padding-inline-end")               \
    X(PADDING_BLOCK_START, "padding-block-start") X(PADDING_BLOCK_END, "padding-block-end")                   \
    X(PADDING_INLINE, "padding-inline") X(PADDING_BLOCK, "padding-block")                                     \
    X(INSET_INLINE_START, "inset-inline-start") X(INSET_INLINE_END, "inset-inline-end")                       \
    X(INSET_BLOCK_START, "inset-block-start") X(INSET_BLOCK_END, "inset-block-end")                           \
    X(TOP, "top") X(RIGHT, "right") X(BOTTOM, "bottom") X(LEFT, "left") X(INSET, "inset") X(Z_INDEX, "z-index")

typedef enum {
    PROP_UNKNOWN = 0,
    PROP_CUSTOM, /* --name */
#define X(id, name) PROP_##id,
    PROP_LIST(X)
#undef X
    PROP__COUNT
} PropId;

PropId prop_lookup(const char *name);

typedef struct Decl {
    PropId       prop;
    const char  *name; /* custom property name (with leading --) */
    const char  *value;
    bool         important;
    struct Decl *next;
} Decl;

/* ---- selectors ---- */
typedef struct {
    const char *name, *value;
    char        op; /* 0 = exists, '=', '~', '|', '^', '$', '*' */
    bool        icase;
} AttrSel;

enum {
    PC_FIRST_CHILD = 1 << 0, PC_LAST_CHILD = 1 << 1, PC_ONLY_CHILD = 1 << 2, PC_LINK = 1 << 3,
    PC_HOVER = 1 << 4,       PC_ROOT = 1 << 5,       PC_EMPTY = 1 << 6,      PC_FIRST_OF_TYPE = 1 << 7,
    PC_LAST_OF_TYPE = 1 << 8, PC_NEVER = 1 << 9,     PC_CHECKED = 1 << 10,   PC_DISABLED = 1 << 11,
    PC_ENABLED = 1 << 12,    PC_ONLY_OF_TYPE = 1 << 13,
};

typedef struct Compound Compound;
struct Compound {
    const char  *tag; /* NULL = any */
    const char  *id;
    const char **classes;
    int          nclasses;
    AttrSel     *attrs;
    int          nattrs;
    uint32_t     pseudo;
    uint8_t      pseudo_el; /* 0 none, 1 ::before, 2 ::after */
    bool         has_nth, nth_of_type, nth_last;
    int          nth_a, nth_b;
    Compound   **nots;
    int          nnots;
    Compound   **anys; /* :is() / :where() alternatives */
    int          nany;
    bool         any_zero;
};

typedef struct {
    Compound *parts; /* left to right */
    char     *combs; /* combs[i] joins parts[i] and parts[i+1]: ' ' '>' '+' '~' */
    int       n;
    uint32_t  specificity;
    uint8_t   pseudo_el;
} ComplexSel;

/* ---- media queries ---- */
typedef struct MediaFeature {
    int   kind; /* see MF_* in css.c */
    float value;
} MediaFeature;

typedef struct MediaQuery {
    bool                 negate, media_ok; /* media_ok: screen/all */
    MediaFeature        *features;
    int                  nfeatures;
    struct MediaQuery   *next; /* comma list = OR */
} MediaQuery;

typedef struct MediaCond {
    MediaQuery       *queries;
    struct MediaCond *outer; /* nested @media: both must hold */
} MediaCond;

typedef struct {
    float vw, vh;
} Viewport;

bool media_matches(const MediaCond *m, const Viewport *vp);
/* Evaluates a media="" attribute (e.g. on <link>/<style>). Empty means all. */
bool css_media_attr_matches(const char *media, const Viewport *vp);

/* ---- stylesheet ---- */
typedef struct {
    ComplexSel **sels;
    int          nsels;
    Decl        *decls;
    MediaCond   *media;
    int          order;
} Rule;

typedef struct {
    Arena  arena;
    Rule  *rules;
    int    nrules, caprules;
    char **imports;
    int    nimports;
    int    origin; /* 0 = user agent, 1 = author */
} Stylesheet;

Stylesheet *css_parse(const char *src, size_t len, int origin);
void        css_free(Stylesheet *s);
Decl       *css_parse_decls(Arena *a, const char *src, size_t len); /* e.g. style="" attribute */

typedef struct {
    bool hover_enabled;
} MatchOpts;

/* Returns specificity of the best matching selector of `rule`, or -1 if none matches. */
long css_rule_match(const Rule *r, const Node *n, int pseudo_el);

#endif
