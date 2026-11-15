/* Browser shell: tabs, history, address bar, chrome drawing, input handling.
 * Backend independent: ui_x11.c feeds it abstract events. */
#ifndef BH_BROWSER_H
#define BH_BROWSER_H

#include "gfx.h"
#include "page.h"

#define BH_BRAND "LUMNA"

#define TAB_H 40
#define TOOL_H 46
#define CHROME_H (TAB_H + TOOL_H)
#define STATUS_H 22
#define MAX_TABS 24

typedef enum {
    KEY_NONE, KEY_BACKSPACE, KEY_DELETE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_ENTER, KEY_ESC, KEY_TAB, KEY_SPACE
} BhKey;

enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4 };
enum { CURSOR_ARROW, CURSOR_HAND, CURSOR_TEXT };

typedef struct {
    char *url;
    char *title;
    float scroll;
} HistEntry;

typedef struct Tab {
    int        id;
    Page      *page;
    HistEntry *hist;
    int        nhist, hpos, caphist;
    float      scroll_y, pending_scroll;
    bool       loading;
    int        load_id;
    bool       load_push;
    char      *load_fragment;
    char      *shown_url; /* what the address bar shows while this tab is active */
} Tab;

typedef struct {
    char *url, *title;
} Recent;

typedef struct Browser {
    Canvas *canvas;
    int     w, h;

    Tab *tabs[MAX_TABS];
    int  ntabs, cur, next_tab_id;

    Recent recent[6];
    int    nrecent;

    char  *addr; /* address bar editing buffer (UTF-8) */
    size_t addr_len, addr_cap, cursor;
    bool   addr_focus, addr_select_all;

    char       *status;
    const Node *hover_link;
    int         cursor_kind;
    bool        dirty, quit;
    int         notify_fd[2];

    /* backend hooks */
    void (*set_cursor)(void *ud, int kind);
    void (*set_title)(void *ud, const char *title);
    void (*clipboard_set)(void *ud, const char *text);
    void (*clipboard_request)(void *ud);
    void *ud;
} Browser;

Browser *browser_new(int w, int h);
void     browser_free(Browser *b);
Tab     *browser_tab(Browser *b);   /* active tab */
Page    *browser_page(Browser *b);  /* active page or NULL */
bool     browser_busy(Browser *b);  /* any tab loading */
void     browser_navigate(Browser *b, const char *url_or_search, bool push, bool user_typed);
void     browser_new_tab(Browser *b, const char *url_or_null);
void     browser_close_tab(Browser *b, int idx);
void     browser_switch_tab(Browser *b, int idx);
void     browser_loader_ready(Browser *b); /* call when notify_fd[0] is readable */
void     browser_resize(Browser *b, int w, int h);
void     browser_render(Browser *b);
void     browser_text(Browser *b, const char *utf8);
void     browser_key(Browser *b, BhKey key, int mods);
bool     browser_shortcut(Browser *b, char letter, int mods);
void     browser_mouse_move(Browser *b, int x, int y);
void     browser_mouse_down(Browser *b, int x, int y, int button);
void     browser_wheel(Browser *b, int x, int y, float dy);
void     browser_paste(Browser *b, const char *utf8);
void     browser_set_status(Browser *b, const char *s);

#endif
