/* Networking: HTTP/1.1 over TCP and TLS (OpenSSL), data: and file: URLs. */
#ifndef BH_NET_H
#define BH_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      status;       /* HTTP status; 200 for data:/file: */
    char    *url;          /* final URL after redirects */
    char    *content_type; /* lowercase MIME without parameters; may be NULL */
    char    *charset;      /* lowercase; may be NULL */
    uint8_t *body;         /* always NUL terminated for convenience */
    size_t   body_len;
    char    *error;        /* non-NULL when the fetch failed */
    bool     secure;       /* delivered over verified TLS */
} Response;

typedef struct {
    size_t max_body;      /* bytes, after decompression */
    int    max_redirects;
    int    timeout_ms;    /* per socket operation */
    int    total_timeout_ms;
    bool   allow_file;    /* only user-initiated top-level navigations may read file: */
    bool   allow_downgrade; /* never set by default: https -> http redirect */
} FetchOpts;

FetchOpts net_default_opts(void);
/* Returns true if an HTTP exchange completed (any status). On failure r->error is set. */
bool net_fetch(const char *url, const FetchOpts *opts, Response *r);
void response_free(Response *r);

#endif
