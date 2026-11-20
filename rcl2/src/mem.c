/* rcl2 - freestanding runtime support.
 *
 * Built with -ffreestanding, so the compiler is allowed to call
 * memcpy/memmove/memset/memcmp for struct copies and the like, and GCC/Clang
 * assume they exist; they must be defined in every freestanding program's
 * link. No libc links into rcl2, so we own tiny correct definitions here.
 *
 * The Rust core's object file also carries a DW.ref.rust_eh_personality
 * section that wants that symbol to exist; with -C panic=abort nothing may
 * ever call it, so a weak, never-returning stub satisfies the reference.
 */
#include "syscalls.h"

void *memcpy(void *dst, const void *src, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);
int   bcmp(const void *a, const void *b, unsigned long n);   /* memcmp alias   */
unsigned long strlen(const char *s);

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return *x < *y ? -1 : 1;
        x++; y++;
    }
    return 0;
}

int bcmp(const void *a, const void *b, unsigned long n) { return memcmp(a, b, n); }

unsigned long strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

__attribute__((weak))
void rust_eh_personality(void)
{
    for (;;) __asm__ volatile ("pause");
}
