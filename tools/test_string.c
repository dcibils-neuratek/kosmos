/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The libc's four that move and compare memory - `memcpy`, `memmove`,
 * `memset`, `memcmp` - checked on this machine against a byte loop, at
 * every alignment of both addresses and at every length that has a path of
 * its own (`runtime/libc/string.c`, `testing.md` 18.329).
 *
 * **Built twice**, because the file is compiled twice and the two copies do
 * not take the same paths: `test_string` with `KOSMOS_USER`, as a process
 * links it - words at any address - and `test_string_kernel` without, as
 * the kernel does - words only where both addresses agree. The four are
 * renamed on the command line, so what is called below is Kosmos's and not
 * the host's. And the kernel's copy is also held to the promise the Mac
 * cannot see broken - no word at an unaligned address, which before the MMU
 * is a fault - by being shown every word it touches.
 *
 * What is held, for each: the bytes it was asked for and none either side
 * of them (a guard before and after every destination), the pointer it
 * returns, and for `memcmp` the sign of the answer - the first difference
 * decides, and a byte above 127 is larger than one below, which a signed
 * comparison gets backwards.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void *kosmos_test_memcpy(void *dst, const void *src, size_t n);
void *kosmos_test_memmove(void *dst, const void *src, size_t n);
void *kosmos_test_memset(void *dst, int c, size_t n);
int   kosmos_test_memcmp(const void *a, const void *b, size_t n);

static int checks;
static int failures;

/* Words the kernel's copy touched at an address not a multiple of their
 * size: `string.c` reports every one it touches when built with
 * `KOSMOS_TEST_ALIGNMENT`, which only `test_string_kernel` is. */
static long misaligned;

void kosmos_test_aligned(const void *p, size_t size);

void kosmos_test_aligned(const void *p, size_t size)
{
    if (((uintptr_t)p & (size - 1)) != 0) {
        misaligned++;
    }
}

static void check(int ok, const char *what, size_t n, size_t x, size_t y)
{
    checks++;

    if (!ok) {
        failures++;

        if (failures <= 20) {
            printf("not ok %d - %s, %zu bytes, at %zu and %zu\n",
                   checks, what, n, x, y);
        }
    }
}

/* Every length up to 80 - each short path and the first turns of each
 * loop - and then either side of the unrolled loop's boundaries, and two
 * long ones with odd tails. */
static const size_t LONG[] = { 95, 96, 97, 127, 128, 129, 255, 256, 257,
                               1000, 4099 };
#define SHORT_TO 80
#define GUARD    16
#define ROOM     (GUARD + 16 + 4099 + GUARD)
#define GUARDED  0xEE

static unsigned char src_buf[ROOM];
static unsigned char dst_buf[ROOM];
static unsigned char want[ROOM];

static size_t length_at(size_t i)
{
    return i <= SHORT_TO ? i : LONG[i - SHORT_TO - 1];
}

#define LENGTHS (SHORT_TO + 1 + sizeof(LONG) / sizeof(LONG[0]))

static void fill(unsigned char *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)(i * 7u + seed * 13u + (i >> 8));
    }
}

/* The guards either side of [at, at + n) in a buffer that was all GUARDED
 * outside it. */
static int guards_hold(const unsigned char *buf, size_t at, size_t n)
{
    for (size_t i = 0; i < at; i++) {
        if (buf[i] != GUARDED) {
            return 0;
        }
    }

    for (size_t i = at + n; i < at + n + GUARD; i++) {
        if (buf[i] != GUARDED) {
            return 0;
        }
    }

    return 1;
}

static int same(const unsigned char *a, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }

    return 1;
}

static void copies(void)
{
    for (size_t li = 0; li < LENGTHS; li++) {
        size_t n = length_at(li);

        for (size_t so = 0; so < 16; so++) {
            for (size_t dof = 0; dof < 16; dof++) {
                unsigned char *s = src_buf + GUARD + so;
                unsigned char *d = dst_buf + GUARD + dof;

                fill(src_buf, ROOM, (unsigned)(n + so));

                for (size_t i = 0; i < ROOM; i++) {
                    dst_buf[i] = GUARDED;
                }

                void *r = kosmos_test_memcpy(d, s, n);

                check(r == d && same(d, s, n)
                      && guards_hold(dst_buf, GUARD + dof, n),
                      "memcpy", n, so, dof);
            }
        }
    }
}

static void sets(void)
{
    static const int VALUES[] = { 0, 0x5a, 0xff, 0x1a5, -1 };

    for (size_t vi = 0; vi < sizeof(VALUES) / sizeof(VALUES[0]); vi++) {
        unsigned char b = (unsigned char)VALUES[vi];

        for (size_t li = 0; li < LENGTHS; li++) {
            size_t n = length_at(li);

            for (size_t dof = 0; dof < 16; dof++) {
                unsigned char *d = dst_buf + GUARD + dof;
                int ok = 1;

                for (size_t i = 0; i < ROOM; i++) {
                    dst_buf[i] = GUARDED;
                }

                void *r = kosmos_test_memset(d, VALUES[vi], n);

                for (size_t i = 0; i < n; i++) {
                    ok &= d[i] == b;
                }

                check(r == d && ok && guards_hold(dst_buf, GUARD + dof, n),
                      "memset", n, (size_t)VALUES[vi] & 0xffu, dof);
            }
        }
    }
}

static int sign(int v)
{
    return (v > 0) - (v < 0);
}

static int reference_compare(const unsigned char *a, const unsigned char *b,
                             size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }

    return 0;
}

static void compares(void)
{
    for (size_t n = 0; n <= 40; n++) {
        for (size_t ao = 0; ao < 16; ao++) {
            for (size_t bo = 0; bo < 16; bo++) {
                unsigned char *a = src_buf + GUARD + ao;
                unsigned char *b = dst_buf + GUARD + bo;

                fill(a, n, 3);
                fill(b, n, 3);
                check(kosmos_test_memcmp(a, b, n) == 0, "memcmp, equal",
                      n, ao, bo);

                /* A difference at each place, both ways round, with bytes
                 * either side of 127 - and a second, opposite difference
                 * after it, which must not decide. */
                for (size_t j = 0; j < n; j++) {
                    fill(a, n, 3);
                    fill(b, n, 3);
                    a[j] = 0x80;
                    b[j] = 0x7f;

                    if (j + 3 < n) {
                        a[j + 3] = 0x00;
                        b[j + 3] = 0xff;
                    }

                    check(sign(kosmos_test_memcmp(a, b, n))
                          == reference_compare(a, b, n)
                          && sign(kosmos_test_memcmp(b, a, n))
                          == reference_compare(b, a, n)
                          && reference_compare(a, b, n) == 1,
                          "memcmp, the first difference", n, ao, bo);
                }
            }
        }
    }
}

/* Overlapping both ways, at every distance up to twenty and at every
 * alignment of the source, against a copy made through a third buffer. */
static void moves(void)
{
    static unsigned char buf[512];

    for (size_t n = 0; n <= 100; n++) {
        for (int delta = -20; delta <= 20; delta++) {
            for (size_t base = 64; base < 72; base++) {
                size_t from = base, to = (size_t)((long)base + delta);

                fill(buf, sizeof(buf), (unsigned)n);
                fill(want, sizeof(buf), (unsigned)n);

                for (size_t i = 0; i < n; i++) {
                    want[to + i] = buf[from + i];
                }

                void *r = kosmos_test_memmove(buf + to, buf + from, n);

                check(r == buf + to && same(buf, want, sizeof(buf)),
                      "memmove", n, from, to);
            }
        }
    }

    /* And far apart, which is a memcpy. */
    for (size_t li = 0; li < LENGTHS; li++) {
        size_t n = length_at(li);

        fill(src_buf, ROOM, 9);

        for (size_t i = 0; i < ROOM; i++) {
            dst_buf[i] = GUARDED;
        }

        void *r = kosmos_test_memmove(dst_buf + GUARD + 3, src_buf + GUARD, n);

        check(r == dst_buf + GUARD + 3 && same(dst_buf + GUARD + 3, src_buf + GUARD, n)
              && guards_hold(dst_buf, GUARD + 3, n),
              "memmove, apart", n, 0, 3);
    }
}

int main(void)
{
    copies();
    sets();
    compares();
    moves();

#ifdef KOSMOS_TEST_ALIGNMENT
    /* The kernel's copy runs before the MMU, where every access is to
     * Device memory and an unaligned word is a fault: not one, at any of
     * the alignments above. */
    checks++;

    if (misaligned != 0) {
        failures++;
        printf("not ok %d - the kernel's copy touched %ld words at an address "
               "not a multiple of their size\n", checks, misaligned);
    }
#endif

    if (failures == 0) {
        printf("PASS: %d checks on memcpy, memset, memcmp and memmove, %s, "
               "on this machine.\n", checks, SIDE);
        return 0;
    }

    printf("FAIL: %d of %d checks on memcpy, memset, memcmp and memmove, %s.\n",
           failures, checks, SIDE);
    return 1;
}
