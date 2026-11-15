/* PNG decoder: all colour types and bit depths, tRNS, Adam7. Uses zlib only for inflate. */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "img.h"
#include "util.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/* Undoes row filters in place. buf has h rows of (1 + stride) bytes. Returns false on a bad filter id. */
static bool unfilter(uint8_t *buf, size_t stride, int h, int bpp) {
    uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *row = buf + (size_t)y * (stride + 1);
        int      ft = row[0];
        uint8_t *cur = row + 1;
        switch (ft) {
        case 0: break;
        case 1: for (size_t i = (size_t)bpp; i < stride; i++) cur[i] = (uint8_t)(cur[i] + cur[i - (size_t)bpp]); break;
        case 2: if (prev) for (size_t i = 0; i < stride; i++) cur[i] = (uint8_t)(cur[i] + prev[i]); break;
        case 3:
            for (size_t i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? cur[i - (size_t)bpp] : 0, b = prev ? prev[i] : 0;
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? cur[i - (size_t)bpp] : 0, b = prev ? prev[i] : 0,
                    c = (prev && i >= (size_t)bpp) ? prev[i - (size_t)bpp] : 0;
                cur[i] = (uint8_t)(cur[i] + paeth(a, b, c));
            }
            break;
        default: return false;
        }
        prev = cur;
    }
    return true;
}

typedef struct {
    int      w, h, bd, ct, channels;
    uint32_t pal[256];
    int      npal;
    uint8_t  trns_a[256];
    bool     has_trns_key;
    uint16_t key[3];
} PngInfo;

static uint32_t sample_pixel(const PngInfo *in, const uint8_t *row, int x) {
    int      bd = in->bd;
    uint32_t a = 255, r, g, b;
    switch (in->ct) {
    case 0: { /* gray */
        unsigned v;
        if (bd == 16) v = (unsigned)(row[x * 2] << 8 | row[x * 2 + 1]);
        else if (bd == 8) v = row[x];
        else {
            int per = 8 / bd, shift = 8 - bd * (x % per + 1);
            v = (unsigned)((row[x / per] >> shift) & ((1 << bd) - 1));
        }
        if (in->has_trns_key && v == in->key[0]) a = 0;
        if (bd == 16) v >>= 8;
        else if (bd < 8) v = v * 255 / (unsigned)((1 << bd) - 1);
        r = g = b = v;
        break;
    }
    case 2: { /* rgb */
        if (bd == 8) { r = row[x * 3]; g = row[x * 3 + 1]; b = row[x * 3 + 2]; if (in->has_trns_key && r == in->key[0] && g == in->key[1] && b == in->key[2]) a = 0; }
        else {
            unsigned rr = (unsigned)(row[x * 6] << 8 | row[x * 6 + 1]), gg = (unsigned)(row[x * 6 + 2] << 8 | row[x * 6 + 3]),
                     bb = (unsigned)(row[x * 6 + 4] << 8 | row[x * 6 + 5]);
            if (in->has_trns_key && rr == in->key[0] && gg == in->key[1] && bb == in->key[2]) a = 0;
            r = rr >> 8; g = gg >> 8; b = bb >> 8;
        }
        break;
    }
    case 3: { /* palette */
        unsigned idx;
        if (bd == 8) idx = row[x];
        else {
            int per = 8 / bd, shift = 8 - bd * (x % per + 1);
            idx = (unsigned)((row[x / per] >> shift) & ((1 << bd) - 1));
        }
        if ((int)idx >= in->npal) return 0xFF000000u;
        return in->pal[idx];
    }
    case 4: { /* gray + alpha */
        if (bd == 8) { r = g = b = row[x * 2]; a = row[x * 2 + 1]; }
        else { r = g = b = row[x * 4]; a = row[x * 4 + 2]; }
        break;
    }
    default: /* 6: rgba */
        if (bd == 8) { r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3]; }
        else { r = row[x * 8]; g = row[x * 8 + 2]; b = row[x * 8 + 4]; a = row[x * 8 + 6]; }
    }
    return a << 24 | r << 16 | g << 8 | b;
}

bool png_decode(const uint8_t *d, size_t n, Image *out) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 8 + 25 || memcmp(d, sig, 8) != 0) return false;
    PngInfo in;
    memset(&in, 0, sizeof in);
    int  interlace = 0;
    bool have_ihdr = false;
    Buf  idat;
    buf_init(&idat);
    size_t pos = 8;
    bool   ok = false;
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t *type = d + pos + 4, *data = d + pos + 8;
        if (len > n - pos - 12) break;
        if (!memcmp(type, "IHDR", 4) && len == 13) {
            uint32_t w = be32(data), h = be32(data + 4);
            in.bd = data[8]; in.ct = data[9];
            if (data[10] != 0 || data[11] != 0 || data[12] > 1) goto done;
            interlace = data[12];
            if (!w || !h || w > IMG_MAX_DIM || h > IMG_MAX_DIM || (uint64_t)w * h > IMG_MAX_PIXELS) goto done;
            in.w = (int)w; in.h = (int)h;
            switch (in.ct) {
            case 0: if (in.bd != 1 && in.bd != 2 && in.bd != 4 && in.bd != 8 && in.bd != 16) goto done; in.channels = 1; break;
            case 2: if (in.bd != 8 && in.bd != 16) goto done; in.channels = 3; break;
            case 3: if (in.bd != 1 && in.bd != 2 && in.bd != 4 && in.bd != 8) goto done; in.channels = 1; break;
            case 4: if (in.bd != 8 && in.bd != 16) goto done; in.channels = 2; break;
            case 6: if (in.bd != 8 && in.bd != 16) goto done; in.channels = 4; break;
            default: goto done;
            }
            have_ihdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            in.npal = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < in.npal; i++) in.pal[i] = 0xFF000000u | (uint32_t)data[i * 3] << 16 | (uint32_t)data[i * 3 + 1] << 8 | data[i * 3 + 2];
        } else if (!memcmp(type, "tRNS", 4)) {
            if (in.ct == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++)
                    if ((int)i < in.npal) in.pal[i] = (in.pal[i] & 0x00FFFFFFu) | (uint32_t)data[i] << 24;
            } else if (in.ct == 0 && len >= 2) {
                in.has_trns_key = true;
                in.key[0] = (uint16_t)(data[0] << 8 | data[1]);
                if (in.bd == 16) in.key[0] = in.key[0]; /* compared against 16-bit samples */
            } else if (in.ct == 2 && len >= 6) {
                in.has_trns_key = true;
                for (int i = 0; i < 3; i++) in.key[i] = (uint16_t)(data[i * 2] << 8 | data[i * 2 + 1]);
                if (in.bd == 8) for (int i = 0; i < 3; i++) in.key[i] &= 0xFF;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat.len + len > n) goto done; /* cannot legitimately exceed the file itself by much; blocks bombs */
            buf_append(&idat, data, len);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)len;
    }
    if (!have_ihdr || !idat.len) goto done;
    if (in.ct == 3 && in.npal == 0) goto done;

    {
        int bpp_bits = in.channels * in.bd;
        int fbpp = bpp_bits >= 8 ? bpp_bits / 8 : 1;
        static const int xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1}, dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        int npass = interlace ? 7 : 1;
        size_t total = 0;
        size_t pass_off[7] = {0}, pass_stride[7] = {0};
        int    pw[7], ph[7];
        for (int p = 0; p < npass; p++) {
            int w = interlace ? (in.w - xs[p] + dx[p] - 1) / dx[p] : in.w;
            int h = interlace ? (in.h - ys[p] + dy[p] - 1) / dy[p] : in.h;
            if (in.w <= xs[p]) w = 0;
            if (in.h <= ys[p]) h = 0;
            pw[p] = w; ph[p] = h;
            pass_stride[p] = ((size_t)w * (size_t)bpp_bits + 7) / 8;
            pass_off[p] = total;
            if (w > 0 && h > 0) total += (pass_stride[p] + 1) * (size_t)h;
        }
        uint8_t *raw = xmalloc(total ? total : 1);
        z_stream zs;
        memset(&zs, 0, sizeof zs);
        if (inflateInit(&zs) != Z_OK) { free(raw); goto done; }
        zs.next_in = (Bytef *)idat.data;
        zs.avail_in = (uInt)idat.len;
        zs.next_out = raw;
        zs.avail_out = (uInt)total;
        int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END || zs.avail_out != 0) { free(raw); goto done; } /* truncated, or larger than declared */

        uint32_t *px = xmalloc((size_t)in.w * (size_t)in.h * 4);
        bool good = true;
        for (int p = 0; p < npass && good; p++) {
            if (pw[p] <= 0 || ph[p] <= 0) continue;
            uint8_t *base = raw + pass_off[p];
            if (!unfilter(base, pass_stride[p], ph[p], fbpp)) { good = false; break; }
            for (int y = 0; y < ph[p]; y++) {
                const uint8_t *row = base + (size_t)y * (pass_stride[p] + 1) + 1;
                for (int x = 0; x < pw[p]; x++) {
                    int ox = interlace ? xs[p] + x * dx[p] : x, oy = interlace ? ys[p] + y * dy[p] : y;
                    px[(size_t)oy * (size_t)in.w + (size_t)ox] = sample_pixel(&in, row, x);
                }
            }
        }
        free(raw);
        if (!good) { free(px); goto done; }
        out->w = in.w;
        out->h = in.h;
        out->px = px;
        ok = true;
    }
done:
    buf_free(&idat);
    return ok;
}
