/* Computed styles: cascade, inheritance, value parsing. */
#ifndef BH_STYLE_H
#define BH_STYLE_H

#include "css.h"
#include "dom.h"

typedef enum {
    DISP_NONE, DISP_BLOCK, DISP_INLINE, DISP_INLINE_BLOCK, DISP_LIST_ITEM,
    DISP_TABLE, DISP_TABLE_ROW, DISP_TABLE_CELL, DISP_TABLE_ROW_GROUP, DISP_TABLE_CAPTION,
    DISP_FLEX, DISP_INLINE_FLEX, DISP_CONTENTS,
} DisplayType;

typedef enum { TA_LEFT, TA_RIGHT, TA_CENTER, TA_JUSTIFY } TextAlign;
typedef enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE } WhiteSpace;
typedef enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP } TextTransform;
typedef enum { FF_SANS, FF_SERIF, FF_MONO } FontFamily;
typedef enum { BS_NONE, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE } BorderStyle;
typedef enum { LS_NONE, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA, LS_LOWER_ROMAN, LS_UPPER_ROMAN } ListStyle;
typedef enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER } VAlign;
typedef enum { FL_NONE, FL_LEFT, FL_RIGHT } Float;
typedef enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY } Position;
typedef enum { OV_VISIBLE, OV_HIDDEN, OV_SCROLL, OV_AUTO } Overflow;

/* A length is px + pct * base; auto is a flag. This makes calc(100% - 20px) representable. */
typedef struct {
    float px, pct;
    bool  is_auto;
} Length;

typedef struct CustomProp {
    const char        *name, *value;
    struct CustomProp *next;
} CustomProp;

struct Style {
    uint8_t display, text_align, white_space, text_transform, font_family, list_style, vertical_align;
    uint8_t visibility_hidden, float_, clear, position, overflow, box_sizing_border;
    bool    italic, underline, line_through;
    int     font_weight;
    float   font_size;
    float   line_height; /* <0: normal; else px, or multiplier if lh_is_mul */
    bool    lh_is_mul;
    float   letter_spacing, text_indent, opacity, border_spacing;
    bool    border_collapse;
    uint32_t color, bg;
    Length   width, height, min_width, max_width, min_height, max_height;
    Length   margin[4], padding[4]; /* top right bottom left */
    float    border_w[4];
    uint8_t  border_style[4];
    uint32_t border_color[4];
    uint8_t  border_color_cur; /* bit i set: border i uses currentColor */
    CustomProp *custom;
};

#define COLOR_TRANSPARENT 0x00000000u
#define MAKE_RGB(r, g, b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Resolves a Length against a percentage base. Auto yields `fallback`. */
static inline float len_resolve(const Length *l, float base, float fallback) {
    return l->is_auto ? fallback : l->px + l->pct * base;
}

typedef struct {
    Stylesheet **sheets;
    int          nsheets;
    Viewport     vp;
} StyleSet;

/* Returns the built-in user agent stylesheet (parsed once). */
Stylesheet *style_ua_sheet(void);

/* Computes styles for every node in the tree. Styles are allocated in `arena`. */
void style_compute_tree(Node *doc, const StyleSet *set, Arena *arena);

/* Parse helpers shared with the layout engine. */
bool style_parse_color(const char *s, uint32_t *out);

#endif
