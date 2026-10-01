/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>   /* strdup allocates */

/*
 * The four that move and compare memory - `memcpy`, `memmove`, `memset`,
 * `memcmp` - a word at a time; everything else a byte at a time, which is
 * correct and, until a profile says otherwise, fast enough.
 *
 * **They were byte loops, and a profile is what asked.** The browser laying
 * out a Wikipedia article spent 9% of its time in these four and every
 * sample of it was a loop over single bytes (`testing.md` 18.329): NetSurf
 * and its libraries compare interned strings, copy small structures and
 * clear what they allocate all the time, and do it at whatever addresses
 * the allocator and the parser hand them. Every process links this file, so
 * every process gets what the browser asked for.
 *
 * **Two copies of this file are compiled, and they do not take the same
 * paths.** A process may load and store a word at any address: it runs with
 * translation on, always, over Normal memory - its own pages, the screen,
 * a driver's DMA buffers - and both machines allow an unaligned access
 * there: x86-64 everywhere, AArch64 because `boot/start.S` leaves
 * `SCTLR_EL1.A` clear. The one Device mapping a process can hold is a
 * driver's registers, reached through `mmio_read32` and never through
 * these. The kernel may not: it runs C before `mmu_init`, where every
 * access is Device-nGnRnE and an unaligned one faults whatever `SCTLR.A`
 * says. So the kernel moves words only when both addresses agree on
 * alignment, and bytes otherwise, as it always did.
 */
#ifdef KOSMOS_USER
#define UNALIGNED_WORDS 1
#else
#define UNALIGNED_WORDS 0
#endif

/*
 * A word from any address, and to any. `__builtin_memcpy` of a constant
 * eight is one load or one store on both machines - and not a call, which
 * a plain `memcpy` is in a freestanding build - and it says to the
 * compiler that the address may be anything, where a cast through
 * `uint64_t *` would promise an alignment that is not there.
 */
/*
 * What the kernel's copy promises - no word at an address that is not a
 * multiple of its size - is a fault before the MMU and nothing at all on
 * the Mac, so the host test is shown every word the kernel's copy touches
 * and counts the ones that are not (`tools/test_string.c`). Nothing in a
 * build that runs.
 */
#ifdef KOSMOS_TEST_ALIGNMENT
void kosmos_test_aligned(const void *p, size_t size);
#define ALIGNED_WORD(p, size) \
    do { if (!UNALIGNED_WORDS) kosmos_test_aligned((p), (size)); } while (0)
#else
#define ALIGNED_WORD(p, size) ((void)0)
#endif

static inline uint64_t load64(const unsigned char *p)
{
    uint64_t w;

    ALIGNED_WORD(p, 8);
    __builtin_memcpy(&w, p, 8);
    return w;
}

static inline void store64(unsigned char *p, uint64_t w)
{
    ALIGNED_WORD(p, 8);
    __builtin_memcpy(p, &w, 8);
}

static inline uint32_t load32(const unsigned char *p)
{
    uint32_t w;

    ALIGNED_WORD(p, 4);
    __builtin_memcpy(&w, p, 4);
    return w;
}

static inline void store32(unsigned char *p, uint32_t w)
{
    ALIGNED_WORD(p, 4);
    __builtin_memcpy(p, &w, 4);
}

/*
 * **In a process**, the words go wherever the addresses are. Up to sixteen
 * bytes is two words that may overlap in the middle - the same bytes
 * written twice with the same values, which is harmless because `memcpy`'s
 * regions do not overlap - and longer is a first word, the destination
 * brought to a multiple of eight, four words a turn, and a last word read
 * before anything is written and stored at the end. No byte loop at
 * either end, which is where a short copy would spend its time.
 *
 * **In the kernel**, eight bytes when both sides are equally misaligned,
 * after bytes enough to align them; four when they agree on four - which
 * is the case that matters for pixels, a row of which starts wherever the
 * pitch says; bytes otherwise, and bytes for the tail.
 *
 * The compositor blits through here, and that is what made it a word loop
 * first: at 1920x1080 a byte loop is 8.3 million stores into a graphics
 * aperture across PCIe, which barely fill the write-combining buffers.
 */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (UNALIGNED_WORDS) {
        if (n <= 16) {
            if (n >= 8) {
                uint64_t a = load64(s), b = load64(s + n - 8);

                store64(d, a);
                store64(d + n - 8, b);
            } else if (n >= 4) {
                uint32_t a = load32(s), b = load32(s + n - 4);

                store32(d, a);
                store32(d + n - 4, b);
            } else {
                while (n-- > 0) {
                    *d++ = *s++;
                }
            }

            return dst;
        }

        uint64_t last = load64(s + n - 8);
        unsigned char *last_at = d + n - 8;
        size_t head = (size_t)(-(uintptr_t)d & 7u);

        store64(d, load64(s));
        d += head;
        s += head;
        n -= head;

        while (n >= 32) {
            uint64_t a = load64(s), b = load64(s + 8);
            uint64_t c = load64(s + 16), e = load64(s + 24);

            store64(d, a);
            store64(d + 8, b);
            store64(d + 16, c);
            store64(d + 24, e);
            d += 32;
            s += 32;
            n -= 32;
        }

        while (n >= 8) {
            store64(d, load64(s));
            d += 8;
            s += 8;
            n -= 8;
        }

        store64(last_at, last);
        return dst;
    }

    if (n >= 16 && (((uintptr_t)d ^ (uintptr_t)s) & 7u) == 0) {
        while (((uintptr_t)d & 7u) != 0) {
            *d++ = *s++;
            n--;
        }

        while (n >= 8) {
            store64(d, load64(s));
            d += 8;
            s += 8;
            n -= 8;
        }
    } else if (n >= 8 && (((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0) {
        while (((uintptr_t)d & 3u) != 0) {
            *d++ = *s++;
            n--;
        }

        while (n >= 4) {
            store32(d, load32(s));
            d += 4;
            s += 4;
            n -= 4;
        }
    }

    while (n-- > 0) {
        *d++ = *s++;
    }

    return dst;
}

/*
 * Regions that do not overlap are a `memcpy`. When they do, the direction
 * decides: forwards when the destination is below the source, backwards
 * when above - the whole reason `memmove` exists, since copying forwards
 * then reads bytes it has already written.
 *
 * A word at a time either way in a process, and safe for the same reason
 * byte by byte is: each word is read before it is stored, and a store only
 * ever reaches source bytes that have been read already - those behind
 * it going forwards, those ahead of it going backwards. Bytes in the kernel.
 */
void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0) {
        return dst;
    }

    if (d + n <= s || s + n <= d) {
        return memcpy(dst, src, n);
    }

    if (d < s) {
        if (UNALIGNED_WORDS) {
            while (n >= 8) {
                store64(d, load64(s));
                d += 8;
                s += 8;
                n -= 8;
            }
        }

        while (n-- > 0) {
            *d++ = *s++;
        }

        return dst;
    }

    d += n;
    s += n;

    if (UNALIGNED_WORDS) {
        while (n >= 8) {
            d -= 8;
            s -= 8;
            store64(d, load64(s));
            n -= 8;
        }
    }

    while (n-- > 0) {
        *--d = *--s;
    }

    return dst;
}

/*
 * The same shape as `memcpy`: in a process two words that may overlap up
 * to sixteen bytes, and beyond that a first word, the destination aligned,
 * four words a turn and a last word at the end. In the kernel, bytes up to
 * an eight-byte boundary, words, and bytes for the tail - so a region that
 * starts unaligned is cleared a word at a time too, which before this it
 * was not.
 */
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    unsigned char b = (unsigned char)c;
    uint64_t word = 0x0101010101010101ULL * b;

    if (UNALIGNED_WORDS && n >= 4) {
        if (n <= 16) {
            if (n >= 8) {
                store64(d, word);
                store64(d + n - 8, word);
            } else {
                store32(d, (uint32_t)word);
                store32(d + n - 4, (uint32_t)word);
            }

            return dst;
        }

        unsigned char *last_at = d + n - 8;
        size_t head = (size_t)(-(uintptr_t)d & 7u);

        store64(d, word);
        d += head;
        n -= head;

        while (n >= 32) {
            store64(d, word);
            store64(d + 8, word);
            store64(d + 16, word);
            store64(d + 24, word);
            d += 32;
            n -= 32;
        }

        while (n >= 8) {
            store64(d, word);
            d += 8;
            n -= 8;
        }

        store64(last_at, word);
        return dst;
    }

    if (n >= 16) {
        while (((uintptr_t)d & 7u) != 0) {
            *d++ = b;
            n--;
        }

        while (n >= 8) {
            store64(d, word);
            d += 8;
            n -= 8;
        }
    }

    while (n-- > 0) {
        *d++ = b;
    }

    return dst;
}

/*
 * Eight bytes compared at once while they are equal, and the first
 * difference found among the next eight a byte at a time - which keeps the
 * answer the standard's, the first differing byte as unsigned char,
 * without asking which end of a word comes first. In a process at any
 * address; in the kernel when both start on a word.
 */
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a;
    const unsigned char *q = b;

    if (UNALIGNED_WORDS || (((uintptr_t)p | (uintptr_t)q) & 7u) == 0) {
        while (n >= 8 && load64(p) == load64(q)) {
            p += 8;
            q += 8;
            n -= 8;
        }
    }

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
