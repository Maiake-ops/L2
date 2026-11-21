/* rcl2 - Linux init, PID 1 core (C side).
 *
 * Design in one paragraph:
 *   - freestanding C, raw syscalls only, no libc, single static binary
 *   - fixed memory, no heap: one config struct, pointer tables, log buffer,
 *     nothing else movable
 *   - single-threaded; all work happens between ppoll() returns
 *   - the Rust core (src/rs/rcl.rs) parses config/cmdline/statm into a
 *     pinned #[repr(C)] ABI; everything here does syscalls & processes
 *   - services are supervised: wait-mode blocks at boot, respawn-mode
 *     reaps asynchronously with 1s..30s exponential backoff (reset to 1s
 *     after a 30s stable run)
 *   - PID 1 never exits: every failure path is logged and retried or degraded
 *   - signals reach the loop through a self-pipe; handlers only write()
 *
 * Modes of the one binary (drop-in for L2's script/init/init.c):
 *   rcl2                        boot as PID 1
 *   rcl2 console [DEV]          getty-style login shell (dash -i)
 *   rcl2 poweroff|reboot|halt   direct mode, callable by l2(8) as any user
 *   rcl2 --selftest [CONF]      parse & memory self check, no side effects
 */
#include "syscalls.h"
#include "rcl.h"

/* ---------------------------------------------------------------- structs */

struct kts { long tv_sec; long tv_nsec; };
struct pollfdx { int fd; short events; short revents; };
struct winsz { unsigned short row, col, xpixel, ypixel; };

/* raw kernel struct termios (asm-generic termbits.h, NCCS = 19) */
struct ktio {
    unsigned c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};
enum { VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VSUSP = 10 };

/* kernel ABI struct for rt_sigaction (x86_64) */
struct ksa { unsigned long handler; unsigned long flags; unsigned long restorer; unsigned long mask; };

/* kernel ABI struct for sysinfo(2) (x86_64); totalram @32, mem_unit @100 */
struct ksysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
};

#define BOOTFLAG_RESCUE 1u   /* l2.rescue=1 | single | s | emergency */

/* ------------------------------------------------------------- constants */

#define CONFIG_PATH     "/etc/rcl2.conf"
#define CMDLINE_PATH    "/proc/cmdline"
#define STATM_PATH      "/proc/self/statm"
#define CONSOLE_DEV     "/dev/console"
#define INSTALLED_MARK  "/etc/l2/installed"

#define DEFAULT_HOSTNAME "L2"
#define DEFAULT_SHELL    "/bin/dash"

#define CFG_RAW_CAP     2048         /* raw config read capacity  */
#define CMDLINE_CAP     2048         /* kernel cmdline buffer     */

#define SB_FLOOR_MS     1000         /* respawn backoff floor     */
#define SB_STABLE_MS    30000UL      /* backoff reset threshold   */
#define SB_CEIL_MS      30000        /* backoff ceiling           */
#define GRACE_TERM_MS   5000         /* shutdown TERM grace       */
#define GRACE_KILL_MS   3000         /* shutdown KILL grace       */

/* ------------------------------------------------------------------ data */

static struct rcl_conf g_conf;                         /* ~8.5KB, static    */
static char       *g_svc_argv[RCL_MAX_SVCS][RCL_MAX_ARGS + 2];
static char       *g_svc_env[RCL_MAX_ENV + 6];
static char        g_svc_envbuf[RCL_MAX_ENV][RCL_ENV_LEN];

/* supervision state, indexed by service slot */
static long           g_pid[RCL_MAX_SVCS];
static unsigned long  g_started[RCL_MAX_SVCS];         /* mono ms           */
static unsigned long  g_next[RCL_MAX_SVCS];            /* next respawn, 0=none */
static int            g_backoff[RCL_MAX_SVCS];         /* current delay ms  */

static int            g_sig_rd = -1, g_sig_wr = -1;
static unsigned       g_flags;                         /* atomic per-signal */
static volatile unsigned char g_closing;               /* shutdown underway */

/* log line buffer: only touched from the main loop, never from handlers */
static char     g_line[512];
static unsigned g_len;
/* Shared scratch for one-shot reads (mounts scan, config, cmdline, statm,
 * selftest). Invariant: rcl2 is single-threaded and every consumer finishes
 * before the next one starts (boot order: mounts_setup -> conf_load ->
 * cmdline_flags -> rss_report; --selftest is a separate mode, its rss_report
 * runs after all printout). Nothing retains a pointer into it: the Rust
 * parser copies into g_conf, every other consumer returns a scalar. Pinned
 * by test-supervision.c::test_iobuf_aliasing. */
static char     g_iobuf[CFG_RAW_CAP];

extern void rcl_sigreturn(void);
void rcl_enter(unsigned long *sp);

unsigned long strlen(const char *s);   /* src/mem.c */

static void supervise(void) __attribute__((noreturn));

/* ----------------------------------------------------------- tiny helpers */

static void putstr(const char *s)
{
    while (*s && g_len + 1 < sizeof(g_line)) g_line[g_len++] = *s++;
}
static void putnum(long n)
{
    char tmp[24];
    unsigned k = 0;
    unsigned long u;
    if (n < 0) {
        putstr("-");
        u = (unsigned long)(-(n + 1)) + 1UL;
    } else {
        u = (unsigned long)n;
    }
    do { tmp[k++] = (char)('0' + (u % 10)); u /= 10; } while (u && k < sizeof(tmp));
    while (k && g_len + 1 < sizeof(g_line)) g_line[g_len++] = tmp[--k];
}
static void line_out(void)
{
    unsigned long n = 0;
    g_line[g_len++] = '\n';
    while (n < g_len) {
        long r = r_sys3(SYS_write, 2L, (long)(g_line + n), (long)(g_len - n));
        if (r == -EINTR) continue;
        if (r <= 0) break;
        n += (unsigned)r;
    }
    g_len = 0;
}
static void slog(const char *a) { g_len = 0; putstr("rcl2: "); putstr(a); line_out(); }
static void slog1(const char *a, const char *b)
{
    g_len = 0; putstr("rcl2: "); putstr(a); putstr(b); line_out();
}
static void slogn(const char *a, long n, const char *b)
{
    g_len = 0; putstr("rcl2: "); putstr(a); putnum(n); putstr(b); line_out();
}

static void slogerr(const char *what, long err)
{
    g_len = 0; putstr("rcl2: "); putstr(what); putnum(err); putstr(" (errno)");
    line_out();
}

static void putexit(int st)
{
    if (w_exited(st))      { putstr("exit ");    putnum((long)w_exitcode(st)); }
    else if (w_signaled(st)){ putstr("signal "); putnum((long)w_sig(st)); }
    else putstr("stop");
}

static unsigned long now_ms(void)
{
    struct kts t;
    if (r_sys2(SYS_clock_gettime, 1L /* CLOCK_MONOTONIC */, (long)&t) != 0) return 0;
    return (unsigned long)t.tv_sec * 1000UL + (unsigned long)(t.tv_nsec / 1000000L);
}

/* always finishes, even when interrupted: shutdown paths & small waits */
static void sleep_ms(long ms)
{
    if (ms <= 0) return;
    struct kts req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    struct kts rem;
    while (r_sys2(SYS_nanosleep, (long)&req, (long)&rem) == -EINTR) req = rem;
}

/* Read up to cap-1 bytes into buf, NUL-terminate, return the count. When the
 * file holds more than cap-1 bytes, return -EFBIG instead of truncating
 * silently (buf still holds the first cap-1 bytes, NUL-terminated) and store
 * the first unread byte in *more (when non-NULL): a whitespace probe means
 * the cut landed on a token/line boundary, which callers may treat as
 * complete (W7 refinement of the W1 probe, auditor semantic note 3).
 * *more = -1 when EOF was reached (exact fit) or on error. Read errors
 * propagate as negative errno; callers treat any negative result as failure. */
static int file_read(const char *path, char *buf, int cap, int *more)
{
    if (more) *more = -1;
    if (cap < 2) return -EINVAL;
    int fd = (int)r_sys3(SYS_open, (long)path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return fd;
    int got = 0;
    while (got < cap - 1) {
        long r = r_sys3(SYS_read, fd, (long)(buf + got), (long)(cap - 1 - got));
        if (r == -EINTR) continue;
        if (r < 0) { r_sys1(SYS_close, fd); return (int)r; }
        if (r == 0) break;
        got += (int)r;
    }
    if (got == cap - 1) {
        /* one byte past the buffer proves the file did not fit */
        char probe;
        long r;
        do { r = r_sys3(SYS_read, fd, (long)&probe, 1); } while (r == -EINTR);
        if (r != 0) {
            r_sys1(SYS_close, fd);
            buf[got] = 0;
            if (r > 0) {
                if (more) *more = (unsigned char)probe;
                return -EFBIG;
            }
            return (int)r;
        }
    }
    r_sys1(SYS_close, fd);
    buf[got] = 0;
    return got;
}

static int file_exists(const char *path)
{
    int fd = (int)r_sys3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;
    r_sys1(SYS_close, fd);
    return 1;
}

/* True when /proc/mounts shows "/" mounted read-only: the kernel silently
 * falls back from rw to ro on mount errors (seen on real-hardware photo
 * boots), and an installed L2 needs /etc, /var and /home writable. Returns
 * 0 when /proc/mounts is unreadable so the caller's guard is a no-op then.
 * The file is streamed in g_iobuf-sized chunks and scanned field by field:
 * on a busy installed system /proc/mounts can exceed any fixed buffer, and
 * dropping the "/" line would silently disable the ro->rw recovery remount.
 * Fields are space-separated (the kernel escapes spaces inside mountpoints),
 * lines end at '\n'; only mountpoint and the start of options are tracked. */
static int root_mounted_readonly(void)
{
    int fd = (int)r_sys3(SYS_open, (long)"/proc/mounts", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return 0;
    int ro = 0;
    int field = 0;                 /* 0 device, 1 mountpoint, 2 fstype, 3 options */
    int mp_len = 0;                /* mountpoint field length */
    char mp_first = 0;             /* its first byte */
    int opt_len = 0;               /* options field length */
    char opt_head[3] = {0, 0, 0};  /* its first bytes */
    for (;;) {
        long r = r_sys3(SYS_read, fd, (long)g_iobuf, (long)sizeof(g_iobuf));
        if (r == -EINTR) continue;
        if (r < 0) { r_sys1(SYS_close, fd); return 0; }
        if (r == 0) break;
        for (long k = 0; k < r; k++) {
            char ch = g_iobuf[k];
            if (ch == '\n') {
                /* a "/" line: options must start with "ro" (last line wins) */
                if (mp_len == 1 && mp_first == '/')
                    ro = (opt_len >= 2 && opt_head[0] == 'r' && opt_head[1] == 'o' &&
                          (opt_len == 2 || opt_head[2] == ','));
                field = 0; mp_len = 0; mp_first = 0; opt_len = 0;
            } else if (ch == ' ') {
                field++;
            } else if (field == 1) {
                if (mp_len == 0) mp_first = ch;
                mp_len++;
            } else if (field == 3) {
                if (opt_len < 3) opt_head[opt_len] = ch;
                opt_len++;
            }
        }
    }
    r_sys1(SYS_close, fd);
    /* a final line without '\n' still counts */
    if (mp_len == 1 && mp_first == '/')
        ro = (opt_len >= 2 && opt_head[0] == 'r' && opt_head[1] == 'o' &&
              (opt_len == 2 || opt_head[2] == ','));
    return ro;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* quit a non(pid 1) process; pid 1 never takes this route */
__attribute__((noreturn))
static void die_ex(int code)
{
    r_sys1(SYS_exit_group, code);
    for (;;) sleep_ms(3600000L);  /* exit cannot fail; belt and braces */
}

/* ------------------------------------------------------------- boot scene */

static void mount_err(const char *dir, long r)
{
    g_len = 0;
    putstr("rcl2: mount "); putstr(dir); putstr(" failed (errno "); putnum(-r); putstr(")");
    line_out();
}

/* mount one fs; EBUSY counts as success; logs the real errno on failure */
static int do_mount(const char *src, const char *dir, const char *fs,
                    unsigned flags, const char *data)
{
    long r = r_sys5(SYS_mount, (long)src, (long)dir, (long)fs, (long)flags, (long)data);
    if (r == 0 || r == -EBUSY) return 0;
    if (dir) mount_err(dir, r);
    return -1;
}

static void console_open(void)
{
    int fd = (int)r_sys3(SYS_open, (long)CONSOLE_DEV, O_RDWR, 0);
    if (fd < 0) {
        slog("cannot open /dev/console, using inherited fds");
        return;
    }
    for (int i = 0; i < 3; i++)
        if (i != fd) r_sys2(SYS_dup2, fd, i);
    if (fd > 2) r_sys1(SYS_close, fd);
}

static void mkdir_try(const char *p, unsigned mode) { r_sys2(SYS_mkdir, (long)p, mode); }
static void symlink_try(const char *a, const char *b) { r_sys2(SYS_symlink, (long)a, (long)b); }

/* Live-mode scratch cap: totalram >> shift clamped to [floor, ceil].
 * /tmp: 12.5% of RAM, 2..16 MiB - ENOSPC arrives well before global OOM
 * (a 16 MiB guest idles ~5.6 MiB used and OOMs around ~8 MiB committed).
 * /run: 6.25%, 1..8 MiB - pidfiles/sockets only. The 16 MiB /tmp ceiling
 * still fits l2install's live scratch: it extracts the ~0.8 MiB initramfs
 * rootfs into /tmp (well under 2 MiB of pages).
 * (maiake-f6 auditor design, 04-rcl2.md §4.3, lead-verified.) */
static unsigned long live_scratch_cap(unsigned long total, unsigned shift,
                                      unsigned long floor, unsigned long ceil)
{
    unsigned long cap = total >> shift;
    if (cap < floor) cap = floor;
    if (cap > ceil) cap = ceil;
    return cap;
}

/* "mode=NNNN,size=<decimal>" mount option builder (no libc snprintf) */
static void scratch_opt(char *dst, unsigned cap, const char *head, unsigned long v)
{
    unsigned k = 0;
    while (head[k] && k + 1 < cap) { dst[k] = head[k]; k++; }
    char tmp[24];
    unsigned n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < sizeof(tmp));
    while (n && k + 1 < cap) dst[k++] = tmp[--n];
    dst[k] = 0;
}

static void mounts_setup(void)
{
    do_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    console_open();

    do_mount("proc", "/proc", "proc", MS_NOSUID, 0);
    symlink_try("/proc/self/fd",   "/dev/fd");
    symlink_try("/proc/self/fd/0", "/dev/stdin");
    symlink_try("/proc/self/fd/1", "/dev/stdout");
    symlink_try("/proc/self/fd/2", "/dev/stderr");

    do_mount("sysfs", "/sys", "sysfs", MS_NOSUID, 0);

    mkdir_try("/dev/pts", 0755);
    mkdir_try("/dev/shm", 0755);
    do_mount("devpts", "/dev/pts", "devpts", MS_NOSUID, "mode=0620,ptmxmode=0666");
    do_mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID, "mode=1777");

    /* /run and /tmp. Installed systems keep the historical mounts (tmpfs
     * default size = half of RAM). Live sessions run on an uncapped ramfs
     * rootfs: without a tmpfs cap any /tmp writer can exhaust all RAM, the
     * OOM killer takes the console shell in a respawn loop, and the
     * pressure file can never be removed (campaign 9). A capped tmpfs
     * turns that into ENOSPC while the system stays alive. */
    if (file_exists(INSTALLED_MARK)) {
        /* Recover from a kernel rw→ro fallback remounting "/" read-only:
         * a remount with MS_REMOUNT alone clears SB_RDONLY (the shutdown
         * path does the reverse with MS_REMOUNT|MS_RDONLY). */
        if (root_mounted_readonly() && do_mount(0, "/", 0, MS_REMOUNT, 0) == 0)
            slog("root was read-only; remounted r/w");
        do_mount("tmpfs", "/run", "tmpfs", MS_NOSUID, "mode=0755");
        do_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID, "mode=1777");
    } else {
        struct ksysinfo si;
        unsigned long total = 0;
        if (r_sys1(SYS_sysinfo, (long)&si) == 0)
            total = si.totalram * (si.mem_unit ? si.mem_unit : 1);
        /* sysinfo failure degrades to the floor caps: still bounded */
        unsigned long tcap = live_scratch_cap(total, 3, 2UL << 20, 16UL << 20);
        unsigned long rcap = live_scratch_cap(total, 4, 1UL << 20, 8UL << 20);
        char opt[48];
        mkdir_try("/run", 0755);
        mkdir_try("/tmp", 01777);
        scratch_opt(opt, sizeof(opt), "mode=0755,size=", rcap);
        do_mount("tmpfs", "/run", "tmpfs", MS_NOSUID, opt);
        scratch_opt(opt, sizeof(opt), "mode=1777,size=", tcap);
        do_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID, opt);
        g_len = 0;
        putstr("rcl2: live scratch caps: /tmp "); putnum((long)(tcap >> 10));
        putstr(" KiB, /run "); putnum((long)(rcap >> 10)); putstr(" KiB");
        line_out();
    }
}

/* ------------------------------------------------------------------ env */

static void env_setup(void)
{
    static const char *defs[] = {
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        "HOME=/root",
        "USER=root",
        "TERM=linux",
        "PS1=L2# ",
    };
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof(defs) / sizeof(defs[0]); i++)
        g_svc_env[n++] = (char *)defs[i];
    for (int i = 0; i < RCL_MAX_ENV && i < g_conf.nenv; i++) {
        g_svc_env[n++] = g_svc_envbuf[i];
        for (unsigned k = 0; k + 1 < RCL_ENV_LEN && g_conf.env[i][k]; k++)
            g_svc_envbuf[i][k] = g_conf.env[i][k];
        g_svc_envbuf[i][RCL_ENV_LEN - 1] = 0;
    }
    g_svc_env[n] = 0;
}

static void hostname_apply(void)
{
    const char *h = g_conf.hostname[0] ? g_conf.hostname : DEFAULT_HOSTNAME;
    r_sys2(SYS_sethostname, (long)h, (long)strlen(h));
}

/* --------------------------------------------------------------- signals */

static void sig_any(int s)
{
    unsigned bit = (s > 0 && s < 32) ? (1u << s) : (1u << 30);
    __atomic_or_fetch(&g_flags, bit, __ATOMIC_RELAXED);
    char one = 1;
    long r = r_sys3(SYS_write, (long)g_sig_wr, (long)&one, 1);
    (void)r;   /* EAGAIN (pipe full) is fine: the bit is set anyway */
}

static void signals_setup(void)
{
    int fds[2] = { -1, -1 };
    /* Drain must stop at EAGAIN, and handlers must never block on a full pipe. */
    if (r_sys2(SYS_pipe2, (long)fds, (long)(O_CLOEXEC | O_NONBLOCK)) != 0)
        fds[0] = fds[1] = -1;
    g_sig_rd = fds[0];
    g_sig_wr = fds[1];

    static const int sigs[] = {
        SIGHUP, SIGINT, SIGQUIT, SIGTERM,
        SIGUSR1, SIGUSR2, SIGPWR, SIGCHLD, SIGWINCH,
    };
    struct ksa sa;
    sa.handler  = (unsigned long)sig_any;
    /* no SA_RESTART: every blocking wait must return EINTR so shutdown requests
     * are acted on even while a wait-mode service (or rescue shell) blocks */
    sa.flags    = SA_RESTORER | SA_NOCLDSTOP;
    sa.restorer = (unsigned long)rcl_sigreturn;
    sa.mask     = 0;
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        r_sys4(SYS_rt_sigaction, sigs[i], (long)&sa, 0, 8UL);
}

#define SIGBIT(s)     (1u << (s))
/* signals -> flag bits set from the async handler; reads MUST be atomic,
 * otherwise the long-lived ppoll loop may serve a stale register copy and
 * never see the pending shutdown request */
#define SIGTAKEN(s)   ((__atomic_load_n(&g_flags, __ATOMIC_RELAXED) & SIGBIT(s)) != 0)
#define SIGCLEAR(s)   do { __atomic_and_fetch(&g_flags, ~SIGBIT(s), __ATOMIC_RELAXED); } while (0)

__attribute__((noreturn))
static void shutdown_run(const char *why, unsigned cmd);

/* signals -> actions; returns only when nothing fatal is pending, never
 * returns when a shutdown action fires. Safe to call from any wait loop. */
static void take_actions(void)
{
    if (SIGTAKEN(SIGWINCH)) SIGCLEAR(SIGWINCH);
    if (SIGTAKEN(SIGCHLD))  SIGCLEAR(SIGCHLD);
    if (SIGTAKEN(SIGINT)) {
        SIGCLEAR(SIGINT);
        shutdown_run("ctrl-alt-del",
                     g_conf.cad == RCL_CAD_POWEROFF ? RB_POWER_OFF : RB_RESTART);
    }
    if (SIGTAKEN(SIGUSR2)) {
        SIGCLEAR(SIGUSR2);
        shutdown_run("reboot request", RB_RESTART);
    }
    if (SIGTAKEN(SIGTERM) || SIGTAKEN(SIGUSR1) || SIGTAKEN(SIGPWR)) {
        SIGCLEAR(SIGTERM); SIGCLEAR(SIGUSR1); SIGCLEAR(SIGPWR);
        shutdown_run("poweroff request", RB_POWER_OFF);
    }
    if (SIGTAKEN(SIGQUIT)) {
        SIGCLEAR(SIGQUIT);
        shutdown_run("halt request", RB_HALT);
    }
    if (SIGTAKEN(SIGHUP)) {
        SIGCLEAR(SIGHUP);
        shutdown_run("halt request (HUP)", RB_HALT);
    }
}

static char g_drain[64];
static void pipe_drain(void)
{
    for (;;) {
        long r = r_sys3(SYS_read, (long)g_sig_rd, (long)g_drain, (long)sizeof(g_drain));
        if (r == -EINTR) continue;
        if (r <= 0) break;    /* EAGAIN/EOF: nothing left */
    }
}

/* ------------------------------------------------------------ config ABI */

static void conf_default(void)
{
    struct rcl_svc *s = &g_conf.svcs[0];
    const char *name = "shell", *path = DEFAULT_SHELL, *arg0 = "-i";
    for (unsigned k = 0; k + 1 < RCL_NAME_LEN && name[k]; k++) s->name[k] = name[k];
    for (unsigned k = 0; k + 1 < RCL_PATH_LEN && path[k]; k++) s->path[k] = path[k];
    for (unsigned k = 0; k + 1 < RCL_ARG_LEN && arg0[k]; k++)  s->args[0][k] = arg0[k];
    s->nargs = 1;
    s->mode = RCL_MODE_RESPAWN;
    g_conf.nsvc = 1;
    slog("no config - default shell service active");
}

static void conf_load(void)
{
    int more = -1;
    int n = file_read(CONFIG_PATH, g_iobuf, CFG_RAW_CAP, &more);
    if (n == -EFBIG) {
        if (more == '\n') {
            /* cut landed exactly on a line boundary: every buffered line
             * is complete, parse them all (config at exactly cap + NL) */
            n = CFG_RAW_CAP - 1;
        } else {
            /* a truncated service line could exec a shortened path prefix
             * (a space probe could also hide further args on the line):
             * refuse the whole config and degrade to the default shell */
            slog("config over capacity - refusing truncated parse");
            conf_default();
            return;
        }
    }
    if (n < 0) {
        slog1("no config at ", CONFIG_PATH);
        conf_default();
        return;
    }
    long got = rcl_rs_parse(&g_conf, g_iobuf, (unsigned long)n);
    if (got < 0) {
        slog1("config parse failed: ", CONFIG_PATH);
        conf_default();
        return;
    }
    slogn("config ", got, " service(s) parsed");
    if (g_conf.nsvc == 0) conf_default();
}

/* token whitespace, kept identical to the Rust core's is_ws() */
static int cmdline_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == 0x0B || c == 0x0C;
}

/* Boot flags from /proc/cmdline. An oversized cmdline whose first unread
 * byte is whitespace ended on a token boundary: parse it fully (the x86
 * max-length cmdline is 2047 chars + '\n' = 2048 B). Otherwise parse up to
 * the last complete token: a flag cut mid-token must neither fire
 * spuriously nor pass silently (the dropped tail is logged).
 * Unreadable -> normal boot. */
static unsigned cmdline_flags(void)
{
    int more = -1;
    int n = file_read(CMDLINE_PATH, g_iobuf, (int)sizeof(g_iobuf), &more);
    if (n == -EFBIG) {
        n = (int)sizeof(g_iobuf) - 1;
        if (!cmdline_ws(more)) {
            while (n > 0 && !cmdline_ws((unsigned char)g_iobuf[n - 1]))
                n--;
            slog("cmdline over capacity - incomplete trailing token dropped");
        }
    }
    if (n < 0) {
        slog("cmdline read failed, assuming normal boot");
        return 0;
    }
    return (unsigned)rcl_rs_bootflags(g_iobuf, (unsigned long)n);
}

/* ------------------------------------------------- service state machine */

static void argv_build(void)
{
    for (unsigned i = 0; i < g_conf.nsvc; i++) {
        struct rcl_svc *s = &g_conf.svcs[i];
        unsigned n = 0;
        g_svc_argv[i][n++] = s->path;
        for (unsigned a = 0; a < s->nargs && n < RCL_MAX_ARGS + 1; a++)
            g_svc_argv[i][n++] = s->args[a];
        g_svc_argv[i][n] = 0;
    }
}

static void svc_state_clear(void)
{
    for (unsigned i = 0; i < RCL_MAX_SVCS; i++) {
        g_pid[i] = 0;
        g_started[i] = 0;
        g_next[i] = 0;
        g_backoff[i] = 0;
    }
}

static void svc_spawn(unsigned i)
{
    struct rcl_svc *s = &g_conf.svcs[i];
    long pid = r_sys0(SYS_fork);
    if (pid == 0) {
        static const char msg[] = "rcl2: execve failed - child exits\n";
        r_sys0(SYS_setsid);
        long x = r_sys3(SYS_execve, (long)s->path, (long)g_svc_argv[i], (long)g_svc_env);
        (void)x;
        r_sys3(SYS_write, 2L, (long)msg, (long)sizeof(msg) - 1);
        r_sys1(SYS_exit, 127L);   /* unreachable on success */
    }
    if (pid < 0) {
        slogerr("fork failed", -pid);
        if (s->mode == RCL_MODE_RESPAWN) {
            int ms = g_backoff[i] ? g_backoff[i] * 2 : 3000;
            if (ms > SB_CEIL_MS) ms = SB_CEIL_MS;
            g_backoff[i] = ms;
            g_next[i] = now_ms() + (unsigned long)ms;
        }
        return;
    }
    g_pid[i] = pid;
    g_started[i] = now_ms();
    g_next[i] = 0;
    /* A successful fork does not mean the service is healthy. Preserve the
     * delay across short-lived runs; svc_schedule_backoff resets it after
     * SB_STABLE_MS of uptime. */

    g_len = 0;
    putstr("rcl2: start "); putstr(s->name); putstr(" pid "); putnum(pid);
    line_out();
}

/* recompute after an unexpected exit */
static void svc_schedule_backoff(unsigned i, int st)
{
    struct rcl_svc *s = &g_conf.svcs[i];
    unsigned long run = now_ms() - g_started[i];
    int ms = (run >= SB_STABLE_MS) ? SB_FLOOR_MS
                                   : (g_backoff[i] ? g_backoff[i] * 2 : SB_FLOOR_MS);
    if (ms > SB_CEIL_MS) ms = SB_CEIL_MS;
    g_backoff[i] = ms;
    g_next[i] = now_ms() + (unsigned long)ms;

    g_len = 0;
    putstr("rcl2: "); putstr(s->name); putstr(" exited ("); putexit(st);
    putstr("), retry in "); putnum((long)(ms / 1000)); putstr("s");
    line_out();
}

/* reap every dead child; map service pids to supervision state.
 * Returns 1 when no children remain (-ECHILD), 0 otherwise. */
static int reap_children(void)
{
    for (;;) {
        int st = 0;
        long pid = r_sys4(SYS_wait4, -1L, (long)&st, WNOHANG, 0);
        if (pid == -EINTR) continue;
        if (pid == -ECHILD) return 1;
        if (pid <= 0) return 0;                         /* 0/EAGAIN: live children remain */
        unsigned hit = RCL_MAX_SVCS;
        for (unsigned i = 0; i < g_conf.nsvc; i++)
            if (g_pid[i] == pid) { hit = i; break; }

        if (hit < g_conf.nsvc) {
            g_pid[hit] = 0;
            struct rcl_svc *s = &g_conf.svcs[hit];
            if (s->mode == RCL_MODE_RESPAWN && !g_closing)
                svc_schedule_backoff(hit, st);
            else {
                g_len = 0;
                putstr("rcl2: "); putstr(s->name); putstr(" done ("); putexit(st); putstr(")");
                line_out();
            }
        } else {
            g_len = 0;
            putstr("rcl2: reap "); putnum(pid); putstr(" "); putexit(st);
            line_out();
        }
    }
}

/* blocking wait for a wait-mode service during boot */
static void svc_wait_boot(unsigned i)
{
    long pid = g_pid[i];
    if (pid <= 0) return;
    for (;;) {
        int st = 0;
        long w = r_sys4(SYS_wait4, pid, (long)&st, 0, 0);
        if (w == -EINTR) {
            /* shutdown must not be starved by a blocked wait-mode service;
             * the reap sweep may collect our child too - check before looping */
            take_actions();
            reap_children();
            if (g_pid[i] == 0) return;   /* swept away, already logged */
            continue;
        }
        if (w == -ECHILD) {                /* child was collected elsewhere */
            g_pid[i] = 0;
            slog1(g_conf.svcs[i].name, ": collected earlier");
            break;
        }
        if (w > 0) {
            g_pid[i] = 0;
            struct rcl_svc *s = &g_conf.svcs[i];
            g_len = 0;
            putstr("rcl2: "); putstr(s->name); putstr(" finished ("); putexit(st); putstr(")");
            line_out();
        }
        break;
    }
    /* collect any zombies that piled up while we blocked */
    reap_children();
}

/* ------------------------------------------------------------ rescue mode */

static void env_write(char *dst, unsigned cap, const char *head, const char *tail)
{
    unsigned k = 0;
    while (head[k] && k + 1 < cap) { dst[k] = head[k]; k++; }
    if (tail)
        for (unsigned j = 0; tail[j] && k + 1 < cap; j++) dst[k++] = tail[j];
    dst[k] = 0;
}

static void rescue_shell(void)
{
    char e_path[48], e_home[16], e_term[16], e_ps1[24];
    char *envp[5];
    char *argv[3] = { (char *)DEFAULT_SHELL, (char *)"-i", 0 };
    env_write(e_path, sizeof(e_path), "PATH=", "/bin:/sbin:/usr/bin:/usr/sbin");
    env_write(e_home, sizeof(e_home), "HOME=", "/root");
    env_write(e_term, sizeof(e_term), "TERM=", "linux");
    env_write(e_ps1,  sizeof(e_ps1),  "PS1=", "L2-rescue# ");
    envp[0] = e_path; envp[1] = e_home; envp[2] = e_term; envp[3] = e_ps1; envp[4] = 0;
    argv[0] = (char *)DEFAULT_SHELL;

    slog("L2 recovery shell. Commands: l2fetch, l2net, l2install, poweroff, reboot.");

    for (;;) {
        take_actions();          /* pendings honored even while respawning shells */
        long pid = r_sys0(SYS_fork);
        if (pid == 0) {
            r_sys0(SYS_setsid);
            r_sys3(SYS_ioctl, 0L, TIOCSCTTY, 1L);
            long x = r_sys3(SYS_execve, (long)DEFAULT_SHELL, (long)argv, (long)envp);
            (void)x;
            static const char msg[] = "rcl2: no recovery shell\n";
            r_sys3(SYS_write, 2L, (long)msg, (long)sizeof(msg) - 1);
            r_sys1(SYS_exit, 127L);
        }
        if (pid < 0) { sleep_ms(1000); continue; }
        for (;;) {
            int st = 0;
            long w = r_sys4(SYS_wait4, pid, (long)&st, 0, 0);
            if (w == -EINTR) {
                /* a poweroff issued in the recovery shell must still land */
                take_actions();
                continue;
            }
            (void)st;
            break;
        }
        sleep_ms(1000);
    }
}

/* ------------------------------------------------------- console (getty) */

static const char *console_banner(void)
{
    /* exactly L2's console wording */
    if (file_exists(INSTALLED_MARK))
        return "L2 installed system ready\nCommands: l2fetch, l2net, l2wifi, l2pm, poweroff, reboot.";
    return "L2 live system ready\nCommands: l2fetch, l2net, l2wifi, l2install, poweroff. RAM-only session: changes are lost at shutdown.";
}

static int console_has_ttys(const char *dev)
{
    /* "ttyS" substring check without strstr, bounded by the string length:
     * scanning past the NUL read adjacent argv memory and could match a
     * "ttyS" belonging to the next string (wrong TERM for short names).
     * (maiake-f6 auditor D5 fix, 04-rcl2.md §5, CWE-125.) */
    unsigned len = (unsigned)strlen(dev);
    if (len < 4) return 0;
    for (unsigned i = 4; i <= len; i++)
        if (dev[i - 4] == 't' && dev[i - 3] == 't' && dev[i - 2] == 'y' && dev[i - 1] == 'S')
            return 1;
    return 0;
}

__attribute__((noreturn))
static void console_run(int argc, char **argv)
{
    const char *dev = (argc > 2) ? argv[2] : "/dev/tty1";
    r_sys0(SYS_setsid);
    int fd = (int)r_sys3(SYS_open, (long)dev, O_RDWR, 0);
    /* Fall back to /dev/console only for the default (argument-less) console.
     * An explicit /dev/ttyX that cannot be opened must fail: two console
     * services silently falling back would otherwise spawn two shells on the
     * same terminal. */
    if (fd < 0 && argc <= 2 && !streq(dev, CONSOLE_DEV)) {
        dev = CONSOLE_DEV;
        fd = (int)r_sys3(SYS_open, (long)dev, O_RDWR, 0);
    }
    if (fd < 0) {
        g_len = 0; putstr("rcl2 console: cannot open "); putstr(dev); line_out();
        die_ex(1);
    }
    r_sys3(SYS_ioctl, (long)fd, TIOCSCTTY, 1L);

    /* same termios as L2's init console */
    struct ktio t;
    if (r_sys3(SYS_ioctl, (long)fd, TCGETS, (long)&t) == 0) {
        t.c_iflag = ICRNL | IXON | BRKINT;
        t.c_oflag = OPOST | ONLCR;
        t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
        t.c_cc[VINTR] = 3;          /* ^C      */
        t.c_cc[VEOF]  = 4;          /* ^D      */
        t.c_cc[VERASE] = 127;       /* DEL     */
        t.c_cc[VKILL] = 21;         /* ^U      */
        t.c_cc[VSUSP] = 26;         /* ^Z      */
        r_sys3(SYS_ioctl, (long)fd, TCSETS, (long)&t);
    }
    struct winsz w;
    if (r_sys3(SYS_ioctl, (long)fd, TIOCGWINSZ, (long)&w) == 0) {
        if (!w.row || !w.col) { w.row = 24; w.col = 80; r_sys3(SYS_ioctl, (long)fd, TIOCSWINSZ, (long)&w); }
    }

    for (int i = 0; i < 3; i++)
        if (i != fd) r_sys2(SYS_dup2, fd, i);
    if (fd > 2) r_sys1(SYS_close, fd);

    r_sys1(SYS_chdir, (long)"/root");

    char e_path[48], e_home[16], e_user[16], e_term[16], e_ps1[16];
    char *envp[6];
    env_write(e_path, sizeof(e_path), "PATH=", "/bin:/sbin:/usr/bin:/usr/sbin");
    env_write(e_home, sizeof(e_home), "HOME=", "/root");
    env_write(e_user, sizeof(e_user), "USER=", "root");
    env_write(e_term, sizeof(e_term), "TERM=", console_has_ttys(dev) ? "vt100" : "linux");
    env_write(e_ps1,  sizeof(e_ps1),  "PS1=", "L2# ");
    envp[0] = e_path; envp[1] = e_home; envp[2] = e_user; envp[3] = e_term; envp[4] = e_ps1; envp[5] = 0;

    char *argv2[3];
    argv2[0] = (char *)DEFAULT_SHELL;
    argv2[1] = (char *)"-i";
    argv2[2] = 0;

    /* banner then interactive shell */
    line_out();   /* newline after any prior kernel noise */
    putstr(console_banner());
    line_out();

    long x = r_sys3(SYS_execve, (long)DEFAULT_SHELL, (long)argv2, (long)envp);
    (void)x;
    static const char msg[] = "rcl2 console: exec failed\n";
    r_sys3(SYS_write, 2L, (long)msg, (long)sizeof(msg) - 1);
    die_ex(127);
}

/* -------------------------------------------------------- power-off mode */

static void reboot_now(unsigned cmd) __attribute__((noreturn));

static void reboot_now(unsigned cmd)
{
    r_sys0(SYS_sync);
    sleep_ms(100);
    r_sys0(SYS_sync);
    r_sys5(SYS_mount, 0L, (long)"/", 0L, (long)(MS_REMOUNT | MS_RDONLY), 0L);
    long r = r_sys4(SYS_reboot, (long)REBOOT_MAGIC1, (long)REBOOT_MAGIC2, (long)cmd, 0);
    if (r != 0) {
        g_len = 0; putstr("rcl2: reboot syscall failed (errno "); putnum(-r); putstr(")");
        line_out();
        die_ex(1);
    }
    for (;;) sleep_ms(3600000L);   /* if it ever returns, wait forever */
}

__attribute__((noreturn))
static void power_direct(const char *cmd)
{
    if (streq(cmd, "poweroff"))     reboot_now(RB_POWER_OFF);
    if (streq(cmd, "halt"))         reboot_now(RB_HALT);
    /* "reboot" compatible with init's RB_AUTOBOOT */
    reboot_now(RB_RESTART);
}

/* ------------------------------------------------------------ shutdown */

__attribute__((noreturn))
static void shutdown_run(const char *why, unsigned cmd)
{
    g_closing = 1;
    slog1("shutting down: ", why);

    for (unsigned i = 0; i < g_conf.nsvc; i++)
        if (g_pid[i] > 0) r_sys2(SYS_kill, g_pid[i], SIGTERM);
    r_sys2(SYS_kill, -1L, SIGTERM);          /* everything else, pid1 gets none */

    unsigned long until = now_ms() + GRACE_TERM_MS;
    while (now_ms() < until) {
        if (reap_children()) break;
        sleep_ms(100);
    }

    r_sys2(SYS_kill, -1L, SIGKILL);
    until = now_ms() + GRACE_KILL_MS;
    while (now_ms() < until) {
        int zz = 0;
        long w;
        while ((w = r_sys4(SYS_wait4, -1L, (long)&zz, WNOHANG, 0)) > 0) zz = 0;
        if (w == -ECHILD) break;
        sleep_ms(100);
    }

    r_sys0(SYS_sync);
    sleep_ms(100);
    r_sys0(SYS_sync);
    /* remount root read-only, best effort */
    r_sys5(SYS_mount, 0L, (long)"/", 0L, (long)(MS_REMOUNT | MS_RDONLY), 0L);

    long r = r_sys4(SYS_reboot, (long)REBOOT_MAGIC1, (long)REBOOT_MAGIC2, (long)cmd, 0);
    if (r != 0) {
        slog1("reboot syscall failed (", why);
        /* power management absent: degrade to halt, then idle forever */
        r_sys4(SYS_reboot, (long)REBOOT_MAGIC1, (long)REBOOT_MAGIC2, (long)RB_HALT, 0);
        for (;;) sleep_ms(3600000L);
    }
    for (;;) sleep_ms(3600000L);
}

/* -------------------------------------------------------------- rss report */

static void rss_report(void)
{
    /* g_iobuf reuse (auditor carry, -256 B bss): called after cmdline_flags
     * in pid1_boot and after all printout in selftest - no live consumer */
    int n = file_read(STATM_PATH, g_iobuf, (int)sizeof(g_iobuf), 0);
    if (n < 0) { slog("rss report unavailable"); return; }
    unsigned long pages = rcl_rs_statm(g_iobuf, (unsigned long)n);
    if (pages == 0) { slog("rss report parse failed"); return; }
    g_len = 0;
    putstr("rcl2: resident "); putnum((long)(pages * 4));
    putstr(" KiB (budget 150)"); line_out();
}

/* C-side sink for Rust panics: log on the console, then idle PID 1 forever.
 * Only raw syscall writes, so it is safe from any context. */
void rcl_c_panic(const void *msg, unsigned long len)
{
    static const char head[] = "rcl2 PANIC: ";
    static const char tail[] = "\nrcl2: pid1 will not exit, idling\n";
    r_sys3(SYS_write, 2L, (long)head, (long)sizeof(head) - 1);
    if (msg && len > 0) r_sys3(SYS_write, 2L, (long)msg, (long)len);
    r_sys3(SYS_write, 2L, (long)tail, (long)sizeof(tail) - 1);
    for (;;) sleep_ms(3600000L);
}

/* -------------------------------------------------------------- selftest */

static const char selftest_conf[] =
    "# rcl2 selftest fixture\n"
    "hostname ST\n"
    "klog 2\n"
    "setenv X=v\n"
    "ctrlaltdel poweroff\n"
    "service a respawn /bin/true arg1 arg2\n"
    "service b once /bin/false\n"
    "service c wait /bin/sh -c x\n"
    "bogus line should be skipped\n"
    "\n\n";

static const char *selftest_cmdlines[] = {
    "rdinit=/init console=tty0 console=ttyS0,115200 loglevel=4 l2.rescue=1",
    "root=PARTUUID=x rootfstype=ext4 rootwait rw init=/sbin/rcl2 s",
    "root=PARTUUID=x rootfstype=ext4 rw",
    /* /proc/cmdline always ends in '\n': a rescue flag as the LAST token
     * must still match (regression: '\n' used to glue onto the token) */
    "root=PARTUUID=x rw l2.rescue=1\n",
    "root=PARTUUID=x rw single\n",
};

static const char *mode_str(unsigned char m)
{
    if (m == RCL_MODE_ONCE) return "once";
    if (m == RCL_MODE_WAIT) return "wait";
    return "respawn";
}

static void selftest(int argc, char **argv)
{
    char banner[80];

    rcl_rs_banner(banner, sizeof(banner));
    g_len = 0; putstr(banner); putstr(" (selftest)"); line_out();

    const char *confdata = selftest_conf;
    unsigned long conflen = strlen(selftest_conf);
    const char *label = "builtin";
    if (argc > 2) {
        int more = -1;
        int n = file_read(argv[2], g_iobuf, (int)sizeof(g_iobuf), &more);
        if (n == -EFBIG) {
            /* mirror conf_load: a newline probe means complete lines */
            if (more == '\n') n = (int)sizeof(g_iobuf) - 1;
            else { slog1("config too large: ", argv[2]); die_ex(1); }
        }
        if (n < 0) { slog1("cannot read ", argv[2]); die_ex(1); }
        confdata = g_iobuf;
        /* parse the bytes actually read, exactly like conf_load at boot:
         * strlen would hide everything after an embedded NUL */
        conflen = (unsigned long)n;
        label = argv[2];
    }
    long got = rcl_rs_parse(&g_conf, confdata, conflen);
    slogn("config ", got, " service(s) accepted (bad lines skipped)");
    g_len = 0;
    putstr("hostname="); putstr(g_conf.hostname);
    putstr(" klog="); putnum(g_conf.klog);
    putstr(" cad="); putnum(g_conf.cad);
    putstr(" nenv="); putnum(g_conf.nenv);
    line_out();
    for (unsigned i = 0; i < g_conf.nsvc; i++) {
        struct rcl_svc *s = &g_conf.svcs[i];
        g_len = 0;
        putstr("  svc "); putnum((long)i);
        putstr(" ["); putstr(s->name); putstr("] mode="); putstr(mode_str(s->mode));
        putstr(" path="); putstr(s->path);
        putstr(" nargs="); putnum(s->nargs);
        for (unsigned a = 0; a < s->nargs; a++) {
            putstr(" \""); putstr(s->args[a]); putstr("\"");
        }
        line_out();
    }

    for (unsigned t = 0; t < sizeof(selftest_cmdlines) / sizeof(selftest_cmdlines[0]); t++) {
        unsigned f = rcl_rs_bootflags(selftest_cmdlines[t], strlen(selftest_cmdlines[t]));
        g_len = 0; putstr("cmdline test "); putnum((long)t);
        putstr(" flags="); putnum((long)f);
        line_out();
    }

    rss_report();
    slog1("selftest of ", label);
    slog("selftest ok");
}

/* -------------------------------------------------------------- pid 1 boot */

static void pid1_boot(void) __attribute__((noreturn));

static void pid1_boot(void)
{
    char banner[80];

    signals_setup();
    rcl_rs_banner(banner, sizeof(banner));
    g_len = 0; putstr(banner); putstr(": booted"); line_out();

    mounts_setup();
    conf_load();

    if (g_conf.klog) {
        long r = r_sys3(SYS_syslog, 8L, 0L, (long)g_conf.klog);
        if (r != 0) slog("klogctl refused");
    }
    hostname_apply();
    env_setup();

    unsigned flags = cmdline_flags();

    argv_build();
    svc_state_clear();

    if (flags & BOOTFLAG_RESCUE) rescue_shell();   /* never returns (L2 semantics) */

    for (unsigned i = 0; i < g_conf.nsvc; i++) {
        struct rcl_svc *s = &g_conf.svcs[i];
        if (s->mode == RCL_MODE_WAIT) { svc_spawn(i); svc_wait_boot(i); }
        else svc_spawn(i);
        reap_children();
    }

    rss_report();
    slogn("boot complete: ", (long)g_conf.nsvc, " service(s), signals armed");
    supervise();
}

/* ------------------------------------------------------- supervise loop */

static unsigned long earliest_pending(void)
{
    unsigned long now = now_ms();
    unsigned long best = 0;          /* 0 = nothing pending */
    for (unsigned i = 0; i < g_conf.nsvc; i++) {
        if (g_pid[i] || !g_next[i]) continue;
        if (g_conf.svcs[i].mode != RCL_MODE_RESPAWN) continue;
        if (g_next[i] <= now) return 50;          /* due: nudge the loop */
        unsigned long rem = g_next[i] - now;
        if (best == 0 || rem < best) best = rem;
    }
    return best;
}

static void spawn_due(void)
{
    unsigned long now = now_ms();
    for (unsigned i = 0; i < g_conf.nsvc; i++) {
        if (g_pid[i] || !g_next[i] || g_next[i] > now) continue;
        if (g_conf.svcs[i].mode != RCL_MODE_RESPAWN) { g_next[i] = 0; continue; }
        svc_spawn(i);
        reap_children();
    }
}

static void supervise(void)
{
    struct pollfdx pf;

    for (;;) {
        pf.fd = g_sig_rd;
        pf.events = POLLIN;
        pf.revents = 0;

        reap_children();
        take_actions();          /* shutdown_run never returns when it fires */

        spawn_due();

        unsigned long wait = earliest_pending();
        /* Without a self-pipe, a signal just before ppoll can lose its wakeup. */
        if (g_sig_rd < 0 && (wait == 0 || wait > 1000)) wait = 1000;
        struct kts tv;
        if (wait) {
            if (wait < 50) wait = 50;
            tv.tv_sec = wait / 1000;
            tv.tv_nsec = (long)(wait % 1000) * 1000000L;
        }
        long r = r_sys5(SYS_ppoll, (long)&pf, 1L, wait ? (long)&tv : 0L, 0L, 0L);
        if (r < 0 && r != -EINTR) sleep_ms(50);
        if (pf.revents & POLLIN) pipe_drain();
    }
}

/* ------------------------------------------------------------- dispatch */

void rcl_enter(unsigned long *sp)
{
    unsigned long argc = sp[0];
    char **argv = (char **)(sp + 1);
    const char *a1 = (argc > 1) ? argv[1] : 0;

    if (a1 && streq(a1, "--selftest")) { selftest(argc, argv); die_ex(0); }
    if (a1 && (streq(a1, "poweroff") || streq(a1, "reboot") || streq(a1, "halt")))
        power_direct(a1);
    if (a1 && streq(a1, "console")) console_run(argc, argv);

    long pid = r_sys0(SYS_getpid);
    if (pid != 1) {
        static const char usage[] =
            "rcl2: not running as pid 1\n";
        g_len = 0; putstr(usage); line_out();
        die_ex(1);
    }
    if (a1) slog1("ignoring kernel arg ", a1);
    pid1_boot();
}

