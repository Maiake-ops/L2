/* rcl2 - raw Linux x86_64 syscall interface, dependency-free.
 *
 * rcl2 is PID 1: it never links libc, it talks to the kernel directly.
 * All numbers below are the stable x86_64 syscall table entries.
 */
#ifndef RCL2_SYSCALLS_H
#define RCL2_SYSCALLS_H

/* syscall numbers (x86_64) */
#define SYS_read          0L
#define SYS_write         1L
#define SYS_open          2L
#define SYS_close         3L
#define SYS_ioctl        16L
#define SYS_dup          32L
#define SYS_dup2         33L
#define SYS_nanosleep    35L
#define SYS_getpid       39L
#define SYS_fork         57L
#define SYS_execve       59L
#define SYS_exit         60L
#define SYS_wait4        61L
#define SYS_kill         62L
#define SYS_chdir        80L
#define SYS_mkdir        83L
#define SYS_symlink      88L
#define SYS_syslog      103L
#define SYS_setsid      112L
#define SYS_sync        162L
#define SYS_mount       165L
#define SYS_umount2     166L
#define SYS_reboot      169L
#define SYS_sethostname 170L
#define SYS_rt_sigreturn 15L
#define SYS_rt_sigaction 13L
#define SYS_rt_sigprocmask 14L
#define SYS_clock_gettime 228L
#define SYS_ppoll       271L
#define SYS_exit_group  231L
#define SYS_pipe2       293L
#define SYS_sysinfo     99L

static inline long r_sys6(long nr, long a, long b, long c, long d, long e, long f)
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
#define r_sys0(n)                    r_sys6(n,0,0,0,0,0,0)
#define r_sys1(n,a)                  r_sys6(n,(long)(a),0,0,0,0,0)
#define r_sys2(n,a,b)                r_sys6(n,(long)(a),(long)(b),0,0,0,0)
#define r_sys3(n,a,b,c)              r_sys6(n,(long)(a),(long)(b),(long)(c),0,0,0)
#define r_sys4(n,a,b,c,d)            r_sys6(n,(long)(a),(long)(b),(long)(c),(long)(d),0,0)
#define r_sys5(n,a,b,c,d,e)          r_sys6(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),0)

#define err_x(r)   (-(long)(r))       /* kernel returns -errno */

/* errno values we actually branch on */
#define EINTR   4
#define EAGAIN 11
#define ENOENT  2
#define EACCES 13
#define EEXIST 17
#define EBUSY  16
#define EINVAL 22
#define ECHILD 10
#define EFBIG  27

/* open(2) flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CLOEXEC 0x80000
#define O_NONBLOCK 0x800

/* mount(2) flags */
#define MS_RDONLY    1UL
#define MS_NOSUID    2UL
#define MS_NODEV     4UL
#define MS_NOEXEC    8UL
#define MS_REMOUNT  32UL

/* umount2 flags */
#define MNT_DETACH 2

/* poll events */
#define POLLIN 0x1

/* ptrace-free minimal termios (kernel termbits layout, asm-generic) */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCSCTTY 0x540E
#define TIOCSWINSZ 0x5414
#define TIOCGWINSZ 0x5413

#define ICRNL 0x100
#define IXON  0x400
#define BRKINT 0x2
#define OPOST 0x1
#define ONLCR 0x4
#define ISIG  0x1
#define ICANON 0x2
#define ECHO  0x8
#define ECHOE 0x10
#define ECHOK 0x20
#define IEXTEN 0x8000

/* signals */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGKILL   9
#define SIGTERM  15
#define SIGUSR1  10
#define SIGUSR2  12
#define SIGCHLD  17
#define SIGWINCH 28
#define SIGPWR   30

#define SA_NOCLDSTOP 1UL
#define SA_RESTORER  0x04000000UL
#define SA_RESTART   0x10000000UL
#define SIG_BLOCK 0
#define SIG_SETMASK 2

/* reboot(2) */
#define RB_RESTART   0x01234567UL
#define RB_HALT      0xCDEF0123UL
#define RB_POWER_OFF 0x4321FEDCUL
#define REBOOT_MAGIC1 0xFEE1DEADUL
#define REBOOT_MAGIC2 672274793UL  /* 0x28121969 */

/* wait4 options */
#define WNOHANG 1
/* wait status decoding */
#define w_exited(st)   (((st) & 0x7F) == 0)
#define w_exitcode(st) (((st) >> 8) & 0xFF)
#define w_signaled(st) (((st) & 0x7F) != 0 && ((st) & 0x7F) != 0x7F)
#define w_sig(st)      ((st) & 0x7F)

#endif /* RCL2_SYSCALLS_H */
