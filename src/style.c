#include "style.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ UA sheet == */
static const char UA_CSS[] =
    "html,address,blockquote,body,center,dialog,div,figure,figcaption,footer,form,header,hr,legend,listing,main,"
    "p,plaintext,pre,search,xmp,article,aside,h1,h2,h3,h4,h5,h6,hgroup,nav,section,dir,dd,dl,dt,menu,ol,ul,"
    "details,summary,fieldset,optgroup{display:block}"
    "li{display:list-item}"
    "head,link,meta,script,style,title,base,template,area,datalist,noembed,noframes,param,rp,[hidden]{display:none}"
    "table{display:table;border-spacing:2px;border-collapse:separate}tr{display:table-row}td,th{display:table-cell;padding:1px}"
    "thead,tbody,tfoot{display:table-row-group}caption{display:table-caption;text-align:center}"
    "col,colgroup{display:none}"
    "body{margin:8px}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:1em 0;font-weight:bold}h4{margin:1.33em 0;font-weight:bold}"
    "h5{font-size:.83em;margin:1.67em 0;font-weight:bold}h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
    "p,dl,menu,pre,fieldset,figure{margin:1em 0}blockquote{margin:1em 40px}"
    "ul,ol,menu,dir{margin:1em 0;padding-left:40px}ul{list-style-type:disc}ol{list-style-type:decimal}"
    "ul ul,ol ul{list-style-type:circle}ul ul ul,ol ul ul,ul ol ul,ol ol ul{list-style-type:square}"
    "ul ul,ul ol,ol ul,ol ol{margin:0}dd{margin-left:40px}"
    "b,strong,th{font-weight:bold}th{text-align:center}"
    "i,em,cite,var,dfn,address{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a:link{color:#0000ee;text-decoration:underline}"
    "code,kbd,samp,tt,pre,listing,xmp{font-family:monospace}pre,listing,xmp{white-space:pre}"
    "small{font-size:.83em}big{font-size:1.17em}sub,sup{font-size:.83em}sub{vertical-align:sub}sup{vertical-align:super}"
    "hr{border:1px solid #9a9a9a;margin:.5em 0;height:0}"
    "mark{background:#ffff00;color:#000}center{text-align:center}"
    "button,input,select,textarea{display:inline-block;font-size:13.33px;padding:1px 6px;"
    "border:1px solid #767676;background:#efefef;color:#000}"
    "input[type=checkbox],input[type=radio]{padding:0}input[type=hidden]{display:none}"
    "textarea{white-space:pre-wrap;background:#fff}input[type=text],input[type=search],input[type=password],"
    "input[type=email],input[type=url],input[type=number],input:not([type]){background:#fff}"
    "img{display:inline-block}"
    "summary{display:block}iframe,object,embed,video,audio,canvas,svg{display:inline-block}"
    "br{display:inline}";

static Stylesheet   *g_ua;
static pthread_once_t g_ua_once = PTHREAD_ONCE_INIT;
static void           ua_init(void) { g_ua = css_parse(UA_CSS, sizeof UA_CSS - 1, 0); }
Stylesheet           *style_ua_sheet(void) {
    pthread_once(&g_ua_once, ua_init);
    return g_ua;
}

/* ================================================================== colors == */
static const struct {
    const char *name;
    uint32_t    rgb;
} named_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6},
    {"olive", 0x808000}, {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB},
    {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080},
    {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
};

static uint32_t pack_argb(float r, float g, float b, float a) {
    #define CL(x) ((x) < 0 ? 0 : (x) > 255 ? 255 : (uint32_t)((x) + 0.5f))
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return (CL(a * 255) << 24) | (CL(r) << 16) | (CL(g) << 8) | CL(b);
    #undef CL
}

static float hue_to_rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.f / 6) return p + (q - p) * 6 * t;
    if (t < 1.f / 2) return q;
    if (t < 2.f / 3) return p + (q - p) * (2.f / 3 - t) * 6;
    return p;
}

/* Parses the numeric arguments of rgb()/hsl(): separators may be commas, spaces or '/'. */
static int parse_func_args(const char *s, float *vals, bool *pct, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',' || *s == '/' || *s == '\t') s++;
        if (!*s || *s == ')') break;
        char  *e;
        float v = strtof(s, &e);
        if (e == s) {
            if (!strncasecmp(s, "none", 4)) { v = 0; e = (char *)s + 4; }
            else return -1;
        }
        pct[n] = false;
        if (*e == '%') { pct[n] = true; e++; }
        else if (!strncasecmp(e, "deg", 3)) e += 3;
        else if (!strncasecmp(e, "turn", 4)) { v *= 360; e += 4; }
        else if (!strncasecmp(e, "rad", 3)) { v *= 57.29578f; e += 3; }
        vals[n++] = v;
        s = e;
    }
    return n;
}

bool style_parse_color(const char *str, uint32_t *out) {
    while (is_space((unsigned char)*str)) str++;
    if (*str == '#') {
        const char *h = str + 1;
        size_t      n = 0;
        while (hex_val((unsigned char)h[n]) >= 0) n++;
        if (h[n] && !is_space((unsigned char)h[n])) return false;
        uint32_t r, g, b, a = 255;
        if (n == 3 || n == 4) {
            r = (uint32_t)hex_val((unsigned char)h[0]) * 17;
            g = (uint32_t)hex_val((unsigned char)h[1]) * 17;
            b = (uint32_t)hex_val((unsigned char)h[2]) * 17;
            if (n == 4) a = (uint32_t)hex_val((unsigned char)h[3]) * 17;
        } else if (n == 6 || n == 8) {
            r = (uint32_t)(hex_val((unsigned char)h[0]) * 16 + hex_val((unsigned char)h[1]));
            g = (uint32_t)(hex_val((unsigned char)h[2]) * 16 + hex_val((unsigned char)h[3]));
            b = (uint32_t)(hex_val((unsigned char)h[4]) * 16 + hex_val((unsigned char)h[5]));
            if (n == 8) a = (uint32_t)(hex_val((unsigned char)h[6]) * 16 + hex_val((unsigned char)h[7]));
        } else return false;
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        return true;
    }
    bool is_rgb = !strncasecmp(str, "rgb", 3), is_hsl = !strncasecmp(str, "hsl", 3);
    if ((is_rgb || is_hsl) && (str[3] == '(' || (to_lower((unsigned char)str[3]) == 'a' && str[4] == '('))) {
        const char *p = strchr(str, '(') + 1;
        float       v[4] = {0, 0, 0, 1};
        bool        pc[4] = {false};
        int         n = parse_func_args(p, v, pc, 4);
        if (n < 3) return false;
        float a = n == 4 ? (pc[3] ? v[3] / 100 : v[3]) : 1;
        if (is_rgb) {
            float r = pc[0] ? v[0] * 2.55f : v[0], g = pc[1] ? v[1] * 2.55f : v[1], b = pc[2] ? v[2] * 2.55f : v[2];
            *out = pack_argb(r, g, b, a);
        } else {
            float h = fmodf(v[0], 360);
            if (h < 0) h += 360;
            h /= 360;
            float s = v[1] / 100, l = v[2] / 100;
            if (s < 0) s = 0;
            if (s > 1) s = 1;
            if (l < 0) l = 0;
            if (l > 1) l = 1;
            float r, g, b;
            if (s == 0) r = g = b = l;
            else {
                float q = l < 0.5f ? l * (1 + s) : l + s - l * s, pp = 2 * l - q;
                r = hue_to_rgb(pp, q, h + 1.f / 3);
                g = hue_to_rgb(pp, q, h);
                b = hue_to_rgb(pp, q, h - 1.f / 3);
            }
            *out = pack_argb(r * 255, g * 255, b * 255, a);
        }
        return true;
    }
    char   name[32];
    size_t n = 0;
    while (str[n] && !is_space((unsigned char)str[n]) && n < sizeof name - 1) {
        name[n] = (char)to_lower((unsigned char)str[n]);
        n++;
    }
    name[n] = 0;
    if (!strcmp(name, "transparent")) { *out = 0; return true; }
    for (size_t i = 0; i < sizeof named_colors / sizeof *named_colors; i++)
        if (!strcmp(named_colors[i].name, name)) {
            *out = 0xFF000000u | named_colors[i].rgb;
            return true;
        }
    return false;
}

/* ================================================================ lengths ==== */
typedef struct {
    const char *s;
    float       em, rem;
    Viewport    vp;
} CalcCtx;

typedef struct {
    float px, pct;
    bool  unitless;
} Calc;

static void csp(CalcCtx *c) { while (is_space((unsigned char)*c->s)) c->s++; }
static bool calc_expr(CalcCtx *c, Calc *out, int depth);

static bool calc_number(CalcCtx *c, Calc *o) {
    char  *e;
    float v = strtof(c->s, &e);
    if (e == c->s) return false;
    c->s = e;
    o->px = o->pct = 0;
    o->unitless = false;
    const char *u = c->s;
    size_t      ul = 0;
    while (is_alpha((unsigned char)u[ul])) ul++;
    if (*u == '%') {
        o->pct = v / 100;
        c->s++;
        return true;
    }
    if (ul == 0) {
        o->px = v;
        o->unitless = true;
        return true;
    }
    #define U(name) (ul == strlen(name) && !strncasecmp(u, name, ul))
    float mul;
    if (U("px")) mul = 1;
    else if (U("em")) mul = c->em;
    else if (U("rem")) mul = c->rem;
    else if (U("ex") || U("ch")) mul = c->em * 0.5f;
    else if (U("pt")) mul = 4.f / 3;
    else if (U("pc")) mul = 16;
    else if (U("in")) mul = 96;
    else if (U("cm")) mul = 96 / 2.54f;
    else if (U("mm")) mul = 96 / 25.4f;
    else if (U("q")) mul = 96 / 101.6f;
    else if (U("vw")) mul = c->vp.vw / 100;
    else if (U("vh")) mul = c->vp.vh / 100;
    else if (U("vmin")) mul = (c->vp.vw < c->vp.vh ? c->vp.vw : c->vp.vh) / 100;
    else if (U("vmax")) mul = (c->vp.vw > c->vp.vh ? c->vp.vw : c->vp.vh) / 100;
    else if (U("dvh") || U("svh") || U("lvh")) mul = c->vp.vh / 100;
    else if (U("dvw") || U("svw") || U("lvw")) mul = c->vp.vw / 100;
    else return false;
    #undef U
    o->px = v * mul;
    c->s += ul;
    return true;
}

static bool calc_func_args(CalcCtx *c, Calc *args, int *n, int max, int depth) {
    *n = 0;
    for (;;) {
        csp(c);
        if (*n >= max || !calc_expr(c, &args[*n], depth + 1)) return false;
        (*n)++;
        csp(c);
        if (*c->s == ',') { c->s++; continue; }
        if (*c->s == ')') { c->s++; return true; }
        return false;
    }
}

static bool calc_primary(CalcCtx *c, Calc *o, int depth) {
    if (depth > 8) return false;
    csp(c);
    if (*c->s == '(') {
        c->s++;
        if (!calc_expr(c, o, depth + 1)) return false;
        csp(c);
        if (*c->s != ')') return false;
        c->s++;
        return true;
    }
    if (!strncasecmp(c->s, "calc(", 5)) {
        c->s += 5;
        if (!calc_expr(c, o, depth + 1)) return false;
        csp(c);
        if (*c->s != ')') return false;
        c->s++;
        return true;
    }
    bool is_min = !strncasecmp(c->s, "min(", 4), is_max = !strncasecmp(c->s, "max(", 4),
         is_clamp = !strncasecmp(c->s, "clamp(", 6);
    if (is_min || is_max || is_clamp) {
        c->s += is_clamp ? 6 : 4;
        Calc args[8];
        int  n;
        if (!calc_func_args(c, args, &n, 8, depth)) return false;
        for (int i = 0; i < n; i++)
            if (args[i].pct != 0) return false; /* cannot resolve mixed percentages without a base */
        float v = args[0].px;
        if (is_clamp) {
            if (n != 3) return false;
            v = args[1].px < args[0].px ? args[0].px : args[1].px > args[2].px ? args[2].px : args[1].px;
        } else
            for (int i = 1; i < n; i++) v = is_min ? (args[i].px < v ? args[i].px : v) : (args[i].px > v ? args[i].px : v);
        o->px = v;
        o->pct = 0;
        o->unitless = false;
        return true;
    }
    return calc_number(c, o);
}

static bool calc_term(CalcCtx *c, Calc *o, int depth) {
    if (!calc_primary(c, o, depth)) return false;
    for (;;) {
        csp(c);
        char op = *c->s;
        if (op != '*' && op != '/') return true;
        c->s++;
        Calc r;
        if (!calc_primary(c, &r, depth)) return false;
        if (op == '*') {
            if (o->unitless) { float f = o->px; *o = r; o->px *= f; o->pct *= f; }
            else if (r.unitless) { o->px *= r.px; o->pct *= r.px; }
            else return false;
        } else {
            if (!r.unitless || r.px == 0) return false;
            o->px /= r.px;
            o->pct /= r.px;
        }
    }
}

static bool calc_expr(CalcCtx *c, Calc *o, int depth) {
    if (!calc_term(c, o, depth)) return false;
    for (;;) {
        csp(c);
        char op = *c->s;
        if (op != '+' && op != '-') return true;
        c->s++;
        Calc r;
        if (!calc_term(c, &r, depth)) return false;
        if (o->unitless != r.unitless) return false;
        float sgn = op == '+' ? 1.f : -1.f;
        o->px += sgn * r.px;
        o->pct += sgn * r.pct;
    }
}

typedef struct {
    float    font_size; /* of this element, for em */
    float    root_fs;
    Viewport vp;
} LenCtx;

/* Parses one length token. `auto` handled when allow_auto. Unitless 0 accepted. */
static bool parse_length(const LenCtx *lc, const char *w, Length *out, bool allow_auto) {
    while (is_space((unsigned char)*w)) w++;
    if (allow_auto && !strcasecmp(w, "auto")) {
        memset(out, 0, sizeof *out);
        out->is_auto = true;
        return true;
    }
    CalcCtx c = {w, lc->font_size, lc->root_fs, lc->vp};
    Calc    r;
    if (!calc_expr(&c, &r, 0)) return false;
    csp(&c);
    if (*c.s) return false;
    if (r.unitless && r.px != 0) return false; /* bare numbers are not lengths (except 0) */
    out->px = r.px;
    out->pct = r.pct;
    out->is_auto = false;
    return true;
}

/* ============================================================ value helpers == */
/* Splits at top-level whitespace (not inside parens or quotes). Modifies buf. */
static int split_words(char *buf, char **words, int max) {
    int n = 0;
    char *p = buf;
    while (*p && n < max) {
        while (is_space((unsigned char)*p)) p++;
        if (!*p) break;
        words[n++] = p;
        int paren = 0;
        while (*p && (paren > 0 || !is_space((unsigned char)*p))) {
            if (*p == '(') paren++;
            else if (*p == ')' && paren > 0) paren--;
            else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q) p++;
                if (!*p) break;
            }
            p++;
        }
        if (*p) *p++ = 0;
    }
    return n;
}

static bool streq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

static const CustomProp *find_custom(const Style *s, const char *name) {
    for (const CustomProp *c = s->custom; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* Replaces var(--x, fallback) occurrences. Returns arena string (or v when no var()). */
static const char *subst_vars(Arena *a, const Style *st, const char *v, int depth) {
    if (!strstr(v, "var(")) return v;
    if (depth > 8) return NULL;
    Buf out;
    buf_init(&out);
    const char *p = v;
    while (*p) {
        const char *vp = strstr(p, "var(");
        if (!vp) {
            buf_puts(&out, p);
            break;
        }
        buf_append(&out, p, (size_t)(vp - p));
        const char *q = vp + 4;
        int         paren = 1;
        const char *close = q;
        while (*close && paren > 0) {
            if (*close == '(') paren++;
            else if (*close == ')') paren--;
            close++;
        }
        if (paren != 0) { buf_free(&out); return NULL; }
        char *inner = xstrndup(q, (size_t)(close - 1 - q));
        char *comma = strchr(inner, ',');
        char *fallback = NULL;
        if (comma) { *comma = 0; fallback = str_trim(comma + 1); }
        char             *name = str_trim(inner);
        const CustomProp *cp = find_custom(st, name);
        const char       *repl = cp ? cp->value : fallback;
        if (!repl) { free(inner); buf_free(&out); return NULL; }
        const char *resolved = subst_vars(a, st, repl, depth + 1);
        if (!resolved) { free(inner); buf_free(&out); return NULL; }
        buf_puts(&out, resolved);
        free(inner);
        p = close;
    }
    const char *r = arena_strndup(a, out.data ? out.data : "", out.len);
    buf_free(&out);
    return r;
}

/* ================================================================ initial === */
static void style_init(Style *s) {
    memset(s, 0, sizeof *s);
    s->display = DISP_INLINE;
    s->font_size = 16;
    s->font_weight = 400;
    s->line_height = -1;
    s->color = MAKE_RGB(0, 0, 0);
    s->opacity = 1;
    s->list_style = LS_DISC;
    s->width.is_auto = s->height.is_auto = s->min_width.is_auto = s->min_height.is_auto = true;
    s->max_width.is_auto = s->max_height.is_auto = true;
    for (int i = 0; i < 4; i++) {
        s->margin[i] = (Length){0, 0, false};
        s->padding[i] = (Length){0, 0, false};
        s->border_w[i] = 3;
        s->border_color_cur |= (uint8_t)(1 << i);
    }
}

static void style_inherit_from(Style *s, const Style *p) {
    s->color = p->color;
    s->font_size = p->font_size;
    s->font_weight = p->font_weight;
    s->italic = p->italic;
    s->font_family = p->font_family;
    s->line_height = p->line_height;
    s->lh_is_mul = p->lh_is_mul;
    s->text_align = p->text_align;
    s->text_transform = p->text_transform;
    s->white_space = p->white_space;
    s->list_style = p->list_style;
    s->visibility_hidden = p->visibility_hidden;
    s->letter_spacing = p->letter_spacing;
    s->text_indent = p->text_indent;
    s->underline = p->underline;
    s->line_through = p->line_through;
    s->border_spacing = p->border_spacing;
    s->border_collapse = p->border_collapse;
    s->custom = p->custom;
}

/* ============================================================= apply values == */
typedef struct {
    Arena       *arena;
    Style       *st;
    const Style *parent;
    LenCtx       lc;
    Style        initial;
} Apply;

static int map_font_size_kw(const char *w, float parent_fs, float *out) {
    static const struct { const char *n; float v; } kw[] = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24},
        {"xx-large", 32}, {"xxx-large", 48},
    };
    for (size_t i = 0; i < sizeof kw / sizeof *kw; i++)
        if (streq(w, kw[i].n)) { *out = kw[i].v; return 1; }
    if (streq(w, "smaller")) { *out = parent_fs / 1.2f; return 1; }
    if (streq(w, "larger")) { *out = parent_fs * 1.2f; return 1; }
    return 0;
}

static bool parse_font_size(Apply *ap, const char *w, float *out) {
    float pfs = ap->parent ? ap->parent->font_size : 16;
    if (map_font_size_kw(w, pfs, out)) return true;
    LenCtx lc = ap->lc;
    lc.font_size = pfs; /* em/% on font-size refer to the parent */
    Length l;
    if (!parse_length(&lc, w, &l, false)) return false;
    float v = l.px + l.pct * pfs;
    if (v < 0) return false;
    *out = v;
    return true;
}

static bool parse_border_width(Apply *ap, const char *w, float *out) {
    if (streq(w, "thin")) { *out = 1; return true; }
    if (streq(w, "medium")) { *out = 3; return true; }
    if (streq(w, "thick")) { *out = 5; return true; }
    Length l;
    if (!parse_length(&ap->lc, w, &l, false) || l.pct != 0 || l.px < 0) return false;
    *out = l.px;
    return true;
}

static bool parse_border_style(const char *w, uint8_t *out) {
    if (streq(w, "none") || streq(w, "hidden")) { *out = BS_NONE; return true; }
    if (streq(w, "solid") || streq(w, "groove") || streq(w, "ridge") || streq(w, "inset") || streq(w, "outset")) {
        *out = BS_SOLID;
        return true;
    }
    if (streq(w, "dashed")) { *out = BS_DASHED; return true; }
    if (streq(w, "dotted")) { *out = BS_DOTTED; return true; }
    if (streq(w, "double")) { *out = BS_DOUBLE; return true; }
    return false;
}

static bool color_from_gradient(const char *w, uint32_t *out) {
    if (!strstr(w, "gradient(")) return false;
    const char *p = strchr(w, '(') + 1;
    /* take the first token that parses as a color */
    char *copy = xstrdup(p);
    char *save = NULL;
    bool  ok = false;
    for (char *tok = strtok_r(copy, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        /* rgb(...) contains commas: re-join naive case */
        if (!strncasecmp(tok, "rgb", 3) || !strncasecmp(tok, "hsl", 3)) {
            const char *start = p + (tok - copy);
            const char *close = strchr(start, ')');
            if (close) {
                char *t = xstrndup(start, (size_t)(close - start + 1));
                ok = style_parse_color(t, out);
                free(t);
            }
        } else ok = style_parse_color(tok, out);
        if (ok) break;
    }
    free(copy);
    return ok;
}

static void set_color_prop(Style *s, int side, uint32_t c, bool cur) {
    s->border_color[side] = c;
    if (cur) s->border_color_cur |= (uint8_t)(1 << side);
    else s->border_color_cur &= (uint8_t)~(1 << side);
}

static void apply_box4(Apply *ap, Length *dst, char **w, int n, bool allow_auto) {
    Length v[4];
    if (n < 1 || n > 4) return;
    for (int i = 0; i < n; i++)
        if (!parse_length(&ap->lc, w[i], &v[i], allow_auto)) return;
    int t = 0, r = 0, b = 0, l = 0;
    switch (n) {
    case 1: t = r = b = l = 0; break;
    case 2: t = b = 0; r = l = 1; break;
    case 3: t = 0; r = l = 1; b = 2; break;
    default: t = 0; r = 1; b = 2; l = 3;
    }
    dst[0] = v[t];
    dst[1] = v[r];
    dst[2] = v[b];
    dst[3] = v[l];
}

static void copy_prop(Style *d, const Style *s, PropId id) {
    switch (id) {
    case PROP_DISPLAY: d->display = s->display; break;
    case PROP_COLOR: d->color = s->color; break;
    case PROP_BACKGROUND: case PROP_BACKGROUND_COLOR: d->bg = s->bg; break;
    case PROP_FONT_SIZE: d->font_size = s->font_size; break;
    case PROP_FONT_WEIGHT: d->font_weight = s->font_weight; break;
    case PROP_FONT_STYLE: d->italic = s->italic; break;
    case PROP_FONT_FAMILY: d->font_family = s->font_family; break;
    case PROP_LINE_HEIGHT: d->line_height = s->line_height; d->lh_is_mul = s->lh_is_mul; break;
    case PROP_TEXT_ALIGN: d->text_align = s->text_align; break;
    case PROP_TEXT_DECORATION: case PROP_TEXT_DECORATION_LINE: d->underline = s->underline; d->line_through = s->line_through; break;
    case PROP_TEXT_TRANSFORM: d->text_transform = s->text_transform; break;
    case PROP_WHITE_SPACE: d->white_space = s->white_space; break;
    case PROP_LETTER_SPACING: d->letter_spacing = s->letter_spacing; break;
    case PROP_TEXT_INDENT: d->text_indent = s->text_indent; break;
    case PROP_VERTICAL_ALIGN: d->vertical_align = s->vertical_align; break;
    case PROP_VISIBILITY: d->visibility_hidden = s->visibility_hidden; break;
    case PROP_MARGIN: for (int i = 0; i < 4; i++) d->margin[i] = s->margin[i]; break;
    case PROP_MARGIN_TOP: d->margin[0] = s->margin[0]; break;
    case PROP_MARGIN_RIGHT: d->margin[1] = s->margin[1]; break;
    case PROP_MARGIN_BOTTOM: d->margin[2] = s->margin[2]; break;
    case PROP_MARGIN_LEFT: d->margin[3] = s->margin[3]; break;
    case PROP_PADDING: for (int i = 0; i < 4; i++) d->padding[i] = s->padding[i]; break;
    case PROP_PADDING_TOP: d->padding[0] = s->padding[0]; break;
    case PROP_PADDING_RIGHT: d->padding[1] = s->padding[1]; break;
    case PROP_PADDING_BOTTOM: d->padding[2] = s->padding[2]; break;
    case PROP_PADDING_LEFT: d->padding[3] = s->padding[3]; break;
    case PROP_WIDTH: d->width = s->width; break;
    case PROP_HEIGHT: d->height = s->height; break;
    case PROP_MIN_WIDTH: d->min_width = s->min_width; break;
    case PROP_MAX_WIDTH: d->max_width = s->max_width; break;
    case PROP_MIN_HEIGHT: d->min_height = s->min_height; break;
    case PROP_MAX_HEIGHT: d->max_height = s->max_height; break;
    case PROP_LIST_STYLE: case PROP_LIST_STYLE_TYPE: d->list_style = s->list_style; break;
    case PROP_BOX_SIZING: d->box_sizing_border = s->box_sizing_border; break;
    case PROP_FLOAT: d->float_ = s->float_; break;
    case PROP_CLEAR: d->clear = s->clear; break;
    case PROP_OVERFLOW: d->overflow = s->overflow; break;
    case PROP_POSITION: d->position = s->position; break;
    case PROP_OPACITY: d->opacity = s->opacity; break;
    case PROP_BORDER_SPACING: d->border_spacing = s->border_spacing; break;
    case PROP_BORDER_COLLAPSE: d->border_collapse = s->border_collapse; break;
    case PROP_BORDER: case PROP_BORDER_WIDTH: case PROP_BORDER_STYLE: case PROP_BORDER_COLOR:
        for (int i = 0; i < 4; i++) {
            d->border_w[i] = s->border_w[i];
            d->border_style[i] = s->border_style[i];
            d->border_color[i] = s->border_color[i];
        }
        d->border_color_cur = s->border_color_cur;
        break;
    default: break;
    }
}

enum { PASS_CUSTOM, PASS_FONTSIZE, PASS_REST };

static void apply_decl(Apply *ap, const Decl *d, int pass) {
    Style *s = ap->st;
    PropId id = d->prop;

    if (pass == PASS_CUSTOM) {
        if (id != PROP_CUSTOM) return;
        const char *v = subst_vars(ap->arena, s, d->value, 0);
        if (!v) return;
        CustomProp *cp = arena_alloc(ap->arena, sizeof *cp);
        cp->name = d->name;
        cp->value = v;
        cp->next = s->custom;
        s->custom = cp;
        return;
    }
    if (id == PROP_CUSTOM) return;
    bool is_fs = id == PROP_FONT_SIZE || id == PROP_FONT;
    if ((pass == PASS_FONTSIZE) != is_fs && !(pass == PASS_REST && id == PROP_FONT)) return;

    const char *val = subst_vars(ap->arena, s, d->value, 0);
    if (!val) return;
    char *buf = xstrdup(val);
    char *words[16];
    int   n = split_words(buf, words, 16);
    if (n == 0) { free(buf); return; }
    char *w0 = words[0];

    /* css-wide keywords */
    if (n == 1 && (streq(w0, "inherit") || streq(w0, "initial") || streq(w0, "unset") || streq(w0, "revert"))) {
        bool inheritable = id == PROP_COLOR || id == PROP_FONT_SIZE || id == PROP_FONT_WEIGHT || id == PROP_FONT_STYLE ||
                           id == PROP_FONT_FAMILY || id == PROP_LINE_HEIGHT || id == PROP_TEXT_ALIGN ||
                           id == PROP_TEXT_TRANSFORM || id == PROP_WHITE_SPACE || id == PROP_VISIBILITY ||
                           id == PROP_LIST_STYLE || id == PROP_LIST_STYLE_TYPE || id == PROP_LETTER_SPACING ||
                           id == PROP_TEXT_INDENT || id == PROP_TEXT_DECORATION || id == PROP_TEXT_DECORATION_LINE;
        bool use_parent = streq(w0, "inherit") || (streq(w0, "unset") && inheritable);
        const Style *src = use_parent && ap->parent ? ap->parent : &ap->initial;
        if (id == PROP_FONT) {
            copy_prop(s, src, PROP_FONT_SIZE); copy_prop(s, src, PROP_FONT_WEIGHT); copy_prop(s, src, PROP_FONT_STYLE);
            copy_prop(s, src, PROP_FONT_FAMILY); copy_prop(s, src, PROP_LINE_HEIGHT);
        } else copy_prop(s, src, id);
        if (id == PROP_FONT_SIZE) ap->lc.font_size = s->font_size;
        free(buf);
        return;
    }

    Length l;
    switch (id) {
    case PROP_DISPLAY: {
        static const struct { const char *n; DisplayType d; } m[] = {
            {"none", DISP_NONE}, {"block", DISP_BLOCK}, {"inline", DISP_INLINE}, {"inline-block", DISP_INLINE_BLOCK},
            {"list-item", DISP_LIST_ITEM}, {"table", DISP_TABLE}, {"inline-table", DISP_TABLE},
            {"table-row", DISP_TABLE_ROW}, {"table-cell", DISP_TABLE_CELL}, {"table-row-group", DISP_TABLE_ROW_GROUP},
            {"table-header-group", DISP_TABLE_ROW_GROUP}, {"table-footer-group", DISP_TABLE_ROW_GROUP},
            {"table-caption", DISP_TABLE_CAPTION}, {"flex", DISP_FLEX}, {"inline-flex", DISP_INLINE_FLEX},
            {"grid", DISP_BLOCK}, {"inline-grid", DISP_INLINE_BLOCK}, {"contents", DISP_CONTENTS},
            {"flow-root", DISP_BLOCK}, {"table-column", DISP_NONE}, {"table-column-group", DISP_NONE},
        };
        for (size_t i = 0; i < sizeof m / sizeof *m; i++)
            if (streq(w0, m[i].n)) { s->display = (uint8_t)m[i].d; break; }
        break;
    }
    case PROP_COLOR: {
        uint32_t c;
        if (streq(w0, "currentcolor")) break;
        if (style_parse_color(val, &c)) s->color = c;
        break;
    }
    case PROP_BACKGROUND_COLOR: {
        uint32_t c;
        if (streq(w0, "currentcolor")) s->bg = s->color;
        else if (style_parse_color(val, &c)) s->bg = c;
        break;
    }
    case PROP_BACKGROUND: {
        uint32_t c = 0;
        bool     found = false;
        for (int i = 0; i < n && !found; i++) {
            if (streq(words[i], "currentcolor")) { c = s->color; found = true; }
            else if (style_parse_color(words[i], &c) || color_from_gradient(words[i], &c)) found = true;
        }
        s->bg = found ? c : 0;
        break;
    }
    case PROP_FONT_SIZE: {
        float fs;
        if (parse_font_size(ap, w0, &fs)) {
            s->font_size = fs;
            ap->lc.font_size = fs;
        }
        break;
    }
    case PROP_FONT_WEIGHT: {
        if (streq(w0, "normal")) s->font_weight = 400;
        else if (streq(w0, "bold")) s->font_weight = 700;
        else if (streq(w0, "bolder")) s->font_weight = s->font_weight < 400 ? 400 : s->font_weight < 600 ? 700 : 900;
        else if (streq(w0, "lighter")) s->font_weight = s->font_weight > 700 ? 700 : s->font_weight > 500 ? 400 : 100;
        else { int v = atoi(w0); if (v >= 1 && v <= 1000) s->font_weight = v; }
        break;
    }
    case PROP_FONT_STYLE:
        s->italic = streq(w0, "italic") || streq(w0, "oblique");
        break;
    case PROP_FONT_FAMILY: {
        /* walk the comma separated list, use the first family we can map */
        char *p = xstrdup(val), *save = NULL;
        for (char *tok = strtok_r(p, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            char *t = str_trim(tok);
            if (*t == '"' || *t == '\'') { t++; size_t tl = strlen(t); if (tl && (t[tl - 1] == '"' || t[tl - 1] == '\'')) t[tl - 1] = 0; }
            str_lower(t);
            static const char *mono[] = {"monospace", "courier", "courier new", "consolas", "menlo", "monaco", "dejavu sans mono",
                "sf mono", "fira code", "fira mono", "source code pro", "ui-monospace", "lucida console", "liberation mono", "jetbrains mono", "roboto mono", NULL};
            static const char *serif[] = {"serif", "times", "times new roman", "georgia", "palatino", "garamond", "cambria",
                "book antiqua", "liberation serif", "ui-serif", "baskerville", "charter", NULL};
            static const char *sans[] = {"sans-serif", "arial", "helvetica", "helvetica neue", "verdana", "tahoma", "segoe ui",
                "roboto", "open sans", "system-ui", "-apple-system", "blinkmacsystemfont", "ui-sans-serif", "inter", "trebuchet ms",
                "liberation sans", "noto sans", "lucida grande", "calibri", "dejavu sans", "ubuntu", "cantarell", NULL};
            bool hit = false;
            for (int i = 0; mono[i] && !hit; i++) if (!strcmp(t, mono[i])) { s->font_family = FF_MONO; hit = true; }
            for (int i = 0; serif[i] && !hit; i++) if (!strcmp(t, serif[i])) { s->font_family = FF_SERIF; hit = true; }
            for (int i = 0; sans[i] && !hit; i++) if (!strcmp(t, sans[i])) { s->font_family = FF_SANS; hit = true; }
            if (hit) break;
        }
        free(p);
        break;
    }
    case PROP_FONT: {
        /* find the size word (it may be glued to "/line-height") */
        int size_idx = -1;
        for (int i = 0; i < n && size_idx < 0; i++) {
            char *slash = strchr(words[i], '/');
            char  save = 0;
            if (slash) { save = *slash; *slash = 0; }
            float fs;
            bool  ok = *words[i] && parse_font_size(ap, words[i], &fs);
            if (slash) *slash = save;
            if (ok) size_idx = i;
        }
        if (size_idx < 0) break; /* invalid shorthand (e.g. system font keywords) */
        if (pass == PASS_FONTSIZE) {
            char *slash = strchr(words[size_idx], '/');
            if (slash) *slash = 0;
            float fs;
            if (parse_font_size(ap, words[size_idx], &fs)) { s->font_size = fs; ap->lc.font_size = fs; }
            if (slash) *slash = '/';
            break;
        }
        /* PASS_REST: the shorthand resets style, weight, line-height, family */
        s->italic = false;
        s->font_weight = 400;
        s->line_height = -1;
        s->lh_is_mul = false;
        for (int i = 0; i < size_idx; i++) {
            if (streq(words[i], "italic") || streq(words[i], "oblique")) s->italic = true;
            else if (streq(words[i], "bold")) s->font_weight = 700;
            else if (atoi(words[i]) >= 100) s->font_weight = atoi(words[i]);
        }
        int         fam_start = size_idx + 1;
        const char *lh = NULL;
        char       *slash = strchr(words[size_idx], '/');
        if (slash) lh = slash + 1;
        else if (fam_start < n && words[fam_start][0] == '/') { lh = words[fam_start] + 1; fam_start++; }
        if (lh && !*lh && fam_start < n) lh = words[fam_start++]; /* "12px / 1.5 serif" */
        if (lh && *lh) {
            Decl tmp = {PROP_LINE_HEIGHT, NULL, lh, false, NULL};
            apply_decl(ap, &tmp, PASS_REST);
        }
        Buf fb;
        buf_init(&fb);
        for (int k = fam_start; k < n; k++) {
            if (k > fam_start) buf_putc(&fb, ' ');
            buf_puts(&fb, words[k]);
        }
        if (fb.len) {
            Decl tmp = {PROP_FONT_FAMILY, NULL, fb.data, false, NULL};
            apply_decl(ap, &tmp, PASS_REST);
        }
        buf_free(&fb);
        break;
    }
    case PROP_LINE_HEIGHT: {
        if (streq(w0, "normal")) { s->line_height = -1; s->lh_is_mul = false; break; }
        char *e;
        float v = strtof(w0, &e);
        if (e != w0 && !*e) { s->line_height = v; s->lh_is_mul = true; break; }
        if (parse_length(&ap->lc, w0, &l, false)) {
            float px = l.px + l.pct * s->font_size;
            if (px >= 0) { s->line_height = px; s->lh_is_mul = false; }
        }
        break;
    }
    case PROP_TEXT_ALIGN:
        if (streq(w0, "left") || streq(w0, "start")) s->text_align = TA_LEFT;
        else if (streq(w0, "right") || streq(w0, "end")) s->text_align = TA_RIGHT;
        else if (streq(w0, "center") || streq(w0, "-webkit-center")) s->text_align = TA_CENTER;
        else if (streq(w0, "justify")) s->text_align = TA_JUSTIFY;
        break;
    case PROP_TEXT_DECORATION:
    case PROP_TEXT_DECORATION_LINE: {
        bool u = false, st = false, any = false;
        for (int i = 0; i < n; i++) {
            if (streq(words[i], "underline")) { u = true; any = true; }
            else if (streq(words[i], "line-through")) { st = true; any = true; }
            else if (streq(words[i], "none")) any = true;
            else if (streq(words[i], "overline")) any = true;
        }
        if (any) { s->underline = u; s->line_through = st; }
        break;
    }
    case PROP_TEXT_TRANSFORM:
        s->text_transform = streq(w0, "uppercase") ? TT_UPPER : streq(w0, "lowercase") ? TT_LOWER : streq(w0, "capitalize") ? TT_CAP : TT_NONE;
        break;
    case PROP_WHITE_SPACE:
        s->white_space = streq(w0, "nowrap") ? WS_NOWRAP : streq(w0, "pre") ? WS_PRE : streq(w0, "pre-wrap") ? WS_PRE_WRAP
                       : streq(w0, "pre-line") ? WS_PRE_LINE : WS_NORMAL;
        break;
    case PROP_LETTER_SPACING:
        if (streq(w0, "normal")) s->letter_spacing = 0;
        else if (parse_length(&ap->lc, w0, &l, false)) s->letter_spacing = l.px;
        break;
    case PROP_TEXT_INDENT:
        if (parse_length(&ap->lc, w0, &l, false)) s->text_indent = l.px;
        break;
    case PROP_VERTICAL_ALIGN:
        s->vertical_align = streq(w0, "top") || streq(w0, "text-top") ? VA_TOP : streq(w0, "middle") ? VA_MIDDLE
                          : streq(w0, "bottom") || streq(w0, "text-bottom") ? VA_BOTTOM : streq(w0, "sub") ? VA_SUB
                          : streq(w0, "super") ? VA_SUPER : VA_BASELINE;
        break;
    case PROP_VISIBILITY:
        s->visibility_hidden = streq(w0, "hidden") || streq(w0, "collapse");
        break;
    case PROP_MARGIN: apply_box4(ap, s->margin, words, n, true); break;
    case PROP_PADDING: apply_box4(ap, s->padding, words, n, false); break;
    case PROP_MARGIN_TOP: case PROP_MARGIN_RIGHT: case PROP_MARGIN_BOTTOM: case PROP_MARGIN_LEFT:
        if (parse_length(&ap->lc, w0, &l, true)) s->margin[id - PROP_MARGIN_TOP] = l;
        break;
    case PROP_PADDING_TOP: case PROP_PADDING_RIGHT: case PROP_PADDING_BOTTOM: case PROP_PADDING_LEFT:
        if (parse_length(&ap->lc, w0, &l, false) && (l.px + l.pct) >= 0) s->padding[id - PROP_PADDING_TOP] = l;
        break;
    case PROP_BORDER: case PROP_BORDER_TOP: case PROP_BORDER_RIGHT: case PROP_BORDER_BOTTOM: case PROP_BORDER_LEFT: {
        float    bw = 3;
        uint8_t  bs = BS_NONE;
        uint32_t bc = 0;
        bool     cur = true;
        for (int i = 0; i < n; i++) {
            float    f;
            uint8_t  st;
            uint32_t c;
            if (parse_border_style(words[i], &st)) bs = st;
            else if (parse_border_width(ap, words[i], &f)) bw = f;
            else if (streq(words[i], "currentcolor")) cur = true;
            else if (style_parse_color(words[i], &c)) { bc = c; cur = false; }
        }
        int lo = id == PROP_BORDER ? 0 : id - PROP_BORDER_TOP, hi = id == PROP_BORDER ? 3 : lo;
        for (int i = lo; i <= hi; i++) {
            s->border_w[i] = bw;
            s->border_style[i] = bs;
            set_color_prop(s, i, bc, cur);
        }
        break;
    }
    case PROP_BORDER_WIDTH: {
        float f[4];
        if (n < 1 || n > 4) break;
        bool ok = true;
        for (int i = 0; i < n; i++) ok = ok && parse_border_width(ap, words[i], &f[i]);
        if (!ok) break;
        int idx[4][4] = {{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};
        for (int i = 0; i < 4; i++) s->border_w[i] = f[idx[n - 1][i]];
        break;
    }
    case PROP_BORDER_STYLE: {
        uint8_t st[4];
        if (n < 1 || n > 4) break;
        bool ok = true;
        for (int i = 0; i < n; i++) ok = ok && parse_border_style(words[i], &st[i]);
        if (!ok) break;
        int idx[4][4] = {{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};
        for (int i = 0; i < 4; i++) s->border_style[i] = st[idx[n - 1][i]];
        break;
    }
    case PROP_BORDER_COLOR: {
        uint32_t c[4];
        bool     cur[4];
        if (n < 1 || n > 4) break;
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            cur[i] = streq(words[i], "currentcolor");
            ok = cur[i] || style_parse_color(words[i], &c[i]);
            if (cur[i]) c[i] = 0;
        }
        if (!ok) break;
        int idx[4][4] = {{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}};
        for (int i = 0; i < 4; i++) set_color_prop(s, i, c[idx[n - 1][i]], cur[idx[n - 1][i]]);
        break;
    }
    case PROP_BORDER_TOP_WIDTH: case PROP_BORDER_RIGHT_WIDTH: case PROP_BORDER_BOTTOM_WIDTH: case PROP_BORDER_LEFT_WIDTH: {
        float f;
        if (parse_border_width(ap, w0, &f)) s->border_w[id - PROP_BORDER_TOP_WIDTH] = f;
        break;
    }
    case PROP_BORDER_TOP_STYLE: case PROP_BORDER_RIGHT_STYLE: case PROP_BORDER_BOTTOM_STYLE: case PROP_BORDER_LEFT_STYLE: {
        uint8_t st;
        if (parse_border_style(w0, &st)) s->border_style[id - PROP_BORDER_TOP_STYLE] = st;
        break;
    }
    case PROP_BORDER_TOP_COLOR: case PROP_BORDER_RIGHT_COLOR: case PROP_BORDER_BOTTOM_COLOR: case PROP_BORDER_LEFT_COLOR: {
        uint32_t c;
        int      side = id - PROP_BORDER_TOP_COLOR;
        if (streq(w0, "currentcolor")) set_color_prop(s, side, 0, true);
        else if (style_parse_color(w0, &c)) set_color_prop(s, side, c, false);
        break;
    }
    case PROP_WIDTH:
        if (parse_length(&ap->lc, w0, &l, true) && (l.is_auto || l.px + l.pct >= 0 || l.pct != 0)) s->width = l;
        else if (streq(w0, "max-content") || streq(w0, "min-content") || streq(w0, "fit-content")) s->width.is_auto = true;
        break;
    case PROP_HEIGHT:
        if (parse_length(&ap->lc, w0, &l, true)) s->height = l;
        break;
    case PROP_MIN_WIDTH:
        if (parse_length(&ap->lc, w0, &l, true)) s->min_width = l;
        break;
    case PROP_MIN_HEIGHT:
        if (parse_length(&ap->lc, w0, &l, true)) s->min_height = l;
        break;
    case PROP_MAX_WIDTH:
        if (streq(w0, "none")) s->max_width = (Length){0, 0, true};
        else if (parse_length(&ap->lc, w0, &l, true)) s->max_width = l;
        break;
    case PROP_MAX_HEIGHT:
        if (streq(w0, "none")) s->max_height = (Length){0, 0, true};
        else if (parse_length(&ap->lc, w0, &l, true)) s->max_height = l;
        break;
    case PROP_LIST_STYLE:
    case PROP_LIST_STYLE_TYPE:
        for (int i = 0; i < n; i++) {
            const char *w = words[i];
            if (streq(w, "none")) s->list_style = LS_NONE;
            else if (streq(w, "disc")) s->list_style = LS_DISC;
            else if (streq(w, "circle")) s->list_style = LS_CIRCLE;
            else if (streq(w, "square")) s->list_style = LS_SQUARE;
            else if (streq(w, "decimal") || streq(w, "decimal-leading-zero")) s->list_style = LS_DECIMAL;
            else if (streq(w, "lower-alpha") || streq(w, "lower-latin")) s->list_style = LS_LOWER_ALPHA;
            else if (streq(w, "upper-alpha") || streq(w, "upper-latin")) s->list_style = LS_UPPER_ALPHA;
            else if (streq(w, "lower-roman")) s->list_style = LS_LOWER_ROMAN;
            else if (streq(w, "upper-roman")) s->list_style = LS_UPPER_ROMAN;
        }
        break;
    case PROP_BOX_SIZING: s->box_sizing_border = streq(w0, "border-box"); break;
    case PROP_FLOAT: s->float_ = streq(w0, "left") || streq(w0, "inline-start") ? FL_LEFT : streq(w0, "right") || streq(w0, "inline-end") ? FL_RIGHT : FL_NONE; break;
    case PROP_CLEAR: s->clear = streq(w0, "left") ? 1 : streq(w0, "right") ? 2 : streq(w0, "both") ? 3 : 0; break;
    case PROP_OVERFLOW:
        s->overflow = streq(w0, "hidden") || streq(w0, "clip") ? OV_HIDDEN : streq(w0, "scroll") ? OV_SCROLL : streq(w0, "auto") ? OV_AUTO : OV_VISIBLE;
        break;
    case PROP_POSITION:
        s->position = streq(w0, "relative") ? POS_RELATIVE : streq(w0, "absolute") ? POS_ABSOLUTE : streq(w0, "fixed") ? POS_FIXED
                    : streq(w0, "sticky") ? POS_STICKY : POS_STATIC;
        break;
    case PROP_BORDER_SPACING:
        if (parse_length(&ap->lc, w0, &l, false) && l.px >= 0) s->border_spacing = l.px;
        break;
    case PROP_BORDER_COLLAPSE: s->border_collapse = streq(w0, "collapse"); break;
    case PROP_OPACITY: {
        char *e;
        float v = strtof(w0, &e);
        if (e != w0) { if (*e == '%') v /= 100; s->opacity = v < 0 ? 0 : v > 1 ? 1 : v; }
        break;
    }
    default: break;
    }
    free(buf);
}

/* ================================================================= cascade == */
typedef struct {
    const Decl *decls;
    uint64_t    key;
} DeclSrc;

typedef struct {
    DeclSrc *v;
    int      n, cap;
} SrcList;

static void srclist_push(SrcList *l, const Decl *d, uint64_t key) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, sizeof(DeclSrc) * (size_t)l->cap);
    }
    l->v[l->n].decls = d;
    l->v[l->n].key = key;
    l->n++;
}

static int cmp_src(const void *a, const void *b) {
    uint64_t ka = ((const DeclSrc *)a)->key, kb = ((const DeclSrc *)b)->key;
    return ka < kb ? -1 : ka > kb;
}

static const char *numeric_px(Arena *a, const char *v) {
    char *e;
    strtod(v, &e);
    if (e != v && !*e) { /* bare number -> px */
        char *r = arena_alloc(a, strlen(v) + 3);
        sprintf(r, "%spx", v);
        return r;
    }
    return v;
}

static const Node *ancestor(const Node *n, const char *tag) {
    for (const Node *p = n->parent; p; p = p->parent)
        if (dom_is(p, tag)) return p;
    return NULL;
}

/* Presentational HTML attributes (bgcolor, align, width...) as low-priority declarations. */
static Decl *presentational_hints(Arena *a, const Node *n) {
    Buf b;
    buf_init(&b);
    const char *v;
    const char *t = n->tag;
    if ((v = dom_attr(n, "bgcolor"))) buf_printf(&b, "background-color:%s;", v);
    if (!strcmp(t, "body") && (v = dom_attr(n, "text"))) buf_printf(&b, "color:%s;", v);
    if (!strcmp(t, "font")) {
        if ((v = dom_attr(n, "color"))) buf_printf(&b, "color:%s;", v);
        if ((v = dom_attr(n, "face"))) buf_printf(&b, "font-family:%s;", v);
        if ((v = dom_attr(n, "size"))) {
            static const int px[] = {10, 13, 16, 18, 24, 32, 48};
            int sz = atoi(v);
            if (v[0] == '+' || v[0] == '-') sz += 3;
            if (sz < 1) sz = 1;
            if (sz > 7) sz = 7;
            buf_printf(&b, "font-size:%dpx;", px[sz - 1]);
        }
    }
    if ((v = dom_attr(n, "align"))) {
        if (!strcmp(t, "table") || !strcmp(t, "div") || !strcmp(t, "img")) {
            if (!strcasecmp(v, "center") && strcmp(t, "img")) buf_puts(&b, "margin-left:auto;margin-right:auto;");
            else if (!strcasecmp(v, "left")) buf_puts(&b, strcmp(t, "div") ? "float:left;" : "text-align:left;");
            else if (!strcasecmp(v, "right")) buf_puts(&b, strcmp(t, "div") ? "float:right;" : "text-align:right;");
            if (!strcmp(t, "div") && !strcasecmp(v, "center")) buf_puts(&b, "text-align:center;");
        } else buf_printf(&b, "text-align:%s;", v);
    }
    if (!strcmp(t, "img") || !strcmp(t, "table") || !strcmp(t, "td") || !strcmp(t, "th")) {
        if ((v = dom_attr(n, "width")) && *v) buf_printf(&b, "width:%s;", numeric_px(a, v));
        if ((v = dom_attr(n, "height")) && *v) buf_printf(&b, "height:%s;", numeric_px(a, v));
    }
    if (!strcmp(t, "table") && (v = dom_attr(n, "border")) && atoi(v) > 0) buf_printf(&b, "border:%dpx outset #808080;", atoi(v));
    if (!strcmp(t, "td") || !strcmp(t, "th")) {
        const Node *tb = ancestor(n, "table");
        if (tb) {
            const char *cp = dom_attr(tb, "cellpadding");
            if (cp) buf_printf(&b, "padding:%dpx;", atoi(cp));
            const char *bd = dom_attr(tb, "border");
            if (bd && atoi(bd) > 0) buf_puts(&b, "border:1px solid #808080;");
        }
        if (dom_attr(n, "nowrap")) buf_puts(&b, "white-space:nowrap;");
    }
    if (!strcmp(t, "table") && (v = dom_attr(n, "cellspacing"))) buf_printf(&b, "border-spacing:%dpx;", atoi(v));
    Decl *d = b.len ? css_parse_decls(a, b.data, b.len) : NULL;
    buf_free(&b);
    return d;
}

void style_compute_tree(Node *doc, const StyleSet *set, Arena *arena) {
    SrcList srcs = {0};
    Apply   ap;
    memset(&ap, 0, sizeof ap);
    style_init(&ap.initial);
    ap.arena = arena;
    float root_fs = 16;

    Node *n = doc->first_child;
    if (!n) return;
    while (n) {
        if (n->type == NODE_ELEMENT) {
            const Style *parent = n->parent && n->parent->type == NODE_ELEMENT ? n->parent->style : NULL;
            srcs.n = 0;
            for (int si = 0; si < set->nsheets; si++) {
                const Stylesheet *sh = set->sheets[si];
                uint64_t          origin = sh->origin == 0 ? 0 : 2;
                for (int ri = 0; ri < sh->nrules; ri++) {
                    const Rule *r = &sh->rules[ri];
                    if (r->media && !media_matches(r->media, &set->vp)) continue;
                    long spec = css_rule_match(r, n);
                    if (spec < 0) continue;
                    srclist_push(&srcs, r->decls, (origin << 56) | ((uint64_t)spec << 32) | (uint64_t)((si << 20) | ri));
                }
            }
            Decl *hints = presentational_hints(arena, n);
            if (hints) srclist_push(&srcs, hints, (uint64_t)1 << 56);
            const char *inline_style = dom_attr(n, "style");
            if (inline_style) {
                Decl *d = css_parse_decls(arena, inline_style, strlen(inline_style));
                if (d) srclist_push(&srcs, d, (uint64_t)3 << 56);
            }
            qsort(srcs.v, (size_t)srcs.n, sizeof(DeclSrc), cmp_src);

            Style *s = arena_alloc(arena, sizeof *s);
            style_init(s);
            if (parent) style_inherit_from(s, parent);
            ap.st = s;
            ap.parent = parent;
            ap.lc.font_size = s->font_size;
            ap.lc.root_fs = root_fs;
            ap.lc.vp = set->vp;
            for (int pass = PASS_CUSTOM; pass <= PASS_REST; pass++) {
                for (int imp = 0; imp < 2; imp++)
                    for (int i = 0; i < srcs.n; i++)
                        for (const Decl *d = srcs.v[i].decls; d; d = d->next)
                            if ((int)d->important == imp) apply_decl(&ap, d, pass);
                if (pass == PASS_FONTSIZE) ap.lc.font_size = s->font_size;
            }
            if (!n->parent || n->parent->type == NODE_DOCUMENT) root_fs = s->font_size;
            /* inline/inline-block elements keep display; floats and absolute positioning blockify */
            if ((s->float_ != FL_NONE || s->position == POS_ABSOLUTE || s->position == POS_FIXED) &&
                (s->display == DISP_INLINE || s->display == DISP_INLINE_BLOCK))
                s->display = DISP_BLOCK;
            n->style = s;
        } else if (n->type == NODE_TEXT && n->parent) {
            n->style = n->parent->style;
        }
        /* next node, pre-order, iterative */
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n && n != doc && !n->next) n = n->parent;
        if (!n || n == doc) break;
        n = n->next;
    }
    free(srcs.v);
}
