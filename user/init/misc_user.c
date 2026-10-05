/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The pieces of the libc that differ inside a process.
 *
 * errno and the locale are the same in shape and different in where they
 * live. `time()` is `clock_user.c`'s, the wall clock from `sysinfo`; Lua's
 * uses of it are redirected in kosmos_lua.h to the monotonic counter. This
 * said a process had no clock and `time()` was a link error, long after it
 * had both.
 */

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "kosmos.h"
#include <tls.h>

/*
 * **One per thread, in the thread's own block** (`threads.md` step 2).
 *
 * It was one per process, which `design.md` §17.3 asked for and which was
 * literally true here rather than aspirational - a different address space,
 * so no other errno to be confused with. A process with two threads makes it
 * wrong again in the old way: whichever thread ran last would own the
 * number.
 *
 * So the block the thread pointer names holds it. `kosmos_tls()` is one
 * instruction - the register on AArch64, the first word at `%fs:0` on x86,
 * which is why the block begins with its own address.
 *
 * **The kernel makes the block**, one page at the bottom of the thread's
 * stack slot, before the thread runs an instruction (`USER_TBLOCK` in
 * `process.h`). It has to: on x86 a thread reads its block *through* the FS
 * base, so a thread whose base is zero faults at the first `errno` - which
 * is exactly what the first thread this system ever made did, while the
 * same code on AArch64 quietly read a fallback. A program may still point
 * the register at a larger block of its own with `kosmos_set_tls`.
 */
int *__errno(void)
{
    struct tls_block *t = kosmos_tls();

    return &t->errno_value;
}

static char decimal_point[] = ".";
static struct lconv c_locale = { decimal_point };

struct lconv *localeconv(void)
{
    return &c_locale;
}

char *setlocale(int category, const char *locale)
{
    (void)category;
    (void)locale;
    return (char *)"C";
}

/*
 * The only copies there are. The kernel has no errno, no locale and no
 * use for either, so these live with the process that has: `USER_LIBC`
 * names what a process gets, and this file is the part of it that differs
 * inside one.
 */

/*
 *--------------------------------------------------------------------------
 * Conversion and sorting, added because a link error asked.
 *--------------------------------------------------------------------------
 */

long strtol(const char *s, char **end, int base)
{
    const char *at = s;
    long value = 0;
    int negative = 0;

    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        at++;
    }

    if (*at == '+' || *at == '-') {
        negative = (*at == '-');
        at++;
    }

    /* `base == 0` means "work it out from the prefix", which is what makes
     * this usable for the `0x` in a config file as well as for a decimal. */
    if ((base == 0 || base == 16)
        && at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) {
        base = 16;
        at += 2;
    } else if (base == 0) {
        base = (at[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int digit;

        if (*at >= '0' && *at <= '9') {
            digit = *at - '0';
        } else if (*at >= 'a' && *at <= 'z') {
            digit = *at - 'a' + 10;
        } else if (*at >= 'A' && *at <= 'Z') {
            digit = *at - 'A' + 10;
        } else {
            break;
        }

        if (digit >= base) {
            break;
        }

        value = value * base + digit;
        at++;
    }

    /*
     * No overflow detection and no ERANGE.
     *
     * Said out loud rather than left to be discovered: this does not set
     * errno and does not saturate at LONG_MAX. Every caller here is parsing
     * a number it wrote itself - a command line argument, a field in a
     * config file - and none of them check. When something needs to parse a
     * number from somewhere it does not control, this is the function that
     * has to grow the check, and it should grow it then rather than carry
     * an unused one now.
     */
    if (end != NULL) {
        *end = (char *)at;
    }

    return negative ? -value : value;
}

/*
 * The unsigned one, which `libdom` asked for: HTML attributes are parsed
 * through it.
 *
 * Written out rather than layered on `strtol` above, because the two differ
 * in exactly the place a wrapper would have to fake - `strtoul` accepts a
 * leading minus and returns the negation modulo `ULONG_MAX + 1`, which is a
 * value `strtol` cannot represent or return.
 *
 * **Neither of them detects overflow**, and that is worth saying once here
 * for both. The standard says to saturate at `LONG_MAX`/`ULONG_MAX` and set
 * `ERANGE`; these wrap. Nothing in this system parses a number long enough
 * for it to matter, and when something does, this comment is where to start.
 */
unsigned long strtoul(const char *s, char **end, int base)
{
    const char *at = s;
    unsigned long value = 0;
    int negative = 0;

    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        at++;
    }

    if (*at == '+' || *at == '-') {
        negative = (*at == '-');
        at++;
    }

    if ((base == 0 || base == 16)
        && at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) {
        base = 16;
        at += 2;
    } else if (base == 0) {
        base = (at[0] == '0') ? 8 : 10;
    }

    for (;;) {
        int digit;

        if (*at >= '0' && *at <= '9') {
            digit = *at - '0';
        } else if (*at >= 'a' && *at <= 'z') {
            digit = *at - 'a' + 10;
        } else if (*at >= 'A' && *at <= 'Z') {
            digit = *at - 'A' + 10;
        } else {
            break;
        }

        if (digit >= base) {
            break;
        }

        value = value * (unsigned long)base + (unsigned long)digit;
        at++;
    }

    if (end != NULL) {
        *end = (char *)at;
    }

    return negative ? 0UL - value : value;
}

/*
 * The `long long` pair, which FFmpeg asked for: its channel-layout parser
 * reads a mask with `strtoull`. Both targets are LP64, where a `long` is
 * already sixty-four bits - the fact `<inttypes.h>` leans on - so these are
 * the two above under the names C99 gave the wider type, overflow and all.
 * The assertion is what would stop a target where that stopped being true.
 */
_Static_assert(sizeof(long long) == sizeof(long), "LP64: long long is long");

long long strtoll(const char *s, char **end, int base)
{
    return strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    return strtoul(s, end, base);
}

/*
 * `logf`, which FFmpeg's `ffmath.h` calls from a helper that every file
 * including it compiles. Through the double `log` musl provides: its error
 * is a fraction of a double's last place, so rounding the answer to a float
 * gives the correctly rounded float in all but the rarest ties - and the
 * one caller is computing a power, not asking for the last bit.
 */
float logf(float x)
{
    return (float)log((double)x);
}

/* `time()` is in `clock_user.c`, where it can be tested on the Mac. */

/*
 * Processor time, which a process here is not told.
 *
 * The kernel charges scheduler ticks to each process (`proc_info.ticks`),
 * but a process has no way to find its own row, and wall time since boot
 * would be a plausible wrong answer. So this is the standard's own word for
 * "not available" (C11 7.27.2.1), and a caller that checks for it is right.
 *
 * Linked because FFmpeg's `av_get_random_seed` mixes it into a seed, and
 * that function waits for the value to move: it is reached only from the
 * colour parser's "random", which nothing here calls, and it would never
 * return. `runtime/upstream/ffmpeg/README.kosmos.md` lists it with the other
 * things the decoder links and does not run.
 */
clock_t clock(void)
{
    return (clock_t)-1;
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

/*
 * There is no environment, so there is nothing in it.
 *
 * Not a stub waiting to be filled in: `CLAUDE.md` forbids a POSIX
 * personality, and an environment is one - a per-process bag of global
 * names that a child inherits without being handed anything. What this
 * system has instead is the namespace, where what you were not given you
 * cannot reach. Returning NULL is the correct and permanent answer, and
 * every caller already handles it because on a real system a variable may
 * simply not be set.
 */
char *getenv(const char *name)
{
    (void)name;

    return NULL;
}

/*
 * Heapsort, under the standard's name.
 *
 * `qsort` is what the standard calls it and this is not quicksort, which is
 * worth saying rather than hiding. It was an insertion sort, chosen because
 * the arrays sorted here were tens of elements - and then the disk server
 * came to sort a directory listing of up to `NAMES_MAX` names through it
 * (`diskfs.c`), and the 3D kit every edge of a mesh (`k3d_mesh.c`), and
 * both were quadratic. Heapsort is O(n log n) in the worst case and not
 * only on average, and it keeps what the insertion sort was chosen for: no
 * recursion, so no stack that grows with the input, and no allocation.
 *
 * Not stable, which the standard does not ask of `qsort` either: two
 * elements that compare equal may come out in either order.
 */

/* Two elements exchanged a piece at a time, because the element size is a
 * runtime value and there is no temporary that is always big enough. */
static void sort_swap(unsigned char *a, unsigned char *b, size_t size)
{
    unsigned char t[64];

    while (size > 0) {
        size_t n = (size < sizeof(t)) ? size : sizeof(t);

        memcpy(t, a, n);
        memcpy(a, b, n);
        memcpy(b, t, n);

        a += n;
        b += n;
        size -= n;
    }
}

/*
 * The element at `root` moved down until neither child is larger, in a heap
 * of the first `count` elements. A node has a child exactly when it is below
 * `count / 2`, which is the test that also keeps `2 * root + 1` from
 * overflowing.
 */
static void sort_sift(unsigned char *a, size_t root, size_t count,
                      size_t size, int (*compare)(const void *, const void *))
{
    while (root < count / 2) {
        size_t child = 2 * root + 1;

        if (child + 1 < count
            && compare(a + child * size, a + (child + 1) * size) < 0) {
            child++;
        }

        if (compare(a + root * size, a + child * size) >= 0) {
            return;
        }

        sort_swap(a + root * size, a + child * size, size);
        root = child;
    }
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    unsigned char *a = base;
    size_t i;

    if (count < 2 || size == 0) {
        return;
    }

    /* The heap, built from the last parent up to the root. */
    for (i = count / 2; i > 0; i--) {
        sort_sift(a, i - 1, count, size, compare);
    }

    /* The largest to the end, and the heap rebuilt over what is left. */
    for (i = count - 1; i > 0; i--) {
        sort_swap(a, a + i * size, size);
        sort_sift(a, 0, i, size, compare);
    }
}

/*
 * The other half of the same job, and it arrived with the NetSurf
 * libraries: they keep their charset tables sorted and look them up this
 * way. Nothing here had needed it before, so `stdlib.h` did not declare it
 * and the port found out at link time.
 *
 * It was the real algorithm from the start, while `qsort` above began as
 * the simple one, because a binary search *is* the simple one - there is no
 * cheaper version to start with.
 */
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const unsigned char *a = base;

    if (key == NULL || base == NULL || compare == NULL || size == 0) {
        return NULL;
    }

    while (count != 0) {
        size_t mid = count / 2;
        const unsigned char *at = a + mid * size;
        int order = compare(key, at);

        if (order == 0) {
            /* The const goes because that is what the standard returns: the
             * array is the caller's and it may write to what it found. */
            return (void *)at;
        }

        if (order > 0) {
            a = at + size;
            count -= mid + 1;
        } else {
            count = mid;
        }
    }

    return NULL;
}

/*
 * **Two more for NetSurf's layout** (`roadmap.md` 6zz j), which its box
 * builder and its memory pool reach for.
 *
 * `bzero` is BSD's `memset(p, 0, n)` under the name 4.2BSD gave it, kept in
 * `<strings.h>` as `strcasecmp` is.
 *
 * `atexit` says it could not: nothing runs when a Kosmos process ends,
 * because a process ends when its main returns or it is killed, and
 * neither calls anybody back. Refusing is the honest answer - saying yes
 * would be a promise to call something that is never called. talloc asks
 * only when its leak report is switched on, which it is not here.
 */
void bzero(void *p, size_t n)
{
    memset(p, 0, n);
}

int atexit(void (*fn)(void))
{
    (void)fn;
    return -1;
}

/*
 * `n` bytes the kernel's entropy chose (`SYS_ENTROPY`, `kernel/entropy.c`),
 * under BSD's name: expat salts its hash tables with them, so a document
 * cannot be written to make every name collide (`runtime/config/
 * expat_config.h`). It cannot fail, by its contract; a kernel that will not
 * answer is a machine with nothing to salt with, and stops rather than
 * salting with nothing.
 */
void arc4random_buf(void *buf, size_t n)
{
    unsigned char *at = buf;

    while (n > 0) {
        size_t take = n > 256 ? 256 : n;
        long got = kosmos_entropy(at, (unsigned long)take);

        if (got <= 0) {
            abort();
        }

        at += got;
        n -= (size_t)got;
    }
}

/*
 * There is no shell to hand a command line to, and there will not be one.
 *
 * `system()` is a POSIX personality in a single function: it takes a string,
 * finds an interpreter by a global name, and runs it with this process's
 * authority. `CLAUDE.md` forbids exactly that shape. Kosmos starts a program
 * by asking a server that already holds the right to start one.
 *
 * -1 is "the command could not be run", which is the answer, and every
 * caller has to handle it because on a real system the shell may be missing.
 */
int system(const char *command)
{
    (void)command;

    return -1;
}

/*
 * Making a directory needs a path, and a path needs a tree to be in.
 *
 * -1 with errno untouched: the caller checks, says so, and goes on without
 * whatever it wanted the directory for - which for Doom is a place to put
 * savegames it also cannot write. See `<sys/stat.h>`.
 */
int mkdir(const char *path, unsigned int mode)
{
    (void)path; (void)mode;

    return -1;
}

/* `atof` is `strtod` without the end pointer, and there is no error to
 * report: it is defined to return zero for text that is not a number. */
double atof(const char *s)
{
    return strtod(s, NULL);
}

/*
 * `rand` and `srand`, which is the generator printed in the C standard.
 *
 * A linear congruential generator with the constants from C99 7.20.2.2's
 * own example, so that what it produces is exactly what the standard says a
 * conforming implementation may produce, and nobody has to wonder whether
 * this one is unusual.
 *
 * **Here, in the userland's half of the libc**, because the kernel has no
 * business with a random number generator - `CLAUDE.md` is clear about
 * what belongs in there and this is not on the list.
 *
 * **It is not for anything that must not be guessed.** Sixteen bits of
 * output from a thirty-two bit state, entirely determined by the seed:
 * `crypto.c` is where randomness with a requirement on it lives. A debug
 * overlay picking a colour is what asked for it, and Quake's particles are
 * what use it now.
 */
static unsigned long rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245UL + 12345UL;

    return (int)((rand_state / 65536UL) % 32768UL);
}

void srand(unsigned seed)
{
    rand_state = seed;
}
