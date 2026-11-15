/* URL parsing and resolution (subset of the WHATWG URL standard). */
#ifndef BH_URL_H
#define BH_URL_H

#include <stdbool.h>

typedef struct {
    char *scheme;   /* lowercase, no ':' */
    char *host;     /* lowercase, no brackets for IPv6; NULL for file/data */
    int   port;     /* explicit or scheme default; 0 if none */
    char *path;     /* starts with '/', percent-encoded; for data: the raw remainder */
    char *query;    /* without '?', may be NULL */
    char *fragment; /* without '#', may be NULL */
} Url;

bool  url_parse(const char *s, Url *out); /* false on invalid */
void  url_free(Url *u);
char *url_to_string(const Url *u);               /* malloc'd, includes fragment */
char *url_to_string_nofrag(const Url *u);        /* malloc'd */
char *url_resolve(const char *base, const char *rel); /* malloc'd, NULL if invalid */
char *url_origin(const Url *u);                  /* "scheme://host[:port]" malloc'd */
bool  url_same_origin(const Url *a, const Url *b);
/* Request-target for HTTP: path[?query], percent-encoded. malloc'd. */
char *url_request_target(const Url *u);
/* Percent-decodes in place. */
void  url_percent_decode(char *s);

#endif
