#include "page.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"
#include "diag.h"
#include "img.h"
#include "html.h"
#include "style.h"
#include "url.h"

#define MAX_SHEETS 64
#define MAX_CSS_BYTES (4u * 1024 * 1024)
#define MAX_IMPORT_DEPTH 3

static Image *resolve_image(void *user, const Node *img);
static void   prefetch_images(Page *p);

static void add_sheet(Page *p, Stylesheet *s) {
    if (p->nsheets >= MAX_SHEETS) {
        css_free(s);
        return;
    }
    p->sheets = xrealloc(p->sheets, sizeof(Stylesheet *) * (size_t)(p->nsheets + 1));
    p->sheets[p->nsheets++] = s;
}

static char *fetch_text(Page *p, const char *url, const char *expect_type, size_t max) {
    FetchOpts o = net_default_opts();
    o.max_body = max;
    o.timeout_ms = 10000;
    o.total_timeout_ms = 20000;
    o.allow_file = p->allow_file_subresources;
    Response r;
    char    *out = NULL;
    if (net_fetch(url, &o, &r) && r.status >= 200 && r.status < 300) {
        bool type_ok = !expect_type || !r.content_type || strstr(r.content_type, expect_type) ||
                       !strcmp(r.content_type, "application/octet-stream");
        if (type_ok) out = charset_to_utf8(r.body, r.body_len, r.charset, NULL);
        else bh_log("ignoring %s: unexpected type %s", url, r.content_type);
    } else {
        bh_log("failed to fetch %s: %s", url, r.error ? r.error : "HTTP error");
        diag_note("rede-falhou", url);
    }
    response_free(&r);
    return out;
}

static void load_sheet_text(Page *p, const char *css, size_t len, const char *base_url, int depth) {
    Stylesheet *s = css_parse(css, len, 1);
    if (depth < MAX_IMPORT_DEPTH) {
        for (int i = 0; i < s->nimports; i++) {
            char *abs = url_resolve(base_url, s->imports[i]);
            if (!abs) continue;
            char *text = fetch_text(p, abs, "css", MAX_CSS_BYTES);
            if (text) {
                load_sheet_text(p, text, strlen(text), abs, depth + 1);
                free(text);
            }
            free(abs);
        }
    }
    add_sheet(p, s);
}

static bool rel_has(const char *rel, const char *word) {
    if (!rel) return false;
    size_t wl = strlen(word);
    for (const char *s = rel; *s;) {
        while (is_space((unsigned char)*s)) s++;
        const char *t = s;
        while (*s && !is_space((unsigned char)*s)) s++;
        if ((size_t)(s - t) == wl && !strncasecmp(t, word, wl)) return true;
    }
    return false;
}

static void collect_stylesheets(Page *p, Viewport vp) {
    const char *base = p->url;
    Node *n = p->doc->first_child;
    /* <base href> */
    Node *bn = dom_find(p->doc, "base");
    char *base_owned = NULL;
    if (bn && dom_attr(bn, "href")) {
        base_owned = url_resolve(p->url, dom_attr(bn, "href"));
        if (base_owned) base = base_owned;
    }
    int ext = 0;
    while (n) {
        if (n->type == NODE_ELEMENT) {
            if (!strcmp(n->tag, "style")) {
                const char *media = dom_attr(n, "media");
                if (css_media_attr_matches(media, &vp)) {
                    char *text = dom_text_content(n);
                    load_sheet_text(p, text, strlen(text), base, 0);
                    free(text);
                }
            } else if (!strcmp(n->tag, "link") && rel_has(dom_attr(n, "rel"), "stylesheet") && !rel_has(dom_attr(n, "rel"), "alternate")) {
                const char *href = dom_attr(n, "href");
                const char *media = dom_attr(n, "media");
                if (href && ext < 24 && css_media_attr_matches(media, &vp)) {
                    char *abs = url_resolve(base, href);
                    if (abs) {
                        ext++;
                        char *text = fetch_text(p, abs, "css", MAX_CSS_BYTES);
                        if (text) {
                            load_sheet_text(p, text, strlen(text), abs, 0);
                            free(text);
                        }
                        free(abs);
                    }
                }
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && n != p->doc && !n->next) n = n->parent;
        if (!n || n == p->doc) break;
        n = n->next;
    }
    free(p->base);
    p->base = xstrdup(base);
    free(base_owned);
}

static void finish_page(Page *p, const char *utf8, size_t len, Viewport vp) {
    p->doc = html_parse(&p->dom_arena, utf8, len);
    Node *t = dom_find(p->doc, "title");
    if (t) {
        char *txt = dom_text_content(t);
        /* collapse whitespace */
        Buf b;
        buf_init(&b);
        bool sp = false;
        for (char *c = txt; *c; c++) {
            if (is_space((unsigned char)*c)) sp = true;
            else {
                if (sp && b.len) buf_putc(&b, ' ');
                sp = false;
                buf_putc(&b, *c);
            }
        }
        p->title = buf_steal(&b);
        free(txt);
    }
    p->nsheets = 0;
    add_sheet(p, style_ua_sheet()); /* index 0, never freed by page_free */
    collect_stylesheets(p, vp);
    prefetch_images(p);
}

static Page *new_page(const char *url) {
    Page *p = xcalloc(1, sizeof *p);
    p->url = xstrdup(url);
    return p;
}

Page *page_from_html(const char *url, const char *html) {
    Page *p = new_page(url);
    Viewport vp = {1024, 768};
    finish_page(p, html, strlen(html), vp);
    return p;
}

static void html_escape(Buf *b, const char *s) {
    for (; *s; s++) {
        if (*s == '<') buf_puts(b, "&lt;");
        else if (*s == '>') buf_puts(b, "&gt;");
        else if (*s == '&') buf_puts(b, "&amp;");
        else buf_putc(b, *s);
    }
}

static Page *error_page(const char *url, const char *msg) {
    Buf h;
    buf_init(&h);
    buf_puts(&h, "<!DOCTYPE html><html><head><title>Erro ao carregar</title><style>"
                 "body{font-family:sans-serif;margin:0;background:#f7f7f8;color:#222}"
                 ".box{max-width:620px;margin:80px auto;padding:0 24px}"
                 "h1{font-size:26px;margin:0 0 12px}code{background:#e8e8ec;padding:2px 6px}"
                 "p{line-height:1.5}.hole{width:56px;height:56px;border-radius:28px;background:#111;margin-bottom:20px}"
                 "</style></head><body><div class=box><div class=hole></div><h1>N&atilde;o foi poss&iacute;vel abrir a p&aacute;gina</h1><p>");
    html_escape(&h, msg);
    buf_puts(&h, "</p><p>Endere&ccedil;o: <code>");
    html_escape(&h, url);
    buf_puts(&h, "</code></p></div></body></html>");
    Page *p = page_from_html(url, h.data);
    buf_free(&h);
    return p;
}

Page *page_load(const char *url, bool user_initiated) {
    if (!strcmp(url, "about:blank")) return page_from_html(url, "<html><body></body></html>");
    FetchOpts o = net_default_opts();
    o.allow_file = user_initiated;
    Response r;
    if (!net_fetch(url, &o, &r)) {
        Page *p = error_page(url, r.error ? r.error : "erro desconhecido");
        response_free(&r);
        return p;
    }
    Page *p = new_page(r.url ? r.url : url);
    p->status = r.status;
    p->secure = r.secure;
    p->html_bytes = r.body_len;
    p->truncated = r.truncated;
    if (r.truncated) diag_note("rede", "corpo da resposta truncado (a conexão caiu antes do fim)");
    p->allow_file_subresources = user_initiated && !strncmp(p->url, "file:", 5);

    const char *ct = r.content_type ? r.content_type : "text/html";
    Viewport    vp = {1024, 768};
    if (strcmp(ct, "text/html") && strcmp(ct, "application/xhtml+xml") && strcmp(ct, "text/plain") && strcmp(ct, "application/octet-stream")) {
        char msg[256];
        snprintf(msg, sizeof msg, "Este navegador ainda não sabe exibir conteúdo do tipo \"%s\".", ct);
        page_free(p);
        Page *e = error_page(url, msg);
        response_free(&r);
        return e;
    }
    const char *label = r.charset;
    char       *meta = NULL;
    if (!label) { meta = charset_sniff_meta(r.body, r.body_len); label = meta; }
    size_t n;
    char  *u8 = charset_to_utf8(r.body, r.body_len, label, &n);
    free(meta);
    if (!strcmp(ct, "text/plain")) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "<html><body><pre>");
        html_escape(&b, u8);
        buf_puts(&b, "</pre></body></html>");
        finish_page(p, b.data, b.len, vp);
        buf_free(&b);
    } else {
        finish_page(p, u8, n, vp);
    }
    free(u8);
    response_free(&r);
    return p;
}

/* ------------------------------------------------------------------- images - */
#define MAX_IMAGES 80
#define MAX_IMAGE_BYTES (10u * 1024 * 1024)
#define IMAGE_WORKERS 6

/* Picks the URL an <img> should load (src, lazy-loading attributes, srcset) or NULL. */
static char *image_url_for(const Page *p, const Node *img) {
    const char *src = dom_attr(img, "src");
    bool placeholder = !src || !*src || !strncmp(src, "data:image/gif;base64,R0lGODlhAQABA", 35) || !strncmp(src, "about:", 6);
    if (placeholder) {
        static const char *lazy[] = {"data-src", "data-lazy-src", "data-original", "data-srcset", "srcset", NULL};
        for (int i = 0; lazy[i]; i++) {
            const char *v = dom_attr(img, lazy[i]);
            if (v && *v) {
                /* srcset syntax: "url 1x, url2 2x": take the first candidate */
                char *copy = xstrdup(v);
                char *t = str_trim(copy);
                size_t n = strcspn(t, " \t,");
                t[n] = 0;
                char *abs = *t ? url_resolve(p->base ? p->base : p->url, t) : NULL;
                free(copy);
                if (abs) return abs;
            }
        }
        if (!src || !*src) return NULL;
    }
    return url_resolve(p->base ? p->base : p->url, src);
}

typedef struct {
    Page       *p;
    char      **urls;
    PageImage **out;
    int         n, next;
    pthread_mutex_t mu;
} ImgJob;

static void *image_worker(void *arg) {
    ImgJob *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        int i = j->next++;
        pthread_mutex_unlock(&j->mu);
        if (i >= j->n) break;
        PageImage *pi = xcalloc(1, sizeof *pi);
        pi->url = xstrdup(j->urls[i]);
        pi->failed = true;
        FetchOpts o = net_default_opts();
        o.max_body = MAX_IMAGE_BYTES;
        o.timeout_ms = 8000;
        o.total_timeout_ms = 20000;
        o.allow_file = j->p->allow_file_subresources;
        Response r;
        if (net_fetch(pi->url, &o, &r) && r.status >= 200 && r.status < 300) {
            if (image_decode(r.body, r.body_len, &pi->img)) pi->failed = false;
            else { bh_log("image decode failed: %s (%s)", pi->url, r.content_type ? r.content_type : "?"); diag_note("imagem-formato-nao-suportado", r.content_type ? r.content_type : "?"); }
        } else {
            bh_log("image fetch failed: %s: %s", pi->url, r.error ? r.error : "HTTP error");
        }
        response_free(&r);
        j->out[i] = pi;
    }
    return NULL;
}

static void prefetch_images(Page *p) {
    char **urls = NULL;
    int    n = 0;
    bool   page_https = !strncmp(p->url, "https://", 8);
    Node  *nd = p->doc->first_child;
    while (nd && n < MAX_IMAGES) {
        if (nd->type == NODE_ELEMENT && !strcmp(nd->tag, "img")) {
            char *u = image_url_for(p, nd);
            bool  ok = u && (!page_https || strncmp(u, "http://", 7) != 0); /* no mixed content */
            if (ok) for (int i = 0; i < n; i++) if (!strcmp(urls[i], u)) { ok = false; break; }
            if (ok) {
                urls = xrealloc(urls, sizeof(char *) * (size_t)(n + 1));
                urls[n++] = u;
            } else free(u);
        }
        if (nd->first_child) { nd = nd->first_child; continue; }
        while (nd && nd != p->doc && !nd->next) nd = nd->parent;
        if (!nd || nd == p->doc) break;
        nd = nd->next;
    }
    if (n == 0) { free(urls); return; }
    ImgJob job = {p, urls, xcalloc((size_t)n, sizeof(PageImage *)), n, 0, PTHREAD_MUTEX_INITIALIZER};
    pthread_t th[IMAGE_WORKERS];
    int       nt = n < IMAGE_WORKERS ? n : IMAGE_WORKERS, started = 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);
    for (int i = 0; i < nt; i++)
        if (pthread_create(&th[started], &at, image_worker, &job) == 0) started++;
    pthread_attr_destroy(&at);
    if (started == 0) image_worker(&job); /* could not spawn threads: do it inline */
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < n; i++) {
        if (job.out[i]) { job.out[i]->next = p->images; p->images = job.out[i]; p->nimages_fetched++; }
        free(urls[i]);
    }
    free(urls);
    free(job.out);
}

static Image *resolve_image(void *user, const Node *img) {
    Page *p = user;
    char *u = image_url_for(p, img);
    if (!u) return NULL;
    Image *res = NULL;
    for (PageImage *pi = p->images; pi; pi = pi->next)
        if (!pi->failed && !strcmp(pi->url, u)) { res = &pi->img; break; }
    free(u);
    return res;
}

void page_layout(Page *p, float vw, float vh) {
    layout_free(p->layout);
    p->layout = NULL;
    arena_free(&p->style_arena);
    p->vw = vw;
    p->vh = vh;
    StyleSet set = {p->sheets, p->nsheets, {vw, vh}, &p->dom_arena};
    style_compute_tree(p->doc, &set, &p->style_arena);
    p->layout = layout_create(p->doc, vw, vh, resolve_image, p);
}

void page_free(Page *p) {
    if (!p) return;
    layout_free(p->layout);
    for (PageImage *pi = p->images; pi;) {
        PageImage *nx = pi->next;
        image_free(&pi->img);
        free(pi->url);
        free(pi);
        pi = nx;
    }
    free(p->base);
    for (int i = 1; i < p->nsheets; i++) css_free(p->sheets[i]);
    free(p->sheets);
    arena_free(&p->style_arena);
    arena_free(&p->dom_arena);
    free(p->url);
    free(p->title);
    free(p);
}
