/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>   /* strdup allocates */

/*
 * Deliberately naive. Every one of these is a byte-at-a-time loop, which is
 * correct and slow.
 *
 * They are not optimised because nothing has measured them yet, and the
 * places where a fast memcpy actually matters are the blitter and the
 * compositor at M6, which get their own C primitives rather than leaning on
 * this. Optimising a word-at-a-time memcpy with alignment handling is a
 * known source of subtle bugs, and buying one before there is a profile
 * asking for it is the trade this project does not make.
 */

/*
 * A word at a time where the addresses allow it, and bytes where they do
 * not.
 *
 * **This was a byte loop, and on a laptop that is what made the desktop
 * slow.** The compositor blits through here: at 1920x1080 a full pass is
 * 8.3 million single-byte stores into a graphics aperture across PCIe.
 * Under QEMU the framebuffer is host memory and the difference is a few
 * milliseconds; on real hardware byte stores barely fill the processor's
 * write-combining buffers, so most of the bandwidth is thrown away one
 * store at a time.
 *
 * Eight bytes when both sides are eight-aligned, four when both are four -
 * which is the case that matters, because a pixel is four bytes and a row
 * of them starts wherever the pitch says. Bytes otherwise, and bytes for
 * the tail.
 *
 * **Both sides checked, not just the destination.** An unaligned load is
 * merely slow on x86-64 and is a fault on some of what `hal.md` lists as
 * possible targets, and a libc that works on one architecture is not a
 * libc.
 */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if ((((uintptr_t)d | (uintptr_t)s) & 7u) == 0) {
        uint64_t *dw = (uint64_t *)(void *)d;
        const uint64_t *sw = (const uint64_t *)(const void *)s;

        while (n >= 8) {
            *dw++ = *sw++;
            n -= 8;
        }

        d = (unsigned char *)(void *)dw;
        s = (const unsigned char *)(const void *)sw;
    } else if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        uint32_t *dw = (uint32_t *)(void *)d;
        const uint32_t *sw = (const uint32_t *)(const void *)s;

        while (n >= 4) {
            *dw++ = *sw++;
            n -= 4;
        }

        d = (unsigned char *)(void *)dw;
        s = (const unsigned char *)(const void *)sw;
    }

    while (n-- > 0) {
        *d++ = *s++;
    }

    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    /* The whole reason memmove exists: when the regions overlap and the
     * destination is above the source, copying forwards reads bytes it has
     * already written. */
    if (d > s && d < s + n) {
        d += n;
        s += n;
        while (n-- > 0) {
            *--d = *--s;
        }
        return dst;
    }

    while (n-- > 0) {
        *d++ = *s++;
    }

    return dst;
}

/* The same argument as `memcpy` above, for the same reason: clearing a
 * region of a framebuffer is the other half of what a compositor does. */
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;

    if (((uintptr_t)d & 7u) == 0 && n >= 8) {
        uint64_t word = 0x0101010101010101ULL * b;
        uint64_t *dw = (uint64_t *)(void *)d;

        while (n >= 8) {
            *dw++ = word;
            n -= 8;
        }

        d = (unsigned char *)(void *)dw;
    }

    while (n-- > 0) {
        *d++ = b;
    }

    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a;
    const unsigned char *q = b;

    while (n-- > 0) {
        if (*p != *q) {
            return (int)*p - (int)*q;
        }
        p++;
        q++;
    }

    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    while (n-- > 0) {
        if (*p == (unsigned char)c) {
            /* Casting the const away is what the standard specifies these
             * functions do, ugly as it is. Through uintptr_t so it is one
             * deliberate conversion rather than a silent qualifier drop. */
            return (void *)(uintptr_t)p;
        }
        p++;
    }

    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p != '\0') {
        p++;
    }

    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    /* Compared as unsigned char, which is what the standard says and what
     * makes the ordering consistent for bytes above 127. */
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a != '\0' && *a == *b) {
        a++;
        b++;
        n--;
    }

    if (n == 0) {
        return 0;
    }

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *out = dst;

    while ((*dst++ = *src++) != '\0') {
        /* the assignment is the loop */
    }

    return out;
}

char *strchr(const char *s, int c)
{
    char target = (char)c;

    for (;;) {
        if (*s == target) {
            return (char *)(uintptr_t)s;
        }
        if (*s == '\0') {
            /* strchr(s, '\0') finds the terminator, which the loop above
             * has already returned. Reaching here means it was not found. */
            return NULL;
        }
        s++;
    }
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;
    char target = (char)c;

    for (;;) {
        if (*s == target) {
            found = s;
        }
        if (*s == '\0') {
            return (char *)(uintptr_t)found;
        }
        s++;
    }
}

/* Whether c is one of the bytes in set. The terminator is deliberately not a
 * member: strspn("abc", "") must be 0, not 3. */
static int in_set(char c, const char *set)
{
    for (; *set != '\0'; set++) {
        if (*set == c) {
            return 1;
        }
    }

    return 0;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s != '\0'; s++) {
        if (in_set(*s, accept)) {
            return (char *)(uintptr_t)s;
        }
    }

    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    const char *p = s;

    while (*p != '\0' && in_set(*p, accept)) {
        p++;
    }

    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p = s;

    while (*p != '\0' && !in_set(*p, reject)) {
        p++;
    }

    return (size_t)(p - s);
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    /* An empty needle matches at the start, which is what the standard says
     * and what the loop below would otherwise get wrong. */
    if (n == 0) {
        return (char *)(uintptr_t)haystack;
    }

    for (; *haystack != '\0'; haystack++) {
        if (strncmp(haystack, needle, n) == 0) {
            return (char *)(uintptr_t)haystack;
        }
    }

    return NULL;
}

/*
 *--------------------------------------------------------------------------
 * Added because a link error asked, which is the rule this file follows.
 *
 * The asker was Doom. Every one of these is ordinary C89 string handling
 * with nothing system-specific in it, so they go here rather than beside
 * the thing that wanted them - the next caller should find them where the
 * standard says they are.
 *--------------------------------------------------------------------------
 */

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }

    /* The padding is not a courtesy, it is what the standard says: strncpy
     * fills the rest of the buffer with NULs, and code that relies on a
     * short copy leaving zeroes behind is common enough that leaving it out
     * would be a bug somebody else has to find. */
    for (; i < n; i++) {
        dst[i] = '\0';
    }

    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *at = dst + strlen(dst);

    while ((*at++ = *src++) != '\0') {
    }

    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *at = dst + strlen(dst);
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++) {
        at[i] = src[i];
    }

    at[i] = '\0';

    return dst;
}

/*
 * Case-insensitive comparison, ASCII only.
 *
 * Not `tolower` from <ctype.h>, which is locale-aware in principle: there is
 * one locale here and it is C, and doing the arithmetic inline keeps this
 * file free of a dependency on that being true.
 */
static int fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a != '\0' && fold((unsigned char)*a) == fold((unsigned char)*b)) {
        a++;
        b++;
    }

    return fold((unsigned char)*a) - fold((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int ca = fold((unsigned char)a[i]);
        int cb = fold((unsigned char)b[i]);

        if (ca != cb) {
            return ca - cb;
        }

        if (ca == '\0') {
            break;
        }
    }

    return 0;
}

/*
 * `strdup` is NOT here, and the reason is worth the four lines.
 *
 * It allocates, and this file is linked into the *kernel* as well as into
 * userland - CLAUDE.md's first principle is that there is no dynamic
 * allocator in the kernel, so there is no `malloc` for it to call and the
 * link fails. That failure is the rule working: a function that allocates
 * cannot live in the half of the libc the kernel shares.
 *
 * It lives in `malloc.c`, which only userland links, next to the heap it
 * needs.
 */
