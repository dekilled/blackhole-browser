#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"
#include "dom.h"
#include "gfx.h"
#include "img.h"
#include "html.h"
#include "layout.h"
#include "net.h"
#include "browser.h"
#include "page.h"
#include "ui.h"
#include "util.h"

static void dump_dom(const Node *root) {
    const Node *n = root;
    int         depth = 0;
    for (;;) {
        for (int i = 0; i < depth; i++) fputs("  ", stdout);
        if (n->type == NODE_ELEMENT) {
            printf("<%s", n->tag);
            for (const Attr *a = n->attrs; a; a = a->next) printf(" %s=\"%s\"", a->name, a->value);
            printf(">\n");
        } else if (n->type == NODE_TEXT) printf("\"%s\"\n", n->text);
        else if (n->type == NODE_COMMENT) printf("<!--%s-->\n", n->text);
        else printf("#document\n");
        if (n->first_child) { n = n->first_child; depth++; continue; }
        while (n != root && !n->next) { n = n->parent; depth--; }
        if (n == root) break;
        n = n->next;
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
            "BlackHole Browser 0.1\n"
            "uso: %s [opcoes] [URL]\n"
            "  --screenshot ARQ.png   renderiza a pagina sem abrir janela e salva um PNG\n"
            "  --size LxA             tamanho da viewport (padrao 1024x768)\n"
            "  --scroll N             rolagem vertical (px) para o screenshot\n"
            "  --full                 screenshot da pagina inteira (limitado a 16000px)\n"
            "  --chrome               screenshot inclui a interface (barra de endereco etc.)\n"
            "  --click X,Y            simula um clique (implica --chrome); pode repetir\n"
            "  --wheel N              simula rolagem do mouse em N pixels (implica --chrome)\n"
            "  --dom                  imprime a arvore DOM e sai\n"
            "  --fetch                imprime o corpo da resposta HTTP e sai\n"
            "  -v                     log detalhado\n",
            prog);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, "en_US.UTF-8");

    if (argc == 4 && !strcmp(argv[1], "--decode-image")) { /* test helper: decode and write a PPM over white */
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 1;
        Buf raw;
        buf_init(&raw);
        char tmp[65536];
        size_t k;
        while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) buf_append(&raw, tmp, k);
        fclose(f);
        Image im;
        if (!image_decode((const uint8_t *)raw.data, raw.len, &im)) { fprintf(stderr, "decode falhou\n"); return 1; }
        FILE *o = fopen(argv[3], "wb");
        fprintf(o, "P6\n%d %d\n255\n", im.w, im.h);
        for (int i = 0; i < im.w * im.h; i++) {
            uint32_t p = im.px[i], a = p >> 24;
            uint8_t rgb[3];
            for (int c = 0; c < 3; c++) {
                uint32_t v = (p >> (16 - 8 * c)) & 255;
                rgb[c] = (uint8_t)((v * a + 255 * (255 - a) + 127) / 255);
            }
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
        image_free(&im);
        buf_free(&raw);
        return 0;
    }
    const char *shot = NULL, *url = NULL;
    int         W = 1024, H = 768;
    float       scroll = 0;
    bool        mode_dom = false, mode_fetch = false, full = false, chrome = false;
    int         clicks[8][2], nclicks = 0;
    float       wheel = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &W, &H) != 2 || W < 100 || H < 100 || W > 8000 || H > 8000) {
                fprintf(stderr, "tamanho invalido\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) scroll = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--dom")) mode_dom = true;
        else if (!strcmp(argv[i], "--fetch")) mode_fetch = true;
        else if (!strcmp(argv[i], "--full")) full = true;
        else if (!strcmp(argv[i], "--chrome")) chrome = true;
        else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            if (nclicks < 8 && sscanf(argv[++i], "%d,%d", &clicks[nclicks][0], &clicks[nclicks][1]) == 2) nclicks++;
            chrome = true;
        } else if (!strcmp(argv[i], "--wheel") && i + 1 < argc) { wheel = (float)atof(argv[++i]); chrome = true; }
        else if (!strcmp(argv[i], "-v")) bh_verbose = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (argv[i][0] != '-') url = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!url) url = "about:blank";

    /* bare paths become file: URLs */
    char *owned = NULL;
    if (!strstr(url, "://") && strncmp(url, "data:", 5) && strncmp(url, "about:", 6)) {
        char *abs = realpath(url, NULL);
        if (abs) {
            owned = xmalloc(strlen(abs) + 8);
            sprintf(owned, "file://%s", abs);
            free(abs);
            url = owned;
        }
    }

    if (mode_fetch || mode_dom) {
        FetchOpts o = net_default_opts();
        o.allow_file = true;
        Response r;
        if (!net_fetch(url, &o, &r)) {
            fprintf(stderr, "falha: %s\n", r.error);
            return 1;
        }
        if (mode_fetch) {
            fprintf(stderr, "status=%d url=%s tipo=%s charset=%s bytes=%zu seguro=%d\n", r.status, r.url,
                    r.content_type ? r.content_type : "-", r.charset ? r.charset : "-", r.body_len, r.secure);
            fwrite(r.body, 1, r.body_len, stdout);
        } else {
            size_t n;
            char  *u8 = charset_to_utf8(r.body, r.body_len, r.charset, &n);
            Arena  a = {0};
            dump_dom(html_parse(&a, u8, n));
            arena_free(&a);
            free(u8);
        }
        response_free(&r);
        return 0;
    }

    if (!font_available()) {
        fprintf(stderr, "nenhuma fonte encontrada (instale fonts-liberation ou fonts-dejavu)\n");
        return 1;
    }
    if (!shot) { /* interactive window */
        Browser *b = browser_new(1100, 760);
        int rc = ui_x11_run(b, url[0] && strcmp(url, "about:blank") ? url : NULL);
        browser_free(b);
        free(owned);
        return rc;
    }
    if (chrome) { /* headless run of the whole browser UI */
        Browser *b = browser_new(W, H);
        browser_navigate(b, url, true, true);
        for (int step = 0; step <= nclicks; step++) {
            while (b->loading) {
                struct pollfd pf = {b->notify_fd[0], POLLIN, 0};
                if (poll(&pf, 1, 30000) <= 0) break;
                browser_loader_ready(b);
            }
            if (step < nclicks) {
                browser_mouse_move(b, clicks[step][0], clicks[step][1]);
                browser_mouse_down(b, clicks[step][0], clicks[step][1], 1);
            }
        }
        if (wheel != 0) browser_wheel(b, 0, 0, wheel);
        browser_render(b);
        bool ok = canvas_write_png(b->canvas, shot);
        fprintf(stderr, "%s: %s (%dx%d) url=%s\n", ok ? "ok" : "ERRO", shot, W, H, b->page ? b->page->url : "-");
        browser_free(b);
        free(owned);
        return ok ? 0 : 1;
    }
    Page *p = page_load(url, true);
    page_layout(p, (float)W, (float)H);
    bh_log("titulo: %s", p->title ? p->title : "(sem titulo)");
    {
        int CH = H;
        if (full) {
            CH = (int)layout_doc_height(p->layout) + 1;
            if (CH > 16000) CH = 16000;
            if (CH < H) CH = H;
        }
        Canvas *c = canvas_new(W, CH);
        layout_paint(p->layout, c, 0, 0, W, CH, scroll);
        if (!canvas_write_png(c, shot)) {
            fprintf(stderr, "nao consegui escrever %s\n", shot);
            return 1;
        }
        fprintf(stderr, "ok: %s (%dx%d) titulo=\"%s\" altura_doc=%.0f\n", shot, W, CH, p->title ? p->title : "", layout_doc_height(p->layout));
        canvas_free(c);
    }
    page_free(p);
    free(owned);
    return 0;
}
