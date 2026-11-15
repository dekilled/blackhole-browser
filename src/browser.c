#include "browser.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "url.h"

/* ---- palette (LUMNA dark purple) ---- */
#define C_STRIP 0xFF0D0A18U
#define C_TABACT 0xFF1C1738U
#define C_PILL 0xFF100D21U
#define C_PILL_LINE 0xFF3B2F8FU
#define C_ACCENT 0xFF8B5CF6U
#define C_TEXT 0xFFE9E6F8U
#define C_DIM 0xFF8D88AAU
#define C_GOOD 0xFF4CD08AU
#define C_BAD 0xFFFF6B6BU
#define C_STATUS 0xFF1B1633U
#define C_LINE 0xFF050309U

typedef struct {
    int   fd, tab_id, load_id;
    char *url;
    bool  user_typed;
} LoadArg;

typedef struct {
    int   tab_id, load_id;
    Page *page;
} LoadResult;

/* ------------------------------------------------------------- utilities --- */
static void set_addr(Browser *b, const char *s) {
    size_t n = strlen(s);
    if (n + 1 > b->addr_cap) {
        b->addr_cap = n + 256;
        b->addr = xrealloc(b->addr, b->addr_cap);
    }
    memcpy(b->addr, s, n + 1);
    b->addr_len = n;
    b->cursor = n;
    b->addr_select_all = false;
}

Tab  *browser_tab(Browser *b) { return b->ntabs ? b->tabs[b->cur] : NULL; }
Page *browser_page(Browser *b) { Tab *t = browser_tab(b); return t ? t->page : NULL; }

bool browser_busy(Browser *b) {
    for (int i = 0; i < b->ntabs; i++) if (b->tabs[i]->loading) return true;
    return false;
}

static float view_h(const Browser *b) { return (float)(b->h - CHROME_H - STATUS_H); }

static float max_scroll(Browser *b) {
    Page *p = browser_page(b);
    if (!p || !p->layout) return 0;
    float m = layout_doc_height(p->layout) - view_h(b);
    return m > 0 ? m : 0;
}

static void clamp_scroll(Browser *b) {
    Tab *t = browser_tab(b);
    if (!t) return;
    float m = max_scroll(b);
    if (t->scroll_y > m) t->scroll_y = m;
    if (t->scroll_y < 0) t->scroll_y = 0;
}

void browser_set_status(Browser *b, const char *s) {
    if (!strcmp(b->status, s)) return;
    free(b->status);
    b->status = xstrdup(s);
    b->dirty = true;
}

static const char *host_of(const char *url, char *out, size_t cap) {
    Url u;
    out[0] = 0;
    if (url_parse(url, &u)) {
        const char *h = u.host && *u.host ? u.host : u.scheme;
        if (!strncmp(h, "www.", 4)) h += 4;
        snprintf(out, cap, "%s", h);
        url_free(&u);
    }
    return out;
}

/* --------------------------------------------------------------- tabs ----- */
static Tab *tab_new(Browser *b) {
    Tab *t = xcalloc(1, sizeof *t);
    t->id = ++b->next_tab_id;
    t->hpos = -1;
    t->shown_url = xstrdup("");
    return t;
}

static void tab_free(Tab *t) {
    page_free(t->page);
    for (int i = 0; i < t->nhist; i++) { free(t->hist[i].url); free(t->hist[i].title); }
    free(t->hist);
    free(t->load_fragment);
    free(t->shown_url);
    free(t);
}

static void update_window_title(Browser *b) {
    Page *p = browser_page(b);
    if (!b->set_title) return;
    Buf t;
    buf_init(&t);
    buf_printf(&t, "%s — %s", p && p->title && *p->title ? p->title : (p ? p->url : "Nova aba"), BH_BRAND);
    b->set_title(b->ud, t.data);
    buf_free(&t);
}

static void sync_addr(Browser *b) {
    Tab *t = browser_tab(b);
    b->addr_focus = false;
    set_addr(b, t && strcmp(t->shown_url, "about:newtab") ? t->shown_url : "");
}

void browser_switch_tab(Browser *b, int idx) {
    if (idx < 0 || idx >= b->ntabs) return;
    b->cur = idx;
    b->hover_link = NULL;
    sync_addr(b);
    update_window_title(b);
    b->dirty = true;
}

void browser_close_tab(Browser *b, int idx) {
    if (idx < 0 || idx >= b->ntabs) return;
    if (b->ntabs == 1) { b->quit = true; return; }
    tab_free(b->tabs[idx]);
    for (int i = idx; i + 1 < b->ntabs; i++) b->tabs[i] = b->tabs[i + 1];
    b->ntabs--;
    if (b->cur > idx || b->cur >= b->ntabs) b->cur = b->cur > 0 ? b->cur - 1 : 0;
    browser_switch_tab(b, b->cur);
}

void browser_new_tab(Browser *b, const char *url) {
    if (b->ntabs >= MAX_TABS) return;
    b->tabs[b->ntabs++] = tab_new(b);
    b->cur = b->ntabs - 1;
    browser_navigate(b, url ? url : "about:newtab", true, true);
    if (!url) {
        b->addr_focus = true;
        b->addr_select_all = false;
    }
}

/* ------------------------------------------------------------ new tab page -- */
static void html_esc(Buf *b, const char *s) {
    for (; *s; s++) {
        if (*s == '<') buf_puts(b, "&lt;");
        else if (*s == '>') buf_puts(b, "&gt;");
        else if (*s == '&') buf_puts(b, "&amp;");
        else if (*s == '"') buf_puts(b, "&quot;");
        else buf_putc(b, *s);
    }
}

static char *newtab_html(Browser *b) {
    Buf h;
    buf_init(&h);
    buf_puts(&h,
        "<!DOCTYPE html><html lang=pt-BR><head><meta charset=utf-8><title>Nova aba</title><style>"
        "*{box-sizing:border-box}"
        "body{margin:0;font-family:sans-serif;color:#ece9fb;background:radial-gradient(circle at 88% -12%,#4c1d95 0%,#150f2e 42%,#0b0816 100%)}"
        ".page{max-width:900px;margin:0 auto;padding:54px 24px 40px;text-align:center}"
        ".logo{font-size:22px;letter-spacing:9px;color:#fff}.gem{color:#a78bfa;font-size:36px;vertical-align:middle;margin-right:12px;letter-spacing:0}"
        "h1{font-size:40px;margin:22px 0 30px;font-weight:600}"
        ".search{display:block;max-width:760px;margin:0 auto 36px;padding:16px 26px;border:1px solid #5b45d6;border-radius:30px;"
        "background:rgba(20,15,45,.85);color:#9d98bd;text-align:left;text-decoration:none;font-size:18px}"
        ".tiles{display:flex;justify-content:center;gap:18px;margin-bottom:48px}"
        ".tile{display:block;width:112px;padding:18px 0 14px;border:1px solid #2a2350;border-radius:14px;background:#141029;color:#ddd8f5;text-decoration:none;font-size:14px}"
        ".ic{width:40px;height:40px;margin:0 auto 10px;border-radius:10px;color:#fff;font-size:22px;line-height:40px;font-weight:bold}"
        ".sec{text-align:left;margin:0 0 14px;font-size:17px;font-weight:600}"
        ".cards{display:flex;gap:18px;text-align:left}"
        ".card{flex:1;display:block;border:1px solid #2a2350;border-radius:12px;background:#120e25;color:#ddd8f5;text-decoration:none}"
        ".thumb{height:120px;border-radius:11px 11px 0 0;background:linear-gradient(135deg,#1b1240,#5b21b6)}"
        ".meta{padding:12px 14px}.meta b{display:block;font-size:15px;margin-bottom:3px}.meta span{color:#8d88ad;font-size:13px}"
        ".empty{color:#8d88ad;text-align:left}"
        "</style></head><body><div class=page>"
        "<div class=logo><span class=gem>&#9670;</span>" BH_BRAND "</div>"
        "<h1>Seu espa&ccedil;o na web.</h1>"
        "<a class=search href=\"bh:focus\">&#9906;&nbsp;&nbsp;O que vamos explorar?</a>"
        "<div class=tiles>"
        "<a class=tile href=\"https://www.youtube.com\"><div class=ic style=\"background:#e11d48\">&#9654;</div>YouTube</a>"
        "<a class=tile href=\"https://github.com\"><div class=ic style=\"background:#f4f4f5;color:#111\">G</div>GitHub</a>"
        "<a class=tile href=\"https://www.netflix.com\"><div class=ic style=\"background:#111;color:#e50914\">N</div>Netflix</a>"
        "<a class=tile href=\"https://wiki.archlinux.org\"><div class=ic style=\"background:#1793d1\">A</div>Arch Wiki</a>"
        "<a class=tile href=\"https://pt.wikipedia.org\"><div class=ic style=\"background:#6d28d9\">W</div>Wikip&eacute;dia</a>"
        "<a class=tile href=\"bh:soon\"><div class=ic style=\"background:#1f1a3d;color:#a78bfa\">+</div>Adicionar</a>"
        "</div><div class=sec>Continuar de onde parou</div>");
    int shown = b->nrecent > 3 ? 3 : b->nrecent;
    if (!shown) buf_puts(&h, "<div class=empty>Suas p&aacute;ginas recentes aparecer&atilde;o aqui.</div>");
    else {
        buf_puts(&h, "<div class=cards>");
        for (int i = 0; i < shown; i++) {
            char host[256];
            host_of(b->recent[i].url, host, sizeof host);
            buf_puts(&h, "<a class=card href=\"");
            html_esc(&h, b->recent[i].url);
            buf_puts(&h, "\"><div class=thumb></div><div class=meta><b>");
            html_esc(&h, b->recent[i].title && *b->recent[i].title ? b->recent[i].title : host);
            buf_puts(&h, "</b><span>");
            html_esc(&h, host);
            buf_puts(&h, "</span></div></a>");
        }
        buf_puts(&h, "</div>");
    }
    buf_puts(&h, "</div></body></html>");
    return buf_steal(&h);
}

static void remember_recent(Browser *b, const Page *p) {
    if (!p || !strncmp(p->url, "about:", 6) || !strncmp(p->url, "data:", 5)) return;
    int at = -1;
    for (int i = 0; i < b->nrecent; i++) if (!strcmp(b->recent[i].url, p->url)) { at = i; break; }
    Recent r = {xstrdup(p->url), xstrdup(p->title ? p->title : "")};
    if (at >= 0) { free(b->recent[at].url); free(b->recent[at].title); }
    else if (b->nrecent < 6) at = b->nrecent++;
    else { at = 5; free(b->recent[5].url); free(b->recent[5].title); }
    for (int i = at; i > 0; i--) b->recent[i] = b->recent[i - 1];
    b->recent[0] = r;
}

/* ---------------------------------------------------------- navigation ----- */
Browser *browser_new(int w, int h) {
    Browser *b = xcalloc(1, sizeof *b);
    b->w = w;
    b->h = h;
    b->canvas = canvas_new(w, h);
    b->dirty = true;
    b->status = xstrdup("");
    b->addr = xstrdup("");
    b->addr_cap = 1;
    if (pipe(b->notify_fd) != 0) bh_die("pipe failed");
    b->tabs[b->ntabs++] = tab_new(b);
    return b;
}

void browser_free(Browser *b) {
    if (!b) return;
    for (int i = 0; i < b->ntabs; i++) tab_free(b->tabs[i]);
    for (int i = 0; i < b->nrecent; i++) { free(b->recent[i].url); free(b->recent[i].title); }
    free(b->addr);
    free(b->status);
    close(b->notify_fd[0]);
    close(b->notify_fd[1]);
    canvas_free(b->canvas);
    free(b);
}

static void *loader_main(void *p) {
    LoadArg *a = p;
    Page    *pg = page_load(a->url, a->user_typed);
    LoadResult *r = xmalloc(sizeof *r);
    r->tab_id = a->tab_id;
    r->load_id = a->load_id;
    r->page = pg;
    if (write(a->fd, &r, sizeof r) != (ssize_t)sizeof r) { page_free(pg); free(r); }
    free(a->url);
    free(a);
    return NULL;
}

static char *normalize_input(const char *in) {
    char *s = xstrdup(in);
    char *t = str_trim(s);
    char *out;
    if (!*t) { free(s); return xstrdup("about:newtab"); }
    bool has_scheme = strstr(t, "://") || !strncmp(t, "about:", 6) || !strncmp(t, "data:", 5) || !strncmp(t, "file:", 5) || !strncmp(t, "bh:", 3);
    bool looks_host = !strchr(t, ' ') && (strchr(t, '.') || !strncmp(t, "localhost", 9));
    if (has_scheme) out = xstrdup(t);
    else if (t[0] == '/' && access(t, R_OK) == 0) {
        out = xmalloc(strlen(t) + 8);
        sprintf(out, "file://%s", t);
    } else if (looks_host) {
        out = xmalloc(strlen(t) + 16);
        bool local = !strncmp(t, "localhost", 9) || !strncmp(t, "127.", 4);
        sprintf(out, "%s://%s", local ? "http" : "https", t);
    } else {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "https://duckduckgo.com/html/?q=");
        for (const char *c = t; *c; c++) {
            if (is_alpha((unsigned char)*c) || is_digit((unsigned char)*c) || *c == '-' || *c == '_' || *c == '.') buf_putc(&b, *c);
            else if (*c == ' ') buf_putc(&b, '+');
            else buf_printf(&b, "%%%02X", (unsigned char)*c);
        }
        out = buf_steal(&b);
    }
    free(s);
    return out;
}

static void hist_push(Tab *t, const char *url, const char *title) {
    for (int i = t->hpos + 1; i < t->nhist; i++) { free(t->hist[i].url); free(t->hist[i].title); }
    t->nhist = t->hpos + 1;
    if (t->nhist == t->caphist) {
        t->caphist = t->caphist ? t->caphist * 2 : 16;
        t->hist = xrealloc(t->hist, sizeof(HistEntry) * (size_t)t->caphist);
    }
    t->hist[t->nhist].url = xstrdup(url);
    t->hist[t->nhist].title = xstrdup(title ? title : "");
    t->hist[t->nhist].scroll = 0;
    t->hpos = t->nhist++;
}

static char *strip_fragment(const char *u) {
    char *c = xstrdup(u);
    char *h = strchr(c, '#');
    if (h) *h = 0;
    return c;
}

static void scroll_to_fragment(Browser *b, Tab *t, const char *frag) {
    if (!t->page || !t->page->layout) return;
    if (!frag || !*frag) t->scroll_y = 0;
    else {
        char *id = xstrdup(frag);
        url_percent_decode(id);
        float y = layout_find_anchor(t->page->layout, id);
        free(id);
        if (y >= 0) t->scroll_y = y;
    }
    if (t == browser_tab(b)) clamp_scroll(b);
}

static void apply_page(Browser *b, Tab *t, Page *pg) {
    page_free(t->page);
    t->page = pg;
    page_layout(pg, (float)b->w, view_h(b));
    t->scroll_y = t->pending_scroll;
    t->pending_scroll = 0;
    bool active = t == browser_tab(b);
    if (active) clamp_scroll(b);
    if (t->load_fragment && *t->load_fragment) scroll_to_fragment(b, t, t->load_fragment);
    Buf shown;
    buf_init(&shown);
    if (t->load_fragment && *t->load_fragment) {
        char *base = strip_fragment(pg->url);
        buf_printf(&shown, "%s#%s", base, t->load_fragment);
        free(base);
    } else buf_puts(&shown, pg->url);
    free(t->shown_url);
    t->shown_url = buf_steal(&shown);
    if (t->load_push) hist_push(t, t->shown_url, pg->title);
    else if (t->hpos >= 0) {
        free(t->hist[t->hpos].url);
        t->hist[t->hpos].url = xstrdup(t->shown_url);
        free(t->hist[t->hpos].title);
        t->hist[t->hpos].title = xstrdup(pg->title ? pg->title : "");
    }
    remember_recent(b, pg);
    t->loading = false;
    if (active) {
        sync_addr(b);
        browser_set_status(b, "");
        update_window_title(b);
    }
    b->dirty = true;
}

void browser_navigate(Browser *b, const char *input, bool push, bool user_typed) {
    Tab *t = browser_tab(b);
    if (!t) return;
    char *url = normalize_input(input);
    if (!strncmp(url, "bh:", 3)) { /* internal pseudo-links */
        if (!strcmp(url, "bh:focus")) { b->addr_focus = true; b->addr_select_all = false; b->dirty = true; }
        else browser_set_status(b, "Em breve!");
        free(url);
        return;
    }
    if (t->page && strchr(url, '#')) { /* same-document fragment navigation */
        char *a = strip_fragment(url), *c = strip_fragment(t->page->url);
        bool  same = !strcmp(a, c);
        free(a);
        free(c);
        if (same) {
            if (t->hpos >= 0) t->hist[t->hpos].scroll = t->scroll_y;
            if (push) hist_push(t, url, t->page->title);
            else if (t->hpos >= 0) { free(t->hist[t->hpos].url); t->hist[t->hpos].url = xstrdup(url); }
            scroll_to_fragment(b, t, strchr(url, '#') + 1);
            free(t->shown_url);
            t->shown_url = xstrdup(url);
            sync_addr(b);
            b->dirty = true;
            free(url);
            return;
        }
    }
    if (t->hpos >= 0 && t->hpos < t->nhist) t->hist[t->hpos].scroll = t->scroll_y;
    t->load_id++;
    t->load_push = push;
    free(t->load_fragment);
    const char *h = strchr(url, '#');
    t->load_fragment = h ? xstrdup(h + 1) : NULL;

    if (!strcmp(url, "about:newtab")) { /* built in, no network */
        char *html = newtab_html(b);
        Page *pg = page_from_html("about:newtab", html);
        free(html);
        free(url);
        apply_page(b, t, pg);
        return;
    }
    t->loading = true;
    free(t->shown_url);
    t->shown_url = xstrdup(url);
    sync_addr(b);
    char msg[600];
    snprintf(msg, sizeof msg, "Carregando %s…", url);
    browser_set_status(b, msg);

    LoadArg *a = xmalloc(sizeof *a);
    a->fd = b->notify_fd[1];
    a->tab_id = t->id;
    a->load_id = t->load_id;
    a->url = url;
    a->user_typed = user_typed;
    pthread_t      th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1 << 20);
    if (pthread_create(&th, &at, loader_main, a) != 0) {
        free(a->url);
        free(a);
        t->loading = false;
    }
    pthread_attr_destroy(&at);
    b->dirty = true;
}

void browser_loader_ready(Browser *b) {
    LoadResult *r;
    if (read(b->notify_fd[0], &r, sizeof r) != (ssize_t)sizeof r) return;
    Tab *t = NULL;
    for (int i = 0; i < b->ntabs; i++) if (b->tabs[i]->id == r->tab_id) t = b->tabs[i];
    if (t && t->load_id == r->load_id) apply_page(b, t, r->page);
    else page_free(r->page); /* tab closed or superseded by a newer navigation */
    free(r);
}

static void go_history(Browser *b, int delta) {
    Tab *t = browser_tab(b);
    int  np = t->hpos + delta;
    if (np < 0 || np >= t->nhist) return;
    if (t->hpos >= 0) t->hist[t->hpos].scroll = t->scroll_y;
    t->hpos = np;
    t->pending_scroll = t->hist[np].scroll;
    char *u = xstrdup(t->hist[np].url);
    browser_navigate(b, u, false, false);
    free(u);
}

void browser_resize(Browser *b, int w, int h) {
    if (w == b->w && h == b->h) return;
    b->w = w;
    b->h = h;
    canvas_resize(b->canvas, w, h);
    for (int i = 0; i < b->ntabs; i++)
        if (b->tabs[i]->page) page_layout(b->tabs[i]->page, (float)w, view_h(b));
    clamp_scroll(b);
    b->dirty = true;
}

/* ----------------------------------------------------------- drawing ------- */
static void rrect(Browser *b, float x, float y, float w, float h, float r, uint32_t col) {
    float rr[4] = {r, r, r, r};
    canvas_fill_rrect(b->canvas, x, y, w, h, rr, col);
}

static void rring(Browser *b, float x, float y, float w, float h, float r, float t, uint32_t col) {
    float rr[4] = {r, r, r, r};
    canvas_stroke_rrect(b->canvas, x, y, w, h, rr, t, col);
}

static void text_ell(Browser *b, BhFont *f, float x, float base, const char *s, float maxw, uint32_t col) {
    size_t n = strlen(s);
    if (font_text_width(f, s, n, 0) <= maxw) { canvas_draw_text(b->canvas, f, x, base, s, n, col, 0); return; }
    float ew = font_text_width(f, "…", 3, 0);
    size_t k = 0;
    const char *end = s + n;
    const char *p = s;
    while (p < end) {
        uint32_t cp;
        int l = utf8_decode(p, end, &cp);
        if (font_text_width(f, s, (size_t)(p - s) + (size_t)l, 0) + ew > maxw) break;
        p += l;
        k = (size_t)(p - s);
    }
    canvas_draw_text(b->canvas, f, x, base, s, k, col, 0);
    canvas_draw_text(b->canvas, f, x + font_text_width(f, s, k, 0), base, "…", 3, col, 0);
}

static void glyph_center(Browser *b, BhFont *f, float cx, float cy, const char *g, uint32_t col) {
    float w = font_text_width(f, g, strlen(g), 0);
    canvas_draw_text(b->canvas, f, cx - w / 2, cy + font_ascent(f) / 2 - 1, g, strlen(g), col, 0);
}

static void favicon(Browser *b, float x, float y, const Page *p) {
    BhFont *f = font_get(FONT_SANS, true, false, 11);
    if (!p || !strcmp(p->url, "about:newtab")) {
        BhFont *g = font_get(FONT_SANS, false, false, 16);
        glyph_center(b, g, x + 8, y + 8, "◆", C_ACCENT);
        return;
    }
    char host[256];
    host_of(p->url, host, sizeof host);
    uint32_t hsh = 2166136261u;
    for (char *c = host; *c; c++) hsh = (hsh ^ (uint8_t)*c) * 16777619u;
    static const uint32_t pal[] = {0xFF7C3AED, 0xFF2563EB, 0xFF0D9488, 0xFFDB2777, 0xFFEA580C, 0xFF65A30D, 0xFFDC2626};
    canvas_fill_ellipse(b->canvas, x + 8, y + 8, 8, 8, pal[hsh % 7]);
    char L[2] = {host[0] ? (char)(host[0] & ~32) : '?', 0};
    glyph_center(b, f, x + 8, y + 8, L, 0xFFFFFFFF);
}

static void tab_geom(const Browser *b, int i, float *x, float *w) {
    float avail = (float)b->w - 10 - 44 - 90;
    float tw = avail / (float)b->ntabs - 4;
    if (tw > 230) tw = 230;
    if (tw < 96) tw = 96;
    *x = 10 + (float)i * (tw + 4);
    *w = tw;
}

static float addr_x(void) { return 128; }
static float addr_w(const Browser *b) { float w = (float)b->w - 128 - 150; return w < 60 ? 60 : w; }

static void draw_chrome(Browser *b) {
    Canvas *c = b->canvas;
    BhFont *ui = font_get(FONT_SANS, false, false, 13);
    BhFont *ico = font_get(FONT_SANS, false, false, 18);
    canvas_fill_rect(c, 0, 0, (float)b->w, TAB_H, C_STRIP);
    canvas_fill_rect(c, 0, TAB_H, (float)b->w, TOOL_H, C_TABACT);
    canvas_fill_rect(c, 0, CHROME_H - 1, (float)b->w, 1, C_LINE);

    /* tabs */
    for (int i = 0; i < b->ntabs; i++) {
        float x, w;
        tab_geom(b, i, &x, &w);
        Tab *t = b->tabs[i];
        bool act = i == b->cur;
        if (act) {
            float rr[4] = {10, 10, 0, 0};
            canvas_fill_rrect(c, x, 6, w, TAB_H - 6, rr, C_TABACT);
        }
        favicon(b, x + 12, 15, t->page);
        const char *title = t->page && t->page->title && *t->page->title ? t->page->title
                            : (t->page && !strcmp(t->page->url, "about:newtab") ? "Nova aba" : (t->loading ? "Carregando…" : "Nova aba"));
        text_ell(b, ui, x + 36, 25, title, w - 36 - 28, act ? C_TEXT : C_DIM);
        glyph_center(b, ui, x + w - 16, 20, "✕", act ? C_DIM : 0xFF5E5A78);
    }
    {
        float x, w;
        tab_geom(b, b->ntabs, &x, &w);
        glyph_center(b, ico, x + 12, 21, "+", C_TEXT);
    }

    /* toolbar buttons */
    Tab *t = browser_tab(b);
    bool can_back = t && t->hpos > 0, can_fwd = t && t->hpos >= 0 && t->hpos < t->nhist - 1;
    glyph_center(b, ico, 24, TAB_H + 21, "←", can_back ? C_TEXT : 0xFF4D4968);
    glyph_center(b, ico, 62, TAB_H + 21, "→", can_fwd ? C_TEXT : 0xFF4D4968);
    glyph_center(b, ico, 100, TAB_H + 23, t && t->loading ? "✕" : "↻", C_TEXT);

    /* address pill */
    float ax = addr_x(), aw = addr_w(b), ay = TAB_H + 6, ah = 34;
    rrect(b, ax, ay, aw, ah, 17, C_PILL);
    rring(b, ax, ay, aw, ah, 17, 1, b->addr_focus ? C_ACCENT : C_PILL_LINE);
    float tx = ax + 40;
    Page *p = t ? t->page : NULL;
    bool newtab = !p || !strcmp(p->url, "about:newtab");
    if (!b->addr_focus && !newtab) {
        bool is_https = !strncmp(p->url, "https://", 8);
        bool plain = !strncmp(p->url, "file://", 7) || !strncmp(p->url, "about:", 6);
        if (is_https) {
            canvas_fill_rect(c, ax + 17, ay + 15, 10, 8, C_GOOD);
            canvas_fill_rect(c, ax + 19, ay + 10, 1, 6, C_GOOD);
            canvas_fill_rect(c, ax + 24, ay + 10, 1, 6, C_GOOD);
            canvas_fill_rect(c, ax + 19, ay + 9, 6, 1, C_GOOD);
        } else if (!plain) {
            glyph_center(b, ui, ax + 22, ay + 17, "⚠", C_BAD);
            tx = ax + 44;
        } else glyph_center(b, ico, ax + 22, ay + 17, "⚲", C_DIM);
        if (!is_https && !plain) {
            const char *warn = "Não seguro";
            BhFont *sf = font_get(FONT_SANS, false, false, 12);
            canvas_draw_text(c, sf, tx, ay + 22, warn, strlen(warn), C_BAD, 0);
            tx += font_text_width(sf, warn, strlen(warn), 0) + 10;
        }
    } else glyph_center(b, ico, ax + 22, ay + 17, "⚲", C_DIM);
    int old[4];
    canvas_set_clip(c, (int)tx, (int)ay, (int)(ax + aw - tx - 14), (int)ah, old);
    BhFont *af = font_get(FONT_SANS, false, false, 15);
    const char *txt = b->addr_len ? b->addr : "Pesquisar ou digitar endereço";
    uint32_t    col = b->addr_len ? C_TEXT : C_DIM;
    float       tw = font_text_width(af, txt, b->addr_len ? b->addr_len : strlen(txt), 0), shift = 0;
    if (b->addr_focus && b->addr_len) {
        float cw = font_text_width(af, b->addr, b->cursor, 0), room = ax + aw - tx - 24;
        if (cw > room) shift = cw - room;
    }
    if (b->addr_focus && b->addr_select_all && b->addr_len) canvas_fill_rect(c, tx, ay + 7, tw, 20, 0xFF4B3FA0);
    canvas_draw_text(c, af, tx - shift, ay + 22, txt, b->addr_len ? b->addr_len : strlen(txt), col, 0);
    if (b->addr_focus && !b->addr_select_all) canvas_fill_rect(c, tx - shift + font_text_width(af, b->addr, b->cursor, 0), ay + 8, 1, 18, C_TEXT);
    canvas_restore_clip(c, old);

    /* right-hand icons (visual only for now) */
    float rx = (float)b->w - 130;
    glyph_center(b, ico, rx, TAB_H + 23, "☆", C_TEXT);
    glyph_center(b, ico, rx + 36, TAB_H + 23, "↓", C_TEXT);
    canvas_fill_ellipse(c, rx + 72, TAB_H + 23, 11, 11, 0xFF2A2250);
    glyph_center(b, ui, rx + 72, TAB_H + 23, "◆", C_ACCENT);
    glyph_center(b, ico, rx + 106, TAB_H + 23, "⋮", C_TEXT);
    if (t && t->loading) {
        canvas_fill_rect(c, 0, CHROME_H - 3, (float)b->w, 2, 0xFF2A2442);
        canvas_fill_rect(c, 0, CHROME_H - 3, (float)b->w * 0.35f, 2, C_ACCENT);
    }
}

static void draw_status(Browser *b) {
    if (!b->status[0]) return;
    Canvas *c = b->canvas;
    BhFont *f = font_get(FONT_SANS, false, false, 12);
    float w = font_text_width(f, b->status, strlen(b->status), 0) + 18;
    if (w > (float)b->w * 0.8f) w = (float)b->w * 0.8f;
    float y = (float)(b->h - STATUS_H);
    float rr[4] = {0, 8, 0, 0};
    canvas_fill_rrect(c, 0, y, w, STATUS_H, rr, C_STATUS);
    int old[4];
    canvas_set_clip(c, 0, (int)y, (int)w, STATUS_H, old);
    canvas_draw_text(c, f, 9, y + 15, b->status, strlen(b->status), C_TEXT, 0);
    canvas_restore_clip(c, old);
}

static void draw_scrollbar(Browser *b) {
    float m = max_scroll(b);
    Tab *t = browser_tab(b);
    if (m <= 0 || !t) return;
    float vh = view_h(b), dh = layout_doc_height(t->page->layout);
    float th = vh * vh / dh;
    if (th < 28) th = 28;
    float ty = CHROME_H + (vh - th) * (t->scroll_y / m);
    rrect(b, (float)b->w - 9, ty, 6, th, 3, 0x99808090);
}

void browser_render(Browser *b) {
    Canvas *c = b->canvas;
    Tab *t = browser_tab(b);
    if (t && t->page && t->page->layout) layout_paint(t->page->layout, c, 0, CHROME_H, b->w, b->h - CHROME_H, t->scroll_y);
    else canvas_fill_rect(c, 0, CHROME_H, (float)b->w, (float)(b->h - CHROME_H), 0xFF0B0816);
    draw_scrollbar(b);
    draw_chrome(b);
    draw_status(b);
    b->dirty = false;
}

/* ---------------------------------------------------------------- input ---- */
static void addr_insert(Browser *b, const char *s) {
    size_t n = strlen(s);
    if (b->addr_select_all) {
        b->addr_len = 0;
        b->cursor = 0;
        b->addr_select_all = false;
    }
    if (b->addr_len + n + 1 > b->addr_cap) {
        b->addr_cap = (b->addr_len + n + 1) * 2;
        b->addr = xrealloc(b->addr, b->addr_cap);
    }
    memmove(b->addr + b->cursor + n, b->addr + b->cursor, b->addr_len - b->cursor + 1);
    memcpy(b->addr + b->cursor, s, n);
    b->addr_len += n;
    b->cursor += n;
    b->dirty = true;
}

static size_t prev_char(const Browser *b, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)b->addr[i] & 0xC0) == 0x80) i--;
    return i;
}
static size_t next_char(const Browser *b, size_t i) {
    if (i >= b->addr_len) return i;
    i++;
    while (i < b->addr_len && ((unsigned char)b->addr[i] & 0xC0) == 0x80) i++;
    return i;
}

void browser_text(Browser *b, const char *utf8) {
    if (!b->addr_focus) return;
    for (const char *p = utf8; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7F) return;
    addr_insert(b, utf8);
}

void browser_paste(Browser *b, const char *utf8) {
    if (!b->addr_focus || !utf8) return;
    char *s = xstrdup(utf8);
    for (char *p = s; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    addr_insert(b, s);
    free(s);
}

static void focus_addr(Browser *b) {
    b->addr_focus = true;
    b->addr_select_all = b->addr_len > 0;
    b->cursor = b->addr_len;
    b->dirty = true;
}

void browser_key(Browser *b, BhKey key, int mods) {
    if ((mods & MOD_ALT) && key == KEY_LEFT) { go_history(b, -1); return; }
    if ((mods & MOD_ALT) && key == KEY_RIGHT) { go_history(b, +1); return; }
    if ((mods & MOD_CTRL) && (key == KEY_TAB || key == KEY_PGDN || key == KEY_PGUP)) {
        int d = (key == KEY_PGUP || (key == KEY_TAB && (mods & MOD_SHIFT))) ? -1 : 1;
        browser_switch_tab(b, (b->cur + d + b->ntabs) % b->ntabs);
        return;
    }
    if (b->addr_focus) {
        switch (key) {
        case KEY_ENTER: {
            char *url = xstrdup(b->addr);
            browser_navigate(b, url, true, true);
            free(url);
            return;
        }
        case KEY_ESC: sync_addr(b); b->dirty = true; return;
        case KEY_BACKSPACE:
            if (b->addr_select_all) { b->addr_len = 0; b->cursor = 0; b->addr[0] = 0; b->addr_select_all = false; }
            else if (b->cursor > 0) {
                size_t p = prev_char(b, b->cursor);
                memmove(b->addr + p, b->addr + b->cursor, b->addr_len - b->cursor + 1);
                b->addr_len -= b->cursor - p;
                b->cursor = p;
            }
            b->dirty = true;
            return;
        case KEY_DELETE:
            if (b->addr_select_all) { b->addr_len = 0; b->cursor = 0; b->addr[0] = 0; b->addr_select_all = false; }
            else if (b->cursor < b->addr_len) {
                size_t n = next_char(b, b->cursor);
                memmove(b->addr + b->cursor, b->addr + n, b->addr_len - n + 1);
                b->addr_len -= n - b->cursor;
            }
            b->dirty = true;
            return;
        case KEY_LEFT: b->addr_select_all = false; b->cursor = prev_char(b, b->cursor); b->dirty = true; return;
        case KEY_RIGHT: b->addr_select_all = false; b->cursor = next_char(b, b->cursor); b->dirty = true; return;
        case KEY_HOME: b->addr_select_all = false; b->cursor = 0; b->dirty = true; return;
        case KEY_END: b->addr_select_all = false; b->cursor = b->addr_len; b->dirty = true; return;
        default: return;
        }
    }
    Tab *t = browser_tab(b);
    if (!t) return;
    float step = 48, page = view_h(b) - 40;
    switch (key) {
    case KEY_DOWN: t->scroll_y += step; break;
    case KEY_UP: t->scroll_y -= step; break;
    case KEY_PGDN: t->scroll_y += page; break;
    case KEY_SPACE: t->scroll_y += (mods & MOD_SHIFT) ? -page : page; break;
    case KEY_PGUP: t->scroll_y -= page; break;
    case KEY_HOME: t->scroll_y = 0; break;
    case KEY_END: t->scroll_y = max_scroll(b); break;
    default: return;
    }
    clamp_scroll(b);
    b->dirty = true;
}

bool browser_shortcut(Browser *b, char letter, int mods) {
    if (!(mods & MOD_CTRL)) return false;
    Tab *t = browser_tab(b);
    switch (letter) {
    case 'l': focus_addr(b); return true;
    case 'r': if (t && t->page) { char *u = xstrdup(t->shown_url); browser_navigate(b, u, false, false); free(u); } return true;
    case 'a': if (b->addr_focus) { b->addr_select_all = b->addr_len > 0; b->dirty = true; } return true;
    case 'c': if (b->addr_focus && b->clipboard_set) b->clipboard_set(b->ud, b->addr); return true;
    case 'v': if (b->addr_focus && b->clipboard_request) b->clipboard_request(b->ud); return true;
    case 't': browser_new_tab(b, NULL); return true;
    case 'w': browser_close_tab(b, b->cur); return true;
    case 'q': b->quit = true; return true;
    default: return false;
    }
}

static int tab_at(Browser *b, int x, bool *close_btn, bool *plus) {
    *close_btn = *plus = false;
    for (int i = 0; i < b->ntabs; i++) {
        float tx, tw;
        tab_geom(b, i, &tx, &tw);
        if ((float)x >= tx && (float)x < tx + tw) {
            *close_btn = (float)x >= tx + tw - 30;
            return i;
        }
    }
    float px, pw;
    tab_geom(b, b->ntabs, &px, &pw);
    if ((float)x >= px && (float)x < px + 30) *plus = true;
    return -1;
}

static void update_hover(Browser *b, int x, int y) {
    const Node *link = NULL;
    int         ck = CURSOR_ARROW;
    Tab        *t = browser_tab(b);
    if (y < CHROME_H) {
        if (y >= TAB_H) {
            if ((float)x >= addr_x() + 36 && (float)x < addr_x() + addr_w(b)) ck = CURSOR_TEXT;
            else ck = CURSOR_HAND;
        } else ck = CURSOR_HAND;
    } else if (t && t->page && t->page->layout) {
        HitResult hr;
        if (layout_hit_test(t->page->layout, (float)x, (float)(y - CHROME_H) + t->scroll_y, t->scroll_y, &hr)) {
            link = hr.link;
            if (link) ck = CURSOR_HAND;
        }
    }
    if (link != b->hover_link) {
        b->hover_link = link;
        if (link && t && t->page) {
            const char *href = dom_attr(link, "href");
            if (href && !strncmp(href, "bh:", 3)) browser_set_status(b, "");
            else {
                char *abs = url_resolve(t->page->url, href ? href : "");
                browser_set_status(b, abs ? abs : (href ? href : ""));
                free(abs);
            }
        } else if (!(t && t->loading)) browser_set_status(b, "");
    }
    if (ck != b->cursor_kind) {
        b->cursor_kind = ck;
        if (b->set_cursor) b->set_cursor(b->ud, ck);
    }
}

void browser_mouse_move(Browser *b, int x, int y) { update_hover(b, x, y); }

void browser_mouse_down(Browser *b, int x, int y, int button) {
    Tab *t = browser_tab(b);
    if (!t) return;
    if (button == 2 && y < TAB_H) { /* middle click closes a tab */
        bool cb, pl;
        int  i = tab_at(b, x, &cb, &pl);
        if (i >= 0) browser_close_tab(b, i);
        return;
    }
    if (button != 1) return;
    if (y < TAB_H) {
        bool cb, pl;
        int  i = tab_at(b, x, &cb, &pl);
        if (i >= 0) { if (cb) browser_close_tab(b, i); else browser_switch_tab(b, i); }
        else if (pl) browser_new_tab(b, NULL);
        return;
    }
    if (y < CHROME_H) {
        if (x >= 8 && x < 40) go_history(b, -1);
        else if (x >= 46 && x < 78) go_history(b, +1);
        else if (x >= 84 && x < 116) {
            if (t->loading) { t->load_id++; t->loading = false; browser_set_status(b, ""); b->dirty = true; }
            else if (t->page) { char *u = xstrdup(t->shown_url); browser_navigate(b, u, false, false); free(u); }
        } else if ((float)x >= addr_x() && (float)x < addr_x() + addr_w(b)) {
            if (!b->addr_focus) focus_addr(b);
        }
        return;
    }
    if (b->addr_focus) {
        b->addr_focus = false;
        b->dirty = true;
    }
    if (!t->page || !t->page->layout) return;
    HitResult hr;
    if (layout_hit_test(t->page->layout, (float)x, (float)(y - CHROME_H) + t->scroll_y, t->scroll_y, &hr) && hr.link) {
        const char *href = dom_attr(hr.link, "href");
        if (!href) return;
        while (is_space((unsigned char)*href)) href++;
        if (!strncasecmp(href, "javascript:", 11)) { browser_set_status(b, "JavaScript ainda não é suportado"); return; }
        char *abs = url_resolve(t->page->url, href);
        if (!abs) return;
        bool from_file = !strncmp(t->page->url, "file:", 5);
        bool to_file = !strncmp(abs, "file:", 5);
        bool internal = !strncmp(t->page->url, "about:", 6);
        if (to_file && !from_file) {
            browser_set_status(b, "Bloqueado: página web tentando abrir arquivo local");
            free(abs);
            return;
        }
        browser_navigate(b, abs, true, from_file || internal);
        free(abs);
    }
}

void browser_wheel(Browser *b, int x, int y, float dy) {
    (void)x;
    (void)y;
    Tab *t = browser_tab(b);
    if (!t) return;
    t->scroll_y += dy;
    clamp_scroll(b);
    b->dirty = true;
}
