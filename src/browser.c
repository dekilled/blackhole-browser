#include "browser.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "url.h"

/* ---- palette ---- */
#define C_CHROME 0xFF17171CU
#define C_FIELD 0xFF26262EU
#define C_FIELD_FOCUS 0xFF2F2F3AU
#define C_TEXT 0xFFE9E9F0U
#define C_DIM 0xFF8C8C9AU
#define C_ACCENT 0xFF8A6CFFU
#define C_GOOD 0xFF4CD08AU
#define C_BAD 0xFFFF6B6BU
#define C_STATUS 0xFF202027U

typedef struct {
    Browser *b;
    int      id;
    char    *url;
    bool     user_typed;
} LoadArg;

typedef struct {
    int   id;
    Page *page;
} LoadResult;

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

Browser *browser_new(int w, int h) {
    Browser *b = xcalloc(1, sizeof *b);
    b->w = w;
    b->h = h;
    b->canvas = canvas_new(w, h);
    b->hpos = -1;
    b->dirty = true;
    b->status = xstrdup("");
    b->addr = xstrdup("");
    b->addr_cap = 1;
    if (pipe(b->notify_fd) != 0) bh_die("pipe failed");
    return b;
}

void browser_free(Browser *b) {
    if (!b) return;
    page_free(b->page);
    for (int i = 0; i < b->nhist; i++) free(b->hist[i].url);
    free(b->hist);
    free(b->addr);
    free(b->status);
    free(b->loading_url);
    free(b->load_fragment);
    close(b->notify_fd[0]);
    close(b->notify_fd[1]);
    canvas_free(b->canvas);
    free(b);
}

void browser_set_status(Browser *b, const char *s) {
    if (!strcmp(b->status, s)) return;
    free(b->status);
    b->status = xstrdup(s);
    b->dirty = true;
}

static float view_h(const Browser *b) { return (float)(b->h - CHROME_H - STATUS_H); }

static float max_scroll(const Browser *b) {
    if (!b->page || !b->page->layout) return 0;
    float m = layout_doc_height(b->page->layout) - view_h(b);
    return m > 0 ? m : 0;
}

static void clamp_scroll(Browser *b) {
    float m = max_scroll(b);
    if (b->scroll_y > m) b->scroll_y = m;
    if (b->scroll_y < 0) b->scroll_y = 0;
}

/* ---------------------------------------------------------- navigation ----- */
static void *loader_main(void *p) {
    LoadArg *a = p;
    Page    *pg = page_load(a->url, a->user_typed);
    LoadResult *r = xmalloc(sizeof *r);
    r->id = a->id;
    r->page = pg;
    if (write(a->b->notify_fd[1], &r, sizeof r) != (ssize_t)sizeof r) { page_free(pg); free(r); }
    free(a->url);
    free(a);
    return NULL;
}

static char *normalize_input(const char *in) {
    char *s = xstrdup(in);
    char *t = str_trim(s);
    char *out;
    if (!*t) { free(s); return xstrdup("about:blank"); }
    bool has_scheme = strstr(t, "://") || !strncmp(t, "about:", 6) || !strncmp(t, "data:", 5) || !strncmp(t, "file:", 5);
    bool looks_host = !strchr(t, ' ') && (strchr(t, '.') || !strncmp(t, "localhost", 9)) ;
    if (has_scheme) out = xstrdup(t);
    else if (t[0] == '/' && access(t, R_OK) == 0) {
        out = xmalloc(strlen(t) + 8);
        sprintf(out, "file://%s", t);
    } else if (looks_host) {
        out = xmalloc(strlen(t) + 16);
        bool local = !strncmp(t, "localhost", 9) || !strncmp(t, "127.", 4);
        sprintf(out, "%s://%s", local ? "http" : "https", t);
    } else {
        /* not a URL: web search through a privacy-friendly engine */
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

static void hist_push(Browser *b, const char *url) {
    for (int i = b->hpos + 1; i < b->nhist; i++) free(b->hist[i].url);
    b->nhist = b->hpos + 1;
    if (b->nhist == b->caphist) {
        b->caphist = b->caphist ? b->caphist * 2 : 16;
        b->hist = xrealloc(b->hist, sizeof(HistEntry) * (size_t)b->caphist);
    }
    b->hist[b->nhist].url = xstrdup(url);
    b->hist[b->nhist].scroll = 0;
    b->hpos = b->nhist++;
}

static char *strip_fragment(const char *u) {
    char *c = xstrdup(u);
    char *h = strchr(c, '#');
    if (h) *h = 0;
    return c;
}

static void scroll_to_fragment(Browser *b, const char *frag) {
    if (!b->page || !b->page->layout) return;
    if (!frag || !*frag) {
        b->scroll_y = 0;
    } else {
        char *id = xstrdup(frag);
        url_percent_decode(id);
        float y = layout_find_anchor(b->page->layout, id);
        free(id);
        if (y >= 0) b->scroll_y = y;
    }
    clamp_scroll(b);
}

void browser_navigate(Browser *b, const char *input, bool push, bool user_typed) {
    char *url = normalize_input(input);
    /* same-document fragment navigation */
    if (b->page && strchr(url, '#')) {
        char *a = strip_fragment(url), *c = strip_fragment(b->page->url);
        bool  same = !strcmp(a, c);
        free(a);
        free(c);
        if (same) {
            const char *frag = strchr(url, '#') + 1;
            if (b->hpos >= 0) b->hist[b->hpos].scroll = b->scroll_y;
            if (push) hist_push(b, url);
            else if (b->hpos >= 0) { free(b->hist[b->hpos].url); b->hist[b->hpos].url = xstrdup(url); }
            scroll_to_fragment(b, frag);
            set_addr(b, url);
            b->dirty = true;
            free(url);
            return;
        }
    }
    if (b->hpos >= 0 && b->hpos < b->nhist) b->hist[b->hpos].scroll = b->scroll_y;
    b->load_id++;
    b->loading = true;
    b->load_push = push;
    free(b->loading_url);
    b->loading_url = xstrdup(url);
    free(b->load_fragment);
    const char *h = strchr(url, '#');
    b->load_fragment = h ? xstrdup(h + 1) : NULL;
    set_addr(b, url);
    b->addr_focus = false;
    char msg[600];
    snprintf(msg, sizeof msg, "Carregando %s…", url);
    browser_set_status(b, msg);

    LoadArg *a = xmalloc(sizeof *a);
    a->b = b;
    a->id = b->load_id;
    a->url = url;
    a->user_typed = user_typed;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1 << 20);
    if (pthread_create(&t, &at, loader_main, a) != 0) {
        free(a->url);
        free(a);
        b->loading = false;
    }
    pthread_attr_destroy(&at);
    b->dirty = true;
}

static void apply_page(Browser *b, Page *pg) {
    page_free(b->page);
    b->page = pg;
    page_layout(pg, (float)b->w, view_h(b));
    b->scroll_y = b->pending_scroll;
    b->pending_scroll = 0;
    clamp_scroll(b);
    if (b->load_fragment && *b->load_fragment) scroll_to_fragment(b, b->load_fragment);
    set_addr(b, pg->url);
    if (b->load_fragment && *b->load_fragment) { /* keep the fragment visible in the address bar */
        Buf s;
        buf_init(&s);
        char *base = strip_fragment(pg->url);
        buf_printf(&s, "%s#%s", base, b->load_fragment);
        set_addr(b, s.data);
        free(base);
        buf_free(&s);
    }
    if (b->load_push) hist_push(b, b->addr);
    else if (b->hpos >= 0) { free(b->hist[b->hpos].url); b->hist[b->hpos].url = xstrdup(b->addr); }
    b->loading = false;
    browser_set_status(b, "");
    if (b->set_title) {
        Buf t;
        buf_init(&t);
        buf_printf(&t, "%s — BlackHole", pg->title && *pg->title ? pg->title : pg->url);
        b->set_title(b->ud, t.data);
        buf_free(&t);
    }
    b->dirty = true;
}

void browser_loader_ready(Browser *b) {
    LoadResult *r;
    while (read(b->notify_fd[0], &r, sizeof r) == (ssize_t)sizeof r) {
        if (r->id == b->load_id) {
            apply_page(b, r->page);
        } else {
            page_free(r->page); /* a newer navigation superseded this one */
        }
        free(r);
        /* notify_fd is blocking: only read once per wakeup unless more data is certainly there */
        break;
    }
}

static void go_history(Browser *b, int delta) {
    int np = b->hpos + delta;
    if (np < 0 || np >= b->nhist) return;
    if (b->hpos >= 0) b->hist[b->hpos].scroll = b->scroll_y;
    b->hpos = np;
    b->pending_scroll = b->hist[np].scroll;
    browser_navigate(b, b->hist[np].url, false, false);
}

void browser_resize(Browser *b, int w, int h) {
    if (w == b->w && h == b->h) return;
    b->w = w;
    b->h = h;
    canvas_resize(b->canvas, w, h);
    if (b->page) {
        page_layout(b->page, (float)w, view_h(b));
        clamp_scroll(b);
    }
    b->dirty = true;
}

/* ----------------------------------------------------------- drawing ------- */
static void draw_button(Browser *b, int x, const char *glyph, bool enabled) {
    BhFont *f = font_get(FONT_SANS, false, false, 18);
    canvas_fill_rect(b->canvas, (float)x, 8, 30, 30, 0xFF1F1F27);
    float w = font_text_width(f, glyph, strlen(glyph), 0);
    canvas_draw_text(b->canvas, f, (float)x + (30 - w) / 2, 29, glyph, strlen(glyph), enabled ? C_TEXT : 0xFF55555F, 0);
}

static void draw_padlock(Canvas *c, float x, float y, uint32_t col) {
    canvas_fill_rect(c, x + 1, y + 6, 10, 8, col);            /* body */
    canvas_fill_rect(c, x + 3, y + 1, 1, 6, col);             /* shackle */
    canvas_fill_rect(c, x + 8, y + 1, 1, 6, col);
    canvas_fill_rect(c, x + 3, y, 6, 1, col);
}

static void draw_chrome(Browser *b) {
    Canvas *c = b->canvas;
    canvas_fill_rect(c, 0, 0, (float)b->w, CHROME_H, C_CHROME);
    canvas_fill_rect(c, 0, CHROME_H - 1, (float)b->w, 1, 0xFF0B0B0E);
    draw_button(b, 8, "←", b->hpos > 0);
    draw_button(b, 44, "→", b->hpos >= 0 && b->hpos < b->nhist - 1);
    draw_button(b, 80, b->loading ? "×" : "↻", true);

    float fx = 120, fw = (float)b->w - fx - 12;
    if (fw < 40) return;
    canvas_fill_rect(c, fx, 8, fw, 30, b->addr_focus ? C_FIELD_FOCUS : C_FIELD);
    if (b->addr_focus) {
        canvas_fill_rect(c, fx, 37, fw, 1, C_ACCENT);
    }
    BhFont *f = font_get(FONT_SANS, false, false, 15);
    BhFont *sf = font_get(FONT_SANS, false, false, 12);
    float tx = fx + 10;
    /* security indicator */
    if (b->page && !b->addr_focus) {
        bool is_https = !strncmp(b->page->url, "https://", 8);
        bool is_file = !strncmp(b->page->url, "file://", 7) || !strncmp(b->page->url, "about:", 6);
        if (is_https) {
            draw_padlock(c, tx, 15, C_GOOD);
            tx += 20;
        } else if (!is_file) {
            const char *warn = "Não seguro";
            canvas_draw_text(c, sf, tx, 27, warn, strlen(warn), C_BAD, 0);
            tx += font_text_width(sf, warn, strlen(warn), 0) + 10;
        }
    }
    int old[4];
    canvas_set_clip(c, (int)tx, 8, (int)(fx + fw - tx - 6), 30, old);
    const char *txt = b->addr_len ? b->addr : "Pesquisar ou digitar endereço";
    uint32_t    col = b->addr_len ? C_TEXT : C_DIM;
    float       tw = font_text_width(f, txt, b->addr_len ? b->addr_len : strlen(txt), 0);
    float       shift = 0;
    if (b->addr_focus && b->addr_len) {
        float cw = font_text_width(f, b->addr, b->cursor, 0);
        float room = fx + fw - tx - 12;
        if (cw > room) shift = cw - room;
    }
    if (b->addr_focus && b->addr_select_all && b->addr_len) canvas_fill_rect(c, tx, 12, tw, 22, 0xFF4B3FA0);
    canvas_draw_text(c, f, tx - shift, 29, txt, b->addr_len ? b->addr_len : strlen(txt), col, 0);
    if (b->addr_focus && !b->addr_select_all) {
        float cw = font_text_width(f, b->addr, b->cursor, 0);
        canvas_fill_rect(c, tx - shift + cw, 13, 1, 20, C_TEXT);
    }
    canvas_restore_clip(c, old);
    if (b->loading) { /* indeterminate progress line */
        canvas_fill_rect(c, 0, CHROME_H - 3, (float)b->w, 2, 0xFF2A2442);
        canvas_fill_rect(c, 0, CHROME_H - 3, (float)b->w * 0.35f, 2, C_ACCENT);
    }
}

static void draw_status(Browser *b) {
    Canvas *c = b->canvas;
    if (!b->status[0]) return;
    BhFont *f = font_get(FONT_SANS, false, false, 12);
    float w = font_text_width(f, b->status, strlen(b->status), 0) + 16;
    if (w > (float)b->w * 0.8f) w = (float)b->w * 0.8f;
    float y = (float)(b->h - STATUS_H);
    canvas_fill_rect(c, 0, y, w, STATUS_H, C_STATUS);
    int old[4];
    canvas_set_clip(c, 0, (int)y, (int)w, STATUS_H, old);
    canvas_draw_text(c, f, 8, y + 15, b->status, strlen(b->status), C_TEXT, 0);
    canvas_restore_clip(c, old);
}

static void draw_scrollbar(Browser *b) {
    float m = max_scroll(b);
    if (m <= 0) return;
    float vh = view_h(b), dh = layout_doc_height(b->page->layout);
    float th = vh * vh / dh;
    if (th < 28) th = 28;
    float ty = CHROME_H + (vh - th) * (b->scroll_y / m);
    canvas_fill_rect(b->canvas, (float)b->w - 9, ty, 6, th, 0x99808090);
}

void browser_render(Browser *b) {
    Canvas *c = b->canvas;
    if (b->page && b->page->layout) layout_paint(b->page->layout, c, 0, CHROME_H, b->w, b->h - CHROME_H, b->scroll_y);
    else canvas_fill_rect(c, 0, CHROME_H, (float)b->w, (float)(b->h - CHROME_H), 0xFF101014);
    draw_scrollbar(b);
    draw_chrome(b);
    if (b->page && !b->loading_url && !b->page->layout) {}
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
        if ((unsigned char)*p < 0x20 || *p == 0x7F) return; /* control characters are not text */
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
    if (b->addr_focus) {
        switch (key) {
        case KEY_ENTER: {
            char *url = xstrdup(b->addr);
            browser_navigate(b, url, true, true);
            free(url);
            return;
        }
        case KEY_ESC:
            b->addr_focus = false;
            if (b->page) set_addr(b, b->hpos >= 0 ? b->hist[b->hpos].url : b->page->url);
            b->dirty = true;
            return;
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
    float step = 48, page = view_h(b) - 40;
    switch (key) {
    case KEY_DOWN: b->scroll_y += step; break;
    case KEY_UP: b->scroll_y -= step; break;
    case KEY_PGDN: b->scroll_y += page; break;
    case KEY_SPACE: b->scroll_y += (mods & MOD_SHIFT) ? -page : page; break;
    case KEY_PGUP: b->scroll_y -= page; break;
    case KEY_HOME: b->scroll_y = 0; break;
    case KEY_END: b->scroll_y = max_scroll(b); break;
    default: return;
    }
    clamp_scroll(b);
    b->dirty = true;
}

/* Handles shortcuts that arrive as (ctrl + letter). Returns true if consumed. */
bool browser_shortcut(Browser *b, char letter, int mods);
bool browser_shortcut(Browser *b, char letter, int mods) {
    if (!(mods & MOD_CTRL)) return false;
    switch (letter) {
    case 'l': focus_addr(b); return true;
    case 'r': if (b->page) browser_navigate(b, b->page->url, false, false); return true;
    case 'a': if (b->addr_focus) { b->addr_select_all = b->addr_len > 0; b->dirty = true; } return true;
    case 'c':
        if (b->addr_focus && b->clipboard_set) b->clipboard_set(b->ud, b->addr);
        return true;
    case 'v':
        if (b->addr_focus && b->clipboard_request) b->clipboard_request(b->ud);
        return true;
    case 'w': case 'q': b->quit = true; return true;
    default: return false;
    }
}

static void update_hover(Browser *b, int x, int y) {
    const Node *link = NULL;
    int         ck = CURSOR_ARROW;
    if (y >= 8 && y < 38 && x >= 120) ck = CURSOR_TEXT;
    else if (y >= CHROME_H && b->page && b->page->layout) {
        HitResult hr;
        if (layout_hit_test(b->page->layout, (float)x, (float)(y - CHROME_H) + b->scroll_y, &hr)) {
            link = hr.link;
            if (link) ck = CURSOR_HAND;
        }
    } else if (y < CHROME_H && x < 112) ck = CURSOR_HAND;
    if (link != b->hover_link) {
        b->hover_link = link;
        if (link && b->page) {
            char *abs = url_resolve(b->page->url, dom_attr(link, "href"));
            browser_set_status(b, abs ? abs : dom_attr(link, "href"));
            free(abs);
        } else if (!b->loading) browser_set_status(b, "");
    }
    if (ck != b->cursor_kind) {
        b->cursor_kind = ck;
        if (b->set_cursor) b->set_cursor(b->ud, ck);
    }
}

void browser_mouse_move(Browser *b, int x, int y) { update_hover(b, x, y); }

void browser_mouse_down(Browser *b, int x, int y, int button) {
    if (button != 1) return;
    if (y < CHROME_H) {
        if (x >= 8 && x < 38) go_history(b, -1);
        else if (x >= 44 && x < 74) go_history(b, +1);
        else if (x >= 80 && x < 110) {
            if (b->loading) { b->load_id++; b->loading = false; browser_set_status(b, ""); b->dirty = true; }
            else if (b->page) browser_navigate(b, b->page->url, false, false);
        } else if (x >= 120 && y >= 8 && y < 38) {
            if (!b->addr_focus) focus_addr(b);
        }
        return;
    }
    if (b->addr_focus) {
        b->addr_focus = false;
        b->dirty = true;
    }
    if (!b->page || !b->page->layout) return;
    HitResult hr;
    if (layout_hit_test(b->page->layout, (float)x, (float)(y - CHROME_H) + b->scroll_y, &hr) && hr.link) {
        const char *href = dom_attr(hr.link, "href");
        if (!href) return;
        while (is_space((unsigned char)*href)) href++;
        if (!strncasecmp(href, "javascript:", 11)) { browser_set_status(b, "JavaScript ainda não é suportado"); return; }
        char *abs = url_resolve(b->page->url, href);
        if (!abs) return;
        bool from_file = !strncmp(b->page->url, "file:", 5);
        bool to_file = !strncmp(abs, "file:", 5);
        if (to_file && !from_file) { /* web pages must never reach local files */
            browser_set_status(b, "Bloqueado: página web tentando abrir arquivo local");
            free(abs);
            return;
        }
        browser_navigate(b, abs, true, from_file);
        free(abs);
    }
}

void browser_wheel(Browser *b, int x, int y, float dy) {
    (void)x;
    (void)y;
    b->scroll_y += dy;
    clamp_scroll(b);
    b->dirty = true;
}
