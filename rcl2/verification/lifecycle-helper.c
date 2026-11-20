/* Freestanding fault-injection service for tools/test_rcl2_lifecycle.py.
 * Raw syscalls only, no libc - runs inside the harness's private chroot.
 *
 *   helper exit N      exit immediately with code N (crash-loop driver)
 *   helper sleep MS    sleep MS milliseconds, then exit 0 (long-lived svc)
 *   helper burst M     fork M children that exit at once, then exit 0
 *                      (zombie burst: children reparent to PID1 = rcl2)
 *   helper stubborn    ignore SIGTERM and sleep forever (KILL-escalation)
 *   helper ok          exit 0
 *
 * Built by the harness: clang -c -ffreestanding ... + ld.lld -static.
 */

static long sys6(long nr, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}
#define S0(n)       sys6((n),0,0,0,0,0,0)
#define S1(n,a)     sys6((n),(long)(a),0,0,0,0,0)

struct kts { long tv_sec; long tv_nsec; };

static void msleep(long ms)
{
    struct kts req, rem;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    while (sys6(35L, (long)&req, (long)&rem, 0, 0, 0, 0) == -4L)
        req = rem;                              /* EINTR: sleep the remainder */
}

static long parse_num(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void helper_main(unsigned long *sp)
{
    unsigned long argc = sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode = (argc > 1) ? argv[1] : "ok";
    long n = (argc > 2) ? parse_num(argv[2]) : 0;

    if (streq(mode, "exit")) S1(60L, n);
    if (streq(mode, "sleep")) { msleep(n); S1(60L, 0); }
    if (streq(mode, "burst")) {
        for (long i = 0; i < n; i++) {
            long pid = S0(57L);                 /* fork */
            if (pid == 0) S1(60L, 0);           /* child: exit at once */
        }
        S1(60L, 0);
    }
    if (streq(mode, "stubborn")) {
        /* SIG_IGN for SIGTERM: no sigframe is ever delivered, so no
         * SA_RESTORER trampoline is needed */
        struct { unsigned long handler, flags, restorer, mask; } sa;
        sa.handler = 1UL;                       /* SIG_IGN */
        sa.flags = 0; sa.restorer = 0; sa.mask = 0;
        sys6(13L, 15L, (long)&sa, 0, 8L, 0, 0); /* rt_sigaction(SIGTERM) */
        for (;;) msleep(1000);
    }
    S1(60L, 0);
}

__asm__(
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "    and $-16, %rsp\n"
    "    mov %rsp, %rdi\n"
    "    call helper_main\n"
    "    mov $60, %eax\n"
    "    xor %edi, %edi\n"
    "    syscall\n"
    "    ud2\n"
    ".section .note.GNU-stack,\"\",@progbits\n"
);
