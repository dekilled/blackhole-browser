/* A loaded page: DOM + stylesheets + layout. */
#ifndef BH_PAGE_H
#define BH_PAGE_H

#include "css.h"
#include "dom.h"
#include "layout.h"
#include "net.h"

typedef struct PageImage {
    char             *url;
    Image             img;
    bool              failed;
    struct PageImage *next;
} PageImage;

typedef struct Page {
    char        *url;   /* final URL */
    char        *title;
    bool         secure;
    int          status;
    Arena        dom_arena;
    Node        *doc;
    Stylesheet **sheets; /* [0] is the UA sheet (not owned) */
    int          nsheets;
    Arena        style_arena;
    LayoutDoc   *layout;
    float        vw, vh;
    PageImage   *images;
    int          nimages_fetched;
    bool         allow_file_subresources;
    char        *base; /* base URL for relative references */
} Page;

/* Loads and parses a URL. Never returns NULL: on failure returns an error page. */
Page *page_load(const char *url, bool user_initiated);
/* Builds a page from literal HTML (used for error pages and tests). */
Page *page_from_html(const char *url, const char *html);
/* Computes styles and layout for a viewport. Safe to call repeatedly (resize). */
void  page_layout(Page *p, float vw, float vh);
void  page_free(Page *p);

#endif
