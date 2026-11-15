/* Fonts via FreeType: face loading, glyph cache, metrics, text drawing. No shaping yet (no ligatures/complex scripts). */
#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "util.h"

typedef struct {
    uint32_t cp;
    bool     used;
    int      left, top, w, h;
    float    advance;
    uint8_t *bitmap; /* 8-bit coverage, w*h */
} Glyph;

#define GLYPH_SLOTS 2048

typedef struct FaceEntry {
    FT_Face face;
    char    path[512];
} FaceEntry;

struct BhFont {
    FaceEntry *chain[4]; /* primary + fallbacks */
    int        nchain;
    float      size;
    int        family;
    bool       bold, italic;
    float      ascent, descent, line_height;
    Glyph     *glyphs; /* one cache for the whole chain, keyed by code point */
};

static FT_Library g_ft;
static bool       g_ft_failed;

static const char *font_dirs[] = {
    "/usr/share/fonts/truetype/liberation", "/usr/share/fonts/truetype/liberation2", "/usr/share/fonts/liberation",
    "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu", "/usr/share/fonts/TTF",
    "/usr/share/fonts/truetype/freefont", "/usr/share/fonts/truetype/noto", "/usr/share/fonts/opentype/noto",
    "/usr/share/fonts/truetype/wqy", "/usr/local/share/fonts", "/usr/share/fonts/truetype", NULL,
};

static FaceEntry *load_face(const char *file) {
    static FaceEntry *cache[32];
    static char       names[32][64];
    static int        n;
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], file)) return cache[i];
    if (!g_ft && !g_ft_failed) {
        if (FT_Init_FreeType(&g_ft)) g_ft_failed = true;
    }
    if (g_ft_failed || n >= 32) return NULL;
    for (int d = 0; font_dirs[d]; d++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", font_dirs[d], file);
        FT_Face face;
        if (FT_New_Face(g_ft, path, 0, &face) == 0) {
            FaceEntry *fe = xcalloc(1, sizeof *fe);
            fe->face = face;
            snprintf(fe->path, sizeof fe->path, "%s", path);
            snprintf(names[n], sizeof names[n], "%s", file);
            cache[n++] = fe;
            bh_log("font: loaded %s", path);
            return fe;
        }
    }
    snprintf(names[n], sizeof names[n], "%s", file);
    cache[n++] = NULL;
    return NULL;
}

static FaceEntry *pick_primary(int family, bool bold, bool italic) {
    static const char *sans[2][2] = {{"LiberationSans-Regular.ttf", "LiberationSans-Italic.ttf"},
                                     {"LiberationSans-Bold.ttf", "LiberationSans-BoldItalic.ttf"}};
    static const char *serif[2][2] = {{"LiberationSerif-Regular.ttf", "LiberationSerif-Italic.ttf"},
                                      {"LiberationSerif-Bold.ttf", "LiberationSerif-BoldItalic.ttf"}};
    static const char *mono[2][2] = {{"LiberationMono-Regular.ttf", "LiberationMono-Italic.ttf"},
                                     {"LiberationMono-Bold.ttf", "LiberationMono-BoldItalic.ttf"}};
    static const char *dj[3][2] = {{"DejaVuSans.ttf", "DejaVuSans-Bold.ttf"},
                                   {"DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf"},
                                   {"DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf"}};
    const char *(*tbl)[2] = family == FONT_SERIF ? serif : family == FONT_MONO ? mono : sans;
    FaceEntry *f = load_face(tbl[bold][italic]);
    if (!f && italic) f = load_face(tbl[bold][0]);
    if (!f) f = load_face(dj[family][bold]);
    if (!f) f = load_face("DejaVuSans.ttf");
    if (!f) f = load_face("FreeSans.ttf");
    return f;
}

bool font_available(void) { return pick_primary(FONT_SANS, false, false) != NULL; }

BhFont *font_get(int family, bool bold, bool italic, float size) {
    static BhFont **cache;
    static int    n, cap;
    if (size < 1) size = 1;
    if (size > 400) size = 400;
    float qs = floorf(size * 4 + 0.5f) / 4; /* quarter-pixel size buckets bound the cache */
    for (int i = 0; i < n; i++) {
        BhFont *f = cache[i];
        if (f->size == qs && f->family == family && f->bold == bold && f->italic == italic) return f;
    }
    FaceEntry *prim = pick_primary(family, bold, italic);
    if (!prim) return NULL;
    BhFont *f = xcalloc(1, sizeof *f);
    if (n == cap) {
        cap = cap ? cap * 2 : 32;
        cache = xrealloc(cache, sizeof(BhFont *) * (size_t)cap);
    }
    cache[n++] = f;
    f->size = qs;
    f->family = family;
    f->bold = bold;
    f->italic = italic;
    f->chain[f->nchain++] = prim;
    FaceEntry *dj = load_face("DejaVuSans.ttf");
    if (dj && dj != prim) f->chain[f->nchain++] = dj;
    FaceEntry *cjk = load_face("wqy-zenhei.ttc");
    if (!cjk) cjk = load_face("fonts-japanese-gothic.ttf");
    if (cjk && f->nchain < 4) f->chain[f->nchain++] = cjk;
    FT_Set_Char_Size(prim->face, 0, (FT_F26Dot6)(qs * 64), 72, 72);
    FT_Size_Metrics *m = &prim->face->size->metrics;
    f->ascent = (float)m->ascender / 64.0f;
    f->descent = -(float)m->descender / 64.0f;
    f->line_height = (float)m->height / 64.0f;
    if (f->line_height < f->ascent + f->descent) f->line_height = f->ascent + f->descent;
    return f;
}

float font_ascent(BhFont *f) { return f->ascent; }
float font_descent(BhFont *f) { return f->descent; }
float font_line_gap_height(BhFont *f) { return f->line_height; }

static Glyph *glyph_lookup(BhFont *f, uint32_t cp) {
    if (!f->glyphs) f->glyphs = xcalloc(GLYPH_SLOTS, sizeof(Glyph));
    Glyph *tbl = f->glyphs;
    uint32_t h = (cp * 2654435761u) % GLYPH_SLOTS;
    for (int probe = 0; probe < GLYPH_SLOTS; probe++) {
        Glyph *g = &tbl[(h + (uint32_t)probe) % GLYPH_SLOTS];
        if (g->used && g->cp == cp) return g;
        if (!g->used) {
            /* load: first face in the chain that has the glyph */
            FaceEntry *use = f->chain[0];
            FT_UInt    gi = 0;
            for (int i = 0; i < f->nchain; i++) {
                gi = FT_Get_Char_Index(f->chain[i]->face, cp);
                if (gi) { use = f->chain[i]; break; }
            }
            FT_Set_Char_Size(use->face, 0, (FT_F26Dot6)(f->size * 64), 72, 72);
            g->used = true;
            g->cp = cp;
            if (FT_Load_Glyph(use->face, gi, FT_LOAD_TARGET_LIGHT) == 0 &&
                FT_Render_Glyph(use->face->glyph, FT_RENDER_MODE_NORMAL) == 0) {
                FT_GlyphSlot s = use->face->glyph;
                g->advance = (float)s->linearHoriAdvance / 65536.0f;
                if (g->advance <= 0) g->advance = (float)s->advance.x / 64.0f;
                g->left = s->bitmap_left;
                g->top = s->bitmap_top;
                g->w = (int)s->bitmap.width;
                g->h = (int)s->bitmap.rows;
                if (g->w > 0 && g->h > 0 && s->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                    g->bitmap = xmalloc((size_t)g->w * (size_t)g->h);
                    for (int y = 0; y < g->h; y++)
                        memcpy(g->bitmap + (size_t)y * (size_t)g->w, s->bitmap.buffer + (size_t)y * (size_t)s->bitmap.pitch,
                               (size_t)g->w);
                }
            }
            return g;
        }
    }
    return NULL;
}

float font_char_width(BhFont *f, uint32_t cp) {
    Glyph *g = glyph_lookup(f, cp);
    return g ? g->advance : f->size * 0.5f;
}

float font_text_width(BhFont *f, const char *s, size_t len, float ls) {
    float       w = 0;
    const char *end = s + len;
    while (s < end) {
        uint32_t cp;
        s += utf8_decode(s, end, &cp);
        if (cp == 0xAD) continue; /* soft hyphen invisible */
        w += font_char_width(f, cp) + ls;
    }
    return w;
}

void canvas_draw_text(Canvas *c, BhFont *f, float x, float baseline, const char *s, size_t len, uint32_t argb, float ls) {
    uint32_t a0 = argb >> 24;
    if (!a0) return;
    const char *end = s + len;
    int         by = (int)floorf(baseline + 0.5f);
    if (by + (int)f->descent + 2 < c->cy0 || by - (int)f->ascent - 2 > c->cy1) return; /* vertically clipped */
    while (s < end) {
        uint32_t cp;
        s += utf8_decode(s, end, &cp);
        if (cp == 0xAD) continue;
        Glyph *g = glyph_lookup(f, cp);
        if (!g) continue;
        if (g->bitmap) {
            int gx = (int)floorf(x + 0.5f) + g->left, gy = by - g->top;
            for (int j = 0; j < g->h; j++) {
                int dy = gy + j;
                if (dy < c->cy0 || dy >= c->cy1) continue;
                for (int i = 0; i < g->w; i++) {
                    int dx = gx + i;
                    if (dx < c->cx0 || dx >= c->cx1) continue;
                    uint32_t cov = g->bitmap[(size_t)j * (size_t)g->w + (size_t)i];
                    if (!cov) continue;
                    uint32_t a = cov * a0 / 255;
                    uint32_t *d = &c->px[(size_t)dy * (size_t)c->w + (size_t)dx];
                    uint32_t ia = 255 - a;
                    uint32_t r = (((argb >> 16) & 255) * a + ((*d >> 16) & 255) * ia + 127) / 255;
                    uint32_t gg = (((argb >> 8) & 255) * a + ((*d >> 8) & 255) * ia + 127) / 255;
                    uint32_t b = ((argb & 255) * a + (*d & 255) * ia + 127) / 255;
                    *d = 0xFF000000u | (r << 16) | (gg << 8) | b;
                }
            }
        }
        x += g->advance + ls;
    }
}
