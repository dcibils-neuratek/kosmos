/*
 * virtio-sound over virtio-mmio: PCM out.
 *
 * The fourth virtio device on this board and the third to be written from
 * the same shape - scan the thirty-two windows, negotiate, build the rings,
 * push descriptors. `blk.c` and `input.c` are the other two and this file
 * deliberately reads like them.
 *
 *--------------------------------------------------------------------------
 * Where the numbers come from.
 *
 * Register offsets: the virtio 1.2 specification, section 4.2.2 (MMIO
 * device register layout), the same set `blk.c` uses.
 *
 * Everything sound-specific - the request codes, the struct layouts, the
 * format and rate enumerations - is from `include/uapi/linux/virtio_snd.h`,
 * which is the specification written down as C and is what QEMU implements
 * against. The device id is 25, from `virtio_ids.h`. None of it is from
 * memory: `CLAUDE.md` asks for the datasheet where an offset is at stake,
 * and an enumerant twelve entries down a list is exactly that.
 *
 *--------------------------------------------------------------------------
 * Latency, which is the whole reason this file exists in the shape it does.
 *
 * `roadmap.md` M11a says audio is in scope and *guaranteed* latency is not:
 * there is no promise about worst-case jitter and there will not be one
 * until something bounds the collector. What there is instead is a number.
 *
 * The number lives in the period size. A period is the unit the device
 * consumes and the unit the machine has to refill before: at 44100 Hz,
 * stereo, sixteen bits, one frame is four bytes, so a 256-frame period is
 * 1024 bytes and 5.8 milliseconds. Four of them in the buffer is 23
 * milliseconds of sound in hand, which is the deadline the rest of the
 * system has to beat.
 *
 * Smaller would be tighter and is not obviously better: every period is a
 * descriptor chain, a notify and an interrupt, and at 128 frames the
 * machine spends more of its time announcing sound than making it. 256 is
 * the smallest size where the cost of the plumbing is clearly below the
 * cost of the mixing, and it is a constant here rather than a decision
 * buried in a driver so that measuring it means changing one line.
 */

#include <stdint.h>
#include <string.h>

#include "mmio.h"
#include "qemu-virt.h"
#include "virtio.h"
#include "hal.h"

/* virtio_snd.h: the four queues, in this order. */
#define VQ_CONTROL              0
#define VQ_EVENT                1
#define VQ_TX                   2
#define VQ_RX                   3
#define VQ_COUNT                4

/* virtio_snd.h: request codes. */
#define SND_R_PCM_INFO          0x0100
#define SND_R_PCM_SET_PARAMS    0x0101
#define SND_R_PCM_PREPARE       0x0102
#define SND_R_PCM_RELEASE       0x0103
#define SND_R_PCM_START         0x0104
#define SND_R_PCM_STOP          0x0105

#define SND_S_OK                0x8000

#define SND_D_OUTPUT            0

/* Twelve entries into the format list and six into the rate list. Counted
 * from the header rather than remembered, because an off-by-one here is a
 * device that plays noise rather than one that refuses. */
#define SND_PCM_FMT_S16         5
#define SND_PCM_RATE_44100      6

/*
 * Thirty-two descriptors on every queue.
 *
 * The transmit queue is the one that needs them: a period in flight is a
 * three-descriptor chain - the header the device reads, the samples, and
 * the status it writes - so thirty-two is ten periods, and ten periods at
 * 5.8 milliseconds is sixty milliseconds of runway. The other three queues
 * would be happy with eight and share the number because one constant is
 * easier to reason about than four.
 */
#define QUEUE_SIZE  32

struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

/* virtio_snd.h, verbatim in layout. */
struct snd_hdr { uint32_t code; };

struct snd_query_info {
    struct snd_hdr hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
};

struct snd_pcm_hdr {
    struct snd_hdr hdr;
    uint32_t stream_id;
};

struct snd_pcm_set_params {
    struct snd_pcm_hdr hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
};

struct snd_pcm_info {
    struct snd_hdr hdr;              /* struct virtio_snd_info: hdr_size */
    uint32_t hda_fn_nid;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t  direction;
    uint8_t  channels_min;
    uint8_t  channels_max;
    uint8_t  padding[5];
};

struct snd_pcm_xfer  { uint32_t stream_id; };
struct snd_pcm_status { uint32_t status; uint32_t latency_bytes; };

/*
 * The buffers the device reads and writes, which must not be on a stack.
 *
 * A descriptor holds a physical address and the device reads it whenever it
 * gets round to it - which for the transmit queue is after this function
 * has returned. Anything a descriptor points at has to outlive the call
 * that queued it, and static storage is the plainest way to be sure of that.
 */
static struct {
    bool present;
    struct virtio_device dev;

    struct vqueue q[VQ_COUNT];
    uint16_t last_used[VQ_COUNT];

    uint32_t streams;
    uint32_t stream_id;
    bool running;

    /* Control requests are one at a time and synchronous. */
    union {
        struct snd_query_info    query;
        struct snd_pcm_set_params params;
        struct snd_pcm_hdr        pcm;
    } req;

    union {
        struct snd_hdr      hdr;
        struct snd_pcm_info info[8];
    } rsp;

    /* One transmit slot per period in flight. */
    struct snd_pcm_xfer   xfer[QUEUE_SIZE / 3];
    struct snd_pcm_status xstat[QUEUE_SIZE / 3];
    uint8_t               period[QUEUE_SIZE / 3][HAL_SND_PERIOD_BYTES];
    unsigned              next_slot;

    /* Deadlines missed, and the closest it came. See `hal_snd_dry`. */
    bool                  primed;     /* the queue has been full at least once */
    volatile bool         wants;      /* the device raised its line */
    volatile unsigned     woke;       /* how many times it has */
    unsigned              dry;
    unsigned              floor;
} snd;

/*
 * Everything written before this is visible to the device after it.
 *
 * **`dsb sy`, not the transport's `dmb oshst`**, and the difference is
 * deliberate rather than an oversight left behind by the refactor. `dmb`
 * orders; `dsb` waits for completion. Everywhere else ordering is all that
 * is needed, and this driver is the one that hands a period to a device on
 * a 5.8 ms deadline - the stronger barrier costs a stall and buys certainty
 * that the samples are in memory and not in a store buffer when the notify
 * lands. Nothing has measured the difference; it has also never clicked.
 */
static void publish(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/* And everything the device wrote is visible to us after this. */
static void consume(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static void ring_setup(unsigned n)
{
    struct vqueue *q = &snd.q[n];

    memset(q, 0, sizeof(*q));

    (void)virtio_queue_attach(&snd.dev, n, QUEUE_SIZE,
                              q->desc, &q->avail, &q->used);
}

/*
 * One control request, and wait for it.
 *
 * Synchronous because every one of them happens at setup: querying the
 * streams, setting the format, starting. Nothing here is on the path that
 * has a deadline - that is the transmit queue, below, and it does not wait
 * for anything.
 */
static bool control(const void *request, unsigned request_len,
                    void *reply, unsigned reply_len)
{
    struct vqueue *q = &snd.q[VQ_CONTROL];
    unsigned long spins;
    unsigned at;

    q->desc[0].addr  = (uint64_t)(uintptr_t)request;
    q->desc[0].len   = request_len;
    q->desc[0].flags = VRING_DESC_F_NEXT;
    q->desc[0].next  = 1;

    q->desc[1].addr  = (uint64_t)(uintptr_t)reply;
    q->desc[1].len   = reply_len;
    q->desc[1].flags = VRING_DESC_F_WRITE;
    q->desc[1].next  = 0;

    at = q->avail.idx % QUEUE_SIZE;
    q->avail.ring[at] = 0;

    publish();
    q->avail.idx++;
    publish();

    virtio_notify(&snd.dev, VQ_CONTROL);

    for (spins = 0; spins < 100000000UL; spins++) {
        consume();

        if (q->used.idx != snd.last_used[VQ_CONTROL]) {
            snd.last_used[VQ_CONTROL] = q->used.idx;
            (void)virtio_ack_interrupt(&snd.dev);
            consume();

            return true;
        }
    }

    return false;
}

static bool pcm_command(uint32_t code)
{
    snd.req.pcm.hdr.code = code;
    snd.req.pcm.stream_id = snd.stream_id;

    if (!control(&snd.req.pcm, sizeof(snd.req.pcm),
                 &snd.rsp.hdr, sizeof(snd.rsp.hdr))) {
        return false;
    }

    return snd.rsp.hdr.code == SND_S_OK;
}

/*
 * The first stream that plays rather than records.
 *
 * Asked rather than assumed. QEMU's virtio-sound offers one output stream
 * today and `-device virtio-sound-device` with a microphone codec would
 * offer two, in an order nothing promises - so picking stream zero would
 * work until the day somebody added an input and then record silence
 * instead of playing anything.
 */
static bool find_output_stream(void)
{
    uint32_t n = snd.streams;
    uint32_t i;

    if (n > 8) {
        n = 8;                          /* what `rsp.info` has room for */
    }

    snd.req.query.hdr.code = SND_R_PCM_INFO;
    snd.req.query.start_id = 0;
    snd.req.query.count = n;
    snd.req.query.size = sizeof(struct snd_pcm_info);

    if (!control(&snd.req.query, sizeof(snd.req.query),
                 &snd.rsp, sizeof(struct snd_hdr)
                           + n * sizeof(struct snd_pcm_info))) {
        return false;
    }

    /*
     * The reply is a status word and then the array, so the entries do not
     * start at `rsp.info[0]` - they start after the header. Reading them
     * from the top of the union is the mistake this comment exists to stop.
     */
    {
        const uint8_t *at = (const uint8_t *)&snd.rsp
                          + sizeof(struct snd_hdr);

        if (snd.rsp.hdr.code != SND_S_OK) {
            return false;
        }

        for (i = 0; i < n; i++) {
            const struct snd_pcm_info *info =
                (const struct snd_pcm_info *)(at + i * sizeof(*info));

            if (info->direction == SND_D_OUTPUT) {
                snd.stream_id = i;

                return true;
            }
        }
    }

    return false;
}

bool hal_snd_init(void)
{
    unsigned from = 0;

    while (virtio_open(VIRTIO_ID_SOUND, from, &snd.dev)) {
        from = snd.dev.slot + 1;

        /*
         * Only VIRTIO_F_VERSION_1, which `virtio_features` always asks for.
         * Nothing sound-specific is: the optional features are about message
         * polling and shared-memory period notification, and both are
         * optimisations for a driver that has measured something first.
         */
        if (!virtio_features(&snd.dev, 0)) {
            return false;               /* it would not take VERSION_1 */
        }

        /* virtio_snd_config: jacks, streams, chmaps, controls. */
        snd.streams = virtio_config32(&snd.dev, 4);

        if (snd.streams == 0) {
            return false;
        }

        ring_setup(VQ_CONTROL);
        ring_setup(VQ_EVENT);
        ring_setup(VQ_TX);
        ring_setup(VQ_RX);

        virtio_ready(&snd.dev);

        if (!find_output_stream()) {
            return false;
        }

        snd.req.params.hdr.hdr.code = SND_R_PCM_SET_PARAMS;
        snd.req.params.hdr.stream_id = snd.stream_id;
        snd.req.params.buffer_bytes =
            HAL_SND_PERIOD_BYTES * HAL_SND_PERIODS;
        snd.req.params.period_bytes = HAL_SND_PERIOD_BYTES;
        snd.req.params.features = 0;
        snd.req.params.channels = HAL_SND_CHANNELS;
        snd.req.params.format = SND_PCM_FMT_S16;
        snd.req.params.rate = SND_PCM_RATE_44100;
        snd.req.params.padding = 0;

        if (!control(&snd.req.params, sizeof(snd.req.params),
                     &snd.rsp.hdr, sizeof(snd.rsp.hdr))
            || snd.rsp.hdr.code != SND_S_OK) {
            return false;
        }

        if (!pcm_command(SND_R_PCM_PREPARE)) {
            return false;
        }

        if (!pcm_command(SND_R_PCM_START)) {
            return false;
        }

        snd.present = true;
        snd.running = true;

        /* The floor only ever comes down, so it starts at the ceiling. */
        snd.primed = false;
        snd.dry = 0;
        snd.floor = HAL_SND_PERIODS;
        snd.wants = false;
        snd.woke = 0;

        /*
         * And ask to be told. Last, so that nothing can arrive before the
         * state it would touch exists - an interrupt into a half-built
         * driver is the kind of bug that only happens on a fast machine.
         */
        gic_enable_spi(VIRTIO_INTID_BASE + snd.dev.slot);

        return true;
    }

    return false;
}

/*
 * How many periods the device has not finished with.
 *
 * The refill deadline in one number: while this is below `HAL_SND_PERIODS`
 * there is room to queue another, and when it reaches zero the device has
 * run dry and whatever comes out next has a click in it. Something above
 * this layer has to watch it, and cannot if this does not say.
 */
bool hal_snd_present(void)
{
    return snd.present;
}

unsigned hal_snd_queued(void)
{
    struct vqueue *q = &snd.q[VQ_TX];

    if (!snd.present) {
        return 0;
    }

    consume();

    return (unsigned)(uint16_t)(q->avail.idx - q->used.idx);
}

/*
 * Deadlines missed, and the closest this has come to missing one.
 *
 * **The number that matters, and the one nothing was counting.** Throughput
 * says how much audio came out; it says nothing about *when*, and a stream
 * that delivers a full second of sound every second with a twenty
 * millisecond hole in the middle measures perfect and sounds broken. Two
 * days were spent declaring this fixed against an average.
 *
 * `dry` counts periods that arrived at a device with nothing left in hand:
 * the hardware had already played silence, and that silence is the click.
 * `floor` is the smallest depth ever seen while running - the margin, in
 * periods. A floor of zero means it underran; a floor of three out of four
 * means the pipeline never came close.
 *
 * Counted where they happen rather than sampled from above, because the
 * moment a period is handed over is the only place the answer is exact.
 */
/*
 * The device has finished with a period, and says so.
 *
 * **This is what turns "usually fine" into a deadline.** Everything above
 * used to ask - `hal_snd_queued` on a timer, at whatever rate somebody had
 * chosen - and a poll is a guess about when the answer changed. The device
 * knows exactly when, and virtio-mmio has had a line for saying so since
 * before this driver existed; it simply was not wired up.
 *
 * The acknowledge is not optional. The status bit stays set until it is
 * written back, and an unacknowledged interrupt fires again immediately and
 * for ever, which presents as a machine that has hung with the fans on.
 */
void snd_interrupt(unsigned slot)
{
    if (!snd.present || snd.dev.slot != slot) {
        return;
    }

    (void)virtio_ack_interrupt(&snd.dev);

    snd.woke++;
    snd.wants = true;
}

/*
 * Has the device asked for a period since this was last read?
 *
 * Read-and-clear, because the question is "has anything happened", not "is
 * something true": leaving it set would have the server come straight back
 * round a loop it has already served.
 */
bool hal_snd_wants(void)
{
    bool w = snd.wants;

    snd.wants = false;
    return w;
}

/* How many times the device has raised its line. Nothing depends on it; it
 * is here so that "the interrupt is not arriving" and "the interrupt is
 * arriving and something else is slow" are different observations. */
unsigned hal_snd_wakes(void)
{
    return snd.woke;
}

unsigned hal_snd_dry(void)
{
    return snd.dry;
}

unsigned hal_snd_floor(void)
{
    return snd.floor;
}

/*
 * One period, queued. False when there is no room or no device.
 *
 * It does not block and it does not wait for the device: that is the whole
 * point of the queue depth. A caller that finds this returning false is a
 * caller that is *ahead*, which is the good problem.
 */
bool hal_snd_write(const void *pcm, unsigned bytes)
{
    struct vqueue *q = &snd.q[VQ_TX];
    unsigned slot, head, at;

    if (!snd.present || !snd.running || bytes == 0
        || bytes > HAL_SND_PERIOD_BYTES) {
        return false;
    }

    if (hal_snd_queued() >= (QUEUE_SIZE / 3) - 1) {
        return false;
    }

    /*
     * The depth *before* this period joins it, which is what says whether
     * the device had anything to play while we were getting here. Zero
     * means it did not.
     */
    {
        unsigned depth = hal_snd_queued();

        /*
         * **Not counted until the pipeline has filled once.**
         *
         * A stream starts with an empty device, so the first period always
         * arrives at a depth of zero and so do the next few - that is the
         * queue filling, not a deadline missed. Counting them made every
         * measurement read "four underruns" whatever the system did, which
         * is worse than no instrument: it looked like a constant fault and
         * hid whether anything had changed.
         *
         * `primed` turns on the first time the device is actually full, and
         * only then does running out mean something went wrong.
         */
        if (depth >= HAL_SND_PERIODS - 1) {
            snd.primed = true;
        }

        if (snd.primed) {
            if (depth == 0) {
                snd.dry++;
            }

            if (depth < snd.floor) {
                snd.floor = depth;
            }
        }
    }

    slot = snd.next_slot;
    snd.next_slot = (snd.next_slot + 1) % (QUEUE_SIZE / 3);

    /*
     * Copied rather than pointed at.
     *
     * The device reads this after the call returns, so the samples have to
     * live somewhere that outlasts the caller's buffer - and the caller is
     * a mixer with one scratch buffer it is about to fill again. One memcpy
     * a period, which at 1024 bytes is nothing next to the mixing that
     * produced them.
     */
    memcpy(snd.period[slot], pcm, bytes);

    snd.xfer[slot].stream_id = snd.stream_id;
    snd.xstat[slot].status = 0;
    snd.xstat[slot].latency_bytes = 0;

    head = slot * 3;

    q->desc[head].addr      = (uint64_t)(uintptr_t)&snd.xfer[slot];
    q->desc[head].len       = sizeof(struct snd_pcm_xfer);
    q->desc[head].flags     = VRING_DESC_F_NEXT;
    q->desc[head].next      = (uint16_t)(head + 1);

    q->desc[head + 1].addr  = (uint64_t)(uintptr_t)snd.period[slot];
    q->desc[head + 1].len   = bytes;
    q->desc[head + 1].flags = VRING_DESC_F_NEXT;
    q->desc[head + 1].next  = (uint16_t)(head + 2);

    q->desc[head + 2].addr  = (uint64_t)(uintptr_t)&snd.xstat[slot];
    q->desc[head + 2].len   = sizeof(struct snd_pcm_status);
    q->desc[head + 2].flags = VRING_DESC_F_WRITE;
    q->desc[head + 2].next  = 0;

    at = q->avail.idx % QUEUE_SIZE;
    q->avail.ring[at] = (uint16_t)head;

    publish();
    q->avail.idx++;
    publish();

    virtio_notify(&snd.dev, VQ_TX);

    /* Acknowledged here rather than in an interrupt handler: nothing has
     * asked for this device's interrupt, and an unacknowledged one would be
     * redelivered for ever the moment something did. */
    (void)virtio_ack_interrupt(&snd.dev);

    return true;
}
