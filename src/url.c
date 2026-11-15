#include "url.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static int default_port(const char *scheme) {
    if (!strcmp(scheme, "http")) return 80;
    if (!strcmp(scheme, "https")) return 443;
    return 0;
}

/* Percent-encode bytes that must not appear raw in a path/query. Keeps existing %XX intact. */
static char *encode_component(const char *s, size_t n, bool is_query) {
    Buf b;
    buf_init(&b);
    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool keep = (c > 0x20 && c < 0x7F && c != '"' && c != '<' && c != '>' && c != '`' && c != '{' && c != '}' &&
                     c != '\\' && c != '^' && c != '|');
        if (is_query && c == '\'') keep = false;
        if (keep) {
            buf_putc(&b, (char)c);
        } else {
            buf_putc(&b, '%');
            buf_putc(&b, hex[c >> 4]);
            buf_putc(&b, hex[c & 15]);
        }
    }
    return buf_steal(&b);
}

/* Remove "." and ".." segments. Input must start with '/'. In place. */
static void remove_dot_segments(char *path) {
    size_t n = strlen(path);
    char  *out = xmalloc(n + 2);
    size_t o = 0;
    size_t i = 0;
    while (i < n) {
        /* path[i] == '/' */
        size_t j = i + 1;
        while (j < n && path[j] != '/') j++;
        size_t seglen = j - (i + 1);
        const char *seg = path + i + 1;
        if (seglen == 1 && seg[0] == '.') {
            if (j >= n) out[o++] = '/';
        } else if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--; /* drop the '/' before removed segment */
            if (j >= n) out[o++] = '/';
        } else {
            out[o++] = '/';
            memcpy(out + o, seg, seglen);
            o += seglen;
        }
        i = j;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    strcpy(path, out);
    free(out);
}

bool url_parse(const char *input, Url *u) {
    memset(u, 0, sizeof *u);
    char *s = xstrdup(input);
    char *t = str_trim(s);
    /* strip tab/newline anywhere, as per spec */
    char *w = t;
    for (char *r = t; *r; r++)
        if (*r != '\t' && *r != '\n' && *r != '\r') *w++ = *r;
    *w = 0;

    /* scheme */
    char *p = t;
    if (!is_alpha((unsigned char)*p)) goto fail;
    while (is_alpha((unsigned char)*p) || is_digit((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
    if (*p != ':') goto fail;
    u->scheme = xstrndup(t, (size_t)(p - t));
    str_lower(u->scheme);
    p++;

    /* fragment */
    char *hash = strchr(p, '#');
    if (hash) {
        u->fragment = xstrdup(hash + 1);
        *hash = 0;
    }

    if (!strcmp(u->scheme, "data") || !strcmp(u->scheme, "javascript") || !strcmp(u->scheme, "about") ||
        !strcmp(u->scheme, "mailto")) {
        u->path = xstrdup(p);
        free(s);
        return true;
    }

    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        char *auth_end = p + strcspn(p, "/?");
        char  saved = *auth_end;
        *auth_end = 0;
        char *auth = p;
        char *at = strrchr(auth, '@'); /* userinfo is discarded: credentials in URLs are an attack vector */
        if (at) auth = at + 1;
        char *host = auth;
        char *portstr = NULL;
        if (*auth == '[') {
            char *close = strchr(auth, ']');
            if (!close) { *auth_end = saved; goto fail; }
            *close = 0;
            host = auth + 1;
            if (close[1] == ':') portstr = close + 2;
            else if (close[1]) { *auth_end = saved; goto fail; }
        } else {
            char *colon = strrchr(auth, ':');
            if (colon) {
                *colon = 0;
                portstr = colon + 1;
            }
        }
        if (portstr && *portstr) {
            char *endp;
            long  pv = strtol(portstr, &endp, 10);
            if (*endp || pv < 0 || pv > 65535) { *auth_end = saved; goto fail; }
            u->port = (int)pv;
        } else {
            u->port = default_port(u->scheme);
        }
        if (strcmp(u->scheme, "file") != 0 && !*host) { *auth_end = saved; goto fail; }
        for (const char *c = host; *c; c++)
            if ((unsigned char)*c <= 0x20 || *c == '%' || *c == '<' || *c == '>' || *c == '\\') {
                *auth_end = saved;
                goto fail;
            }
        u->host = xstrdup(host);
        str_lower(u->host);
        *auth_end = saved;
        p = auth_end;
    } else if (!strcmp(u->scheme, "http") || !strcmp(u->scheme, "https")) {
        goto fail; /* http:foo is not supported */
    }

    /* query */
    char *q = strchr(p, '?');
    if (q) {
        u->query = encode_component(q + 1, strlen(q + 1), true);
        *q = 0;
    }
    /* path */
    char *rawpath = *p ? p : (char *)"/";
    if (*rawpath != '/') {
        /* non-hierarchical remainder; keep it as is */
        u->path = encode_component(rawpath, strlen(rawpath), false);
    } else {
        u->path = encode_component(rawpath, strlen(rawpath), false);
        remove_dot_segments(u->path);
    }
    free(s);
    return true;

fail:
    free(s);
    url_free(u);
    return false;
}

void url_free(Url *u) {
    free(u->scheme);
    free(u->host);
    free(u->path);
    free(u->query);
    free(u->fragment);
    memset(u, 0, sizeof *u);
}

static char *to_string(const Url *u, bool frag) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, u->scheme);
    buf_putc(&b, ':');
    if (u->host || !strcmp(u->scheme, "file")) {
        buf_puts(&b, "//");
        if (u->host) {
            bool v6 = strchr(u->host, ':') != NULL;
            if (v6) buf_putc(&b, '[');
            buf_puts(&b, u->host);
            if (v6) buf_putc(&b, ']');
            if (u->port && u->port != default_port(u->scheme)) buf_printf(&b, ":%d", u->port);
        }
    }
    if (u->path) buf_puts(&b, u->path);
    if (u->query) {
        buf_putc(&b, '?');
        buf_puts(&b, u->query);
    }
    if (frag && u->fragment) {
        buf_putc(&b, '#');
        buf_puts(&b, u->fragment);
    }
    return buf_steal(&b);
}

char *url_to_string(const Url *u) { return to_string(u, true); }
char *url_to_string_nofrag(const Url *u) { return to_string(u, false); }

char *url_origin(const Url *u) {
    Buf b;
    buf_init(&b);
    if (!u->host) {
        buf_puts(&b, "null");
        return buf_steal(&b);
    }
    buf_printf(&b, "%s://%s", u->scheme, u->host);
    if (u->port && u->port != default_port(u->scheme)) buf_printf(&b, ":%d", u->port);
    return buf_steal(&b);
}

bool url_same_origin(const Url *a, const Url *b) {
    if (!a->host || !b->host) return false;
    return !strcmp(a->scheme, b->scheme) && !strcmp(a->host, b->host) && a->port == b->port;
}

char *url_request_target(const Url *u) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, u->path && *u->path ? u->path : "/");
    if (u->query) {
        buf_putc(&b, '?');
        buf_puts(&b, u->query);
    }
    return buf_steal(&b);
}

char *url_resolve(const char *base, const char *rel) {
    while (is_space((unsigned char)*rel)) rel++;
    Url r;
    if (url_parse(rel, &r)) {
        char *out = url_to_string(&r);
        url_free(&r);
        return out;
    }
    Url b;
    if (!url_parse(base, &b)) return NULL;
    Buf out;
    buf_init(&out);
    char *result = NULL;

    if (rel[0] == '/' && rel[1] == '/') {
        char *tmp = xmalloc(strlen(b.scheme) + strlen(rel) + 2);
        sprintf(tmp, "%s:%s", b.scheme, rel);
        Url x;
        if (url_parse(tmp, &x)) {
            result = url_to_string(&x);
            url_free(&x);
        }
        free(tmp);
        goto done;
    }
    if (!b.host && strcmp(b.scheme, "file") != 0) goto done; /* cannot resolve against data:, etc. */

    /* split rel into path / query / fragment */
    char *copy = xstrdup(rel);
    char *frag = strchr(copy, '#');
    char *fragv = NULL;
    if (frag) { fragv = frag + 1; *frag = 0; }
    char *q = strchr(copy, '?');
    char *qv = NULL;
    if (q) { qv = q + 1; *q = 0; }

    Url x;
    memset(&x, 0, sizeof x);
    x.scheme = xstrdup(b.scheme);
    x.host = b.host ? xstrdup(b.host) : NULL;
    x.port = b.port;
    if (copy[0] == '/') {
        x.path = encode_component(copy, strlen(copy), false);
        remove_dot_segments(x.path);
        x.query = qv ? encode_component(qv, strlen(qv), true) : NULL;
    } else if (copy[0] == 0) {
        x.path = xstrdup(b.path ? b.path : "/");
        x.query = qv ? encode_component(qv, strlen(qv), true) : (b.query ? xstrdup(b.query) : NULL);
    } else {
        const char *bp = b.path ? b.path : "/";
        const char *slash = strrchr(bp, '/');
        size_t      dirlen = slash ? (size_t)(slash - bp) + 1 : 1;
        Buf         merged;
        buf_init(&merged);
        buf_append(&merged, bp, dirlen);
        char *enc = encode_component(copy, strlen(copy), false);
        buf_puts(&merged, enc);
        free(enc);
        x.path = buf_steal(&merged);
        remove_dot_segments(x.path);
        x.query = qv ? encode_component(qv, strlen(qv), true) : NULL;
    }
    x.fragment = fragv ? xstrdup(fragv) : NULL;
    result = url_to_string(&x);
    url_free(&x);
    free(copy);
done:
    buf_free(&out);
    url_free(&b);
    return result;
}

void url_percent_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '%' && hex_val((unsigned char)r[1]) >= 0 && hex_val((unsigned char)r[2]) >= 0) {
            *w++ = (char)(hex_val((unsigned char)r[1]) * 16 + hex_val((unsigned char)r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}
