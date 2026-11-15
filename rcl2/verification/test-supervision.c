/* Exercise the actual supervisor with a fake clock, fork, and wait queue.
 * No real children, signals, mounts, or shutdown syscalls are permitted. */
#include "../src/syscalls.h"

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

static unsigned long clock_ms = 1000;
static long fork_result = 42;
static long exited_pid;
static int interrupt_wait;
static unsigned polls;

#define CHECK(condition) do { if (!(condition)) finish(1); } while (0)

static long fake_syscall(long nr, long a, long b, long c,
                         long d, long e, long f)
{
    (void)a; (void)d; (void)e; (void)f;
    switch (nr) {
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
            return 0;
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
