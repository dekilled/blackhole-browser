/* X11 backend: window, events, cursor, clipboard. Everything else lives in browser.c. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "browser.h"
#include "ui.h"

bool browser_shortcut(Browser *b, char letter, int mods);

typedef struct {
    Display *dpy;
    Window   win;
    GC       gc;
    XImage  *img;
    XIM      xim;
    XIC      xic;
    Cursor   cur[3];
    Atom     wm_delete, clipboard, utf8, targets, paste_prop, net_wm_name;
    char    *clip_text;
    Browser *b;
} X;

static void make_image(X *x) {
    Browser *b = x->b;
    if (x->img) {
        x->img->data = NULL; /* pixels belong to the canvas */
        XDestroyImage(x->img);
    }
    int depth = DefaultDepth(x->dpy, DefaultScreen(x->dpy));
    x->img = XCreateImage(x->dpy, DefaultVisual(x->dpy, DefaultScreen(x->dpy)), (unsigned)depth, ZPixmap, 0,
                          (char *)b->canvas->px, (unsigned)b->canvas->w, (unsigned)b->canvas->h, 32, 0);
}

static void cb_cursor(void *ud, int kind) {
    X *x = ud;
    XDefineCursor(x->dpy, x->win, x->cur[kind]);
    XFlush(x->dpy);
}

static void cb_title(void *ud, const char *title) {
    X *x = ud;
    Xutf8SetWMProperties(x->dpy, x->win, title, title, NULL, 0, NULL, NULL, NULL);
}

static void cb_clip_set(void *ud, const char *text) {
    X *x = ud;
    free(x->clip_text);
    x->clip_text = xstrdup(text);
    XSetSelectionOwner(x->dpy, x->clipboard, x->win, CurrentTime);
}

static void cb_clip_request(void *ud) {
    X *x = ud;
    XConvertSelection(x->dpy, x->clipboard, x->utf8, x->paste_prop, x->win, CurrentTime);
}

static BhKey map_key(KeySym ks) {
    switch (ks) {
    case XK_BackSpace: return KEY_BACKSPACE;
    case XK_Delete: case XK_KP_Delete: return KEY_DELETE;
    case XK_Left: case XK_KP_Left: return KEY_LEFT;
    case XK_Right: case XK_KP_Right: return KEY_RIGHT;
    case XK_Up: case XK_KP_Up: return KEY_UP;
    case XK_Down: case XK_KP_Down: return KEY_DOWN;
    case XK_Home: case XK_KP_Home: return KEY_HOME;
    case XK_End: case XK_KP_End: return KEY_END;
    case XK_Page_Up: case XK_KP_Page_Up: return KEY_PGUP;
    case XK_Page_Down: case XK_KP_Page_Down: return KEY_PGDN;
    case XK_Return: case XK_KP_Enter: return KEY_ENTER;
    case XK_Escape: return KEY_ESC;
    case XK_Tab: return KEY_TAB;
    case XK_space: return KEY_SPACE;
    default: return KEY_NONE;
    }
}

static void handle_key(X *x, XKeyEvent *ke) {
    Browser *b = x->b;
    char     buf[64];
    KeySym   ks = NoSymbol;
    int      n;
    if (x->xic) {
        Status st;
        n = Xutf8LookupString(x->xic, ke, buf, sizeof buf - 1, &ks, &st);
        if (st == XBufferOverflow) n = 0;
    } else {
        n = XLookupString(ke, buf, sizeof buf - 1, &ks, NULL);
    }
    if (n < 0) n = 0;
    buf[n] = 0;
    int mods = ((ke->state & ControlMask) ? MOD_CTRL : 0) | ((ke->state & Mod1Mask) ? MOD_ALT : 0) |
               ((ke->state & ShiftMask) ? MOD_SHIFT : 0);
    if ((mods & MOD_CTRL) && ((ks >= XK_a && ks <= XK_z) || (ks >= XK_A && ks <= XK_Z))) {
        char letter = (char)((ks >= XK_a ? ks - XK_a : ks - XK_A) + 'a');
        browser_shortcut(b, letter, mods);
        return;
    }
    BhKey k = map_key(ks);
    if (k == KEY_SPACE && b->addr_focus) k = KEY_NONE; /* typed as text */
    if (k != KEY_NONE) {
        browser_key(b, k, mods);
        return;
    }
    if (n > 0 && !(mods & (MOD_CTRL | MOD_ALT))) browser_text(b, buf);
}

static void handle_selection_request(X *x, XSelectionRequestEvent *rq) {
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify;
    ev.display = rq->display;
    ev.requestor = rq->requestor;
    ev.selection = rq->selection;
    ev.target = rq->target;
    ev.time = rq->time;
    ev.property = None;
    Atom prop = rq->property ? rq->property : rq->target;
    if (rq->target == x->targets) {
        Atom t[3] = {x->targets, x->utf8, XA_STRING};
        XChangeProperty(x->dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
        ev.property = prop;
    } else if ((rq->target == x->utf8 || rq->target == XA_STRING) && x->clip_text) {
        XChangeProperty(x->dpy, rq->requestor, prop, rq->target, 8, PropModeReplace, (unsigned char *)x->clip_text,
                        (int)strlen(x->clip_text));
        ev.property = prop;
    }
    XSendEvent(x->dpy, rq->requestor, False, 0, (XEvent *)&ev);
}

static void handle_selection_notify(X *x, XSelectionEvent *se) {
    if (se->property == None) return;
    Atom           type;
    int            fmt;
    unsigned long  nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(x->dpy, x->win, se->property, 0, 65536, True, AnyPropertyType, &type, &fmt, &nitems, &after, &data) == Success && data) {
        if (fmt == 8 && nitems > 0 && nitems < 65536) {
            char *s = xstrndup((char *)data, nitems);
            browser_paste(x->b, s);
            free(s);
        }
        XFree(data);
    }
}

int ui_x11_run(Browser *b, const char *initial_url) {
    X x;
    memset(&x, 0, sizeof x);
    x.b = b;
    XSetLocaleModifiers("");
    x.dpy = XOpenDisplay(NULL);
    if (!x.dpy) {
        fprintf(stderr, "não foi possível abrir o display X11 (DISPLAY não definido?)\n");
        return 1;
    }
    int scr = DefaultScreen(x.dpy);
    x.win = XCreateSimpleWindow(x.dpy, RootWindow(x.dpy, scr), 0, 0, (unsigned)b->w, (unsigned)b->h, 0, 0, 0xFF101014);
    XSelectInput(x.dpy, x.win, ExposureMask | KeyPressMask | ButtonPressMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask);
    x.wm_delete = XInternAtom(x.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(x.dpy, x.win, &x.wm_delete, 1);
    x.clipboard = XInternAtom(x.dpy, "CLIPBOARD", False);
    x.utf8 = XInternAtom(x.dpy, "UTF8_STRING", False);
    x.targets = XInternAtom(x.dpy, "TARGETS", False);
    x.paste_prop = XInternAtom(x.dpy, "BLACKHOLE_PASTE", False);
    x.cur[CURSOR_ARROW] = XCreateFontCursor(x.dpy, XC_left_ptr);
    x.cur[CURSOR_HAND] = XCreateFontCursor(x.dpy, XC_hand2);
    x.cur[CURSOR_TEXT] = XCreateFontCursor(x.dpy, XC_xterm);
    XDefineCursor(x.dpy, x.win, x.cur[CURSOR_ARROW]);
    x.gc = XCreateGC(x.dpy, x.win, 0, NULL);
    x.xim = XOpenIM(x.dpy, NULL, NULL, NULL);
    if (x.xim)
        x.xic = XCreateIC(x.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, x.win, XNFocusWindow, x.win, NULL);
    Xutf8SetWMProperties(x.dpy, x.win, "BlackHole", "BlackHole", NULL, 0, NULL, NULL, NULL);
    XMapWindow(x.dpy, x.win);
    make_image(&x);

    b->ud = &x;
    b->set_cursor = cb_cursor;
    b->set_title = cb_title;
    b->clipboard_set = cb_clip_set;
    b->clipboard_request = cb_clip_request;

    if (initial_url) browser_navigate(b, initial_url, true, true);

    struct pollfd fds[2] = {{ConnectionNumber(x.dpy), POLLIN, 0}, {b->notify_fd[0], POLLIN, 0}};
    while (!b->quit) {
        while (XPending(x.dpy)) {
            XEvent ev;
            XNextEvent(x.dpy, &ev);
            if (XFilterEvent(&ev, None)) continue;
            switch (ev.type) {
            case Expose: b->dirty = true; break;
            case ConfigureNotify:
                if (ev.xconfigure.width != b->w || ev.xconfigure.height != b->h) {
                    browser_resize(b, ev.xconfigure.width, ev.xconfigure.height);
                    make_image(&x);
                }
                break;
            case KeyPress: handle_key(&x, &ev.xkey); break;
            case MotionNotify: browser_mouse_move(b, ev.xmotion.x, ev.xmotion.y); break;
            case ButtonPress: {
                unsigned bt = ev.xbutton.button;
                if (bt == 4) browser_wheel(b, ev.xbutton.x, ev.xbutton.y, -64);
                else if (bt == 5) browser_wheel(b, ev.xbutton.x, ev.xbutton.y, 64);
                else if (bt == 8) browser_key(b, KEY_LEFT, MOD_ALT);
                else if (bt == 9) browser_key(b, KEY_RIGHT, MOD_ALT);
                else browser_mouse_down(b, ev.xbutton.x, ev.xbutton.y, (int)bt);
                break;
            }
            case SelectionRequest: handle_selection_request(&x, &ev.xselectionrequest); break;
            case SelectionNotify: handle_selection_notify(&x, &ev.xselection); break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == x.wm_delete) b->quit = true;
                break;
            default: break;
            }
        }
        if (b->quit) break;
        if (b->dirty) {
            browser_render(b);
            XPutImage(x.dpy, x.win, x.gc, x.img, 0, 0, 0, 0, (unsigned)b->canvas->w, (unsigned)b->canvas->h);
            XFlush(x.dpy);
        }
        fds[0].revents = fds[1].revents = 0;
        poll(fds, 2, -1);
        if (fds[1].revents & POLLIN) browser_loader_ready(b);
    }
    if (x.img) {
        x.img->data = NULL;
        XDestroyImage(x.img);
    }
    if (x.xic) XDestroyIC(x.xic);
    if (x.xim) XCloseIM(x.xim);
    XFreeGC(x.dpy, x.gc);
    XDestroyWindow(x.dpy, x.win);
    XCloseDisplay(x.dpy);
    free(x.clip_text);
    return 0;
}
