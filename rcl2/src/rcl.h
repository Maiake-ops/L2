/* rcl2 - shared ABI between the C core and the Rust core.
 *
 * The Rust #![no_std] core (src/rs/rcl.rs) is embedded as a static library
 * and exposes six plain `extern "C"` functions.  Everything crossing the
 * boundary lives in fixed-size #[repr(C)] byte arrays: no pointers, no
 * heap, nothing Rust-specific, so the layout can be pinned with
 * _Static_assert on the C side and const asserts on the Rust side.
 */
#ifndef RCL2_RCL_H
#define RCL2_RCL_H

#define RCL_MAX_SVCS   16
#define RCL_MAX_ARGS    6
#define RCL_MAX_ENV     8
#define RCL_NAME_LEN   24
#define RCL_PATH_LEN   96
#define RCL_ARG_LEN    64
#define RCL_ENV_LEN    64
#define RCL_HOST_LEN   64

/* service modes */
#define RCL_MODE_ONCE    0
#define RCL_MODE_RESPAWN 1
#define RCL_MODE_WAIT    2

/* ctrl-alt-del behaviour */
#define RCL_CAD_REBOOT   0
#define RCL_CAD_POWEROFF 1

struct rcl_svc {
    char name[RCL_NAME_LEN];               /* NUL-terminated label          */
    char path[RCL_PATH_LEN];               /* exec path                     */
    char args[RCL_MAX_ARGS][RCL_ARG_LEN];  /* argv[1..], NUL-terminated     */
    unsigned char nargs;                   /* number of populated args      */
    unsigned char mode;                    /* RCL_MODE_*, above             */
    unsigned char flags;                   /* reserved, must stay 0         */
};
struct rcl_conf {
    unsigned char nsvc;
    unsigned char nenv;
    unsigned char klog;                    /* console loglevel, 0 = default */
    unsigned char cad;                     /* RCL_CAD_*                     */
    char hostname[RCL_HOST_LEN];
    char env[RCL_MAX_ENV][RCL_ENV_LEN];
    struct rcl_svc svcs[RCL_MAX_SVCS];
};

_Static_assert(sizeof(struct rcl_svc) ==
    RCL_NAME_LEN + RCL_PATH_LEN + RCL_MAX_ARGS * RCL_ARG_LEN + 3,
    "rcl_svc layout is mirrored in src/rs/rcl.rs");
_Static_assert(sizeof(struct rcl_conf) ==
    4 + RCL_HOST_LEN + RCL_MAX_ENV * RCL_ENV_LEN +
    RCL_MAX_SVCS * sizeof(struct rcl_svc),
    "rcl_conf layout is mirrored in src/rs/rcl.rs");
/* offsets matter as much as sizes: Rust writes the same fields by index */
_Static_assert(__builtin_offsetof(struct rcl_svc, nargs) == 504, "svc ABI drift");
_Static_assert(__builtin_offsetof(struct rcl_conf, hostname) == 4,   "conf ABI drift");
_Static_assert(__builtin_offsetof(struct rcl_conf, env) == 68,       "conf ABI drift");
_Static_assert(__builtin_offsetof(struct rcl_conf, svcs) == 580,     "conf ABI drift");

/* entry points implemented by src/rs/rcl.rs */
long           rcl_rs_parse(struct rcl_conf *conf, const void *buf, unsigned long len);
unsigned long  rcl_rs_bootflags(const void *buf, unsigned long len);
unsigned long  rcl_rs_statm(const void *buf, unsigned long len);     /* resident pages */
unsigned long  rcl_rs_banner(char *out, unsigned long cap);
const char    *rcl_rs_version(void);

/* C-side panic sink for the Rust core (named in the #[panic_handler]) */
void rcl_c_panic(const void *msg, unsigned long len);

#endif /* RCL2_RCL_H */
