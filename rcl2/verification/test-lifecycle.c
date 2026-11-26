/* Lifecycle regression: burst zombie reaping, once/wait semantics, shutdown
 * TERM->KILL escalation with respawn suppression, and exit-status logging.
 * Fake clock, fork queue, wait queue and kill log - no real children,
 * signals, mounts or reboot syscalls are permitted. Complements
 * test-supervision.c (backoff timing) without duplicating it. */
#include "../src/syscalls.h"

void *memcpy(void *dst, const void *src, unsigned long n);   /* src/mem.c */
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

/* ------------------------------------------------------------- fake state */

static unsigned long clock_ms = 1000;
static long fork_queue[8];
static int fork_n, fork_i;
static int fork_count;                     /* children the kernel would hold */
static struct { long pid; int st; } exit_queue[16];
static int exit_n, exit_i;
static int interrupt_wait;
static int eintr_budget;
static struct { long pid; int sig; } kill_log[32];
static int kill_n;
static unsigned reboot_cmds[4];
static int reboot_n;
static long reboot_fail_first;
static int reboot_done;
static unsigned long mount_flags;
static long mount_rc;
static int umount_n;
static int umount_detach_n;
static int sync_n;
static int sync_after_remount;
static int remount_seen;
static int kill_wait_eintr;
static int next_fcntl_fd;
static int fake_pipe_fds[2];
static int closed_fds[16];
static int closed_n;
static int dup2_targets[16];
static int dup2_n;
static int ppoll_n;
static long ppoll_release_pid;
static int ppoll_release_st;
static long fake_pid = 1;
static int panic_check_mode;   /* 1 = non-pid1 exit, 2 = pid1 idle+reap */
static int rescue_check_mode;  /* 1 = rescue_shell reap + shutdown */
static char log_buf[8192];
static unsigned log_len;

static void fake_child(long pid) { (void)pid; fork_count++; }

static void queue_exit(long pid, int st)
{
    CHECK(exit_n < 16);
    exit_queue[exit_n].pid = pid;
    exit_queue[exit_n].st = st;
    exit_n++;
}

static long pop_exit(void)
{
    CHECK(exit_i < exit_n);
    long pid = exit_queue[exit_i].pid;
    exit_i++;
    fork_count--;
    return pid;
}

static int log_contains(const char *needle)
{
    for (unsigned i = 0; i + 1 < log_len; i++) {
        unsigned j = 0;
        while (needle[j] && i + j < log_len && log_buf[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int kill_index(long pid, int sig)
{
    for (int i = 0; i < kill_n; i++)
        if (kill_log[i].pid == pid && kill_log[i].sig == sig) return i;
    return -1;
}

static void reset(void)
{
    memset(&g_conf, 0, sizeof(g_conf));
    svc_state_clear();
    fork_n = fork_i = fork_count = 0;
    exit_n = exit_i = 0;
    interrupt_wait = 0;
    eintr_budget = 1000;
    kill_n = 0;
    reboot_done = 0;
    reboot_n = 0;
    reboot_fail_first = 0;
    mount_flags = 0;
    mount_rc = 0;
    umount_n = 0;
    umount_detach_n = 0;
    sync_n = 0;
    sync_after_remount = 0;
    remount_seen = 0;
    kill_wait_eintr = 0;
    next_fcntl_fd = 3;
    fake_pipe_fds[0] = 0;
    fake_pipe_fds[1] = 1;
    closed_n = 0;
    dup2_n = 0;
    ppoll_n = 0;
    ppoll_release_pid = 0;
    ppoll_release_st = 0;
    fake_pid = 1;
    panic_check_mode = 0;
    rescue_check_mode = 0;
    log_len = 0;
    clock_ms = 1000;
    g_closing = 0;
    g_flags = 0;
    g_sig_rd = -1;
    g_sig_wr = -1;
}

static void name_svc(unsigned i, const char *name)
{
    for (unsigned k = 0; k + 1 < RCL_NAME_LEN && name[k]; k++)
        g_conf.svcs[i].name[k] = name[k];
}

static long fake_syscall(long nr, long a, long b, long c, long d, long e, long f)
{
    (void)e; (void)f;
    switch (nr) {
    case SYS_clock_gettime: {
        struct kts *t = (struct kts *)b;
        t->tv_sec = (long)(clock_ms / 1000);
        t->tv_nsec = (long)(clock_ms % 1000) * 1000000L;
        return 0;
    }
    case SYS_nanosleep: {
        if (panic_check_mode == 2) {
            CHECK(log_contains("rcl2 PANIC: boom1"));
            CHECK(log_contains("pid1 will not exit, idling"));
            CHECK(fork_count == 0);
            finish(0);
        }
        if (reboot_done) {
            if (rescue_check_mode) {
                CHECK(log_contains("reap 888 exit 0"));
                CHECK(log_contains("shutting down: poweroff request"));
                CHECK(ppoll_n >= 1);
                finish(0);
            }
            /* shutdown_run idles after reboot: final audit */
            CHECK(reboot_n == 2);
            CHECK(reboot_cmds[0] == RB_POWER_OFF);
            CHECK(reboot_cmds[1] == RB_HALT);
            CHECK(mount_flags == (MS_REMOUNT | MS_RDONLY));
            CHECK(umount_n >= 6);
            CHECK(umount_detach_n >= 1);
            CHECK(sync_after_remount >= 1);
            CHECK(log_contains("remount / ro failed 16"));
            int t1 = kill_index(301, SIGTERM), t2 = kill_index(302, SIGTERM);
            int ta = kill_index(-1, SIGTERM), tk = kill_index(-1, SIGKILL);
            CHECK(t1 >= 0 && t2 >= 0 && ta > t1 && ta > t2 && tk > ta);
            CHECK(g_next[0] == 0 && g_next[1] == 0);   /* no respawn while closing */
            CHECK(log_contains("shutting down: poweroff request"));
            /* the KILL grace sweep reaps silently (raw wait4, by design):
             * assert the children are gone, not a log line */
            CHECK(fork_count == 0);
            finish(0);
        }
        struct kts *req = (struct kts *)a;
        clock_ms += (unsigned long)req->tv_sec * 1000UL +
                    (unsigned long)(req->tv_nsec / 1000000L);
        return 0;
    }
    case SYS_fork:
        CHECK(fork_i < fork_n);
        fork_count++;
        return fork_queue[fork_i++];
    case SYS_wait4: {
        if (c & WNOHANG) {
            if (kill_wait_eintr > 0) {
                kill_wait_eintr--;
                return -EINTR;
            }
            if (exit_i < exit_n && (a == -1 || exit_queue[exit_i].pid == a)) {
                long pid = pop_exit();
                if (b) *(int *)b = exit_queue[exit_i - 1].st;
                return pid;
            }
            return fork_count > 0 ? 0 : -ECHILD;
        }
        if (interrupt_wait) { interrupt_wait = 0; return -EINTR; }
        if (exit_i < exit_n && (a == -1 || exit_queue[exit_i].pid == a)) {
            long pid = pop_exit();
            if (b) *(int *)b = exit_queue[exit_i - 1].st;
            return pid;
        }
        if (fork_count == 0) return -ECHILD;
        CHECK(--eintr_budget > 0);
        return -EINTR;
    }
    case SYS_ppoll: {
        struct pollfdx *pf = (struct pollfdx *)a;
        ppoll_n++;
        if (rescue_check_mode) {
            __atomic_or_fetch(&g_flags, SIGBIT(SIGTERM), __ATOMIC_RELAXED);
        }
        if (ppoll_release_pid > 0) {
            queue_exit(ppoll_release_pid, ppoll_release_st);
            ppoll_release_pid = 0;
        }
        if (pf && pf->fd >= 0) pf->revents = POLLIN;
        return 1;
    }
    case SYS_read:
        return -EAGAIN;
    case SYS_getpid:
        return fake_pid;
    case SYS_exit_group:
        if (panic_check_mode == 1) {
            CHECK(a == 1);
            CHECK(log_contains("rcl2 PANIC: boom"));
            CHECK(!log_contains("pid1 will not exit"));
            finish(0);
        }
        finish((int)a);
    case SYS_kill:
        CHECK(kill_n < 32);
        kill_log[kill_n].pid = a;
        kill_log[kill_n].sig = (int)b;
        kill_n++;
        if (a == -1 && b == SIGKILL) {
            /* the KILL sweep lands: every supervised child dies */
            for (unsigned i = 0; i < g_conf.nsvc; i++)
                if (g_pid[i] > 0) queue_exit(g_pid[i], SIGKILL);
        }
        return 0;
    case SYS_reboot:
        CHECK(reboot_n < 4);
        reboot_cmds[reboot_n++] = (unsigned)c;
        if (reboot_n == 1 && reboot_fail_first != 0)
            return reboot_fail_first;
        reboot_done = 1;
        return 0;
    case SYS_sync:
        sync_n++;
        if (remount_seen) sync_after_remount++;
        return 0;
    case SYS_umount2:
        umount_n++;
        if (b == 0 && umount_n == 1) return -EBUSY;
        if (b == MNT_DETACH) umount_detach_n++;
        return 0;
    case SYS_mount:
        mount_flags = (unsigned long)d;
        remount_seen = 1;
        return mount_rc;
    case SYS_pipe2: {
        int *fds = (int *)a;
        fds[0] = fake_pipe_fds[0];
        fds[1] = fake_pipe_fds[1];
        return 0;
    }
    case SYS_fcntl:
        CHECK(b == F_DUPFD_CLOEXEC && c == 3L);
        return next_fcntl_fd++;
    case SYS_open:
        return 0;
    case SYS_dup2:
        if (dup2_n < 16) dup2_targets[dup2_n++] = (int)b;
        return b;
    case SYS_close:
        if (closed_n < 16) closed_fds[closed_n++] = (int)a;
        return 0;
    case SYS_rt_sigaction:
        return 0;
    case SYS_write: {
        unsigned n = (unsigned)c;
        if (n > sizeof(log_buf) - log_len) n = sizeof(log_buf) - log_len;
        memcpy(log_buf + log_len, (const void *)b, n);
        log_len += n;
        return c;
    }
    default: finish(2);
    }
    return 0;
}

/* ------------------------------------------------------------------ tests */

/* burst exits: several services plus unknown pids in one reap sweep */
static void test_burst_reaping(void)
{
    reset();
    g_conf.nsvc = 3;
    for (unsigned i = 0; i < 3; i++) {
        g_conf.svcs[i].mode = RCL_MODE_RESPAWN;
        name_svc(i, i == 0 ? "s0" : i == 1 ? "s1" : "s2");
    }
    fork_queue[0] = 101; fork_queue[1] = 102; fork_queue[2] = 103;
    fork_n = 3;
    svc_spawn(0); svc_spawn(1); svc_spawn(2);
    fake_child(999);                       /* reparented zombie, not a service */
    fake_child(401);
    queue_exit(101, 0);                    /* exit 0 */
    queue_exit(999, 11 /* SIGSEGV */);     /* unknown pid, killed */
    queue_exit(102, 3 << 8);               /* exit 3 */
    queue_exit(401, 0x137f);               /* stopped (WIFSTOPPED) */
    queue_exit(103, SIGTERM);              /* signaled */
    reap_children();
    CHECK(g_pid[0] == 0 && g_pid[1] == 0 && g_pid[2] == 0);
    CHECK(g_next[0] > 0 && g_next[1] > 0 && g_next[2] > 0);  /* respawn queued */
    CHECK(fork_count == 0);                /* nothing left to reap */
    CHECK(reap_children() == 1);           /* now -ECHILD */
    CHECK(log_contains("reap 999 signal 11"));
    CHECK(log_contains("reap 401 stop"));
    CHECK(log_contains("exit 3"));
    CHECK(log_contains("signal 15"));
}

/* once never respawns; wait blocks at boot and survives an EINTR sweep */
static void test_once_wait(void)
{
    reset();
    g_conf.nsvc = 1;
    name_svc(0, "o");
    g_conf.svcs[0].mode = RCL_MODE_ONCE;
    fork_queue[0] = 201; fork_n = 1;
    svc_spawn(0);
    queue_exit(201, 0);
    reap_children();
    CHECK(g_pid[0] == 0 && g_next[0] == 0);
    CHECK(log_contains("o done (exit 0)"));

    g_conf.svcs[0].mode = RCL_MODE_WAIT;
    name_svc(0, "w");
    fork_queue[0] = 202; fork_i = 0; fork_n = 1;
    svc_spawn(0);
    queue_exit(202, 0);
    svc_wait_boot(0);
    CHECK(g_pid[0] == 0 && g_next[0] == 0);
    CHECK(log_contains("w finished (exit 0)"));

    /* ppoll + reap during wait: orphan zombie 998 is reaped while waiting,
     * then ppoll wakes when wait child 203 exits */
    g_sig_rd = 3;
    fork_queue[0] = 203; fork_i = 0;
    svc_spawn(0);
    fake_child(998);
    queue_exit(998, 0);
    ppoll_release_pid = 203;
    ppoll_release_st = 5 << 8;
    svc_wait_boot(0);
    CHECK(ppoll_n >= 1);
    CHECK(g_pid[0] == 0 && g_next[0] == 0);
    CHECK(fork_count == 0);
    CHECK(log_contains("reap 998 exit 0"));
    CHECK(log_contains("w finished (exit 5)"));
}

/* self-pipe must be moved above FDs 0/1/2 so console_open() never clobbers it */
static void test_selfpipe_no_stdio_clobber(void)
{
    reset();
    fake_pipe_fds[0] = 0;
    fake_pipe_fds[1] = 1;
    signals_setup();
    CHECK(g_sig_rd == 3 && g_sig_wr == 4);
    CHECK(closed_n == 2 && closed_fds[0] == 0 && closed_fds[1] == 1);

    console_open();
    CHECK(g_sig_rd == 3 && g_sig_wr == 4);
    for (int i = 0; i < dup2_n; i++)
        CHECK(dup2_targets[i] != g_sig_rd && dup2_targets[i] != g_sig_wr);
}

/* shutdown: TERM each service, TERM sweep, grace, KILL sweep (surviving EINTR),
 * umount2, ro remount (logged on failure) + post-remount sync, RB_POWER_OFF ->
 * RB_HALT degradation; respawn stays suppressed throughout. */
static void test_shutdown_escalation(void)
{
    reset();
    g_conf.nsvc = 2;
    for (unsigned i = 0; i < 2; i++) {
        g_conf.svcs[i].mode = RCL_MODE_RESPAWN;
        name_svc(i, i == 0 ? "s0" : "s1");
    }
    fork_queue[0] = 301; fork_queue[1] = 302; fork_n = 2;
    svc_spawn(0); svc_spawn(1);
    kill_wait_eintr = 2;
    mount_rc = -EBUSY;
    reboot_fail_first = -EINVAL;
    /* services ignore TERM: nothing exits until the KILL sweep */
    shutdown_run("poweroff request", RB_POWER_OFF);
    finish(3);                             /* unreachable: fake finishes first */
}

void rcl_enter(unsigned long *sp)
{
    unsigned long argc = sp ? sp[0] : 0;
    char **argv = sp ? (char **)(sp + 1) : 0;
    const char *a1 = (argc > 1 && argv) ? argv[1] : 0;

    if (a1 && streq(a1, "--panic-nonpid1")) {
        reset();
        fake_pid = 42;
        panic_check_mode = 1;
        rcl_c_panic("boom", 4);
    }
    if (a1 && streq(a1, "--panic-pid1")) {
        reset();
        fake_pid = 1;
        panic_check_mode = 2;
        fake_child(777);
        queue_exit(777, 0);
        rcl_c_panic("boom1", 5);
    }
    if (a1 && streq(a1, "--rescue-reap")) {
        reset();
        g_sig_rd = 3;
        rescue_check_mode = 1;
        fork_queue[0] = 501; fork_n = 1;
        fake_child(888);
        queue_exit(888, 0);
        rescue_shell();
    }

    test_selfpipe_no_stdio_clobber();
    test_burst_reaping();
    test_once_wait();
    test_shutdown_escalation();
    finish(0);
}
