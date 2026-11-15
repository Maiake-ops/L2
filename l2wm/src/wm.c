#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

extern uint32_t l2wm_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static Display *d;
static Window root, *clients;
static unsigned count, focus;
static int running = 1, startup = 1, conflict;
static int error(Display *display, XErrorEvent *e) {
    (void)display;
    if (startup && e->error_code == BadAccess) conflict = 1;
    return 0; /* Windows can disappear between an event and a request. */
}
static int index_of(Window w) {
    for (unsigned i = 0; i < count; ++i) if (clients[i] == w) return (int)i;
    return -1;
}
static void tile(void) {
    XWindowAttributes a;
    XGetWindowAttributes(d, root, &a);
    for (unsigned i = 0; i < count; ++i) {
        unsigned v[4];
        for (unsigned j = 0; j < 4; ++j) v[j] = l2wm_rect(i, count, a.width, a.height, j);
        XMoveResizeWindow(d, clients[i], v[0], v[1], v[2] > 2 ? v[2]-2 : 1, v[3] > 2 ? v[3]-2 : 1);
    }
    if (count) { focus %= count; XSetInputFocus(d, clients[focus], RevertToPointerRoot, CurrentTime); }
    else XSetInputFocus(d, root, RevertToPointerRoot, CurrentTime);
    XFlush(d);
}
static void add(Window w) {
    XWindowAttributes a;
    if (index_of(w) >= 0 || !XGetWindowAttributes(d, w, &a) || a.override_redirect) return;
    Window *p = realloc(clients, (count + 1) * sizeof(*clients));
    if (!p) { fprintf(stderr, "l2wm: out of memory\n"); exit(1); }
    clients = p; clients[count++] = w;
    XSetWindowBorderWidth(d, w, 1);
    XSelectInput(d, w, EnterWindowMask);
}
static void remove_client(Window w) {
    int i = index_of(w);
    if (i < 0) return;
    for (unsigned j = (unsigned)i; j + 1 < count; ++j) clients[j] = clients[j+1];
    --count; tile();
}
static void close_client(void) {
    if (!count) return;
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False), *protocols;
    int n;
    if (XGetWMProtocols(d, clients[focus], &protocols, &n)) {
        for (int i = 0; i < n; ++i) if (protocols[i] == del) {
            XEvent e = {0}; e.xclient.type = ClientMessage; e.xclient.window = clients[focus];
            e.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", False);
            e.xclient.format = 32; e.xclient.data.l[0] = del; e.xclient.data.l[1] = CurrentTime;
            XSendEvent(d, clients[focus], False, NoEventMask, &e); XFree(protocols); return;
        }
        XFree(protocols);
    }
    XKillClient(d, clients[focus]);
}
int main(void) {
    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "l2wm: cannot open DISPLAY; start an X server first\n"); return 1; }
    root = DefaultRootWindow(d); XSetErrorHandler(error);
    XSelectInput(d, root, SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask);
    XSync(d, False); startup = 0;
    if (conflict) { fprintf(stderr, "l2wm: another window manager is running\n"); XCloseDisplay(d); return 1; }
    signal(SIGCHLD, SIG_IGN);
    KeySym keys[] = {XK_Return, XK_j, XK_k, XK_q, XK_Escape};
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i)
        for (unsigned locks = 0; locks < 4; ++locks)
            XGrabKey(d, XKeysymToKeycode(d, keys[i]), Mod4Mask | ((locks & 1) ? LockMask : 0) | ((locks & 2) ? Mod2Mask : 0), root, True, GrabModeAsync, GrabModeAsync);
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
        switch (e.type) {
        case MapRequest: add(e.xmaprequest.window); XMapWindow(d, e.xmaprequest.window); tile(); break;
        case DestroyNotify: remove_client(e.xdestroywindow.window); break;
        case UnmapNotify: remove_client(e.xunmap.window); break;
        case EnterNotify: { int i = index_of(e.xcrossing.window); if (i >= 0) { focus = i; tile(); } break; }
        case ConfigureNotify: if (e.xconfigure.window == root) tile(); break;
        case ConfigureRequest: {
            XConfigureRequestEvent *r = &e.xconfigurerequest;
            if (index_of(r->window) >= 0) tile();
            else { XWindowChanges c = {r->x, r->y, r->width, r->height, r->border_width, r->above, r->detail};
                XConfigureWindow(d, r->window, r->value_mask, &c); }
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
            case XK_j: if (count) { focus = (focus + 1) % count; tile(); } break;
            case XK_k: if (count) { focus = (focus + count - 1) % count; tile(); } break;
            case XK_q: close_client(); break;
            case XK_Escape: running = 0; break;
            }
        }
    }
    XSetInputFocus(d, PointerRoot, RevertToPointerRoot, CurrentTime);
    XCloseDisplay(d); free(clients); return 0;
}
