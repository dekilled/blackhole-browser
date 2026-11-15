/* Built-in unit tests: run with `blackhole --selftest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css.h"
#include "dom.h"
#include "html.h"
#include "layout.h"
#include "page.h"
#include "style.h"
#include "url.h"
#include "util.h"

static int g_fail, g_total;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_total++;                                                                       \
        if (!(cond)) { g_fail++; printf("  FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static void check_str(const char *what, const char *got, const char *want) {
    g_total++;
    if (!got || strcmp(got, want) != 0) {
        g_fail++;
        printf("  FALHOU %s: obtido '%s', esperado '%s'\n", what, got ? got : "(null)", want);
    }
}

static void test_urls(void) {
    const char *base = "http://a/b/c/d;p?q";
    static const char *cases[][2] = {
        {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"}, {"/g", "http://a/g"},
        {"//g", "http://g/"}, {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"}, {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"}, {"../g", "http://a/b/g"}, {"../../g", "http://a/g"}, {"../../../g", "http://a/g"},
        {"./g/.", "http://a/b/c/g/"}, {"g/../h", "http://a/b/c/h"}, {"https://x.org/p", "https://x.org/p"}, {"", "http://a/b/c/d;p?q"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *r = url_resolve(base, cases[i][0]);
        check_str(cases[i][0], r, cases[i][1]);
        free(r);
    }
    Url u;
    CHECK(url_parse("HTTP://EXAMPLE.com:80/a/./b/../c", &u));
    char *s = url_to_string(&u);
    check_str("normalizacao", s, "http://example.com/a/c");
    free(s);
    url_free(&u);
    CHECK(url_parse("http://user:pw@host:8080/x y", &u));
    s = url_to_string(&u);
    check_str("userinfo removido + espaco codificado", s, "http://host:8080/x%20y");
    free(s);
    url_free(&u);
    CHECK(!url_parse("http://", &u));
    CHECK(!url_parse("http://exa mple.com/", &u));
    CHECK(!url_parse("notaurl", &u));
    CHECK(!url_parse("http://h:99999/", &u));
}

static void test_colors(void) {
    uint32_t c;
    CHECK(style_parse_color("#fff", &c) && c == 0xFFFFFFFFu);
    CHECK(style_parse_color("#11223344", &c) && c == 0x44112233u);
    CHECK(style_parse_color("rgb(1, 2, 3)", &c) && c == 0xFF010203u);
    CHECK(style_parse_color("rgba(255 0 0 / 50%)", &c) && (c >> 24) >= 127 && (c >> 24) <= 128 && (c & 0xFFFFFF) == 0xFF0000);
    CHECK(style_parse_color("hsl(120, 100%, 50%)", &c) && c == 0xFF00FF00u);
    CHECK(style_parse_color("rebeccapurple", &c) && c == 0xFF663399u);
    CHECK(style_parse_color("transparent", &c) && c == 0);
    CHECK(!style_parse_color("#12", &c));
    CHECK(!style_parse_color("notacolor", &c));
}

static void test_entities(void) {
    Buf b;
    buf_init(&b);
    const char *in = "&amp;&lt;&#65;&#x42;&eacute;&bogus;&#0;&copy";
    html_decode_entities(&b, in, strlen(in), false);
    check_str("entidades", b.data, "&<AB\xC3\xA9&bogus;\xEF\xBF\xBD\xC2\xA9");
    buf_free(&b);
}

static Node *find_id(Node *root, const char *id) {
    for (Node *n = root->first_child; n;) {
        const char *v = n->type == NODE_ELEMENT ? dom_attr(n, "id") : NULL;
        if (v && !strcmp(v, id)) return n;
        if (n->first_child) { n = n->first_child; continue; }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) return NULL;
        n = n->next;
    }
    return NULL;
}

static void test_cascade(void) {
    Page *p = page_from_html("about:test",
        "<style>:root{--c:#00f;--w:40px} p.a > b{color:red} #x{color:var(--c)} .imp{color:green!important}"
        " #w{width:calc(100% - var(--w))} div{font-size:20px} span{font-size:1.5em;margin-left:2em} i{font-weight:bold}"
        " @media (max-width: 500px){ #m{color:#010203} } @media (min-width: 501px){ #m{color:#040506} }</style>"
        "<p class=a><b id=x class=imp style='color:purple'>t</b></p><div id=w><span id=s>x</span></div><p><i id=i>q</i></p><b id=m>m</b>");
    page_layout(p, 600, 400);
    Node *x = find_id(p->doc, "x"), *w = find_id(p->doc, "w"), *s = find_id(p->doc, "s"), *i = find_id(p->doc, "i"), *m = find_id(p->doc, "m");
    CHECK(x && x->style && x->style->color == 0xFF008000u); /* !important author beats inline */
    CHECK(w && w->style && w->style->width.pct > 0.99f && w->style->width.px < -39.9f && w->style->width.px > -40.1f);
    CHECK(s && s->style && s->style->font_size == 30 && s->style->margin[3].px == 60); /* font 1.5em of 20px = 30; margin 2em uses the element own size */
    CHECK(i && i->style && i->style->font_weight >= 700);
    CHECK(m && m->style && m->style->color == 0xFF040506u); /* viewport 600 > 500 */
    page_layout(p, 400, 400);
    CHECK(m->style->color == 0xFF010203u);
    page_free(p);
}

static void test_layout(void) {
    Page *p = page_from_html("about:test",
        "<style>body{margin:0}div{height:10px}</style>"
        "<div id=a style='margin:10px 0'></div><div id=b style='margin:20px 0'></div>"
        "<div id=c style='margin-top:5px'><div id=d style='margin-top:30px'></div></div>"
        "<div style='width:300px;margin:0 auto' id=e></div>"
        "<div style='display:flex;gap:10px;width:310px'><i id=f style='flex:1'>a</i><i id=g style='flex:2'>b</i></div>");
    page_layout(p, 800, 600);
    float ya = layout_find_anchor(p->layout, "a"), yb = layout_find_anchor(p->layout, "b"), yc = layout_find_anchor(p->layout, "c");
    float yd = layout_find_anchor(p->layout, "d");
    CHECK(ya == 10);          /* top margin of first block */
    CHECK(yb - ya == 10 + 20); /* adjacent margins collapse: max(10,20) = 20 */
    CHECK(yd == yc);           /* parent/child top margins collapse (child sits at the parent's top) */
    CHECK(yc - yb == 10 + 30); /* ...and the collapsed margin is max(5,30) = 30 */
    page_free(p);
}

static void test_float_and_pseudo(void) {
    Page *p = page_from_html("about:test",
        "<style>body{margin:0}p{margin:0}b::before{content:\"[\"}b::after{content:\"]\"}"
        "#f{float:left;width:100px;height:50px}#r{float:right;width:100px;height:50px}"
        "#c{clear:both;height:5px}</style>"
        "<div id=f></div><div id=r></div><p id=p>texto ao lado</p><div id=c></div><b id=b>x</b>");
    page_layout(p, 400, 300);
    float yf = layout_find_anchor(p->layout, "f"), yp = layout_find_anchor(p->layout, "p"), yc = layout_find_anchor(p->layout, "c");
    CHECK(yf == 0 && yp == 0);    /* the paragraph flows beside the floats, it does not go below them */
    CHECK(yc >= 50);              /* clear:both moves below both floats */
    Node *b = find_id(p->doc, "b");
    CHECK(b && b->first_child && (b->first_child->flags & NODE_PSEUDO) && b->last_child && (b->last_child->flags & NODE_PSEUDO));
    char *txt = b ? dom_text_content(b) : xstrdup("");
    check_str("textContent ignora pseudo-elementos", txt, "x");
    free(txt);
    page_layout(p, 500, 300); /* relayout must not duplicate generated nodes */
    int n = 0;
    for (Node *c = b->first_child; c; c = c->next) n++;
    CHECK(n == 3);
    page_free(p);
}

static void test_parser_robustness(void) {
    Arena a = {0};
    Node *d = html_parse(&a, "<p>um<p>dois<ul><li>a<li>b</ul><table><td>x</table><b><i>mal</b></i>", 0);
    CHECK(d != NULL);
    arena_free(&a);
    static const char *junk[] = {"<", "<<<<", "<a", "<a href='x", "&", "&#", "<!--", "<![CDATA[", "<style>", "</", "<\xff\xfe>", ""};
    for (size_t i = 0; i < sizeof junk / sizeof *junk; i++) {
        Arena b = {0};
        CHECK(html_parse(&b, junk[i], strlen(junk[i])) != NULL);
        arena_free(&b);
    }
}

int selftest_run(void) {
    g_fail = g_total = 0;
    test_urls();
    test_colors();
    test_entities();
    test_cascade();
    test_layout();
    test_float_and_pseudo();
    test_parser_robustness();
    printf("selftest: %d verificacoes, %d falhas\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
