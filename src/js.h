/* JavaScript engine, phase 1: lexer + parser producing an AST. See docs/JS_ENGINE.md. */
#ifndef BH_JS_H
#define BH_JS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef enum {
    /* ---- expressions ---- */
    J_NUM,       /* num; flags F_BIGINT for 123n */
    J_STR,       /* str/slen: cooked UTF-8 value */
    J_TEMPLATE,  /* list: [str, expr, str, expr, ..., str] (always odd length) */
    J_TAGGED,    /* a = tag expression, b = J_TEMPLATE */
    J_REGEX,     /* str = pattern, str2 = flags */
    J_IDENT,     /* str */
    J_THIS, J_NULL, J_TRUE, J_FALSE, J_SUPER, J_NEWTARGET,
    J_ARRAY,     /* list elements; J_HOLE for elisions, J_SPREAD for ...x */
    J_OBJECT,    /* list of J_PROP / J_SPREAD */
    J_PROP,      /* a = key (J_IDENT/J_STR/J_NUM node, or any expr when F_COMPUTED), b = value; flags below */
    J_FUNC,      /* str = name or NULL, list = params (patterns), a = body (J_BLOCK, or expression with F_EXPR_BODY) */
    J_CLASS,     /* a = name (J_IDENT) or NULL, b = superclass or NULL, list = members (J_PROP) */
    J_UNARY,     /* str = operator, a = operand */
    J_UPDATE,    /* str = "++"/"--", a = target, F_PREFIX when prefix */
    J_BINARY,    /* str = operator, a, b */
    J_LOGICAL,   /* str = "&&" "||" "??" */
    J_ASSIGN,    /* str = "=" "+=" ..., a = target (pattern for "="), b = value */
    J_COND,      /* a ? b : c */
    J_CALL,      /* a = callee, list = args (J_SPREAD allowed); F_OPTIONAL for ?.() */
    J_NEW,       /* a = callee, list = args */
    J_MEMBER,    /* a = object, b = J_IDENT property (or J_PRIVATE name); F_OPTIONAL for ?. */
    J_INDEX,     /* a = object, b = expression; F_OPTIONAL for ?.[] */
    J_OPTCHAIN,  /* a = whole chain containing ?. (short-circuits to undefined) */
    J_SEQ,       /* list */
    J_SPREAD,    /* a */
    J_AWAIT, J_YIELD, /* a = operand or NULL; F_DELEGATE for yield* */
    J_PRIVATE,   /* str = #name */
    J_HOLE,
    /* ---- patterns ---- */
    J_ARRAY_PAT, /* list of patterns / J_HOLE / J_REST */
    J_OBJECT_PAT,/* list of J_PROP (b = pattern) / J_REST */
    J_ASSIGN_PAT,/* a = pattern, b = default value */
    J_REST,      /* a = pattern */
    /* ---- statements ---- */
    J_PROGRAM,   /* list */
    J_VAR,       /* str = "var"/"let"/"const", list of J_DECL */
    J_DECL,      /* a = pattern, b = init or NULL */
    J_EXPR,      /* a */
    J_BLOCK,     /* list */
    J_IF,        /* a = test, b = then, c = else or NULL */
    J_FOR,       /* a = init or NULL, b = test or NULL, c = update or NULL, d = body */
    J_FORIN,     /* a = left (J_VAR with one J_DECL, or pattern/expr), b = right, d = body */
    J_FOROF,     /* same; F_AWAIT for `for await` */
    J_WHILE,     /* a = test, d = body */
    J_DOWHILE,   /* a = test, d = body */
    J_RETURN, J_THROW, /* a or NULL */
    J_BREAK, J_CONTINUE, /* str = label or NULL */
    J_TRY,       /* a = block, b = catch param (pattern) or NULL, c = catch block or NULL, d = finally block or NULL */
    J_SWITCH,    /* a = discriminant, list = J_CASE */
    J_CASE,      /* a = test (NULL for default), list = statements */
    J_LABEL,     /* str = label, a = statement */
    J_EMPTY, J_DEBUGGER,
    J__COUNT
} JsNodeType;

enum {
    F_OPTIONAL = 1 << 0,  /* ?. */
    F_COMPUTED = 1 << 1,  /* [key] */
    F_PREFIX = 1 << 2,    /* ++x */
    F_ASYNC = 1 << 3,
    F_GENERATOR = 1 << 4,
    F_ARROW = 1 << 5,
    F_EXPR_BODY = 1 << 6, /* arrow with an expression body */
    F_STATIC = 1 << 7,
    F_GETTER = 1 << 8,
    F_SETTER = 1 << 9,
    F_METHOD = 1 << 10,
    F_FIELD = 1 << 11,    /* class field */
    F_SHORTHAND = 1 << 12,/* { a } */
    F_AWAIT = 1 << 13,    /* for await */
    F_DELEGATE = 1 << 14, /* yield* */
    F_BIGINT = 1 << 15,
    F_PAREN = 1 << 16,    /* was parenthesised */
    F_STATIC_BLOCK = 1 << 17,
    F_FUNC_DECL = 1 << 18 /* function/class declaration statement (vs expression) */
};

typedef struct JsNode {
    uint8_t         type;
    uint32_t        flags;
    int             line, col;
    double          num;
    const char     *str, *str2;
    size_t          slen;
    struct JsNode  *a, *b, *c, *d;
    struct JsNode **list;
    int             n;
} JsNode;

typedef struct {
    JsNode *program; /* NULL on error */
    char    error[256];
    int     line, col;
    int     nodes;
} JsParseResult;

/* Parses `src` (UTF-8). All nodes live in `arena`. */
void js_parse(Arena *arena, const char *src, size_t len, JsParseResult *out);
/* S-expression dump of a node tree (for tests). */
void js_dump(const JsNode *n, Buf *out);

#endif
