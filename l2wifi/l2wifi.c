/*
 * l2wifi - tiny wifi manager for L2.  Talks to wpa_supplicant over its UNIX
 * control socket; no libc beyond what musl provides, no dependencies.
 *
 *   l2wifi                    interactive: scan, pick a network, connect
 *   l2wifi status             link, SSID, signal, IP
 *   l2wifi scan               list networks (strongest first, one row per SSID)
 *   l2wifi connect SSID [PSK] save + connect (PSK is prompted, hidden, if omitted
 *                             and the network is secured; --open forces open)
 *   l2wifi list               saved networks
 *   l2wifi forget SSID
 *   l2wifi disconnect | reconnect
 *   l2wifi up | down          start / stop wpa_supplicant + DHCP (via l2net)
 *   l2wifi country XX         set regulatory domain (saved)
 *   options: -i IFACE   --no-color
 *
 * Build:  cc -std=c11 -Os -static -o l2wifi l2wifi.c        (see Makefile)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define CTRL_DIR "/run/wpa_supplicant"
#define MAXNET 128

static char iface[64];
static int sock = -1;
static char local_path[108];
static int color;
static volatile sig_atomic_t secret_tty_saved;
static struct termios secret_tty_orig;

static void cleanup_socket(void)
{
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
    if (local_path[0]) {
        unlink(local_path);
        local_path[0] = 0;
    }
}

static void on_signal(int sig)
{
    if (secret_tty_saved) {
        tcsetattr(0, TCSANOW, &secret_tty_orig);
        secret_tty_saved = 0;
    }
    if (local_path[0])
        unlink(local_path);
    _exit(128 + sig);
}

#define C(n) (color ? "\033[38;5;" #n "m" : "")
#define RST (color ? "\033[0m" : "")

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#include <stdarg.h>
static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%sl2wifi:%s ", C(203), RST);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (local_path[0])
        unlink(local_path);
    exit(1);
}

/* ------------------------------------------------------------ interface */
/* Interface names reach system() via l2net and fixed-size path buffers;
 * accept only the kernel-typical charset (IFNAMSIZ is 16 incl. NUL). */
static int valid_iface(const char *s)
{
    size_t n = strlen(s), i;

    if (!n || n > 15)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
            return 0;
    }
    return 1;
}

static int pick_iface(void)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    char p[256];
    struct stat st;

    if (!d)
        return -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (!valid_iface(e->d_name))
            continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
        if (stat(p, &st) == 0) {
            snprintf(iface, sizeof iface, "%s", e->d_name);
            break;
        }
        snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", e->d_name);
        if (stat(p, &st) == 0) {
            snprintf(iface, sizeof iface, "%s", e->d_name);
            break;
        }
    }
    closedir(d);
    return iface[0] ? 0 : -1;
}

/* ------------------------------------------------------------- ctrl i/o */
static int ctrl_open(void)
{
    struct sockaddr_un a = {.sun_family = AF_UNIX}, b = {.sun_family = AF_UNIX};
    struct timeval tv = {.tv_sec = 4};
    char dir[64];
    const char *rd = getenv("L2WIFI_CTRL_DIR");

    snprintf(dir, sizeof dir, "%s", rd ? rd : CTRL_DIR);
    snprintf(a.sun_path, sizeof a.sun_path, "%s/%s", dir, iface);
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;
    snprintf(local_path, sizeof local_path, "%s/l2wifi.%d", rd ? dir : "/tmp", (int)getpid());
    snprintf(b.sun_path, sizeof b.sun_path, "%s", local_path);
    unlink(local_path);
    if (bind(sock, (struct sockaddr *)&b, sizeof b) < 0) {
        close(sock);
        sock = -1;
        unlink(local_path);
        local_path[0] = 0;
        return -1;
    }
    chmod(local_path, 0600);
    if (connect(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        close(sock);
        sock = -1;
        unlink(local_path);
        local_path[0] = 0;
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return 0;
}

/* send one command, return reply (static buffer, NUL terminated) */
static char *ctrl(const char *cmd)
{
    static char buf[16384];
    ssize_t n;

    if (send(sock, cmd, strlen(cmd), 0) < 0)
        die("send failed: %s", strerror(errno));
    for (;;) {
        n = recv(sock, buf, sizeof buf - 1, 0);
        if (n < 0)
            die("no reply from wpa_supplicant (%s)", strerror(errno));
        buf[n] = 0;
        if (buf[0] == '<')      /* unsolicited event: skip */
            continue;
        break;
    }
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    return buf;
}

static char *ctrlf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *ctrlf(const char *fmt, ...)
{
    char cmd[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    return ctrl(cmd);
}

static int run_l2net(const char *arg)
{
    const char *bin = getenv("L2WIFI_NET") ? getenv("L2WIFI_NET") : "/bin/l2net";
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        const char *argv[4];
        argv[0] = bin;
        argv[1] = arg;
        argv[2] = iface[0] ? iface : NULL;
        argv[3] = NULL;
        execv(bin, (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* connect to the supervisor, starting it through l2net when needed */
static void ensure_supplicant(void)
{
    int i;

    if (!iface[0])
        pick_iface();
    if (iface[0] && ctrl_open() == 0)
        return;
    fprintf(stderr, "%s*%s starting wifi (drivers, wpa_supplicant)...\n", C(117), RST);
    run_l2net("wifi-start");
    for (i = 0; i < 40; i++) {              /* wait up to ~10 s */
        if (!iface[0])
            pick_iface();
        if (iface[0] && ctrl_open() == 0)
            return;
        usleep(250000);
    }
    die("wpa_supplicant is not running and no wifi interface appeared.\n"
        "       check: dmesg | tail ; ls /lib/firmware ; l2net status");
}

/* ------------------------------------------------------------ utilities */
static void hexenc(const char *s, char *out, size_t n)
{
    size_t o = 0;

    for (; *s && o + 2 < n; s++, o += 2)
        sprintf(out + o, "%02x", (unsigned char)*s);
    out[o] = 0;
}

/* wpa_supplicant escapes non-printables in ssid as \xNN */
static void ssid_unescape(const char *in, char *out, size_t n)
{
    size_t o = 0;

    while (*in && o + 1 < n) {
        if (in[0] == '\\' && in[1] == 'x' && isxdigit((unsigned char)in[2]) &&
            isxdigit((unsigned char)in[3])) {
            char h[3] = {in[2], in[3], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            in += 4;
        } else
            out[o++] = *in++;
    }
    out[o] = 0;
}

struct ap {
    char ssid[96];
    char flags[128];
    int sig, freq;
    char sec[12];
};

static const char *sec_of(const char *f)
{
    if (strstr(f, "SAE") && !strstr(f, "PSK"))
        return "WPA3";
    if (strstr(f, "SAE"))
        return "WPA2/3";
    if (strstr(f, "WPA2-EAP") || strstr(f, "WPA-EAP") || strstr(f, "RSN-EAP"))
        return "802.1X";
    if (strstr(f, "WPA2") || strstr(f, "RSN"))
        return "WPA2";
    if (strstr(f, "WPA"))
        return "WPA";
    if (strstr(f, "WEP"))
        return "WEP";
    if (strstr(f, "OWE"))
        return "OWE";
    return "open";
}

static int cmp_ap(const void *a, const void *b)
{
    return ((const struct ap *)b)->sig - ((const struct ap *)a)->sig;
}

/* parse SCAN_RESULTS: bssid / frequency / signal level / flags / ssid */
static int get_scan(struct ap *aps, int max)
{
    char *r = ctrl("SCAN_RESULTS"), *line, *save = NULL;
    int n = 0, first = 1;

    for (line = strtok_r(r, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[5], *p = line, *sv2 = NULL;
        int i, dup = 0;
        struct ap a;

        if (first) {
            first = 0;
            continue;
        }
        for (i = 0; i < 5; i++) {
            f[i] = strtok_r(i ? NULL : p, "\t", &sv2);
            if (!f[i]) {
                f[i] = (char *)"";
            }
        }
        if (!f[4][0])
            continue;                    /* hidden network */
        memset(&a, 0, sizeof a);
        ssid_unescape(f[4], a.ssid, sizeof a.ssid);
        a.freq = atoi(f[1]);
        a.sig = atoi(f[2]);
        snprintf(a.flags, sizeof a.flags, "%s", f[3]);
        snprintf(a.sec, sizeof a.sec, "%s", sec_of(f[3]));
        for (i = 0; i < n; i++)
            if (!strcmp(aps[i].ssid, a.ssid)) {
                if (a.sig > aps[i].sig)
                    aps[i] = a;
                dup = 1;
                break;
            }
        if (!dup && n < max)
            aps[n++] = a;
    }
    qsort(aps, n, sizeof *aps, cmp_ap);
    return n;
}

static int do_scan(struct ap *aps, int max)
{
    int i, n;
    char *r;

    r = ctrl("SCAN");
    if (strncmp(r, "OK", 2) && strncmp(r, "FAIL-BUSY", 9))
        die("scan failed: %s", r);
    for (i = 0; i < 12; i++) {              /* ~3 s, results land as they arrive */
        usleep(250000);
    }
    n = get_scan(aps, max);
    if (n == 0) {                           /* slow radio: one more wait */
        sleep(2);
        n = get_scan(aps, max);
    }
    return n;
}

static void bars(int sig, char *o)
{
    int q = sig >= -55 ? 4 : sig >= -65 ? 3 : sig >= -75 ? 2 : sig >= -85 ? 1 : 0, i;

    for (i = 0; i < 4; i++)
        o[i] = i < q ? '#' : '.';
    o[4] = 0;
}

static void print_aps(struct ap *aps, int n, int numbered)
{
    int i;
    char b[8];

    printf("  %s%s%-34s %-8s %-6s %s%s\n", C(212), numbered ? "   " : "", "SSID", "SECURITY",
           "BAND", "SIGNAL", RST);
    for (i = 0; i < n; i++) {
        bars(aps[i].sig, b);
        if (numbered)
            printf("  %s%2d%s ", C(212), i + 1, RST);
        else
            printf("  ");
        printf("%-34.34s %-8s %-6s %s %d dBm\n", aps[i].ssid, aps[i].sec,
               aps[i].freq > 5900 ? "6 GHz" : aps[i].freq > 4000 ? "5 GHz" : "2.4G", b, aps[i].sig);
    }
}

static const char *ip_of(const char *ifn, char *buf, size_t n)
{
    struct ifaddrs *ifa, *p;

    buf[0] = 0;
    if (getifaddrs(&ifa) < 0)
        return buf;
    for (p = ifa; p; p = p->ifa_next)
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, ifn)) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, buf, n);
            break;
        }
    freeifaddrs(ifa);
    return buf;
}

/* value of "key=" in a STATUS reply */
static const char *kv(const char *reply, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = reply;

    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *e = strchr(p + kl + 1, '\n');
            size_t l = e ? (size_t)(e - (p + kl + 1)) : strlen(p + kl + 1);
            if (l >= n)
                l = n - 1;
            memcpy(out, p + kl + 1, l);
            out[l] = 0;
            return out;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return out;
}

/* ----------------------------------------------------------- commands */
static int cmd_status(void)
{
    char st[2048], v[256], ip[64], sig[64] = "";
    char *r;

    ensure_supplicant();
    snprintf(st, sizeof st, "%s", ctrl("STATUS"));
    printf("  %-10s %s\n", "interface", iface);
    printf("  %-10s %s\n", "state", kv(st, "wpa_state", v, sizeof v));
    if (kv(st, "ssid", v, sizeof v)[0]) {
        char s[96];
        ssid_unescape(v, s, sizeof s);
        printf("  %-10s %s\n", "network", s);
    }
    kv(st, "freq", v, sizeof v);
    if (v[0])
        printf("  %-10s %s MHz\n", "frequency", v);
    kv(st, "key_mgmt", v, sizeof v);
    if (v[0])
        printf("  %-10s %s\n", "security", v);
    r = ctrl("SIGNAL_POLL");
    kv(r, "RSSI", sig, sizeof sig);
    if (sig[0])
        printf("  %-10s %s dBm\n", "signal", sig);
    ip_of(iface, ip, sizeof ip);
    printf("  %-10s %s\n", "address", ip[0] ? ip : "(none yet)");
    return 0;
}

static int cmd_scan(int numbered, struct ap *aps, int max)
{
    int n;

    ensure_supplicant();
    n = do_scan(aps, max);
    if (n == 0) {
        printf("  no networks found (radio blocked? try: l2net status)\n");
        return 0;
    }
    print_aps(aps, n, numbered);
    return n;
}

static int wait_connected(int secs)
{
    int i;
    char v[64], ip[64];

    for (i = 0; i < secs * 4; i++) {
        char *r = ctrl("STATUS");
        kv(r, "wpa_state", v, sizeof v);
        if (!strcmp(v, "COMPLETED"))
            break;
        usleep(250000);
    }
    if (strcmp(v, "COMPLETED"))
        return -1;
    {   /* ask udhcpc for a fresh lease only if the pid really belongs to udhcpc */
        char pf[128], comm_path[64], comm[32] = "";
        FILE *f;
        long pid = 0;

        snprintf(pf, sizeof pf, "/run/udhcpc.%s.pid", iface);
        f = fopen(pf, "r");
        if (f) {
            if (fscanf(f, "%ld", &pid) != 1)
                pid = 0;
            fclose(f);
        }
        if (pid > 1) {
            snprintf(comm_path, sizeof comm_path, "/proc/%ld/comm", pid);
            f = fopen(comm_path, "r");
            if (f) {
                if (fgets(comm, sizeof comm, f)) {
                    comm[strcspn(comm, "\r\n")] = 0;
                    if (strcmp(comm, "udhcpc") != 0)
                        pid = 0;
                } else {
                    pid = 0;
                }
                fclose(f);
            } else {
                pid = 0;
            }
        } else {
            pid = 0;
        }
        if (pid > 1)
            kill((pid_t)pid, SIGUSR1);
        else
            run_l2net("dhcp");
    }
    for (i = 0; i < 60; i++) {
        if (ip_of(iface, ip, sizeof ip)[0])
            return 0;
        usleep(250000);
    }
    return 1;       /* associated, no address yet */
}

static void read_secret(const char *prompt, char *out, size_t n)
{
    struct termios t, o;
    int tty = isatty(0) && tcgetattr(0, &o) == 0;

    fprintf(stderr, "%s", prompt);
    if (tty) {
        secret_tty_orig = o;
        secret_tty_saved = 1;
        t = o;
        t.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &t);
    }
    if (!fgets(out, n, stdin))
        out[0] = 0;
    if (tty) {
        tcsetattr(0, TCSANOW, &o);
        secret_tty_saved = 0;
        fputc('\n', stderr);
    }
    out[strcspn(out, "\r\n")] = 0;
}

static int cmd_connect(const char *ssid, const char *psk, int force_open, const char *known_sec)
{
    char hex[256], *r, id[16];
    char pass[128] = "";
    int rc, secured;

    if (!ssid[0] || strlen(ssid) > 32)
        die("SSID must be 1-32 bytes");
    ensure_supplicant();
    secured = !force_open && (!known_sec || strcmp(known_sec, "open"));
    if (secured && !psk) {
        read_secret("  passphrase: ", pass, sizeof pass);
        psk = pass;
    }
    if (secured) {
        size_t plen = strlen(psk);
        if (plen < 8 || plen > 63)
            die("a WPA passphrase must be 8-63 characters");
        for (const char *p = psk; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
                die("passphrase contains invalid control characters");
    }

    /* replace any saved entry with the same SSID */
    {
        char *l = ctrl("LIST_NETWORKS"), *s, *sv = NULL, tmp[4096];

        snprintf(tmp, sizeof tmp, "%s", l);
        for (s = strtok_r(tmp, "\n", &sv); s; s = strtok_r(NULL, "\n", &sv)) {
            char *tab1 = strchr(s, '\t'), *tab2;
            if (!tab1 || !isdigit((unsigned char)s[0]))
                continue;
            tab2 = strchr(tab1 + 1, '\t');
            if (tab2)
                *tab2 = 0;
            {
                char un[96];
                ssid_unescape(tab1 + 1, un, sizeof un);
                if (!strcmp(un, ssid)) {
                    *tab1 = 0;
                    ctrlf("REMOVE_NETWORK %s", s);
                }
            }
        }
    }
    r = ctrl("ADD_NETWORK");
    snprintf(id, sizeof id, "%s", r);
    if (!isdigit((unsigned char)id[0]))
        die("ADD_NETWORK failed: %s", r);
    hexenc(ssid, hex, sizeof hex);
    r = ctrlf("SET_NETWORK %s ssid %s", id, hex);
    if (strncmp(r, "OK", 2))
        die("SET_NETWORK ssid failed: %s", r);
    if (secured) {
        /* Escape quotes/backslashes for the wpa_supplicant quoted-string
         * parser; an unescaped '"' would inject extra config fields. */
        char esc[256];
        size_t o = 0;
        const char *p;
        for (p = psk; *p && o + 2 < sizeof esc; p++) {
            if (*p == '"' || *p == '\\')
                esc[o++] = '\\';
            esc[o++] = *p;
        }
        esc[o] = 0;
        r = ctrlf("SET_NETWORK %s psk \"%s\"", id, esc);
        {
            volatile char *ve = esc;
            for (size_t k = 0; k < sizeof esc; k++) ve[k] = 0;
        }
        if (strncmp(r, "OK", 2))
            die("rejected passphrase: %s", r);
        if (known_sec && (!strcmp(known_sec, "WPA3") || !strcmp(known_sec, "WPA2/3"))) {
            r = ctrlf("SET_NETWORK %s key_mgmt %s", id, !strcmp(known_sec, "WPA3") ? "SAE" : "WPA-PSK SAE");
            if (strncmp(r, "OK", 2))
                die("SET_NETWORK key_mgmt failed: %s", r);
        }
        if (known_sec && !strcmp(known_sec, "WPA3")) {
            r = ctrlf("SET_NETWORK %s ieee80211w 2", id);
            if (strncmp(r, "OK", 2))
                die("SET_NETWORK ieee80211w failed: %s", r);
        }
    } else {
        r = ctrlf("SET_NETWORK %s key_mgmt NONE", id);
        if (strncmp(r, "OK", 2))
            die("SET_NETWORK key_mgmt failed: %s", r);
    }
    {
        volatile char *vp = pass;
        for (size_t k = 0; k < sizeof pass; k++) vp[k] = 0;
    }
    r = ctrlf("ENABLE_NETWORK %s", id);
    if (strncmp(r, "OK", 2))
        die("ENABLE_NETWORK failed: %s", r);
    r = ctrlf("SELECT_NETWORK %s", id);
    if (strncmp(r, "OK", 2))
        die("SELECT_NETWORK failed: %s", r);
    printf("  %s*%s connecting to %s ...\n", C(117), RST, ssid);
    rc = wait_connected(20);
    if (rc < 0) {
        ctrlf("REMOVE_NETWORK %s", id);
        die("could not associate (wrong passphrase, weak signal or unsupported security)");
    }
    ctrl("SAVE_CONFIG");
    {
        char ip[64];
        ip_of(iface, ip, sizeof ip);
        if (rc == 0)
            printf("  %s+%s connected, address %s\n", C(114), RST, ip);
        else
            printf("  %s!%s associated, still waiting for DHCP (check: l2net status)\n", C(214), RST);
    }
    return 0;
}

static int cmd_list(void)
{
    char *l, *s, *sv = NULL;

    ensure_supplicant();
    l = ctrl("LIST_NETWORKS");
    for (s = strtok_r(l, "\n", &sv); s; s = strtok_r(NULL, "\n", &sv)) {
        char *t1, *t2, *t3;
        if (!isdigit((unsigned char)s[0]))
            continue;
        t1 = strchr(s, '\t');
        if (!t1)
            continue;
        t2 = strchr(t1 + 1, '\t');
        if (!t2)
            continue;
        t3 = strchr(t2 + 1, '\t');
        *t2 = 0;
        {
            char un[96];
            ssid_unescape(t1 + 1, un, sizeof un);
            printf("  %-34.34s %s\n", un, t3 && strstr(t3, "CURRENT") ? "(connected)" : "");
        }
    }
    return 0;
}

static int cmd_forget(const char *ssid)
{
    char *l, *s, *sv = NULL, tmp[4096];
    int hit = 0;

    ensure_supplicant();
    l = ctrl("LIST_NETWORKS");
    snprintf(tmp, sizeof tmp, "%s", l);
    for (s = strtok_r(tmp, "\n", &sv); s; s = strtok_r(NULL, "\n", &sv)) {
        char *t1, *t2;
        char un[96];
        if (!isdigit((unsigned char)s[0]))
            continue;
        t1 = strchr(s, '\t');
        if (!t1)
            continue;
        t2 = strchr(t1 + 1, '\t');
        if (t2)
            *t2 = 0;
        ssid_unescape(t1 + 1, un, sizeof un);
        if (!strcmp(un, ssid)) {
            *t1 = 0;
            ctrlf("REMOVE_NETWORK %s", s);
            hit++;
        }
    }
    if (!hit)
        die("no saved network named '%s'", ssid);
    ctrl("SAVE_CONFIG");
    printf("  forgot %s\n", ssid);
    return 0;
}

/* no-argument mode: scan, choose, connect */
static int cmd_interactive(void)
{
    struct ap aps[MAXNET];
    int n, pick = 0;
    char line[64];

    n = cmd_scan(1, aps, MAXNET);
    if (n <= 0)
        return 1;
    if (!isatty(0))
        return 0;
    printf("\n  network number (empty to cancel): ");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) || !(pick = atoi(line)) || pick < 1 || pick > n)
        return 0;
    return cmd_connect(aps[pick - 1].ssid, NULL, 0, aps[pick - 1].sec);
}

static void usage(void)
{
    puts("l2wifi " VERSION " - tiny wifi manager (wpa_supplicant client)\n\n"
         "usage: l2wifi [-i IFACE] [command]\n"
         "  (none)                interactive: scan, pick, connect\n"
         "  status                link, SSID, signal, address\n"
         "  scan                  list networks, strongest first\n"
         "  connect SSID [PSK]    connect and save (PSK prompted if omitted)\n"
         "  connect --open SSID   open network\n"
         "  list                  saved networks\n"
         "  forget SSID           delete a saved network\n"
         "  disconnect | reconnect\n"
         "  up | down             start / stop wpa_supplicant + DHCP\n"
         "  country XX            set regulatory domain\n");
}

int main(int argc, char **argv)
{
    int i = 1, open_only = 0;
    struct ap aps[MAXNET];
    const char *cmd;
    struct sigaction sa = {0};

    atexit(cleanup_socket);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    color = isatty(1) && !getenv("NO_COLOR");
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            if (!valid_iface(argv[i + 1]))
                die("bad interface name: %s", argv[i + 1]);
            snprintf(iface, sizeof iface, "%s", argv[i + 1]);
            i += 2;
        } else if (!strcmp(argv[i], "--no-color")) {
            color = 0;
            i++;
        } else if (!strcmp(argv[i], "--open")) {
            open_only = 1;
            i++;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            puts("l2wifi " VERSION);
            return 0;
        } else
            break;
    }
    cmd = i < argc ? argv[i++] : "";

    if (!*cmd)
        return cmd_interactive();
    if (!strcmp(cmd, "status"))
        return cmd_status();
    if (!strcmp(cmd, "scan"))
        return cmd_scan(0, aps, MAXNET) < 0;
    if (!strcmp(cmd, "list"))
        return cmd_list();
    if (!strcmp(cmd, "connect")) {
        if (i < argc && !strcmp(argv[i], "--open")) {
            open_only = 1;
            i++;
        }
        if (i >= argc)
            die("usage: l2wifi connect SSID [PSK]");
        return cmd_connect(argv[i], i + 1 < argc ? argv[i + 1] : NULL, open_only, NULL);
    }
    if (!strcmp(cmd, "forget")) {
        if (i >= argc)
            die("usage: l2wifi forget SSID");
        return cmd_forget(argv[i]);
    }
    if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "reconnect")) {
        ensure_supplicant();
        puts(ctrl(!strcmp(cmd, "disconnect") ? "DISCONNECT" : "RECONNECT"));
        return 0;
    }
    if (!strcmp(cmd, "up"))
        return run_l2net("wifi-start");
    if (!strcmp(cmd, "down"))
        return run_l2net("wifi-stop");
    if (!strcmp(cmd, "country")) {
        if (i >= argc || strlen(argv[i]) != 2 ||
            !isalpha((unsigned char)argv[i][0]) || !isalpha((unsigned char)argv[i][1]))
            die("usage: l2wifi country XX   (two letters, e.g. TH)");
        char cc[3] = {
            (char)toupper((unsigned char)argv[i][0]),
            (char)toupper((unsigned char)argv[i][1]),
            0
        };
        ensure_supplicant();
        char *res = ctrlf("SET country %s", cc);
        if (strncmp(res, "OK", 2))
            die("country rejected: %s", res);
        puts(res);
        ctrl("SAVE_CONFIG");
        return 0;
    }
    usage();
    return 2;
}
