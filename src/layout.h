/* Layout engine: box tree, block/inline/table layout, painting and hit testing. */
#ifndef BH_LAYOUT_H
#define BH_LAYOUT_H

#include "dom.h"
#include "gfx.h"
#include "style.h"

typedef struct {
    int       w, h;
    uint32_t *px; /* ARGB, owned by whoever created it */
} Image;

/* Returns the decoded image for an <img> element or NULL (not loaded / failed). */
typedef Image *(*ImageResolver)(void *user, const Node *img);

typedef struct LayoutDoc LayoutDoc;

typedef struct {
    const Node *node; /* deepest element under the point */
    const Node *link; /* enclosing <a href>, if any */
} HitResult;

/* Builds the box tree and lays it out for a viewport of the given width. Styles must be computed. */
LayoutDoc *layout_create(Node *doc, float viewport_w, float viewport_h, ImageResolver resolver, void *user);
void       layout_free(LayoutDoc *d);
float      layout_doc_height(const LayoutDoc *d);
float      layout_doc_width(const LayoutDoc *d);

/* Paints the document region starting at scroll_y into the canvas rectangle (x, y, w, h). */
void layout_paint(const LayoutDoc *d, Canvas *c, int x, int y, int w, int h, float scroll_y);
bool layout_hit_test(const LayoutDoc *d, float doc_x, float doc_y, float scroll_y, HitResult *out);
/* Document-space y of the element with the given id (for #fragment navigation). Returns -1 if absent. */
float layout_find_anchor(const LayoutDoc *d, const char *id);

#endif
