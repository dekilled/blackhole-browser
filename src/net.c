#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "url.h"
#include "util.h"

#define MAX_HEADER_BYTES (128 * 1024)
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) BlackHole/0.1"

FetchOpts net_default_opts(void) {
    FetchOpts o;
    memset(&o, 0, sizeof o);
    o.max_body = 64u * 1024 * 1024;
    o.max_redirects = 10;
    o.timeout_ms = 15000;
    o.total_timeout_ms = 60000;
    return o;
}

void response_free(Response *r) {
    free(r->url);
    free(r->content_type);
    free(r->charset);
    free(r->body);
    free(r->error);
    memset(r, 0, sizeof *r);
}

static void set_err(Response *r, const char *fmt, ...) {
    free(r->error);
    char    tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    r->error = xstrdup(tmp);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ TLS ---- */
static SSL_CTX *g_ctx;

static SSL_CTX *tls_ctx(void) {
    if (g_ctx) return g_ctx;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION); /* no SSLv3 / TLS 1.0 / 1.1 */
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);     /* CRIME */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);      /* certificate verification is NOT optional */
    SSL_CTX_set_default_verify_paths(ctx);               /* honors SSL_CERT_FILE / SSL_CERT_DIR */
    static const unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    SSL_CTX_set_alpn_protos(ctx, alpn, sizeof alpn);
    g_ctx = ctx;
    return ctx;
}

typedef struct {
    int  fd;
    SSL *ssl;
} Conn;

static void conn_close(Conn *c) {
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int tcp_connect(const char *host, int port, int timeout_ms, Response *r) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        set_err(r, "DNS lookup failed for %s: %s", host, gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = {.fd = fd, .events = POLLOUT};
            rc = poll(&p, 1, timeout_ms);
            if (rc == 1) {
                int       soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                rc = soerr == 0 ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl); /* back to blocking; timeouts via SO_RCVTIMEO/SO_SNDTIMEO */
            struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) set_err(r, "could not connect to %s:%d", host, port);
    return fd;
}

static bool conn_open(Conn *c, const char *host, int port, bool tls, int timeout_ms, Response *r) {
    c->ssl = NULL;
    c->fd = tcp_connect(host, port, timeout_ms, r);
    if (c->fd < 0) return false;
    if (!tls) return true;

    SSL_CTX *ctx = tls_ctx();
    if (!ctx) {
        set_err(r, "TLS initialisation failed");
        conn_close(c);
        return false;
    }
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, c->fd);
    unsigned char ipbuf[16];
    bool          is_ip = inet_pton(AF_INET, host, ipbuf) == 1 || inet_pton(AF_INET6, host, ipbuf) == 1;
    if (is_ip) {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host);
    } else {
        SSL_set_tlsext_host_name(ssl, host); /* SNI */
        X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), host, 0); /* hostname must match the certificate */
    }
    int rc = SSL_connect(ssl);
    if (rc != 1) {
        long vr = SSL_get_verify_result(ssl);
        if (vr != X509_V_OK)
            set_err(r, "TLS certificate rejected for %s: %s", host, X509_verify_cert_error_string(vr));
        else {
            unsigned long e = ERR_peek_last_error();
            char          eb[160] = "handshake failed";
            if (e) ERR_error_string_n(e, eb, sizeof eb);
            set_err(r, "TLS handshake with %s failed: %s", host, eb);
        }
        ERR_clear_error();
        SSL_free(ssl);
        close(c->fd);
        c->fd = -1;
        return false;
    }
    c->ssl = ssl;
    return true;
}

static ssize_t conn_read(Conn *c, void *buf, size_t n) {
    if (c->ssl) {
        int k = SSL_read(c->ssl, buf, (int)(n > 1 << 20 ? 1 << 20 : n));
        if (k > 0) return k;
        int e = SSL_get_error(c->ssl, k);
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        if (e == SSL_ERROR_SYSCALL && k == 0) return 0; /* peer closed without close_notify */
        ERR_clear_error();
        errno = (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE || errno == EAGAIN) ? EAGAIN : EIO;
        return -1;
    }
    ssize_t k;
    do k = read(c->fd, buf, n);
    while (k < 0 && errno == EINTR);
    return k;
}

static bool conn_write_all(Conn *c, const char *p, size_t n) {
    while (n) {
        ssize_t k;
        if (c->ssl) {
            int w = SSL_write(c->ssl, p, (int)n);
            k = w > 0 ? w : -1;
            if (w <= 0) ERR_clear_error();
        } else {
            do k = write(c->fd, p, n);
            while (k < 0 && errno == EINTR);
        }
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* ------------------------------------------------------- buffered reader --- */
typedef struct {
    Conn     *c;
    uint8_t   buf[16384];
    size_t    pos, len;
    long long deadline;
    bool      timed_out;
} Reader;

static bool rd_fill(Reader *r) {
    if (r->pos < r->len) return true;
    if (now_ms() > r->deadline) {
        r->timed_out = true;
        return false;
    }
    ssize_t k = conn_read(r->c, r->buf, sizeof r->buf);
    if (k <= 0) {
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) r->timed_out = true;
        return false;
    }
    r->pos = 0;
    r->len = (size_t)k;
    return true;
}

/* Reads a line (without CRLF) into out. false on EOF before any '\n'. */
static bool rd_line(Reader *r, Buf *out, size_t max) {
    buf_clear(out);
    for (;;) {
        if (!rd_fill(r)) return false;
        uint8_t ch = r->buf[r->pos++];
        if (ch == '\n') {
            if (out->len && out->data[out->len - 1] == '\r') out->data[--out->len] = 0;
            return true;
        }
        if (out->len >= max) return false;
        buf_putc(out, (char)ch);
    }
}

/* Reads up to n bytes into out; returns the count actually read (less on EOF). */
static size_t rd_bytes(Reader *r, Buf *out, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (!rd_fill(r)) break;
        size_t take = r->len - r->pos;
        if (take > n - got) take = n - got;
        buf_append(out, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return got;
}

/* ---------------------------------------------------------------- headers -- */
typedef struct {
    char **names;
    char **values;
    int    n;
} Headers;

static void headers_free(Headers *h) {
    for (int i = 0; i < h->n; i++) {
        free(h->names[i]);
        free(h->values[i]);
    }
    free(h->names);
    free(h->values);
    memset(h, 0, sizeof *h);
}

static void headers_add(Headers *h, const char *name, size_t nl, const char *val) {
    if (h->n >= 256) return; /* header count cap */
    h->names = xrealloc(h->names, sizeof(char *) * (size_t)(h->n + 1));
    h->values = xrealloc(h->values, sizeof(char *) * (size_t)(h->n + 1));
    h->names[h->n] = xstrndup(name, nl);
    str_lower(h->names[h->n]);
    h->values[h->n] = xstrdup(val);
    h->n++;
}

static const char *headers_get(const Headers *h, const char *name) {
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->names[i], name)) return h->values[i];
    return NULL;
}

/* ------------------------------------------------------------- decompression */
static bool inflate_body(const Buf *in, Buf *out, size_t cap, bool raw) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, raw ? -15 : 15 + 32) != Z_OK) return false;
    zs.next_in = (Bytef *)in->data;
    zs.avail_in = (uInt)in->len;
    uint8_t chunk[16384];
    int     rc;
    do {
        zs.next_out = chunk;
        zs.avail_out = sizeof chunk;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        size_t produced = sizeof chunk - zs.avail_out;
        if (out->len + produced > cap) { /* decompression bomb */
            inflateEnd(&zs);
            return false;
        }
        buf_append(out, chunk, produced);
    } while (rc != Z_STREAM_END && (zs.avail_in > 0 || zs.avail_out == 0));
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

/* ------------------------------------------------------------------- misc -- */
static void parse_content_type(const char *ct, Response *r) {
    free(r->content_type);
    free(r->charset);
    r->content_type = r->charset = NULL;
    if (!ct) return;
    char *copy = xstrdup(ct);
    char *semi = strchr(copy, ';');
    if (semi) {
        *semi = 0;
        char *params = semi + 1;
        char *cs = strcasestr(params, "charset=");
        if (cs) {
            cs += 8;
            if (*cs == '"' || *cs == '\'') cs++;
            size_t n = strcspn(cs, ";\"' \t");
            r->charset = xstrndup(cs, n);
            str_lower(r->charset);
        }
    }
    r->content_type = xstrdup(str_trim(copy));
    str_lower(r->content_type);
    free(copy);
}

static const char *mime_from_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const struct { const char *ext, *mime; } map[] = {
        {".html", "text/html"}, {".htm", "text/html"}, {".css", "text/css"},   {".txt", "text/plain"},
        {".png", "image/png"},  {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"},
        {".svg", "image/svg+xml"}, {".js", "text/javascript"}, {".json", "application/json"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (str_ieq(dot, map[i].ext)) return map[i].mime;
    return "application/octet-stream";
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static void base64_decode(const char *s, Buf *out) {
    uint32_t acc = 0;
    int      bits = 0;
    for (; *s; s++) {
        int v = b64_val((unsigned char)*s);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf_putc(out, (char)((acc >> bits) & 0xFF));
        }
    }
}

static bool fetch_data_url(const Url *u, Response *r) {
    const char *p = u->path ? u->path : "";
    const char *comma = strchr(p, ',');
    if (!comma) {
        set_err(r, "malformed data: URL");
        return false;
    }
    char *meta = xstrndup(p, (size_t)(comma - p));
    bool  b64 = false;
    char *semi = strrchr(meta, ';');
    if (semi && str_ieq(semi + 1, "base64")) {
        b64 = true;
        *semi = 0;
    }
    parse_content_type(*meta ? meta : "text/plain;charset=us-ascii", r);
    free(meta);
    Buf body;
    buf_init(&body);
    if (b64) {
        char *dec = xstrdup(comma + 1);
        url_percent_decode(dec);
        base64_decode(dec, &body);
        free(dec);
    } else {
        char *dec = xstrdup(comma + 1);
        url_percent_decode(dec);
        buf_puts(&body, dec);
        free(dec);
    }
    r->body_len = body.len;
    r->body = (uint8_t *)buf_steal(&body);
    r->status = 200;
    r->url = url_to_string(u);
    return true;
}

static bool fetch_file_url(const Url *u, const FetchOpts *o, Response *r) {
    if (!o->allow_file) {
        set_err(r, "file: URLs are not allowed from this context");
        return false;
    }
    char *path = xstrdup(u->path ? u->path : "/");
    url_percent_decode(path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(r, "cannot open %s: %s", path, strerror(errno));
        free(path);
        return false;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        set_err(r, "%s is not a regular file", path);
        fclose(f);
        free(path);
        return false;
    }
    if ((size_t)st.st_size > o->max_body) {
        set_err(r, "file too large");
        fclose(f);
        free(path);
        return false;
    }
    Buf b;
    buf_init(&b);
    char chunk[16384];
    size_t k;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(&b, chunk, k);
    fclose(f);
    r->content_type = xstrdup(mime_from_ext(path));
    r->body_len = b.len;
    r->body = (uint8_t *)buf_steal(&b);
    r->status = 200;
    r->url = url_to_string(u);
    free(path);
    return true;
}

/* -------------------------------------------------------------- HTTP fetch - */
/* One request/response exchange, no redirect handling. On redirect, sets *location. */
static bool http_exchange(const Url *u, const FetchOpts *o, Response *r, char **location) {
    bool  tls = !strcmp(u->scheme, "https");
    Conn  c = {.fd = -1, .ssl = NULL};
    if (!conn_open(&c, u->host, u->port, tls, o->timeout_ms, r)) return false;

    char *target = url_request_target(u);
    Buf   req;
    buf_init(&req);
    bool v6 = strchr(u->host, ':') != NULL;
    buf_printf(&req, "GET %s HTTP/1.1\r\nHost: ", target);
    if (v6) buf_putc(&req, '[');
    buf_puts(&req, u->host);
    if (v6) buf_putc(&req, ']');
    if (u->port != (tls ? 443 : 80)) buf_printf(&req, ":%d", u->port);
    buf_puts(&req, "\r\nUser-Agent: " USER_AGENT "\r\n"
                   "Accept: text/html,application/xhtml+xml,text/css,image/png,*/*;q=0.8\r\n"
                   "Accept-Language: pt-BR,pt;q=0.9,en;q=0.8\r\n"
                   "Accept-Encoding: gzip, deflate\r\n"
                   "Connection: close\r\n\r\n");
    free(target);
    bool ok = conn_write_all(&c, req.data, req.len);
    buf_free(&req);
    if (!ok) {
        set_err(r, "failed to send request");
        conn_close(&c);
        return false;
    }

    Reader *rd = xcalloc(1, sizeof *rd);
    rd->c = &c;
    rd->deadline = now_ms() + o->total_timeout_ms;
    Buf   line, raw;
    Headers h = {0};
    buf_init(&line);
    buf_init(&raw);
    bool result = false;
    size_t header_bytes = 0;

    /* status line */
    if (!rd_line(rd, &line, 8192)) {
        set_err(r, rd->timed_out ? "timed out waiting for response" : "connection closed before response");
        goto out;
    }
    if (strncmp(line.data, "HTTP/1.", 7) != 0) {
        set_err(r, "not an HTTP/1.x response");
        goto out;
    }
    r->status = atoi(line.data + 9);
    if (r->status < 100 || r->status > 599) {
        set_err(r, "bad status line");
        goto out;
    }
    /* header fields */
    for (;;) {
        if (!rd_line(rd, &line, 16384)) {
            set_err(r, "truncated or oversized headers");
            goto out;
        }
        if (line.len == 0) break;
        header_bytes += line.len;
        if (header_bytes > MAX_HEADER_BYTES) {
            set_err(r, "headers too large");
            goto out;
        }
        char *colon = strchr(line.data, ':');
        if (!colon) continue;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        headers_add(&h, line.data, (size_t)(colon - line.data), v);
    }

    /* redirect? */
    if (r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308) {
        const char *loc = headers_get(&h, "location");
        if (loc) {
            *location = xstrdup(loc);
            result = true;
            goto out;
        }
    }

    /* body */
    bool        no_body = (r->status >= 100 && r->status < 200) || r->status == 204 || r->status == 304;
    const char *te = headers_get(&h, "transfer-encoding");
    const char *cl = headers_get(&h, "content-length");
    const char *ce = headers_get(&h, "content-encoding");
    if (!no_body) {
        if (te && strcasestr(te, "chunked")) {
            for (;;) {
                if (!rd_line(rd, &line, 1024)) {
                    set_err(r, "truncated chunked body");
                    goto out;
                }
                char *endp;
                errno = 0;
                unsigned long sz = strtoul(line.data, &endp, 16);
                if (endp == line.data || errno) {
                    set_err(r, "bad chunk size");
                    goto out;
                }
                if (sz == 0) break;
                if (raw.len + sz > o->max_body) {
                    set_err(r, "response too large");
                    goto out;
                }
                if (rd_bytes(rd, &raw, sz) != sz) {
                    set_err(r, "truncated chunk");
                    goto out;
                }
                if (!rd_line(rd, &line, 16)) { /* CRLF after chunk */
                    set_err(r, "bad chunk terminator");
                    goto out;
                }
            }
            /* trailers ignored */
        } else if (cl) {
            char *endp;
            errno = 0;
            unsigned long long n = strtoull(cl, &endp, 10);
            if (endp == cl || errno || n > o->max_body) {
                set_err(r, "bad or too large Content-Length");
                goto out;
            }
            if (rd_bytes(rd, &raw, (size_t)n) != n) {
                set_err(r, rd->timed_out ? "timed out reading body" : "body truncated");
                goto out;
            }
        } else {
            while (rd_fill(rd)) {
                size_t take = rd->len - rd->pos;
                if (raw.len + take > o->max_body) {
                    set_err(r, "response too large");
                    goto out;
                }
                buf_append(&raw, rd->buf + rd->pos, take);
                rd->pos = rd->len;
            }
        }
    }

    /* content decoding */
    if (ce && (strcasestr(ce, "gzip") || strcasestr(ce, "deflate")) && raw.len) {
        Buf dec;
        buf_init(&dec);
        bool good = inflate_body(&raw, &dec, o->max_body, false);
        if (!good) {
            buf_clear(&dec);
            good = inflate_body(&raw, &dec, o->max_body, true);
        }
        if (!good) {
            buf_free(&dec);
            set_err(r, "could not decode compressed body");
            goto out;
        }
        buf_free(&raw);
        raw = dec;
    } else if (ce && !str_ieq(ce, "identity") && *ce) {
        set_err(r, "unsupported Content-Encoding: %s", ce);
        goto out;
    }
    parse_content_type(headers_get(&h, "content-type"), r);
    r->body_len = raw.len;
    r->body = (uint8_t *)buf_steal(&raw);
    r->secure = tls;
    result = true;

out:
    buf_free(&line);
    buf_free(&raw);
    headers_free(&h);
    free(rd);
    conn_close(&c);
    return result;
}

bool net_fetch(const char *url, const FetchOpts *opts_in, Response *r) {
    FetchOpts o = opts_in ? *opts_in : net_default_opts();
    memset(r, 0, sizeof *r);
    char *cur = xstrdup(url);
    bool  was_https = false;

    for (int hop = 0; hop <= o.max_redirects; hop++) {
        Url u;
        if (!url_parse(cur, &u)) {
            set_err(r, "invalid URL: %s", cur);
            free(cur);
            return false;
        }
        bool is_https = !strcmp(u.scheme, "https");
        bool is_http = !strcmp(u.scheme, "http");
        if (hop > 0 && was_https && is_http && !o.allow_downgrade) {
            set_err(r, "refusing redirect from https to http (downgrade)");
            url_free(&u);
            free(cur);
            return false;
        }
        if (hop > 0 && !is_http && !is_https) {
            set_err(r, "refusing redirect to %s: URL", u.scheme);
            url_free(&u);
            free(cur);
            return false;
        }
        if (!strcmp(u.scheme, "data")) {
            bool ok = fetch_data_url(&u, r);
            url_free(&u);
            free(cur);
            return ok;
        }
        if (!strcmp(u.scheme, "file")) {
            bool ok = fetch_file_url(&u, &o, r);
            url_free(&u);
            free(cur);
            return ok;
        }
        if (!is_http && !is_https) {
            set_err(r, "unsupported URL scheme: %s", u.scheme);
            url_free(&u);
            free(cur);
            return false;
        }
        char *location = NULL;
        bool  ok = http_exchange(&u, &o, r, &location);
        if (!ok) {
            url_free(&u);
            free(cur);
            return false;
        }
        if (location) {
            char *next = url_resolve(cur, location);
            free(location);
            url_free(&u);
            free(cur);
            if (!next) {
                set_err(r, "bad redirect target");
                return false;
            }
            cur = next;
            was_https = is_https;
            r->status = 0;
            continue;
        }
        r->url = cur;
        url_free(&u);
        return true;
    }
    set_err(r, "too many redirects");
    free(cur);
    return false;
}
