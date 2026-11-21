#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#define NUM_WORKSPACES 9
#define LAYOUT_TILE    0
#define LAYOUT_MONOCLE 1
#define MAX_TERM_LEN   128

typedef struct {
    Window win;
    unsigned ws;
    int floating;
    int mapped;
    unsigned ignore_unmap;
    int x, y;
    unsigned w, h;
} Client;

typedef struct {
    unsigned master_percent;
    unsigned border_width;
    unsigned gap;
    unsigned long focus_color;
    unsigned long unfocus_color;
    char terminal[MAX_TERM_LEN];
} Config;

static Config cfg = {
    .master_percent = 50,
    .border_width = 2,
    .gap = 0,
    .focus_color = 0x5e81ac,
    .unfocus_color = 0x2e3440,
    .terminal = "xterm",
};

static Display *dpy;
static int screen;
static Window root;
static Window wm_check;
static Cursor root_cursor;

static Client *clients;
static unsigned client_count;
static unsigned client_cap;

static unsigned cur_ws;
static int ws_layout[NUM_WORKSPACES];
static unsigned ws_master[NUM_WORKSPACES];
static Window ws_focus[NUM_WORKSPACES];

static int startup = 1;
static int conflict = 0;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_pending = 0;

/* Drag/resize state for floating windows */
static Window drag_win = None;
static int drag_mode = 0; /* 1 = move, 3 = resize */
static int drag_start_x, drag_start_y;
static int drag_win_x, drag_win_y;
static unsigned drag_win_w, drag_win_h;

/* Atoms */
static Atom atom_wm_protocols;
static Atom atom_wm_delete;
static Atom atom_utf8;
static Atom atom_net_supported;
static Atom atom_net_wm_check;
static Atom atom_net_wm_name;
static Atom atom_net_active;
static Atom atom_net_client_list;
static Atom atom_net_cur_desktop;
static Atom atom_net_num_desktops;

static void handle_signal(int sig) {
    if (sig == SIGHUP) {
        reload_pending = 1;
    } else {
        running = 0;
    }
}

static int xerror(Display *display, XErrorEvent *e) {
    (void)display;
    if (startup && e->error_code == BadAccess) conflict = 1;
    return 0; /* Windows can disappear between an event and a request. */
}

static int parse_color(const char *val, unsigned long *out) {
    if (!val || !dpy) return 0;
    XColor col;
    Colormap cmap = DefaultColormap(dpy, screen);
    if (XParseColor(dpy, cmap, val, &col) && XAllocColor(dpy, cmap, &col)) {
        *out = col.pixel;
        return 1;
    }
    return 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

static void load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (strcmp(key, "master_percent") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v >= 10 && v <= 90) cfg.master_percent = (unsigned)v;
        } else if (strcmp(key, "border_width") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v <= 16) cfg.border_width = (unsigned)v;
        } else if (strcmp(key, "gap") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v <= 64) cfg.gap = (unsigned)v;
        } else if (strcmp(key, "focus_color") == 0) {
            parse_color(val, &cfg.focus_color);
        } else if (strcmp(key, "unfocus_color") == 0) {
            parse_color(val, &cfg.unfocus_color);
        } else if (strcmp(key, "terminal") == 0 && *val) {
            snprintf(cfg.terminal, sizeof(cfg.terminal), "%s", val);
        }
    }
    fclose(f);
}

static void reload_config(void) {
    cfg.master_percent = 50;
    cfg.border_width = 2;
    cfg.gap = 0;
    snprintf(cfg.terminal, sizeof(cfg.terminal), "%s", "xterm");
    if (dpy) {
        parse_color("#5e81ac", &cfg.focus_color);
        parse_color("#2e3440", &cfg.unfocus_color);
    }

    load_config_file("/etc/l2/l2wm.conf");
    const char *home = getenv("HOME");
    if (home && *home) {
        char user_conf[512];
        int n = snprintf(user_conf, sizeof(user_conf), "%s/.config/l2wm/l2wm.conf", home);
        if (n > 0 && (size_t)n < sizeof(user_conf)) load_config_file(user_conf);
    }

    for (unsigned i = 0; i < NUM_WORKSPACES; ++i) {
        ws_master[i] = cfg.master_percent;
    }
}

static int index_of(Window w) {
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].win == w) return (int)i;
    }
    return -1;
}

static void update_client_list(void) {
    Window *list = NULL;
    if (client_count > 0) {
        list = malloc(client_count * sizeof(*list));
        if (list) {
            for (unsigned i = 0; i < client_count; ++i) list[i] = clients[i].win;
        }
    }
    XChangeProperty(dpy, root, atom_net_client_list, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)list, list ? (int)client_count : 0);
    free(list);
}

static void update_ewmh_desktop(void) {
    long cur = (long)cur_ws;
    XChangeProperty(dpy, root, atom_net_cur_desktop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&cur, 1);
}

static int first_on_ws(unsigned ws) {
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws == ws) return (int)i;
    }
    return -1;
}

static void apply_focus(void) {
    Window target = ws_focus[cur_ws];
    int idx = (target != None) ? index_of(target) : -1;
    if (idx < 0 || clients[idx].ws != cur_ws) {
        idx = first_on_ws(cur_ws);
        target = (idx >= 0) ? clients[idx].win : None;
        ws_focus[cur_ws] = target;
    }

    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws != cur_ws) continue;
        int is_focused = (clients[i].win == target);
        XSetWindowBorderWidth(dpy, clients[i].win,
                              (ws_layout[cur_ws] == LAYOUT_MONOCLE && !clients[i].floating) ? 0 : cfg.border_width);
        XSetWindowBorder(dpy, clients[i].win, is_focused ? cfg.focus_color : cfg.unfocus_color);
        if (is_focused) {
            XUngrabButton(dpy, Button1, AnyModifier, clients[i].win);
            if (ws_layout[cur_ws] == LAYOUT_MONOCLE || clients[i].floating) {
                XRaiseWindow(dpy, clients[i].win);
            }
        } else {
            XGrabButton(dpy, Button1, AnyModifier, clients[i].win, True,
                        ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
        }
    }

    if (target != None) {
        XSetInputFocus(dpy, target, RevertToPointerRoot, CurrentTime);
        XChangeProperty(dpy, root, atom_net_active, XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)&target, 1);
    } else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
        XDeleteProperty(dpy, root, atom_net_active);
    }
    XFlush(dpy);
}

static void tile(void) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, root, &a)) return;
    unsigned sw = (unsigned)a.width;
    unsigned sh = (unsigned)a.height;
    unsigned bw = cfg.border_width;
    unsigned gap = cfg.gap;

    /* Count tiled clients on current workspace */
    unsigned tiled_n = 0;
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws == cur_ws && !clients[i].floating) ++tiled_n;
    }

    if (tiled_n > 0) {
        if (ws_layout[cur_ws] == LAYOUT_MONOCLE) {
            for (unsigned i = 0; i < client_count; ++i) {
                if (clients[i].ws != cur_ws || clients[i].floating) continue;
                XSetWindowBorderWidth(dpy, clients[i].win, 0);
                XMoveResizeWindow(dpy, clients[i].win, 0, 0, sw ? sw : 1, sh ? sh : 1);
            }
        } else {
            unsigned usable_w = (sw > 2 * gap) ? (sw - 2 * gap) : sw;
            unsigned usable_h = (sh > 2 * gap) ? (sh - 2 * gap) : sh;
            unsigned ox = (sw > 2 * gap) ? gap : 0;
            unsigned oy = (sh > 2 * gap) ? gap : 0;

            unsigned master_w = (tiled_n == 1) ? usable_w
                              : (usable_w > gap ? (usable_w - gap) * ws_master[cur_ws] / 100 : usable_w / 2);
            unsigned stack_w = (usable_w > master_w + gap) ? (usable_w - master_w - gap) : 1;
            unsigned stack_n = (tiled_n > 1) ? (tiled_n - 1) : 1;

            unsigned ti = 0;
            for (unsigned i = 0; i < client_count; ++i) {
                if (clients[i].ws != cur_ws || clients[i].floating) continue;
                unsigned x, y, w, h;
                if (ti == 0) {
                    x = ox;
                    y = oy;
                    w = master_w;
                    h = usable_h;
                } else {
                    unsigned si = ti - 1;
                    unsigned total_gaps = (stack_n > 1) ? gap * (stack_n - 1) : 0;
                    unsigned avail_h = (usable_h > total_gaps) ? (usable_h - total_gaps) : usable_h;
                    unsigned y0 = (unsigned)((uint64_t)avail_h * si / stack_n);
                    unsigned y1 = (unsigned)((uint64_t)avail_h * (si + 1) / stack_n);
                    x = ox + master_w + gap;
                    y = oy + y0 + si * gap;
                    w = stack_w;
                    h = (y1 > y0) ? (y1 - y0) : 1;
                }
                unsigned inner_w = (w > 2 * bw) ? (w - 2 * bw) : 1;
                unsigned inner_h = (h > 2 * bw) ? (h - 2 * bw) : 1;
                XSetWindowBorderWidth(dpy, clients[i].win, bw);
                XMoveResizeWindow(dpy, clients[i].win, (int)x, (int)y, inner_w, inner_h);
                ++ti;
            }
        }
    }

    /* Keep floating windows raised above tiled windows */
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws == cur_ws && clients[i].floating) {
            XSetWindowBorderWidth(dpy, clients[i].win, bw);
            XMoveResizeWindow(dpy, clients[i].win, clients[i].x, clients[i].y,
                              clients[i].w ? clients[i].w : 1, clients[i].h ? clients[i].h : 1);
            XRaiseWindow(dpy, clients[i].win);
        }
    }

    apply_focus();
}

static void add_client(Window w, int focus_new) {
    XWindowAttributes a;
    if (w == wm_check || index_of(w) >= 0) return;
    if (!XGetWindowAttributes(dpy, w, &a) || a.override_redirect) return;

    if (client_count == client_cap) {
        unsigned new_cap = client_cap ? client_cap * 2 : 16;
        Client *p = realloc(clients, new_cap * sizeof(*clients));
        if (!p) {
            fprintf(stderr, "l2wm: out of memory\n");
            exit(1);
        }
        clients = p;
        client_cap = new_cap;
    }

    Window trans = None;
    int is_floating = (XGetTransientForHint(dpy, w, &trans) && trans != None);
    int cx = a.x, cy = a.y;
    unsigned cw = (a.width > 0) ? (unsigned)a.width : 400;
    unsigned ch = (a.height > 0) ? (unsigned)a.height : 300;

    if (is_floating) {
        XWindowAttributes ra;
        if (XGetWindowAttributes(dpy, root, &ra)) {
            if (cw > (unsigned)ra.width) cw = (unsigned)ra.width;
            if (ch > (unsigned)ra.height) ch = (unsigned)ra.height;
            cx = (ra.width - (int)cw) / 2;
            cy = (ra.height - (int)ch) / 2;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
        }
    }

    Client *c = &clients[client_count++];
    c->win = w;
    c->ws = cur_ws;
    c->floating = is_floating;
    c->mapped = 1;
    c->ignore_unmap = 0;
    c->x = cx;
    c->y = cy;
    c->w = cw;
    c->h = ch;

    XSetWindowBorderWidth(dpy, w, cfg.border_width);
    XSelectInput(dpy, w, EnterWindowMask);

    if (focus_new || ws_focus[cur_ws] == None) {
        ws_focus[cur_ws] = w;
    }
    update_client_list();
}

static void remove_client(Window w) {
    int i = index_of(w);
    if (i < 0) return;
    unsigned ws = clients[i].ws;
    int was_focused = (ws_focus[ws] == w);

    if (drag_win == w) {
        drag_win = None;
        drag_mode = 0;
    }

    for (unsigned j = (unsigned)i; j + 1 < client_count; ++j) {
        clients[j] = clients[j + 1];
    }
    --client_count;

    if (was_focused) {
        Window next = None;
        for (unsigned j = (unsigned)i; j < client_count; ++j) {
            if (clients[j].ws == ws) { next = clients[j].win; break; }
        }
        if (next == None) {
            for (int j = i - 1; j >= 0; --j) {
                if (clients[j].ws == ws) { next = clients[j].win; break; }
            }
        }
        ws_focus[ws] = next;
    }

    update_client_list();
    if (ws == cur_ws) tile();
}

static void close_focused(void) {
    Window target = ws_focus[cur_ws];
    if (target == None) return;
    Atom *protocols = NULL;
    int n = 0;
    if (XGetWMProtocols(dpy, target, &protocols, &n)) {
        for (int i = 0; i < n; ++i) {
            if (protocols[i] == atom_wm_delete) {
                XEvent e;
                memset(&e, 0, sizeof(e));
                e.xclient.type = ClientMessage;
                e.xclient.window = target;
                e.xclient.message_type = atom_wm_protocols;
                e.xclient.format = 32;
                e.xclient.data.l[0] = (long)atom_wm_delete;
                e.xclient.data.l[1] = CurrentTime;
                XSendEvent(dpy, target, False, NoEventMask, &e);
                XFree(protocols);
                XFlush(dpy);
                return;
            }
        }
        XFree(protocols);
    }
    XKillClient(dpy, target);
    XFlush(dpy);
}

static void cycle_focus(int dir) {
    unsigned ws_indices[256];
    unsigned n = 0;
    int cur_pos = -1;

    for (unsigned i = 0; i < client_count && n < 256; ++i) {
        if (clients[i].ws != cur_ws) continue;
        if (clients[i].win == ws_focus[cur_ws]) cur_pos = (int)n;
        ws_indices[n++] = i;
    }
    if (n == 0) return;
    if (cur_pos < 0) cur_pos = 0;
    unsigned next_pos = (dir > 0) ? ((unsigned)(cur_pos + 1) % n)
                                  : ((unsigned)(cur_pos + (int)n - 1) % n);
    ws_focus[cur_ws] = clients[ws_indices[next_pos]].win;
    apply_focus();
}

static void swap_focused(int dir) {
    int idx = index_of(ws_focus[cur_ws]);
    if (idx < 0 || clients[idx].ws != cur_ws || clients[idx].floating) return;

    unsigned tiled_indices[256];
    unsigned n = 0;
    int cur_pos = -1;
    for (unsigned i = 0; i < client_count && n < 256; ++i) {
        if (clients[i].ws != cur_ws || clients[i].floating) continue;
        if ((int)i == idx) cur_pos = (int)n;
        tiled_indices[n++] = i;
    }
    if (n < 2 || cur_pos < 0) return;

    unsigned other_pos = (dir > 0) ? ((unsigned)(cur_pos + 1) % n)
                                   : ((unsigned)(cur_pos + (int)n - 1) % n);
    unsigned a = tiled_indices[cur_pos];
    unsigned b = tiled_indices[other_pos];
    Client tmp = clients[a];
    clients[a] = clients[b];
    clients[b] = tmp;
    tile();
}

static void promote_master(void) {
    int idx = index_of(ws_focus[cur_ws]);
    if (idx < 0 || clients[idx].ws != cur_ws || clients[idx].floating) return;

    int first = -1;
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws == cur_ws && !clients[i].floating) {
            first = (int)i;
            break;
        }
    }
    if (first < 0) return;
    if (first == idx) {
        /* If already master, swap with second tiled window */
        for (unsigned i = (unsigned)first + 1; i < client_count; ++i) {
            if (clients[i].ws == cur_ws && !clients[i].floating) {
                idx = (int)i;
                break;
            }
        }
        if (first == idx) return;
    }

    Client tmp = clients[first];
    clients[first] = clients[idx];
    clients[idx] = tmp;
    ws_focus[cur_ws] = clients[first].win;
    tile();
}

static void adjust_master(int delta) {
    int next = (int)ws_master[cur_ws] + delta;
    if (next < 10) next = 10;
    if (next > 90) next = 90;
    ws_master[cur_ws] = (unsigned)next;
    tile();
}

static void toggle_floating(void) {
    int idx = index_of(ws_focus[cur_ws]);
    if (idx < 0 || clients[idx].ws != cur_ws) return;
    clients[idx].floating = !clients[idx].floating;
    if (clients[idx].floating) {
        XWindowAttributes a;
        if (XGetWindowAttributes(dpy, clients[idx].win, &a)) {
            clients[idx].x = a.x;
            clients[idx].y = a.y;
            clients[idx].w = (a.width > 0) ? (unsigned)a.width : 400;
            clients[idx].h = (a.height > 0) ? (unsigned)a.height : 300;
        }
    }
    tile();
}

static void switch_workspace(unsigned target_ws) {
    if (target_ws >= NUM_WORKSPACES || target_ws == cur_ws) return;
    unsigned old_ws = cur_ws;
    cur_ws = target_ws;

    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws == cur_ws && !clients[i].mapped) {
            clients[i].mapped = 1;
            XMapWindow(dpy, clients[i].win);
        }
    }
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws == old_ws && clients[i].mapped) {
            clients[i].mapped = 0;
            clients[i].ignore_unmap++;
            XUnmapWindow(dpy, clients[i].win);
        }
    }

    update_ewmh_desktop();
    tile();
}

static void send_to_workspace(unsigned target_ws) {
    if (target_ws >= NUM_WORKSPACES || target_ws == cur_ws) return;
    int idx = index_of(ws_focus[cur_ws]);
    if (idx < 0 || clients[idx].ws != cur_ws) return;

    clients[idx].ws = target_ws;
    ws_focus[target_ws] = clients[idx].win;
    if (clients[idx].mapped) {
        clients[idx].mapped = 0;
        clients[idx].ignore_unmap++;
        XUnmapWindow(dpy, clients[idx].win);
    }

    ws_focus[cur_ws] = None;
    int next = first_on_ws(cur_ws);
    if (next >= 0) ws_focus[cur_ws] = clients[next].win;
    tile();
}

static void spawn_terminal(void) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        close(ConnectionNumber(dpy));
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        const char *term = getenv("L2WM_TERMINAL");
        if (!term || !*term) term = cfg.terminal;
        if (strcmp(term, "xterm") == 0) {
            execlp("xterm", "xterm", "-fn", "fixed", "-sl", "256", (char *)NULL);
        } else {
            execlp(term, term, (char *)NULL);
        }
        _exit(127);
    }
}

static void grab_key_sym(KeySym sym, unsigned int mod) {
    KeyCode code = XKeysymToKeycode(dpy, sym);
    if (!code) return;
    for (unsigned locks = 0; locks < 4; ++locks) {
        unsigned int m = mod | ((locks & 1) ? LockMask : 0) | ((locks & 2) ? Mod2Mask : 0);
        XGrabKey(dpy, code, m, root, True, GrabModeAsync, GrabModeAsync);
    }
}

static void grab_inputs(void) {
    KeySym base_keys[] = {
        XK_Return, XK_j, XK_k, XK_h, XK_l, XK_m, XK_t, XK_f, XK_space, XK_q, XK_r, XK_Escape
    };
    for (unsigned i = 0; i < sizeof(base_keys) / sizeof(base_keys[0]); ++i) {
        grab_key_sym(base_keys[i], Mod4Mask);
    }

    KeySym shift_keys[] = { XK_Return, XK_j, XK_k };
    for (unsigned i = 0; i < sizeof(shift_keys) / sizeof(shift_keys[0]); ++i) {
        grab_key_sym(shift_keys[i], Mod4Mask | ShiftMask);
    }

    for (unsigned ws = 0; ws < NUM_WORKSPACES; ++ws) {
        KeySym num = (KeySym)(XK_1 + ws);
        grab_key_sym(num, Mod4Mask);
        grab_key_sym(num, Mod4Mask | ShiftMask);
    }

    for (unsigned locks = 0; locks < 4; ++locks) {
        unsigned int m = Mod4Mask | ((locks & 1) ? LockMask : 0) | ((locks & 2) ? Mod2Mask : 0);
        XGrabButton(dpy, Button1, m, root, True,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, None);
        XGrabButton(dpy, Button3, m, root, True,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, None);
    }
}

static void init_ewmh(void) {
    atom_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    atom_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    atom_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    atom_net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    atom_net_wm_check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    atom_net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    atom_net_cur_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    atom_net_num_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);

    Atom supported[] = {
        atom_net_supported, atom_net_wm_check, atom_net_wm_name,
        atom_net_active, atom_net_client_list,
        atom_net_cur_desktop, atom_net_num_desktops,
    };
    XChangeProperty(dpy, root, atom_net_supported, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)supported, (int)(sizeof(supported) / sizeof(supported[0])));

    wm_check = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, wm_check, atom_net_wm_check, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&wm_check, 1);
    XChangeProperty(dpy, wm_check, atom_net_wm_name, atom_utf8, 8, PropModeReplace,
                    (const unsigned char *)"l2wm", 4);
    XChangeProperty(dpy, root, atom_net_wm_check, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&wm_check, 1);

    long ndesk = NUM_WORKSPACES;
    XChangeProperty(dpy, root, atom_net_num_desktops, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&ndesk, 1);
    update_ewmh_desktop();
    update_client_list();
}

static void handle_key_press(XKeyEvent *ev) {
    if (!(ev->state & Mod4Mask)) return;
    int shifted = (ev->state & ShiftMask) != 0;
    KeySym sym = XLookupKeysym(ev, 0);

    if (sym >= XK_1 && sym <= XK_9) {
        unsigned target_ws = (unsigned)(sym - XK_1);
        if (shifted) send_to_workspace(target_ws);
        else switch_workspace(target_ws);
        return;
    }

    switch (sym) {
    case XK_Return:
        if (shifted) promote_master();
        else spawn_terminal();
        break;
    case XK_j:
        if (shifted) swap_focused(1);
        else cycle_focus(1);
        break;
    case XK_k:
        if (shifted) swap_focused(-1);
        else cycle_focus(-1);
        break;
    case XK_h: adjust_master(-5); break;
    case XK_l: adjust_master(5); break;
    case XK_m: promote_master(); break;
    case XK_t: toggle_floating(); break;
    case XK_f:
    case XK_space:
        ws_layout[cur_ws] = (ws_layout[cur_ws] == LAYOUT_TILE) ? LAYOUT_MONOCLE : LAYOUT_TILE;
        tile();
        break;
    case XK_r: reload_config(); tile(); break;
    case XK_q: close_focused(); break;
    case XK_Escape: running = 0; break;
    default: break;
    }
}

static void handle_button_press(XButtonEvent *ev) {
    Window w = (ev->window == root && ev->subwindow != None) ? ev->subwindow : ev->window;
    int idx = index_of(w);

    if ((ev->state & Mod4Mask) && idx >= 0 && clients[idx].ws == cur_ws) {
        ws_focus[cur_ws] = w;
        if (!clients[idx].floating) {
            clients[idx].floating = 1;
            XWindowAttributes a;
            if (XGetWindowAttributes(dpy, w, &a)) {
                clients[idx].x = a.x;
                clients[idx].y = a.y;
                clients[idx].w = (a.width > 0) ? (unsigned)a.width : 400;
                clients[idx].h = (a.height > 0) ? (unsigned)a.height : 300;
            }
            tile();
        } else {
            apply_focus();
        }
        if (ev->button == Button1 || ev->button == Button3) {
            drag_win = w;
            drag_mode = (int)ev->button;
            drag_start_x = ev->x_root;
            drag_start_y = ev->y_root;
            drag_win_x = clients[idx].x;
            drag_win_y = clients[idx].y;
            drag_win_w = clients[idx].w;
            drag_win_h = clients[idx].h;
        }
        return;
    }

    if (idx >= 0 && clients[idx].ws == cur_ws) {
        ws_focus[cur_ws] = w;
        apply_focus();
    }
    XAllowEvents(dpy, ReplayPointer, ev->time);
}

static void handle_motion(XMotionEvent *ev) {
    if (drag_win == None || !drag_mode) return;
    int idx = index_of(drag_win);
    if (idx < 0 || !clients[idx].floating) {
        drag_win = None;
        drag_mode = 0;
        return;
    }
    int dx = ev->x_root - drag_start_x;
    int dy = ev->y_root - drag_start_y;
    if (drag_mode == Button1) {
        clients[idx].x = drag_win_x + dx;
        clients[idx].y = drag_win_y + dy;
    } else if (drag_mode == Button3) {
        int nw = (int)drag_win_w + dx;
        int nh = (int)drag_win_h + dy;
        clients[idx].w = (nw > 40) ? (unsigned)nw : 40;
        clients[idx].h = (nh > 40) ? (unsigned)nh : 40;
    }
    XMoveResizeWindow(dpy, drag_win, clients[idx].x, clients[idx].y, clients[idx].w, clients[idx].h);
    XFlush(dpy);
}

static void handle_event(XEvent *e) {
    switch (e->type) {
    case MapRequest:
        add_client(e->xmaprequest.window, 1);
        XMapWindow(dpy, e->xmaprequest.window);
        tile();
        break;
    case DestroyNotify:
        remove_client(e->xdestroywindow.window);
        break;
    case UnmapNotify: {
        int idx = index_of(e->xunmap.window);
        if (idx >= 0) {
            if (clients[idx].ignore_unmap > 0) {
                clients[idx].ignore_unmap--;
            } else {
                remove_client(e->xunmap.window);
            }
        }
        break;
    }
    case EnterNotify: {
        int idx = index_of(e->xcrossing.window);
        if (idx >= 0 && clients[idx].ws == cur_ws && ws_focus[cur_ws] != clients[idx].win) {
            ws_focus[cur_ws] = clients[idx].win;
            apply_focus();
        }
        break;
    }
    case ConfigureNotify:
        if (e->xconfigure.window == root) tile();
        break;
    case ConfigureRequest: {
        XConfigureRequestEvent *r = &e->xconfigurerequest;
        int idx = index_of(r->window);
        if (idx >= 0 && !clients[idx].floating) {
            tile();
        } else {
            if (idx >= 0 && clients[idx].floating) {
                if (r->value_mask & CWX) clients[idx].x = r->x;
                if (r->value_mask & CWY) clients[idx].y = r->y;
                if (r->value_mask & CWWidth && r->width > 0) clients[idx].w = (unsigned)r->width;
                if (r->value_mask & CWHeight && r->height > 0) clients[idx].h = (unsigned)r->height;
            }
            XWindowChanges c = {
                r->x, r->y, r->width, r->height, r->border_width, r->above, r->detail
            };
            XConfigureWindow(dpy, r->window, r->value_mask, &c);
        }
        break;
    }
    case KeyPress:
        handle_key_press(&e->xkey);
        break;
    case ButtonPress:
        handle_button_press(&e->xbutton);
        break;
    case MotionNotify:
        while (XCheckTypedEvent(dpy, MotionNotify, e)) { /* compress motion */ }
        handle_motion(&e->xmotion);
        break;
    case ButtonRelease:
        drag_win = None;
        drag_mode = 0;
        break;
    default:
        break;
    }
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "l2wm: cannot open DISPLAY; start an X server first\n");
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);
    XSetErrorHandler(xerror);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | StructureNotifyMask);
    XSync(dpy, False);
    startup = 0;
    if (conflict) {
        fprintf(stderr, "l2wm: another window manager is running\n");
        XCloseDisplay(dpy);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);

    reload_config();
    init_ewmh();

    root_cursor = XCreateFontCursor(dpy, XC_left_ptr);
    XDefineCursor(dpy, root, root_cursor);
    grab_inputs();

    Window parent, rr, *children = NULL;
    unsigned n = 0;
    if (XQueryTree(dpy, root, &rr, &parent, &children, &n)) {
        for (unsigned i = 0; i < n; ++i) {
            XWindowAttributes a;
            if (XGetWindowAttributes(dpy, children[i], &a) && a.map_state == IsViewable) {
                add_client(children[i], 0);
            }
        }
        if (children) XFree(children);
    }
    tile();

    int xfd = ConnectionNumber(dpy);
    while (running) {
        if (reload_pending) {
            reload_pending = 0;
            reload_config();
            tile();
        }
        while (running && XPending(dpy) > 0) {
            XEvent e;
            XNextEvent(dpy, &e);
            handle_event(&e);
        }
        if (!running) break;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        if (select(xfd + 1, &fds, NULL, NULL, NULL) < 0 && errno != EINTR) break;
    }

    /* Remap hidden workspace windows on exit so clients aren't left unmapped */
    for (unsigned i = 0; i < client_count; ++i) {
        if (!clients[i].mapped) XMapWindow(dpy, clients[i].win);
    }
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    XDeleteProperty(dpy, root, atom_net_active);
    XDeleteProperty(dpy, root, atom_net_client_list);
    XDeleteProperty(dpy, root, atom_net_wm_check);
    if (wm_check != None) XDestroyWindow(dpy, wm_check);
    if (root_cursor != None) XFreeCursor(dpy, root_cursor);
    XCloseDisplay(dpy);
    free(clients);
    return 0;
}
