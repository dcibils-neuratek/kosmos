/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The audio ring's arithmetic, checked on this machine rather than the target.
 *
 * `tools/test_kfs.lua`, `tools/test_wav.lua` and `tools/test_litexl_surface.c`
 * are here for the same reason: a thing that depends on nothing but C should
 * be tested without booting a machine, because a test that costs thirty
 * seconds and an emulator is a test somebody runs less often.
 *
 * **This one earns it more than most, because its failures are silent.** A
 * ring index that is wrong makes a noise you can hear. A *position* that is
 * wrong makes no noise at all - it is a number, it looks plausible, and the
 * only symptom is a progress bar slightly off or two streams that do not
 * quite line up. There is no version of that which an emulator finds and a
 * host does not.
 *
 * `audioring.h` needs `stddef.h` and `stdint.h` and nothing else - no
 * syscalls, no Kosmos headers, no device - so it compiles here exactly as it
 * does with the cross compiler. That property is worth keeping: it is what
 * makes this file possible.
 *
 * What is modelled below is the whole path a frame takes: a client publishes
 * periods into the ring, the server takes them and hands them to the device,
 * and the device plays them some time later. The thing under test is that
 * `frames_played` counts the last of those three and not the first, which
 * is the entire reason the field exists.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/include/audioring.h"

static int checks;
static int failures;

static void ok(bool cond, const char *what)
{
    checks++;

    if (!cond) {
        failures++;
        printf("not ok %d - %s\n", checks, what);
    }
}

static void eq(uint64_t got, uint64_t want, const char *what)
{
    checks++;

    if (got != want) {
        failures++;
        printf("not ok %d - %s: got %llu, wanted %llu\n", checks, what,
               (unsigned long long)got, (unsigned long long)want);
    }
}

/*
 * The board this test pretends to be. QEMU's numbers, because they are the
 * ones the rest of the tree is written against - a period of 256 frames and
 * a device that holds four of them.
 */
#define PERIOD_FRAMES  256u
#define PERIOD_BYTES   (PERIOD_FRAMES * 4u)     /* stereo, sixteen bits */
#define PERIODS        8u          /* the ring's default depth */
#define DEPTH          4u          /* what the device holds */

/*
 * A machine: the ring, the server's per-stream counter, and the device.
 *
 * The device is one integer because that is all the server can see of it -
 * `hal_snd_queued` returns periods and nothing else, and a model that knew
 * more than the server does would be testing something the server cannot do.
 */
struct machine {
    struct audio_ring *ring;
    uint64_t           consumed;        /* the server's `struct stream` field */
    unsigned           device;          /* periods the device still holds */
};

/*
 * `periods` is an argument rather than the constant, because it stopped
 * being a constant: a client now chooses how deep its own ring is, so a
 * shallow one and a deep one are two shapes this arithmetic has to be right
 * for. Everything below reads `m->ring->periods` rather than the default,
 * which is exactly what the server does.
 */
static struct machine *machine_new(unsigned periods)
{
    struct machine *m = calloc(1, sizeof(*m));
    size_t bytes = AUDIO_RING_DATA + (size_t)periods * PERIOD_BYTES;

    m->ring = calloc(1, bytes);
    m->ring->periods = periods;
    m->ring->period_bytes = PERIOD_BYTES;
    m->ring->magic = AUDIO_RING_MAGIC;
    return m;
}

static void machine_free(struct machine *m)
{
    free(m->ring);
    free(m);
}

/* What `publish_positions` in the server does, and the only place the two
 * halves of the subtraction are both known. */
static void publish(struct machine *m)
{
    audio_ring_position(m->ring,
                        audio_frames_played(m->consumed,
                                            (uint64_t)m->device
                                            * PERIOD_FRAMES));
}

/* The client, handing over periods it has filled. */
static bool client_write(struct machine *m, unsigned periods)
{
    if (audio_ring_space(m->ring) < periods) {
        return false;
    }

    audio_ring_publish(m->ring, m->ring->write + periods);
    return true;
}

/* One turn of `refill`: take a period if there is one and the device has
 * room. Returns whether it did. */
static bool server_mix(struct machine *m)
{
    if (m->device >= DEPTH || audio_ring_ready(m->ring) == 0) {
        return false;
    }

    audio_ring_consumed(m->ring, m->ring->read + 1);
    m->consumed += PERIOD_FRAMES;
    m->device++;
    publish(m);
    return true;
}

/* The device, finishing periods and raising its interrupt. */
static void device_play(struct machine *m, unsigned periods)
{
    while (periods-- > 0 && m->device > 0) {
        m->device--;
    }

    publish(m);
}

int main(void)
{
    /*--------------------------------------------------- the layout itself */
    {
        /*
         * The `_Static_assert`s in the header have already fired at compile
         * time if these are wrong. Saying it again here is what makes the
         * failure legible to somebody reading this output rather than the
         * compiler's, and it costs three lines.
         */
        eq(offsetof(struct audio_ring, write), 16, "write is at 16");
        eq(offsetof(struct audio_ring, read), 20, "read is at 20");
        eq(offsetof(struct audio_ring, frames_played), 24,
           "frames_played is at 24");
        ok(sizeof(struct audio_ring) <= AUDIO_RING_DATA,
           "the header fits before the samples");
    }

    /*------------------------------------------ the subtraction on its own */
    {
        eq(audio_frames_played(0, 0), 0, "nothing taken, nothing heard");
        eq(audio_frames_played(1024, 0), 1024,
           "an empty device means everything taken has been heard");
        eq(audio_frames_played(1024, 1024), 0,
           "a device holding all of it means none of it has been heard");

        /*
         * The clamp, and it is not decoration. `consumed` and the device
         * depth are read at different instants in the server: the device can
         * report a depth from before a period this loop has not yet counted,
         * and the unclamped answer would be an enormous unsigned number
         * rather than a small negative one. That is the exact shape of the
         * two timeout bugs `CLAUDE.md` records.
         */
        eq(audio_frames_played(256, 1024), 0,
           "held more than taken clamps to zero rather than wrapping");
    }

    /*------------------------------------- a period's journey, end to end */
    {
        struct machine *m = machine_new(PERIODS);

        eq(audio_ring_played(m->ring), 0, "a fresh ring has heard nothing");
        eq(audio_ring_delay(m->ring, PERIOD_FRAMES), 0, "and owes nothing");

        ok(client_write(m, 4), "four periods written");
        eq(audio_ring_delay(m->ring, PERIOD_FRAMES), 4 * PERIOD_FRAMES,
           "all four are still in the ring");
        eq(audio_ring_played(m->ring), 0, "and none of them heard");

        /*
         * **The check this whole field exists for.** The server has taken
         * every period, so the ring is empty and a position built from the
         * ring alone would say all four had played. They are sitting in the
         * device. Nothing has been heard yet and the delay has not moved.
         */
        while (server_mix(m)) { }

        eq(audio_ring_ready(m->ring), 0, "the ring is empty");
        eq(audio_ring_played(m->ring), 0,
           "taken is not heard: the device is holding all four");
        eq(audio_ring_delay(m->ring, PERIOD_FRAMES), 4 * PERIOD_FRAMES,
           "so the latency is unchanged by the ring emptying");

        device_play(m, 1);
        eq(audio_ring_played(m->ring), PERIOD_FRAMES, "one period heard");
        eq(audio_ring_delay(m->ring, PERIOD_FRAMES), 3 * PERIOD_FRAMES,
           "and three still to come");

        device_play(m, 3);
        eq(audio_ring_played(m->ring), 4 * PERIOD_FRAMES, "all four heard");
        eq(audio_ring_delay(m->ring, PERIOD_FRAMES), 0, "and nothing owed");

        machine_free(m);
    }

    /*----------------------------------- the device is what bounds latency */
    {
        struct machine *m = machine_new(PERIODS);
        unsigned turn;

        /*
         * A client that fills the ring and keeps it full. The delay settles
         * at the ring plus the device and stays there, which is the number
         * `audiolag` exists to argue about: eight periods of slack and four
         * of device is 46 + 23 ms, and it is a *bound* rather than an
         * average because the ring cannot hold more than it holds.
         */
        for (turn = 0; turn < 200; turn++) {
            while (client_write(m, 1)) { }

            server_mix(m);
            device_play(m, 1);
        }

        ok(audio_ring_delay(m->ring, PERIOD_FRAMES)
           <= (uint64_t)(PERIODS + DEPTH) * PERIOD_FRAMES,
           "a full ring never owes more than the ring plus the device");
        ok(audio_ring_played(m->ring) > 0, "and it is playing");

        machine_free(m);
    }

    /*--------------------------- and the same arithmetic at other depths */
    {
        /*
         * **The depth is the client's choice now, so one depth is not a
         * test.** A video player asks for a deep ring and a game for a
         * shallow one, and the bound has to be the ring plus the device in
         * both cases rather than the default plus the device.
         *
         * Two is the shallowest ring that can hold anything while a period
         * is in flight; 64 is what `ring_create` allows at the top. If this
         * ever passes at 8 and fails at 2, something has the constant baked
         * into it where it should be reading the header.
         */
        static const unsigned depths[] = { 2u, 8u, 32u, 64u };
        unsigned d;

        for (d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
            struct machine *m = machine_new(depths[d]);
            unsigned turn;
            uint64_t bound = (uint64_t)(depths[d] + DEPTH) * PERIOD_FRAMES;

            for (turn = 0; turn < 300; turn++) {
                while (client_write(m, 1)) { }

                server_mix(m);
                device_play(m, 1);
            }

            ok(audio_ring_delay(m->ring, PERIOD_FRAMES) <= bound,
               "a ring of any depth owes at most itself plus the device");
            ok(audio_ring_played(m->ring) > 0, "and it plays at any depth");

            /*
             * And the shallow ring really is shallower. A bound that did not
             * move with the depth would pass every check above by being
             * generous, which is the way this kind of test usually fails.
             */
            eq(audio_ring_space(m->ring) + audio_ring_ready(m->ring),
               depths[d], "the ring is as deep as it was made");

            machine_free(m);
        }
    }

    /*------------------------------------------ what starvation does to it */
    {
        struct machine *m = machine_new(PERIODS);
        uint64_t after;

        /*
         * A stream that stops with its periods already through the device is
         * credited with all of them. This is the ordinary case and it is
         * exact.
         */
        client_write(m, 2);
        while (server_mix(m)) { }
        device_play(m, 2);
        eq(audio_ring_played(m->ring), 2 * PERIOD_FRAMES,
           "a stream that stops is still credited with what it played");

        /*
         * And now the case the header admits to. The server mixed other
         * streams while this one was empty, so the device holds periods this
         * stream is not in - and `publish` subtracts all of them. The
         * position reads *behind* the truth, never ahead, which is the way
         * round that turns a bug into a glitch.
         *
         * Asserted rather than described, because a documented inexactness
         * nothing checks is an inexactness that will quietly get worse.
         */
        m->device = DEPTH;              /* somebody else filled the device */
        publish(m);
        after = audio_ring_played(m->ring);

        ok(after <= 2 * PERIOD_FRAMES,
           "starvation never makes the position run ahead of the truth");
        ok(2 * PERIOD_FRAMES - after <= (uint64_t)DEPTH * PERIOD_FRAMES,
           "and never puts it further behind than the device is deep");

        machine_free(m);
    }

    /*------------------------------------------------- the indices at 2^32 */
    {
        struct machine *m = machine_new(PERIODS);

        /*
         * `write` and `read` are monotonic and unsigned so that the wrap is
         * arithmetic rather than a special case, which the header claims and
         * nothing checked. At 172 periods a second this arrives after about
         * eight months of continuous sound, which is precisely the kind of
         * thing that is never reached by hand and is why it is reached here.
         */
        m->ring->write = 0xFFFFFFFEu;
        m->ring->read = 0xFFFFFFFEu;

        client_write(m, 3);
        eq(audio_ring_ready(m->ring), 3, "three ready across the wrap");
        ok(m->ring->write < 3, "and the index really did wrap");

        server_mix(m);
        eq(audio_ring_ready(m->ring), 2, "two left after one was taken");
        eq(audio_ring_space(m->ring), PERIODS - 2, "and the space agrees");

        machine_free(m);
    }

    if (failures == 0) {
        printf("PASS: %d checks on the audio ring's position, on this "
               "machine.\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the audio ring's position.\n",
           failures, checks);
    return 1;
}
