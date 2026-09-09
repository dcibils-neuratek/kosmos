/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Intel High Definition Audio, output only.
 *
 *--------------------------------------------------------------------------
 * Where the numbers come from.
 *
 * The Intel High Definition Audio Specification, revision 1.0a: section 3.3
 * for the controller's register file, 3.6 for the ring buffers the codec is
 * addressed through, and section 7 for the verbs and parameters. Nothing
 * here is from memory - `CLAUDE.md` asks for the datasheet where an offset
 * is at stake, and this file is eighty offsets.
 *
 *--------------------------------------------------------------------------
 * The shape of the thing, because it is not virtio's shape.
 *
 * A virtio sound device is a *queue*: the driver hands over a period, the
 * device consumes it, and the queue depth is the answer to "how much is in
 * hand". HDA is a **cyclic buffer that never stops**. The controller walks
 * a list of descriptors for ever, playing whatever bytes are under them at
 * the moment it arrives - so there is no such thing as handing something
 * over, and no such thing as running out. There is only being late.
 *
 * Everything below follows from that:
 *
 * - **The read position is the hardware's**, in `SDLPIB`, and this driver
 *   derives its depth from it rather than from a counter it keeps. A
 *   counter and a device can disagree; two interrupts coalescing into one
 *   is all it takes, and the symptom is a driver writing into the period
 *   the hardware is reading.
 * - **One slot is always left free**, which is what makes the depth
 *   unambiguous: `write - play` modulo the ring is zero for empty and never
 *   wraps to zero for full. `hal/virtio/snd.c` keeps a slot free for the
 *   same reason and says so in one line.
 * - **A finished period is zeroed**, in the interrupt handler. A queue that
 *   runs dry goes quiet; a ring that runs dry *repeats*, and the sound of
 *   an underrun here would be the last 5.8 ms of audio played over and over
 *   at 172 Hz. Zeroing costs a kilobyte of stores every period and turns
 *   that into silence.
 *
 * **The stream runs from initialisation and is never stopped**, which is a
 * real cost on a laptop: 172 interrupts a second for as long as the machine
 * is on, whether or not anything is playing. It buys the thing the audio
 * server is built around - a device that says when it wants more - and the
 * fix when it matters is named rather than guessed: stop `RUN` after some
 * number of consecutive silent periods and start it again on the next
 * write. That is a state machine, and it is not worth writing before there
 * is a battery to measure it against.
 *
 *--------------------------------------------------------------------------
 * What is proven and where.
 *
 * Written against `qemu-system-x86_64 -device intel-hda -device hda-output`,
 * which is a faithful implementation of the controller and a deliberately
 * simple codec: one function group, one DAC, one output pin, and a
 * connection list with a single entry in it. A real codec - the ALC257 in
 * the machine `docs/thinkpad.md` is aimed at - has a dozen widgets, several
 * pins, and mixers between them, and the graph walk below is written for
 * that case rather than for the easy one. What cannot be tested here is
 * which of its pins is wired to the speakers, because QEMU's codec has one
 * pin and every answer is the same answer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "hda.h"
#include "mmio.h"
#include "mmu.h"
#include "pc.h"
#include "pci.h"
#include "spinlock.h"

/*------------------------------------------------------------------------
 * The controller's registers. Specification section 3.3.
 *----------------------------------------------------------------------*/

#define GCAP            0x00        /* 16: how many streams of each kind */
#define GCTL            0x08        /* 32 */
#define STATESTS        0x0e        /* 16: which SDIN lines have a codec */
#define INTCTL          0x20        /* 32 */

#define CORBLBASE       0x40
#define CORBUBASE       0x44
#define CORBWP          0x48        /* 16 */
#define CORBRP          0x4a        /* 16 */
#define CORBCTL         0x4c        /* 8  */
#define CORBSTS         0x4d        /* 8  */
#define CORBSIZE        0x4e        /* 8  */

#define RIRBLBASE       0x50
#define RIRBUBASE       0x54
#define RIRBWP          0x58        /* 16 */
#define RINTCNT         0x5a        /* 16 */
#define RIRBCTL         0x5c        /* 8  */
#define RIRBSTS         0x5d        /* 8  */
#define RIRBSIZE        0x5e        /* 8  */

#define GCTL_CRST       (1u << 0)   /* 0 is reset asserted, 1 is running */

#define INTCTL_GIE      (1u << 31)

#define CORBCTL_RUN     (1u << 1)
#define CORBRP_RST      (1u << 15)

#define RIRBCTL_INT     (1u << 0)   /* raise a status when RINTCNT lands */
#define RIRBCTL_DMA     (1u << 1)
#define RIRBWP_RST      (1u << 15)

/* Stream descriptors begin here, 0x20 bytes apart, input streams first. */
#define STREAM_BASE     0x80
#define STREAM_STRIDE   0x20

#define SD_CTL          0x00        /* 32, of which 24 are defined */
#define SD_STS          0x03        /* 8 */
#define SD_LPIB         0x04        /* 32: where the hardware has read to */
#define SD_CBL          0x08        /* 32: the length of the whole ring */
#define SD_LVI          0x0c        /* 16: the last valid BDL index */
#define SD_FMT          0x12        /* 16 */
#define SD_BDPL         0x18
#define SD_BDPU         0x1c

#define SD_CTL_SRST     (1u << 0)
#define SD_CTL_RUN      (1u << 1)
#define SD_CTL_IOCE     (1u << 2)   /* interrupt when a descriptor completes */
#define SD_CTL_STRM     20          /* the stream tag lives at bits 23:20 */

#define SD_STS_BCIS     (1u << 2)   /* a buffer completed */
#define SD_STS_FIFOE    (1u << 3)
#define SD_STS_DESE     (1u << 4)
#define SD_STS_CLEAR    (SD_STS_BCIS | SD_STS_FIFOE | SD_STS_DESE)

/*
 * The stream tag, which is not the stream index.
 *
 * The index says which descriptor register file to use; the tag is a number
 * on the *link*, and the codec is told to listen for it. They are
 * independent and the specification says tag 0 means "no stream", so this
 * is 1 and would be 1 on a machine with sixteen streams.
 */
#define STREAM_TAG      1

/*
 * 44100 Hz, sixteen bits, two channels, in the one register that says so.
 *
 * Bit 14 selects the 44.1 kHz base rather than 48 kHz; the multiplier and
 * divider fields are both "by one"; bits 6:4 are 001 for sixteen bits; and
 * the channel field is the count minus one. Section 3.3.41, and the same
 * sixteen bits go to the codec in a `SET_CONVERTER_FORMAT` verb - which is
 * the point of the format being a register value rather than a struct.
 */
#define FORMAT_44100_S16_STEREO 0x4011u

/*------------------------------------------------------------------------
 * Codec verbs. Specification section 7.3.
 *----------------------------------------------------------------------*/

#define VERB_GET_PARAMETER      0xf00u
#define VERB_GET_CONNECTIONS    0xf02u
#define VERB_GET_CONFIG_DEFAULT 0xf1cu
#define VERB_SET_CONNECTION_SEL 0x701u
#define VERB_SET_POWER_STATE    0x705u
#define VERB_SET_STREAM_CHANNEL 0x706u
#define VERB_SET_PIN_CONTROL    0x707u

/* The two verbs with a sixteen-bit payload, whose opcode is four bits. */
#define VERB_SET_FORMAT         0x2u
#define VERB_SET_AMP            0x3u

#define PARAM_SUBNODES          0x04u
#define PARAM_FUNCTION_TYPE     0x05u
#define PARAM_WIDGET_CAPS       0x09u
#define PARAM_PIN_CAPS          0x0cu
#define PARAM_CONNECTION_LEN    0x0eu
#define PARAM_IN_AMP_CAPS       0x0du
#define PARAM_OUT_AMP_CAPS      0x12u

#define FUNCTION_AUDIO          0x01u

/* Widget capabilities, bits 23:20 and the flags below them. */
#define WIDGET_TYPE(caps)       (((caps) >> 20) & 0xfu)
#define WIDGET_DAC              0x0u
#define WIDGET_MIXER            0x2u
#define WIDGET_SELECTOR         0x3u
#define WIDGET_PIN              0x4u

#define WIDGET_OUT_AMP          (1u << 2)
#define WIDGET_AMP_OVERRIDE     (1u << 3)
#define WIDGET_CONN_LIST        (1u << 8)
#define WIDGET_POWER_CONTROL    (1u << 10)

#define PIN_CAP_HEADPHONE       (1u << 3)
#define PIN_CAP_OUTPUT          (1u << 4)

#define PIN_CTL_OUT_ENABLE      (1u << 6)
#define PIN_CTL_HP_ENABLE       (1u << 7)

/*
 * The configuration default's top two bits: how the pin is connected.
 *
 * 3 means "not connected to anything" - a pin the board designer brought
 * out of the codec and wired to nothing - and it is the one value that
 * disqualifies a pin outright. Everything else is a jack, a fixed internal
 * device (which is what a laptop's speakers are), or both.
 */
#define CONFIG_PORT_NONE        3u

/*------------------------------------------------------------------------
 * The ring, and how deep it is.
 *----------------------------------------------------------------------*/

/*
 * Eight periods here against the HAL's four, and the difference is not
 * generosity.
 *
 * Two of these are unusable by construction: the hardware is reading one,
 * and one is left free so that "write equals play" can mean empty rather
 * than full. Six remain, which is comfortably more than the four
 * `HAL_SND_PERIODS` promises the audio server - so the server's own limit
 * is what governs, and this driver never refuses a period the server
 * thought it was allowed to send.
 *
 * Eight periods is 8 KB and 46 ms of sound. It is static because a
 * descriptor holds a physical address the controller reads whenever it
 * likes, which is the same sentence `hal/virtio/snd.c` writes about its own
 * buffers, and 128-byte aligned because the specification requires it of
 * every structure the controller fetches.
 */
#define RING_PERIODS    8u
#define RING_BYTES      (RING_PERIODS * HAL_SND_PERIOD_BYTES)

/* 256 entries each, which is the largest size the specification defines and
 * the one every controller supports. The capability field is read anyway,
 * because a device that says it cannot is a device this must not program. */
#define CORB_ENTRIES    256u
#define RIRB_ENTRIES    256u

struct bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;             /* bit 0: interrupt on completion */
} __attribute__((packed));

struct rirb_entry {
    uint32_t response;
    uint32_t extended;          /* bits 3:0 the codec, bit 4 unsolicited */
} __attribute__((packed));

static uint32_t corb[CORB_ENTRIES]             __attribute__((aligned(128)));
static struct rirb_entry rirb[RIRB_ENTRIES]    __attribute__((aligned(128)));
static struct bdl_entry bdl[RING_PERIODS]      __attribute__((aligned(128)));
static uint8_t ring[RING_BYTES]                __attribute__((aligned(128)));

/*
 * The device's lock, and it guards exactly what `hal/virtio/snd.c`'s does:
 * a writer arriving from a syscall and a handler arriving from an interrupt,
 * both touching the same ring position.
 */
static struct spinlock hda_lock = SPINLOCK("hda");

static struct {
    bool     present;
    uintptr_t base;             /* the mapped register file */
    uintptr_t sd;               /* the output stream descriptor */
    unsigned  stream_index;     /* which descriptor, for INTCTL */
    unsigned  irq;

    unsigned  codec;
    unsigned  dac;
    unsigned  pin;

    unsigned  corb_wp;
    unsigned  rirb_rp;

    unsigned  write_slot;

    bool              primed;
    volatile bool     wants;
    volatile unsigned woke;
    unsigned          dry;
    unsigned          floor;
} hda;

static const char *description = "no HDA controller on the bus";

/*------------------------------------------------------------------------
 * Register access.
 *
 * `mmio.h` has 8 and 32; HDA needs 16 for half its register file, and this
 * is the same three lines `hal/pc/virtio.c` writes for the same reason.
 *----------------------------------------------------------------------*/

static uint16_t read16(uintptr_t at)
{
    uint16_t v;

    __asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(at) : "memory");

    return v;
}

static void write16(uintptr_t at, uint16_t value)
{
    __asm__ volatile("movw %0, (%1)" :: "r"(value), "r"(at) : "memory");
}

/*
 * Everything written is in memory before the controller is told to look.
 *
 * The same `mfence` and the same argument as `hal/virtio/snd.c`: TSO gives
 * the ordering for free, and what this asks for is that the samples have
 * *left* the store buffer before a descriptor pointing at them is armed.
 */
static void publish(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/*
 * How long to wait for a bit to change, in reads.
 *
 * A number rather than a clock, for `i8042.c`'s reason: sound is brought up
 * from `kernel/main.c` and a driver that needed a working timer in order to
 * report that it had failed would be no use at all. Generous - a real
 * controller answers a reset in microseconds.
 */
#define PATIENCE    500000u

/*------------------------------------------------------------------------
 * The command ring, which is how a codec is spoken to at all.
 *----------------------------------------------------------------------*/

/*
 * A verb, assembled. Specification section 7.3.1.
 *
 * Two shapes and the caller does not get to confuse them: a twelve-bit
 * opcode carries eight bits of payload, and a four-bit opcode carries
 * sixteen. `SET_CONVERTER_FORMAT` is the second kind, which is why it is 2
 * rather than 0x200 - and getting that wrong produces a codec that answers
 * nothing rather than one that complains.
 */
static uint32_t verb8(unsigned nid, unsigned opcode, unsigned payload)
{
    return ((uint32_t)hda.codec << 28) | ((uint32_t)nid << 20)
         | ((uint32_t)opcode << 8) | (uint32_t)(payload & 0xffu);
}

static uint32_t verb16(unsigned nid, unsigned opcode, unsigned payload)
{
    return ((uint32_t)hda.codec << 28) | ((uint32_t)nid << 20)
         | ((uint32_t)opcode << 16) | (uint32_t)(payload & 0xffffu);
}

/*
 * One verb out, one response back, and it blocks.
 *
 * Every caller is initialisation, so blocking is free and the alternative -
 * a state machine over a ring that is only ever one deep in practice - buys
 * nothing. Unsolicited responses are the thing that would break this, since
 * they arrive in the same ring without anybody asking; they stay off, which
 * is `GCTL.UNSOL` left clear.
 *
 * `0xffffffff` for a codec that did not answer, which is what an absent
 * codec reads as anyway and is never a valid parameter response.
 */
#define CODEC_NO_ANSWER 0xffffffffu

static uint32_t command(uint32_t verb)
{
    unsigned n;
    unsigned wp = (hda.corb_wp + 1) % CORB_ENTRIES;

    corb[wp] = verb;
    publish();

    hda.corb_wp = wp;
    write16(hda.base + CORBWP, (uint16_t)wp);

    for (n = 0; n < PATIENCE; n++) {
        unsigned hw = read16(hda.base + RIRBWP) & (RIRB_ENTRIES - 1u);

        if (hw != hda.rirb_rp) {
            uint32_t response;

            hda.rirb_rp = (hda.rirb_rp + 1) % RIRB_ENTRIES;
            response = rirb[hda.rirb_rp].response;

            /*
             * Write-one-to-clear, and it is what lets the *next* verb be
             * answered at all rather than merely being tidy: the paragraph
             * in `rings_start` about `RINTCNT` is this line's other half.
             * Bit 0 is the response status and bit 2 the overrun.
             */
            mmio_write8(hda.base + RIRBSTS, 0x05);

            return response;
        }
    }


    return CODEC_NO_ANSWER;
}

static uint32_t get_param(unsigned nid, unsigned param)
{
    return command(verb8(nid, VERB_GET_PARAMETER, param));
}

static bool rings_start(void)
{
    unsigned n;
    uint8_t cap;

    /* Both stopped before either is moved. A DMA engine reading a base
     * address that is being written is the one failure here that does not
     * announce itself. */
    mmio_write8(hda.base + CORBCTL, 0);
    mmio_write8(hda.base + RIRBCTL, 0);

    cap = mmio_read8(hda.base + CORBSIZE);

    /* Bits 7:4 say which sizes exist; bit 6 is the 256-entry one. A
     * controller without it would need a smaller ring here, and there is no
     * such controller - so this refuses rather than pretending. */
    if ((cap & 0x40u) == 0) {
        return false;
    }

    mmio_write8(hda.base + CORBSIZE, (uint8_t)((cap & 0xf0u) | 0x02u));

    cap = mmio_read8(hda.base + RIRBSIZE);

    if ((cap & 0x40u) == 0) {
        return false;
    }

    mmio_write8(hda.base + RIRBSIZE, (uint8_t)((cap & 0xf0u) | 0x02u));

    /* The kernel is identity mapped, so the address of a static array is
     * already the physical address the controller needs - and the upper
     * halves are zero because everything this kernel maps is below 4 GB.
     * `arch/x86_64/mmu.h` is where that stops being true. */
    mmio_write32(hda.base + CORBLBASE, (uint32_t)(uintptr_t)corb);
    mmio_write32(hda.base + CORBUBASE, 0);
    mmio_write32(hda.base + RIRBLBASE, (uint32_t)(uintptr_t)rirb);
    mmio_write32(hda.base + RIRBUBASE, 0);

    /*
     * Resetting the read pointer is a handshake rather than a write: set
     * the bit, wait for the controller to agree it is set, clear it, wait
     * again. Both waits are bounded and neither failing is fatal - some
     * controllers famously do not read the bit back, and the pointer is at
     * zero either way after a controller reset.
     */
    write16(hda.base + CORBRP, CORBRP_RST);

    for (n = 0; n < PATIENCE; n++) {
        if ((read16(hda.base + CORBRP) & CORBRP_RST) != 0) {
            break;
        }
    }

    write16(hda.base + CORBRP, 0);

    for (n = 0; n < PATIENCE; n++) {
        if ((read16(hda.base + CORBRP) & CORBRP_RST) == 0) {
            break;
        }
    }

    write16(hda.base + CORBWP, 0);
    hda.corb_wp = 0;

    /* The response ring's write pointer resets by writing the bit, with no
     * handshake; the specification says it clears itself. */
    write16(hda.base + RIRBWP, RIRBWP_RST);
    hda.rirb_rp = 0;

    /*
     * One response per interrupt, and **the interrupt is enabled even
     * though every verb here is polled.**
     *
     * This looks redundant and is load-bearing. `RINTCNT` is not only an
     * interrupt threshold: it is also how many responses the controller
     * will write before it considers the response ring full and stops
     * consuming commands. What restarts it is the driver acknowledging
     * `RIRBSTS` - and with the interrupt disabled there is no status to
     * acknowledge, so the count never clears and the controller answers
     * exactly one verb and then nothing, for ever.
     *
     * It cost an afternoon: `GET_PARAMETER` on the root node answered
     * correctly, the same call on the function group underneath it timed
     * out, and the registers said the command pointer had moved and the
     * controller's had not.
     *
     * Nothing is raised on the line by this, because `INTCTL.CIE` - the
     * controller's own interrupt enable, as distinct from the streams' -
     * stays clear. The status bit is set, `command` clears it after every
     * response, and the pin never moves.
     */
    write16(hda.base + RINTCNT, 1);

    publish();

    mmio_write8(hda.base + CORBCTL, CORBCTL_RUN);
    mmio_write8(hda.base + RIRBCTL, RIRBCTL_DMA | RIRBCTL_INT);

    return true;
}

/*------------------------------------------------------------------------
 * The codec graph.
 *----------------------------------------------------------------------*/

/*
 * The gain to write, in the units this widget uses.
 *
 * An amplifier's range is its own: the capabilities word says how many
 * steps it has and where 0 dB sits among them, and the same number means a
 * different loudness on two codecs. **The offset field is 0 dB**, which is
 * the one setting that means the same thing everywhere - so that is what
 * gets written, rather than the maximum, which on a laptop's speaker
 * amplifier is distortion.
 *
 * A widget whose capabilities say nothing borrows the function group's,
 * unless it set the override bit to say it has its own. Section 7.3.4.10.
 */
static unsigned amp_gain(unsigned nid, unsigned caps, unsigned group,
                         unsigned which)
{
    uint32_t amp = get_param((caps & WIDGET_AMP_OVERRIDE) ? nid : group,
                             which);

    if (amp == CODEC_NO_ANSWER || amp == 0) {
        return 0;
    }

    return (unsigned)(amp & 0x7fu);
}

/* Unmutes an output amplifier, both channels, at 0 dB. Bit 15 selects the
 * output amp, 13 and 12 the two channels, and bit 7 clear is unmuted. */
static void unmute(unsigned nid, unsigned caps, unsigned group)
{
    if ((caps & WIDGET_OUT_AMP) == 0) {
        return;
    }

    (void)command(verb16(nid, VERB_SET_AMP,
                         0xb000u | amp_gain(nid, caps, group,
                                            PARAM_OUT_AMP_CAPS)));
}

/* How many widgets this widget can take its input from, and which. */
static unsigned connections(unsigned nid, unsigned *out, unsigned max)
{
    uint32_t length = get_param(nid, PARAM_CONNECTION_LEN);
    bool wide;
    unsigned count, i, n = 0;

    if (length == CODEC_NO_ANSWER) {
        return 0;
    }

    /* Bit 7 says the entries are sixteen bits rather than eight, which a
     * codec with more than 128 widgets needs and QEMU's does not have. */
    wide = (length & 0x80u) != 0;
    count = (unsigned)(length & 0x7fu);

    for (i = 0; i < count && n < max; i++) {
        uint32_t word = command(verb8(nid, VERB_GET_CONNECTIONS,
                                           wide ? (i & ~1u) : (i & ~3u)));

        if (word == CODEC_NO_ANSWER) {
            return n;
        }

        if (wide) {
            out[n++] = (word >> ((i & 1u) * 16u)) & 0x7fffu;
        } else {
            out[n++] = (word >> ((i & 3u) * 8u)) & 0x7fu;
        }
    }

    return n;
}

/*
 * A path from this pin back to a converter, one widget deep.
 *
 * **Two levels is what real codecs need and one is what QEMU has.** The
 * emulated codec wires its pin straight to its DAC, so a driver written
 * against it alone would find the DAC in the pin's own connection list and
 * stop there - and then find nothing at all on a laptop, where the pin's
 * list holds a mixer and the mixer's list holds the converter.
 *
 * Selecting is only meaningful on a selector or a pin, which take one input
 * at a time; a mixer sums all of its inputs and has nothing to select, so
 * what it needs instead is the input unmuted. Both are done, because a
 * widget that is one and not the other ignores the wrong one.
 */
static bool route(unsigned pin, unsigned dac, unsigned group)
{
    unsigned first[16];
    unsigned n = connections(pin, first, 16);
    unsigned i;

    for (i = 0; i < n; i++) {
        if (first[i] == dac) {
            (void)command(verb8(pin, VERB_SET_CONNECTION_SEL, i));
            return true;
        }
    }

    for (i = 0; i < n; i++) {
        unsigned mid = first[i];
        uint32_t caps = get_param(mid, PARAM_WIDGET_CAPS);
        unsigned second[16];
        unsigned m, j;

        if (caps == CODEC_NO_ANSWER) {
            continue;
        }

        if (WIDGET_TYPE(caps) != WIDGET_MIXER
            && WIDGET_TYPE(caps) != WIDGET_SELECTOR) {
            continue;
        }

        m = connections(mid, second, 16);

        for (j = 0; j < m; j++) {
            if (second[j] != dac) {
                continue;
            }

            (void)command(verb8(pin, VERB_SET_CONNECTION_SEL, i));
            (void)command(verb8(mid, VERB_SET_CONNECTION_SEL, j));

            /*
             * Unmute the mixer's input for that entry: bit 14 selects the
             * input amp and bits 11:8 say which of its inputs. At its own
             * 0 dB, for the reason `amp_gain` gives - a gain of zero here
             * is the bottom of that amplifier's range on most codecs, which
             * is a path that is connected, unmuted, and inaudible.
             */
            (void)command(verb16(mid, VERB_SET_AMP,
                                 0x7000u | (j << 8)
                                 | amp_gain(mid, (unsigned)caps, group,
                                            PARAM_IN_AMP_CAPS)));
            unmute(mid, (unsigned)caps, group);

            return true;
        }
    }

    return false;
}

/*
 * Walks the codec and picks a converter and a pin to drive.
 *
 * **The first output pin that is connected to something**, and the ordering
 * is the codec's rather than a preference of this driver's. A laptop lists
 * its internal speaker before its headphone jack, so first is usually
 * right; when it is not, what decides is the configuration default's device
 * type, and that is a table of fifteen values to write on the day there is
 * a machine to be wrong about.
 */
static bool find_widgets(void)
{
    uint32_t roots = get_param(0, PARAM_SUBNODES);
    unsigned start, count, fg;

    if (roots == CODEC_NO_ANSWER) {
        return false;
    }

    start = (unsigned)((roots >> 16) & 0xffu);
    count = (unsigned)(roots & 0xffu);

    for (fg = start; fg < start + count; fg++) {
        uint32_t type = get_param(fg, PARAM_FUNCTION_TYPE);
        uint32_t nodes;
        unsigned wstart, wcount, w;

        if (type == CODEC_NO_ANSWER || (type & 0xffu) != FUNCTION_AUDIO) {
            continue;
        }

        /* D0, before anything else is asked of it. A function group left in
         * D3 answers its parameters and produces no sound, which is the
         * most convincing kind of silence there is. */
        (void)command(verb8(fg, VERB_SET_POWER_STATE, 0));

        nodes = get_param(fg, PARAM_SUBNODES);

        if (nodes == CODEC_NO_ANSWER) {
            continue;
        }

        wstart = (unsigned)((nodes >> 16) & 0xffu);
        wcount = (unsigned)(nodes & 0xffu);

        for (w = wstart; w < wstart + wcount; w++) {
            uint32_t caps = get_param(w, PARAM_WIDGET_CAPS);


            if (caps == CODEC_NO_ANSWER) {
                continue;
            }

            if ((caps & WIDGET_POWER_CONTROL) != 0) {
                (void)command(verb8(w, VERB_SET_POWER_STATE, 0));
            }

            if (WIDGET_TYPE(caps) == WIDGET_DAC && hda.dac == 0) {
                hda.dac = w;
                unmute(w, (unsigned)caps, fg);
            }

            if (WIDGET_TYPE(caps) == WIDGET_PIN && hda.pin == 0) {
                uint32_t pin = get_param(w, PARAM_PIN_CAPS);
                uint32_t config;

                if (pin == CODEC_NO_ANSWER || (pin & PIN_CAP_OUTPUT) == 0) {
                    continue;
                }

                config = command(verb8(w, VERB_GET_CONFIG_DEFAULT, 0));

                if (config != CODEC_NO_ANSWER
                    && ((config >> 30) & 3u) == CONFIG_PORT_NONE) {
                    continue;       /* brought out of the codec and wired
                                     * to nothing */
                }

                hda.pin = w;

                (void)command(verb8(w, VERB_SET_PIN_CONTROL,
                                       PIN_CTL_OUT_ENABLE
                                       | ((pin & PIN_CAP_HEADPHONE)
                                          ? PIN_CTL_HP_ENABLE : 0u)));
                unmute(w, (unsigned)caps, fg);
            }
        }

        if (hda.dac != 0 && hda.pin != 0) {
            (void)route(hda.pin, hda.dac, fg);
            return true;
        }
    }

    return false;
}

/*------------------------------------------------------------------------
 * The stream.
 *----------------------------------------------------------------------*/

static void stream_setup(void)
{
    unsigned n, i;

    /* Stopped, then reset, and the reset is a handshake in the same shape
     * as the CORB's: assert, wait for it to be seen, release, wait again. */
    mmio_write32(hda.sd + SD_CTL, 0);

    for (n = 0; n < PATIENCE; n++) {
        if ((mmio_read32(hda.sd + SD_CTL) & SD_CTL_RUN) == 0) {
            break;
        }
    }

    mmio_write32(hda.sd + SD_CTL, SD_CTL_SRST);

    for (n = 0; n < PATIENCE; n++) {
        if ((mmio_read32(hda.sd + SD_CTL) & SD_CTL_SRST) != 0) {
            break;
        }
    }

    mmio_write32(hda.sd + SD_CTL, 0);

    for (n = 0; n < PATIENCE; n++) {
        if ((mmio_read32(hda.sd + SD_CTL) & SD_CTL_SRST) == 0) {
            break;
        }
    }

    for (i = 0; i < RING_PERIODS; i++) {
        bdl[i].address = (uint64_t)(uintptr_t)&ring[i * HAL_SND_PERIOD_BYTES];
        bdl[i].length = HAL_SND_PERIOD_BYTES;
        bdl[i].flags = 1u;      /* interrupt on completion, on every one */
    }

    memset(ring, 0, sizeof(ring));

    mmio_write32(hda.sd + SD_CBL, RING_BYTES);
    write16(hda.sd + SD_LVI, (uint16_t)(RING_PERIODS - 1u));
    write16(hda.sd + SD_FMT, FORMAT_44100_S16_STEREO);
    mmio_write32(hda.sd + SD_BDPL, (uint32_t)(uintptr_t)bdl);
    mmio_write32(hda.sd + SD_BDPU, 0);
    mmio_write8(hda.sd + SD_STS, SD_STS_CLEAR);

    publish();

    /* Tagged and armed, and then started - two writes rather than one so
     * that nothing is running while the tag is half written. */
    mmio_write32(hda.sd + SD_CTL,
                 ((uint32_t)STREAM_TAG << SD_CTL_STRM) | SD_CTL_IOCE);
    mmio_write32(hda.sd + SD_CTL,
                 ((uint32_t)STREAM_TAG << SD_CTL_STRM) | SD_CTL_IOCE
                 | SD_CTL_RUN);
}

/*------------------------------------------------------------------------
 * Bringing it up.
 *----------------------------------------------------------------------*/

static bool reset_controller(void)
{
    unsigned n;

    /* Into reset, and out of it. The controller clears CRST itself when the
     * link is down, so both directions are waited on. */
    mmio_write32(hda.base + GCTL, 0);

    for (n = 0; n < PATIENCE; n++) {
        if ((mmio_read32(hda.base + GCTL) & GCTL_CRST) == 0) {
            break;
        }
    }

    mmio_write32(hda.base + GCTL, GCTL_CRST);

    for (n = 0; n < PATIENCE; n++) {
        if ((mmio_read32(hda.base + GCTL) & GCTL_CRST) != 0) {
            break;
        }
    }

    if ((mmio_read32(hda.base + GCTL) & GCTL_CRST) == 0) {
        return false;
    }

    /*
     * And then wait, which is the part that looks like superstition and is
     * not. The codecs on the link need 25 frames - 521 microseconds - to
     * announce themselves in `STATESTS`, and reading it immediately reads
     * zero on hardware that has four of them. There is no bit to poll; the
     * specification gives a duration. This is a spin because the timer is
     * not necessarily up yet, and it is generous for the same reason
     * `PATIENCE` is.
     */
    for (n = 0; n < PATIENCE; n++) {
        if (read16(hda.base + STATESTS) != 0) {
            break;
        }
    }

    return true;
}

bool hda_init(void)
{
    struct pci_device dev;
    uint16_t gcap, states;
    unsigned inputs, outputs, i;

    /* Class 4 subclass 3 is High Definition Audio wherever it is fitted,
     * which is why this is not `pci_find` with an identifier in it. */
    if (!pci_find_class(0x04, 0x03, 0, &dev, NULL)) {
        return false;
    }

    pci_enable(&dev);

    if (dev.bar[0] == 0) {
        description = "an HDA controller whose BAR0 is empty";
        return false;
    }

    hda.base = mmu_map_device((uintptr_t)dev.bar[0], 0x4000);

    if (hda.base == 0) {
        description = "an HDA controller and no room to map it";
        return false;
    }

    hda.irq = dev.irq;

    if (!reset_controller()) {
        description = "an HDA controller that would not leave reset";
        return false;
    }

    states = read16(hda.base + STATESTS);

    if (states == 0) {
        description = "an HDA controller with no codec on the link";
        return false;
    }

    /* The lowest line with something on it. A laptop has a codec on SDI0
     * and an HDMI codec further along; the first is the one with speakers. */
    for (i = 0; i < 15u; i++) {
        if ((states & (1u << i)) != 0) {
            hda.codec = i;
            break;
        }
    }

    gcap = read16(hda.base + GCAP);
    inputs = (gcap >> 8) & 0xfu;
    outputs = (gcap >> 12) & 0xfu;

    if (outputs == 0) {
        description = "an HDA controller with no output stream";
        return false;
    }

    /* Input stream descriptors come first, so the first output one is at
     * index `inputs`. Getting this wrong programs a capture stream and
     * produces a machine that is silent and reports no error at all. */
    hda.stream_index = inputs;
    hda.sd = hda.base + STREAM_BASE + inputs * STREAM_STRIDE;

    if (!rings_start()) {
        description = "an HDA controller that refused a 256-entry ring";
        return false;
    }

    if (!find_widgets()) {
        description = "an HDA codec with no output path";
        return false;
    }

    /* The converter is told what it is playing and which stream to take it
     * from. The format is the same sixteen bits the stream descriptor got,
     * because they are the same field in two places and the specification
     * requires them to agree. */
    (void)command(verb16(hda.dac, VERB_SET_FORMAT,
                                  FORMAT_44100_S16_STEREO));
    (void)command(verb8(hda.dac, VERB_SET_STREAM_CHANNEL,
                                 (STREAM_TAG << 4)));

    stream_setup();

    hda.write_slot = 0;
    hda.primed = false;
    hda.dry = 0;
    /*
     * The floor only ever comes down, so it starts at the ceiling - and the
     * ceiling is the HAL's depth rather than the ring's, because that is
     * what `sysinfo` prints it against.
     */
    hda.floor = HAL_SND_PERIODS;
    hda.wants = false;
    hda.woke = 0;
    hda.present = true;

    /*
     * And ask to be told, last - an interrupt into a half-built driver is
     * the bug that only happens on a fast machine, which is the sentence
     * `hal/virtio/snd.c` writes above the same line.
     */
    mmio_write32(hda.base + INTCTL, INTCTL_GIE | (1u << hda.stream_index));
    pc_irq_unmask(hda.irq);

    description = "Intel HDA";
    return true;
}

bool hda_present(void)
{
    return hda.present;
}

const char *hda_describe(void)
{
    return description;
}

/*------------------------------------------------------------------------
 * Playing.
 *----------------------------------------------------------------------*/

/*
 * Which period the hardware is inside, from the hardware.
 *
 * `SDLPIB` is a byte offset into the cyclic buffer that the controller
 * updates as it reads, so dividing by the period size is the slot. This is
 * the only source of truth in the file: a count kept here could disagree
 * with the device after a single coalesced interrupt, and the way that bug
 * presents is audio written into the period being played.
 */
static unsigned play_slot(void)
{
    uint32_t at = mmio_read32(hda.sd + SD_LPIB);

    return (at / HAL_SND_PERIOD_BYTES) % RING_PERIODS;
}

static unsigned depth(void)
{
    return (hda.write_slot + RING_PERIODS - play_slot()) % RING_PERIODS;
}

unsigned hda_queued(void)
{
    if (!hda.present) {
        return 0;
    }

    return depth();
}

static bool write_locked(const void *pcm, unsigned bytes)
{
    unsigned have;

    if (!hda.present || bytes == 0 || bytes > HAL_SND_PERIOD_BYTES) {
        return false;
    }

    have = depth();

    /* One slot short of the ring, which is what keeps zero meaning empty. */
    if (have >= RING_PERIODS - 1u) {
        return false;
    }

    /*
     * The same two instruments `hal/virtio/snd.c` keeps and the same reason
     * they are counted here rather than in the interrupt: a period arriving
     * at an empty device is a deadline missed, and this is the moment it is
     * known. Not counted until the pipeline has filled once, because a
     * stream always starts empty and counting that would report a fault on
     * every machine that ever played a sound.
     *
     * **Full is `HAL_SND_PERIODS`, not `RING_PERIODS`**, and the difference
     * is not a detail. The ring is deeper than the depth the HAL promises,
     * so a writer that respects the promise never fills it - and a threshold
     * measured against the ring would never be reached, which is an
     * instrument that reads zero for ever and looks like a system with no
     * underruns rather than one with no measurement.
     */
    if (have >= HAL_SND_PERIODS - 1u) {
        hda.primed = true;
    }

    if (hda.primed) {
        if (have == 0) {
            hda.dry++;
        }

        if (have < hda.floor) {
            hda.floor = have;
        }
    }

    /*
     * An empty ring means the write cursor is sitting on the period the
     * hardware is reading right now, and filling that one would be audible
     * as a tear. Step past it: at most one period is given up, and only
     * when the pipeline had already run out.
     */
    if (have == 0) {
        hda.write_slot = (play_slot() + 1u) % RING_PERIODS;
    }

    memcpy(&ring[hda.write_slot * HAL_SND_PERIOD_BYTES], pcm, bytes);

    if (bytes < HAL_SND_PERIOD_BYTES) {
        memset(&ring[hda.write_slot * HAL_SND_PERIOD_BYTES + bytes], 0,
               HAL_SND_PERIOD_BYTES - bytes);
    }

    publish();
    hda.write_slot = (hda.write_slot + 1u) % RING_PERIODS;

    return true;
}

bool hda_write(const void *pcm, unsigned bytes)
{
    unsigned long flags = spin_lock(&hda_lock);
    bool r = write_locked(pcm, bytes);

    spin_unlock(&hda_lock, flags);

    return r;
}

/*
 * A period has been played.
 *
 * **The period just finished is zeroed**, and that is the difference
 * between a ring and a queue. A queue that runs out plays nothing; a ring
 * that runs out plays whatever is still under its descriptors, which is the
 * last thing anybody wrote, for ever. One kilobyte of stores every 5.8 ms
 * turns that into silence.
 *
 * The slot cannot be one a writer is filling: `write_locked` never writes
 * to the period being played nor to the one before it, and this is the one
 * before it.
 */
static void interrupt_locked(unsigned line)
{
    uint8_t status;

    if (!hda.present || hda.irq != line) {
        return;
    }

    status = mmio_read8(hda.sd + SD_STS);

    if ((status & SD_STS_BCIS) == 0) {
        return;                 /* a shared line, and this was somebody else */
    }

    /*
     * Write-one-to-clear, and the whole machine depends on it: a status bit
     * left set re-raises the line immediately and for ever, which presents
     * as a computer that has hung with its fans on.
     *
     * This is the only register written here. `INTSTS` looks like a second
     * acknowledgement and is not one - it is read-only, a mirror of the
     * stream status bits, and clearing this is what clears that.
     */
    mmio_write8(hda.sd + SD_STS, SD_STS_CLEAR);

    memset(&ring[((play_slot() + RING_PERIODS - 1u) % RING_PERIODS)
                 * HAL_SND_PERIOD_BYTES], 0, HAL_SND_PERIOD_BYTES);

    hda.woke++;
    hda.wants = true;
}

void hda_interrupt(unsigned line)
{
    unsigned long flags = spin_lock(&hda_lock);

    interrupt_locked(line);
    spin_unlock(&hda_lock, flags);
}

bool hda_wants(void)
{
    bool w = hda.wants;

    hda.wants = false;

    return w;
}

unsigned hda_wakes(void)
{
    return hda.woke;
}

unsigned hda_dry(void)
{
    return hda.dry;
}

unsigned hda_floor(void)
{
    return hda.floor;
}
