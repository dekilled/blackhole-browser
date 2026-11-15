/* Image format sniffing + GIF decoder (first frame). PNG lives in img_png.c, JPEG in img_jpeg.c. */
#include <stdlib.h>
#include <string.h>

#include "img.h"
#include "util.h"

bool gif_decode(const uint8_t *d, size_t n, Image *out) {
    if (n < 13 || memcmp(d, "GIF8", 4) != 0) return false;
    int      sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8;
    int      flags = d[10];
    if (sw <= 0 || sh <= 0 || sw > IMG_MAX_DIM || sh > IMG_MAX_DIM || (uint64_t)sw * (uint64_t)sh > IMG_MAX_PIXELS) return false;
    size_t   pos = 13;
    uint32_t gct[256];
    int      ngct = 0;
    if (flags & 0x80) {
        ngct = 2 << (flags & 7);
        if (pos + (size_t)ngct * 3 > n) return false;
        for (int i = 0; i < ngct; i++) gct[i] = 0xFF000000u | (uint32_t)d[pos + (size_t)i * 3] << 16 | (uint32_t)d[pos + (size_t)i * 3 + 1] << 8 | d[pos + (size_t)i * 3 + 2];
        pos += (size_t)ngct * 3;
    }
    int transparent = -1;
    while (pos < n) {
        int b = d[pos++];
        if (b == 0x21) { /* extension */
            if (pos >= n) return false;
            int label = d[pos++];
            while (pos < n) {
                int sz = d[pos++];
                if (sz == 0) break;
                if (pos + (size_t)sz > n) return false;
                if (label == 0xF9 && sz >= 4 && (d[pos] & 1)) transparent = d[pos + 3];
                pos += (size_t)sz;
            }
        } else if (b == 0x2C) { /* image descriptor */
            if (pos + 9 > n) return false;
            int ix = d[pos] | d[pos + 1] << 8, iy = d[pos + 2] | d[pos + 3] << 8, iw = d[pos + 4] | d[pos + 5] << 8, ih = d[pos + 6] | d[pos + 7] << 8;
            int lf = d[pos + 8];
            pos += 9;
            uint32_t  lct[256];
            uint32_t *pal = gct;
            int       npal = ngct;
            if (lf & 0x80) {
                npal = 2 << (lf & 7);
                if (pos + (size_t)npal * 3 > n) return false;
                for (int i = 0; i < npal; i++) lct[i] = 0xFF000000u | (uint32_t)d[pos + (size_t)i * 3] << 16 | (uint32_t)d[pos + (size_t)i * 3 + 1] << 8 | d[pos + (size_t)i * 3 + 2];
                pos += (size_t)npal * 3;
                pal = lct;
            }
            if (npal == 0 || iw <= 0 || ih <= 0 || (uint64_t)iw * (uint64_t)ih > IMG_MAX_PIXELS) return false;
            bool interlaced = lf & 0x40;
            if (pos >= n) return false;
            int min_code = d[pos++];
            if (min_code < 2 || min_code > 11) return false;
            /* gather sub-blocks */
            Buf data;
            buf_init(&data);
            while (pos < n) {
                int sz = d[pos++];
                if (sz == 0) break;
                if (pos + (size_t)sz > n) { buf_free(&data); return false; }
                buf_append(&data, d + pos, (size_t)sz);
                pos += (size_t)sz;
            }
            /* LZW */
            uint8_t *idx = xcalloc((size_t)iw * (size_t)ih, 1);
            int      clear = 1 << min_code, eoi = clear + 1, next = eoi + 1, size = min_code + 1;
            uint16_t prefix[4096];
            uint8_t  suffix[4096], stack[4100];
            int      prev = -1;
            uint32_t acc = 0;
            int      nb = 0;
            size_t   ip = 0, op = 0, total = (size_t)iw * (size_t)ih;
            for (int i = 0; i < clear; i++) { prefix[i] = 0; suffix[i] = (uint8_t)i; }
            while (op < total) {
                while (nb < size && ip < data.len) { acc |= (uint32_t)(uint8_t)data.data[ip++] << nb; nb += 8; }
                if (nb < size) break;
                int code = (int)(acc & (uint32_t)((1 << size) - 1));
                acc >>= size;
                nb -= size;
                if (code == clear) { next = eoi + 1; size = min_code + 1; prev = -1; continue; }
                if (code == eoi) break;
                if (prev < 0) { /* first code after a clear must be a literal */
                    if (code >= clear) break;
                    idx[op++] = (uint8_t)code;
                    prev = code;
                    continue;
                }
                int     start = code < next ? code : (code == next ? prev : -1);
                if (start < 0) break; /* corrupt stream */
                int     sp = 0, c = start;
                while (c >= clear) {
                    if (sp >= 4096) break;
                    stack[sp++] = suffix[c];
                    c = prefix[c];
                }
                stack[sp++] = suffix[c & 0xFF];
                uint8_t firstc = stack[sp - 1];
                while (sp > 0 && op < total) idx[op++] = stack[--sp];
                if (code == next && op < total) idx[op++] = firstc; /* KwKwK: string(prev) + its first char */
                if (next < 4096) {
                    prefix[next] = (uint16_t)prev;
                    suffix[next] = firstc;
                    next++;
                    if (next == (1 << size) && size < 12) size++;
                }
                prev = code;
            }
            buf_free(&data);
            uint32_t *px = xcalloc((size_t)sw * (size_t)sh, 4);
            int pass_start[4] = {0, 4, 2, 1}, pass_step[4] = {8, 8, 4, 2};
            int srow = 0, pass = 0, row = interlaced ? 0 : 0;
            for (int sr = 0; sr < ih; sr++) {
                int dy;
                if (!interlaced) dy = sr;
                else {
                    if (srow == 0 && sr == 0) { pass = 0; row = pass_start[0]; }
                    dy = row;
                    row += pass_step[pass];
                    while (row >= ih && pass < 3) { pass++; row = pass_start[pass]; }
                    srow++;
                }
                int oy = iy + dy;
                if (oy < 0 || oy >= sh) continue;
                for (int x = 0; x < iw; x++) {
                    int ox = ix + x;
                    if (ox < 0 || ox >= sw) continue;
                    int c = idx[(size_t)sr * (size_t)iw + (size_t)x];
                    if (c == transparent || c >= npal) continue;
                    px[(size_t)oy * (size_t)sw + (size_t)ox] = pal[c];
                }
            }
            free(idx);
            out->w = sw;
            out->h = sh;
            out->px = px;
            return true;
        } else if (b == 0x3B) {
            break;
        } else {
            return false;
        }
    }
    return false;
}

bool image_decode(const uint8_t *d, size_t n, Image *out) {
    memset(out, 0, sizeof *out);
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P') return png_decode(d, n, out);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_decode(d, n, out);
    if (n >= 6 && !memcmp(d, "GIF8", 4)) return gif_decode(d, n, out);
    return false;
}

void image_free(Image *img) {
    free(img->px);
    img->px = NULL;
}
