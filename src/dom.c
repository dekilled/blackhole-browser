#include "dom.h"

#include <string.h>

Node *dom_new_document(Arena *a) {
    Node *n = arena_alloc(a, sizeof *n);
    n->type = NODE_DOCUMENT;
    return n;
}

Node *dom_new_element(Arena *a, const char *tag) {
    Node *n = arena_alloc(a, sizeof *n);
    n->type = NODE_ELEMENT;
    n->tag = arena_strdup(a, tag);
    return n;
}

Node *dom_new_text(Arena *a, const char *text, size_t len) {
    Node *n = arena_alloc(a, sizeof *n);
    n->type = NODE_TEXT;
    n->text = arena_strndup(a, text, len);
    return n;
}

Node *dom_new_comment(Arena *a, const char *text, size_t len) {
    Node *n = dom_new_text(a, text, len);
    n->type = NODE_COMMENT;
    return n;
}

void dom_append(Node *parent, Node *child) {
    child->parent = parent;
    child->prev = parent->last_child;
    child->next = NULL;
    if (parent->last_child) parent->last_child->next = child;
    else parent->first_child = child;
    parent->last_child = child;
}

const char *dom_attr(const Node *n, const char *name) {
    for (const Attr *a = n->attrs; a; a = a->next)
        if (!strcmp(a->name, name)) return a->value;
    return NULL;
}

void dom_set_attr(Arena *a, Node *n, const char *name, const char *value) {
    for (Attr *at = n->attrs; at; at = at->next)
        if (!strcmp(at->name, name)) {
            at->value = arena_strdup(a, value);
            return;
        }
    Attr *at = arena_alloc(a, sizeof *at);
    at->name = arena_strdup(a, name);
    at->value = arena_strdup(a, value);
    /* append to keep source order */
    Attr **pp = &n->attrs;
    while (*pp) pp = &(*pp)->next;
    *pp = at;
}

bool dom_is(const Node *n, const char *tag) { return n && n->type == NODE_ELEMENT && !strcmp(n->tag, tag); }

Node *dom_find(Node *root, const char *tag) {
    /* iterative pre-order walk: no recursion on untrusted depth */
    Node *n = root->first_child;
    while (n) {
        if (dom_is(n, tag)) return n;
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) return NULL;
        n = n->next;
    }
    return NULL;
}

char *dom_text_content(const Node *root) {
    Buf b;
    buf_init(&b);
    const Node *n = root->first_child;
    while (n) {
        if (n->type == NODE_TEXT) buf_puts(&b, n->text);
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) break;
        n = n->next;
    }
    return buf_steal(&b);
}
