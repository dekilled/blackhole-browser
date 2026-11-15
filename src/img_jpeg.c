/* JPEG decoder: baseline, extended sequential and progressive Huffman; 1 or 3 components; any subsampling. */
#include <stdlib.h>
#include <string.h>

#include "img.h"
#include "util.h"

static const uint8_t zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                   41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                   30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

typedef struct {
    uint8_t  counts[17];
    uint8_t  symbols[256];
    int      maxcode[18];
    int      valptr[17];
    int      mincode[17];
    bool     present;
} Huff;

typedef struct {
    int      id, h, v, tq;
    int      bw, bh;       /* blocks per row / column, padded to whole MCUs */
    int      cw, ch;       /* real size in samples */
    int      dc_tbl, ac_tbl;
    int      pred;
    int16_t *coef;         /* bw*bh*64 */
    uint8_t *plane;        /* bw*8 x bh*8 */
} Comp;

typedef struct {
    const uint8_t *p, *end;
    uint32_t       bits;
    int            nbits;
    bool           marker;
} BR;

typedef struct {
    int      w, h, ncomp, hmax, vmax, mcux, mcuy;
    bool     progressive, adobe;
    int      adobe_transform;
    Comp     c[3];
    uint16_t qt[4][64]; /* natural order */
    Huff     dc[4], ac[4];
    int      restart_interval;
    int      eobrun;
} Jpeg;

static void br_fill(BR *b) {
    while (b->nbits <= 24) {
        uint32_t byte = 0;
        if (!b->marker && b->p < b->end) {
            byte = *b->p;
            if (byte == 0xFF) {
                if (b->p + 1 < b->end && b->p[1] == 0) {
                    b->p += 2;
                } else if (b->p + 1 < b->end && b->p[1] == 0xFF) {
                    b->p++; /* fill bytes */
                    continue;
                } else {
                    b->marker = true; /* real marker: stop consuming, feed zeros */
                    byte = 0;
                }
            } else {
                b->p++;
            }
        }
        b->bits |= byte << (24 - b->nbits);
        b->nbits += 8;
    }
}

static uint32_t br_get(BR *b, int n) {
    if (n == 0) return 0;
    if (b->nbits < n) br_fill(b);
    uint32_t v = b->bits >> (32 - n);
    b->bits <<= n;
    b->nbits -= n;
    return v;
}

static int br_bit(BR *b) { return (int)br_get(b, 1); }

static bool huff_build(Huff *h) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->counts[l];
        k += h->counts[l];
        if (code > (1 << l)) return false; /* oversubscribed */
        h->maxcode[l] = h->counts[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->present = true;
    return true;
}

static int huff_decode(BR *b, const Huff *h) {
    if (b->nbits < 16) br_fill(b);
    uint32_t look = b->bits;
    for (int l = 1; l <= 16; l++) {
        int code = (int)(look >> (32 - l));
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) {
            b->bits <<= l;
            b->nbits -= l;
            int idx = h->valptr[l] + code - h->mincode[l];
            return idx >= 0 && idx < 256 ? h->symbols[idx] : -1;
        }
    }
    return -1;
}

static int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

/* ---- block decoders ---- */
static bool decode_block_seq(Jpeg *j, BR *br, Comp *c, int16_t *blk) {
    const Huff *dc = &j->dc[c->dc_tbl], *ac = &j->ac[c->ac_tbl];
    if (!dc->present || !ac->present) return false;
    int t = huff_decode(br, dc);
    if (t < 0 || t > 11) return false;
    int diff = t ? extend((int)br_get(br, t), t) : 0;
    c->pred += diff;
    blk[0] = (int16_t)c->pred;
    for (int k = 1; k < 64;) {
        int rs = huff_decode(br, ac);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k > 63) return false;
        blk[zigzag[k]] = (int16_t)extend((int)br_get(br, s), s);
        k++;
    }
    return true;
}

static bool decode_dc_first(Jpeg *j, BR *br, Comp *c, int16_t *blk, int al) {
    const Huff *dc = &j->dc[c->dc_tbl];
    if (!dc->present) return false;
    int t = huff_decode(br, dc);
    if (t < 0 || t > 11) return false;
    int diff = t ? extend((int)br_get(br, t), t) : 0;
    c->pred += diff;
    blk[0] = (int16_t)(c->pred * (1 << al));
    return true;
}

static bool decode_ac_first(Jpeg *j, BR *br, Comp *c, int16_t *blk, int ss, int se, int al) {
    if (j->eobrun > 0) { j->eobrun--; return true; }
    const Huff *ac = &j->ac[c->ac_tbl];
    if (!ac->present) return false;
    for (int k = ss; k <= se;) {
        int rs = huff_decode(br, ac);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += (int)br_get(br, r);
                break;
            }
            k += 16;
        } else {
            k += r;
            if (k > 63) return false;
            blk[zigzag[k]] = (int16_t)(extend((int)br_get(br, s), s) * (1 << al));
            k++;
        }
    }
    return true;
}

static bool decode_ac_refine(Jpeg *j, BR *br, Comp *c, int16_t *blk, int ss, int se, int al) {
    int p1 = 1 << al, m1 = -(1 << al);
    int k = ss;
    const Huff *ac = &j->ac[c->ac_tbl];
    if (j->eobrun <= 0) {
        if (!ac->present) return false;
        for (; k <= se; k++) {
            int rs = huff_decode(br, ac);
            if (rs < 0) return false;
            int r = rs >> 4, s = rs & 15, val = 0;
            if (s) {
                val = br_bit(br) ? p1 : m1;
            } else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += (int)br_get(br, r);
                break;
            }
            do {
                int16_t *coef = &blk[zigzag[k]];
                if (*coef != 0) {
                    if (br_bit(br) && (*coef & p1) == 0) *coef = (int16_t)(*coef + (*coef >= 0 ? p1 : m1));
                } else {
                    if (--r < 0) break;
                }
                k++;
            } while (k <= se);
            if (val && k <= 63) blk[zigzag[k]] = (int16_t)val;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            int16_t *coef = &blk[zigzag[k]];
            if (*coef != 0 && br_bit(br) && (*coef & p1) == 0) *coef = (int16_t)(*coef + (*coef >= 0 ? p1 : m1));
        }
        j->eobrun--;
    }
    return true;
}

/* ---- IDCT (integer, "islow" style) ---- */
#define F2F(x) ((int)((x) * 4096 + 0.5))
#define FSH(x) ((x) * 4096)

#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                  \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                      \
    p2 = s2; p3 = s6;                                                            \
    p1 = (p2 + p3) * F2F(0.5411961f);                                            \
    t2 = p1 + p3 * F2F(-1.847759065f);                                           \
    t3 = p1 + p2 * F2F(0.765366865f);                                            \
    p2 = s0; p3 = s4;                                                            \
    t0 = FSH(p2 + p3);                                                           \
    t1 = FSH(p2 - p3);                                                           \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;                      \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1;                                          \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;                      \
    p5 = (p3 + p4) * F2F(1.175875602f);                                          \
    t0 = t0 * F2F(0.298631336f); t1 = t1 * F2F(2.053119869f);                    \
    t2 = t2 * F2F(3.072711026f); t3 = t3 * F2F(1.501321110f);                    \
    p1 = p5 + p1 * F2F(-0.899976223f); p2 = p5 + p2 * F2F(-2.562915447f);        \
    p3 = p3 * F2F(-1.961570560f); p4 = p4 * F2F(-0.390180644f);                  \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static inline uint8_t clamp8(int x) { return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)x; }

static void idct_block(uint8_t *out, int stride, const int16_t *coef, const uint16_t *qt) {
    int d[64], val[64];
    for (int i = 0; i < 64; i++) d[i] = coef[i] * qt[i];
    int *v = val;
    for (int i = 0; i < 8; i++, v++) {
        int *dd = d + i;
        if (!dd[8] && !dd[16] && !dd[24] && !dd[32] && !dd[40] && !dd[48] && !dd[56]) {
            int dc = dd[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(dd[0], dd[8], dd[16], dd[24], dd[32], dd[40], dd[48], dd[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10;  v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10;  v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
        }
    }
    v = val;
    for (int i = 0; i < 8; i++, v += 8, out += stride) {
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        out[0] = clamp8((x0 + t3) >> 17); out[7] = clamp8((x0 - t3) >> 17);
        out[1] = clamp8((x1 + t2) >> 17); out[6] = clamp8((x1 - t2) >> 17);
        out[2] = clamp8((x2 + t1) >> 17); out[5] = clamp8((x2 - t1) >> 17);
        out[3] = clamp8((x3 + t0) >> 17); out[4] = clamp8((x3 - t0) >> 17);
    }
}

/* ---- scan decoding ---- */
static void jfree(Jpeg *j) {
    for (int i = 0; i < 3; i++) {
        free(j->c[i].coef);
        free(j->c[i].plane);
    }
}

static bool decode_scan(Jpeg *j, const uint8_t *data, size_t len, size_t *pos, int ncs, const int *cs, int ss, int se, int ah, int al) {
    BR br = {data + *pos, data + len, 0, 0, false};
    for (int i = 0; i < ncs; i++) j->c[cs[i]].pred = 0;
    j->eobrun = 0;
    int total_units, units_x = 0;
    if (ncs == 1) {
        Comp *c = &j->c[cs[0]];
        units_x = (c->cw + 7) / 8;
        int units_y = (c->ch + 7) / 8;
        total_units = units_x * units_y;
    } else {
        units_x = j->mcux;
        total_units = j->mcux * j->mcuy;
    }
    int restart_count = 0;
    for (int u = 0; u < total_units; u++) {
        if (j->restart_interval && u > 0 && u % j->restart_interval == 0) {
            /* align and consume RSTn */
            br.bits = 0;
            br.nbits = 0;
            const uint8_t *p = br.p;
            while (p + 1 < br.end && !(p[0] == 0xFF && p[1] >= 0xD0 && p[1] <= 0xD7)) {
                if (p[0] == 0xFF && p[1] != 0 && p[1] != 0xFF) break; /* some other marker: stop */
                p++;
            }
            if (p + 1 < br.end && p[0] == 0xFF && p[1] >= 0xD0 && p[1] <= 0xD7) { br.p = p + 2; br.marker = false; }
            else { br.p = p; br.marker = false; }
            for (int i = 0; i < ncs; i++) j->c[cs[i]].pred = 0;
            j->eobrun = 0;
            restart_count++;
        }
        if (ncs == 1) {
            Comp *c = &j->c[cs[0]];
            int   bx = u % units_x, by = u / units_x;
            if (bx >= c->bw || by >= c->bh) return false;
            int16_t *blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
            bool ok;
            if (!j->progressive) ok = decode_block_seq(j, &br, c, blk);
            else if (ss == 0) ok = ah == 0 ? decode_dc_first(j, &br, c, blk, al) : (br_bit(&br) ? (blk[0] |= (int16_t)(1 << al), true) : true);
            else ok = ah == 0 ? decode_ac_first(j, &br, c, blk, ss, se, al) : decode_ac_refine(j, &br, c, blk, ss, se, al);
            if (!ok) return false;
        } else {
            int mx = u % j->mcux, my = u / j->mcux;
            for (int i = 0; i < ncs; i++) {
                Comp *c = &j->c[cs[i]];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int bx = mx * c->h + h, by = my * c->v + v;
                        int16_t *blk = c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
                        bool ok;
                        if (!j->progressive) ok = decode_block_seq(j, &br, c, blk);
                        else ok = ah == 0 ? decode_dc_first(j, &br, c, blk, al) : (br_bit(&br) ? (blk[0] |= (int16_t)(1 << al), true) : true);
                        if (!ok) return false;
                    }
            }
        }
        if (br.marker && br.nbits <= 0 && u + 1 < total_units && !j->restart_interval) break; /* data ended early: keep what we have */
    }
    (void)restart_count;
    /* advance to the next marker */
    const uint8_t *p = br.p;
    while (p + 1 < br.end && !(p[0] == 0xFF && p[1] != 0 && !(p[1] >= 0xD0 && p[1] <= 0xD7) && p[1] != 0xFF)) p++;
    *pos = (size_t)(p - data);
    return true;
}

bool jpeg_decode(const uint8_t *d, size_t n, Image *out) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    Jpeg *j = xcalloc(1, sizeof *j);
    bool  ok = false, have_frame = false, have_scan = false;
    size_t pos = 2;
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) { pos++; continue; }
        int m = d[pos + 1];
        if (m == 0xFF) { pos++; continue; }
        if (m == 0 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { pos += 2; continue; }
        if (m == 0xD9) break;
        if (pos + 4 > n) break;
        size_t seg = (size_t)(d[pos + 2] << 8 | d[pos + 3]);
        if (seg < 2 || pos + 2 + seg > n) goto done;
        const uint8_t *s = d + pos + 4;
        size_t         sl = seg - 2;
        switch (m) {
        case 0xDB: { /* DQT */
            size_t i = 0;
            while (i < sl) {
                int pq = s[i] >> 4, tq = s[i] & 15;
                i++;
                if (tq > 3) goto done;
                size_t need = pq ? 128 : 64;
                if (i + need > sl) goto done;
                for (int k = 0; k < 64; k++) {
                    j->qt[tq][zigzag[k]] = pq ? (uint16_t)(s[i + (size_t)k * 2] << 8 | s[i + (size_t)k * 2 + 1]) : s[i + (size_t)k];
                }
                i += need;
            }
            break;
        }
        case 0xC4: { /* DHT */
            size_t i = 0;
            while (i + 17 <= sl) {
                int tc = s[i] >> 4, th = s[i] & 15;
                if (tc > 1 || th > 3) goto done;
                Huff *h = tc ? &j->ac[th] : &j->dc[th];
                memset(h, 0, sizeof *h);
                int total = 0;
                for (int l = 1; l <= 16; l++) { h->counts[l] = s[i + (size_t)l]; total += h->counts[l]; }
                i += 17;
                if (total > 256 || i + (size_t)total > sl) goto done;
                memcpy(h->symbols, s + i, (size_t)total);
                i += (size_t)total;
                if (!huff_build(h)) goto done;
            }
            break;
        }
        case 0xDD: if (sl >= 2) j->restart_interval = s[0] << 8 | s[1]; break;
        case 0xEE: if (sl >= 12 && !memcmp(s, "Adobe", 5)) { j->adobe = true; j->adobe_transform = s[11]; } break;
        case 0xC0: case 0xC1: case 0xC2: { /* SOF */
            if (have_frame || sl < 6) goto done;
            if (s[0] != 8) goto done; /* 8-bit samples only */
            j->progressive = m == 0xC2;
            j->h = s[1] << 8 | s[2];
            j->w = s[3] << 8 | s[4];
            j->ncomp = s[5];
            if (j->w <= 0 || j->h <= 0 || j->w > IMG_MAX_DIM || j->h > IMG_MAX_DIM || (uint64_t)j->w * (uint64_t)j->h > IMG_MAX_PIXELS) goto done;
            if ((j->ncomp != 1 && j->ncomp != 3) || sl < (size_t)6 + (size_t)j->ncomp * 3) goto done;
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->c[i];
                c->id = s[6 + i * 3];
                c->h = s[7 + i * 3] >> 4;
                c->v = s[7 + i * 3] & 15;
                c->tq = s[8 + i * 3];
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) goto done;
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->c[i];
                c->bw = j->mcux * c->h;
                c->bh = j->mcuy * c->v;
                c->cw = (j->w * c->h + j->hmax - 1) / j->hmax;
                c->ch = (j->h * c->v + j->vmax - 1) / j->vmax;
                size_t nb = (size_t)c->bw * (size_t)c->bh;
                if (nb > (size_t)IMG_MAX_PIXELS / 16) goto done;
                c->coef = xcalloc(nb * 64, sizeof(int16_t));
            }
            have_frame = true;
            break;
        }
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            goto done; /* lossless / arithmetic / hierarchical: unsupported */
        case 0xDA: { /* SOS */
            if (!have_frame || sl < 1) goto done;
            int ncs = s[0];
            if (ncs < 1 || ncs > j->ncomp || sl < (size_t)(1 + ncs * 2 + 3)) goto done;
            int cs[3];
            for (int i = 0; i < ncs; i++) {
                int id = s[1 + i * 2], tt = s[2 + i * 2];
                int idx = -1;
                for (int k = 0; k < j->ncomp; k++) if (j->c[k].id == id) idx = k;
                if (idx < 0) goto done;
                cs[i] = idx;
                j->c[idx].dc_tbl = (tt >> 4) & 3;
                j->c[idx].ac_tbl = tt & 3;
            }
            int ss = s[1 + ncs * 2], se = s[2 + ncs * 2], ahal = s[3 + ncs * 2];
            int ah = ahal >> 4, al = ahal & 15;
            if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
            if (ss > se || se > 63 || al > 13) goto done;
            if (j->progressive && ss > 0 && ncs != 1) goto done;
            size_t spos = pos + 2 + seg;
            if (!decode_scan(j, d, n, &spos, ncs, cs, ss, se, ah, al)) {
                if (!have_scan) goto done; /* nothing decoded yet */
                spos = n;                   /* keep partial progressive image */
            }
            have_scan = true;
            pos = spos;
            continue;
        }
        default: break;
        }
        pos += 2 + seg;
    }
    if (!have_frame || !have_scan) goto done;

    /* IDCT into planes */
    for (int i = 0; i < j->ncomp; i++) {
        Comp *c = &j->c[i];
        size_t pw = (size_t)c->bw * 8, ph = (size_t)c->bh * 8;
        c->plane = xmalloc(pw * ph);
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++)
                idct_block(c->plane + ((size_t)by * 8) * pw + (size_t)bx * 8, (int)pw, c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, j->qt[c->tq]);
        free(c->coef);
        c->coef = NULL;
    }
    uint32_t *px = xmalloc((size_t)j->w * (size_t)j->h * 4);
    bool      ycc = j->ncomp == 3 && !(j->adobe && j->adobe_transform == 0);
    for (int y = 0; y < j->h; y++) {
        for (int x = 0; x < j->w; x++) {
            int comp[3];
            for (int i = 0; i < j->ncomp; i++) {
                Comp  *c = &j->c[i];
                size_t stride = (size_t)c->bw * 8;
                if (c->h == j->hmax && c->v == j->vmax) {
                    comp[i] = c->plane[(size_t)y * stride + (size_t)x];
                } else {
                    /* subsampled plane: bilinear interpolation at pixel centres (fixed point, 1/16 steps) */
                    int fx = ((2 * x + 1) * c->h * 16) / (2 * j->hmax) - 8, fy = ((2 * y + 1) * c->v * 16) / (2 * j->vmax) - 8;
                    int x0 = fx >> 4, y0 = fy >> 4, wx = fx & 15, wy = fy & 15;
                    int x1 = x0 + 1, y1 = y0 + 1;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    if (x1 < 0) x1 = 0;
                    if (y1 < 0) y1 = 0;
                    if (x0 >= c->cw) x0 = c->cw - 1;
                    if (x1 >= c->cw) x1 = c->cw - 1;
                    if (y0 >= c->ch) y0 = c->ch - 1;
                    if (y1 >= c->ch) y1 = c->ch - 1;
                    int a = c->plane[(size_t)y0 * stride + (size_t)x0], b = c->plane[(size_t)y0 * stride + (size_t)x1];
                    int cc = c->plane[(size_t)y1 * stride + (size_t)x0], dd = c->plane[(size_t)y1 * stride + (size_t)x1];
                    int top = a * (16 - wx) + b * wx, bot = cc * (16 - wx) + dd * wx;
                    comp[i] = (top * (16 - wy) + bot * wy + 128) >> 8;
                }
            }
            uint32_t r, g, b;
            if (j->ncomp == 1) r = g = b = (uint32_t)comp[0];
            else if (ycc) {
                int Y = comp[0], cb = comp[1] - 128, cr = comp[2] - 128;
                r = clamp8(Y + ((91881 * cr) >> 16));
                g = clamp8(Y - ((22554 * cb + 46802 * cr) >> 16));
                b = clamp8(Y + ((116130 * cb) >> 16));
            } else { r = (uint32_t)comp[0]; g = (uint32_t)comp[1]; b = (uint32_t)comp[2]; }
            px[(size_t)y * (size_t)j->w + (size_t)x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
    out->w = j->w;
    out->h = j->h;
    out->px = px;
    ok = true;
done:
    jfree(j);
    free(j);
    return ok;
}
