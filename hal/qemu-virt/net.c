/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * virtio-net: the card.
 *
 * Frames in and frames out, and nothing above that. What an address means,
 * what a checksum covers, which of these bytes is a protocol number - none
 * of that is here, and `user/servers/net.c` is where it lives. The same
 * division `blk.c` draws by knowing about sectors and nothing about files.
 *
 * **Two queues, and they run in opposite directions**, which is the whole
 * shape of the file. Queue 0 is receive: the driver hands the device empty
 * buffers and the device fills them, exactly as `input.c` does with events.
 * Queue 1 is transmit: the driver fills a buffer and the device drains it,
 * closer to what `blk.c` does with a request. So this is both of the
 * patterns that already exist here, one per queue, which is why neither of
 * them was worth generalising.
 *
 * **Every frame carries a twelve-byte header the wire never sees.** Modern
 * virtio-net puts `struct virtio_net_hdr_v1` in front of the Ethernet frame
 * in both directions, and it is not part of the packet - it is how the
 * device and the driver talk about checksum offload and segmentation, none
 * of which this driver asks for. So it is zeroed going out and skipped
 * coming in, and the one thing to get right is that it is *twelve* bytes
 * because VIRTIO_F_VERSION_1 was negotiated. The legacy header is ten, and a
 * driver that used the wrong length would hand the stack a frame two bytes
 * out of alignment - which decodes as a completely different protocol rather
 * than as an error.
 *
 * **Written against the specification from knowledge, not from a copy of
 * `virtio_net.h`.** So the offsets and the header size are the parts to
 * distrust; what establishes them is a frame going out and QEMU writing it
 * into a pcap the host can read, which is what `run_network.py` does.
 *
 * `roadmap.md` records why this lives in the kernel rather than a process of
 * its own, and the reason is the same one `blk.c` gives: a driver at EL0 is
 * the claim this project makes and the right long-term answer, but it needs
 * MMIO mapped into a process, interrupts delivered to one, and DMA memory a
 * process can hand a device, and none of the three exists. Building them
 * here would make the networking milestone a milestone about driver
 * infrastructure.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "qemu-virt.h"
#include "virtio.h"
#include "process.h"

/*
 * virtio_net.h, the features worth naming.
 *
 * Only MAC is asked for. Everything else - checksum offload, segmentation,
 * multiple queues, control - is a promise this driver would then have to
 * keep, and none of them is needed to put a frame on a wire. The rule
 * `blk.c` follows for the same reason.
 */
#define NET_F_MAC       (1u << 5)

/* The header both directions carry, in front of the frame. Twelve bytes
 * under VERSION_1: `num_buffers` is always present, where the legacy layout
 * stopped at ten. */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

#define VQ_RX   0
#define VQ_TX   1

/*
 * Sixteen buffers each way.
 *
 * Receive is the side that matters: a device with nowhere to put a frame
 * drops it, and a dropped frame is a retransmission somebody waits for. At
 * the rate a ping runs this is absurd headroom; at the rate a TCP stream
 * runs it is about one window, which is the number to revisit when there is
 * a TCP stream to measure.
 *
 * Transmit needs far less - this driver sends one frame and returns - but
 * one ring size is easier to hold in the head than two, and the memory is
 * 16 * 1526 twice over, which is 49 KB in a 512 MB machine.
 */
#define QUEUE_SIZE  16

struct vqueue { VIRTQ_FIELDS(QUEUE_SIZE); };

/* A header and a frame, contiguous, so one descriptor covers both. The
 * specification allows the header in its own descriptor and this is
 * simpler: one buffer, one descriptor, no chain to get wrong. */
struct packet {
    struct virtio_net_hdr hdr;
    uint8_t               frame[HAL_NET_FRAME];
};

/*
 * `.bss`, and load-bearing for the reason `blk.c` and `input.c` both give:
 * the kernel is identity mapped, so a pointer into these structures is
 * already the physical address the device needs. That equality ends the day
 * the kernel moves to TTBR1.
 */
static struct {
    struct virtio_device dev;
    bool     present;

    struct vqueue rx;
    struct vqueue tx;

    uint16_t rx_last_used;
    uint16_t tx_last_used;

    struct packet rx_buf[QUEUE_SIZE];
    struct packet tx_buf[QUEUE_SIZE];

    unsigned tx_next;               /* which transmit buffer to fill next */

    uint8_t  mac[6];

    /* Set by the interrupt, cleared by whoever asks. See `hal_net_arrived`. */
    volatile bool arrived;
} net;

/*
 * Hand buffer `i` back to the device, empty.
 *
 * Marked writable, because on the receive queue the device is the one doing
 * the writing - which is the flag that separates this from the transmit
 * path and the single easiest thing to get backwards.
 */
static void offer(unsigned i)
{
    uint16_t at = net.rx.avail.idx % QUEUE_SIZE;

    net.rx.desc[i].addr  = (uint64_t)(uintptr_t)&net.rx_buf[i];
    net.rx.desc[i].len   = sizeof(net.rx_buf[i]);
    net.rx.desc[i].flags = VRING_DESC_F_WRITE;
    net.rx.desc[i].next  = 0;

    net.rx.avail.ring[at] = (uint16_t)i;

    /* The entry has to be visible before the index that publishes it. */
    virtio_publish();
    net.rx.avail.idx++;
}

bool hal_net_init(struct netdev *out)
{
    unsigned from = 0;
    unsigned i;

    net.present = false;

    while (virtio_open(VIRTIO_ID_NET, from, &net.dev)) {
        from = net.dev.slot + 1;

        if (!virtio_features(&net.dev, NET_F_MAC)) {
            continue;
        }

        memset(&net.rx, 0, sizeof(net.rx));
        memset(&net.tx, 0, sizeof(net.tx));

        net.rx_last_used = 0;
        net.tx_last_used = 0;
        net.tx_next      = 0;

        if (!virtio_queue_attach(&net.dev, VQ_RX, QUEUE_SIZE, net.rx.desc,
                                 &net.rx.avail, &net.rx.used)
            || !virtio_queue_attach(&net.dev, VQ_TX, QUEUE_SIZE, net.tx.desc,
                                    &net.tx.avail, &net.tx.used)) {
            virtio_fail(&net.dev);
            continue;
        }

        virtio_ready(&net.dev);

        /*
         * The address, if the device offered one.
         *
         * Six bytes at offset 0 of configuration space, read one at a time
         * because a MAC is not naturally aligned and the specification says
         * byte access is how you read a field that is not.
         *
         * Asked, not assumed: `dev.features` is what was *agreed*, so a
         * device that never offered MAC leaves this zeroed and the stack
         * above has to notice. A driver that read the register anyway would
         * hand up six bytes of whatever the window happens to return.
         */
        if ((net.dev.features & NET_F_MAC) != 0) {
            for (i = 0; i < 6; i++) {
                net.mac[i] = virtio_config8(&net.dev, i);
            }
        }

        /* Every receive buffer offered at once, for the reason input.c
         * gives about events: a device with nowhere to put a frame drops
         * it, and nothing tells you it did. */
        for (i = 0; i < QUEUE_SIZE; i++) {
            offer(i);
        }

        virtio_publish();
        virtio_notify(&net.dev, VQ_RX);

        gic_enable_spi(VIRTIO_INTID_BASE + net.dev.slot);

        net.present = true;

        if (out != NULL) {
            memcpy(out->mac, net.mac, sizeof(out->mac));
            out->mtu = HAL_NET_MTU;
        }

        return true;
    }

    return false;
}

bool hal_net_send(const void *frame, unsigned bytes)
{
    struct packet *p;
    uint16_t at;
    unsigned i;

    if (!net.present || frame == NULL || bytes == 0
        || bytes > HAL_NET_FRAME) {
        return false;
    }

    /*
     * Reap first, then fill.
     *
     * The device returns transmit buffers through the used ring when it has
     * finished with them, and nothing here waits for that - a send that
     * blocked until the card drained would put the stack's latency in the
     * hands of the wire. So the used index is read to know how many are
     * free, and a full ring is a refusal rather than a wait.
     */
    virtio_consume();
    net.tx_last_used = net.tx.used.idx;

    if ((uint16_t)(net.tx.avail.idx - net.tx_last_used) >= QUEUE_SIZE) {
        return false;               /* the card has not caught up */
    }

    i = net.tx_next;
    net.tx_next = (net.tx_next + 1) % QUEUE_SIZE;

    p = &net.tx_buf[i];

    /* Zeroed rather than left over: every field in it is a request for an
     * offload this driver did not negotiate, and a stale one is a device
     * asked to segment a frame that is not what it thinks. */
    memset(&p->hdr, 0, sizeof(p->hdr));
    memcpy(p->frame, frame, bytes);

    net.tx.desc[i].addr  = (uint64_t)(uintptr_t)p;
    net.tx.desc[i].len   = sizeof(p->hdr) + bytes;
    net.tx.desc[i].flags = 0;       /* the device reads it */
    net.tx.desc[i].next  = 0;

    at = net.tx.avail.idx % QUEUE_SIZE;
    net.tx.avail.ring[at] = (uint16_t)i;

    virtio_publish();
    net.tx.avail.idx++;
    virtio_publish();

    virtio_notify(&net.dev, VQ_TX);

    return true;
}

int hal_net_recv(void *frame, unsigned max)
{
    unsigned slot;
    uint32_t len;

    if (!net.present || frame == NULL) {
        return 0;
    }

    virtio_consume();

    if (net.rx.used.idx == net.rx_last_used) {
        return 0;                   /* nothing waiting, which is usual */
    }

    slot = net.rx.used.ring[net.rx_last_used % QUEUE_SIZE].id % QUEUE_SIZE;
    len  = net.rx.used.ring[net.rx_last_used % QUEUE_SIZE].len;

    net.rx_last_used++;

    /*
     * `len` counts the header the device wrote as well as the frame, so the
     * frame is what is left after it. A device that reported less than a
     * header is a device this driver does not understand, and the buffer
     * goes back rather than being decoded.
     */
    if (len < sizeof(struct virtio_net_hdr)) {
        offer(slot);
        virtio_publish();
        virtio_notify(&net.dev, VQ_RX);
        return 0;
    }

    len -= sizeof(struct virtio_net_hdr);

    if (len > HAL_NET_FRAME) {
        len = HAL_NET_FRAME;        /* the ring cannot have held more */
    }

    if (len > max) {
        /* The caller's buffer is smaller than a frame. Returned rather than
         * truncated: half a frame is not a frame, and silently shortening
         * one is how a stack decodes a packet that was never sent. */
        offer(slot);
        virtio_publish();
        virtio_notify(&net.dev, VQ_RX);
        return -1;
    }

    memcpy(frame, net.rx_buf[slot].frame, len);

    /* Back to the device before anything is done with the bytes, for the
     * reason input.c gives: a descriptor that was not returned is one the
     * device cannot write to again. */
    offer(slot);
    virtio_publish();
    virtio_notify(&net.dev, VQ_RX);

    return (int)len;
}

bool hal_net_present(void)
{
    return net.present;
}

bool hal_net_info(struct netdev *out)
{
    if (!net.present || out == NULL) {
        return false;
    }

    memcpy(out->mac, net.mac, sizeof(out->mac));
    out->mtu = HAL_NET_MTU;

    return true;
}

bool hal_net_arrived(void)
{
    bool was = net.arrived;

    net.arrived = false;

    return was;
}

void net_interrupt(unsigned slot)
{
    if (!net.present || net.dev.slot != slot) {
        return;
    }

    /* The device raised it; the device is told it was seen. Without the ack
     * the status bit stays set and the interrupt fires for ever. */
    (void)virtio_ack_interrupt(&net.dev);

    net.arrived = true;

    /* And whoever is waiting for one. The card said so, rather than the
     * stack asking at a rate somebody picked. */
    process_wake_net();
}
