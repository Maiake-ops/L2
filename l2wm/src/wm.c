#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

extern uint32_t l2wm_rect_ex(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static Display *d;
static Window root, *clients;
static unsigned count, focus, master_pct = 50, monocle = 0;
static unsigned long col_focus, col_unfocus;
static volatile sig_atomic_t running = 1;
static int startup = 1, conflict;

static void stop_wm(int sig) {
    (void)sig;
    running = 0;
}
static unsigned long alloc_color(const char *hex, unsigned long fallback) {
    XColor c, exact;
    Colormap cm = DefaultColormap(d, DefaultScreen(d));
    if (XAllocNamedColor(d, cm, hex, &c, &exact)) return c.pixel;
    return fallback;
}
static int error(Display *display, XErrorEvent *e) {
    (void)display;
    if (startup && e->error_code == BadAccess) conflict = 1;
    return 0; /* Windows can disappear between an event and a request. */
}
static int index_of(Window w) {
    for (unsigned i = 0; i < count; ++i) if (clients[i] == w) return (int)i;
    return -1;
}
static void apply_focus(void) {
    if (!count) {
        XSetInputFocus(d, root, RevertToPointerRoot, CurrentTime);
        XFlush(d);
        return;
    }
    focus %= count;
    for (unsigned i = 0; i < count; ++i)
        XSetWindowBorder(d, clients[i], i == focus ? col_focus : col_unfocus);
    if (monocle) XRaiseWindow(d, clients[focus]);
    XSetInputFocus(d, clients[focus], RevertToPointerRoot, CurrentTime);
    XFlush(d);
}
static void tile(void) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, root, &a)) return;
    for (unsigned i = 0; i < count; ++i) {
        unsigned v[4];
        for (unsigned j = 0; j < 4; ++j)
            v[j] = l2wm_rect_ex(i, count, (uint32_t)a.width, (uint32_t)a.height, master_pct, monocle, j);
        XMoveResizeWindow(d, clients[i], (int)v[0], (int)v[1],
                          v[2] > 4 ? v[2] - 4 : 1, v[3] > 4 ? v[3] - 4 : 1);
    }
    apply_focus();
}
static void add(Window w) {
    XWindowAttributes a;
    if (index_of(w) >= 0 || !XGetWindowAttributes(d, w, &a) || a.override_redirect) return;
    Window *p = realloc(clients, (count + 1) * sizeof(*clients));
    if (!p) { fprintf(stderr, "l2wm: out of memory\n"); exit(1); }
    clients = p; clients[count++] = w;
    XSetWindowBorderWidth(d, w, 2);
    XSetWindowBorder(d, w, col_unfocus);
    XSelectInput(d, w, EnterWindowMask);
}
static void remove_client(Window w) {
    int i = index_of(w);
    if (i < 0) return;
    for (unsigned j = (unsigned)i; j + 1 < count; ++j) clients[j] = clients[j+1];
    --count;
    if (count && focus >= count) focus = count - 1;
    tile();
}
static void close_client(void) {
    if (!count) return;
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False), *protocols;
    int n;
    if (XGetWMProtocols(d, clients[focus], &protocols, &n)) {
        for (int i = 0; i < n; ++i) if (protocols[i] == del) {
            XEvent e = {0}; e.xclient.type = ClientMessage; e.xclient.window = clients[focus];
            e.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", False);
            e.xclient.format = 32; e.xclient.data.l[0] = (long)del; e.xclient.data.l[1] = CurrentTime;
            XSendEvent(d, clients[focus], False, NoEventMask, &e); XFree(protocols); return;
        }
        XFree(protocols);
    }
    XKillClient(d, clients[focus]);
}
int main(void) {
    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "l2wm: cannot open DISPLAY; start an X server first\n"); return 1; }
    root = DefaultRootWindow(d);
    col_focus = alloc_color("#7ebae4", WhitePixel(d, DefaultScreen(d)));
    col_unfocus = alloc_color("#30363d", BlackPixel(d, DefaultScreen(d)));
    XSetErrorHandler(error);
    XSelectInput(d, root, SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask);
    XSync(d, False); startup = 0;
    if (conflict) { fprintf(stderr, "l2wm: another window manager is running\n"); XCloseDisplay(d); return 1; }
    signal(SIGCHLD, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = stop_wm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    KeySym keys[] = {XK_Return, XK_j, XK_k, XK_h, XK_l, XK_m, XK_f, XK_q, XK_Escape};
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        KeyCode kc = XKeysymToKeycode(d, keys[i]);
        if (!kc) continue;
        for (unsigned locks = 0; locks < 4; ++locks)
            XGrabKey(d, kc, Mod4Mask | ((locks & 1) ? LockMask : 0) | ((locks & 2) ? Mod2Mask : 0),
                     root, True, GrabModeAsync, GrabModeAsync);
    }
    Window parent, rr, *children; unsigned n;
    if (XQueryTree(d, root, &rr, &parent, &children, &n)) {
        for (unsigned i = 0; i < n; ++i) {
            XWindowAttributes a;
            if (XGetWindowAttributes(d, children[i], &a) && a.map_state == IsViewable) add(children[i]);
        }
        XFree(children);
    }
    tile();
    while (running) {
        XEvent e; XNextEvent(d, &e);
        if (!running) break;
        switch (e.type) {
        case MapRequest:
            add(e.xmaprequest.window);
            XMapWindow(d, e.xmaprequest.window);
            if (count) focus = count - 1;
            tile();
            break;
        case DestroyNotify: remove_client(e.xdestroywindow.window); break;
        case UnmapNotify:
            if (e.xunmap.event == root || e.xunmap.send_event) remove_client(e.xunmap.window);
            break;
        case EnterNotify:
            if (e.xcrossing.mode != NotifyNormal) break;
            { int i = index_of(e.xcrossing.window); if (i >= 0) { focus = (unsigned)i; apply_focus(); } }
            break;
        case ConfigureNotify: if (e.xconfigure.window == root) tile(); break;
        case ConfigureRequest: {
            XConfigureRequestEvent *r = &e.xconfigurerequest;
            if (index_of(r->window) >= 0) tile();
            else { XWindowChanges c = {r->x, r->y, r->width, r->height, r->border_width, r->above, r->detail};
                XConfigureWindow(d, r->window, (unsigned)r->value_mask, &c); }
            break;
        }
        case KeyPress:
            if (!(e.xkey.state & Mod4Mask)) break;
            switch (XLookupKeysym(&e.xkey, 0)) {
            case XK_Return:
                if (fork() == 0) { close(ConnectionNumber(d)); setsid();
                    const char *term = getenv("L2WM_TERMINAL");
                    execlp(term ? term : "xterm", term ? term : "xterm", (char *)NULL); _exit(127); }
                break;
            case XK_j: if (count) { focus = (focus + 1) % count; apply_focus(); } break;
            case XK_k: if (count) { focus = (focus + count - 1) % count; apply_focus(); } break;
            case XK_h: if (master_pct >= 25) master_pct -= 5; else master_pct = 20; tile(); break;
            case XK_l: if (master_pct <= 75) master_pct += 5; else master_pct = 80; tile(); break;
            case XK_m:
                if (count > 1 && focus > 0 && focus < count) {
                    Window tmp = clients[0]; clients[0] = clients[focus]; clients[focus] = tmp;
                    focus = 0; tile();
                }
                break;
            case XK_f: monocle = !monocle; tile(); break;
            case XK_q: close_client(); break;
            case XK_Escape: running = 0; break;
            }
        }
    }
    XSetInputFocus(d, PointerRoot, RevertToPointerRoot, CurrentTime);
    XCloseDisplay(d); free(clients); return 0;
}
