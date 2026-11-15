/* `blackhole --diagnose URL`: explains why a page may render badly (what the engine ignored). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "page.h"
#include "style.h"
#include "url.h"

static bool hidden(const Node *n) {
    for (; n; n = n->parent)
        if (n->type == NODE_ELEMENT && n->style && n->style->display == DISP_NONE) return true;
    return false;
}

int diagnose_run(const char *url, int W, int H) {
    diag_on = true;
    Page *p = page_load(url, true);
    page_layout(p, (float)W, (float)H);

    bool empty_shell = false;
    int elements = 0, scripts_in = 0, scripts_ext = 0, imgs = 0, iframes = 0, videos = 0, svgs = 0, canvases = 0, custom = 0, forms = 0, links = 0, noscript = 0;
    size_t inline_js = 0, text_chars = 0, noscript_chars = 0;
    char sample[260] = "";
    size_t sn = 0;
    Node *n = p->doc->first_child;
    while (n) {
        if (n->type == NODE_ELEMENT) {
            elements++;
            const char *t = n->tag;
            if (!strcmp(t, "script")) {
                if (dom_attr(n, "src")) scripts_ext++;
                else {
                    scripts_in++;
                    for (Node *c = n->first_child; c; c = c->next) if (c->type == NODE_TEXT) inline_js += strlen(c->text);
                }
            } else if (!strcmp(t, "img")) imgs++;
            else if (!strcmp(t, "iframe")) iframes++;
            else if (!strcmp(t, "video")) videos++;
            else if (!strcmp(t, "svg")) svgs++;
            else if (!strcmp(t, "canvas")) canvases++;
            else if (!strcmp(t, "form")) forms++;
            else if (!strcmp(t, "a")) links++;
            else if (!strcmp(t, "noscript")) noscript++;
            if (strchr(t, '-')) custom++;
            const char *id = dom_attr(n, "id");
            if (id && (!strcmp(id, "root") || !strcmp(id, "app") || !strcmp(id, "__next") || !strcmp(id, "__nuxt") || !strcmp(id, "app-root"))) {
                char *tc = dom_text_content(n);
                bool blank = true;
                for (char *c = tc; *c; c++) if (!is_space((unsigned char)*c)) blank = false;
                if (blank) empty_shell = true;
                free(tc);
            }
        } else if (n->type == NODE_TEXT && !hidden(n)) {
            bool in_script = n->parent && (dom_is(n->parent, "script") || dom_is(n->parent, "style"));
            bool in_noscript = false;
            for (Node *a = n->parent; a; a = a->parent) if (dom_is(a, "noscript")) in_noscript = true;
            if (!in_script) {
                for (const char *c = n->text; *c; c++) {
                    if (is_space((unsigned char)*c)) continue;
                    if (in_noscript) noscript_chars++; else text_chars++;
                    if (!in_noscript && sn < sizeof sample - 2) sample[sn++] = *c;
                }
                if (!in_noscript && sn < sizeof sample - 2) sample[sn++] = ' ';
            }
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && n != p->doc && !n->next) n = n->parent;
        if (!n || n == p->doc) break;
        n = n->next;
    }
    sample[sn] = 0;
    int rules = 0;
    for (int i = 1; i < p->nsheets; i++) rules += p->sheets[i]->nrules;
    int img_ok = 0, img_bad = 0;
    for (PageImage *pi = p->images; pi; pi = pi->next) pi->failed ? img_bad++ : img_ok++;

    printf("== DIAGNÓSTICO: %s\n", p->url);
    printf("status HTTP ...... %d   %s\n", p->status, p->secure ? "(HTTPS verificado)" : "(sem TLS)");
    printf("título ........... %s\n", p->title ? p->title : "(nenhum)");
    printf("HTML ............. %zu bytes, %d elementos, %d links, %d formulários\n", p->html_bytes, elements, links, forms);
    printf("scripts .......... %d externos, %d inline (%zu bytes de JS inline)  <- o motor NÃO executa JavaScript\n", scripts_ext, scripts_in, inline_js);
    printf("CSS .............. %d folhas carregadas, %d regras válidas\n", p->nsheets - 1, rules);
    printf("imagens .......... %d <img>: %d decodificadas, %d falharam/não suportadas (WebP/AVIF/SVG não existem ainda)\n", imgs, img_ok, img_bad);
    printf("outros ........... %d iframes, %d vídeos, %d SVG inline, %d canvas, %d custom elements (web components)\n", iframes, videos, svgs, canvases, custom);
    printf("texto visível .... %zu caracteres (+%zu dentro de <noscript>)\n", text_chars, noscript_chars);
    printf("layout ........... documento %.0f x %.0f px na viewport %dx%d\n", layout_doc_width(p->layout), layout_doc_height(p->layout), W, H);
    printf("amostra do texto . %.200s\n", sample);

    printf("\n== VEREDITO\n");
    int  nscripts = scripts_ext + scripts_in;
    bool strong = nscripts > 0 && text_chars < 50 && (empty_shell || custom > 0 || text_chars == 0);
    bool weak = !strong && text_chars < 400 && nscripts >= 3;
    if (p->status >= 400 || !p->status) printf("* A página não carregou como esperado (HTTP %d). Veja o título/amostra acima.\n", p->status);
    if (p->truncated) printf("* A conexão caiu antes do fim do HTML: a página foi renderizada com o que chegou.\n");
    if (strong) printf("* O HTML não tem conteúdo (%zu caracteres) e há %d scripts%s: a página é MONTADA POR JAVASCRIPT.\n  Sem motor JS ela fica vazia, não importa quão bom seja o CSS. (Este é o caso de YouTube, Netflix logada etc.)\n", text_chars, nscripts, empty_shell ? " + um contêiner raiz vazio (#root/#app)" : "");
    else if (weak) printf("* Pouco texto (%zu caracteres) com %d scripts: ou a página é minimalista (como a home do Google) ou depende de JS.\n  Abra o diagnose.png e compare com o que o site mostra num navegador normal.\n", text_chars, nscripts);
    if (noscript_chars > text_chars && text_chars < 200) printf("* Há mais texto dentro de <noscript> do que fora: o site pede JavaScript.\n");
    if (!strong && !weak && text_chars >= 400) printf("* Há conteúdo no HTML. Se a aparência está errada, o problema é CSS/layout (veja a lista abaixo).\n");
    printf("\n== O QUE O MOTOR IGNOROU OU APROXIMOU (ocorrências x nome)");
    diag_report(stdout);
    printf("\n");
    if (p->status && W > 0) {
        Canvas *c = canvas_new(W, H);
        layout_paint(p->layout, c, 0, 0, W, H, 0);
        canvas_write_png(c, "diagnose.png");
        canvas_free(c);
        printf("(screenshot salvo em ./diagnose.png)\n");
    }
    page_free(p);
    return 0;
}
