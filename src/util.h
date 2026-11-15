/* BlackHole Browser - base utilities: safe allocation, growable buffer, arena, UTF-8, strings. */
#ifndef BH_UTIL_H
#define BH_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- allocation: abort on OOM (a browser that continues after OOM is a security bug) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- growable byte buffer; always NUL terminated ---- */
typedef struct {
    char  *data;
    size_t len, cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_clear(Buf *b);
void  buf_reserve(Buf *b, size_t extra);
void  buf_append(Buf *b, const void *p, size_t n);
void  buf_puts(Buf *b, const char *s);
void  buf_putc(Buf *b, char c);
void  buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void  buf_put_utf8(Buf *b, uint32_t cp);
char *buf_steal(Buf *b); /* returns malloc'd string, buffer reset */

/* ---- arena: bump allocator, zeroed memory, freed all at once ---- */
typedef struct ArenaChunk ArenaChunk;
typedef struct {
    ArenaChunk *head;
} Arena;

void *arena_alloc(Arena *a, size_t n);
char *arena_strdup(Arena *a, const char *s);
char *arena_strndup(Arena *a, const char *s, size_t n);
void  arena_free(Arena *a);

/* ---- UTF-8 ---- */
/* Decodes one code point. Returns bytes consumed (>=1). Invalid input yields U+FFFD and consumes 1 byte. */
int utf8_decode(const char *s, const char *end, uint32_t *cp);
int utf8_encode(uint32_t cp, char out[4]);

/* ---- strings ---- */
bool  str_ieq(const char *a, const char *b);
bool  str_ieqn(const char *a, const char *b, size_t n);
bool  str_starts_with(const char *s, const char *prefix);
bool  str_istarts_with(const char *s, const char *prefix);
char *str_trim(char *s); /* in place; returns pointer into s */
void  str_lower(char *s);
static inline bool is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static inline bool is_digit(int c) { return c >= '0' && c <= '9'; }
static inline bool is_alpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static inline int  to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static inline int  hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
    return -1;
}

/* ---- logging ---- */
extern int bh_verbose;
void bh_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bh_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
