/* Exercise the actual supervisor with a fake clock, fork, and wait queue.
 * No real children, signals, mounts, or shutdown syscalls are permitted.
 * Config/cmdline tests additionally use fake files and the real Rust core.
 *
 * Ported from the maiake-f6 auditor worktree (commit 0792991, packet
 * build/campaigns/04-rcl2.md §3/§6): fake file I/O + iobuf_aliasing,
 * conf_exact_fit, cmdline_newline, cmdline_truncated, service_args,
 * console_ttys regressions and the SKIP_TESTS mask. conf_truncated was
 * adapted from the auditor's drop-partial-line semantics to this tree's
 * W1 EFBIG semantics (oversize config -> refuse -> default shell); the
 * mid-line-cut hazard itself is also covered by test-truncation.c.
 *
 * -DSKIP_TESTS=<bitmask> disables individual regression tests (1=aliasing,
 * 2=conf-truncated, 4=conf-exact-fit, 8=cmdline-newline, 16=cmdline-truncated,
 * 32=service-args, 64=console-ttys); used to demonstrate pre-fix failures.
 * The campaign-9 live /tmp cap decision is tested in test-tmpcap.c. */
#include "../src/syscalls.h"

#ifndef SKIP_TESTS
#define SKIP_TESTS 0
#endif

/* src/mem.c stubs (linked in, same as the init binary) */
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

static const char *test_name = "supervision";

__attribute__((noreturn)) static void finish(int status)
{
    if (status == 1) {
        unsigned long n = 0;
        while (test_name[n]) n++;
        r_sys3(SYS_write, 2L, (long)"FAIL: ", 6L);
        r_sys3(SYS_write, 2L, (long)test_name, (long)n);
        r_sys3(SYS_write, 2L, (long)"\n", 1L);
    }
    r_sys1(SYS_exit, status);
    __builtin_unreachable();
}

static long fake_syscall(long, long, long, long, long, long, long);
#define r_sys6 fake_syscall
#define rcl_enter unused_init_entry
#include "../src/main.c"
#undef rcl_enter
#undef r_sys6

static unsigned long clock_ms = 1000;
static long fork_result = 42;
static long exited_pid;
static int interrupt_wait;
static unsigned polls;

#define CHECK(condition) do { if (!(condition)) finish(1); } while (0)

/* ---- fake file I/O -------------------------------------------------------
 * Lets conf_load()/cmdline_flags() run against the REAL Rust parser with
 * controlled content, including files larger than the read caps. fds 10..15
 * map to registered fake files; anything else is ENOENT. */
struct fake_file { const char *path; const char *data; unsigned long len; };
static struct fake_file fake_files[6];
static unsigned fake_nfiles;
static int fake_fd_file[6];               /* slot -> file index, -1 = closed */
static unsigned long fake_off[6];

static void fake_file_reset(void)
{
    fake_nfiles = 0;
    for (unsigned i = 0; i < 6; i++) fake_fd_file[i] = -1;
}

static void fake_file_add(const char *path, const char *data, unsigned long len)
{
    CHECK(fake_nfiles < 6);
    fake_files[fake_nfiles].path = path;
    fake_files[fake_nfiles].data = data;
    fake_files[fake_nfiles].len = len;
    fake_nfiles++;
}

static unsigned buf_put(char *buf, unsigned k, const char *s)
{
    while (*s) buf[k++] = *s++;
    return k;
}

static long fake_syscall(long nr, long a, long b, long c,
                         long d, long e, long f)
{
    (void)d; (void)e; (void)f;
    switch (nr) {
    case SYS_open: {
        for (unsigned i = 0; i < fake_nfiles; i++)
            if (streq((const char *)a, fake_files[i].path)) {
                for (unsigned s = 0; s < 6; s++)
                    if (fake_fd_file[s] < 0) {
                        fake_fd_file[s] = (int)i;
                        fake_off[s] = 0;
                        return 10L + (long)s;
                    }
                return -EINVAL;
            }
        return -ENOENT;
    }
    case SYS_read: {
        if (a < 10 || a > 15 || fake_fd_file[a - 10] < 0) return -EINVAL;
        struct fake_file *fl = &fake_files[fake_fd_file[a - 10]];
        unsigned long off = fake_off[a - 10];
        unsigned long n = fl->len - off;
        if (n > (unsigned long)c) n = (unsigned long)c;
        memcpy((void *)b, fl->data + off, n);
        fake_off[a - 10] = off + n;
        return (long)n;
    }
    case SYS_close:
        if (a >= 10 && a <= 15) fake_fd_file[a - 10] = -1;
        return 0;
    case SYS_clock_gettime: {
        struct kts *t = (struct kts *)b;
        t->tv_sec = clock_ms / 1000;
        t->tv_nsec = (clock_ms % 1000) * 1000000;
        return 0;
    }
    case SYS_fork: return fork_result;
    case SYS_write: return c;
    case SYS_wait4: {
        if (interrupt_wait) { interrupt_wait = 0; return -EINTR; }
        long pid = exited_pid;
        exited_pid = 0;
        *(int *)b = 127 << 8;
        return pid;
    }
    case SYS_ppoll: {
        struct pollfdx *pf = (struct pollfdx *)a;
        struct kts *timeout = (struct kts *)c;
        if (polls++ == 0) {
            CHECK(pf->fd == 3 && timeout == 0);
            g_sig_rd = -1;
            return -EINTR;  /* the wakeup must not take a nanosleep detour */
        }
        CHECK(pf->fd == -1 && timeout != 0);
        CHECK(timeout->tv_sec == 1 && timeout->tv_nsec == 0);
        if (polls == 2) {
            exited_pid = g_pid[0];
            clock_ms += 10;
        } else if (polls == 3) {
            CHECK(g_pid[0] == 0 && g_backoff[0] == SB_CEIL_MS);
            clock_ms = g_next[0];
        } else {
            CHECK(g_pid[0] == fork_result && g_next[0] == 0);
            finish(0);
        }
        return 0;
    }
    default: finish(2);
    }
}

/* ---- config / cmdline / parser regression tests -------------------------
 * These run against the real Rust core (librcl2rs.a is linked in) and the
 * real C read path with fake files. */

/* g_iobuf is shared by conf_load and the cmdline read. The parser must COPY
 * every string into g_conf; if it ever retains pointers into the input, the
 * later cmdline read silently corrupts the live config. Pin the contract. */
static void test_iobuf_aliasing(void)
{
    test_name = "iobuf_aliasing";
    fake_file_reset();
    const char *conf = "hostname ALIAS\nservice keep respawn /bin/dash -i\n";
    fake_file_add(CONFIG_PATH, conf, strlen(conf));
    memset(&g_conf, 0, sizeof g_conf);
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.hostname, "ALIAS"));
    CHECK(streq(g_conf.svcs[0].path, "/bin/dash"));
    CHECK(streq(g_conf.svcs[0].args[0], "-i"));
    /* pid1_boot reads /proc/cmdline into the same scratch buffer afterwards */
    const char *cmd = "rdinit=/init l2.rescue=1 console=ttyS0\n";
    fake_file_add(CMDLINE_PATH, cmd, strlen(cmd));
    CHECK(cmdline_flags() == BOOTFLAG_RESCUE);
    /* config must be untouched by the g_iobuf reuse */
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.hostname, "ALIAS"));
    CHECK(streq(g_conf.svcs[0].name, "keep"));
    CHECK(streq(g_conf.svcs[0].path, "/bin/dash"));
    CHECK(streq(g_conf.svcs[0].args[0], "-i"));
}

/* A config larger than CFG_RAW_CAP ends mid-line. The fragment must NOT be
 * parsed as a whole line: "service evil respawn /bin/da" would exec /bin/da
 * instead of /bin/dash. ADAPTED to this tree's W1 semantics: the whole
 * oversize config is refused (-EFBIG, probe byte non-whitespace) and the
 * default respawn shell takes over - PID1 never loses its shell. (The
 * auditor's variant kept complete prefix lines; both are safe.) */
static char bigconf[4096];
static void test_conf_truncated(void)
{
    test_name = "conf_truncated";
    unsigned k = 0;
    k = buf_put(bigconf, k, "service good respawn /bin/dash -i\n"); /* 34 */
    k = buf_put(bigconf, k, "#");
    while (k < 2018) bigconf[k++] = 'x';
    bigconf[k++] = '\n';                            /* complete prefix: 2019 */
    k = buf_put(bigconf, k, "service evil respawn /bin/dash -i"); /* 33, no NL */
    CHECK(k == 2052);                 /* read keeps 2047: fragment "/bin/da" */
    fake_file_reset();
    fake_file_add(CONFIG_PATH, bigconf, k);
    memset(&g_conf, 0, sizeof g_conf);
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].name, "shell"));
    CHECK(streq(g_conf.svcs[0].path, DEFAULT_SHELL));
}

/* Exactly cap-1 bytes with a complete (newline-less) last line is NOT
 * truncation: the probe read hits EOF and nothing may be dropped. */
static void test_conf_exact_fit(void)
{
    test_name = "conf_exact_fit";
    unsigned k = 0;
    for (unsigned i = 0; i < 31; i++) {             /* 31 x 64 = 1984 */
        bigconf[k++] = '#';
        for (unsigned j = 0; j < 62; j++) bigconf[k++] = 'x';
        bigconf[k++] = '\n';
    }
    CHECK(k == 1984);
    k = buf_put(bigconf, k, "service last once /bin/true");       /* 27 */
    while (k < 2047) bigconf[k++] = ' ';            /* trailing ws is inert */
    CHECK(k == 2047);
    fake_file_reset();
    fake_file_add(CONFIG_PATH, bigconf, k);
    memset(&g_conf, 0, sizeof g_conf);
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].name, "last"));
    CHECK(g_conf.svcs[0].mode == RCL_MODE_ONCE);
}

/* /proc/cmdline ends with '\n'. A rescue marker as the LAST token must still
 * match: the newline must be token whitespace, not part of the token. */
static void test_cmdline_newline(void)
{
    test_name = "cmdline_newline";
    static const char *cls[] = {
        "rdinit=/init console=ttyS0,115200 l2.rescue=1\n",
        "root=/dev/x single\n",
        "root=/dev/x s\n",
        "root=/dev/x emergency\n",
        "root=/dev/x -b\n",
        "root=/dev/x -s\n",
        "root=/dev/x l2.rescue=1 rw\n",
    };
    for (unsigned i = 0; i < sizeof(cls) / sizeof(cls[0]); i++)
        CHECK(rcl_rs_bootflags(cls[i], strlen(cls[i])) == BOOTFLAG_RESCUE);
    const char *norm = "root=/dev/x rw\n";
    CHECK(rcl_rs_bootflags(norm, strlen(norm)) == 0);
}

/* A cmdline longer than CMDLINE_CAP can end mid-token. The partial token
 * must be dropped: "single" cut to "s" would falsely trigger rescue mode. */
static char bigcmd[4096];
static void test_cmdline_truncated(void)
{
    test_name = "cmdline_truncated";
    unsigned k = buf_put(bigcmd, 0, "root=/dev/x ");
    while (k + 9 <= 2046) k = buf_put(bigcmd, k, "aaaaaaaa ");
    while (k < 2046) bigcmd[k++] = ' ';
    k = buf_put(bigcmd, k, "single");               /* kept part ends in "s" */
    CHECK(k == 2052);
    fake_file_reset();
    fake_file_add(CMDLINE_PATH, bigcmd, k);
    CHECK(cmdline_flags() == 0);
    /* a COMPLETE rescue token earlier in the same truncated cmdline survives */
    k = buf_put(bigcmd, 0, "root=/dev/x emergency ");
    while (k + 10 <= 2046) k = buf_put(bigcmd, k, "aaaaaaaaa ");
    while (k < 2046) bigcmd[k++] = ' ';
    k = buf_put(bigcmd, k, "single");
    CHECK(k == 2052);
    fake_file_reset();
    fake_file_add(CMDLINE_PATH, bigcmd, k);
    CHECK(cmdline_flags() == BOOTFLAG_RESCUE);
}

/* Over-long args must be rejected like over-long paths, not silently
 * truncated (a cut `sh -c` script executes something else). More than
 * RCL_MAX_ARGS args must be rejected, not silently dropped. */
static char argconf[512];
static void test_service_args(void)
{
    test_name = "service_args";
    unsigned k = buf_put(argconf, 0, "service a respawn /bin/sh -c ");
    for (unsigned j = 0; j < 63; j++) argconf[k++] = 'x';
    argconf[k++] = '\n';
    memset(&g_conf, 0, sizeof g_conf);
    CHECK(rcl_rs_parse(&g_conf, argconf, k) == 1);
    CHECK(g_conf.svcs[0].nargs == 2);
    CHECK(strlen(g_conf.svcs[0].args[1]) == 63);

    k = buf_put(argconf, 0, "service a respawn /bin/sh -c ");
    for (unsigned j = 0; j < 64; j++) argconf[k++] = 'x';
    argconf[k++] = '\n';
    memset(&g_conf, 0, sizeof g_conf);
    CHECK(rcl_rs_parse(&g_conf, argconf, k) == 0);

    k = buf_put(argconf, 0, "service a respawn /bin/sh a b c d e f g\n");
    memset(&g_conf, 0, sizeof g_conf);
    CHECK(rcl_rs_parse(&g_conf, argconf, k) == 0);

    k = buf_put(argconf, 0, "service a respawn /bin/sh a b c d e f\n");
    memset(&g_conf, 0, sizeof g_conf);
    CHECK(rcl_rs_parse(&g_conf, argconf, k) == 1);
    CHECK(g_conf.svcs[0].nargs == 6);
}

/* console_has_ttys must not scan past the string's NUL: with adjacent memory
 * spelling "ttyS", a one-char device name used to match and set TERM=vt100. */
static char craft[16] = "x\0\0ttyS0";
static char empty[16] = "";
static char ttys[16] = "ttyS";
static void test_console_ttys(void)
{
    test_name = "console_ttys";
    CHECK(console_has_ttys("/dev/ttyS0") == 1);
    CHECK(console_has_ttys("/dev/tty1") == 0);
    CHECK(console_has_ttys(ttys) == 1);
    CHECK(console_has_ttys(empty) == 0);
    CHECK(console_has_ttys(craft) == 0);
}

static void crash_after(unsigned long duration, int expected_delay)
{
    CHECK(g_pid[0] == fork_result);
    clock_ms += duration;
    exited_pid = g_pid[0];
    reap_children();
    CHECK(g_pid[0] == 0);
    CHECK(g_backoff[0] == expected_delay);
    CHECK(g_next[0] == clock_ms + (unsigned long)expected_delay);
}

void rcl_enter(unsigned long *sp)
{
    (void)sp;
    /* parser / read-path regressions first (real Rust core, fake files) */
    if (!(SKIP_TESTS & 1))  test_iobuf_aliasing();
    if (!(SKIP_TESTS & 2))  test_conf_truncated();
    if (!(SKIP_TESTS & 4))  test_conf_exact_fit();
    if (!(SKIP_TESTS & 8))  test_cmdline_newline();
    if (!(SKIP_TESTS & 16)) test_cmdline_truncated();
    if (!(SKIP_TESTS & 32)) test_service_args();
    if (!(SKIP_TESTS & 64)) test_console_ttys();
    fake_file_reset();
    memset(&g_conf, 0, sizeof g_conf);
    g_len = 0;
    test_name = "supervision";

    /* supervisor sequence below needs a pristine single respawn service */
    g_conf.nsvc = 1;
    g_conf.svcs[0].mode = RCL_MODE_RESPAWN;
    svc_state_clear();
    svc_spawn(0);
    /* A signal interrupting wait4 must not leave the exit queued. */
    interrupt_wait = 1;
    const int delays[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        crash_after(10, delays[i]);
        clock_ms = g_next[0] - 1;
        spawn_due();
        CHECK(g_pid[0] == 0);
        clock_ms++;
        spawn_due();
        CHECK(g_pid[0] == fork_result);
        CHECK(g_next[0] == 0);
    }
    /* Reset only after the service survives the stability threshold. */
    crash_after(SB_STABLE_MS - 1, 30000);
    clock_ms = g_next[0];
    spawn_due();
    crash_after(SB_STABLE_MS, 1000);
    clock_ms = g_next[0];
    spawn_due();
    crash_after(10, 2000);

    /* A failed fork must retain and increase the existing crash delay. */
    clock_ms = g_next[0];
    fork_result = -EAGAIN;
    spawn_due();
    CHECK(g_pid[0] == 0 && g_backoff[0] == 4000);
    CHECK(g_next[0] == clock_ms + 4000);
    clock_ms = g_next[0];
    fork_result = 43;
    spawn_due();
    crash_after(10, 8000);

    /* Once services and shutdown never schedule a restart. */
    g_conf.svcs[0].mode = RCL_MODE_ONCE;
    svc_spawn(0);
    exited_pid = g_pid[0];
    reap_children();
    CHECK(g_pid[0] == 0 && g_next[0] == 0);
    g_conf.svcs[0].mode = RCL_MODE_RESPAWN;
    svc_spawn(0);
    g_closing = 1;
    exited_pid = g_pid[0];
    reap_children();
    CHECK(g_pid[0] == 0 && g_next[0] == 0);
    /* Losing the wakeup pipe must bound both idle and backoff waits. */
    g_closing = 0;
    svc_state_clear();
    svc_spawn(0);
    g_backoff[0] = SB_CEIL_MS;
    g_sig_rd = 3;
    supervise();
}
