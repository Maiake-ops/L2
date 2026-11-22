#define _POSIX_C_SOURCE 200809L
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef XF86XK_AudioLowerVolume
#define XF86XK_AudioLowerVolume 0x1008ff11
#endif
#ifndef XF86XK_AudioMute
#define XF86XK_AudioMute        0x1008ff12
#endif
#ifndef XF86XK_AudioRaiseVolume
#define XF86XK_AudioRaiseVolume 0x1008ff13
#endif

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
    unsigned bar_height;
    int show_bar;
    unsigned long focus_color;
    unsigned long unfocus_color;
    unsigned long bar_bg;
    unsigned long bar_fg;
    unsigned long bar_accent;
    unsigned long bar_muted;
    unsigned long bar_dim;
    unsigned long bar_ok;
    unsigned long bar_warn;
    char terminal[MAX_TERM_LEN];
} Config;

static Config cfg = {
    .master_percent = 50,
    .border_width = 2,
    .gap = 0,
    .bar_height = 24,
    .show_bar = 1,
    .focus_color = 0x7ebae4,
    .unfocus_color = 0x1e2633,
    .bar_bg = 0x0d1117,
    .bar_fg = 0xe6edf3,
    .bar_accent = 0x7ebae4,
    .bar_muted = 0x6e7681,
    .bar_dim = 0x161b22,
    .bar_ok = 0x7ee787,
    .bar_warn = 0xff7b72,
    .terminal = "xterm",
};

static Display *dpy;
static int screen;
static Window root;
static Window wm_check;
static Cursor root_cursor;

/* Status bar state */
static Window bar_win = None;
static Pixmap bar_pixmap = None;
static GC bar_gc = NULL;
static XFontStruct *bar_font = NULL;
static unsigned bar_w = 0;
static unsigned bar_h = 0;
static int bar_dirty = 1;

typedef struct {
    int x0, x1;
} BarBox;

static BarBox brand_box;
static BarBox ws_box[NUM_WORKSPACES];
static BarBox layout_box;
static BarBox vol_box;

/* Built-in App Launcher overlay state */
#define LAUNCHER_W      420
#define LAUNCHER_H      276
#define LAUNCHER_LIST_Y 44
#define LAUNCHER_ROW_H  28

typedef struct {
    const char *title;
    const char *desc;
    const char *cmd;
} AppEntry;

static const AppEntry app_entries[] = {
    { "NetSurf Browser",   "Graphical web browser",    "exec netsurf" },
    { "Terminal",          "Midnight XTerm shell",     "exec xterm -fn fixed -sl 256" },
    { "Wi-Fi Manager",     "Scan and connect Wi-Fi",   "exec xterm -fn fixed -sl 256 -e l2wifi" },
    { "Audio Test",        "On-demand sound & chime",  "exec xterm -fn fixed -sl 256 -e sh -c 'l2audio status; l2audio test; sleep 1'" },
    { "Audio Mute Toggle", "Mute or unmute audio",     "l2audio toggle" },
    { "System Monitor",    "Live processes & RAM",     "exec xterm -fn fixed -sl 256 -e top" },
    { "System Summary",    "L2 hardware & OS summary", "exec xterm -fn fixed -sl 256 -e sh -c 'l2fetch; read -r _'" },
    { "Disk Installer",    "Install L2 to disk",       "exec xterm -fn fixed -sl 256 -e sh -c 'l2install -h; exec /bin/dash -i'" },
};
#define NUM_APPS ((int)(sizeof(app_entries) / sizeof(app_entries[0])))

static Window launcher_win = None;
static Pixmap launcher_pixmap = None;
static int launcher_open = 0;
static char launcher_query[64];
static int launcher_qlen = 0;
static int launcher_sel = 0;
static volatile sig_atomic_t launcher_toggle_pending = 0;

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
static volatile sig_atomic_t bar_refresh_pending = 0;

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
    } else if (sig == SIGUSR1) {
        bar_refresh_pending = 1;
    } else if (sig == SIGUSR2) {
        launcher_toggle_pending = 1;
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
        } else if (strcmp(key, "bar_height") == 0) {
            unsigned long v = strtoul(val, NULL, 10);
            if (v >= 16 && v <= 48) cfg.bar_height = (unsigned)v;
        } else if (strcmp(key, "show_bar") == 0) {
            cfg.show_bar = (strcmp(val, "0") != 0 && strcmp(val, "false") != 0);
        } else if (strcmp(key, "focus_color") == 0) {
            parse_color(val, &cfg.focus_color);
        } else if (strcmp(key, "unfocus_color") == 0) {
            parse_color(val, &cfg.unfocus_color);
        } else if (strcmp(key, "bar_bg") == 0) {
            parse_color(val, &cfg.bar_bg);
        } else if (strcmp(key, "bar_fg") == 0) {
            parse_color(val, &cfg.bar_fg);
        } else if (strcmp(key, "bar_accent") == 0) {
            parse_color(val, &cfg.bar_accent);
        } else if (strcmp(key, "bar_muted") == 0) {
            parse_color(val, &cfg.bar_muted);
        } else if (strcmp(key, "bar_dim") == 0) {
            parse_color(val, &cfg.bar_dim);
        } else if (strcmp(key, "bar_ok") == 0) {
            parse_color(val, &cfg.bar_ok);
        } else if (strcmp(key, "bar_warn") == 0) {
            parse_color(val, &cfg.bar_warn);
        } else if (strcmp(key, "terminal") == 0 && *val) {
            snprintf(cfg.terminal, sizeof(cfg.terminal), "%s", val);
        }
    }
    fclose(f);
}

static void update_bar_geometry(void);

static void reload_config(void) {
    cfg.master_percent = 50;
    cfg.border_width = 2;
    cfg.gap = 0;
    cfg.bar_height = 24;
    cfg.show_bar = 1;
    snprintf(cfg.terminal, sizeof(cfg.terminal), "%s", "xterm");
    if (dpy) {
        parse_color("#7ebae4", &cfg.focus_color);
        parse_color("#1e2633", &cfg.unfocus_color);
        parse_color("#0d1117", &cfg.bar_bg);
        parse_color("#e6edf3", &cfg.bar_fg);
        parse_color("#7ebae4", &cfg.bar_accent);
        parse_color("#6e7681", &cfg.bar_muted);
        parse_color("#161b22", &cfg.bar_dim);
        parse_color("#7ee787", &cfg.bar_ok);
        parse_color("#ff7b72", &cfg.bar_warn);
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
    if (bar_win != None) {
        update_bar_geometry();
        bar_dirty = 1;
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
    bar_dirty = 1;
    XFlush(dpy);
}

static void setup_root_background(void) {
    unsigned long c_base = BlackPixel(dpy, screen);
    unsigned long c_sub = c_base;
    unsigned long c_dot = WhitePixel(dpy, screen);
    parse_color("#0a0e14", &c_base);
    parse_color("#121a26", &c_sub);
    parse_color("#182232", &c_dot);

    Pixmap pm = XCreatePixmap(dpy, root, 16, 16, (unsigned)DefaultDepth(dpy, screen));
    if (!pm) return;
    GC gc = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc, c_base);
    XFillRectangle(dpy, pm, gc, 0, 0, 16, 16);
    XSetForeground(dpy, gc, c_sub);
    XDrawPoint(dpy, pm, gc, 8, 8);
    XSetForeground(dpy, gc, c_dot);
    XFillRectangle(dpy, pm, gc, 0, 0, 2, 2);
    XFreeGC(dpy, gc);

    XSetWindowBackgroundPixmap(dpy, root, pm);
    XFreePixmap(dpy, pm);
    XClearWindow(dpy, root);
}

static int bar_text_width(const char *s, int len) {
    return bar_font ? XTextWidth(bar_font, s, len) : (len * 6);
}

static void read_volume(int *pct, int *muted) {
    *pct = 75;
    *muted = 0;
    FILE *f = fopen("/run/l2-vol", "r");
    if (!f) return;
    int v = 75, m = 0;
    if (fscanf(f, "%d %d", &v, &m) >= 1) {
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        *pct = v;
        *muted = (m != 0);
    }
    fclose(f);
}

static unsigned read_mem_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    unsigned long total = 0, avail = 0, mfree = 0, buf = 0, cached = 0;
    int has_avail = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned long v = 0;
        if (sscanf(line, "MemTotal: %lu", &v) == 1) total = v;
        else if (sscanf(line, "MemAvailable: %lu", &v) == 1) { avail = v; has_avail = 1; }
        else if (sscanf(line, "MemFree: %lu", &v) == 1) mfree = v;
        else if (sscanf(line, "Buffers: %lu", &v) == 1) buf = v;
        else if (sscanf(line, "Cached: %lu", &v) == 1) cached = v;
    }
    fclose(f);
    unsigned long used = 0;
    if (has_avail) {
        used = (total > avail) ? (total - avail) : 0;
    } else {
        unsigned long rec = mfree + buf + cached;
        used = (total > rec) ? (total - rec) : 0;
    }
    return (unsigned)((used + 512) / 1024);
}

static int read_net_status(char *out, size_t sz) {
    snprintf(out, sz, "NET --");
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' || strcmp(ent->d_name, "lo") == 0) continue;
        char p[320];
        snprintf(p, sizeof(p), "/sys/class/net/%.240s/operstate", ent->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char st[32] = "";
        int is_up = (fgets(st, sizeof(st), f) && strncmp(st, "up", 2) == 0);
        fclose(f);
        if (is_up) {
            snprintf(p, sizeof(p), "/sys/class/net/%.240s/wireless", ent->d_name);
            struct stat sb;
            if (stat(p, &sb) == 0 || strncmp(ent->d_name, "wl", 2) == 0) {
                snprintf(out, sz, "WIFI");
                closedir(d);
                return 1;
            }
            snprintf(out, sz, "ETH");
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static int read_bat_pct(int *pct) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "BAT", 3) != 0) continue;
        char p[320];
        snprintf(p, sizeof(p), "/sys/class/power_supply/%.240s/capacity", ent->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int v = -1;
        int ok = (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100);
        fclose(f);
        if (ok) {
            *pct = v;
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static void get_client_title(Window w, char *out, size_t sz) {
    out[0] = '\0';
    if (w == None || sz < 4) return;
    Atom actual_type;
    int actual_fmt;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, atom_net_wm_name, 0, 64, False, AnyPropertyType,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &prop) != Success ||
        !prop || nitems == 0) {
        if (prop) {
            XFree(prop);
            prop = NULL;
        }
        XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 64, False, AnyPropertyType,
                           &actual_type, &actual_fmt, &nitems, &bytes_after, &prop);
    }
    if (prop) {
        size_t i = 0;
        while (i < nitems && i + 1 < sz && prop[i]) {
            unsigned char ch = prop[i];
            out[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
            ++i;
        }
        out[i] = '\0';
        XFree(prop);
    }
}

static void init_bar(void) {
    bar_font = XLoadQueryFont(dpy, "-misc-fixed-medium-r-normal--13-120-75-75-c-70-iso8859-1");
    if (!bar_font) bar_font = XLoadQueryFont(dpy, "fixed");
    if (!bar_font) bar_font = XLoadQueryFont(dpy, "*");

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = cfg.bar_bg;
    swa.event_mask = ExposureMask | ButtonPressMask;
    bar_win = XCreateWindow(dpy, root, 0, 0, 1, cfg.bar_height ? cfg.bar_height : 24, 0,
                            DefaultDepth(dpy, screen), CopyFromParent, DefaultVisual(dpy, screen),
                            CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
    bar_gc = XCreateGC(dpy, bar_win, 0, NULL);
    if (bar_font) XSetFont(dpy, bar_gc, bar_font->fid);
}

static void update_bar_geometry(void) {
    if (bar_win == None) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, root, &a)) return;
    if (!cfg.show_bar) {
        XUnmapWindow(dpy, bar_win);
        return;
    }
    unsigned bw = (a.width > 0) ? (unsigned)a.width : 1;
    unsigned bh = cfg.bar_height ? cfg.bar_height : 24;
    if (bw != bar_w || bh != bar_h || bar_pixmap == None) {
        if (bar_pixmap != None) XFreePixmap(dpy, bar_pixmap);
        bar_w = bw;
        bar_h = bh;
        bar_pixmap = XCreatePixmap(dpy, root, bar_w, bar_h, (unsigned)DefaultDepth(dpy, screen));
    }
    XMoveResizeWindow(dpy, bar_win, 0, 0, bar_w, bar_h);
    XMapRaised(dpy, bar_win);
}

static void draw_bar(void) {
    if (!cfg.show_bar || bar_win == None || bar_pixmap == None || !bar_gc) return;

    int fh = bar_font ? (bar_font->ascent + bar_font->descent) : 13;
    int asc = bar_font ? bar_font->ascent : 10;
    int ty = ((int)bar_h - fh) / 2 + asc;
    if (ty < asc) ty = asc;

    XSetForeground(dpy, bar_gc, cfg.bar_bg);
    XFillRectangle(dpy, bar_pixmap, bar_gc, 0, 0, bar_w, bar_h);

    /* Crisp bottom hairline */
    XSetForeground(dpy, bar_gc, cfg.unfocus_color);
    XDrawLine(dpy, bar_pixmap, bar_gc, 0, (int)bar_h - 1, (int)bar_w, (int)bar_h - 1);

    int lx = 6;
    /* L2 brand / launcher pill */
    {
        const char *brand = "L2";
        int bw = bar_text_width(brand, 2) + 12;
        brand_box.x0 = lx;
        brand_box.x1 = lx + bw;
        XSetForeground(dpy, bar_gc, cfg.bar_dim);
        XFillRectangle(dpy, bar_pixmap, bar_gc, lx, 3, (unsigned)bw, bar_h - 7);
        XSetForeground(dpy, bar_gc, cfg.bar_accent);
        XFillRectangle(dpy, bar_pixmap, bar_gc, lx, (int)bar_h - 4, (unsigned)bw, 2);
        XDrawString(dpy, bar_pixmap, bar_gc, lx + 6, ty, brand, 2);
        lx += bw + 6;
    }

    /* Workspace indicators */
    unsigned ws_occ = 0;
    for (unsigned i = 0; i < client_count; ++i) {
        if (clients[i].ws < NUM_WORKSPACES) ws_occ |= (1u << clients[i].ws);
    }
    for (unsigned i = 0; i < NUM_WORKSPACES; ++i) {
        char num[2] = { (char)('1' + i), '\0' };
        int pw = 20;
        ws_box[i].x0 = lx;
        ws_box[i].x1 = lx + pw;
        int active = (i == cur_ws);
        int occupied = ((ws_occ & (1u << i)) != 0);
        if (active) {
            XSetForeground(dpy, bar_gc, cfg.bar_dim);
            XFillRectangle(dpy, bar_pixmap, bar_gc, lx, 3, (unsigned)pw, bar_h - 7);
            XSetForeground(dpy, bar_gc, cfg.bar_accent);
            XFillRectangle(dpy, bar_pixmap, bar_gc, lx, (int)bar_h - 4, (unsigned)pw, 2);
        } else if (occupied) {
            XSetForeground(dpy, bar_gc, cfg.bar_dim);
            XFillRectangle(dpy, bar_pixmap, bar_gc, lx, 3, (unsigned)pw, bar_h - 7);
            XSetForeground(dpy, bar_gc, cfg.bar_muted);
            XFillRectangle(dpy, bar_pixmap, bar_gc, lx + 8, (int)bar_h - 4, 4, 2);
            XSetForeground(dpy, bar_gc, cfg.bar_fg);
        } else {
            XSetForeground(dpy, bar_gc, cfg.bar_muted);
        }
        int tw = bar_text_width(num, 1);
        XDrawString(dpy, bar_pixmap, bar_gc, lx + (pw - tw) / 2, ty, num, 1);
        lx += pw + 2;
    }

    /* Layout indicator pill */
    lx += 4;
    {
        const char *ltxt = (ws_layout[cur_ws] == LAYOUT_MONOCLE) ? "[M]" : "[]=";
        int lw = bar_text_width(ltxt, 3) + 12;
        layout_box.x0 = lx;
        layout_box.x1 = lx + lw;
        XSetForeground(dpy, bar_gc, cfg.bar_dim);
        XFillRectangle(dpy, bar_pixmap, bar_gc, lx, 3, (unsigned)lw, bar_h - 7);
        XSetForeground(dpy, bar_gc, cfg.bar_accent);
        XDrawString(dpy, bar_pixmap, bar_gc, lx + 6, ty, ltxt, 3);
        lx += lw + 8;
    }

    /* Right side status pills (clock, optional battery, RAM, net, volume) */
    struct {
        char text[32];
        unsigned long fg;
        int is_vol;
    } pills[5];
    int npills = 0;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(pills[npills].text, sizeof(pills[npills].text), "%02d:%02d",
             tm_now.tm_hour, tm_now.tm_min);
    pills[npills].fg = cfg.bar_fg;
    pills[npills].is_vol = 0;
    ++npills;

    int bat = 0;
    if (read_bat_pct(&bat)) {
        snprintf(pills[npills].text, sizeof(pills[npills].text), "BAT %d%%", bat);
        pills[npills].fg = (bat <= 20) ? cfg.bar_warn : cfg.bar_ok;
        pills[npills].is_vol = 0;
        ++npills;
    }

    snprintf(pills[npills].text, sizeof(pills[npills].text), "RAM %uM", read_mem_mb());
    pills[npills].fg = cfg.bar_accent;
    pills[npills].is_vol = 0;
    ++npills;

    char net_str[16];
    int net_up = read_net_status(net_str, sizeof(net_str));
    snprintf(pills[npills].text, sizeof(pills[npills].text), "%s", net_str);
    pills[npills].fg = net_up ? cfg.bar_ok : cfg.bar_muted;
    pills[npills].is_vol = 0;
    ++npills;

    int vol = 75, muted = 0;
    read_volume(&vol, &muted);
    if (muted) {
        snprintf(pills[npills].text, sizeof(pills[npills].text), "MUTE");
        pills[npills].fg = cfg.bar_warn;
    } else {
        snprintf(pills[npills].text, sizeof(pills[npills].text), "VOL %d%%", vol);
        pills[npills].fg = cfg.bar_fg;
    }
    pills[npills].is_vol = 1;
    ++npills;

    vol_box.x0 = -1;
    vol_box.x1 = -1;
    int rx = (int)bar_w - 6;
    for (int i = 0; i < npills; ++i) {
        int len = (int)strlen(pills[i].text);
        int pw = bar_text_width(pills[i].text, len) + 12;
        int x0 = rx - pw;
        if (x0 <= lx + 8) break;
        XSetForeground(dpy, bar_gc, cfg.bar_dim);
        XFillRectangle(dpy, bar_pixmap, bar_gc, x0, 3, (unsigned)pw, bar_h - 7);
        if (pills[i].is_vol) {
            vol_box.x0 = x0;
            vol_box.x1 = x0 + pw;
            XSetForeground(dpy, bar_gc, muted ? cfg.bar_warn : cfg.bar_accent);
            XFillRectangle(dpy, bar_pixmap, bar_gc, x0, (int)bar_h - 4, (unsigned)pw, 2);
        }
        XSetForeground(dpy, bar_gc, pills[i].fg);
        XDrawString(dpy, bar_pixmap, bar_gc, x0 + 6, ty, pills[i].text, len);
        rx = x0 - 4;
    }

    /* Focused client title in center space */
    if (rx - lx > 40) {
        char title[96];
        get_client_title(ws_focus[cur_ws], title, sizeof(title));
        unsigned long tfg = cfg.bar_fg;
        if (!title[0]) {
            snprintf(title, sizeof(title), "L2 Midnight");
            tfg = cfg.bar_muted;
        }
        int avail = rx - lx - 12;
        int tlen = (int)strlen(title);
        if (bar_text_width(title, tlen) > avail) {
            while (tlen > 3 && bar_text_width(title, tlen) > avail - 12) --tlen;
            if (tlen >= 3) {
                title[tlen - 3] = '.';
                title[tlen - 2] = '.';
                title[tlen - 1] = '.';
            }
            title[tlen] = '\0';
        }
        XSetForeground(dpy, bar_gc, tfg);
        XDrawString(dpy, bar_pixmap, bar_gc, lx + 4, ty, title, tlen);
    }

    XCopyArea(dpy, bar_pixmap, bar_win, bar_gc, 0, 0, bar_w, bar_h, 0, 0);
    XFlush(dpy);
}

static void spawn_sh(const char *cmd);

static int str_casestr(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            ++h;
            ++n;
        }
        if (!*n) return 1;
    }
    return 0;
}

static int launcher_filter(int *idxs, int max_idxs) {
    int count = 0;
    for (int i = 0; i < NUM_APPS && count < max_idxs; ++i) {
        if (!launcher_qlen ||
            str_casestr(app_entries[i].title, launcher_query) ||
            str_casestr(app_entries[i].desc, launcher_query) ||
            str_casestr(app_entries[i].cmd, launcher_query)) {
            idxs[count++] = i;
        }
    }
    return count;
}

static void draw_launcher(void) {
    if (!launcher_open || launcher_win == None || launcher_pixmap == None || !bar_gc) return;

    int fh = bar_font ? (bar_font->ascent + bar_font->descent) : 13;
    int asc = bar_font ? bar_font->ascent : 10;
    int idxs[NUM_APPS];
    int n = launcher_filter(idxs, NUM_APPS);
    if (n > 0) {
        if (launcher_sel < 0) launcher_sel = 0;
        if (launcher_sel >= n) launcher_sel = n - 1;
    } else {
        launcher_sel = 0;
    }

    XSetForeground(dpy, bar_gc, cfg.bar_bg);
    XFillRectangle(dpy, launcher_pixmap, bar_gc, 0, 0, LAUNCHER_W, LAUNCHER_H);

    /* Prompt box */
    XSetForeground(dpy, bar_gc, cfg.bar_dim);
    XFillRectangle(dpy, launcher_pixmap, bar_gc, 6, 6, LAUNCHER_W - 12, 30);
    XSetForeground(dpy, bar_gc, cfg.bar_accent);
    XFillRectangle(dpy, launcher_pixmap, bar_gc, 6, 34, LAUNCHER_W - 12, 2);

    int py = 6 + (30 - fh) / 2 + asc;
    const char *prompt = "LAUNCH > ";
    int pw = bar_text_width(prompt, 9);
    XDrawString(dpy, launcher_pixmap, bar_gc, 14, py, prompt, 9);

    int qx = 14 + pw;
    XSetForeground(dpy, bar_gc, cfg.bar_fg);
    if (launcher_qlen > 0) {
        XDrawString(dpy, launcher_pixmap, bar_gc, qx, py, launcher_query, launcher_qlen);
        qx += bar_text_width(launcher_query, launcher_qlen);
    }
    XSetForeground(dpy, bar_gc, cfg.bar_accent);
    XFillRectangle(dpy, launcher_pixmap, bar_gc, qx + 2, 12, 6, 16);

    const char *hint = "ESC close";
    int hw = bar_text_width(hint, 9);
    XSetForeground(dpy, bar_gc, cfg.bar_muted);
    XDrawString(dpy, launcher_pixmap, bar_gc, LAUNCHER_W - 14 - hw, py, hint, 9);

    if (n == 0) {
        int ry = LAUNCHER_LIST_Y + (LAUNCHER_ROW_H - fh) / 2 + asc;
        char custom[96];
        snprintf(custom, sizeof(custom), "Run command: %s", launcher_query);
        XSetForeground(dpy, bar_gc, cfg.bar_dim);
        XFillRectangle(dpy, launcher_pixmap, bar_gc, 6, LAUNCHER_LIST_Y, LAUNCHER_W - 12, LAUNCHER_ROW_H - 2);
        XSetForeground(dpy, bar_gc, cfg.bar_accent);
        XFillRectangle(dpy, launcher_pixmap, bar_gc, 6, LAUNCHER_LIST_Y, 3, LAUNCHER_ROW_H - 2);
        XSetForeground(dpy, bar_gc, cfg.bar_fg);
        XDrawString(dpy, launcher_pixmap, bar_gc, 16, ry, custom, (int)strlen(custom));
    } else {
        for (int r = 0; r < n; ++r) {
            const AppEntry *ent = &app_entries[idxs[r]];
            int y0 = LAUNCHER_LIST_Y + r * LAUNCHER_ROW_H;
            int ry = y0 + (LAUNCHER_ROW_H - fh) / 2 + asc;
            int sel = (r == launcher_sel);
            if (sel) {
                XSetForeground(dpy, bar_gc, cfg.bar_dim);
                XFillRectangle(dpy, launcher_pixmap, bar_gc, 6, y0, LAUNCHER_W - 12, LAUNCHER_ROW_H - 2);
                XSetForeground(dpy, bar_gc, cfg.bar_accent);
                XFillRectangle(dpy, launcher_pixmap, bar_gc, 6, y0, 3, LAUNCHER_ROW_H - 2);
            }
            XSetForeground(dpy, bar_gc, sel ? cfg.bar_fg : cfg.bar_muted);
            XDrawString(dpy, launcher_pixmap, bar_gc, 16, ry, ent->title, (int)strlen(ent->title));

            int dlen = (int)strlen(ent->desc);
            int dw = bar_text_width(ent->desc, dlen);
            XSetForeground(dpy, bar_gc, sel ? cfg.bar_accent : cfg.bar_muted);
            XDrawString(dpy, launcher_pixmap, bar_gc, LAUNCHER_W - 14 - dw, ry, ent->desc, dlen);
        }
    }

    XCopyArea(dpy, launcher_pixmap, launcher_win, bar_gc, 0, 0, LAUNCHER_W, LAUNCHER_H, 0, 0);
    XFlush(dpy);
}

static void close_launcher(void) {
    if (!launcher_open) return;
    launcher_open = 0;
    XUngrabKeyboard(dpy, CurrentTime);
    if (launcher_win != None) XUnmapWindow(dpy, launcher_win);
    apply_focus();
    XFlush(dpy);
}

static void open_launcher(void) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, root, &a)) return;
    int x = ((int)a.width - LAUNCHER_W) / 2;
    int y = ((int)a.height - LAUNCHER_H) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (launcher_win == None) {
        XSetWindowAttributes swa;
        swa.override_redirect = True;
        swa.background_pixel = cfg.bar_bg;
        swa.border_pixel = cfg.focus_color;
        swa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | KeyPressMask;
        launcher_win = XCreateWindow(dpy, root, x, y, LAUNCHER_W, LAUNCHER_H, 2,
                                     DefaultDepth(dpy, screen), CopyFromParent, DefaultVisual(dpy, screen),
                                     CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &swa);
        launcher_pixmap = XCreatePixmap(dpy, root, LAUNCHER_W, LAUNCHER_H, (unsigned)DefaultDepth(dpy, screen));
    } else {
        XMoveWindow(dpy, launcher_win, x, y);
        XSetWindowBorder(dpy, launcher_win, cfg.focus_color);
    }

    launcher_query[0] = '\0';
    launcher_qlen = 0;
    launcher_sel = 0;
    launcher_open = 1;
    XMapRaised(dpy, launcher_win);
    XSetInputFocus(dpy, launcher_win, RevertToPointerRoot, CurrentTime);
    XGrabKeyboard(dpy, launcher_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    draw_launcher();
}

static void toggle_launcher(void) {
    if (launcher_open) close_launcher();
    else open_launcher();
}

static void launcher_exec_row(int row) {
    int idxs[NUM_APPS];
    int n = launcher_filter(idxs, NUM_APPS);
    if (n > 0 && row >= 0 && row < n) {
        const char *cmd = app_entries[idxs[row]].cmd;
        close_launcher();
        spawn_sh(cmd);
    } else if (n == 0 && launcher_qlen > 0) {
        char raw[64];
        snprintf(raw, sizeof(raw), "%s", launcher_query);
        close_launcher();
        spawn_sh(raw);
    } else {
        close_launcher();
    }
}

static void launcher_hover(int y) {
    if (!launcher_open || y < LAUNCHER_LIST_Y) return;
    int idxs[NUM_APPS];
    int n = launcher_filter(idxs, NUM_APPS);
    int row = (y - LAUNCHER_LIST_Y) / LAUNCHER_ROW_H;
    if (row >= 0 && row < n && row != launcher_sel) {
        launcher_sel = row;
        draw_launcher();
    }
}

static void tile(void) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, root, &a)) return;
    unsigned sw = (unsigned)a.width;
    unsigned sh = (unsigned)a.height;
    unsigned bw = cfg.border_width;
    unsigned gap = cfg.gap;
    unsigned top_off = (cfg.show_bar && cfg.bar_height < sh) ? cfg.bar_height : 0;
    unsigned avail_screen_h = sh - top_off;

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
                XMoveResizeWindow(dpy, clients[i].win, 0, (int)top_off,
                                  sw ? sw : 1, avail_screen_h ? avail_screen_h : 1);
            }
        } else {
            unsigned usable_w = (sw > 2 * gap) ? (sw - 2 * gap) : sw;
            unsigned usable_h = (avail_screen_h > 2 * gap) ? (avail_screen_h - 2 * gap) : avail_screen_h;
            unsigned ox = (sw > 2 * gap) ? gap : 0;
            unsigned oy = top_off + ((avail_screen_h > 2 * gap) ? gap : 0);

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

    if (cfg.show_bar && bar_win != None) XRaiseWindow(dpy, bar_win);
    if (launcher_open && launcher_win != None) XRaiseWindow(dpy, launcher_win);
    apply_focus();
}

static void add_client(Window w, int focus_new) {
    XWindowAttributes a;
    if (w == wm_check || w == bar_win || w == launcher_win || index_of(w) >= 0) return;
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
    XSelectInput(dpy, w, EnterWindowMask | PropertyChangeMask);

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

static void spawn_cmd(const char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        close(ConnectionNumber(dpy));
        setsid();
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
}

static void spawn_sh(const char *cmd) {
    const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
    spawn_cmd(argv);
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
        XK_Return, XK_j, XK_k, XK_h, XK_l, XK_m, XK_t, XK_f, XK_space,
        XK_b, XK_d, XK_p, XK_equal, XK_plus, XK_minus, XK_0, XK_q, XK_r, XK_Escape
    };
    for (unsigned i = 0; i < sizeof(base_keys) / sizeof(base_keys[0]); ++i) {
        grab_key_sym(base_keys[i], Mod4Mask);
    }

    KeySym media_keys[] = {
        XF86XK_AudioRaiseVolume, XF86XK_AudioLowerVolume, XF86XK_AudioMute
    };
    for (unsigned i = 0; i < sizeof(media_keys) / sizeof(media_keys[0]); ++i) {
        grab_key_sym(media_keys[i], 0);
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
    KeySym sym = XLookupKeysym(ev, 0);
    if (launcher_open) {
        char buf[16];
        int len = XLookupString(ev, buf, (int)sizeof(buf) - 1, &sym, NULL);
        if ((ev->state & Mod4Mask) && (sym == XK_d || sym == XK_p)) {
            close_launcher();
            return;
        }
        if (sym == XK_Escape) {
            close_launcher();
            return;
        }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            launcher_exec_row(launcher_sel);
            return;
        }
        if (sym == XK_Up || sym == XK_KP_Up || ((ev->state & ControlMask) && sym == XK_k)) {
            int idxs[NUM_APPS];
            int n = launcher_filter(idxs, NUM_APPS);
            if (n > 0) launcher_sel = (launcher_sel > 0) ? (launcher_sel - 1) : (n - 1);
            draw_launcher();
            return;
        }
        if (sym == XK_Down || sym == XK_KP_Down || sym == XK_Tab ||
            ((ev->state & ControlMask) && sym == XK_j)) {
            int idxs[NUM_APPS];
            int n = launcher_filter(idxs, NUM_APPS);
            if (n > 0) launcher_sel = (launcher_sel + 1) % n;
            draw_launcher();
            return;
        }
        if (sym == XK_BackSpace) {
            if (launcher_qlen > 0) {
                launcher_query[--launcher_qlen] = '\0';
                launcher_sel = 0;
                draw_launcher();
            }
            return;
        }
        if ((ev->state & ControlMask) && sym == XK_u) {
            launcher_qlen = 0;
            launcher_query[0] = '\0';
            launcher_sel = 0;
            draw_launcher();
            return;
        }
        if (!(ev->state & (ControlMask | Mod1Mask | Mod4Mask)) && len > 0) {
            for (int i = 0; i < len; ++i) {
                unsigned char ch = (unsigned char)buf[i];
                if (ch >= 0x20 && ch < 0x7f && launcher_qlen + 1 < (int)sizeof(launcher_query)) {
                    launcher_query[launcher_qlen++] = (char)ch;
                }
            }
            launcher_query[launcher_qlen] = '\0';
            launcher_sel = 0;
            draw_launcher();
        }
        return;
    }

    if (sym == XF86XK_AudioRaiseVolume) {
        const char *argv[] = {"l2audio", "up", "5", NULL};
        spawn_cmd(argv);
        return;
    }
    if (sym == XF86XK_AudioLowerVolume) {
        const char *argv[] = {"l2audio", "down", "5", NULL};
        spawn_cmd(argv);
        return;
    }
    if (sym == XF86XK_AudioMute) {
        const char *argv[] = {"l2audio", "toggle", NULL};
        spawn_cmd(argv);
        return;
    }

    if (!(ev->state & Mod4Mask)) return;
    int shifted = (ev->state & ShiftMask) != 0;

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
    case XK_b:
        cfg.show_bar = !cfg.show_bar;
        update_bar_geometry();
        tile();
        bar_dirty = 1;
        break;
    case XK_d:
    case XK_p:
        toggle_launcher();
        break;
    case XK_equal:
    case XK_plus: {
        const char *argv[] = {"l2audio", "up", "5", NULL};
        spawn_cmd(argv);
        break;
    }
    case XK_minus: {
        const char *argv[] = {"l2audio", "down", "5", NULL};
        spawn_cmd(argv);
        break;
    }
    case XK_0: {
        const char *argv[] = {"l2audio", "toggle", NULL};
        spawn_cmd(argv);
        break;
    }
    case XK_r: reload_config(); tile(); break;
    case XK_q: close_focused(); break;
    case XK_Escape: running = 0; break;
    default: break;
    }
}

static void handle_button_press(XButtonEvent *ev) {
    if (launcher_open && launcher_win != None && ev->window == launcher_win) {
        int idxs[NUM_APPS];
        int n = launcher_filter(idxs, NUM_APPS);
        if (ev->button == Button4) {
            if (n > 0) launcher_sel = (launcher_sel > 0) ? (launcher_sel - 1) : (n - 1);
            draw_launcher();
        } else if (ev->button == Button5) {
            if (n > 0) launcher_sel = (launcher_sel + 1) % n;
            draw_launcher();
        } else if (ev->button == Button1 && ev->y >= LAUNCHER_LIST_Y) {
            int row = (ev->y - LAUNCHER_LIST_Y) / LAUNCHER_ROW_H;
            if (row >= 0 && (row < n || (n == 0 && row == 0))) {
                launcher_exec_row(row);
            }
        }
        return;
    }

    if (cfg.show_bar && bar_win != None && ev->window == bar_win) {
        if (ev->button == Button1) {
            if (ev->x >= brand_box.x0 && ev->x < brand_box.x1) {
                toggle_launcher();
                return;
            }
            for (unsigned i = 0; i < NUM_WORKSPACES; ++i) {
                if (ev->x >= ws_box[i].x0 && ev->x < ws_box[i].x1) {
                    switch_workspace(i);
                    return;
                }
            }
            if (ev->x >= layout_box.x0 && ev->x < layout_box.x1) {
                ws_layout[cur_ws] = (ws_layout[cur_ws] == LAYOUT_TILE) ? LAYOUT_MONOCLE : LAYOUT_TILE;
                tile();
                return;
            }
        }
        if (vol_box.x0 >= 0 && ev->x >= vol_box.x0 && ev->x < vol_box.x1) {
            if (ev->button == Button1) {
                const char *argv[] = {"l2audio", "toggle", NULL};
                spawn_cmd(argv);
            } else if (ev->button == Button4) {
                const char *argv[] = {"l2audio", "up", "5", NULL};
                spawn_cmd(argv);
            } else if (ev->button == Button5) {
                const char *argv[] = {"l2audio", "down", "5", NULL};
                spawn_cmd(argv);
            }
        }
        return;
    }

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
    case Expose:
        if (e->xexpose.window == bar_win && e->xexpose.count == 0) bar_dirty = 1;
        else if (launcher_open && e->xexpose.window == launcher_win && e->xexpose.count == 0) draw_launcher();
        break;
    case PropertyNotify:
        if (e->xproperty.window == ws_focus[cur_ws] &&
            (e->xproperty.atom == XA_WM_NAME || e->xproperty.atom == atom_net_wm_name)) {
            bar_dirty = 1;
        }
        break;
    case ConfigureNotify:
        if (e->xconfigure.window == root) {
            update_bar_geometry();
            tile();
            bar_dirty = 1;
        }
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
        if (launcher_open && e->xmotion.window == launcher_win) {
            launcher_hover(e->xmotion.y);
        } else {
            handle_motion(&e->xmotion);
        }
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
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);

    reload_config();
    setup_root_background();
    init_ewmh();
    init_bar();
    update_bar_geometry();

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
    draw_bar();

    int xfd = ConnectionNumber(dpy);
    while (running) {
        if (reload_pending) {
            reload_pending = 0;
            reload_config();
            tile();
        }
        if (bar_refresh_pending) {
            bar_refresh_pending = 0;
            bar_dirty = 1;
        }
        if (launcher_toggle_pending) {
            launcher_toggle_pending = 0;
            toggle_launcher();
        }
        while (running && XPending(dpy) > 0) {
            XEvent e;
            XNextEvent(dpy, &e);
            handle_event(&e);
        }
        if (!running) break;
        if (bar_dirty && cfg.show_bar) {
            bar_dirty = 0;
            draw_bar();
        }
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int sret = select(xfd + 1, &fds, NULL, NULL, &tv);
        if (sret < 0 && errno != EINTR) break;
        if (sret == 0) bar_dirty = 1;
    }

    /* Remap hidden workspace windows on exit so clients aren't left unmapped */
    for (unsigned i = 0; i < client_count; ++i) {
        if (!clients[i].mapped) XMapWindow(dpy, clients[i].win);
    }
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    XDeleteProperty(dpy, root, atom_net_active);
    XDeleteProperty(dpy, root, atom_net_client_list);
    XDeleteProperty(dpy, root, atom_net_wm_check);
    if (launcher_pixmap != None) XFreePixmap(dpy, launcher_pixmap);
    if (launcher_win != None) XDestroyWindow(dpy, launcher_win);
    if (bar_pixmap != None) XFreePixmap(dpy, bar_pixmap);
    if (bar_gc) XFreeGC(dpy, bar_gc);
    if (bar_font) XFreeFont(dpy, bar_font);
    if (bar_win != None) XDestroyWindow(dpy, bar_win);
    if (wm_check != None) XDestroyWindow(dpy, wm_check);
    if (root_cursor != None) XFreeCursor(dpy, root_cursor);
    XCloseDisplay(dpy);
    free(clients);
    return 0;
}
