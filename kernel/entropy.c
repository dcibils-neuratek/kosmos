/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The hardware's randomness, held to a health test before any process sees
 * a byte of it (`SYS_ENTROPY`).
 *
 * **Why the kernel, which does not know what a file is**: randomness is a
 * device read, like a key or the clock, and `hal_entropy` is where the
 * device is. What a program does with it - a generator, a key - is the
 * Crypto Kit's, in the program's own process (`crypto.random`). This file is
 * the one thing between the two: a check that the source still works.
 *
 * **A broken source still answers, and answers the same thing**, which is
 * why the check is this one: SP 800-90B's repetition count test, on 64-bit
 * words. A source with even a bit of entropy in each of them repeats a
 * word by chance about once in 2^64; a stuck one - a processor whose RDRAND
 * has failed to all ones, a device handing back a zeroed buffer - repeats
 * on the next word. So a word equal to the one before it retires the
 * source, from then on, and says so once. Nothing is handed out that
 * failed, and nothing is quietly replaced with something that did not.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "console.h"
#include "spinlock.h"
#include "entropy.h"

static struct spinlock entropy_lock = SPINLOCK("entropy");
static bool     present, failed;
static bool     have_last;
static uint64_t last;

bool entropy_init(void)
{
    present = hal_entropy_init();
    return present;
}

size_t entropy_read(void *buf, size_t bytes)
{
    uint8_t words[ENTROPY_MAX];
    size_t got, at, rounded;
    unsigned long flags;

    if (bytes == 0 || bytes > ENTROPY_MAX) {
        return 0;
    }

    flags = spin_lock(&entropy_lock);

    if (!present || failed) {
        spin_unlock(&entropy_lock, flags);
        return 0;
    }

    /* Whole words, so each is tested: rounded up, the rest dropped after.
     * A source that fills less than it was asked is no answer - the words
     * it did not write would be tested, and handed out, as though it had. */
    rounded = (bytes + 7u) & ~(size_t)7u;
    got = hal_entropy(words, rounded);

    if (got < rounded) {
        got = 0;
    }

    for (at = 0; got != 0 && at + 8 <= rounded; at += 8) {
        uint64_t w;

        memcpy(&w, words + at, sizeof(w));

        if (have_last && w == last) {
            failed = true;
            got = 0;
            kputs("entropy: the source gave the same word twice running; "
                  "retired, and nothing more is handed out\n");
            break;
        }

        last = w;
        have_last = true;
    }

    if (got != 0) {
        memcpy(buf, words, bytes);
    }

    memset(words, 0, sizeof(words));
    spin_unlock(&entropy_lock, flags);

    return got != 0 ? bytes : 0;
}
