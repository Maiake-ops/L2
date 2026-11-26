/* Campaign 9 regression: capped live-mode tmpfs /tmp + /run.
 *
 * Ported from the maiake-f6 auditor worktree (commit 0792991, packet
 * build/campaigns/04-rcl2.md §4.6) with attribution; local additions:
 * mount-ORDER assertion and an installed-mode ro-root recovery unit test
 * (the installed branch must stay byte-identical: strings, order, remount).
 * Both modes are wired into `make -C rcl2 test` in this tree.
 *
 * Defect: on the live ramfs rootfs, /tmp is an UNCAPPED directory. Writing
 * past MemAvailable exhausts RAM, the OOM killer takes the console shell,
 * rcl2 respawns it into the same pressure, and the console wedges until
 * reboot (the pressure file can never be removed). See
 * build/campaigns/09-baseline-mempressure-16M.log and 04-rcl2.md.
 *
 * Fix (landed in src/main.c): mount tmpfs on /tmp and /run in live mode
 * too, size = clamp(RAM>>shift, floor, ceil):
 *   /tmp: 12.5% of RAM, 2..16 MiB, mode=1777
 *   /run:  6.25% of RAM, 1..8 MiB, mode=0755
 * Installed mode keeps the exact legacy mounts (no size= option).
 *
 * TWO BUILD MODES:
 *   default          exercises the helpers verbatim (copied below) plus a
 *                    replica of the mount decision; passed BEFORE the patch
 *                    landed in src/main.c (decision pin).
 *   -DTEST_REAL_SRC  includes the real src/main.c with fake syscalls and
 *                    exercises the REAL mounts_setup(). -DRCL_MAIN_C='"path"'
 *                    overrides the included main.c.
 *
 * Build (default):  make -C rcl2 build/test-tmpcap && rcl2/build/test-tmpcap
 * Build (real):     make -C rcl2 build/test-tmpcap-real && rcl2/build/test-tmpcap-real
 * syscalls.h resolves via -I src (SYS_sysinfo).
 */
#include "syscalls.h"

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
unsigned long strlen(const char *s);

static const char *test_name = "tmpcap";

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

#define CHECK(condition) do { if (!(condition)) finish(1); } while (0)

#ifdef TEST_REAL_SRC               /* real mode: main.c defines streq below */
static int streq(const char *a, const char *b);
#else
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
#endif

/* ---- mount record table (shared by both modes) ---- */
struct mount_rec { const char *dir, *fs; unsigned long flags; char data[64]; };
static struct mount_rec mounts[16];
static unsigned nmounts;

static void rec_mount(const char *dir, const char *fs, unsigned long flags,
                      const char *data)
{
    CHECK(nmounts < 16);
    struct mount_rec *m = &mounts[nmounts++];
    m->dir = dir;
    m->fs = fs;
    m->flags = flags;
    unsigned k = 0;
    if (data) while (data[k] && k < sizeof(m->data) - 1) { m->data[k] = data[k]; k++; }
    m->data[k] = 0;
}

static void check_mount(const char *dir, const char *data)
{
    unsigned long expected = streq(dir, "/run")
        ? (MS_NOSUID | MS_NODEV | MS_NOEXEC)
        : (MS_NOSUID | MS_NODEV);
    for (unsigned i = 0; i < nmounts; i++)
        if (streq(mounts[i].dir, dir)) {
            CHECK(streq(mounts[i].fs, "tmpfs"));
            CHECK(mounts[i].flags == expected);
            CHECK(streq(mounts[i].data, data));
            return;
        }
    finish(1);                       /* no mount at all for this dir */
}

/* local addition: /run must be mounted before /tmp (legacy order) */
static void check_order(const char *first, const char *second)
{
    int a = -1, b = -1;
    for (unsigned i = 0; i < nmounts; i++) {
        if (streq(mounts[i].dir, first)) a = (int)i;
        if (streq(mounts[i].dir, second)) b = (int)i;
    }
    CHECK(a >= 0 && b >= 0 && a < b);
}

#ifdef TEST_REAL_SRC
/* ------------------------------------------------------------------------
 * REAL mode: run the actual mounts_setup() from src/main.c against fake
 * syscalls. Only passes once the proposed patch has landed.
 * ------------------------------------------------------------------------ */
#ifndef RCL_MAIN_C
#define RCL_MAIN_C "../src/main.c"
#endif

static long fake_syscall(long, long, long, long, long, long, long);
#define r_sys6 fake_syscall
#define rcl_enter unused_init_entry
#include RCL_MAIN_C
#undef rcl_enter
#undef r_sys6

static unsigned long fake_totalram = 16UL << 20;
static unsigned fake_mem_unit = 1;
static int fake_sysinfo_fail;
static int fake_installed;
/* local addition: serve a ro-root /proc/mounts to unit-assert the
 * installed branch's rw-recovery remount (byte-identical requirement) */
static int fake_ro_root;
static const char fake_mounts_ro[] = "/dev/root / ext4 ro,relatime 0 0\n";
static unsigned long fake_mounts_off;

static long fake_syscall(long nr, long a, long b, long c,
                         long d, long e, long f)
{
    (void)f;
    switch (nr) {
    case SYS_mount:
        rec_mount((const char *)b, (const char *)c, (unsigned long)d,
                  (const char *)e);
        return 0;
    case SYS_sysinfo: {
        if (fake_sysinfo_fail) return -EINVAL;
        /* r_sys1 passes the pointer in the FIRST argument (a).
         * kernel struct sysinfo: totalram at offset 32, mem_unit at 100 */
        char *p = (char *)a;
        memset(p, 0, 104);
        *(unsigned long *)(p + 32) = fake_totalram;
        *(unsigned int *)(p + 100) = fake_mem_unit;
        return 0;
    }
    case SYS_open:
        if (fake_installed && streq((const char *)a, INSTALLED_MARK)) return 10;
        if (fake_ro_root && streq((const char *)a, "/proc/mounts")) {
            fake_mounts_off = 0;
            return 11;
        }
        return -ENOENT;              /* /dev/console, /proc/mounts: absent */
    case SYS_read:
        if (a == 11) {               /* fake /proc/mounts content */
            unsigned long n = sizeof(fake_mounts_ro) - 1 - fake_mounts_off;
            if (n > (unsigned long)c) n = (unsigned long)c;
            memcpy((void *)b, fake_mounts_ro + fake_mounts_off, n);
            fake_mounts_off += n;
            return (long)n;
        }
        return 0;                    /* installed mark: empty file */
    case SYS_close:
    case SYS_mkdir:
    case SYS_symlink:
        return 0;
    case SYS_write:
        return c;
    default:
        finish(2);
    }
}

static void run_live(unsigned long totalram)
{
    nmounts = 0;
    fake_installed = 0;
    fake_sysinfo_fail = 0;
    fake_mem_unit = 1;
    fake_ro_root = 0;
    fake_totalram = totalram;
    mounts_setup();
}

#else /* !TEST_REAL_SRC */
/* ------------------------------------------------------------------------
 * DEFAULT mode: the PROPOSED helpers, verbatim from the 04-rcl2.md patch,
 * plus a replica of the proposed mounts_setup() tail. When the patch lands,
 * rebuild with -DTEST_REAL_SRC to test the real thing instead.
 * ------------------------------------------------------------------------ */

/* proposed: src/main.c helpers */
static unsigned long live_scratch_cap(unsigned long total, unsigned shift,
                                      unsigned long floor, unsigned long ceil)
{
    unsigned long cap = total >> shift;
    if (cap < floor) cap = floor;
    if (cap > ceil) cap = ceil;
    return cap;
}

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

/* replica of the proposed mounts_setup() /run+/tmp tail */
static void run_live(unsigned long totalram)
{
    nmounts = 0;
    unsigned long tcap = live_scratch_cap(totalram, 3, 2UL << 20, 16UL << 20);
    unsigned long rcap = live_scratch_cap(totalram, 4, 1UL << 20, 8UL << 20);
    char opt[48];
    scratch_opt(opt, sizeof(opt), "mode=0755,size=", rcap);
    rec_mount("/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, opt);
    scratch_opt(opt, sizeof(opt), "mode=1777,size=", tcap);
    rec_mount("/tmp", "tmpfs", MS_NOSUID | MS_NODEV, opt);
}

static void run_installed(void)
{
    nmounts = 0;
    rec_mount("/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755");
    rec_mount("/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

static void run_sysinfo_fail(void)
{
    run_live(0);                     /* failed sysinfo degrades to total=0 */
}

static void helper_boundaries(void)
{
    CHECK(live_scratch_cap(0, 3, 2UL << 20, 16UL << 20) == 2UL << 20);
    CHECK(live_scratch_cap(1UL << 40, 3, 2UL << 20, 16UL << 20) == 16UL << 20);
    CHECK(live_scratch_cap(64UL << 20, 3, 2UL << 20, 16UL << 20) == 8UL << 20);
    char small[8];
    scratch_opt(small, sizeof(small), "mode=1777,size=", 16777216);
    CHECK(strlen(small) < sizeof(small));      /* bounded, NUL-terminated */
    char opt[48];
    scratch_opt(opt, sizeof(opt), "mode=1777,size=", 2097152);
    CHECK(streq(opt, "mode=1777,size=2097152"));
}
#endif /* TEST_REAL_SRC */

/* ---- scenarios shared by both modes ---- */
static void scenarios(void)
{
    /* live, 16 MiB guest: floors (2 MiB /tmp, 1 MiB /run) */
    run_live(16UL << 20);
    check_mount("/tmp", "mode=1777,size=2097152");
    check_mount("/run", "mode=0755,size=1048576");

    /* live, 32 MiB: proportional (4 MiB /tmp, 2 MiB /run) */
    run_live(32UL << 20);
    check_mount("/tmp", "mode=1777,size=4194304");
    check_mount("/run", "mode=0755,size=2097152");

    /* live, 256 MiB and 1 GiB: ceilings (16 MiB /tmp, 8 MiB /run) */
    run_live(256UL << 20);
    check_mount("/tmp", "mode=1777,size=16777216");
    check_mount("/run", "mode=0755,size=8388608");
    run_live(1UL << 30);
    check_mount("/tmp", "mode=1777,size=16777216");
    check_mount("/run", "mode=0755,size=8388608");

#ifdef TEST_REAL_SRC
    /* sysinfo failure: floor caps, still bounded */
    nmounts = 0; fake_installed = 0; fake_sysinfo_fail = 1;
    mounts_setup();
    check_mount("/tmp", "mode=1777,size=2097152");
    check_mount("/run", "mode=0755,size=1048576");
    fake_sysinfo_fail = 0;

    /* pre-2.3.23-style mem_unit scaling: pages * unit = bytes */
    nmounts = 0; fake_mem_unit = 4096; fake_totalram = (16UL << 20) / 4096;
    mounts_setup();
    check_mount("/tmp", "mode=1777,size=2097152");
    fake_mem_unit = 1;

    /* installed: legacy mounts EXACTLY, no size= option, legacy order */
    nmounts = 0; fake_installed = 1;
    mounts_setup();
    check_mount("/run", "mode=0755");
    check_mount("/tmp", "mode=1777");
    check_order("/run", "/tmp");

    /* installed + ro root: the byte-identical branch must still fire the
     * rw-recovery remount of "/" (local addition) */
    nmounts = 0; fake_ro_root = 1;
    mounts_setup();
    check_mount("/run", "mode=0755");
    check_mount("/tmp", "mode=1777");
    check_order("/run", "/tmp");
    {
        int remounted = 0;
        for (unsigned i = 0; i < nmounts; i++)
            if (streq(mounts[i].dir, "/") && mounts[i].flags == MS_REMOUNT)
                remounted = 1;
        CHECK(remounted);
    }
    fake_ro_root = 0;

    /* M04: verify hardened MS_NODEV/MS_NOEXEC flags on virtual mounts */
    {
        int seen_proc = 0, seen_sys = 0, seen_shm = 0, seen_dev = 0, seen_pts = 0;
        for (unsigned i = 0; i < nmounts; i++) {
            if (streq(mounts[i].dir, "/proc")) { CHECK(mounts[i].flags == (MS_NOSUID | MS_NODEV | MS_NOEXEC)); seen_proc = 1; }
            if (streq(mounts[i].dir, "/sys"))  { CHECK(mounts[i].flags == (MS_NOSUID | MS_NODEV | MS_NOEXEC)); seen_sys = 1; }
            if (streq(mounts[i].dir, "/dev/shm")) { CHECK(mounts[i].flags == (MS_NOSUID | MS_NODEV | MS_NOEXEC)); seen_shm = 1; }
            if (streq(mounts[i].dir, "/dev"))  { CHECK(mounts[i].flags == (MS_NOSUID | MS_NOEXEC)); seen_dev = 1; }
            if (streq(mounts[i].dir, "/dev/pts")) { CHECK(mounts[i].flags == (MS_NOSUID | MS_NOEXEC)); seen_pts = 1; }
        }
        CHECK(seen_proc && seen_sys && seen_shm && seen_dev && seen_pts);
    }

    /* M04: verify env_setup() deduplicates PATH and repeated keys (last wins) */
    {
        memset(&g_conf, 0, sizeof(g_conf));
        static const char *e0 = "PATH=/custom/bin";
        static const char *e1 = "FOO=1";
        static const char *e2 = "FOO=2";
        memcpy(g_conf.env[0], e0, strlen(e0) + 1);
        memcpy(g_conf.env[1], e1, strlen(e1) + 1);
        memcpy(g_conf.env[2], e2, strlen(e2) + 1);
        g_conf.nenv = 3;
        env_setup();
        int path_cnt = 0, foo_cnt = 0, path_ok = 0, foo_ok = 0;
        for (unsigned i = 0; g_svc_env[i]; i++) {
            if (env_same_key(g_svc_env[i], "PATH=")) { path_cnt++; path_ok = streq(g_svc_env[i], e0); }
            if (env_same_key(g_svc_env[i], "FOO="))  { foo_cnt++;  foo_ok = streq(g_svc_env[i], e2); }
        }
        CHECK(path_cnt == 1 && path_ok);
        CHECK(foo_cnt == 1 && foo_ok);
    }
#else
    run_sysinfo_fail();
    check_mount("/tmp", "mode=1777,size=2097152");
    check_mount("/run", "mode=0755,size=1048576");

    run_installed();
    check_mount("/run", "mode=0755");
    check_mount("/tmp", "mode=1777");
    check_order("/run", "/tmp");

    helper_boundaries();
#endif
}

void rcl_enter(unsigned long *sp)
{
    (void)sp;
    scenarios();
    finish(0);
}
