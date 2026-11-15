#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int bh_verbose = 0;

void bh_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "blackhole: fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

void bh_log(const char *fmt, ...) {
    if (!bh_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bh] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) bh_die("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t n, size_t m) {
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) bh_die("out of memory (%zu x %zu)", n, m);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) bh_die("out of memory (realloc %zu)", n);
    return q;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---- Buf ---- */
void buf_init(Buf *b) {
    b->data = NULL;
    b->len = b->cap = 0;
}

void buf_free(Buf *b) {
    free(b->data);
    buf_init(b);
}

void buf_clear(Buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = 0;
}

void buf_reserve(Buf *b, size_t extra) {
    if (extra > SIZE_MAX / 2 - b->len) bh_die("buffer overflow");
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_append(Buf *b, const void *p, size_t n) {
    buf_reserve(b, n);
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_putc(Buf *b, char c) { buf_append(b, &c, 1); }

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_put_utf8(Buf *b, uint32_t cp) {
    char tmp[4];
    int n = utf8_encode(cp, tmp);
    buf_append(b, tmp, (size_t)n);
}

char *buf_steal(Buf *b) {
    char *p = b->data;
    if (!p) p = xstrdup("");
    buf_init(b);
    return p;
}

/* ---- Arena ---- */
struct ArenaChunk {
    ArenaChunk *next;
    size_t      cap, used;
    _Alignas(16) unsigned char data[];
};

#define ARENA_CHUNK 65536

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;
    ArenaChunk *c = a->head;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
        ArenaChunk *nc = xcalloc(1, sizeof(ArenaChunk) + cap);
        nc->cap = cap;
        /* oversized chunks go behind the head so the head keeps its free space */
        if (n > ARENA_CHUNK && c) {
            nc->next = c->next;
            c->next = nc;
            nc->used = n;
            return nc->data;
        }
        nc->next = a->head;
        a->head = nc;
        c = nc;
    }
    void *p = c->data + c->used;
    c->used += n;
    return p; /* calloc'd chunk => zeroed, and never reused */
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

void arena_free(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *n = c->next;
        free(c);
        c = n;
    }
    a->head = NULL;
}

/* ---- UTF-8 ---- */
int utf8_decode(const char *s, const char *end, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s;
    size_t avail = (size_t)(end - s);
    if (avail == 0) {
        *cp = 0xFFFD;
        return 1;
    }
    unsigned c = p[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int need;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) { need = 1; v = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { need = 2; v = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { need = 3; v = c & 0x07; min = 0x10000; }
    else { *cp = 0xFFFD; return 1; }
    if ((size_t)need > avail - 1) { *cp = 0xFFFD; return 1; } /* truncated sequence */
    for (int i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) { *cp = 0xFFFD; return 1; }
    *cp = v;
    return need + 1;
}

int utf8_encode(uint32_t cp, char out[4]) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- strings ---- */
bool str_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (to_lower((unsigned char)*a) != to_lower((unsigned char)*b)) return false;
    return *a == *b;
}

bool str_ieqn(const char *a, const char *b, size_t n) {
    for (; n && *a && *b; a++, b++, n--)
        if (to_lower((unsigned char)*a) != to_lower((unsigned char)*b)) return false;
    return n == 0 || *a == *b;
}

bool str_starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

bool str_istarts_with(const char *s, const char *prefix) { return str_ieqn(s, prefix, strlen(prefix)); }

char *str_trim(char *s) {
    while (*s && is_space((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && is_space((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

void str_lower(char *s) {
    for (; *s; s++) *s = (char)to_lower((unsigned char)*s);
}
