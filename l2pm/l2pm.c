/* l2pm - L2 package manager.
 *
 * Design goals (the whole point of this file):
 *   - no libc dependencies at runtime beyond what the static binary carries
 *   - no tar(1), no sha256sum(1), no gzip(1) needed to install a package
 *   - immutable, content-addressed store: /l2/store/<sha256>-<name>-<version>
 *   - reproducible package archives: sorted entries, uid/gid 0, fixed mtime
 *   - resident memory measured in single-digit MiB (usually < 2 MiB)
 *
 * Package format: a plain ustar archive (.l2p) produced by this program.
 * The archive embeds .l2pm/info (name, version, epoch, deps).  Installing
 * reads that file, verifies paths, extracts with the built-in tar reader and
 * activates a profile by building a tree of symlinks.  Nothing is ever
 * overwritten in the store; removing a package only drops the DB entry and
 * rebuilds the profile.  `gc` deletes unreferenced store paths.
 *
 * Recipes are tiny key=value files executed with /bin/sh.  They are used on
 * build hosts only; an installed L2 can install .l2p files offline.
 *
 * Environment:
 *   L2PM_ROOT       prefix for tests, e.g. L2PM_ROOT=/tmp/l2-test
 *   L2PM_RECIPES    recipe directory (default <root>/l2/recipes)
 *   L2PM_RECIPE_REPO      remote recipe repository: a git URL (cloned into
 *                   <root>/l2/var/recipe-repo) or a raw base URL serving
 *                   <base>/<name>.recipe.  Local recipes take precedence.
 *   L2PM_RECIPE_REPO_REF  commit hash pinning the git recipe repository
 *   SOURCE_DATE_EPOCH   fixed timestamp for reproducible archives
 *   JOBS            build parallelism (default 8)
 *
 * Recipes run in four steps: 1 repo (git source clone/checkout), 2 fetch +
 * extract, 3 compile, 4 install into $DESTDIR.  Legacy recipes with a single
 * build= command keep working (build = compile+install combined).  Remote
 * recipes execute shell code: they are shown and must be explicitly trusted
 * (confirmation or --trust); trusted content hashes are remembered under
 * <root>/l2/var/trusted-recipes.
 *
 * Transactions: one add/install/remove command is one transaction.  If any
 * step fails, the database and the active profile generation are restored
 * to their pre-command state (see the failure contract at struct txn).
 * `verify --repair` reconciles torn DB/profile states left by a crash.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define L2PM_VERSION "0.1.0"
#define MAXPATH 4096
#define MAXREL  4096
#define DEFAULT_EPOCH 315532800LL /* 1980-01-01T00:00:00Z, ZIP-safe */

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...)
{
    va_list ap;
    fputs("l2pm: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *fmt(const char *f, ...)
{
    va_list ap;
    int n;
    va_start(ap, f);
    n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    if (n < 0) die("format error");
    char *p = xmalloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(p, (size_t)n + 1, f, ap);
    va_end(ap);
    return p;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static const char *root_prefix(void)
{
    const char *r = getenv("L2PM_ROOT");
    return (r && *r) ? r : "";
}

/* path under <root>/l2/... */
static char *l2path(const char *fmt_, ...)
{
    va_list ap;
    char tail[MAXPATH];
    va_start(ap, fmt_);
    vsnprintf(tail, sizeof(tail), fmt_, ap);
    va_end(ap);
    return fmt("%s/l2/%s", root_prefix(), tail);
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[MAXPATH];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    if (tmp[n - 1] == '/') tmp[n - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Global state lock on <root>/l2/var/.lock so concurrent l2pm invocations
 * serialize DB, profile generation, rollback, repair, and GC mutations.
 * O_CLOEXEC prevents child build/fetch processes from holding the lock. */
static int l2pm_lock(int exclusive)
{
    char *vdir = l2path("var");
    if (exclusive) {
        if (mkdir_p(vdir, 0755) != 0) {
            fprintf(stderr, "l2pm: cannot create %s: %s\n", vdir, strerror(errno));
            free(vdir);
            return -1;
        }
    }
    free(vdir);
    char *lpath = l2path("var/.lock");
    int flags = exclusive ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
    int fd = open(lpath, flags, 0644);
    if (fd < 0) {
        if (!exclusive && (errno == ENOENT || errno == EACCES || errno == EROFS)) {
            free(lpath);
            return -2; /* read-only/uninitialized root: proceed unlocked */
        }
        fprintf(stderr, "l2pm: cannot open lock %s: %s\n", lpath, strerror(errno));
        free(lpath);
        return -1;
    }
    free(lpath);
    int op = exclusive ? LOCK_EX : LOCK_SH;
    while (flock(fd, op) != 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "l2pm: cannot acquire lock: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void l2pm_unlock(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

static int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        /* Read-only directories (packages may ship mode 0555 dirs) must
         * still be removable by their owner during gc/failed installs. */
        chmod(path, 0700);
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char *c = fmt("%s/%s", path, e->d_name);
                rm_rf(c);
                free(c);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

static int write_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

/* Durability helpers: rename is atomic but not durable until the file
 * contents and the containing directory entry are on disk.  Every
 * temp+rename mutation fsyncs the temp before the rename and the directory
 * after it, so a power loss cannot tear a DB entry or a profile switch. */
static int fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0 && rc == 0) rc = -1;
    return rc;
}

static int write_file(const char *path, const void *buf, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int rc = write_all(fd, buf, n);
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0 && rc == 0) rc = -1;
    return rc;
}

static int write_file_atomic(const char *path, const void *buf, size_t n)
{
    const char *sl = strrchr(path, '/');
    char *tmp;
    if (!sl) tmp = fmt(".%s.tmp-%ld", path, (long)getpid());
    else if (sl == path) tmp = fmt("/.%s.tmp-%ld", sl + 1, (long)getpid());
    else tmp = fmt("%.*s/.%s.tmp-%ld", (int)(sl - path), path, sl + 1, (long)getpid());
    if (write_file(tmp, buf, n) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    char *dir;
    if (!sl) dir = xstrdup(".");
    else if (sl == path) dir = xstrdup("/");
    else dir = fmt("%.*s", (int)(sl - path), path);
    int rc = fsync_dir(dir);
    free(dir);
    return rc;
}

/* Fsync every directory of a staged/extracted tree (file contents are
 * fsynced at write time), so the whole tree is durable before its rename
 * into the store or a generation directory. */
static int fsync_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *p = fmt("%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode) && fsync_tree(p) != 0) rc = -1;
        free(p);
    }
    closedir(d);
    if (fsync_dir(dir) != 0) rc = -1;
    return rc;
}

/* Upper bound for metadata files (db entries, .l2pm/info, /proc files).
 * Keeps a hostile or corrupt file from exhausting memory. */
#define READ_SMALL_MAX (1024 * 1024)

static char *read_small(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            buf = xrealloc(buf, cap);
        }
        size_t r = fread(buf + len, 1, 4096, f);
        len += r;
        if (len > READ_SMALL_MAX) { fclose(f); free(buf); return NULL; }
        if (r < 4096) break;
    }
    fclose(f);
    buf[len] = 0;
    return buf;
}

static int valid_name(const char *s)
{
    if (!s || !*s || s[0] == '.') return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && !strchr("._+-", *s)) return 0;
    return 1;
}

static char *shq(const char *s)
{
    size_t n = 2;
    for (const char *p = s; *p; p++) n += (*p == '\'') ? 4 : 1;
    char *o = xmalloc(n + 1), *q = o;
    *q++ = '\'';
    for (const char *p = s; *p; p++) {
        if (*p == '\'') { memcpy(q, "'\\''", 4); q += 4; }
        else *q++ = *p;
    }
    *q++ = '\'';
    *q = 0;
    return o;
}

/* --------------------------------------------------------------- sha256 */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t n;
} sha256_ctx;

static uint32_t ror32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const unsigned char *p = data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->n;
        if (take > n) take = n;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        n -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    uint64_t bits = c->len * 8;
    unsigned char pad[72];
    size_t padn = (c->n < 56) ? 56 - c->n : 120 - c->n;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padn + i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(c, pad, padn + 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static void hex32(const unsigned char h[32], char out[65])
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2] = d[h[i] >> 4];
        out[i*2+1] = d[h[i] & 15];
    }
    out[64] = 0;
}

static int hash_file(const char *path, char out[65])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    sha256_ctx c;
    sha256_init(&c);
    unsigned char buf[65536];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (r == 0) break;
        sha256_update(&c, buf, (size_t)r);
    }
    close(fd);
    unsigned char h[32];
    sha256_final(&c, h);
    hex32(h, out);
    return 0;
}

/* -------------------------------------------------------------- tar out */

struct pathlist { char **v; size_t n, cap; };

static void pl_free(struct pathlist *l)
{
    for (size_t i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static int pl_has(const struct pathlist *l, const char *s)
{
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->v[i], s)) return 1;
    return 0;
}

static void pl_add(struct pathlist *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, l->cap * sizeof(*l->v));
    }
    l->v[l->n++] = xstrdup(s);
}

static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void walk(const char *base, const char *rel, struct pathlist *l)
{
    char *full = rel[0] ? fmt("%s/%s", base, rel) : xstrdup(base);
    struct stat st;
    if (lstat(full, &st) != 0) { free(full); return; }
    if (rel[0]) pl_add(l, rel);
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(full);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char *r = rel[0] ? fmt("%s/%s", rel, e->d_name) : xstrdup(e->d_name);
                walk(base, r, l);
                free(r);
            }
            closedir(d);
        }
    }
    free(full);
}

static void put_octal(char *dst, size_t width, unsigned long long v)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%0*llo", (int)(width - 1), v);
    memcpy(dst, tmp, width - 1);
    dst[width - 1] = 0;
}

static int split_ustar(const char *name, char outname[100], char outprefix[155])
{
    size_t n = strlen(name);
    memset(outname, 0, 100);
    memset(outprefix, 0, 155);
    if (n < 100) { memcpy(outname, name, n); return 0; }
    /* split at a slash so suffix <= 99 and prefix <= 154 */
    for (size_t i = n - 1; i > 0; i--) {
        if (name[i] != '/') continue;
        size_t pre = i, suf = n - i - 1;
        if (pre <= 154 && suf <= 99 && suf > 0) {
            memcpy(outprefix, name, pre);
            memcpy(outname, name + i + 1, suf);
            return 0;
        }
    }
    return -1;
}

static int tar_add_file(int fd, const char *base, const char *rel, long long epoch)
{
    char *full = fmt("%s/%s", base, rel);
    struct stat st;
    if (lstat(full, &st) != 0) { free(full); return -1; }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        fprintf(stderr, "l2pm: unsupported file type in archive: %s\n", full);
        free(full);
        return -1;
    }

    char name[MAXREL];
    snprintf(name, sizeof(name), "%s%s", rel, S_ISDIR(st.st_mode) ? "/" : "");
    char nm[100], pre[155];
    if (split_ustar(name, nm, pre) != 0) {
        fprintf(stderr, "l2pm: path too long for ustar: %s\n", name);
        free(full);
        return -1;
    }

    char *link = NULL;
    if (S_ISLNK(st.st_mode)) {
        link = xmalloc((size_t)st.st_size + 1);
        ssize_t r = readlink(full, link, (size_t)st.st_size + 1);
        if (r < 0) { free(link); free(full); return -1; }
        link[r] = 0;
        if (strlen(link) >= 100) {
            fprintf(stderr, "l2pm: symlink target too long: %s\n", full);
            free(link); free(full); return -1;
        }
    }

    /* USTAR symlinks (type '2') store target in linkname and have size 0. */
    long long sz = S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
    /* 11 octal digits is the ustar size field limit; put_octal would
     * silently truncate anything larger and produce a corrupt archive. */
    if (sz > 077777777777LL) {
        fprintf(stderr, "l2pm: file too large for ustar: %s\n", full);
        free(link); free(full); return -1;
    }

    unsigned char hdr[512];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, nm, 100);
    put_octal((char *)hdr + 100, 8, st.st_mode & 07777);
    put_octal((char *)hdr + 108, 8, 0);
    put_octal((char *)hdr + 116, 8, 0);
    put_octal((char *)hdr + 124, 12, (unsigned long long)sz);
    put_octal((char *)hdr + 136, 12, (unsigned long long)epoch);
    memset(hdr + 148, ' ', 8);
    hdr[156] = S_ISDIR(st.st_mode) ? '5' : S_ISLNK(st.st_mode) ? '2' : '0';
    memcpy(hdr + 257, "ustar", 5);
    memcpy(hdr + 263, "00", 2);
    memcpy(hdr + 265, "root", 4);
    memcpy(hdr + 297, "root", 4);
    memcpy(hdr + 345, pre, 155);
    if (link) memcpy(hdr + 157, link, strlen(link));

    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += hdr[i];
    char cks[8];
    snprintf(cks, sizeof(cks), "%06o", sum);
    memcpy(hdr + 148, cks, 6);
    hdr[154] = 0;
    hdr[155] = ' ';

    if (write_all(fd, hdr, 512) != 0) { free(link); free(full); return -1; }

    if (S_ISREG(st.st_mode)) {
        int in = open(full, O_RDONLY | O_CLOEXEC);
        if (in < 0) { free(link); free(full); return -1; }
        char buf[65536];
        long long left = sz;
        while (left > 0) {
            ssize_t r = read(in, buf, (size_t)(left < (long long)sizeof(buf) ? left : (long long)sizeof(buf)));
            if (r < 0) { if (errno == EINTR) continue; close(in); free(link); free(full); return -1; }
            if (r == 0) break;
            if (write_all(fd, buf, (size_t)r) != 0) { close(in); free(link); free(full); return -1; }
            left -= r;
        }
        close(in);
        if (left != 0) { free(link); free(full); return -1; }
        char zero[512];
        memset(zero, 0, sizeof(zero));
        size_t pad = (size_t)((512 - (sz % 512)) % 512);
        if (pad && write_all(fd, zero, pad) != 0) { free(link); free(full); return -1; }
    }
    free(link);
    free(full);
    return 0;
}

static int tar_dir(const char *dir, const char *out, long long epoch)
{
    struct pathlist l = {0};
    walk(dir, "", &l);
    qsort(l.v, l.n, sizeof(*l.v), cmpstr);
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { fprintf(stderr, "l2pm: cannot create %s: %s\n", out, strerror(errno)); return -1; }
    int rc = 0;
    for (size_t i = 0; i < l.n; i++) {
        if (tar_add_file(fd, dir, l.v[i], epoch) != 0) { rc = -1; break; }
    }
    if (rc == 0) {
        char zero[1024];
        memset(zero, 0, sizeof(zero));
        if (write_all(fd, zero, sizeof(zero)) != 0) rc = -1;
    }
    if (close(fd) != 0 && rc == 0) rc = -1;
    for (size_t i = 0; i < l.n; i++) free(l.v[i]);
    free(l.v);
    if (rc != 0) unlink(out);
    return rc;
}

/* --------------------------------------------------------------- tar in */

/* Entry names must be relative and must not contain ".." or "." components
 * (a leading "./" is stripped by the caller, as tar commonly emits it). */
static int safe_rel(const char *name)
{
    if (!name[0] || name[0] == '/') return 0;
    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 0) break;
        if (n == 1 && p[0] == '.') return 0;
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        p = e;
    }
    return 1;
}

/* Lexically check that `link`, resolved relative to directory `dir`
 * ("" = archive root), stays inside the archive root.  Rejects absolute
 * targets, any ".." that would escape, and any ".." appearing after a
 * regular path component (since that component could itself be a symlink
 * pointing toward the root, turning "sym/.." into an escape). */
static int link_inside(const char *dir, const char *link)
{
    if (!link[0] || link[0] == '/') return 0;
    int depth = 0;
    const char *p = dir;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 0) break;
        if (n == 1 && p[0] == '.') { /* ignore */ }
        else if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        else depth++;
        p = e;
    }
    int seen_name = 0;
    p = link;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 0) break;
        if (n == 1 && p[0] == '.') { /* "." stays */ }
        else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (seen_name || --depth < 0) return 0;
        } else {
            seen_name = 1;
            depth++;
        }
        p = e;
    }
    return 1;
}

/* Verify that `rel` below `dest` traverses only real directories (never
 * symlinks) and resolves to an existing regular file. */
static int hardlink_target_ok(const char *dest, const char *rel)
{
    if (!safe_rel(rel)) return 0;
    char *cur = xstrdup(dest);
    const char *p = rel;
    int ok = 0;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 0) break;
        const char *rest = e;
        while (*rest == '/') rest++;
        int is_last = (*rest == 0);
        char *next = fmt("%s/%.*s", cur, (int)n, p);
        free(cur);
        cur = next;
        struct stat st;
        if (lstat(cur, &st) != 0) { ok = 0; break; }
        if (is_last) {
            ok = S_ISREG(st.st_mode);
            break;
        }
        if (!S_ISDIR(st.st_mode)) { ok = 0; break; }
        p = rest;
    }
    free(cur);
    return ok;
}

/* Strict tar numeric field: octal with optional leading spaces and
 * NUL/space padding, or GNU base-256 (positive form only).  Rejects
 * garbage instead of silently misparsing it like strtoll would. */
static int parse_tar_num(const unsigned char *p, size_t n, long long *out)
{
    if (n == 0) return -1;
    if (p[0] & 0x80) {
        if (p[0] != 0x80) return -1;   /* negative base-256: not meaningful */
        unsigned long long v = 0;
        for (size_t i = 1; i < n; i++) {
            if (v > (ULLONG_MAX - p[i]) / 256) return -1;
            v = v * 256 + p[i];
        }
        if (v > (unsigned long long)LLONG_MAX) return -1;
        *out = (long long)v;
        return 0;
    }
    size_t i = 0;
    while (i < n && p[i] == ' ') i++;
    long long v = 0;
    for (; i < n && p[i] && p[i] != ' '; i++) {
        if (p[i] < '0' || p[i] > '7') return -1;
        if (v > (LLONG_MAX - (p[i] - '0')) / 8) return -1;
        v = v * 8 + (p[i] - '0');
    }
    for (; i < n; i++)
        if (p[i] && p[i] != ' ') return -1;
    *out = v;                          /* empty field reads as 0 */
    return 0;
}

/* Validate the header checksum (both the unsigned and the GNU signed
 * convention) with the checksum field itself treated as spaces. */
static int tar_hdr_checksum_ok(const unsigned char *h)
{
    long long want;
    if (parse_tar_num(h + 148, 8, &want) != 0) return 0;
    unsigned usum = 0;
    long ssum = 0;
    for (int i = 0; i < 512; i++) {
        unsigned char c = (i >= 148 && i < 156) ? ' ' : h[i];
        usum += c;
        ssum += (signed char)c;
    }
    return want == (long long)usum || want == ssum;
}

/* Every parent of `out` below `dest` must be a real directory, never a
 * symlink: this is what stops a previously extracted symlink from being
 * used as a write-through tunnel out of dest.  Missing parents are created
 * (tar archives may list files before their directory entries). */
static int ensure_parents(const char *dest, const char *out)
{
    size_t dn = strlen(dest);
    char *tmp = xstrdup(out);
    int rc = 0;
    for (char *p = tmp + dn + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        struct stat st;
        if (lstat(tmp, &st) != 0) {
            if (errno != ENOENT || mkdir(tmp, 0755) != 0) { rc = -1; break; }
        } else if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "l2pm: path component is not a directory: %s\n", tmp);
            rc = -1; break;
        }
        *p = '/';
    }
    free(tmp);
    return rc;
}

/* Directory modes are applied after the whole archive is extracted, so a
 * read-only directory entry cannot block writing its children. */
struct dirfix { char *path; mode_t mode; long long mt; };

#define TAR_MAX_ENTRIES    65536
#define TAR_MAX_FILE_SIZE  (512LL << 20)   /* 512 MiB per entry */
#define TAR_MAX_TOTAL_SIZE (1LL << 30)     /* 1 GiB total extracted */

static int tar_extract(const char *tar, const char *dest)
{
    FILE *f = fopen(tar, "rb");
    if (!f) { fprintf(stderr, "l2pm: cannot open %s: %s\n", tar, strerror(errno)); return -1; }
    if (mkdir_p(dest, 0755) != 0) { fclose(f); return -1; }
    struct dirfix *df = NULL;
    size_t dfn = 0, dfc = 0;
    size_t nentries = 0;
    long long total_bytes = 0;
    int rc = 0;
    for (;;) {
        unsigned char hdr[512];
        if (fread(hdr, 1, 512, f) != 512) {
            fprintf(stderr, "l2pm: truncated tar header in %s\n", tar);
            rc = -1; break;
        }
        int zero = 1;
        for (int i = 0; i < 512; i++) if (hdr[i]) { zero = 0; break; }
        if (zero) {
            /* End of archive: allow trailing all-zero 512B blocks (POSIX 2x512
             * or 10 KiB blocking), reject any non-zero byte or partial block. */
            unsigned char z2[512];
            size_t zblocks = 1;
            for (;;) {
                size_t r = fread(z2, 1, 512, f);
                if (r == 0) {
                    if (ferror(f)) rc = -1;
                    break;
                }
                if (r != 512) {
                    fprintf(stderr, "l2pm: truncated end-of-archive in %s\n", tar);
                    rc = -1;
                    break;
                }
                for (int i = 0; i < 512; i++) {
                    if (z2[i]) {
                        fprintf(stderr, "l2pm: corrupt end-of-archive in %s\n", tar);
                        rc = -1;
                        break;
                    }
                }
                if (rc != 0) break;
                if (++zblocks > 256) {
                    fprintf(stderr, "l2pm: excessive zero trailer in %s\n", tar);
                    rc = -1;
                    break;
                }
            }
            break;
        }
        if (++nentries > TAR_MAX_ENTRIES) {
            fprintf(stderr, "l2pm: tar entry count exceeds limit (%d) in %s\n",
                    TAR_MAX_ENTRIES, tar);
            rc = -1; break;
        }
        if (!tar_hdr_checksum_ok(hdr)) {
            fprintf(stderr, "l2pm: bad tar header checksum in %s\n", tar);
            rc = -1; break;
        }
        if (memcmp(hdr + 257, "ustar", 5) != 0) {
            fprintf(stderr, "l2pm: not a ustar archive: %s\n", tar);
            rc = -1; break;
        }

        long long size, mt, m;
        if (parse_tar_num(hdr + 124, 12, &size) != 0 ||
            parse_tar_num(hdr + 136, 12, &mt) != 0 ||
            parse_tar_num(hdr + 100, 8, &m) != 0 || m < 0) {
            fprintf(stderr, "l2pm: bad numeric field in tar header (%s)\n", tar);
            rc = -1; break;
        }
        if (size < 0 || size > TAR_MAX_FILE_SIZE ||
            total_bytes > TAR_MAX_TOTAL_SIZE - size) {
            fprintf(stderr, "l2pm: tar entry size out of range (%s)\n", tar);
            rc = -1; break;
        }
        total_bytes += size;
        if ((m & (S_ISUID | S_ISGID)) != 0) {
            fprintf(stderr, "l2pm: setuid/setgid mode not permitted in archive (%s)\n", tar);
            rc = -1; break;
        }
        mode_t mode = (mode_t)m & 01777;
        char type = (char)hdr[156];

        /* Extended metadata (PAX 'x'/'g', GNU longname 'L'/'K'): consume
         * the payload so the stream stays synchronized, then reject the
         * archive cleanly.  l2pm's own archives are plain sorted ustar. */
        if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            size_t pad = (size_t)((512 - (size % 512)) % 512);
            if (fseeko(f, (off_t)size, SEEK_CUR) != 0 ||
                (pad && fseeko(f, (off_t)pad, SEEK_CUR) != 0)) {
                fprintf(stderr, "l2pm: truncated extended header in %s\n", tar);
            } else {
                fprintf(stderr, "l2pm: extended tar metadata (type '%c') is not supported\n", type);
            }
            rc = -1; break;
        }

        char name[256], prefix[156], linkname[101];
        memcpy(name, hdr + 0, 100); name[100] = 0;
        memcpy(prefix, hdr + 345, 155); prefix[155] = 0;
        memcpy(linkname, hdr + 157, 100); linkname[100] = 0;
        char fullrel[512];
        if (prefix[0]) snprintf(fullrel, sizeof(fullrel), "%s/%s", prefix, name);
        else snprintf(fullrel, sizeof(fullrel), "%s", name);
        char *rel = fullrel;
        while (rel[0] == '.' && rel[1] == '/') rel += 2;
        size_t rn = strlen(rel);
        while (rn && rel[rn-1] == '/') rel[--rn] = 0;
        if (!rel[0] || !safe_rel(rel)) {
            fprintf(stderr, "l2pm: unsafe archive path: %s\n", fullrel);
            rc = -1; break;
        }

        char *out = fmt("%s/%s", dest, rel);
        if (ensure_parents(dest, out) != 0) { free(out); rc = -1; break; }

        if (type == '5') {
            if (size != 0) {
                fprintf(stderr, "l2pm: directory entry with non-zero size: %s\n", rel);
                rc = -1;
            } else {
                struct stat st;
                if (lstat(out, &st) == 0) {
                    if (!S_ISDIR(st.st_mode)) {
                        if (unlink(out) != 0 || mkdir(out, 0755) != 0) rc = -1;
                    }
                } else if (errno == ENOENT) {
                    if (mkdir(out, 0755) != 0) rc = -1;
                } else rc = -1;
                if (rc == 0) {
                    if (dfn == dfc) {
                        dfc = dfc ? dfc * 2 : 64;
                        df = xrealloc(df, dfc * sizeof(*df));
                    }
                    df[dfn].path = xstrdup(out);
                    df[dfn].mode = mode ? mode : 0755;
                    df[dfn].mt = mt;
                    dfn++;
                }
            }
        } else if (type == '2') {
            /* Symlink: the target must stay inside the package root. */
            char *dir = xstrdup(rel);
            char *sl = strrchr(dir, '/');
            if (sl) *sl = 0; else dir[0] = 0;
            int ok = size == 0 && link_inside(dir, linkname);
            free(dir);
            if (!ok) {
                fprintf(stderr, "l2pm: unsafe symlink target: %s -> %s\n", rel, linkname);
                rc = -1;
            } else {
                unlink(out);
                if (symlink(linkname, out) != 0) rc = -1;
            }
        } else if (type == '1') {
            /* Hardlink: target is archive-root-relative, must traverse only
             * real directories inside dest and end at an existing regular file. */
            const char *lrel = linkname;
            while (lrel[0] == '.' && lrel[1] == '/') lrel += 2;
            char *target = fmt("%s/%s", dest, lrel);
            if (size != 0 || !hardlink_target_ok(dest, lrel)) {
                fprintf(stderr, "l2pm: unsafe or missing hardlink target: %s -> %s\n", rel, linkname);
                rc = -1;
            } else {
                unlink(out);
                if (link(target, out) != 0) rc = -1;
            }
            free(target);
        } else if (type == '0' || type == 0) {
            /* Regular file: never write through an existing symlink (the
             * entry may be a duplicate name); O_NOFOLLOW plus unlink make
             * the final component safe, fchmod avoids a chmod race. */
            unlink(out);
            int o = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                         mode ? mode : 0644);
            if (o < 0) rc = -1;
            else {
                long long left = size;
                unsigned char buf[65536];
                while (left > 0) {
                    size_t want = (size_t)(left < (long long)sizeof(buf) ? left : (long long)sizeof(buf));
                    size_t r = fread(buf, 1, want, f);
                    if (r != want) { rc = -1; break; }
                    size_t off = 0;
                    while (off < r) {
                        ssize_t w = write(o, buf + off, r - off);
                        if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
                        off += (size_t)w;
                    }
                    if (rc != 0) break;
                    left -= (long long)r;
                }
                if (rc == 0 && fchmod(o, mode ? mode : 0644) != 0) rc = -1;
                if (rc == 0 && fsync(o) != 0) rc = -1;
                if (close(o) != 0 && rc == 0) rc = -1;
            }
            if (rc == 0 && mt >= 0) {
                struct timespec ts[2];
                ts[0].tv_sec = (time_t)mt; ts[0].tv_nsec = 0;
                ts[1] = ts[0];
                utimensat(AT_FDCWD, out, ts, AT_SYMLINK_NOFOLLOW);
            }
        } else {
            fprintf(stderr, "l2pm: unsupported tar entry type '%c' (%s)\n", type, rel);
            rc = -1;
        }
        free(out);
        if (rc != 0) break;

        size_t pad = (size_t)((512 - (size % 512)) % 512);
        if (pad) {
            unsigned char pbuf[512];
            if (fread(pbuf, 1, pad, f) != pad) {
                fprintf(stderr, "l2pm: truncated tar padding in %s\n", tar);
                rc = -1; break;
            }
            for (size_t pi = 0; pi < pad; pi++) {
                if (pbuf[pi]) {
                    fprintf(stderr, "l2pm: non-zero tar padding in %s\n", tar);
                    rc = -1; break;
                }
            }
            if (rc != 0) break;
        }
    }
    fclose(f);
    if (rc == 0) {
        /* Apply directory modes deepest-first (best effort mtime). */
        for (size_t i = dfn; i > 0; i--) {
            if (chmod(df[i-1].path, df[i-1].mode) != 0) rc = -1;
            if (df[i-1].mt >= 0) {
                struct timespec ts[2];
                ts[0].tv_sec = (time_t)df[i-1].mt; ts[0].tv_nsec = 0;
                ts[1] = ts[0];
                utimensat(AT_FDCWD, df[i-1].path, ts, AT_SYMLINK_NOFOLLOW);
            }
        }
    }
    for (size_t i = 0; i < dfn; i++) free(df[i].path);
    free(df);
    return rc;
}

/* --------------------------------------------------------------- recipe */

struct recipe {
    char name[128];
    char version[128];
    char source[MAXPATH];
    char sha256[128];
    char srcdir[MAXPATH];
    char base[MAXPATH];
    char build[16384];
    char deps[1024];
    long long epoch;
    /* 4-step pipeline: repo (git source) / compile / install.
     * Legacy recipes use build= alone (compile+install combined). */
    char repo[MAXPATH];
    char ref[128];
    char compile[16384];
    char install[16384];
};

static int parse_recipe(const char *path, struct recipe *r)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->base, sizeof(r->base), "%s", path);
    char *bs = strrchr(r->base, '/');
    if (bs) *bs = 0;
    else snprintf(r->base, sizeof(r->base), ".");
    r->epoch = DEFAULT_EPOCH;
    const char *e = getenv("SOURCE_DATE_EPOCH");
    if (e && *e) r->epoch = strtoll(e, NULL, 10);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "l2pm: cannot read recipe %s: %s\n", path, strerror(errno)); return -1; }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    size_t line_number = 0;
    while ((n = getline(&line, &cap, f)) >= 0) {
        line_number++;
        if (memchr(line, 0, (size_t)n) != NULL) {
            fprintf(stderr, "l2pm: recipe %s:%zu: embedded NUL byte not permitted\n",
                    path, line_number);
            free(line);
            fclose(f);
            return -1;
        }
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s);
        char *v = trim(eq + 1);
        char *dst = NULL;
        size_t size = 0;
        if (!strcmp(k, "name")) { dst = r->name; size = sizeof(r->name); }
        else if (!strcmp(k, "version")) { dst = r->version; size = sizeof(r->version); }
        else if (!strcmp(k, "source")) { dst = r->source; size = sizeof(r->source); }
        else if (!strcmp(k, "sha256")) { dst = r->sha256; size = sizeof(r->sha256); }
        else if (!strcmp(k, "source_dir") || !strcmp(k, "srcdir")) { dst = r->srcdir; size = sizeof(r->srcdir); }
        else if (!strcmp(k, "build")) { dst = r->build; size = sizeof(r->build); }
        else if (!strcmp(k, "deps")) { dst = r->deps; size = sizeof(r->deps); }
        else if (!strcmp(k, "epoch")) r->epoch = strtoll(v, NULL, 10);
        else if (!strcmp(k, "repo")) { dst = r->repo; size = sizeof(r->repo); }
        else if (!strcmp(k, "ref")) { dst = r->ref; size = sizeof(r->ref); }
        else if (!strcmp(k, "compile")) { dst = r->compile; size = sizeof(r->compile); }
        else if (!strcmp(k, "install")) { dst = r->install; size = sizeof(r->install); }
        if (dst) {
            size_t len = strlen(v);
            if (len >= size) {
                fprintf(stderr, "l2pm: recipe %s:%zu: %s exceeds %zu bytes\n",
                        path, line_number, k, size - 1);
                free(line);
                fclose(f);
                return -1;
            }
            memcpy(dst, v, len + 1);
        }
    }
    free(line);
    fclose(f);
    if (!valid_name(r->name) || !valid_name(r->version)) {
        fprintf(stderr, "l2pm: recipe %s needs valid name/version\n", path);
        return -1;
    }
    if (r->repo[0] && r->source[0]) {
        fprintf(stderr, "l2pm: recipe %s: use either source= or repo=, not both\n", path);
        return -1;
    }
    if (r->repo[0] && !r->ref[0]) {
        fprintf(stderr, "l2pm: recipe %s: repo= needs ref= (pinned commit hash)\n", path);
        return -1;
    }
    if (!r->repo[0] && r->ref[0]) {
        fprintf(stderr, "l2pm: recipe %s: ref= requires repo=\n", path);
        return -1;
    }
    if ((r->compile[0] || r->install[0]) && r->build[0]) {
        fprintf(stderr, "l2pm: recipe %s: use either build= or compile=/install=, not both\n", path);
        return -1;
    }
    if (r->compile[0] && !r->install[0]) {
        fprintf(stderr, "l2pm: recipe %s: compile= requires install=\n", path);
        return -1;
    }
    if (r->repo[0] && r->sha256[0])
        fprintf(stderr, "l2pm: warning: sha256= ignored for repo= sources (the commit hash is the pin)\n");
    return 0;
}

static int run_shell(const char *cmd, const char *cwd, int quiet)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) _exit(126);
        if (quiet) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        }
        execl("/bin/sh", "sh", "-e", "-u", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int copy_tree(const char *src, const char *dst);

static int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir_p(dst, st.st_mode & 07777) != 0) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *e;
        int rc = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char *s = fmt("%s/%s", src, e->d_name);
            char *t = fmt("%s/%s", dst, e->d_name);
            if (copy_tree(s, t) != 0) rc = -1;
            free(s); free(t);
        }
        closedir(d);
        return rc;
    }
    if (S_ISLNK(st.st_mode)) {
        char *buf = xmalloc((size_t)st.st_size + 1);
        ssize_t r = readlink(src, buf, (size_t)st.st_size + 1);
        if (r < 0) { free(buf); return -1; }
        buf[r] = 0;
        unlink(dst);
        int rc = symlink(buf, dst);
        free(buf);
        return rc;
    }
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) { if (errno == EINTR) continue; rc = -1; break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += w;
        }
        if (rc != 0) break;
    }
    close(in);
    close(out);
    return rc;
}

static int verify_sha256(const char *file, const char *want)
{
    if (!want || !*want || !strcmp(want, "-")) return 0;
    char got[65];
    if (hash_file(file, got) != 0) return -1;
    if (strcasecmp(got, want) != 0) {
        fprintf(stderr, "l2pm: sha256 mismatch for %s\n  want %s\n  got  %s\n", file, want, got);
        return -1;
    }
    return 0;
}

static int valid_url_chars(const char *s)
{
    if (!s || !*s || s[0] == '-') return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c == 0x7f) return 0;
    }
    return 1;
}

static int valid_git_url(const char *s)
{
    if (!valid_url_chars(s)) return 0;
    if (!strncasecmp(s, "ext::", 5) || !strncasecmp(s, "fd::", 4)) return 0;
    if (!strncmp(s, "https://", 8) || !strncmp(s, "http://", 7) ||
        !strncmp(s, "git://", 6) || !strncmp(s, "ssh://", 6) ||
        !strncmp(s, "git@", 4) || !strncmp(s, "file://", 7) ||
        s[0] == '/' || (s[0] == '.' && s[1] == '/'))
        return 1;
    return 0;
}

static int fetch_source(const struct recipe *r, const char *cache, const char *srcdir)
{
    if (!r->source[0]) { mkdir_p(srcdir, 0755); return 0; }
    char *file = NULL;
    if (!strncmp(r->source, "file://", 7)) {
        file = xstrdup(r->source + 7);
    } else {
        char *local = (r->source[0] == '/') ? xstrdup(r->source)
                                            : fmt("%s/%s", r->base, r->source);
        if (access(local, R_OK) == 0) file = local;
        else free(local);
    }
    if (!file && (!strncmp(r->source, "http://", 7) || !strncmp(r->source, "https://", 8))) {
        if (!valid_url_chars(r->source)) {
            fprintf(stderr, "l2pm: unsafe source URL: %s\n", r->source);
            return -1;
        }
        const char *slash = strrchr(r->source, '/');
        const char *raw_base = (slash && slash[1]) ? slash + 1 : "source.tar";
        size_t blen = strcspn(raw_base, "?#");
        char base[256];
        if (blen == 0 || blen >= sizeof(base) || raw_base[0] == '.' || raw_base[0] == '-') {
            snprintf(base, sizeof(base), "source.tar");
        } else {
            memcpy(base, raw_base, blen);
            base[blen] = 0;
        }
        file = fmt("%s/%s", cache, base);
        if (access(file, R_OK) != 0) {
            char *part = fmt("%s.part-%ld", file, (long)getpid());
            char *q = shq(r->source);
            char *qp = shq(part);
            char *cmd = fmt("curl -fL --proto =http,https --proto-redir =http,https "
                            "--retry 3 --connect-timeout 20 -o %s -- %s || "
                            "wget -O %s -- %s",
                            qp, q, qp, q);
            int rc = run_shell(cmd, NULL, 0);
            free(cmd); free(q); free(qp);
            if (rc != 0 || verify_sha256(part, r->sha256) != 0) {
                unlink(part);
                free(part);
                free(file);
                return -1;
            }
            int pfd = open(part, O_RDONLY | O_CLOEXEC);
            if (pfd >= 0) { fsync(pfd); close(pfd); }
            if (rename(part, file) != 0) {
                unlink(part);
                free(part);
                free(file);
                return -1;
            }
            fsync_dir(cache);
            free(part);
        }
    }
    if (!file) {
        fprintf(stderr, "l2pm: unsupported source: %s\n", r->source);
        return -1;
    }

    if (verify_sha256(file, r->sha256) != 0) { free(file); return -1; }

    struct stat st;
    if (stat(file, &st) != 0) { free(file); return -1; }
    if (S_ISDIR(st.st_mode)) {
        if (copy_tree(file, srcdir) != 0) { free(file); return -1; }
    } else if (S_ISREG(st.st_mode)) {
        const char *exts[] = { ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz",
                               ".tar.bz2", ".tbz2", ".tar.zst", NULL };
        int is_tar = 0;
        size_t fl = strlen(file);
        for (int i = 0; exts[i]; i++) {
            size_t el = strlen(exts[i]);
            if (fl >= el && !strcmp(file + fl - el, exts[i])) { is_tar = 1; break; }
        }
        if (is_tar) {
            char *safe_file = (file[0] == '-') ? fmt("./%s", file) : xstrdup(file);
            char *safe_dir = (srcdir[0] == '-') ? fmt("./%s", srcdir) : xstrdup(srcdir);
            char *q = shq(safe_file), *d = shq(safe_dir);
            char *cmd = fmt("tar -C %s -xf %s", d, q);
            int rc = run_shell(cmd, NULL, 0);
            free(cmd); free(q); free(d); free(safe_file); free(safe_dir);
            if (rc != 0) { free(file); return -1; }
        } else {
            const char *bn = strrchr(file, '/');
            bn = bn ? bn + 1 : file;
            char *dst = fmt("%s/%s", srcdir, bn);
            if (copy_tree(file, dst) != 0) { free(dst); free(file); return -1; }
            free(dst);
        }
    }
    free(file);
    return 0;
}

static char *find_src_root(const char *dir, const struct recipe *r)
{
    if (r->srcdir[0]) return fmt("%s/%s", dir, r->srcdir);
    DIR *d = opendir(dir);
    if (!d) return xstrdup(dir);
    char *only = NULL;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        count++;
        free(only);
        only = xstrdup(e->d_name);
    }
    closedir(d);
    if (count == 1) {
        char *p = fmt("%s/%s", dir, only);
        struct stat st;
        free(only);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return p;
        free(p);
        return xstrdup(dir);
    }
    free(only);
    return xstrdup(dir);
}

/* ------------------------------------------------------- git sources */

/* A ref reaches the shell via git checkout: allow only the git ref charset
 * and never a leading '-' (option injection) or ".." (path traversal). */
static int valid_ref(const char *s)
{
    if (!s || !*s || s[0] == '-' || strstr(s, "..")) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("._/-", *p)) return 0;
    return 1;
}

static int is_commit_hash(const char *s)
{
    if (strlen(s) != 40) return 0;
    for (int i = 0; i < 40; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int git_available(void)
{
    return run_shell("command -v git >/dev/null 2>&1", NULL, 1) == 0;
}

/* Step 1 (repo) for package sources: clone/update a cached clone, check out
 * the pinned ref, export the worktree without .git and normalize mtimes to
 * the recipe epoch so the build stays deterministic.  The commit hash is
 * the pin (it replaces the tarball sha256). */
static int fetch_repo(const struct recipe *r, const char *cache, const char *srcdir)
{
    if (!git_available()) {
        fprintf(stderr, "l2pm: git is required for repo= sources (l2 deps install)\n");
        return -1;
    }
    if (!valid_git_url(r->repo)) {
        fprintf(stderr, "l2pm: unsafe git repo URL: %s\n", r->repo);
        return -1;
    }
    if (!valid_ref(r->ref)) {
        fprintf(stderr, "l2pm: bad git ref: %s\n", r->ref);
        return -1;
    }
    if (!is_commit_hash(r->ref))
        fprintf(stderr, "l2pm: warning: ref '%s' is not a commit hash; the build is NOT reproducible\n", r->ref);

    char *parent = fmt("%s/git", cache);
    mkdir_p(parent, 0755);
    free(parent);
    char *gdir = fmt("%s/git/%s", cache, r->name);
    struct stat st;
    if (stat(gdir, &st) != 0) {
        char *qr = shq(r->repo), *qg = shq(gdir);
        char *cmd = fmt("git -c protocol.ext.allow=never clone --quiet -- %s %s", qr, qg);
        int rc = run_shell(cmd, NULL, 0);
        free(cmd); free(qr); free(qg);
        if (rc != 0) {
            fprintf(stderr, "l2pm: git clone failed: %s\n", r->repo);
            free(gdir);
            return -1;
        }
    } else {
        /* Best effort: a cached clone keeps working offline. */
        if (run_shell("git -c protocol.ext.allow=never fetch --quiet origin", gdir, 1) != 0)
            fprintf(stderr, "l2pm: git fetch failed; using the cached clone (offline?)\n");
    }
    char *qref = shq(r->ref);
    char *cmd = fmt("git checkout --quiet --detach %s --", qref);
    int rc = run_shell(cmd, gdir, 0);
    free(cmd); free(qref);
    if (rc != 0) {
        fprintf(stderr, "l2pm: git checkout of ref '%s' failed\n", r->ref);
        free(gdir);
        return -1;
    }
    /* Export the worktree; .git must not leak into the package. */
    if (copy_tree(gdir, srcdir) != 0) {
        fprintf(stderr, "l2pm: cannot export git worktree\n");
        free(gdir);
        return -1;
    }
    char *gitmeta = fmt("%s/.git", srcdir);
    rm_rf(gitmeta);
    free(gitmeta);
    char *qs = shq(srcdir);
    cmd = fmt("find %s -exec touch -h -d @%lld {} +", qs, r->epoch);
    rc = run_shell(cmd, NULL, 1);
    free(cmd); free(qs);
    free(gdir);
    if (rc != 0) {
        fprintf(stderr, "l2pm: mtime normalization failed\n");
        return -1;
    }
    return 0;
}

static int build_once(const struct recipe *r, const char *outtar)
{
    char *buildroot = l2path("build/%s-%s-%ld", r->name, r->version, (long)getpid());
    char *stage = fmt("%s/stage", buildroot);
    char *src = fmt("%s/src", buildroot);
    char *cache = l2path("cache");
    rm_rf(buildroot);
    if (mkdir_p(stage, 0755) != 0 || mkdir_p(src, 0755) != 0 || mkdir_p(cache, 0755) != 0) {
        fprintf(stderr, "l2pm: cannot create build dirs\n");
        free(buildroot); free(stage); free(src); free(cache);
        return -1;
    }

    /* [1/4] repo + [2/4] fetch/extract */
    int rc = 0;
    if (r->repo[0]) {
        fprintf(stderr, "l2pm: [1/4] repo: git %s @ %s\n", r->repo, r->ref);
        if (fetch_repo(r, cache, src) != 0) rc = -1;
        if (rc == 0) fprintf(stderr, "l2pm: [2/4] fetch: worktree exported (mtimes normalized)\n");
    } else {
        fprintf(stderr, "l2pm: [1/4] repo: local recipe\n");
        fprintf(stderr, "l2pm: [2/4] fetch: %s\n", r->source[0] ? r->source : "(no source)");
        if (fetch_source(r, cache, src) != 0) rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "l2pm: source fetch failed for %s\n", r->name);
        free(buildroot); free(stage); free(src); free(cache);
        return -1;
    }
    char *srcroot = find_src_root(src, r);

    char ep[64], stagevar[MAXPATH], srcvar[MAXPATH];
    snprintf(ep, sizeof(ep), "%lld", r->epoch);
    snprintf(stagevar, sizeof(stagevar), "DESTDIR=%s", stage);
    snprintf(srcvar, sizeof(srcvar), "SRC=%s", srcroot);
    setenv("SOURCE_DATE_EPOCH", ep, 1);
    setenv("DESTDIR", stage, 1);
    setenv("SRC", srcroot, 1);
    setenv("PREFIX", "/usr", 1);
    setenv("TZ", "UTC", 1);
    setenv("LC_ALL", "C", 1);
    setenv("LANG", "C", 1);
    if (!getenv("JOBS")) setenv("JOBS", "8", 1);
    if (!getenv("CC")) setenv("CC", "cc", 1);
    (void)stagevar; (void)srcvar;
    umask(022);

    /* [3/4] compile + [4/4] install (phased), or the legacy combined build= */
    if (r->compile[0] || r->install[0]) {
        if (r->compile[0]) {
            fprintf(stderr, "l2pm: [3/4] compile: %s %s\n", r->name, r->version);
            rc = run_shell(r->compile, srcroot, 0);
            if (rc != 0) fprintf(stderr, "l2pm: compile command failed (%d)\n", rc);
        } else {
            fprintf(stderr, "l2pm: [3/4] compile: (none)\n");
        }
        if (rc == 0) {
            fprintf(stderr, "l2pm: [4/4] install: staging into %s\n", stage);
            rc = run_shell(r->install, srcroot, 0);
            if (rc != 0) fprintf(stderr, "l2pm: install command failed (%d)\n", rc);
        }
    } else if (r->build[0]) {
        fprintf(stderr, "l2pm: [3/4] build (legacy compile+install): %s %s\n", r->name, r->version);
        rc = run_shell(r->build, srcroot, 0);
        if (rc != 0) fprintf(stderr, "l2pm: build command failed (%d)\n", rc);
        else fprintf(stderr, "l2pm: [4/4] install: (included in legacy build)\n");
    }
    if (rc == 0) {
        char *infodir = fmt("%s/.l2pm", stage);
        mkdir_p(infodir, 0755);
        char info[2048];
        int n = snprintf(info, sizeof(info),
                         "name=%s\nversion=%s\nepoch=%lld\ndeps=%s\n",
                         r->name, r->version, r->epoch, r->deps);
        char *infofile = fmt("%s/info", infodir);
        if (write_file(infofile, info, (size_t)n) != 0) rc = -1;
        free(infofile); free(infodir);
    }
    if (rc == 0) {
        char *parent = xstrdup(outtar);
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = 0; mkdir_p(parent, 0755); *sl = '/'; }
        free(parent);
        rc = tar_dir(stage, outtar, r->epoch);
    }
    if (!getenv("L2PM_KEEP")) rm_rf(buildroot);
    free(buildroot); free(stage); free(src); free(cache); free(srcroot);
    return rc;
}

/* --------------------------------------------------------------- store */

static void ensure_layout(void)
{
    const char *dirs[] = { "store", "var/db/installed", "var/profiles", "cache", "src", "build", "tmp", "recipes", NULL };
    for (int i = 0; dirs[i]; i++) {
        char *p = l2path("%s", dirs[i]);
        mkdir_p(p, 0755);
        free(p);
    }
}

static char *store_path_for(const char *hash, const char *name, const char *version)
{
    return l2path("store/%s-%s-%s", hash, name, version);
}

static int read_info(const char *dir, char *name, size_t nn, char *ver, size_t vn, char *deps, size_t dn)
{
    char *path = fmt("%s/.l2pm/info", dir);
    char *txt = read_small(path);
    free(path);
    if (!txt) return -1;
    name[0] = ver[0] = deps[0] = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, "name")) snprintf(name, nn, "%s", eq);
        else if (!strcmp(line, "version")) snprintf(ver, vn, "%s", eq);
        else if (!strcmp(line, "deps")) snprintf(deps, dn, "%s", eq);
    }
    free(txt);
    return valid_name(name) && valid_name(ver) ? 0 : -1;
}

static int db_write(const char *name, const char *version, const char *hash,
                    const char *store, const char *deps)
{
    char *db = l2path("var/db/installed/%s", name);
    char buf[8192];
    int n = snprintf(buf, sizeof(buf),
                     "name=%s\nversion=%s\nhash=%s\nstore=%s\ndeps=%s\n",
                     name, version, hash, store, deps ? deps : "");
    if (n < 0 || (size_t)n >= sizeof(buf)) { free(db); return -1; }
    int rc = write_file_atomic(db, buf, (size_t)n);
    free(db);
    return rc;
}

static char *db_get(const char *name, const char *key)
{
    char *path = l2path("var/db/installed/%s", name);
    char *txt = read_small(path);
    free(path);
    if (!txt) return NULL;
    char *res = NULL;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(line, key)) { res = xstrdup(eq); break; }
    }
    free(txt);
    return res;
}

static int cmp_db_name(const void *a, const void *b);
static char **db_names(size_t *count)
{
    char *dir = l2path("var/db/installed");
    DIR *d = opendir(dir);
    char **v = NULL;
    size_t n = 0, cap = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            size_t len = strlen(e->d_name);
            if (len > 9 && !strcmp(e->d_name + len - 9, ".manifest")) continue;
            if (n == cap) { cap = cap ? cap * 2 : 16; v = xrealloc(v, cap * sizeof(*v)); }
            v[n++] = xstrdup(e->d_name);
        }
        closedir(d);
    }
    free(dir);
    if (n) qsort(v, n, sizeof(*v), cmp_db_name);
    *count = n;
    return v;
}

static int cmp_db_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int parse_deps_list(const char *deps_str, struct pathlist *out)
{
    if (!deps_str || !*deps_str) return 0;
    char *copy = xstrdup(deps_str);
    for (char *p = copy; *p; p++)
        if (*p == ',') *p = ' ';
    char *save = NULL;
    for (char *tok = strtok_r(copy, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (!valid_name(tok)) {
            fprintf(stderr, "l2pm: invalid dependency name '%s'\n", tok);
            free(copy);
            return -1;
        }
        if (!pl_has(out, tok)) pl_add(out, tok);
    }
    free(copy);
    return 0;
}

static int is_installed(const char *name)
{
    char *p = l2path("var/db/installed/%s", name);
    int ok = access(p, F_OK) == 0;
    free(p);
    return ok;
}

static int manifest_collect(const char *store, struct pathlist *lines)
{
    struct pathlist paths = {0};
    walk(store, "", &paths);
    if (paths.n) qsort(paths.v, paths.n, sizeof(*paths.v), cmpstr);
    for (size_t i = 0; i < paths.n; i++) {
        const char *rel = paths.v[i];
        char *full = fmt("%s/%s", store, rel);
        struct stat st;
        if (lstat(full, &st) != 0) {
            free(full);
            pl_free(&paths);
            pl_free(lines);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            char *ln = fmt("D\t%s", rel);
            pl_add(lines, ln);
            free(ln);
        } else if (S_ISLNK(st.st_mode)) {
            char *tgt = xmalloc((size_t)st.st_size + 1);
            ssize_t r = readlink(full, tgt, (size_t)st.st_size + 1);
            if (r < 0) {
                free(tgt); free(full);
                pl_free(&paths); pl_free(lines);
                return -1;
            }
            tgt[r] = 0;
            char *ln = fmt("L\t%s\t%s", rel, tgt);
            pl_add(lines, ln);
            free(ln);
            free(tgt);
        } else if (S_ISREG(st.st_mode)) {
            char hx[65];
            if (hash_file(full, hx) != 0) {
                free(full);
                pl_free(&paths); pl_free(lines);
                return -1;
            }
            char *ln = fmt("F\t%s\t%s", rel, hx);
            pl_add(lines, ln);
            free(ln);
        } else {
            free(full);
            pl_free(&paths);
            pl_free(lines);
            return -1;
        }
        free(full);
    }
    pl_free(&paths);
    return 0;
}

static int manifest_write(const char *name, const char *store)
{
    struct pathlist lines = {0};
    if (manifest_collect(store, &lines) != 0) return -1;
    size_t total = 0;
    for (size_t i = 0; i < lines.n; i++) total += strlen(lines.v[i]) + 1;
    char *buf = xmalloc(total + 1);
    size_t off = 0;
    for (size_t i = 0; i < lines.n; i++) {
        size_t len = strlen(lines.v[i]);
        memcpy(buf + off, lines.v[i], len);
        off += len;
        buf[off++] = '\n';
    }
    buf[off] = 0;
    char *mp = l2path("var/db/installed/%s.manifest", name);
    int rc = write_file_atomic(mp, buf, off);
    free(mp);
    free(buf);
    pl_free(&lines);
    return rc;
}

static int link_tree(const char *store, const char *profile)
{
    struct pathlist l = {0};
    walk(store, "", &l);
    qsort(l.v, l.n, sizeof(*l.v), cmpstr);
    int rc = 0;
    for (size_t i = 0; i < l.n; i++) {
        const char *rel = l.v[i];
        if (!strcmp(rel, ".l2pm") || !strncmp(rel, ".l2pm/", 6)) continue;
        char *src = fmt("%s/%s", store, rel);
        char *dst = fmt("%s/%s", profile, rel);
        struct stat st;
        if (lstat(src, &st) != 0) { free(src); free(dst); continue; }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir_p(dst, 0755) != 0) rc = -1;
        } else {
            char *slash = strrchr(dst, '/');
            if (slash) { *slash = 0; mkdir_p(dst, 0755); *slash = '/'; }
            /* File conflicts between packages: last-wins (stores are linked
             * in sorted order), but never silently. */
            struct stat ds;
            if (lstat(dst, &ds) == 0 && S_ISLNK(ds.st_mode)) {
                char old[MAXREL];
                ssize_t r = readlink(dst, old, sizeof(old) - 1);
                if (r > 0) {
                    old[r] = 0;
                    if (strcmp(old, src) != 0)
                        fprintf(stderr, "l2pm: warning: profile conflict at %s: %s overrides %s\n",
                                rel, src, old);
                }
            }
            unlink(dst);
            if (symlink(src, dst) != 0) rc = -1;
        }
        free(src); free(dst);
    }
    pl_free(&l);
    return rc;
}

static int parse_gen_for_profile(const char *entry, const char *profile, long *out_gen)
{
    size_t plen = strlen(profile);
    size_t elen = strlen(entry);
    if (elen <= plen + 6) return 0;
    if (strncmp(entry, profile, plen) != 0 || entry[plen] != '-') return 0;
    if (strcmp(entry + elen - 5, "-link") != 0) return 0;
    const char *num = entry + plen + 1;
    const char *end = entry + elen - 5;
    if (num >= end) return 0;
    for (const char *p = num; p < end; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    char *tmp = xmalloc((size_t)(end - num) + 1);
    memcpy(tmp, num, (size_t)(end - num));
    tmp[end - num] = 0;
    errno = 0;
    long v = strtol(tmp, NULL, 10);
    free(tmp);
    if (errno == ERANGE || v < 0) return 0;
    *out_gen = v;
    return 1;
}

static int parse_any_gen_link(const char *entry, char **out_profile, long *out_gen)
{
    size_t elen = strlen(entry);
    if (elen < 7 || strcmp(entry + elen - 5, "-link") != 0) return 0;
    const char *end = entry + elen - 5;
    const char *p = end;
    while (p > entry && isdigit((unsigned char)p[-1])) p--;
    if (p == end || p == entry || p[-1] != '-') return 0;
    size_t plen = (size_t)((p - 1) - entry);
    if (plen == 0) return 0;
    char *prof = xmalloc(plen + 1);
    memcpy(prof, entry, plen);
    prof[plen] = 0;
    if (!valid_name(prof)) { free(prof); return 0; }
    long v = 0;
    if (!parse_gen_for_profile(entry, prof, &v)) { free(prof); return 0; }
    *out_profile = prof;
    *out_gen = v;
    return 1;
}

static int switch_profile_symlink(const char *pdir, const char *profile, const char *target_rel)
{
    char *tmp_lnk = fmt("%s/.%s.link-%ld", pdir, profile, (long)getpid());
    unlink(tmp_lnk);
    if (symlink(target_rel, tmp_lnk) != 0) {
        free(tmp_lnk);
        return -1;
    }
    char *cur = fmt("%s/%s", pdir, profile);
    struct stat st;
    int rc = 0;
    if (lstat(cur, &st) == 0 && S_ISDIR(st.st_mode)) {
        char *old = fmt("%s/.%s.old-%ld", pdir, profile, (long)getpid());
        rm_rf(old);
        if (rename(cur, old) != 0 || rename(tmp_lnk, cur) != 0) {
            unlink(tmp_lnk);
            rc = -1;
        } else {
            if (fsync_dir(pdir) != 0) rc = -1;
            rm_rf(old);
        }
        free(old);
    } else if (rename(tmp_lnk, cur) != 0) {
        unlink(tmp_lnk);
        rc = -1;
    } else if (fsync_dir(pdir) != 0) {
        rc = -1;
    }
    free(cur);
    free(tmp_lnk);
    return rc;
}

static int rebuild_profile(const char *profile)
{
    if (!valid_name(profile)) { fprintf(stderr, "l2pm: bad profile name\n"); return -1; }
    char *pdir = l2path("var/profiles");
    if (mkdir_p(pdir, 0755) != 0) { free(pdir); return -1; }

    long max_gen = 0;
    DIR *d = opendir(pdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            long g = 0;
            if (parse_gen_for_profile(e->d_name, profile, &g) && g > max_gen)
                max_gen = g;
        }
        closedir(d);
    }
    long next = max_gen + 1;

    char *stage = fmt("%s/.%s-%ld-tmp-%ld", pdir, profile, next, (long)getpid());
    rm_rf(stage);
    if (mkdir_p(stage, 0755) != 0) { free(stage); free(pdir); return -1; }

    /* Stable PATH directories even before the first package is installed.
     * The L2 rootfs points /usr/bin at this tree. */
    const char *stddirs[] = { "bin", "sbin", "usr/bin", "usr/sbin", NULL };
    for (int i = 0; stddirs[i]; i++) {
        char *p = fmt("%s/%s", stage, stddirs[i]);
        mkdir_p(p, 0755);
        free(p);
    }

    char *genfile = fmt("%s/.l2pm-gen", stage);
    FILE *gf = fopen(genfile, "w");
    if (!gf) { free(genfile); rm_rf(stage); free(stage); free(pdir); return -1; }

    size_t n = 0;
    char **names = db_names(&n);
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        char *ver = db_get(names[i], "version");
        char *hash = db_get(names[i], "hash");
        char *store = db_get(names[i], "store");
        char *deps = db_get(names[i], "deps");
        if (store && link_tree(store, stage) != 0) rc = -1;
        if (fprintf(gf, "%s\t%s\t%s\t%s\t%s\n",
                    names[i],
                    ver ? ver : "",
                    hash ? hash : "",
                    store ? store : "",
                    deps ? deps : "") < 0) rc = -1;
        free(ver); free(hash); free(store); free(deps);
        free(names[i]);
    }
    free(names);
    if (fflush(gf) != 0 || fsync(fileno(gf)) != 0) rc = -1;
    if (fclose(gf) != 0) rc = -1;
    free(genfile);

    char *marker = fmt("%s/.l2pm-profile", stage);
    if (write_file(marker, "profile\n", 8) != 0) rc = -1;
    free(marker);

    char *gen_rel = fmt("%s-%ld-link", profile, next);
    char *gen_dir = fmt("%s/%s", pdir, gen_rel);
    if (rc == 0) {
        rm_rf(gen_dir);
        if (fsync_tree(stage) != 0 || rename(stage, gen_dir) != 0 ||
            fsync_dir(pdir) != 0) rc = -1;
    }
    if (rc == 0 && switch_profile_symlink(pdir, profile, gen_rel) != 0) rc = -1;
    if (rc != 0) rm_rf(stage);

    free(gen_dir);
    free(gen_rel);
    free(stage);
    free(pdir);
    return rc;
}

/* ---------------------------------------------------------- transactions */

/* Failure contract (one command = one transaction):
 * `add` (including recursive dependencies), `install` (one or more archives)
 * and `remove` (one or more names) restore the database and the active
 * profile generation to their pre-command state when any step fails, and
 * remove the generations the failed command created.  Store paths and the
 * build cache are NEVER deleted by a transaction rollback: they are
 * content-addressed and `gc` reclaims unreferenced paths.  A crash (SIGKILL,
 * power loss) is not rolled back automatically; every fully completed
 * package leaves a consistent DB/profile pair, `rollback` undoes completed
 * steps and `verify --repair` reconciles torn states.  Transactions assume
 * a single writer: concurrent l2pm mutations are not locked. */

struct txn_saved {
    char *name;
    char *db;       /* pre-command db entry bytes, NULL when absent */
    char *manifest; /* pre-command manifest bytes, NULL when absent */
};

struct txn {
    const char *profile;
    int have_gen;
    long gen;       /* active generation at begin, -1 when none */
    struct txn_saved *saved;
    size_t n, cap;
};

static int read_active_gen(const char *pdir, const char *profile,
                           char *target, size_t cap, long *out_gen);

static void txn_begin(struct txn *t, const char *profile)
{
    memset(t, 0, sizeof(*t));
    t->profile = profile;
    t->gen = -1;
    char *pdir = l2path("var/profiles");
    char target[MAXPATH];
    long g = -1;
    if (read_active_gen(pdir, profile, target, sizeof(target), &g) == 0) {
        t->have_gen = 1;
        t->gen = g;
    }
    free(pdir);
}

static void txn_free(struct txn *t)
{
    for (size_t i = 0; i < t->n; i++) {
        free(t->saved[i].name);
        free(t->saved[i].db);
        free(t->saved[i].manifest);
    }
    free(t->saved);
    t->saved = NULL;
    t->n = t->cap = 0;
}

/* Snapshot the pre-command state of `name` (first record wins).  Must be
 * called before any mutation of the entry.  Fails closed when an existing
 * file cannot be read, rather than lose the pre-state. */
static int txn_record(struct txn *t, const char *name)
{
    for (size_t i = 0; i < t->n; i++)
        if (!strcmp(t->saved[i].name, name)) return 0;
    char *dbp = l2path("var/db/installed/%s", name);
    char *mpp = l2path("var/db/installed/%s.manifest", name);
    char *db = read_small(dbp);
    char *mp = read_small(mpp);
    int rc = 0;
    if ((!db && access(dbp, F_OK) == 0) || (!mp && access(mpp, F_OK) == 0)) {
        fprintf(stderr, "l2pm: cannot snapshot database entry for %s\n", name);
        rc = -1;
    }
    if (rc == 0) {
        if (t->n == t->cap) {
            t->cap = t->cap ? t->cap * 2 : 16;
            t->saved = xrealloc(t->saved, t->cap * sizeof(*t->saved));
        }
        t->saved[t->n].name = xstrdup(name);
        t->saved[t->n].db = db;
        t->saved[t->n].manifest = mp;
        t->n++;
        db = mp = NULL;
    }
    free(db); free(mp); free(dbp); free(mpp);
    return rc;
}

/* Drop generations created during the transaction and point the profile
 * symlink back at the pre-command generation (or remove it when there was
 * none).  Only symlink trees are deleted here, never store data. */
static void txn_restore_profile(struct txn *t)
{
    char *pdir = l2path("var/profiles");
    DIR *d = opendir(pdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            long g = 0;
            if (!parse_gen_for_profile(e->d_name, t->profile, &g)) continue;
            if (t->have_gen && g <= t->gen) continue;
            char *p = fmt("%s/%s", pdir, e->d_name);
            rm_rf(p);
            free(p);
        }
        closedir(d);
    }
    if (t->have_gen) {
        char *rel = fmt("%s-%ld-link", t->profile, t->gen);
        switch_profile_symlink(pdir, t->profile, rel);
        free(rel);
    } else {
        char *cur = fmt("%s/%s", pdir, t->profile);
        struct stat st;
        if (lstat(cur, &st) == 0 && S_ISLNK(st.st_mode)) unlink(cur);
        free(cur);
    }
    free(pdir);
}

static void txn_abort(struct txn *t)
{
    for (size_t i = t->n; i > 0; i--) {
        struct txn_saved *s = &t->saved[i - 1];
        char *dbp = l2path("var/db/installed/%s", s->name);
        char *mpp = l2path("var/db/installed/%s.manifest", s->name);
        if (s->db) write_file_atomic(dbp, s->db, strlen(s->db));
        else unlink(dbp);
        if (s->manifest) write_file_atomic(mpp, s->manifest, strlen(s->manifest));
        else unlink(mpp);
        free(dbp); free(mpp);
    }
    txn_restore_profile(t);
    if (t->n > 0)
        fprintf(stderr, "l2pm: transaction failed; restored profile %s and "
                        "database to pre-operation state\n", t->profile);
    txn_free(t);
}

static void txn_commit(struct txn *t)
{
    txn_free(t);
}

/* ------------------------------------------------------------- commands */

/* ------------------------------------------- remote recipe repository */

/* Trust store: sha256 of every recipe the user explicitly trusted.  A
 * fetched recipe executes arbitrary shell, so a new (or changed) remote
 * recipe is shown and must be confirmed before it runs; local recipes
 * (L2PM_RECIPES, CWD) are the user's own files and need no gate. */
static int recipe_trusted(const char *hash)
{
    char *p = l2path("var/trusted-recipes");
    char *txt = read_small(p);
    free(p);
    if (!txt) return 0;
    int found = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, hash, 64) && (line[64] == ' ' || line[64] == 0)) { found = 1; break; }
    }
    free(txt);
    return found;
}

static void recipe_trust_add(const char *hash, const char *path)
{
    char *p = l2path("var/trusted-recipes");
    char *vdir = l2path("var");
    char *line = fmt("%s  %s\n", hash, path);
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        if (write_all(fd, line, strlen(line)) != 0 || fsync(fd) != 0)
            fprintf(stderr, "l2pm: warning: failed to sync trusted recipe: %s\n", strerror(errno));
        close(fd);
        fsync_dir(vdir);
    } else {
        fprintf(stderr, "l2pm: warning: cannot record trusted recipe: %s\n", strerror(errno));
    }
    free(line);
    free(vdir);
    free(p);
}

/* Print a remote recipe safely to stderr: escape NUL, ESC, CR, and any other
 * control/non-printable byte as \xNN so a hostile recipe cannot hide commands
 * via terminal escape sequences or embedded NUL truncation. */
static void print_recipe_escaped(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    fprintf(stderr, "---- %s ----\n", path);
    unsigned char buf[4096];
    size_t total = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        for (ssize_t i = 0; i < r && total < READ_SMALL_MAX; i++, total++) {
            unsigned char c = buf[i];
            if ((c >= 0x20 && c <= 0x7e) || c == '\n' || c == '\t')
                fputc(c, stderr);
            else
                fprintf(stderr, "\\x%02x", c);
        }
        if (total >= READ_SMALL_MAX) break;
    }
    close(fd);
    fprintf(stderr, "\n---- end ----\n");
}

static int trust_gate(const char *path, int remote, int trust_flag)
{
    if (!remote) return 0;
    char hash[65];
    if (hash_file(path, hash) != 0) return -1;
    if (recipe_trusted(hash)) return 0;
    if (trust_flag) {
        recipe_trust_add(hash, path);
        fprintf(stderr, "l2pm: trusted recipe recorded (--trust): %s\n", path);
        return 0;
    }
    fprintf(stderr, "l2pm: recipe fetched from a remote repository; it will run shell commands:\n");
    print_recipe_escaped(path);
    if (!isatty(0)) {
        fprintf(stderr, "l2pm: refusing to execute an untrusted remote recipe; "
                        "review it and re-run with --trust\n");
        return -1;
    }
    fprintf(stderr, "l2pm: trust and execute this recipe? [y/N] ");
    fflush(stderr);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return -1;
    char *a = trim(buf);
    if (!strcmp(a, "y") || !strcmp(a, "yes")) {
        recipe_trust_add(hash, path);
        return 0;
    }
    fprintf(stderr, "l2pm: recipe not trusted; aborting\n");
    return -1;
}

static int is_git_repo_url(const char *s)
{
    return !strncmp(s, "git://", 6) || !strncmp(s, "git@", 4) ||
           !strncmp(s, "ssh://", 6) ||
           (strlen(s) > 4 && !strcmp(s + strlen(s) - 4, ".git"));
}

/* Fetch <name>.recipe from L2PM_RECIPE_REPO into the local cache.
 * Two transports, auto-detected:
 *   - git URL: clone/pull <root>/l2/var/recipe-repo, optionally pinned by
 *     L2PM_RECIPE_REPO_REF=<commit>; the cached clone works offline.
 *   - any other URL: raw base, download <base>/<name>.recipe (curl/wget);
 *     the cached copy works offline.  Trust-on-first-use pins the exact
 *     content via the trust store.
 * Returns a malloc'd cache path, or NULL when unavailable. */
static char *recipe_repo_fetch(const char *name)
{
    const char *base = getenv("L2PM_RECIPE_REPO");
    if (!base || !*base || !valid_name(name)) return NULL;
    char *cachedir = l2path("cache/recipes");
    mkdir_p(cachedir, 0755);
    char *dest = fmt("%s/%s.recipe", cachedir, name);
    free(cachedir);

    if (is_git_repo_url(base)) {
        if (!valid_git_url(base)) {
            fprintf(stderr, "l2pm: unsafe L2PM_RECIPE_REPO URL: %s\n", base);
            free(dest);
            return NULL;
        }
        if (!git_available()) {
            fprintf(stderr, "l2pm: git is required for a git recipe repository (l2 deps install)\n");
            free(dest);
            return NULL;
        }
        char *repo = l2path("var/recipe-repo");
        struct stat st;
        if (stat(repo, &st) != 0) {
            char *qb = shq(base), *qr = shq(repo);
            char *cmd = fmt("git -c protocol.ext.allow=never clone --quiet -- %s %s", qb, qr);
            int rc = run_shell(cmd, NULL, 0);
            free(cmd); free(qb); free(qr);
            if (rc != 0) {
                fprintf(stderr, "l2pm: cannot clone recipe repository %s\n", base);
                free(repo); free(dest);
                return NULL;
            }
        } else if (run_shell("git -c protocol.ext.allow=never fetch --quiet origin", repo, 1) != 0) {
            fprintf(stderr, "l2pm: recipe repo fetch failed; using the cached clone (offline?)\n");
        }
        const char *ref = getenv("L2PM_RECIPE_REPO_REF");
        if (ref && *ref) {
            if (!valid_ref(ref)) {
                fprintf(stderr, "l2pm: bad L2PM_RECIPE_REPO_REF: %s\n", ref);
                free(repo); free(dest);
                return NULL;
            }
            char *qref = shq(ref);
            char *cmd = fmt("git checkout --quiet --detach %s --", qref);
            int rc = run_shell(cmd, repo, 0);
            free(cmd); free(qref);
            if (rc != 0) {
                fprintf(stderr, "l2pm: recipe repo checkout of '%s' failed\n", ref);
                free(repo); free(dest);
                return NULL;
            }
        } else {
            fprintf(stderr, "l2pm: warning: recipe repo not pinned; set L2PM_RECIPE_REPO_REF=<commit> for reproducibility\n");
            /* Follow the remote default branch tip (fetch only moves the
             * remote-tracking refs, not the working tree).  Offline this
             * stays at the last fetched tip, which is what we want. */
            if (run_shell("git checkout --quiet --detach origin/HEAD --", repo, 1) != 0)
                fprintf(stderr, "l2pm: warning: cannot update recipe repo tip; using the cached checkout\n");
        }
        char *src = fmt("%s/%s.recipe", repo, name);
        int have = access(src, R_OK) == 0;
        if (have) have = copy_tree(src, dest) == 0;
        free(src);
        free(repo);
        if (have) return dest;
        free(dest);
        return NULL;
    }

    /* raw base URL: allow only http://, https://, or file:// */
    if (!valid_url_chars(base) ||
        (strncmp(base, "https://", 8) != 0 &&
         strncmp(base, "http://", 7) != 0 &&
         strncmp(base, "file://", 7) != 0)) {
        fprintf(stderr, "l2pm: unsafe L2PM_RECIPE_REPO URL: %s\n", base);
        free(dest);
        return NULL;
    }
    const char *proto = !strncmp(base, "file://", 7) ? "=file" : "=http,https";
    char *url = fmt("%s/%s.recipe", base, name);
    char *part = fmt("%s.part-%ld", dest, (long)getpid());
    char *q = shq(url), *qp = shq(part);
    char *cmd = fmt("curl -fsSL --proto %s --proto-redir =http,https --connect-timeout 20 -o %s -- %s || "
                    "wget -q -O %s -- %s", proto, qp, q, qp, q);
    int rc = run_shell(cmd, NULL, 1);
    free(cmd); free(q); free(qp);
    if (rc == 0) {
        int pfd = open(part, O_RDONLY | O_CLOEXEC);
        if (pfd >= 0) { fsync(pfd); close(pfd); }
    }
    if (rc == 0 && rename(part, dest) == 0) {
        char *cdir = l2path("cache/recipes");
        fsync_dir(cdir);
        free(cdir);
        free(part); free(url);
        return dest;
    }
    unlink(part);
    free(part); free(url);
    if (access(dest, R_OK) == 0) {
        fprintf(stderr, "l2pm: recipe download failed; using the cached copy (offline?)\n");
        return dest;
    }
    free(dest);
    return NULL;
}

/* Resolve a recipe argument.  Local locations always win; a bare name that
 * is not found locally is looked up in `from_dir` (for sibling dependencies)
 * and then in L2PM_RECIPE_REPO (if set), setting *remote for trust gating.
 * local_only skips the remote repository (no network side effects). */
static char *resolve_recipe_from(const char *arg, const char *from_dir, int *remote,
                                 int local_only)
{
    struct stat st;
    *remote = 0;
    /* An explicit path or a *.recipe file is used as-is.  A bare name is a
     * recipe NAME: it must not be shadowed by an unrelated file in the CWD
     * (e.g. the l2fetch script next to the recipes). */
    size_t al = strlen(arg);
    int is_path = strchr(arg, '/') != NULL ||
                  (al >= 7 && !strcmp(arg + al - 7, ".recipe"));
    if (is_path && stat(arg, &st) == 0 && S_ISREG(st.st_mode)) return xstrdup(arg);
    const char *env = getenv("L2PM_RECIPES");
    char *p;
    if (env && *env) {
        p = fmt("%s/%s", env, arg);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return p;
        free(p);
        p = fmt("%s/%s.recipe", env, arg);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return p;
        free(p);
    }
    p = l2path("recipes/%s", arg);
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return p;
    free(p);
    p = l2path("recipes/%s.recipe", arg);
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return p;
    free(p);
    p = fmt("%s.recipe", arg);
    if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return p;
    free(p);
    if (from_dir && *from_dir && !is_path) {
        p = fmt("%s/%s.recipe", from_dir, arg);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) return p;
        free(p);
    }
    if (!local_only) {
        p = recipe_repo_fetch(arg);
        if (p) { *remote = 1; return p; }
    }
    return NULL;
}

static char *resolve_recipe(const char *arg, int *remote)
{
    return resolve_recipe_from(arg, NULL, remote, 0);
}

static int cmd_build(int argc, char **argv)
{
    const char *arg = NULL, *out = NULL;
    int check = 0, trust = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--trust")) trust = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) out = argv[++i];
        else if (!arg) arg = argv[i];
        else die("build: unexpected argument %s", argv[i]);
    }
    if (!arg) die("build: missing recipe");
    int remote = 0;
    char *recipe_path = resolve_recipe(arg, &remote);
    if (!recipe_path) die("build: recipe not found: %s", arg);
    struct recipe r;
    if (parse_recipe(recipe_path, &r) != 0) { free(recipe_path); return 1; }
    ensure_layout();   /* trust store lives under var/ */
    if (trust_gate(recipe_path, remote, trust) != 0) { free(recipe_path); return 1; }
    free(recipe_path);

    char *tar1;
    if (out) tar1 = xstrdup(out);
    else {
        char *cache = l2path("cache");
        tar1 = fmt("%s/%s-%s.l2p", cache, r.name, r.version);
        free(cache);
    }
    if (build_once(&r, tar1) != 0) { free(tar1); return 1; }
    char h1[65];
    if (hash_file(tar1, h1) != 0) { free(tar1); return 1; }
    if (check) {
        char *tar2 = fmt("%s.check.%ld", tar1, (long)getpid());
        if (build_once(&r, tar2) != 0) { unlink(tar2); free(tar2); free(tar1); return 1; }
        char h2[65];
        if (hash_file(tar2, h2) != 0) { unlink(tar2); free(tar2); free(tar1); return 1; }
        unlink(tar2);
        free(tar2);
        if (strcmp(h1, h2) != 0) {
            fprintf(stderr, "l2pm: NOT REPRODUCIBLE\n  %s\n  %s\n", h1, h2);
            free(tar1);
            return 1;
        }
        printf("reproducible: yes\n");
    }
    struct stat st;
    stat(tar1, &st);
    printf("package: %s\nsha256: %s\nsize: %lld\n", tar1, h1, (long long)st.st_size);
    free(tar1);
    return 0;
}

static int install_tar(const char *file, const char *profile, struct txn *t)
{
    ensure_layout();
    /* Hash the whole archive before anything is extracted. */
    char hash[65];
    if (hash_file(file, hash) != 0) { fprintf(stderr, "l2pm: cannot hash %s\n", file); return 1; }
    char *tmp = l2path("tmp/install-XXXXXX");
    if (!mkdtemp(tmp)) {
        fprintf(stderr, "l2pm: cannot create install dir: %s\n", strerror(errno));
        free(tmp); return 1;
    }
    if (tar_extract(file, tmp) != 0) { rm_rf(tmp); free(tmp); return 1; }
    char name[128], ver[128], deps[1024];
    if (read_info(tmp, name, sizeof(name), ver, sizeof(ver), deps, sizeof(deps)) != 0) {
        fprintf(stderr, "l2pm: package has no valid .l2pm/info\n");
        rm_rf(tmp); free(tmp); return 1;
    }
    char *store = store_path_for(hash, name, ver);
    struct stat st;
    if (lstat(store, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "l2pm: store path exists but is not a directory: %s\n", store);
            rm_rf(tmp); free(tmp); free(store); return 1;
        }
        /* A matching directory name alone cannot vouch for cached bytes. */
        struct pathlist archive = {0}, cached = {0};
        int match = manifest_collect(tmp, &archive) == 0 &&
                    manifest_collect(store, &cached) == 0 && archive.n == cached.n;
        for (size_t i = 0; match && i < archive.n; i++)
            match = strcmp(archive.v[i], cached.v[i]) == 0;
        pl_free(&archive); pl_free(&cached);
        rm_rf(tmp);
        if (!match) {
            fprintf(stderr, "l2pm: cached store differs from archive: %s\n", store);
            free(tmp); free(store); return 1;
        }
    } else {
        /* Extract to a sibling temp and rename, so a failed extraction
         * never leaves a half package in the store.  Both paths live under
         * the same L2 root, so rename is atomic there; fsync makes it
         * durable against power loss before the DB entry points at it. */
        char *tmp2 = fmt("%s.tmp-%ld", store, (long)getpid());
        rm_rf(tmp2);
        if (fsync_tree(tmp) != 0 || rename(tmp, tmp2) != 0 || rename(tmp2, store) != 0) {
            rm_rf(tmp2);
            rm_rf(tmp);
            free(tmp2); free(tmp); free(store);
            return 1;
        }
        free(tmp2);
        char *sdir = l2path("store");
        char *tdir = l2path("tmp");
        int frc = fsync_dir(sdir);
        if (fsync_dir(tdir) != 0 && frc == 0) frc = -1;
        free(sdir); free(tdir);
        if (frc != 0) { free(tmp); free(store); return 1; }
    }
    free(tmp);
    struct pathlist decl_deps = {0};
    if (parse_deps_list(deps, &decl_deps) == 0) {
        for (size_t i = 0; i < decl_deps.n; i++) {
            if (strcmp(decl_deps.v[i], name) != 0 && !is_installed(decl_deps.v[i])) {
                fprintf(stderr, "l2pm: warning: missing dependency '%s' for %s\n",
                        decl_deps.v[i], name);
            }
        }
    }
    pl_free(&decl_deps);
    if (txn_record(t, name) != 0) { free(store); return 1; }
    if (db_write(name, ver, hash, store, deps) != 0) { free(store); return 1; }
    if (manifest_write(name, store) != 0) { free(store); return 1; }
    if (rebuild_profile(profile) != 0) { free(store); return 1; }
    printf("installed %s %s -> %s\n", name, ver, store);
    free(store);
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    const char *profile = "default";
    struct pathlist files = {0};
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else pl_add(&files, argv[i]);
    }
    if (!valid_name(profile)) die("install: bad profile name");
    if (!files.n) die("install: missing package");
    int lfd = l2pm_lock(1);
    if (lfd < 0) { pl_free(&files); return 1; }
    struct txn t;
    txn_begin(&t, profile);
    int rc = 0;
    for (size_t i = 0; i < files.n; i++) {
        if (install_tar(files.v[i], profile, &t) != 0) { rc = 1; break; }
    }
    if (rc != 0) txn_abort(&t);
    else txn_commit(&t);
    l2pm_unlock(lfd);
    pl_free(&files);
    return rc;
}

static int add_recipe_rec(const char *arg, const char *from_dir, const char *profile,
                          int trust, int no_deps, struct pathlist *stack, int depth,
                          struct txn *t)
{
    if (depth > 32) {
        fprintf(stderr, "l2pm: dependency depth limit exceeded at %s\n", arg);
        return 1;
    }
    int remote = 0;
    char *recipe_path = resolve_recipe_from(arg, from_dir, &remote, 0);
    if (!recipe_path) {
        fprintf(stderr, "l2pm: add: recipe not found: %s\n", arg);
        return 1;
    }
    struct recipe r;
    if (parse_recipe(recipe_path, &r) != 0) { free(recipe_path); return 1; }
    if (pl_has(stack, r.name)) {
        fprintf(stderr, "l2pm: dependency cycle detected: %s\n", r.name);
        free(recipe_path);
        return 1;
    }
    ensure_layout();   /* trust store lives under var/ */
    if (trust_gate(recipe_path, remote, trust) != 0) { free(recipe_path); return 1; }
    free(recipe_path);

    pl_add(stack, r.name);
    if (!no_deps && r.deps[0]) {
        struct pathlist deps = {0};
        if (parse_deps_list(r.deps, &deps) != 0) {
            pl_free(&deps);
            free(stack->v[--stack->n]);
            return 1;
        }
        for (size_t i = 0; i < deps.n; i++) {
            const char *dep = deps.v[i];
            if (pl_has(stack, dep)) {
                fprintf(stderr, "l2pm: dependency cycle detected: %s\n", dep);
                pl_free(&deps);
                free(stack->v[--stack->n]);
                return 1;
            }
            if (!is_installed(dep)) {
                if (add_recipe_rec(dep, remote ? NULL : r.base, profile, trust, no_deps, stack, depth + 1, t) != 0) {
                    pl_free(&deps);
                    free(stack->v[--stack->n]);
                    return 1;
                }
            } else {
                /* Dependency semantics: deps are NAME-ONLY (no version
                 * constraints).  An installed package satisfies the
                 * dependency as-is and is never implicitly upgraded or
                 * downgraded.  Warn when a locally available recipe
                 * advertises a different version (no remote fetch here). */
                char *iv = db_get(dep, "version");
                int dremote = 0;
                char *rp = resolve_recipe_from(dep, remote ? NULL : r.base, &dremote, 1);
                if (rp) {
                    struct recipe dr;
                    if (parse_recipe(rp, &dr) == 0 && iv && strcmp(dr.version, iv) != 0)
                        fprintf(stderr, "l2pm: warning: dependency '%s' installed at "
                                        "version %s; recipe provides %s "
                                        "(keeping installed version)\n",
                                dep, iv, dr.version);
                    free(rp);
                }
                free(iv);
            }
        }
        pl_free(&deps);
    }

    char *cache = l2path("cache");
    char *tar = fmt("%s/%s-%s.l2p", cache, r.name, r.version);
    free(cache);
    int rc = 0;
    if (build_once(&r, tar) != 0) rc = 1;
    if (rc == 0 && install_tar(tar, profile, t) != 0) rc = 1;
    free(tar);
    free(stack->v[--stack->n]);
    return rc;
}

static int cmd_add(int argc, char **argv)
{
    const char *arg = NULL, *profile = "default";
    int trust = 0, no_deps = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--trust")) trust = 1;
        else if (!strcmp(argv[i], "--no-deps")) no_deps = 1;
        else if (!arg) arg = argv[i];
        else die("add: unexpected argument %s", argv[i]);
    }
    if (!valid_name(profile)) die("add: bad profile name");
    if (!arg) die("add: missing recipe");
    int lfd = l2pm_lock(1);
    if (lfd < 0) return 1;
    struct pathlist stack = {0};
    struct txn t;
    txn_begin(&t, profile);
    int rc = add_recipe_rec(arg, NULL, profile, trust, no_deps, &stack, 0, &t);
    if (rc != 0) txn_abort(&t);
    else txn_commit(&t);
    l2pm_unlock(lfd);
    pl_free(&stack);
    return rc;
}

static int cmd_remove(int argc, char **argv)
{
    const char *profile = "default";
    int force = 0;
    struct pathlist names = {0};
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!pl_has(&names, argv[i])) pl_add(&names, argv[i]);
    }
    if (!valid_name(profile)) die("remove: bad profile name");
    if (!names.n) die("remove: missing package");
    for (size_t i = 0; i < names.n; i++)
        if (!valid_name(names.v[i])) die("remove: bad package name");
    int lfd = l2pm_lock(1);
    if (lfd < 0) { pl_free(&names); return 1; }
    /* All existence and reverse-dependency checks run before any mutation.
     * Names removed together satisfy each other's dependencies. */
    for (size_t i = 0; i < names.n; i++) {
        char *db = l2path("var/db/installed/%s", names.v[i]);
        int there = access(db, F_OK) == 0;
        free(db);
        if (!there) {
            fprintf(stderr, "l2pm: %s is not installed\n", names.v[i]);
            l2pm_unlock(lfd);
            pl_free(&names);
            return 1;
        }
    }
    if (!force) {
        size_t n = 0;
        char **dbn = db_names(&n);
        int blocked = 0;
        for (size_t i = 0; i < names.n && !blocked; i++) {
            struct pathlist rdeps = {0};
            for (size_t j = 0; j < n; j++) {
                if (pl_has(&names, dbn[j])) continue;
                char *dstr = db_get(dbn[j], "deps");
                struct pathlist parsed = {0};
                if (dstr && parse_deps_list(dstr, &parsed) == 0 && pl_has(&parsed, names.v[i]))
                    pl_add(&rdeps, dbn[j]);
                pl_free(&parsed);
                free(dstr);
            }
            if (rdeps.n > 0) {
                fprintf(stderr, "l2pm: cannot remove %s; required by:", names.v[i]);
                for (size_t k = 0; k < rdeps.n; k++) fprintf(stderr, " %s", rdeps.v[k]);
                fprintf(stderr, " (use --force to override)\n");
                blocked = 1;
            }
            pl_free(&rdeps);
        }
        for (size_t j = 0; j < n; j++) free(dbn[j]);
        free(dbn);
        if (blocked) {
            l2pm_unlock(lfd);
            pl_free(&names);
            return 1;
        }
    }
    struct txn t;
    txn_begin(&t, profile);
    int rc = 0;
    for (size_t i = 0; i < names.n; i++) {
        if (txn_record(&t, names.v[i]) != 0) { rc = 1; break; }
        char *db = l2path("var/db/installed/%s", names.v[i]);
        char *mp = l2path("var/db/installed/%s.manifest", names.v[i]);
        if (unlink(db) != 0) rc = 1;
        unlink(mp);
        free(db); free(mp);
        if (rc != 0) break;
    }
    if (rc == 0 && rebuild_profile(profile) != 0) rc = 1;
    if (rc != 0) {
        txn_abort(&t);
    } else {
        txn_commit(&t);
        for (size_t i = 0; i < names.n; i++) printf("removed %s\n", names.v[i]);
    }
    l2pm_unlock(lfd);
    pl_free(&names);
    return rc;
}

static int cmd_list(void)
{
    size_t n = 0;
    char **names = db_names(&n);
    for (size_t i = 0; i < n; i++) {
        char *ver = db_get(names[i], "version");
        char *hash = db_get(names[i], "hash");
        printf("%s\t%s\t%s\n", names[i], ver ? ver : "?", hash ? hash : "?");
        free(ver); free(hash); free(names[i]);
    }
    free(names);
    if (!n) fprintf(stderr, "(no packages installed)\n");
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    if (argc < 1) die("info: missing package");
    if (!valid_name(argv[0])) die("info: bad package name");
    char *p = l2path("var/db/installed/%s", argv[0]);
    char *txt = read_small(p);
    free(p);
    if (!txt) die("info: %s is not installed", argv[0]);
    fputs(txt, stdout);
    free(txt);
    return 0;
}

/* Returns 0 when the store matches the manifest, 1 when the manifest file
 * is missing (never a silent pass: the caller must warn/repair or fail),
 * -1 on mismatch or unreadable data (reason is set). */
static int verify_manifest(const char *name, const char *store, char *reason, size_t rcap)
{
    char *mp = l2path("var/db/installed/%s.manifest", name);
    char *txt = read_small(mp);
    if (!txt) {
        int missing = access(mp, F_OK) != 0;
        free(mp);
        if (missing) return 1;
        snprintf(reason, rcap, "unreadable manifest");
        return -1;
    }
    free(mp);
    struct pathlist exp = {0};
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (*line) pl_add(&exp, line);
    }
    free(txt);

    struct pathlist act = {0};
    if (manifest_collect(store, &act) != 0) {
        snprintf(reason, rcap, "unreadable store tree");
        pl_free(&exp);
        return -1;
    }
    size_t min_n = exp.n < act.n ? exp.n : act.n;
    for (size_t i = 0; i < min_n; i++) {
        if (strcmp(exp.v[i], act.v[i]) != 0) {
            snprintf(reason, rcap, "modified or missing entry");
            pl_free(&exp);
            pl_free(&act);
            return -1;
        }
    }
    if (exp.n != act.n) {
        snprintf(reason, rcap, act.n > exp.n ? "unexpected entry in store" : "missing entry in store");
        pl_free(&exp);
        pl_free(&act);
        return -1;
    }
    pl_free(&exp);
    pl_free(&act);
    return 0;
}

/* cmd_verify is defined after cmd_rollback: --repair reuses the rollback
 * DB re-sync (sync_db_to_gen) and the generation metadata readers below. */

struct gen_pkg {
    char *name;
    char *version;
    char *hash;
    char *store;
    char *deps;
};

static void free_gen_pkgs(struct gen_pkg *pkgs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(pkgs[i].name);
        free(pkgs[i].version);
        free(pkgs[i].hash);
        free(pkgs[i].store);
        free(pkgs[i].deps);
    }
    free(pkgs);
}

static int read_gen_file(const char *gen_dir, struct gen_pkg **out_pkgs, size_t *out_n)
{
    char *gp = fmt("%s/.l2pm-gen", gen_dir);
    char *txt = read_small(gp);
    free(gp);
    if (!txt) return -1;
    struct gen_pkg *v = NULL;
    size_t n = 0, cap = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line) continue;
        char *t1 = strchr(line, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (!t1 || !t2 || !t3) {
            free(txt);
            free_gen_pkgs(v, n);
            return -1;
        }
        *t1++ = 0;
        *t2++ = 0;
        *t3++ = 0;
        char *t4 = strchr(t3, '\t');
        const char *deps = "";
        if (t4) { *t4++ = 0; deps = t4; }
        if (n == cap) { cap = cap ? cap * 2 : 16; v = xrealloc(v, cap * sizeof(*v)); }
        v[n].name = xstrdup(line);
        v[n].version = xstrdup(t1);
        v[n].hash = xstrdup(t2);
        v[n].store = xstrdup(t3);
        v[n].deps = xstrdup(deps);
        n++;
    }
    free(txt);
    *out_pkgs = v;
    *out_n = n;
    return 0;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static int read_active_gen(const char *pdir, const char *profile, char *target, size_t cap, long *out_gen)
{
    char *cur = fmt("%s/%s", pdir, profile);
    ssize_t r = readlink(cur, target, cap - 1);
    free(cur);
    if (r < 0) { target[0] = 0; return -1; }
    target[r] = 0;
    return parse_gen_for_profile(target, profile, out_gen) ? 0 : -1;
}

static int cmd_generations(int argc, char **argv)
{
    const char *profile = "default";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else die("generations: unexpected argument %s", argv[i]);
    }
    if (!valid_name(profile)) die("generations: bad profile name");
    char *pdir = l2path("var/profiles");
    char cur_target[MAXPATH] = "";
    long cur_gen = -1;
    read_active_gen(pdir, profile, cur_target, sizeof(cur_target), &cur_gen);

    DIR *d = opendir(pdir);
    long *gens = NULL;
    size_t n = 0, cap = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            long g = 0;
            if (parse_gen_for_profile(e->d_name, profile, &g)) {
                if (n == cap) { cap = cap ? cap * 2 : 16; gens = xrealloc(gens, cap * sizeof(*gens)); }
                gens[n++] = g;
            }
        }
        closedir(d);
    }
    if (n) qsort(gens, n, sizeof(*gens), cmp_long);
    for (size_t i = 0; i < n; i++) {
        char *gdir = fmt("%s/%s-%ld-link", pdir, profile, gens[i]);
        struct gen_pkg *pkgs = NULL;
        size_t pn = 0;
        if (read_gen_file(gdir, &pkgs, &pn) == 0) free_gen_pkgs(pkgs, pn);
        free(gdir);
        char rel[256];
        snprintf(rel, sizeof(rel), "%s-%ld-link", profile, gens[i]);
        printf("%ld\t%zu package(s)%s\n", gens[i], pn,
               !strcmp(cur_target, rel) ? "\t(current)" : "");
    }
    if (!n) fprintf(stderr, "(no generations for profile %s)\n", profile);
    free(gens);
    free(pdir);
    return 0;
}

/* Synchronize var/db/installed with a generation's package list.  Shared
 * by rollback and by `verify --repair` completing an interrupted rollback
 * (the generation metadata is the snapshot; no separate journal format). */
static int sync_db_to_gen(struct gen_pkg *pkgs, size_t pn)
{
    size_t cur_n = 0;
    char **cur_names = db_names(&cur_n);
    for (size_t i = 0; i < cur_n; i++) {
        int keep = 0;
        for (size_t j = 0; j < pn; j++)
            if (!strcmp(cur_names[i], pkgs[j].name)) { keep = 1; break; }
        if (!keep) {
            char *db = l2path("var/db/installed/%s", cur_names[i]);
            char *mp = l2path("var/db/installed/%s.manifest", cur_names[i]);
            unlink(db);
            unlink(mp);
            free(db);
            free(mp);
        }
        free(cur_names[i]);
    }
    free(cur_names);
    for (size_t j = 0; j < pn; j++) {
        if (db_write(pkgs[j].name, pkgs[j].version, pkgs[j].hash, pkgs[j].store, pkgs[j].deps) != 0 ||
            manifest_write(pkgs[j].name, pkgs[j].store) != 0)
            return -1;
    }
    return 0;
}

static int cmd_rollback(int argc, char **argv)
{
    const char *profile = "default", *gen_arg = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!gen_arg) gen_arg = argv[i];
        else die("rollback: unexpected argument %s", argv[i]);
    }
    if (!valid_name(profile)) die("rollback: bad profile name");
    int lfd = l2pm_lock(1);
    if (lfd < 0) return 1;

    char *pdir = l2path("var/profiles");
    char cur_target[MAXPATH] = "";
    long cur_gen = -1;
    if (read_active_gen(pdir, profile, cur_target, sizeof(cur_target), &cur_gen) != 0) {
        free(pdir);
        die("rollback: no active generation for profile %s", profile);
    }

    long target_gen = -1;
    if (gen_arg) {
        if (!*gen_arg) { free(pdir); die("rollback: invalid generation"); }
        for (const char *p = gen_arg; *p; p++)
            if (!isdigit((unsigned char)*p)) { free(pdir); die("rollback: invalid generation '%s'", gen_arg); }
        errno = 0;
        target_gen = strtol(gen_arg, NULL, 10);
        if (errno == ERANGE || target_gen < 0) { free(pdir); die("rollback: invalid generation '%s'", gen_arg); }
    } else {
        DIR *d = opendir(pdir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                long g = 0;
                if (parse_gen_for_profile(e->d_name, profile, &g) && g < cur_gen && g > target_gen)
                    target_gen = g;
            }
            closedir(d);
        }
        if (target_gen < 0) {
            free(pdir);
            die("rollback: no previous generation for profile %s", profile);
        }
    }

    char *target_rel = fmt("%s-%ld-link", profile, target_gen);
    char *target_dir = fmt("%s/%s", pdir, target_rel);
    if (!is_dir(target_dir)) {
        free(target_dir); free(target_rel); free(pdir);
        die("rollback: generation %ld does not exist for profile %s", target_gen, profile);
    }

    struct gen_pkg *pkgs = NULL;
    size_t pn = 0;
    if (read_gen_file(target_dir, &pkgs, &pn) != 0) {
        free(target_dir); free(target_rel); free(pdir);
        die("rollback: generation %ld has no valid metadata", target_gen);
    }
    for (size_t i = 0; i < pn; i++) {
        if (!valid_name(pkgs[i].name) || !valid_name(pkgs[i].version) || !is_dir(pkgs[i].store)) {
            fprintf(stderr, "l2pm: rollback: store path missing for %s (%s); run gc with --keep-generations to preserve old generations\n",
                    pkgs[i].name, pkgs[i].store);
            free_gen_pkgs(pkgs, pn);
            free(target_dir); free(target_rel); free(pdir);
            l2pm_unlock(lfd);
            return 1;
        }
    }
    free(target_dir);

    /* Intent marker, written before any mutation: a crash between the
     * symlink switch and the DB re-sync leaves a torn state that
     * `verify --repair` completes from this marker.  Removed on success. */
    char *intent = l2path("var/rollback-intent");
    char ibuf[256];
    int in = snprintf(ibuf, sizeof(ibuf), "profile=%s\ngen=%ld\n", profile, target_gen);
    if (in < 0 || (size_t)in >= sizeof(ibuf) ||
        write_file_atomic(intent, ibuf, (size_t)in) != 0) {
        free(intent);
        free_gen_pkgs(pkgs, pn);
        free(target_rel); free(pdir);
        die("rollback: cannot write rollback intent marker");
    }
    free(intent);

    if (switch_profile_symlink(pdir, profile, target_rel) != 0) {
        free_gen_pkgs(pkgs, pn);
        free(target_rel); free(pdir);
        die("rollback: failed to switch profile symlink");
    }
    free(target_rel);
    free(pdir);

    /* Synchronize var/db/installed with the rolled-back generation.  On
     * failure the intent marker stays so `verify --repair` can finish. */
    if (sync_db_to_gen(pkgs, pn) != 0) {
        free_gen_pkgs(pkgs, pn);
        die("rollback: failed to update package database");
    }
    free_gen_pkgs(pkgs, pn);
    char *intent_done = l2path("var/rollback-intent");
    unlink(intent_done);
    free(intent_done);
    l2pm_unlock(lfd);
    printf("rolled back %s to generation %ld\n", profile, target_gen);
    return 0;
}

/* DB snapshot in generation-file format (db_names is sorted, matching the
 * order rebuild_profile writes into .l2pm-gen). */
static void db_snapshot_lines(struct pathlist *out)
{
    size_t n = 0;
    char **names = db_names(&n);
    for (size_t i = 0; i < n; i++) {
        char *ver = db_get(names[i], "version");
        char *hash = db_get(names[i], "hash");
        char *store = db_get(names[i], "store");
        char *deps = db_get(names[i], "deps");
        char *ln = fmt("%s\t%s\t%s\t%s\t%s", names[i], ver ? ver : "",
                       hash ? hash : "", store ? store : "", deps ? deps : "");
        pl_add(out, ln);
        free(ln);
        free(ver); free(hash); free(store); free(deps);
        free(names[i]);
    }
    free(names);
}

/* verify: per-package store integrity plus DB/profile divergence.
 * --repair additionally: completes an interrupted rollback from its intent
 * marker, reconstructs missing/corrupt DB entries from the ACTIVE default
 * generation (fail-safe: never drops a package the generation still lists,
 * never adopts orphan stores), rewrites missing manifests from the store,
 * rebuilds diverged profiles from the DB, and removes stale staging
 * artifacts.  Store contents are NEVER modified or deleted here. */
static int cmd_verify(int argc, char **argv)
{
    int repair = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--repair")) repair = 1;
        else die("verify: unexpected argument %s", argv[i]);
    }
    int lfd = l2pm_lock(repair);
    if (lfd == -1) return 1;

    char *pdir = l2path("var/profiles");
    struct pathlist profiles = {0};
    pl_add(&profiles, "default");
    DIR *pd = opendir(pdir);
    if (pd) {
        struct dirent *e;
        while ((e = readdir(pd))) {
            if (e->d_name[0] == '.') continue;
            char *prof = NULL;
            long g = 0;
            if (parse_any_gen_link(e->d_name, &prof, &g)) {
                if (!pl_has(&profiles, prof)) pl_add(&profiles, prof);
                free(prof);
                continue;
            }
            char *full = fmt("%s/%s", pdir, e->d_name);
            char tgt[MAXPATH];
            ssize_t r = readlink(full, tgt, sizeof(tgt) - 1);
            free(full);
            if (r > 0) {
                tgt[r] = 0;
                long gg = 0;
                if (parse_gen_for_profile(tgt, e->d_name, &gg) && !pl_has(&profiles, e->d_name))
                    pl_add(&profiles, e->d_name);
            }
        }
        closedir(pd);
    }

    int bad = 0;
    if (repair) {
        /* 1. Complete an interrupted rollback from its intent marker. */
        char *ip = l2path("var/rollback-intent");
        char *txt = read_small(ip);
        if (txt) {
            char prof[128] = "";
            long gen = -1;
            char *save = NULL;
            for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                if (!strncmp(line, "profile=", 8)) snprintf(prof, sizeof(prof), "%s", line + 8);
                else if (!strncmp(line, "gen=", 4)) gen = strtol(line + 4, NULL, 10);
            }
            free(txt);
            int done = 0;
            if (valid_name(prof) && gen >= 0) {
                char *rel = fmt("%s-%ld-link", prof, gen);
                char *gdir = fmt("%s/%s", pdir, rel);
                struct gen_pkg *pkgs = NULL;
                size_t pn = 0;
                if (is_dir(gdir) && read_gen_file(gdir, &pkgs, &pn) == 0) {
                    int ok = 1;
                    for (size_t j = 0; j < pn; j++)
                        if (!valid_name(pkgs[j].name) || !valid_name(pkgs[j].version) ||
                            !is_dir(pkgs[j].store)) { ok = 0; break; }
                    if (ok && sync_db_to_gen(pkgs, pn) == 0) {
                        char cur[MAXPATH];
                        long cg = -1;
                        int active = read_active_gen(pdir, prof, cur, sizeof(cur), &cg) == 0 &&
                                     !strcmp(cur, rel);
                        if ((active || switch_profile_symlink(pdir, prof, rel) == 0) &&
                            unlink(ip) == 0) {
                            printf("repaired: completed interrupted rollback of %s to generation %ld\n",
                                   prof, gen);
                            done = 1;
                        }
                    }
                    free_gen_pkgs(pkgs, pn);
                }
                free(gdir); free(rel);
            }
            if (!done) {
                fprintf(stderr, "l2pm: rollback recovery incomplete; retaining intent for retry\n");
                free(ip); pl_free(&profiles); free(pdir);
                return 1;
            }
        }
        free(ip);

        /* 2. Reconstruct missing/corrupt DB entries from the active
         *    generation (default profile first, else the first profile
         *    with a readable active generation). */
        for (size_t i = 0; i < profiles.n; i++) {
            char cur[MAXPATH];
            long gen = -1;
            if (read_active_gen(pdir, profiles.v[i], cur, sizeof(cur), &gen) != 0)
                continue;
            char *gdir = fmt("%s/%s", pdir, cur);
            struct gen_pkg *pkgs = NULL;
            size_t pn = 0;
            if (read_gen_file(gdir, &pkgs, &pn) != 0) {
                free(gdir);
                continue;
            }
            for (size_t j = 0; j < pn; j++) {
                struct gen_pkg *g = &pkgs[j];
                if (!valid_name(g->name) || !valid_name(g->version) || !is_dir(g->store))
                    continue;
                char *dbnm = db_get(g->name, "name");
                char *dbstore = db_get(g->name, "store");
                char *dbver = db_get(g->name, "version");
                int valid = dbnm && !strcmp(dbnm, g->name) &&
                            dbstore && is_dir(dbstore) && dbver && valid_name(dbver);
                free(dbnm); free(dbstore); free(dbver);
                if (valid) continue;
                if (db_write(g->name, g->version, g->hash, g->store, g->deps) == 0 &&
                    manifest_write(g->name, g->store) == 0)
                    printf("repaired: reconstructed database entry for %s from generation %ld\n",
                           g->name, gen);
                else { printf("BROKEN %s: cannot reconstruct database entry\n", g->name); bad++; }
            }
            free_gen_pkgs(pkgs, pn);
            free(gdir);
            break;
        }
    }

    /* Per-package integrity (detection always runs, on the repaired DB). */
    size_t n = 0;
    char **names = db_names(&n);
    for (size_t i = 0; i < n; i++) {
        char *store = db_get(names[i], "store");
        char *ver = db_get(names[i], "version");
        if (!store || !is_dir(store)) {
            printf("BROKEN %s: store path missing\n", names[i]);
            bad++;
        } else {
            char nm[128], vv[128], dd[1024];
            char reason[128] = "";
            if (read_info(store, nm, sizeof(nm), vv, sizeof(vv), dd, sizeof(dd)) != 0 ||
                strcmp(nm, names[i]) != 0) {
                printf("BROKEN %s: metadata mismatch\n", names[i]);
                bad++;
            } else if (ver && strcmp(vv, ver) != 0) {
                printf("BROKEN %s: version mismatch\n", names[i]);
                bad++;
            } else {
                int mrc = verify_manifest(names[i], store, reason, sizeof(reason));
                if (mrc == 1) {
                    if (repair && manifest_write(names[i], store) == 0) {
                        printf("repaired: rewrote missing manifest for %s\n", names[i]);
                        printf("ok %s %s\n", names[i], vv);
                    } else {
                        printf("BROKEN %s: manifest missing\n", names[i]);
                        bad++;
                    }
                } else if (mrc != 0) {
                    printf("BROKEN %s: store integrity check failed (%s)\n", names[i], reason);
                    bad++;
                } else {
                    printf("ok %s %s\n", names[i], vv);
                }
            }
        }
        free(store); free(ver); free(names[i]);
    }
    free(names);

    /* DB/profile divergence: the profile is a derived view of the DB. */
    struct pathlist dblines = {0};
    db_snapshot_lines(&dblines);
    for (size_t i = 0; i < profiles.n; i++) {
        const char *prof = profiles.v[i];
        char cur[MAXPATH];
        long gen = -1;
        int have = read_active_gen(pdir, prof, cur, sizeof(cur), &gen) == 0;
        struct gen_pkg *pkgs = NULL;
        size_t pn = 0;
        int genok = 0;
        if (have) {
            char *gdir = fmt("%s/%s", pdir, cur);
            genok = read_gen_file(gdir, &pkgs, &pn) == 0;
            free(gdir);
        }
        int diverged = (!have && dblines.n > 0) || (have && (!genok || pn != dblines.n));
        if (!diverged && have && genok) {
            for (size_t j = 0; j < pn; j++) {
                char *ln = fmt("%s\t%s\t%s\t%s\t%s", pkgs[j].name, pkgs[j].version,
                               pkgs[j].hash, pkgs[j].store, pkgs[j].deps);
                int diff = strcmp(ln, dblines.v[j]) != 0;
                free(ln);
                if (diff) { diverged = 1; break; }
            }
        }
        if (diverged) {
            if (repair) {
                if (rebuild_profile(prof) == 0)
                    printf("repaired: rebuilt profile %s from database\n", prof);
                else { printf("BROKEN profile %s: cannot rebuild from database\n", prof); bad++; }
            } else {
                printf("BROKEN profile %s: database/profile divergence\n", prof);
                bad++;
            }
        }
        if (pkgs) free_gen_pkgs(pkgs, pn);
    }
    pl_free(&dblines);

    if (repair) {
        /* Stale staging artifacts from crashed runs (dot-prefixed temps
         * created by rebuild_profile / switch_profile_symlink). */
        DIR *sd = opendir(pdir);
        if (sd) {
            struct dirent *e;
            while ((e = readdir(sd))) {
                if (e->d_name[0] != '.') continue;
                if (!strstr(e->d_name, "-tmp-") && !strstr(e->d_name, ".link-") &&
                    !strstr(e->d_name, ".old-")) continue;
                char *full = fmt("%s/%s", pdir, e->d_name);
                rm_rf(full);
                printf("repaired: removed stale temporary artifact %s\n", e->d_name);
                free(full);
            }
            closedir(sd);
        }
        /* Unreferenced store paths: report only, never delete (gc's job). */
        struct pathlist refs = {0};
        size_t rn = 0;
        char **rnames = db_names(&rn);
        for (size_t i = 0; i < rn; i++) {
            char *s = db_get(rnames[i], "store");
            if (s && !pl_has(&refs, s)) pl_add(&refs, s);
            free(s); free(rnames[i]);
        }
        free(rnames);
        DIR *gd = opendir(pdir);
        if (gd) {
            struct dirent *e;
            while ((e = readdir(gd))) {
                char *prof = NULL;
                long g = 0;
                if (!parse_any_gen_link(e->d_name, &prof, &g)) continue;
                free(prof);
                char *gdir = fmt("%s/%s", pdir, e->d_name);
                struct gen_pkg *pkgs = NULL;
                size_t pn = 0;
                if (read_gen_file(gdir, &pkgs, &pn) == 0) {
                    for (size_t j = 0; j < pn; j++)
                        if (*pkgs[j].store && !pl_has(&refs, pkgs[j].store))
                            pl_add(&refs, pkgs[j].store);
                    free_gen_pkgs(pkgs, pn);
                }
                free(gdir);
            }
            closedir(gd);
        }
        char *sdir = l2path("store");
        DIR *s = opendir(sdir);
        int orphans = 0;
        if (s) {
            struct dirent *e;
            while ((e = readdir(s))) {
                if (e->d_name[0] == '.') continue;
                char *full = fmt("%s/%s", sdir, e->d_name);
                if (!pl_has(&refs, full)) orphans++;
                free(full);
            }
            closedir(s);
        }
        free(sdir);
        pl_free(&refs);
        if (orphans > 0)
            printf("note: %d unreferenced store path(s); run gc to reclaim\n", orphans);
    }
    pl_free(&profiles);
    free(pdir);
    l2pm_unlock(lfd);
    return bad ? 1 : 0;
}

struct gc_gen {
    char *prof;
    long gen;
    char *dir;
    char *rel;
    struct gen_pkg *pkgs;
    size_t pn;
    int keep;
};

static int cmp_gc_gen(const void *a, const void *b)
{
    const struct gc_gen *x = a, *y = b;
    int c = strcmp(x->prof, y->prof);
    if (c) return c;
    return (y->gen > x->gen) - (y->gen < x->gen); /* descending */
}

/* gc: reclaim unreferenced store paths.  FAIL-SAFE: liveness comes from
 * the DB AND from generation metadata; an unreadable DB entry or gen file
 * means unknown liveness, so gc refuses rather than risk deleting live
 * data.  By default the active generation plus the newest 2 per profile
 * are kept (rollback history survives routine gc); --keep-generations
 * keeps all, --prune-generations opts into the legacy keep-active-only
 * behavior.  Store paths referenced by any kept generation are never
 * deleted, even when the DB entry is gone (torn remove/rollback). */
static int cmd_gc(int argc, char **argv)
{
    int keep_gens = 0, prune_gens = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--keep-generations")) keep_gens = 1;
        else if (!strcmp(argv[i], "--prune-generations")) prune_gens = 1;
        else die("gc: unexpected argument %s", argv[i]);
    }
    if (keep_gens && prune_gens)
        die("gc: --keep-generations and --prune-generations are mutually exclusive");
    long keep_last = prune_gens ? 0 : 2;
    int lfd = l2pm_lock(1);
    if (lfd < 0) return 1;

    /* Fail-closed if an interrupted rollback is pending repair. */
    {
        char *intent = l2path("var/rollback-intent");
        struct stat ist;
        int has_intent = (lstat(intent, &ist) == 0);
        free(intent);
        if (has_intent) {
            fprintf(stderr, "l2pm: gc: refusing: interrupted rollback pending (run verify --repair)\n");
            l2pm_unlock(lfd);
            return 1;
        }
    }

    /* Fail-closed if the installed DB directory exists but cannot be opened. */
    {
        char *dbdir = l2path("var/db/installed");
        DIR *dbd = opendir(dbdir);
        if (!dbd && errno != ENOENT) {
            fprintf(stderr, "l2pm: gc: refusing: cannot read %s: %s\n", dbdir, strerror(errno));
            free(dbdir);
            l2pm_unlock(lfd);
            return 1;
        }
        if (dbd) closedir(dbd);
        free(dbdir);
    }

    char *pdir = l2path("var/profiles");
    struct pathlist live_stores = {0};

    size_t dbn = 0;
    char **names = db_names(&dbn);
    int refuse = 0;
    for (size_t i = 0; i < dbn; i++) {
        char *s = db_get(names[i], "store");
        if (!s) {
            fprintf(stderr, "l2pm: gc: refusing: database entry %s has no readable "
                            "store path (run verify --repair)\n", names[i]);
            refuse = 1;
        } else if (!pl_has(&live_stores, s)) {
            pl_add(&live_stores, s);
        }
        free(s);
        free(names[i]);
    }
    free(names);
    if (refuse) {
        pl_free(&live_stores);
        free(pdir);
        l2pm_unlock(lfd);
        return 1;
    }

    /* Read every generation's metadata before deleting anything. */
    struct gc_gen *gens = NULL;
    size_t gn = 0, gcap = 0;
    DIR *pd = opendir(pdir);
    if (!pd && errno != ENOENT) {
        fprintf(stderr, "l2pm: gc: refusing: cannot read %s: %s\n", pdir, strerror(errno));
        pl_free(&live_stores);
        free(pdir);
        l2pm_unlock(lfd);
        return 1;
    }
    if (pd) {
        struct dirent *e;
        while ((e = readdir(pd))) {
            if (e->d_name[0] == '.') continue;
            char *prof = NULL;
            long g = 0;
            if (!parse_any_gen_link(e->d_name, &prof, &g)) continue;
            char *gdir = fmt("%s/%s", pdir, e->d_name);
            struct gen_pkg *pkgs = NULL;
            size_t pn = 0;
            if (read_gen_file(gdir, &pkgs, &pn) != 0) {
                fprintf(stderr, "l2pm: gc: refusing: cannot read generation metadata "
                                "%s (run verify --repair)\n", e->d_name);
                free(gdir); free(prof);
                refuse = 1;
                break;
            }
            if (gn == gcap) {
                gcap = gcap ? gcap * 2 : 16;
                gens = xrealloc(gens, gcap * sizeof(*gens));
            }
            gens[gn].prof = prof;
            gens[gn].gen = g;
            gens[gn].dir = gdir;
            gens[gn].rel = xstrdup(e->d_name);
            gens[gn].pkgs = pkgs;
            gens[gn].pn = pn;
            gens[gn].keep = 0;
            gn++;
        }
        closedir(pd);
    }
    if (refuse) {
        for (size_t i = 0; i < gn; i++) {
            free_gen_pkgs(gens[i].pkgs, gens[i].pn);
            free(gens[i].prof); free(gens[i].dir); free(gens[i].rel);
        }
        free(gens);
        pl_free(&live_stores);
        free(pdir);
        l2pm_unlock(lfd);
        return 1;
    }
    if (gn) qsort(gens, gn, sizeof(*gens), cmp_gc_gen);
    for (size_t i = 0; i < gn; i++) {
        char active_rel[MAXPATH] = "";
        long ag = -1;
        int have_active = read_active_gen(pdir, gens[i].prof, active_rel,
                                          sizeof(active_rel), &ag) == 0;
        if (!have_active) {
            char *plink = fmt("%s/%s", pdir, gens[i].prof);
            struct stat pst;
            int link_exists = (lstat(plink, &pst) == 0);
            free(plink);
            if (link_exists) {
                fprintf(stderr, "l2pm: gc: refusing: profile %s active link is corrupt "
                                "(run verify --repair)\n", gens[i].prof);
                refuse = 1;
                break;
            }
        }
        int is_active = have_active && !strcmp(active_rel, gens[i].rel);
        long newer = 0; /* same-profile entries before i are newer (sorted) */
        for (size_t j = 0; j < i; j++)
            if (!strcmp(gens[j].prof, gens[i].prof)) newer++;
        gens[i].keep = is_active || keep_gens || newer < keep_last;
    }
    if (refuse) {
        for (size_t i = 0; i < gn; i++) {
            free_gen_pkgs(gens[i].pkgs, gens[i].pn);
            free(gens[i].prof); free(gens[i].dir); free(gens[i].rel);
        }
        free(gens);
        pl_free(&live_stores);
        free(pdir);
        l2pm_unlock(lfd);
        return 1;
    }
    for (size_t i = 0; i < gn; i++) {
        int keep = gens[i].keep;
        if (!keep && rm_rf(gens[i].dir) != 0) keep = 1;
        if (keep) {
            for (size_t j = 0; j < gens[i].pn; j++)
                if (*gens[i].pkgs[j].store && !pl_has(&live_stores, gens[i].pkgs[j].store))
                    pl_add(&live_stores, gens[i].pkgs[j].store);
        }
        free_gen_pkgs(gens[i].pkgs, gens[i].pn);
        free(gens[i].prof); free(gens[i].dir); free(gens[i].rel);
    }
    free(gens);
    free(pdir);

    char *dir = l2path("store");
    DIR *d = opendir(dir);
    if (!d) { pl_free(&live_stores); free(dir); l2pm_unlock(lfd); return 0; }
    struct pathlist entries = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        pl_add(&entries, e->d_name);
    }
    closedir(d);
    if (entries.n) qsort(entries.v, entries.n, sizeof(*entries.v), cmp_db_name);
    int removed = 0;
    for (size_t i = 0; i < entries.n; i++) {
        char *full = fmt("%s/%s", dir, entries.v[i]);
        if (!pl_has(&live_stores, full)) { rm_rf(full); removed++; }
        free(full);
    }
    pl_free(&entries);
    pl_free(&live_stores);
    free(dir);
    l2pm_unlock(lfd);
    printf("gc: removed %d store path(s)\n", removed);
    return 0;
}

static void collect_recipes_in(const char *dir, const char *needle, struct pathlist *out)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!*needle || strstr(e->d_name, needle)) pl_add(out, e->d_name);
    }
    closedir(d);
}

static int cmd_search(int argc, char **argv)
{
    const char *needle = argc ? argv[0] : "";
    const char *env = getenv("L2PM_RECIPES");
    char *dir = (env && *env) ? xstrdup(env) : l2path("recipes");
    char *repo = l2path("var/recipe-repo");
    struct pathlist found = {0};
    collect_recipes_in(dir, needle, &found);
    collect_recipes_in(repo, needle, &found);
    free(dir);
    free(repo);
    if (found.n) qsort(found.v, found.n, sizeof(*found.v), cmpstr);
    for (size_t i = 0; i < found.n; i++) {
        if (i > 0 && !strcmp(found.v[i], found.v[i - 1])) continue;
        printf("%s\n", found.v[i]);
    }
    pl_free(&found);
    return 0;
}

static long self_rss_kb(void)
{
    char *txt = read_small("/proc/self/status");
    if (!txt) return -1;
    long kb = -1;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "VmHWM:", 6)) { kb = strtol(line + 6, NULL, 10); break; }
    }
    free(txt);
    return kb;
}

static int cmd_selftest(void)
{
    char selftmp[128] = "";
    int ownroot = 0;
    if ((!getenv("L2PM_ROOT") || !getenv("L2PM_ROOT")[0]) && geteuid() != 0) {
        snprintf(selftmp, sizeof(selftmp), "/tmp/l2pm-selftest-XXXXXX");
        if (mkdtemp(selftmp)) { setenv("L2PM_ROOT", selftmp, 1); ownroot = 1; }
    }
    const char *abc = "abc";
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, abc, 3);
    unsigned char h[32];
    sha256_final(&c, h);
    char hx[65];
    hex32(h, hx);
    if (strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
        fprintf(stderr, "selftest: sha256 FAILED\n");
        return 1;
    }
    ensure_layout();
    char *tmp = l2path("tmp/selftest-%ld", (long)getpid());
    rm_rf(tmp);
    mkdir_p(tmp, 0755);
    char *tree = fmt("%s/tree", tmp);
    char *sub = fmt("%s/sub", tree);
    mkdir_p(sub, 0755);
    char *f1 = fmt("%s/hello", tree);
    char *f2 = fmt("%s/sub/world", sub);
    char *lnk = fmt("%s/link", tree);
    write_file(f1, "hello\n", 6);
    write_file(f2, "world\n", 6);
    symlink("hello", lnk);
    char *tar1 = fmt("%s/a.l2p", tmp);
    char *tar2 = fmt("%s/b.l2p", tmp);
    char *out = fmt("%s/out", tmp);
    int rc = 0;
    if (tar_dir(tree, tar1, DEFAULT_EPOCH) != 0 || tar_dir(tree, tar2, DEFAULT_EPOCH) != 0) rc = 1;
    char a[65], b[65];
    if (!rc && (hash_file(tar1, a) != 0 || hash_file(tar2, b) != 0 || strcmp(a, b))) rc = 1;
    if (!rc && tar_extract(tar1, out) != 0) rc = 1;
    char *g1 = fmt("%s/hello", out);
    char *got = rc ? NULL : read_small(g1);
    if (!rc && (!got || strcmp(got, "hello\n"))) rc = 1;
    char *gl = fmt("%s/link", out);
    char lbuf[64];
    ssize_t lr = rc ? -1 : readlink(gl, lbuf, sizeof(lbuf) - 1);
    if (!rc && (lr < 0 || (lbuf[lr] = 0, strcmp(lbuf, "hello") != 0))) rc = 1;
    free(gl);
    free(got); free(g1);
    long rss = self_rss_kb();
    printf("sha256: ok\nreproducible tar: %s\nrss: %ld KiB (budget 10240)\n",
           rc ? "FAILED" : "ok", rss);
    rm_rf(tmp);
    free(lnk); free(f1); free(f2); free(tar1); free(tar2); free(out); free(sub); free(tree); free(tmp);
    if (ownroot) { rm_rf(selftmp); }
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
        "l2pm %s - L2 package manager\n"
        "usage:\n"
        "  l2pm build <recipe> [-o file.l2p] [--check] [--trust]\n"
        "  l2pm add <recipe> [--profile default] [--trust] [--no-deps]\n"
        "  l2pm install <file.l2p>... [--profile default]\n"
        "  l2pm remove <name>... [--profile default] [--force]\n"
        "  l2pm list | info <name> | verify [--repair] | search [text]\n"
        "  l2pm generations [--profile default] | rollback [GEN] [--profile default]\n"
        "  l2pm gc [--keep-generations | --prune-generations]\n"
        "  l2pm hash <file> | tar <dir> <out> [--epoch N]\n"
        "  l2pm selftest\n"
        "recipe steps: 1 repo (git source clone)  2 fetch/extract  3 compile  4 install\n"
        "  keys: source/sha256 or repo/ref (pinned commit); build= (legacy) or\n"
        "        compile= + install=; remote recipes need --trust or confirmation\n"
        "env: L2PM_RECIPE_REPO=<git url | raw base url>  L2PM_RECIPE_REPO_REF=<commit>\n"
        "store: %s/l2/store   profile: %s/l2/var/profiles/default\n",
        L2PM_VERSION, root_prefix(), root_prefix());
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    argc -= 2;
    argv += 2;

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) { usage(); return 0; }
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "version")) { puts(L2PM_VERSION); return 0; }
    if (!strcmp(cmd, "build")) return cmd_build(argc, argv);
    if (!strcmp(cmd, "add")) return cmd_add(argc, argv);
    if (!strcmp(cmd, "install")) return cmd_install(argc, argv);
    if (!strcmp(cmd, "remove") || !strcmp(cmd, "rm")) return cmd_remove(argc, argv);
    if (!strcmp(cmd, "list") || !strcmp(cmd, "ls")) return cmd_list();
    if (!strcmp(cmd, "info")) return cmd_info(argc, argv);
    if (!strcmp(cmd, "verify")) return cmd_verify(argc, argv);
    if (!strcmp(cmd, "generations")) return cmd_generations(argc, argv);
    if (!strcmp(cmd, "rollback")) return cmd_rollback(argc, argv);
    if (!strcmp(cmd, "gc")) return cmd_gc(argc, argv);
    if (!strcmp(cmd, "search")) return cmd_search(argc, argv);
    if (!strcmp(cmd, "selftest")) return cmd_selftest();

    if (!strcmp(cmd, "hash")) {
        if (argc < 1) die("hash: missing file");
        char h[65];
        if (hash_file(argv[0], h) != 0) die("hash: %s: %s", argv[0], strerror(errno));
        printf("%s  %s\n", h, argv[0]);
        return 0;
    }
    if (!strcmp(cmd, "tar")) {
        if (argc < 2) die("tar: usage: l2pm tar <dir> <out> [--epoch N]");
        long long ep = DEFAULT_EPOCH;
        if (argc >= 4 && !strcmp(argv[2], "--epoch")) ep = strtoll(argv[3], NULL, 10);
        if (tar_dir(argv[0], argv[1], ep) != 0) return 1;
        return 0;
    }

    usage();
    return 2;
}