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
 *   SOURCE_DATE_EPOCH   fixed timestamp for reproducible archives
 *   JOBS            build parallelism (default 8)
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

static int rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
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

static int write_file(const char *path, const void *buf, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return close(fd);
}

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
        if (r < 4096) break;
    }
    fclose(f);
    buf[len] = 0;
    return buf;
}

static int valid_name(const char *s)
{
    if (!s || !*s) return 0;
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

    char name[MAXREL];
    snprintf(name, sizeof(name), "%s%s", rel, S_ISDIR(st.st_mode) ? "/" : "");
    char nm[100], pre[155];
    if (split_ustar(name, nm, pre) != 0) {
        fprintf(stderr, "l2pm: path too long for ustar: %s\n", name);
        free(full);
        return -1;
    }

    unsigned char hdr[512];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, nm, 100);
    put_octal((char *)hdr + 100, 8, st.st_mode & 07777);
    put_octal((char *)hdr + 108, 8, 0);
    put_octal((char *)hdr + 116, 8, 0);
    long long sz = S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
    if (S_ISLNK(st.st_mode)) sz = (long long)st.st_size;
    put_octal((char *)hdr + 124, 12, (unsigned long long)sz);
    put_octal((char *)hdr + 136, 12, (unsigned long long)epoch);
    memset(hdr + 148, ' ', 8);
    hdr[156] = S_ISDIR(st.st_mode) ? '5' : S_ISLNK(st.st_mode) ? '2' : '0';
    memcpy(hdr + 257, "ustar", 5);
    memcpy(hdr + 263, "00", 2);
    memcpy(hdr + 265, "root", 4);
    memcpy(hdr + 297, "root", 4);
    memcpy(hdr + 345, pre, 155);

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
        memcpy(hdr + 157, link, strlen(link));
    }

    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += hdr[i];
    char cks[8];
    snprintf(cks, sizeof(cks), "%06o", sum);
    memcpy(hdr + 148, cks, 6);
    hdr[154] = 0;
    hdr[155] = ' ';

    if (write(fd, hdr, 512) != 512) { free(link); free(full); return -1; }

    if (S_ISREG(st.st_mode)) {
        int in = open(full, O_RDONLY | O_CLOEXEC);
        if (in < 0) { free(link); free(full); return -1; }
        char buf[65536];
        long long left = sz;
        while (left > 0) {
            ssize_t r = read(in, buf, (size_t)(left < (long long)sizeof(buf) ? left : (long long)sizeof(buf)));
            if (r < 0) { if (errno == EINTR) continue; close(in); free(link); free(full); return -1; }
            if (r == 0) break;
            if (write(fd, buf, (size_t)r) != r) { close(in); free(link); free(full); return -1; }
            left -= r;
        }
        close(in);
        char zero[512];
        memset(zero, 0, sizeof(zero));
        size_t pad = (size_t)((512 - (sz % 512)) % 512);
        if (pad && write(fd, zero, pad) != (ssize_t)pad) { free(link); free(full); return -1; }
    } else if (S_ISLNK(st.st_mode) && sz > 0) {
        if (write(fd, link, (size_t)sz) != sz) { free(link); free(full); return -1; }
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
        if (write(fd, zero, sizeof(zero)) != (ssize_t)sizeof(zero)) rc = -1;
    }
    close(fd);
    for (size_t i = 0; i < l.n; i++) free(l.v[i]);
    free(l.v);
    if (rc != 0) unlink(out);
    return rc;
}

/* --------------------------------------------------------------- tar in */

static int safe_rel(const char *name)
{
    if (!name[0] || name[0] == '/') return 0;
    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (n == 0) break;
        p = e;
    }
    return 1;
}

static int tar_extract(const char *tar, const char *dest)
{
    FILE *f = fopen(tar, "rb");
    if (!f) { fprintf(stderr, "l2pm: cannot open %s: %s\n", tar, strerror(errno)); return -1; }
    if (mkdir_p(dest, 0755) != 0) { fclose(f); return -1; }
    int rc = 0;
    for (;;) {
        unsigned char hdr[512];
        if (fread(hdr, 1, 512, f) != 512) { rc = -1; break; }
        int zero = 1;
        for (int i = 0; i < 512; i++) if (hdr[i]) { zero = 0; break; }
        if (zero) break;

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
        char modebuf[16], sizebuf[16], mtbuf[16];
        memcpy(modebuf, hdr + 100, 8); modebuf[8] = 0;
        memcpy(sizebuf, hdr + 124, 12); sizebuf[12] = 0;
        memcpy(mtbuf, hdr + 136, 12); mtbuf[12] = 0;
        mode_t mode = (mode_t)strtoul(modebuf, NULL, 8) & 07777;
        long long size = strtoll(sizebuf, NULL, 8);
        long long mt = strtoll(mtbuf, NULL, 8);
        char type = (char)hdr[156];
        if (size < 0 || size > (1LL << 40)) { rc = -1; break; }

        char *out = fmt("%s/%s", dest, rel);
        char *slash = strrchr(out, '/');
        if (slash) { *slash = 0; mkdir_p(*out ? out : "/", 0755); *slash = '/'; }

        if (type == '5') {
            if (mkdir_p(out, mode ? mode : 0755) != 0) rc = -1;
        } else if (type == '2') {
            unlink(out);
            if (symlink(linkname, out) != 0) rc = -1;
        } else if (type == '1') {
            char *target = fmt("%s/%s", dest, linkname);
            unlink(out);
            if (link(target, out) != 0) rc = -1;
            free(target);
        } else if (type == '0' || type == 0) {
            if (size > 0) {
                FILE *o = fopen(out, "wb");
                if (!o) rc = -1;
                else {
                    long long left = size;
                    unsigned char buf[65536];
                    while (left > 0) {
                        size_t want = (size_t)(left < (long long)sizeof(buf) ? left : (long long)sizeof(buf));
                        size_t r = fread(buf, 1, want, f);
                        if (r != want) { rc = -1; break; }
                        if (fwrite(buf, 1, r, o) != r) { rc = -1; break; }
                        left -= (long long)r;
                    }
                    fclose(o);
                }
                chmod(out, mode ? mode : 0644);
            } else {
                int o = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode ? mode : 0644);
                if (o < 0) rc = -1; else close(o);
            }
        } else if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
            /* metadata header: skip its payload */
        } else {
            fprintf(stderr, "l2pm: unsupported tar entry type '%c' (%s)\n", type, rel);
            rc = -1;
        }
        free(out);
        if (rc != 0) break;

        size_t pad = (size_t)((512 - (size % 512)) % 512);
        if (pad && fseek(f, (long)pad, SEEK_CUR) != 0) { rc = -1; break; }
        /* set mtime best effort (skip metadata pseudo entries and symlinks) */
        if (type == '0' || type == '5') {
            char *o = fmt("%s/%s", dest, rel);
            struct timespec ts[2];
            ts[0].tv_sec = (time_t)mt; ts[0].tv_nsec = 0;
            ts[1] = ts[0];
            utimensat(AT_FDCWD, o, ts, AT_SYMLINK_NOFOLLOW);
            free(o);
        }
    }
    fclose(f);
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
    while ((n = getline(&line, &cap, f)) >= 0) {
        (void)n;
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s);
        char *v = trim(eq + 1);
        if (!strcmp(k, "name")) snprintf(r->name, sizeof(r->name), "%s", v);
        else if (!strcmp(k, "version")) snprintf(r->version, sizeof(r->version), "%s", v);
        else if (!strcmp(k, "source")) snprintf(r->source, sizeof(r->source), "%s", v);
        else if (!strcmp(k, "sha256")) snprintf(r->sha256, sizeof(r->sha256), "%s", v);
        else if (!strcmp(k, "source_dir") || !strcmp(k, "srcdir")) snprintf(r->srcdir, sizeof(r->srcdir), "%s", v);
        else if (!strcmp(k, "build")) snprintf(r->build, sizeof(r->build), "%s", v);
        else if (!strcmp(k, "deps")) snprintf(r->deps, sizeof(r->deps), "%s", v);
        else if (!strcmp(k, "epoch")) r->epoch = strtoll(v, NULL, 10);
    }
    free(line);
    fclose(f);
    if (!valid_name(r->name) || !valid_name(r->version)) {
        fprintf(stderr, "l2pm: recipe %s needs valid name/version\n", path);
        return -1;
    }
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
        const char *base = strrchr(r->source, '/');
        base = base ? base + 1 : "source.tar";
        file = fmt("%s/%s", cache, base);
        if (access(file, R_OK) != 0) {
            char *q = shq(r->source);
            char *cmd = fmt("curl -fL --retry 3 --connect-timeout 20 -o %s %s || wget -O %s %s",
                            shq(file), q, shq(file), q);
            int rc = run_shell(cmd, NULL, 0);
            free(cmd); free(q);
            if (rc != 0) { free(file); return -1; }
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
            char *q = shq(file), *d = shq(srcdir);
            char *cmd = fmt("tar -xf %s -C %s", q, d);
            int rc = run_shell(cmd, NULL, 0);
            free(cmd); free(q); free(d);
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

    if (fetch_source(r, cache, src) != 0) {
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

    int rc = 0;
    if (r->build[0]) {
        fprintf(stderr, "l2pm: building %s %s\n", r->name, r->version);
        rc = run_shell(r->build, srcroot, 0);
        if (rc != 0) fprintf(stderr, "l2pm: build command failed (%d)\n", rc);
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
    int rc = write_file(db, buf, (size_t)n);
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
            unlink(dst);
            if (symlink(src, dst) != 0) rc = -1;
        }
        free(src); free(dst);
    }
    for (size_t i = 0; i < l.n; i++) free(l.v[i]);
    free(l.v);
    return rc;
}

static int rebuild_profile(const char *profile)
{
    if (!valid_name(profile)) { fprintf(stderr, "l2pm: bad profile name\n"); return -1; }
    char *dir = l2path("var/profiles/%s", profile);
    rm_rf(dir);
    if (mkdir_p(dir, 0755) != 0) { free(dir); return -1; }
    /* Stable PATH directories even before the first package is installed.
     * The L2 rootfs points /usr/bin at this tree. */
    const char *stddirs[] = { "bin", "sbin", "usr/bin", "usr/sbin", NULL };
    for (int i = 0; stddirs[i]; i++) {
        char *p = fmt("%s/%s", dir, stddirs[i]);
        mkdir_p(p, 0755);
        free(p);
    }
    size_t n = 0;
    char **names = db_names(&n);
    for (size_t i = 0; i < n; i++) {
        char *store = db_get(names[i], "store");
        if (store) { link_tree(store, dir); free(store); }
        free(names[i]);
    }
    free(names);
    char *marker = fmt("%s/.l2pm-profile", dir);
    write_file(marker, "profile\n", 8);
    free(marker);
    free(dir);
    return 0;
}

/* ------------------------------------------------------------- commands */

static char *resolve_recipe(const char *arg)
{
    struct stat st;
    if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) return xstrdup(arg);
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
    return NULL;
}

static int cmd_build(int argc, char **argv)
{
    const char *arg = NULL, *out = NULL;
    int check = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) out = argv[++i];
        else if (!arg) arg = argv[i];
        else die("build: unexpected argument %s", argv[i]);
    }
    if (!arg) die("build: missing recipe");
    char *recipe_path = resolve_recipe(arg);
    if (!recipe_path) die("build: recipe not found: %s", arg);
    struct recipe r;
    if (parse_recipe(recipe_path, &r) != 0) return 1;
    free(recipe_path);
    ensure_layout();

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

static int install_tar(const char *file, const char *profile)
{
    ensure_layout();
    char hash[65];
    if (hash_file(file, hash) != 0) { fprintf(stderr, "l2pm: cannot hash %s\n", file); return 1; }
    char *tmp = l2path("tmp/install-%ld", (long)getpid());
    rm_rf(tmp);
    if (tar_extract(file, tmp) != 0) { rm_rf(tmp); free(tmp); return 1; }
    char name[128], ver[128], deps[1024];
    if (read_info(tmp, name, sizeof(name), ver, sizeof(ver), deps, sizeof(deps)) != 0) {
        fprintf(stderr, "l2pm: package has no valid .l2pm/info\n");
        rm_rf(tmp); free(tmp); return 1;
    }
    char *store = store_path_for(hash, name, ver);
    struct stat st;
    if (lstat(store, &st) == 0) {
        rm_rf(tmp);
    } else {
        /* Extract to a sibling temp and rename, so a failed extraction
         * never leaves a half package in the store.  Both paths live under
         * the same L2 root, so rename is atomic there. */
        char *tmp2 = fmt("%s.tmp-%ld", store, (long)getpid());
        rm_rf(tmp2);
        if (rename(tmp, tmp2) != 0 || rename(tmp2, store) != 0) {
            rm_rf(tmp2);
            rm_rf(tmp);
            free(tmp2); free(tmp); free(store);
            return 1;
        }
        free(tmp2);
    }
    free(tmp);
    if (db_write(name, ver, hash, store, deps) != 0) { free(store); return 1; }
    if (rebuild_profile(profile) != 0) { free(store); return 1; }
    printf("installed %s %s -> %s\n", name, ver, store);
    free(store);
    return 0;
}

static int cmd_install(int argc, char **argv)
{
    const char *file = NULL, *profile = "default";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!file) file = argv[i];
        else die("install: unexpected argument %s", argv[i]);
    }
    if (!file) die("install: missing package");
    return install_tar(file, profile);
}

static int cmd_add(int argc, char **argv)
{
    const char *arg = NULL, *profile = "default";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!arg) arg = argv[i];
        else die("add: unexpected argument %s", argv[i]);
    }
    if (!arg) die("add: missing recipe");
    char *recipe_path = resolve_recipe(arg);
    if (!recipe_path) die("add: recipe not found: %s", arg);
    struct recipe r;
    if (parse_recipe(recipe_path, &r) != 0) { free(recipe_path); return 1; }
    free(recipe_path);
    ensure_layout();
    char *cache = l2path("cache");
    char *tar = fmt("%s/%s-%s.l2p", cache, r.name, r.version);
    free(cache);
    if (build_once(&r, tar) != 0) { free(tar); return 1; }
    int rc = install_tar(tar, profile);
    free(tar);
    return rc;
}

static int cmd_remove(int argc, char **argv)
{
    const char *name = NULL, *profile = "default";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!name) name = argv[i];
        else die("remove: unexpected argument %s", argv[i]);
    }
    if (!name) die("remove: missing package");
    if (!valid_name(name)) die("remove: bad package name");
    char *db = l2path("var/db/installed/%s", name);
    if (unlink(db) != 0) { fprintf(stderr, "l2pm: %s is not installed\n", name); free(db); return 1; }
    free(db);
    rebuild_profile(profile);
    printf("removed %s\n", name);
    return 0;
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
    char *p = l2path("var/db/installed/%s", argv[0]);
    char *txt = read_small(p);
    free(p);
    if (!txt) die("info: %s is not installed", argv[0]);
    fputs(txt, stdout);
    free(txt);
    return 0;
}

static int cmd_verify(void)
{
    size_t n = 0;
    char **names = db_names(&n);
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        char *store = db_get(names[i], "store");
        char *ver = db_get(names[i], "version");
        if (!store || !is_dir(store)) {
            printf("BROKEN %s: store path missing\n", names[i]);
            bad++;
        } else {
            char nm[128], vv[128], dd[1024];
            if (read_info(store, nm, sizeof(nm), vv, sizeof(vv), dd, sizeof(dd)) != 0 ||
                strcmp(nm, names[i]) != 0) {
                printf("BROKEN %s: metadata mismatch\n", names[i]);
                bad++;
            } else if (ver && strcmp(vv, ver) != 0) {
                printf("BROKEN %s: version mismatch\n", names[i]);
                bad++;
            } else {
                printf("ok %s %s\n", names[i], vv);
            }
        }
        free(store); free(ver); free(names[i]);
    }
    free(names);
    return bad ? 1 : 0;
}

static int path_referenced(const char *store)
{
    size_t n = 0;
    char **names = db_names(&n);
    int found = 0;
    for (size_t i = 0; i < n && !found; i++) {
        char *s = db_get(names[i], "store");
        if (s && !strcmp(s, store)) found = 1;
        free(s);
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return found;
}

static int cmd_gc(void)
{
    char *dir = l2path("store");
    DIR *d = opendir(dir);
    if (!d) { free(dir); return 0; }
    size_t n = 0, cap = 0;
    char **v = NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = xrealloc(v, cap * sizeof(*v)); }
        v[n++] = xstrdup(e->d_name);
    }
    closedir(d);
    qsort(v, n, sizeof(*v), cmp_db_name);
    int removed = 0;
    for (size_t i = 0; i < n; i++) {
        char *full = fmt("%s/%s", dir, v[i]);
        if (!path_referenced(full)) { rm_rf(full); removed++; }
        free(full);
        free(v[i]);
    }
    free(v);
    free(dir);
    printf("gc: removed %d store path(s)\n", removed);
    return 0;
}

static int cmd_search(int argc, char **argv)
{
    const char *needle = argc ? argv[0] : "";
    const char *env = getenv("L2PM_RECIPES");
    char *dir = (env && *env) ? xstrdup(env) : l2path("recipes");
    DIR *d = opendir(dir);
    if (!d) { free(dir); return 0; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!*needle || strstr(e->d_name, needle)) printf("%s\n", e->d_name);
    }
    closedir(d);
    free(dir);
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
    write_file(f1, "hello\n", 6);
    write_file(f2, "world\n", 6);
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
    free(got); free(g1);
    long rss = self_rss_kb();
    printf("sha256: ok\nreproducible tar: %s\nrss: %ld KiB (budget 10240)\n",
           rc ? "FAILED" : "ok", rss);
    rm_rf(tmp);
    free(f1); free(f2); free(tar1); free(tar2); free(out); free(sub); free(tree); free(tmp);
    if (ownroot) { rmdir(selftmp); }
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
        "l2pm %s - L2 package manager\n"
        "usage:\n"
        "  l2pm build <recipe> [-o file.l2p] [--check]\n"
        "  l2pm add <recipe> [--profile default]\n"
        "  l2pm install <file.l2p> [--profile default]\n"
        "  l2pm remove <name> [--profile default]\n"
        "  l2pm list | info <name> | verify | gc | search [text]\n"
        "  l2pm hash <file> | tar <dir> <out> [--epoch N]\n"
        "  l2pm selftest\n"
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
    if (!strcmp(cmd, "verify")) return cmd_verify();
    if (!strcmp(cmd, "gc")) return cmd_gc();
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