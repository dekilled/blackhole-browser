/* HTML parser: forgiving tokenizer + tree builder (practical subset of the WHATWG algorithm). */
#ifndef BH_HTML_H
#define BH_HTML_H

#include "dom.h"

/* Parses UTF-8 `src`. Nodes live in arena `a`. Never fails: any input produces a document. */
Node *html_parse(Arena *a, const char *src, size_t len);

/* Decodes character references in s[0..n) and appends to out. Used by the HTML and CSS code. */
void html_decode_entities(Buf *out, const char *s, size_t n, bool in_attr);

#endif
