/* l2gpt - tiny GPT partitioner with an sgdisk-compatible command line.
 *
 * Part of L2.  The live image ships this (as "sgdisk" in the l2/install
 * pack) so `l2install disk` can partition a disk without host tools.  It
 * implements the subset of sgdisk that l2install and tools/mkimage.sh use:
 *
 *   l2gpt --zap-all DEV                 destroy GPT + MBR structures
 *   l2gpt [-U GUID] [-n N:START:END] [-t N:CODE] [-c N:NAME] [-u N:GUID] DEV
 *   l2gpt -p DEV                        print the partition table
 *   l2gpt -i N DEV                      print one partition's details
 *   l2gpt -v DEV                        verify primary/backup GPT
 *   l2gpt --version
 *
 * -n START/END: sectors; 0 = first aligned free sector / last usable sector;
 * +N[K|M|G|T] = relative size (K/M/G/T are powers of 1024).  -t takes the
 * usual sgdisk hex type codes (ef00, ef02, 8300, ...) or a full GUID.
 * Changes are batched and written once: protective MBR, primary GPT at LBA
 * 1..33 and the backup copy in the last 33 sectors, then BLKRRPART so the
 * kernel picks the new table up immediately.
 *
 * Why not the real sgdisk?  gdisk is C++; a static musl build would need a
 * musl-targeted libstdc++ (host libstdc++ is ABI-bound to glibc's locale
 * internals) and the binary is ~1 MiB.  This is ~20 KiB of plain C and its
 * on-disk output is byte-identical to sgdisk's for the layouts L2 creates.
 *
 * 512-byte sectors only (4Kn devices are rejected with a clear error).
 * C11, no dependencies beyond libc.  GPLv2, like the rest of L2.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#define SECT        512
#define NPARTS      128
#define ENTRY_SZ    128
#define HDR_SZ      92
#define ALIGN       2048
#define FIRST_USE   34
#define GUID_LEN    16

static uint8_t entries[NPARTS][ENTRY_SZ];
static uint8_t disk_guid[GUID_LEN];
static uint64_t total_sectors;          /* disk size in 512-byte sectors */
static uint64_t last_use;               /* last usable LBA */
static int have_gpt;                    /* entries[] holds a valid table */
static int dirty;                       /* something changed -> write */
static int rdonly;                      /* device could only be opened O_RDONLY */
static const char *dev;
static int fd = -1;

static int part_used(int n);

/* ------------------------------------------------------------------ util */
static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "l2gpt: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)v; v >>= 8; }
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

/* GUIDs are stored mixed-endian: first three fields little-endian. */
static void guid_str(const uint8_t *g, char *out) {
    sprintf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void guid_parse(const char *s, uint8_t *g) {
    static const int pos[16] = {3,2,1,0, 5,4, 7,6, 8,9, 10,11,12,13,14,15};
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '-') continue;
        int hi = hexval((unsigned char)*p), lo;
        if (hi < 0 || !p[1] || (lo = hexval((unsigned char)p[1])) < 0 || n >= 16)
            die("bad GUID: %s", s);
        g[pos[n++]] = (uint8_t)(hi * 16 + lo);
        p++;
    }
    if (n != 16) die("bad GUID: %s", s);
}
static void guid_random(uint8_t *g) {
    int r = open("/dev/urandom", O_RDONLY);
    if (r < 0 || read(r, g, GUID_LEN) != GUID_LEN) die("cannot read /dev/urandom");
    close(r);
    g[6] = (uint8_t)((g[6] & 0x0F) | 0x40);     /* UUID version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);     /* RFC 4122 variant */
}

/* ------------------------------------------------------------- disk I/O */
static void read_sect(uint64_t lba, void *buf) {
    if (pread(fd, buf, SECT, (off_t)lba * SECT) != SECT)
        die("read %s sector %llu failed: %s", dev, (unsigned long long)lba, strerror(errno));
}
static void write_sect(uint64_t lba, const void *buf) {
    if (pwrite(fd, buf, SECT, (off_t)lba * SECT) != SECT)
        die("write %s sector %llu failed: %s", dev, (unsigned long long)lba, strerror(errno));
}

static void open_disk(void) {
    struct stat st;
    fd = open(dev, O_RDWR);
    if (fd < 0 && (errno == EACCES || errno == EROFS || errno == EPERM)) {
        /* Read-only media (write-protected stick, ro file): -p/-v/-i still
         * work; any write attempt dies with a clear error. */
        fd = open(dev, O_RDONLY);
        rdonly = 1;
    }
    if (fd < 0) die("cannot open %s: %s", dev, strerror(errno));
    if (fstat(fd, &st) < 0) die("fstat %s: %s", dev, strerror(errno));
    uint64_t bytes;
    if (S_ISBLK(st.st_mode)) {
        int ssz = 0;
        if (ioctl(fd, BLKSSZGET, &ssz) == 0 && ssz != SECT)
            die("%s has %d-byte sectors; l2gpt supports 512-byte sectors only", dev, ssz);
        if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) die("cannot size %s: %s", dev, strerror(errno));
    } else if (S_ISREG(st.st_mode)) {
        bytes = (uint64_t)st.st_size;
    } else {
        die("%s is not a disk or image file", dev);
    }
    total_sectors = bytes / SECT;
    if (total_sectors < FIRST_USE + 33 + ALIGN)
        die("%s is too small for GPT (%llu sectors)", dev, (unsigned long long)total_sectors);
    last_use = total_sectors - 34;
}

/* ------------------------------------------------------- GPT load/save */
static int hdr_valid(const uint8_t *h, uint64_t expect_lba) {
    uint8_t tmp[HDR_SZ];
    if (memcmp(h, "EFI PART", 8) != 0) return 0;
    if (rd32(h + 8) != 0x00010000) return 0;          /* GPT revision 1.0 */
    if (rd32(h + 12) != HDR_SZ) return 0;
    if (rd64(h + 24) != expect_lba) return 0;
    uint64_t alt = rd64(h + 32);
    if (alt == expect_lba || alt >= total_sectors) return 0;
    uint64_t first = rd64(h + 40), last = rd64(h + 48);
    if (first < FIRST_USE || last > total_sectors - 34 || first > last) return 0;
    memcpy(tmp, h, HDR_SZ);
    memset(tmp + 16, 0, 4);
    return crc32(tmp, HDR_SZ) == rd32(h + 16);
}

/* The entry array must fit on the disk and not overlap the headers. */
static int entries_lba_ok(uint64_t elba) {
    return elba >= 2 && elba <= total_sectors - NPARTS * ENTRY_SZ / SECT;
}

static void load_gpt(void) {
    uint8_t h[SECT], bh[SECT];
    read_sect(1, h);
    read_sect(total_sectors - 1, bh);
    const uint8_t *use = NULL;
    uint64_t elba = 0;
    if (hdr_valid(h, 1)) { use = h; elba = rd64(h + 72); }
    else if (hdr_valid(bh, total_sectors - 1)) { use = bh; elba = rd64(bh + 72); }
    if (!use) { have_gpt = 0; return; }
    uint32_t n = rd32(use + 80), esz = rd32(use + 84);
    if (n != NPARTS || esz != ENTRY_SZ)
        die("%s: GPT with %u entries of %u bytes not supported", dev, n, esz);
    if (!entries_lba_ok(elba))
        die("%s: GPT entry array LBA %llu out of range", dev, (unsigned long long)elba);
    uint8_t buf[NPARTS * ENTRY_SZ];
    for (uint32_t i = 0; i < NPARTS * ENTRY_SZ / SECT; i++)
        read_sect(elba + i, buf + i * SECT);
    if (crc32(buf, sizeof buf) != rd32(use + 88))
        die("%s: GPT entry array CRC mismatch", dev);
    memcpy(entries, buf, sizeof buf);
    memcpy(disk_guid, use + 56, GUID_LEN);
    /* A crafted table with a valid CRC can still carry nonsense LBAs;
     * reject them before any sector arithmetic (first_free, overlap
     * checks, print) can overflow on them. */
    for (int i = 0; i < NPARTS; i++) {
        if (!part_used(i)) continue;
        uint64_t s = rd64(entries[i] + 32), e = rd64(entries[i] + 40);
        if (s < FIRST_USE || e > last_use || s > e)
            die("%s: GPT partition %d has invalid LBA range %llu..%llu",
                dev, i + 1, (unsigned long long)s, (unsigned long long)e);
    }
    have_gpt = 1;
}

static void new_gpt(void) {
    memset(entries, 0, sizeof entries);
    guid_random(disk_guid);
    have_gpt = 1;
}

/* CHS for the protective MBR, matching sgdisk (255 heads / 63 sectors). */
static void chs(uint64_t lba, uint8_t out[3]) {
    uint64_t h = 255, s = 63;
    if (lba >= 1024 * h * s) { out[0] = out[1] = out[2] = 0xFF; return; }
    uint64_t c = lba / (h * s), r = lba % (h * s);
    uint64_t hd = r / s, sec = r % s + 1;
    out[0] = (uint8_t)hd;
    out[1] = (uint8_t)(sec | ((c >> 2) & 0xC0));
    out[2] = (uint8_t)c;
}

static void write_gpt(void) {
    if (rdonly) die("%s is read-only; cannot write a GPT", dev);
    uint8_t sect[SECT];
    uint64_t backup = total_sectors - 1;

    /* protective MBR */
    memset(sect, 0, SECT);
    uint8_t *p = sect + 0x1BE;
    p[0] = 0x00;                          /* not bootable */
    chs(1, p + 1);
    p[4] = 0xEE;
    chs(total_sectors - 1, p + 5);
    wr32(p + 8, 1);
    wr32(p + 12, total_sectors - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(total_sectors - 1));
    sect[510] = 0x55; sect[511] = 0xAA;
    write_sect(0, sect);

    /* entry array (same bytes for primary and backup) */
    uint32_t ecrc = crc32((const uint8_t *)entries, sizeof entries);
    for (int i = 0; i < NPARTS * ENTRY_SZ / SECT; i++) {
        write_sect(2 + i, (const uint8_t *)entries + i * SECT);
        write_sect(backup - 32 + i, (const uint8_t *)entries + i * SECT);
    }

    /* headers: primary at LBA 1, backup at the last LBA */
    for (int copy = 0; copy < 2; copy++) {
        uint8_t h[HDR_SZ];
        memset(h, 0, sizeof h);
        memcpy(h, "EFI PART", 8);
        wr32(h + 8, 0x00010000);
        wr32(h + 12, HDR_SZ);
        wr64(h + 24, copy ? backup : 1);
        wr64(h + 32, copy ? 1 : backup);
        wr64(h + 40, FIRST_USE);
        wr64(h + 48, last_use);
        memcpy(h + 56, disk_guid, GUID_LEN);
        wr64(h + 72, copy ? backup - 32 : 2);
        wr32(h + 80, NPARTS);
        wr32(h + 84, ENTRY_SZ);
        wr32(h + 88, ecrc);
        wr32(h + 16, crc32(h, HDR_SZ));
        memset(sect, 0, SECT);
        memcpy(sect, h, HDR_SZ);
        write_sect(copy ? backup : 1, sect);
    }
    if (fsync(fd) < 0) die("fsync %s: %s", dev, strerror(errno));
    ioctl(fd, BLKRRPART);               /* re-read partitions; fails on files */
}

static void zap_all(void) {
    if (rdonly) die("%s is read-only; cannot zap", dev);
    uint8_t z[SECT];
    memset(z, 0, SECT);
    for (uint64_t i = 0; i < 34; i++) write_sect(i, z);
    for (uint64_t i = total_sectors - 33; i < total_sectors; i++) write_sect(i, z);
    if (fsync(fd) < 0) die("fsync %s: %s", dev, strerror(errno));
    ioctl(fd, BLKRRPART);
    memset(entries, 0, sizeof entries);
    have_gpt = 0;
    dirty = 0;
}

/* ---------------------------------------------------------- operations */
static int part_used(int n) {
    /* in use if either GUID is set: -n assigns the unique GUID first,
     * the type GUID only follows with -t (same invocation or a later one) */
    static const uint8_t zero[2 * GUID_LEN];
    return memcmp(entries[n], zero, 2 * GUID_LEN) != 0;
}

/* First aligned free sector with room for `need` sectors: walk existing
 * partitions in start order, skipping gaps that are too small (sgdisk
 * behaviour for -n N:0:...). */
static uint64_t first_free(uint64_t need) {
    int idx[NPARTS], n = 0;
    for (int i = 0; i < NPARTS; i++) if (part_used(i)) idx[n++] = i;
    for (int i = 1; i < n; i++) {       /* insertion sort by first LBA */
        int k = idx[i], j = i - 1;
        while (j >= 0 && rd64(entries[idx[j]] + 32) > rd64(entries[k] + 32)) {
            idx[j + 1] = idx[j]; j--;
        }
        idx[j + 1] = k;
    }
    uint64_t cur = FIRST_USE;
    for (int i = 0; i < n; i++) {
        uint64_t s = rd64(entries[idx[i]] + 32), e = rd64(entries[idx[i]] + 40);
        if (e < cur) continue;
        if (s > cur) {                  /* gap [cur, s-1] before this part */
            uint64_t a = (cur + ALIGN - 1) / ALIGN * ALIGN;
            /* overflow-safe form of: a + need - 1 < s */
            if (a < s && need <= s - a) return a;
        }
        cur = e + 1;
    }
    return (cur + ALIGN - 1) / ALIGN * ALIGN;
}

/* Strict unsigned parse: no sign, no trailing garbage, no overflow. */
static uint64_t parse_u64(const char *s) {
    if (!*s || *s < '0' || *s > '9') die("bad number: %s", s);
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno == ERANGE || *end) die("bad number: %s", s);
    return v;
}

/* size suffixes are powers of 1024; plain numbers are sectors */
static uint64_t size_sectors(const char *s) {
    size_t len = strlen(s);
    uint64_t mult = 1;
    char c = len ? s[len - 1] : 0;
    if (c == 'K' || c == 'M' || c == 'G' || c == 'T') {
        mult = c == 'K' ? 1024ULL : c == 'M' ? 1048576ULL :
               c == 'G' ? 1073741824ULL : 1099511627776ULL;
        char *tmp = strndup(s, len - 1);
        if (!tmp) die("out of memory");
        uint64_t v = parse_u64(tmp);
        free(tmp);
        if (v > 0xFFFFFFFFFFFFULL / mult) die("size out of range: %s", s);
        return v * mult / SECT;
    }
    return parse_u64(s);
}

static void op_new(const char *arg) {
    char *tmp = strdup(arg);
    if (!tmp) die("out of memory");
    char *ns = tmp, *ss = strchr(tmp, ':'), *es = ss ? strchr(ss + 1, ':') : NULL;
    if (!ss || !es) die("bad -n argument: %s (want N:START:END)", arg);
    *ss++ = 0; *es++ = 0;
    int n = atoi(ns);
    if (n < 1 || n > NPARTS) die("partition number out of range: %s", ns);
    n--;
    if (!have_gpt) new_gpt();
    if (part_used(n)) die("partition %d already exists", n + 1);

    uint64_t need = strcmp(es, "0") == 0 ? last_use :
                    es[0] == '+' ? size_sectors(es + 1) : size_sectors(es);
    uint64_t start = strcmp(ss, "0") == 0 ? first_free(need) : size_sectors(ss);
    if (start < FIRST_USE) start = FIRST_USE;
    uint64_t end;
    if (strcmp(es, "0") == 0) end = last_use;
    else if (es[0] == '+') end = start + size_sectors(es + 1) - 1;
    else end = size_sectors(es);
    if (end > last_use) end = last_use;
    if (start > end || start < FIRST_USE)
        die("invalid partition range %llu..%llu", (unsigned long long)start, (unsigned long long)end);
    for (int i = 0; i < NPARTS; i++) {
        if (i == n || !part_used(i)) continue;
        uint64_t s = rd64(entries[i] + 32), e = rd64(entries[i] + 40);
        if (start <= e && end >= s) die("new partition overlaps partition %d", i + 1);
    }
    uint8_t *e = entries[n];
    memset(e, 0, ENTRY_SZ);
    guid_random(e + 16);
    wr64(e + 32, start);
    wr64(e + 40, end);
    dirty = 1;
    free(tmp);
}

struct typecode { const char *code, *guid, *name; };
static const struct typecode types[] = {
    { "8300", "0FC63DAF-8483-4772-8E79-3D69D8477DE4", "Linux filesystem" },
    { "8200", "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "Linux swap" },
    { "8e00", "E6D6D379-F507-44C2-A23C-238F2A3DF928", "Linux LVM" },
    { "fd00", "A19D880F-05FC-4D3B-A006-743F0F84911E", "Linux RAID" },
    { "ef00", "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "EFI system partition" },
    { "ef02", "21686148-6449-6E6F-744E-656564454649", "BIOS boot partition" },
    { "0700", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", "Microsoft basic data" },
    { "0c01", "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "Microsoft reserved" },
    { "8302", "933AC7E1-2EB4-4F13-B844-0E14E2AEF915", "Linux /home" },
    { "8304", "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709", "Linux x86-64 root (/)" },
};

static int part_no(const char *arg, const char *opt) {
    const char *colon = strchr(arg, ':');
    if (!colon) die("bad -%s argument: %s (want N:VALUE)", opt, arg);
    int n = atoi(arg);
    if (n < 1 || n > NPARTS || !part_used(n - 1))
        die("no such partition: %.*s", (int)(colon - arg), arg);
    return n - 1;
}

static void op_type(const char *arg) {
    int n = part_no(arg, "t");
    const char *code = strchr(arg, ':') + 1;
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (strcasecmp(code, types[i].code) == 0) {
            guid_parse(types[i].guid, entries[n]);
            dirty = 1;
            return;
        }
    }
    if (strlen(code) == 36) { guid_parse(code, entries[n]); dirty = 1; return; }
    die("unknown partition type code: %s", code);
}

static void op_name(const char *arg) {
    int n = part_no(arg, "c");
    const char *name = strchr(arg, ':') + 1;
    uint8_t *dst = entries[n] + 56;
    memset(dst, 0, 72);
    for (int i = 0; i < 36 && name[i]; i++) {
        uint16_t c = (unsigned char)name[i];      /* Latin-1 -> UTF-16LE */
        dst[2 * i] = (uint8_t)c;
        dst[2 * i + 1] = (uint8_t)(c >> 8);
    }
    dirty = 1;
}

static void op_uuid(const char *arg, int disk) {
    if (disk) {
        if (!have_gpt) new_gpt();
        guid_parse(arg, disk_guid);
        dirty = 1;
        return;
    }
    int n = part_no(arg, "u");
    guid_parse(strchr(arg, ':') + 1, entries[n] + 16);
    dirty = 1;
}

/* -------------------------------------------------------------- print */
static void name_str(const uint8_t *e, char *out) {
    int i;
    for (i = 0; i < 36; i++) {
        uint16_t c = e[56 + 2 * i] | (uint16_t)e[57 + 2 * i] << 8;
        if (!c) break;
        out[i] = c < 128 ? (char)c : '?';
    }
    out[i] = 0;
}

static const char *type_name(const uint8_t *g) {
    char s[40];
    guid_str(g, s);
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (strcasecmp(s, types[i].guid) == 0) return types[i].name;
    return "Unknown";
}

static void print_table(void) {
    char gs[40], nm[40];
    guid_str(disk_guid, gs);
    printf("Disk %s: %llu sectors, %.1f MiB\n", dev, (unsigned long long)total_sectors,
           (double)total_sectors * SECT / 1048576.0);
    printf("Disk identifier (GUID): %s\n", gs);
    printf("Partition table holds up to %d entries\n", NPARTS);
    printf("First usable sector is %d, last usable sector is %llu\n", FIRST_USE,
           (unsigned long long)last_use);
    printf("\nNumber  Start (sector)    End (sector)  Size       Code  Name\n");
    for (int i = 0; i < NPARTS; i++) {
        if (!part_used(i)) continue;
        uint64_t s = rd64(entries[i] + 32), e = rd64(entries[i] + 40);
        char code[8] = "----";
        guid_str(entries[i], gs);
        for (size_t t = 0; t < sizeof types / sizeof types[0]; t++)
            if (strcasecmp(gs, types[t].guid) == 0) { snprintf(code, sizeof code, "%s", types[t].code); break; }
        name_str(entries[i], nm);
        printf("%5d  %14llu  %16llu  %8.1f MiB  %s  %s\n", i + 1,
               (unsigned long long)s, (unsigned long long)e,
               (double)(e - s + 1) * SECT / 1048576.0, code, nm);
    }
}

static void print_info(int n) {
    if (n < 1 || n > NPARTS || !part_used(n - 1)) die("no such partition: %d", n);
    const uint8_t *e = entries[n - 1];
    char gs[40], nm[40];
    uint64_t s = rd64(e + 32), en = rd64(e + 40);
    guid_str(e, gs);
    printf("Partition GUID code: %s (%s)\n", gs, type_name(e));
    guid_str(e + 16, gs);
    printf("Partition unique GUID: %s\n", gs);
    printf("First sector: %llu (at %.1f MiB)\n", (unsigned long long)s, (double)s * SECT / 1048576.0);
    printf("Last sector: %llu (at %.1f MiB)\n", (unsigned long long)en, (double)en * SECT / 1048576.0);
    printf("Partition size: %llu sectors (%.1f MiB)\n", (unsigned long long)(en - s + 1),
           (double)(en - s + 1) * SECT / 1048576.0);
    printf("Attribute flags: %016llX\n", (unsigned long long)rd64(e + 48));
    name_str(e, nm);
    printf("Partition name: '%s'\n", nm);
}

static int verify(void) {
    uint8_t h[SECT], bh[SECT];
    read_sect(1, h);
    read_sect(total_sectors - 1, bh);
    int bad = 0;
    if (!hdr_valid(h, 1)) { fprintf(stderr, "primary GPT header invalid\n"); bad = 1; }
    if (!hdr_valid(bh, total_sectors - 1)) { fprintf(stderr, "backup GPT header invalid\n"); bad = 1; }
    if (!bad) {
        uint8_t buf[NPARTS * ENTRY_SZ];
        for (int pass = 0; pass < 2; pass++) {
            uint64_t elba = rd64((pass ? bh : h) + 72);
            if (!entries_lba_ok(elba)) {
                fprintf(stderr, "%s entry array LBA out of range\n", pass ? "backup" : "primary");
                bad = 1;
                continue;
            }
            for (uint32_t i = 0; i < sizeof buf / SECT; i++) read_sect(elba + i, buf + i * SECT);
            if (crc32(buf, sizeof buf) != rd32((pass ? bh : h) + 88)) {
                fprintf(stderr, "%s entry array CRC mismatch\n", pass ? "backup" : "primary");
                bad = 1;
            }
        }
        if (rd64(h + 32) != total_sectors - 1 || rd64(bh + 32) != 1) {
            fprintf(stderr, "backup/primary LBA mismatch\n"); bad = 1;
        }
    }
    if (bad) printf("Problems found!\n");
    else printf("No problems found.\n");
    return bad;
}

/* --------------------------------------------------------------- main */
int main(int argc, char **argv) {
    int info_n = 0, do_print = 0, do_verify = 0, do_zap = 0;
    /* first pass: find the device and the read-only operations */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--version") == 0) { puts("l2gpt 1.0 (sgdisk-compatible subset for L2)"); return 0; }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("usage: l2gpt [--zap-all] [-p] [-v] [-i N] [-U GUID]\n"
                   "             [-n N:START:END] [-t N:CODE] [-c N:NAME] [-u N:GUID] DEVICE\n");
            return 0;
        }
        if (a[0] != '-' || a[1] == 0) { dev = a; continue; }
        if (strcmp(a, "--zap-all") == 0) { do_zap = 1; continue; }
        if (strcmp(a, "-p") == 0) { do_print = 1; continue; }
        if (strcmp(a, "-v") == 0) { do_verify = 1; continue; }
        if (strcmp(a, "-i") == 0) { if (++i >= argc) die("-i needs an argument"); info_n = atoi(argv[i]); continue; }
        if (a[1] && strchr("ntcuU", a[1]) && !a[2]) {
            if (++i >= argc) die("%s needs an argument", a);
            continue;                   /* applied in the second pass below */
        }
        die("unknown option: %s", a);
    }
    if (!dev) die("no device given (try --help)");
    open_disk();
    load_gpt();

    if (do_zap) zap_all();
    /* second pass: mutating options, in command-line order (like sgdisk) */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1] || a[2] || !strchr("ntcuU", a[1])) continue;
        const char *arg = argv[++i];
        switch (a[1]) {
        case 'n': op_new(arg); break;
        case 't': op_type(arg); break;
        case 'c': op_name(arg); break;
        case 'u': op_uuid(arg, 0); break;
        case 'U': op_uuid(arg, 1); break;
        }
    }
    if (dirty) write_gpt();
    if (do_print) { if (!have_gpt) new_gpt(); print_table(); }
    if (info_n) print_info(info_n);
    if (do_verify) return verify();
    if (dirty) printf("The operation has completed successfully.\n");
    return 0;
}
