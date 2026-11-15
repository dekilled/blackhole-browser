/* Software 2D graphics: canvas, rectangles, text (FreeType), PNG output. Pixels are 0xAARRGGBB. */
#ifndef BH_GFX_H
#define BH_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int       w, h;
    uint32_t *px;
    int       cx0, cy0, cx1, cy1; /* clip rectangle, [x0,x1) x [y0,y1) */
} Canvas;

Canvas *canvas_new(int w, int h);
void    canvas_free(Canvas *c);
void    canvas_resize(Canvas *c, int w, int h);
void    canvas_clear(Canvas *c, uint32_t argb);
/* Sets the clip to the intersection of the canvas and the rectangle. Returns the previous clip in `old` (4 ints). */
void    canvas_set_clip(Canvas *c, int x, int y, int w, int h, int old[4]);
void    canvas_restore_clip(Canvas *c, const int old[4]);
void    canvas_fill_rect(Canvas *c, float x, float y, float w, float h, uint32_t argb);
void    canvas_fill_ellipse(Canvas *c, float cx, float cy, float rx, float ry, uint32_t argb);
void    canvas_blend_px(Canvas *c, int x, int y, uint32_t argb); /* clip aware, straight alpha */
/* Rounded rectangles; r = {top-left, top-right, bottom-right, bottom-left} radii in pixels. */
void    canvas_fill_rrect(Canvas *c, float x, float y, float w, float h, const float r[4], uint32_t argb);
void    canvas_stroke_rrect(Canvas *c, float x, float y, float w, float h, const float r[4], float thick, uint32_t argb);
float   canvas_rrect_coverage(float px, float py, float x, float y, float w, float h, const float r[4]);
void    canvas_draw_image_r(Canvas *c, float dx, float dy, float dw, float dh, const uint32_t *src, int sw, int sh, const float r[4]);
void    canvas_hline(Canvas *c, int x, int y, int w, uint32_t argb);
void    canvas_blit_argb(Canvas *c, int x, int y, int w, int h, const uint32_t *src, int src_stride);
/* Scaled blit with nearest/bilinear sampling of an ARGB image into a destination rectangle. */
void    canvas_draw_image(Canvas *c, float dx, float dy, float dw, float dh, const uint32_t *src, int sw, int sh);
bool    canvas_write_png(const Canvas *c, const char *path);

/* ---- fonts ---- */
typedef struct BhFont BhFont;
enum { FONT_SANS = 0, FONT_SERIF = 1, FONT_MONO = 2 };

/* Returns a cached font object (never NULL once fonts are available). */
BhFont  *font_get(int family, bool bold, bool italic, float size);
bool   font_available(void);
float  font_ascent(BhFont *f);
float  font_descent(BhFont *f);
float  font_line_gap_height(BhFont *f); /* natural line height */
float  font_text_width(BhFont *f, const char *s, size_t len, float letter_spacing);
float  font_char_width(BhFont *f, uint32_t cp);
void   canvas_draw_text(Canvas *c, BhFont *f, float x, float baseline, const char *s, size_t len, uint32_t argb,
                        float letter_spacing);

#endif
