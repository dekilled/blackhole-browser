#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "util.h"

#define MAX_CANVAS_DIM 16384

Canvas *canvas_new(int w, int h) {
    Canvas *c = xcalloc(1, sizeof *c);
    canvas_resize(c, w, h);
    return c;
}

void canvas_free(Canvas *c) {
    if (!c) return;
    free(c->px);
    free(c);
}

void canvas_resize(Canvas *c, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > MAX_CANVAS_DIM) w = MAX_CANVAS_DIM;
    if (h > MAX_CANVAS_DIM) h = MAX_CANVAS_DIM;
    free(c->px);
    c->px = xcalloc((size_t)w * (size_t)h, sizeof(uint32_t));
    c->w = w;
    c->h = h;
    c->cx0 = c->cy0 = 0;
    c->cx1 = w;
    c->cy1 = h;
}

void canvas_clear(Canvas *c, uint32_t argb) {
    for (size_t i = 0, n = (size_t)c->w * (size_t)c->h; i < n; i++) c->px[i] = argb;
}

void canvas_set_clip(Canvas *c, int x, int y, int w, int h, int old[4]) {
    if (old) {
        old[0] = c->cx0; old[1] = c->cy0; old[2] = c->cx1; old[3] = c->cy1;
    }
    int x1 = x + w, y1 = y + h;
    if (x > c->cx0) c->cx0 = x;
    if (y > c->cy0) c->cy0 = y;
    if (x1 < c->cx1) c->cx1 = x1;
    if (y1 < c->cy1) c->cy1 = y1;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

void canvas_restore_clip(Canvas *c, const int old[4]) {
    c->cx0 = old[0]; c->cy0 = old[1]; c->cx1 = old[2]; c->cy1 = old[3];
}

static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    /* src color (RGB) with coverage/alpha a in 0..255 over dst (assumed opaque) */
    uint32_t ia = 255 - a;
    uint32_t r = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia + 127) / 255;
    uint32_t g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia + 127) / 255;
    uint32_t b = ((src & 255) * a + (dst & 255) * ia + 127) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void canvas_fill_rect(Canvas *c, float fx, float fy, float fw, float fh, uint32_t argb) {
    uint32_t a = argb >> 24;
    if (a == 0 || fw <= 0 || fh <= 0) return;
    int x0 = (int)floorf(fx + 0.5f), y0 = (int)floorf(fy + 0.5f);
    int x1 = (int)floorf(fx + fw + 0.5f), y1 = (int)floorf(fy + fh + 0.5f);
    if (x1 == x0 && fw > 0) x1 = x0 + 1; /* never let a thin rect vanish */
    if (y1 == y0 && fh > 0) y1 = y0 + 1;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (size_t)y * (size_t)c->w;
        if (a == 255) {
            for (int x = x0; x < x1; x++) row[x] = argb;
        } else {
            for (int x = x0; x < x1; x++) row[x] = blend(row[x], argb, a);
        }
    }
}

void canvas_fill_ellipse(Canvas *c, float cx, float cy, float rx, float ry, uint32_t argb) {
    if (rx <= 0 || ry <= 0 || !(argb >> 24)) return;
    int x0 = (int)floorf(cx - rx - 1), x1 = (int)ceilf(cx + rx + 1), y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    uint32_t base = argb >> 24;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            /* 4x4 supersampling for smooth edges */
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float dx = ((float)x + ((float)sx + 0.5f) / 4 - cx) / rx, dy = ((float)y + ((float)sy + 0.5f) / 4 - cy) / ry;
                    if (dx * dx + dy * dy <= 1) hit++;
                }
            if (!hit) continue;
            uint32_t a = base * (uint32_t)hit / 16;
            uint32_t *d = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
            *d = a >= 255 ? argb : blend(*d, argb, a);
        }
}

void canvas_blend_px(Canvas *c, int x, int y, uint32_t argb) {
    if (x < c->cx0 || x >= c->cx1 || y < c->cy0 || y >= c->cy1) return;
    uint32_t a = argb >> 24;
    uint32_t *d = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    if (a == 255) *d = argb;
    else if (a) *d = blend(*d, argb, a);
}

/* Signed-distance coverage of a rounded rectangle at pixel centre (px,py). */
float canvas_rrect_coverage(float px, float py, float x, float y, float w, float h, const float r[4]) {
    float hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    float rr = (px < cx) ? ((py < cy) ? r[0] : r[3]) : ((py < cy) ? r[1] : r[2]);
    if (rr > hw) rr = hw;
    if (rr > hh) rr = hh;
    float qx = fabsf(px - cx) - (hw - rr), qy = fabsf(py - cy) - (hh - rr);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inner = qx > qy ? qx : qy;
    float d = sqrtf(ox * ox + oy * oy) + (inner < 0 ? inner : 0) - rr;
    float cov = 0.5f - d;
    return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

static float max4(const float r[4]) {
    float m = r[0];
    for (int i = 1; i < 4; i++) if (r[i] > m) m = r[i];
    return m;
}

void canvas_fill_rrect(Canvas *c, float fx, float fy, float fw, float fh, const float r[4], uint32_t argb) {
    uint32_t a0 = argb >> 24;
    if (!a0 || fw <= 0 || fh <= 0) return;
    float R = max4(r);
    if (R <= 0.5f) { canvas_fill_rect(c, fx, fy, fw, fh, argb); return; }
    int x0 = (int)floorf(fx + 0.5f), y0 = (int)floorf(fy + 0.5f), x1 = (int)floorf(fx + fw + 0.5f), y1 = (int)floorf(fy + fh + 0.5f);
    float x = (float)x0, y = (float)y0, w = (float)(x1 - x0), h = (float)(y1 - y0);
    if (w <= 0 || h <= 0) return;
    int band = (int)ceilf(R) + 1;
    int top_end = y0 + band;
    if (top_end > y1) top_end = y1;
    int bot_start = y1 - band;
    if (bot_start < top_end) bot_start = top_end;
    if (bot_start > top_end) canvas_fill_rect(c, x, (float)top_end, w, (float)(bot_start - top_end), argb);
    for (int pass = 0; pass < 2; pass++) {
        int ra = pass == 0 ? y0 : bot_start, rb = pass == 0 ? top_end : y1;
        for (int yy = ra; yy < rb; yy++) {
            if (yy < c->cy0 || yy >= c->cy1) continue;
            for (int xx = x0; xx < x1; xx++) {
                if (xx < c->cx0 || xx >= c->cx1) continue;
                float cov = canvas_rrect_coverage((float)xx + 0.5f, (float)yy + 0.5f, x, y, w, h, r);
                if (cov <= 0) continue;
                canvas_blend_px(c, xx, yy, ((uint32_t)((float)a0 * cov) << 24) | (argb & 0xFFFFFF));
            }
        }
    }
}

void canvas_stroke_rrect(Canvas *c, float fx, float fy, float fw, float fh, const float r[4], float t, uint32_t argb) {
    uint32_t a0 = argb >> 24;
    if (!a0 || fw <= 0 || fh <= 0 || t <= 0) return;
    int x0 = (int)floorf(fx + 0.5f), y0 = (int)floorf(fy + 0.5f), x1 = (int)floorf(fx + fw + 0.5f), y1 = (int)floorf(fy + fh + 0.5f);
    float x = (float)x0, y = (float)y0, w = (float)(x1 - x0), h = (float)(y1 - y0);
    float ir[4];
    for (int i = 0; i < 4; i++) ir[i] = r[i] > t ? r[i] - t : 0;
    int band = (int)ceilf(max4(r) + t) + 1;
    for (int yy = y0; yy < y1; yy++) {
        if (yy < c->cy0 || yy >= c->cy1) continue;
        bool edge_row = yy < y0 + band || yy >= y1 - band;
        for (int xx = x0; xx < x1; xx++) {
            if (!edge_row && xx >= x0 + (int)ceilf(t) + 1 && xx < x1 - (int)ceilf(t) - 1) { xx = x1 - (int)ceilf(t) - 2; continue; }
            if (xx < c->cx0 || xx >= c->cx1) continue;
            float px = (float)xx + 0.5f, py = (float)yy + 0.5f;
            float co = canvas_rrect_coverage(px, py, x, y, w, h, r);
            float ci = canvas_rrect_coverage(px, py, x + t, y + t, w - 2 * t, h - 2 * t, ir);
            float cov = co - ci;
            if (cov <= 0) continue;
            canvas_blend_px(c, xx, yy, ((uint32_t)((float)a0 * cov) << 24) | (argb & 0xFFFFFF));
        }
    }
}

void canvas_hline(Canvas *c, int x, int y, int w, uint32_t argb) { canvas_fill_rect(c, (float)x, (float)y, (float)w, 1, argb); }

void canvas_blit_argb(Canvas *c, int x, int y, int w, int h, const uint32_t *src, int stride) {
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if (dy < c->cy0 || dy >= c->cy1) continue;
        for (int i = 0; i < w; i++) {
            int dx = x + i;
            if (dx < c->cx0 || dx >= c->cx1) continue;
            uint32_t s = src[(size_t)j * (size_t)stride + (size_t)i];
            uint32_t a = s >> 24;
            uint32_t *d = &c->px[(size_t)dy * (size_t)c->w + (size_t)dx];
            if (a == 255) *d = s;
            else if (a) *d = blend(*d, s, a);
        }
    }
}

void canvas_draw_image_r(Canvas *c, float dx, float dy, float dw, float dh, const uint32_t *src, int sw, int sh, const float rad[4]) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = (int)floorf(dx + 0.5f), y0 = (int)floorf(dy + 0.5f);
    int x1 = (int)floorf(dx + dw + 0.5f), y1 = (int)floorf(dy + dh + 0.5f);
    int cx0 = x0 < c->cx0 ? c->cx0 : x0, cy0 = y0 < c->cy0 ? c->cy0 : y0;
    int cx1 = x1 > c->cx1 ? c->cx1 : x1, cy1 = y1 > c->cy1 ? c->cy1 : y1;
    float fx = (float)sw / (float)(x1 - x0 > 0 ? x1 - x0 : 1), fy = (float)sh / (float)(y1 - y0 > 0 ? y1 - y0 : 1);
    bool  down = fx > 1.5f || fy > 1.5f; /* box filter when shrinking a lot */
    for (int y = cy0; y < cy1; y++) {
        for (int x = cx0; x < cx1; x++) {
            uint32_t s;
            if (!down) {
                int sx = (int)((float)(x - x0 + 0.5f) * fx), sy = (int)((float)(y - y0 + 0.5f) * fy);
                if (sx >= sw) sx = sw - 1;
                if (sy >= sh) sy = sh - 1;
                s = src[(size_t)sy * (size_t)sw + (size_t)sx];
            } else {
                int sxa = (int)((float)(x - x0) * fx), sxb = (int)((float)(x - x0 + 1) * fx);
                int sya = (int)((float)(y - y0) * fy), syb = (int)((float)(y - y0 + 1) * fy);
                if (sxb <= sxa) sxb = sxa + 1;
                if (syb <= sya) syb = sya + 1;
                if (sxb > sw) sxb = sw;
                if (syb > sh) syb = sh;
                uint64_t A = 0, R = 0, G = 0, B = 0, cnt = 0;
                for (int yy = sya; yy < syb; yy++)
                    for (int xx = sxa; xx < sxb; xx++) {
                        uint32_t p = src[(size_t)yy * (size_t)sw + (size_t)xx];
                        uint32_t pa = p >> 24;
                        A += pa;
                        R += ((p >> 16) & 255) * pa;
                        G += ((p >> 8) & 255) * pa;
                        B += (p & 255) * pa;
                        cnt++;
                    }
                if (!cnt || !A) continue;
                s = (uint32_t)(A / cnt) << 24 | (uint32_t)(R / A) << 16 | (uint32_t)(G / A) << 8 | (uint32_t)(B / A);
            }
            uint32_t a = s >> 24;
            if (rad) {
                float cov = canvas_rrect_coverage((float)x + 0.5f, (float)y + 0.5f, dx, dy, dw, dh, rad);
                a = (uint32_t)((float)a * cov);
            }
            uint32_t *d = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
            if (a == 255) *d = s;
            else if (a) *d = blend(*d, s, a);
        }
    }
}

void canvas_draw_image(Canvas *c, float dx, float dy, float dw, float dh, const uint32_t *src, int sw, int sh) {
    canvas_draw_image_r(c, dx, dy, dw, dh, src, sw, sh, NULL);
}

/* ------------------------------------------------------------------- PNG --- */
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t cb[4];
    put_be32(cb, (uint32_t)crc);
    fwrite(cb, 1, 4, f);
}

bool canvas_write_png(const Canvas *c, const char *path) {
    size_t   rowlen = (size_t)c->w * 3 + 1;
    uint8_t *raw = xmalloc(rowlen * (size_t)c->h);
    for (int y = 0; y < c->h; y++) {
        uint8_t *r = raw + rowlen * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[(size_t)y * (size_t)c->w + (size_t)x];
            *r++ = (uint8_t)(p >> 16);
            *r++ = (uint8_t)(p >> 8);
            *r++ = (uint8_t)p;
        }
    }
    uLongf   zlen = compressBound((uLong)(rowlen * (size_t)c->h));
    uint8_t *z = xmalloc(zlen);
    if (compress2(z, &zlen, raw, (uLong)(rowlen * (size_t)c->h), 6) != Z_OK) {
        free(raw);
        free(z);
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(raw);
        free(z);
        return false;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)c->w);
    put_be32(ihdr + 4, (uint32_t)c->h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zlen);
    png_chunk(f, "IEND", NULL, 0);
    bool ok = fclose(f) == 0;
    free(raw);
    free(z);
    return ok;
}
