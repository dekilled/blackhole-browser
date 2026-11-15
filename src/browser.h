/* Browser shell: tab state, history, address bar, chrome drawing, input handling.
 * Backend independent: ui_x11.c feeds it abstract events. */
#ifndef BH_BROWSER_H
#define BH_BROWSER_H

#include "gfx.h"
#include "page.h"

#define CHROME_H 46
#define STATUS_H 22

typedef enum {
    KEY_NONE, KEY_BACKSPACE, KEY_DELETE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_ENTER, KEY_ESC, KEY_TAB, KEY_SPACE
} BhKey;

enum { MOD_CTRL = 1, MOD_ALT = 2, MOD_SHIFT = 4 };
enum { CURSOR_ARROW, CURSOR_HAND, CURSOR_TEXT };

typedef struct {
    char *url;
    float scroll;
} HistEntry;

typedef struct Browser {
    Canvas *canvas;
    int     w, h;
    Page   *page;

    HistEntry *hist;
    int        nhist, hpos, caphist;

    char  *addr; /* address bar contents (UTF-8) */
    size_t addr_len, addr_cap, cursor;
    bool   addr_focus, addr_select_all;

    float       scroll_y;
    float       pending_scroll; /* applied when a history navigation finishes */
    char       *status;
    const Node *hover_link;
    int         cursor_kind;
    bool        loading, dirty, quit;
    int         load_id;
    char       *loading_url;
    bool        load_push;
    char       *load_fragment;
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
void     browser_navigate(Browser *b, const char *url_or_search, bool push, bool user_typed);
void     browser_loader_ready(Browser *b); /* call when notify_fd[0] is readable */
void     browser_resize(Browser *b, int w, int h);
void     browser_render(Browser *b);
void     browser_text(Browser *b, const char *utf8);
void     browser_key(Browser *b, BhKey key, int mods);
void     browser_mouse_move(Browser *b, int x, int y);
void     browser_mouse_down(Browser *b, int x, int y, int button);
void     browser_wheel(Browser *b, int x, int y, float dy);
void     browser_paste(Browser *b, const char *utf8);
void     browser_set_status(Browser *b, const char *s);

#endif
