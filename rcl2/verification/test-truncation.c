/* Regression tests for the file_read silent-truncation hazard and its call
 * sites (config load, cmdline boot flags, /proc/mounts ro guard), plus the
 * parser's NUL/overlong-token handling. Fake open/read/close serve synthetic
 * files; no real filesystem access, no children, no signals.
 *
 * The oversized-input cases FAILED before the truncation fix (file_read
 * returned a short count with no overflow signal; see the campaign probe):
 *   - a cut config line exec'd a shortened path prefix ("/bin/l2")
 *   - a cmdline cut to the token "s" raised a spurious rescue flag
 *   - a "/" ro mount line past byte 2047 was dropped
 *   - a NUL inside a path token was accepted (execve stops at the NUL)
 */
#include "../src/syscalls.h"

void *memcpy(void *dst, const void *src, unsigned long n);   /* src/mem.c */
void *memmove(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

__attribute__((noreturn)) static void finish(int status)
{
    r_sys1(SYS_exit, status);
    __builtin_unreachable();
}

static long fake_syscall(long, long, long, long, long, long, long);
#define r_sys6 fake_syscall
#define rcl_enter unused_init_entry
#include "../src/main.c"
#undef rcl_enter
#undef r_sys6

#define CHECK(condition) do { if (!(condition)) finish(1); } while (0)

/* ---------------------------------------------------- synthetic file table */

struct fakefile { const char *path; const char *data; int len; int pos; int chunk; };
static struct fakefile g_files[4];
static int g_nfiles;
static int g_eintr_left;                 /* inject EINTR into the next N reads */
static char g_data[4096];

static void serve(const char *path, const char *data, int len, int chunk)
{
    CHECK(g_nfiles < 4);
    g_files[g_nfiles++] = (struct fakefile){ path, data, len, 0, chunk };
}

static void reset(void)
{
    g_nfiles = 0;
    g_eintr_left = 0;
    memset(&g_conf, 0, sizeof(g_conf));
}

static long fake_syscall(long nr, long a, long b, long c, long d, long e, long f)
{
    (void)d; (void)e; (void)f;
    switch (nr) {
    case SYS_open:
        for (int i = 0; i < g_nfiles; i++)
            if (streq(g_files[i].path, (const char *)a)) return 10 + i;
        return -ENOENT;
    case SYS_read: {
        if (g_eintr_left > 0) { g_eintr_left--; return -EINTR; }
        CHECK(a >= 10 && a < 10 + g_nfiles);
        struct fakefile *fl = &g_files[a - 10];
        int n = fl->len - fl->pos;
        if (n > fl->chunk) n = fl->chunk;
        if (n > (int)c) n = (int)c;
        if (n <= 0) return 0;
        memcpy((void *)b, fl->data + fl->pos, (unsigned long)n);
        fl->pos += n;
        return n;
    }
    case SYS_close: return 0;
    case SYS_write: return c;
    case SYS_clock_gettime: {
        struct kts *t = (struct kts *)b;
        t->tv_sec = 1; t->tv_nsec = 0;
        return 0;
    }
    default: finish(2);
    }
    return 0;
}

/* exactly n bytes of comment lines (n % 40 must not leave a 1-byte rest) */
static int fill_comments(int n)
{
    int k = 0;
    while (n - k >= 40) {
        g_data[k] = '#';
        for (int j = 1; j < 39; j++) g_data[k + j] = 'a';
        g_data[k + 39] = '\n';
        k += 40;
    }
    CHECK(n - k != 1);
    if (n - k > 1) {
        g_data[k++] = '#';
        while (n - k > 1) g_data[k++] = 'a';
        g_data[k++] = '\n';
    }
    return k;
}

/* append a mount filler line "tmpfs /mnt/nNN tmpfs rw,relatime 0 0\n" */
static int mounts_filler(int k, int nlines)
{
    for (int i = 0; i < nlines; i++) {
        const char *pre = "tmpfs /mnt/n";
        for (int j = 0; pre[j]; j++) g_data[k++] = pre[j];
        g_data[k++] = (char)('0' + i / 10);
        g_data[k++] = (char)('0' + i % 10);
        const char *post = " tmpfs rw,relatime 0 0\n";
        for (int j = 0; post[j]; j++) g_data[k++] = post[j];
    }
    return k;
}

static int build_mounts(int nlines, const char *rootline)
{
    int k = mounts_filler(0, nlines);
    int rl = (int)strlen(rootline);
    memcpy(g_data + k, rootline, (unsigned long)rl);
    return k + rl;
}

/* ------------------------------------------------------- file_read itself */

static void test_file_read_boundaries(void)
{
    char buf[CFG_RAW_CAP];
    int more = 99;
    reset();
    /* exact fit: cap-1 bytes is a complete read, not an overflow */
    int k = fill_comments(CFG_RAW_CAP - 1);
    serve("/tmp/x", g_data, k, 4096);
    int n = file_read("/tmp/x", buf, CFG_RAW_CAP, &more);
    CHECK(n == CFG_RAW_CAP - 1 && buf[n] == 0 && more == -1);
    /* one byte more must be refused, not silently truncated; the probe
     * byte is exposed (here the final '\n' of the comment filler) */
    reset();
    k = fill_comments(CFG_RAW_CAP);
    serve("/tmp/x", g_data, k, 4096);
    n = file_read("/tmp/x", buf, CFG_RAW_CAP, &more);
    CHECK(n == -EFBIG && buf[CFG_RAW_CAP - 1] == 0 && more == '\n');
    /* well past capacity: probe is a mid-line byte */
    reset();
    k = fill_comments(CFG_RAW_CAP + 500);
    serve("/tmp/x", g_data, k, 4096);
    n = file_read("/tmp/x", buf, CFG_RAW_CAP, &more);
    CHECK(n == -EFBIG && more == 'a');
    /* short reads + EINTR still deliver the whole file */
    reset();
    serve("/tmp/x", "hello world", 11, 3);
    g_eintr_left = 2;
    n = file_read("/tmp/x", buf, 64, &more);
    CHECK(n == 11 && streq(buf, "hello world") && more == -1);
    /* missing file stays a plain error */
    reset();
    CHECK(file_read("/tmp/none", buf, 64, 0) == -ENOENT);
}

/* ---------------------------------------------------------- config loading */

static void test_config_oversize_refused(void)
{
    reset();
    /* cut lands 28 bytes into the service line: path becomes "/bin/l2",
     * a valid prefix of /bin/l2net - the old code would exec it */
    int k = fill_comments(2019);
    const char *svc = "service evil respawn /bin/l2net boot\n";
    int sl = (int)strlen(svc);
    memcpy(g_data + k, svc, (unsigned long)sl);
    serve(CONFIG_PATH, g_data, k + sl, 4096);
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].path, DEFAULT_SHELL));
    CHECK(streq(g_conf.svcs[0].name, "shell"));
}

static void test_config_normal_loads(void)
{
    reset();
    const char *cfg = "hostname T\nservice ok once /bin/true\n";
    serve(CONFIG_PATH, cfg, (int)strlen(cfg), 7);   /* short reads */
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].path, "/bin/true"));
    CHECK(streq(g_conf.hostname, "T"));
    /* exact-capacity config is complete and must load */
    reset();
    int k = fill_comments(CFG_RAW_CAP - 1 - 30);
    const char *tail = "service ok once /bin/true\n";
    memcpy(g_data + k, tail, strlen(tail));
    k += (int)strlen(tail);
    while (k < CFG_RAW_CAP - 1) g_data[k++] = '\n';
    serve(CONFIG_PATH, g_data, k, 4096);
    conf_load();
    CHECK(g_conf.nsvc == 1 && streq(g_conf.svcs[0].path, "/bin/true"));
}

/* W7 probe-byte refinement: a config of exactly cap bytes ending in '\n'
 * has only complete lines buffered - it must parse fully, not degrade. */
static void test_config_newline_probe_parses(void)
{
    reset();
    int k = fill_comments(2019);
    const char *svc = "service ok once /bin/true\n";   /* 26 */
    memcpy(g_data + k, svc, strlen(svc));
    k += (int)strlen(svc);                              /* 2045 */
    g_data[k++] = '\n'; g_data[k++] = '\n'; g_data[k++] = '\n';
    CHECK(k == CFG_RAW_CAP);                            /* 2048, probe '\n' */
    serve(CONFIG_PATH, g_data, k, 4096);
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].name, "ok"));
    CHECK(streq(g_conf.svcs[0].path, "/bin/true"));
}

/* A SPACE probe is NOT a line boundary for configs (further args could
 * follow on the same line): still refuse -> default shell. */
static void test_config_space_probe_refused(void)
{
    reset();
    int k = fill_comments(CFG_RAW_CAP - 1);
    g_data[k++] = ' ';
    CHECK(k == CFG_RAW_CAP);
    serve(CONFIG_PATH, g_data, k, 4096);
    conf_load();
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].name, "shell"));
    CHECK(streq(g_conf.svcs[0].path, DEFAULT_SHELL));
}

static void test_config_nul_line_skipped(void)
{
    reset();
    static const char cfg[] = "service a respawn /bin/true\0evil\nservice b once /bin/false\n";
    serve(CONFIG_PATH, cfg, (int)sizeof(cfg) - 1, 4096);
    conf_load();
    /* the NUL-bearing line must be skipped whole, never NUL-shortened */
    CHECK(g_conf.nsvc == 1);
    CHECK(streq(g_conf.svcs[0].name, "b"));
    CHECK(streq(g_conf.svcs[0].path, "/bin/false"));
}

/* ------------------------------------------------------- parser token caps */

static void test_parser_token_limits(void)
{
    char cfg[512];
    int k;
    /* path of exactly 95 chars fits; 96 must be rejected, not truncated */
    reset();
    const char *head = "service x respawn ";
    k = (int)strlen(head);
    memcpy(cfg, head, (unsigned long)k);
    cfg[k++] = '/';
    for (int i = 0; i < 94; i++) cfg[k++] = 'p';
    cfg[k++] = '\n';
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)k) == 1);
    reset();
    memmove(cfg + k - 1, "p\n", 2);      /* one more path char -> 96 */
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)(k + 1)) == 0);
    /* argument of 63 chars fits; 64 must be rejected, not truncated */
    reset();
    head = "service x respawn /bin/dash -c ";
    k = (int)strlen(head);
    memcpy(cfg, head, (unsigned long)k);
    for (int i = 0; i < 63; i++) cfg[k++] = 'q';
    cfg[k++] = '\n';
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)k) == 1);
    CHECK(g_conf.svcs[0].nargs == 2);    /* "-c" plus the 63-char script */
    reset();
    cfg[k - 1] = 'q';                    /* 64th arg char */
    cfg[k++] = '\n';
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)k) == 0);
    /* 6 args fit; a 7th must reject the line, not be silently dropped */
    reset();
    head = "service x respawn /bin/dash";
    k = (int)strlen(head);
    memcpy(cfg, head, (unsigned long)k);
    for (int i = 0; i < 6; i++) { memcpy(cfg + k, " a", 2); k += 2; }
    cfg[k++] = '\n';
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)k) == 1);
    CHECK(g_conf.svcs[0].nargs == 6);
    reset();
    cfg[k - 1] = ' ';
    memcpy(cfg + k, "a\n", 2);
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)(k + 2)) == 0);
    /* 16 services fill the table; the 17th is skipped */
    reset();
    k = 0;
    for (int i = 0; i < 17; i++) {
        memcpy(cfg + k, "service s respawn /bin/true\n", 28);
        k += 28;
    }
    CHECK(rcl_rs_parse(&g_conf, cfg, (unsigned long)k) == 16);
}

/* --------------------------------------------------------- cmdline flags */

static void test_cmdline_cut_token_dropped(void)
{
    reset();
    /* cut at byte 2047 leaves the lone token "s" - must NOT mean rescue */
    for (int i = 0; i < 2045; i++) g_data[i] = 'a';
    g_data[2045] = ' ';
    memcpy(g_data + 2046, "singleuser", 10);
    serve(CMDLINE_PATH, g_data, 2056, 4096);
    CHECK(cmdline_flags() == 0);
}

static void test_cmdline_complete_flag_preserved(void)
{
    reset();
    /* rescue flag complete before the cut: it must survive the trim */
    for (int i = 0; i < 2000; i++) g_data[i] = 'a';
    g_data[2000] = ' ';
    memcpy(g_data + 2001, "l2.rescue=1", 11);
    g_data[2012] = ' ';
    for (int i = 2013; i < 2113; i++) g_data[i] = 'b';
    serve(CMDLINE_PATH, g_data, 2113, 4096);
    CHECK(cmdline_flags() == BOOTFLAG_RESCUE);
}

static void test_cmdline_normal(void)
{
    reset();
    const char *a = "root=/dev/x l2.rescue=1\n";
    serve(CMDLINE_PATH, a, (int)strlen(a), 4096);
    CHECK(cmdline_flags() == BOOTFLAG_RESCUE);
    reset();
    const char *b = "root=/dev/x rw\n";
    serve(CMDLINE_PATH, b, (int)strlen(b), 4096);
    CHECK(cmdline_flags() == 0);
    reset();                              /* unreadable -> normal boot */
    CHECK(cmdline_flags() == 0);
}

/* W7: x86 max-length cmdline (2047 chars + '\n' = 2048 B): the newline
 * probe means every token is complete - parse fully, keep the rescue flag
 * (the W1 trim dropped the last COMPLETE token here). */
static void test_cmdline_maxlen_newline_probe(void)
{
    reset();
    for (int i = 0; i < 2035; i++) g_data[i] = 'a';
    g_data[2035] = ' ';
    memcpy(g_data + 2036, "l2.rescue=1", 11);   /* ends at 2046 */
    g_data[2047] = '\n';
    serve(CMDLINE_PATH, g_data, 2048, 4096);
    CHECK(cmdline_flags() == BOOTFLAG_RESCUE);
}

/* W7: the trim stop-set matches the Rust is_ws: a '\r' before the cut
 * token must stop the trim so a complete rescue flag before it survives
 * (the W1 stop-set {' ','\t','\n'} over-dropped it). */
static void test_cmdline_cr_stop_set(void)
{
    reset();
    for (int i = 0; i < 2000; i++) g_data[i] = 'a';
    g_data[2000] = ' ';
    memcpy(g_data + 2001, "l2.rescue=1", 11);   /* 2001..2011 */
    g_data[2012] = '\r';
    memcpy(g_data + 2013, "singleuser", 10);    /* 2013..2022, cut mid-token */
    for (int i = 2023; i < 2052; i++) g_data[i] = 'b';
    serve(CMDLINE_PATH, g_data, 2052, 4096);
    CHECK(cmdline_flags() == BOOTFLAG_RESCUE);
}

/* ------------------------------------------------ /proc/mounts ro guard */

static void test_mounts_streaming(void)
{
    /* "/" ro line far beyond the old 2047-byte window, small read chunks
     * so fields straddle chunk boundaries */
    reset();
    int len = build_mounts(70, "/dev/root / ext4 ro,relatime 0 0\n");
    CHECK(len > CFG_RAW_CAP);
    serve("/proc/mounts", g_data, len, 64);
    CHECK(root_mounted_readonly() == 1);
    /* same layout, rw root */
    reset();
    len = build_mounts(70, "/dev/root / ext4 rw,relatime 0 0\n");
    serve("/proc/mounts", g_data, len, 64);
    CHECK(root_mounted_readonly() == 0);
    /* byte-at-a-time reads: the scanner must not depend on chunk size */
    reset();
    len = build_mounts(70, "/dev/root / ext4 ro 0 0\n");
    serve("/proc/mounts", g_data, len, 1);
    CHECK(root_mounted_readonly() == 1);
    /* last "/" line wins: ro early, rw late */
    reset();
    int k = 0;
    const char *first = "/dev/root / ext4 ro,relatime 0 0\n";
    memcpy(g_data, first, strlen(first));
    k = (int)strlen(first);
    k = mounts_filler(k, 70);
    const char *last = "/dev/root / ext4 rw,relatime 0 0\n";
    memcpy(g_data + k, last, strlen(last));
    serve("/proc/mounts", g_data, k + (int)strlen(last), 100);
    CHECK(root_mounted_readonly() == 0);
    /* last "/" line wins: rw early, ro late */
    reset();
    first = "/dev/root / ext4 rw,relatime 0 0\n";
    memcpy(g_data, first, strlen(first));
    k = (int)strlen(first);
    k = mounts_filler(k, 70);
    last = "/dev/root / ext4 ro,relatime 0 0\n";
    memcpy(g_data + k, last, strlen(last));
    serve("/proc/mounts", g_data, k + (int)strlen(last), 100);
    CHECK(root_mounted_readonly() == 1);
    /* options must START with ro: "rw,ro" is not read-only */
    reset();
    len = build_mounts(0, "/dev/root / ext4 rw,ro 0 0\n");
    serve("/proc/mounts", g_data, len, 4096);
    CHECK(root_mounted_readonly() == 0);
    /* unterminated final line still counts */
    reset();
    len = build_mounts(0, "/dev/root / ext4 ro,relatime 0 0");
    serve("/proc/mounts", g_data, len, 4096);
    CHECK(root_mounted_readonly() == 1);
    /* unreadable -> guard no-ops */
    reset();
    CHECK(root_mounted_readonly() == 0);
}

void rcl_enter(unsigned long *sp)
{
    (void)sp;
    test_file_read_boundaries();
    test_config_oversize_refused();
    test_config_normal_loads();
    test_config_newline_probe_parses();
    test_config_space_probe_refused();
    test_config_nul_line_skipped();
    test_parser_token_limits();
    test_cmdline_cut_token_dropped();
    test_cmdline_complete_flag_preserved();
    test_cmdline_normal();
    test_cmdline_maxlen_newline_probe();
    test_cmdline_cr_stop_set();
    test_mounts_streaming();
    finish(0);
}
