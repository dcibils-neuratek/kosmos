/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The i8042's drain, with this file playing the controller
 * (`hal/pc/i8042_drain.c`).
 *
 * **The M700's case is the one no emulator here makes**: a machine with no
 * PS/2 port whose firmware answers as a controller until the USB driver
 * takes the controller from it, after which the ports read 0xff. QEMU's
 * q35 has a real i8042 or none, never one that goes away, so the drain that
 * read thirty-two bytes of 0xff at every question - and put the console
 * server at 71% of a processor - is played here instead.
 *
 * And the other side, which matters as much: the ThinkPad's keyboard *is*
 * this controller, so bytes still arrive from a live one, a glitch of 0xff
 * does not retire it, and one good drain starts the count again.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hal/pc/i8042_drain.h"

static int checks, failed;

static void check(bool ok, const char *what)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s\n", what);
    }
}

/*
 * The controller: a list of (status, data) the drain will find, one per
 * status read. A status read past the end finds `rest`.
 */
static const uint8_t *script_status, *script_data;
static unsigned script_len, script_at;
static uint8_t rest;
static unsigned data_reads;

static uint8_t port_in(uint16_t port)
{
    if (port == 0x64) {
        return script_at < script_len ? script_status[script_at] : rest;
    }

    data_reads++;
    return script_at < script_len ? script_data[script_at++] : rest;
}

static uint8_t got[64];
static bool got_aux[64];
static unsigned n_got;

static void take(uint8_t b, bool aux)
{
    if (n_got < 64) {
        got[n_got] = b;
        got_aux[n_got] = aux;
    }

    n_got++;
}

static int drain_with(const uint8_t *status, const uint8_t *data, unsigned len,
                      uint8_t after, unsigned *reads)
{
    script_status = status;
    script_data = data;
    script_len = len;
    script_at = 0;
    rest = after;
    data_reads = 0;
    n_got = 0;

    return i8042_drain_bytes(port_in, take, 32, reads);
}

int main(void)
{
    unsigned reads;
    int how;

    /* Nothing waiting: one read of the status, and nothing handed on. */
    how = drain_with(NULL, NULL, 0, 0x1c, &reads);
    check(how == I8042_DRAIN_EMPTY && reads == 1 && n_got == 0,
          "an empty controller is one status read");

    /* Three keyboard bytes, then empty: A down, A up, and an 0xe0 prefix. */
    {
        static const uint8_t st[] = { 0x1d, 0x1d, 0x1d };
        static const uint8_t da[] = { 0x1e, 0x9e, 0xe0 };

        how = drain_with(st, da, 3, 0x1c, &reads);
        check(how == I8042_DRAIN_EMPTY && reads == 7 && n_got == 3,
              "three keyboard bytes are six reads and the empty status");
        check(got[0] == 0x1e && got[1] == 0x9e && got[2] == 0xe0
              && !got_aux[0] && !got_aux[2], "the keyboard's bytes, in order, "
              "as the keyboard's");
    }

    /* A byte from the auxiliary port is said to be one. */
    {
        static const uint8_t st[] = { 0x3d };
        static const uint8_t da[] = { 0x08 };

        how = drain_with(st, da, 1, 0x1c, &reads);
        check(how == I8042_DRAIN_EMPTY && n_got == 1 && got_aux[0],
              "an auxiliary byte is marked as the pointer's");
    }

    /*
     * **The M700**: the bus floats. One read, no byte - where the drain
     * before this took 0xff as a byte thirty-two times over, sixty-four
     * reads at every question.
     */
    how = drain_with(NULL, NULL, 0, 0xff, &reads);
    check(how == I8042_DRAIN_FLOATS && reads == 1 && n_got == 0
          && data_reads == 0, "a floating bus is one read and no byte");

    /* Two real bytes, then the bus goes: both kept, then it says so. */
    {
        static const uint8_t st[] = { 0x1d, 0x1d };
        static const uint8_t da[] = { 0x1e, 0x9e };

        how = drain_with(st, da, 2, 0xff, &reads);
        check(how == I8042_DRAIN_FLOATS && n_got == 2 && reads == 5,
              "bytes before the bus floated are kept");
    }

    /* A controller that never empties stops at the bound, as it did. */
    how = drain_with(NULL, NULL, 0, 0x1d, &reads);
    check(how == I8042_DRAIN_BOUND && n_got == 32 && reads == 64,
          "a controller that never empties stops at thirty-two bytes");

    /*
     * **Gone only when it stays gone.** A glitch of 63 floating drains
     * leaves the ThinkPad its keyboard; a good one starts again; the 64th
     * in a row says gone, once.
     */
    {
        struct i8042_watch w;
        unsigned i;
        bool early = false;

        memset(&w, 0, sizeof(w));

        for (i = 0; i < I8042_GONE_AFTER - 1; i++) {
            early |= i8042_gone(&w, I8042_DRAIN_FLOATS);
        }

        check(!early, "63 floating drains in a row are not gone");
        check(!i8042_gone(&w, I8042_DRAIN_EMPTY),
              "a drain that did not float is not gone");

        for (i = 0; i < I8042_GONE_AFTER - 1; i++) {
            early |= i8042_gone(&w, I8042_DRAIN_FLOATS);
        }

        check(!early, "and it started the count again");
        check(i8042_gone(&w, I8042_DRAIN_FLOATS), "the 64th in a row is gone");
        check(!i8042_gone(&w, I8042_DRAIN_BOUND)
              && !i8042_gone(&w, I8042_DRAIN_EMPTY),
              "a full controller and an empty one are there");
    }

    if (failed) {
        printf("FAIL: %d of %d checks on the i8042's drain\n", failed, checks);
        return 1;
    }

    printf("PASS: %d checks on the i8042's drain (an empty controller one "
           "read; keyboard and pointer bytes kept and told apart; a floating "
           "bus one read and no byte, where it was sixty-four; a controller "
           "that never empties bounded at 32; gone only after 64 floating "
           "drains in a row, and a good one starting the count again).\n",
           checks);
    return 0;
}
