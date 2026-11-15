#include "charset.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

const uint16_t cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

char *charset_sniff_meta(const uint8_t *data, size_t len) {
    size_t n = len < 2048 ? len : 2048;
    char  *head = xstrndup((const char *)data, n);
    char  *found = NULL;
    char  *p = strcasestr(head, "charset");
    while (p) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"' || *p == '\'') p++;
            size_t k = 0;
            while (p[k] && (is_alpha((unsigned char)p[k]) || is_digit((unsigned char)p[k]) || p[k] == '-' ||
                            p[k] == '_' || p[k] == ':' || p[k] == '.'))
                k++;
            if (k) {
                found = xstrndup(p, k);
                str_lower(found);
                break;
            }
        }
        p = strcasestr(p, "charset");
    }
    free(head);
    return found;
}

static bool is_latin1_label(const char *l) {
    return !strcmp(l, "iso-8859-1") || !strcmp(l, "latin1") || !strcmp(l, "iso8859-1") || !strcmp(l, "us-ascii") ||
           !strcmp(l, "ascii") || !strcmp(l, "windows-1252") || !strcmp(l, "cp1252") || !strcmp(l, "iso-8859-15") ||
           !strcmp(l, "l1");
}

char *charset_to_utf8(const uint8_t *data, size_t len, const char *label, size_t *out_len) {
    Buf b;
    buf_init(&b);
    /* BOMs win over everything */
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        len -= 3;
        label = "utf-8";
    } else if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
        bool le = data[0] == 0xFF;
        for (size_t i = 2; i + 1 < len; i += 2) {
            uint32_t u = le ? (uint32_t)(data[i] | data[i + 1] << 8) : (uint32_t)(data[i] << 8 | data[i + 1]);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
                uint32_t u2 = le ? (uint32_t)(data[i + 2] | data[i + 3] << 8) : (uint32_t)(data[i + 2] << 8 | data[i + 3]);
                if (u2 >= 0xDC00 && u2 < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00);
                    i += 2;
                }
            }
            buf_put_utf8(&b, u);
        }
        if (out_len) *out_len = b.len;
        return buf_steal(&b);
    }
    if (label && is_latin1_label(label)) {
        bool win = strcmp(label, "iso-8859-1") != 0; /* browsers treat latin1 labels as windows-1252 */
        for (size_t i = 0; i < len; i++) {
            uint8_t c = data[i];
            if (c < 0x80) buf_putc(&b, (char)c);
            else if (win && c < 0xA0) buf_put_utf8(&b, cp1252_high[c - 0x80]);
            else buf_put_utf8(&b, c);
        }
    } else {
        /* UTF-8: re-encode through the decoder so invalid sequences become U+FFFD */
        const char *s = (const char *)data, *end = s + len;
        while (s < end) {
            if ((unsigned char)*s < 0x80) {
                buf_putc(&b, *s++);
                continue;
            }
            uint32_t cp;
            s += utf8_decode(s, end, &cp);
            buf_put_utf8(&b, cp);
        }
    }
    if (out_len) *out_len = b.len;
    return buf_steal(&b);
}
