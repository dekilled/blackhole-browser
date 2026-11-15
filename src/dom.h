/* Minimal DOM: document/element/text/comment nodes, all arena allocated. */
#ifndef BH_DOM_H
#define BH_DOM_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

typedef enum { NODE_DOCUMENT, NODE_ELEMENT, NODE_TEXT, NODE_COMMENT } NodeType;

typedef struct Attr {
    const char  *name;
    const char  *value;
    struct Attr *next;
} Attr;

typedef struct Style Style;

typedef struct Node {
    NodeType     type;
    const char  *tag;  /* lowercase, elements only */
    const char  *text; /* text and comment nodes */
    Attr        *attrs;
    struct Node *parent, *first_child, *last_child, *prev, *next;
    Style       *style; /* computed style (elements; text nodes point at parent's) */
    uint32_t     flags;
} Node;

#define NODE_HOVER 0x1u
#define NODE_PSEUDO 0x2u        /* generated ::before / ::after box */
#define NODE_PSEUDO_BEFORE 0x4u

Node       *dom_new_document(Arena *a);
Node       *dom_new_element(Arena *a, const char *tag);
Node       *dom_new_text(Arena *a, const char *text, size_t len);
Node       *dom_new_comment(Arena *a, const char *text, size_t len);
void        dom_append(Node *parent, Node *child);
const char *dom_attr(const Node *n, const char *name); /* NULL if absent */
void        dom_set_attr(Arena *a, Node *n, const char *name, const char *value);
bool        dom_is(const Node *n, const char *tag);
Node       *dom_find(Node *root, const char *tag); /* first descendant element with tag */
/* Concatenated text content of a subtree. malloc'd. */
char *dom_text_content(const Node *n);

#endif
