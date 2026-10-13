/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A sandbox's network** - `docs/sandboxes.md` S4.
 *
 * A virtio-net device (VIRTIO 1.2, 5.1) on the shared transport (`vdev.c`),
 * and behind it a gateway in this process - what QEMU's user-mode network
 * does for a guest, here for a sandbox. The guest is on a network of its
 * own, 10.0.3.0/24: it is 10.0.3.15, the gateway 10.0.3.2 and the name
 * server 10.0.3.3, both of which are this file. It is answered:
 *
 * - **ARP** for those two, with the gateway's address;
 * - **ping** of either;
 * - **DNS** questions to 10.0.3.3, answered by Kosmos's resolver
 *   (`net_client_resolve`) - an A record, or none;
 * - **TCP**: each connection the guest opens is a connection Kosmos opens
 *   to the same address and port (`netclient.h`), its bytes carried both
 *   ways, ended by either side's FIN or RST. The gateway's own address
 *   means this machine.
 *
 * **And ports carried in**: Kosmos's port answered here, and each
 * connection that arrives on it opened to the guest's port, from the
 * gateway's address - as QEMU's `hostfwd` does. Arrivals are waited for on
 * a thread of their own (`waiter`), so neither the guest's thread nor the
 * one answering `/Running` waits on a listener; each is handed across in a
 * ring the waiter only writes and the guest's thread only reads.
 *
 * Nothing else crosses. The guest reaches only what Kosmos's own network
 * reaches, as Kosmos's process does, and only when its sandbox was given
 * the network - or, given only ports, nothing at all: it answers what is
 * carried in and opens nothing of its own. No packet of the guest's is put
 * on a wire as it is.
 *
 * **The link to the guest loses nothing**: a segment is sent only when the
 * guest has a buffer posted for it, and bytes are read from Kosmos's side
 * only then - so nothing sent is ever sent again, and the gateway keeps no
 * copy of what the guest has not acknowledged.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "kits/network/netclient.h"
#include "vdev.h"
#include "vmmnet.h"

#define VIRTIO_ID_NET       1u
#define VIRTIO_NET_F_MAC    (1ull << 5)
#define RX                  0u
#define TX                  1u
#define NET_HDR             12u             /* struct virtio_net_hdr, VERSION_1 */

#define FRAME_MOST          (1514u + NET_HDR)
#define MSS                 1460u
#define WINDOW              65535u

static const uint8_t GUEST_MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t GATE_MAC[6]  = { 0x52, 0x54, 0x00, 0x12, 0x35, 0x02 };
static const uint8_t GUEST_IP[4]  = { 10, 0, 3, 15 };
static const uint8_t GATE_IP[4]   = { 10, 0, 3, 2 };
static const uint8_t DNS_IP[4]    = { 10, 0, 3, 3 };

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

struct link {
    bool     used;
    bool     opening, syn_sent;             /* carried in: our SYN, its answer awaited */
    uint16_t gport, dport;                  /* the guest's port, and where it went */
    uint8_t  dip[4];
    uint32_t rcv_nxt;                       /* the next byte expected of the guest */
    uint32_t snd_nxt, snd_una;              /* the next to send it; the oldest unacknowledged */
    uint32_t wnd;                           /* what the guest says it has room for */
    bool     fin_sent, guest_fin;
    struct net_conn conn;
};

static struct vdev dev;
static bool on, outgoing;
static struct link *links;
static unsigned links_n;
static uint16_t ip_id = 1;
static uint32_t dns_wait_ticks = 500;
static uint64_t counted_in, counted_out;    /* bytes, for a window's graph */

/*
 * **Ports carried in.** Each a listener of Kosmos's; what arrives is
 * accepted by the waiter and left in `arrived`, which it alone writes
 * `head` of and the guest's thread alone `tail` of.
 */
#define FORWARDS    4u
#define ARRIVALS    8u

static struct forward {
    uint16_t from, to;
    uint64_t listener;
} forwards[FORWARDS];
static unsigned forwards_n;

static struct arrival {
    struct net_conn conn;
    uint16_t        to;
} arrived[ARRIVALS];
static uint32_t arrived_head, arrived_tail;  /* __atomic */
static uint16_t next_port = 49152;          /* the gateway's side of one carried in */
static uint32_t tick_hz_here = 250;

/* ---------------------------------------------------------------------
 * Sums and headers.
 * --------------------------------------------------------------------- */

static uint32_t sum16(const uint8_t *p, size_t n, uint32_t sum)
{
    while (n > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        n -= 2;
    }

    if (n > 0) {
        sum += (uint32_t)(p[0] << 8);
    }

    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

static void be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, (uint16_t)(v >> 16)); be16(p + 2, (uint16_t)v); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }

/* ---------------------------------------------------------------------
 * To the guest: a frame into the next buffer it posted, or false.
 * --------------------------------------------------------------------- */

static bool to_guest(const uint8_t *frame, uint32_t len)
{
    static uint8_t buf[FRAME_MOST];
    struct vdev_chain c;

    if (len + NET_HDR > sizeof(buf) || !vdev_pop(&dev, RX, &c)) {
        return false;
    }

    memset(buf, 0, NET_HDR);
    vput16(buf + 10, 1);                    /* num_buffers */
    memcpy(buf + NET_HDR, frame, len);
    vdev_push(&dev, RX, c.head, vdev_scatter(&c, buf, NET_HDR + len));
    return true;
}

/* An IPv4 packet from `src` to the guest, `proto` and its payload. */
static bool ip_to_guest(uint8_t proto, const uint8_t src[4], const uint8_t *payload, uint32_t n)
{
    static uint8_t f[1514];
    uint8_t *ip = f + 14;

    if (14 + 20 + n > sizeof(f)) {
        return false;
    }

    memcpy(f, GUEST_MAC, 6);
    memcpy(f + 6, GATE_MAC, 6);
    be16(f + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45;
    be16(ip + 2, (uint16_t)(20 + n));
    be16(ip + 4, ip_id++);
    ip[8] = 64;
    ip[9] = proto;
    memcpy(ip + 12, src, 4);
    memcpy(ip + 16, GUEST_IP, 4);
    be16(ip + 10, fold(sum16(ip, 20, 0)));
    memcpy(ip + 20, payload, n);
    return to_guest(f, 14 + 20 + n);
}

/* A TCP segment of a link, to the guest. */
static bool tcp_to_guest(struct link *l, uint8_t flags, const uint8_t *data, uint32_t n,
                         bool mss_option)
{
    static uint8_t seg[20 + 4 + MSS];
    uint32_t hl = mss_option ? 24 : 20, sum;
    uint8_t pseudo[12];

    if (n > MSS) {
        return false;
    }

    memset(seg, 0, hl);
    be16(seg + 0, l->dport);
    be16(seg + 2, l->gport);
    be32(seg + 4, l->snd_nxt);
    be32(seg + 8, l->rcv_nxt);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    be16(seg + 14, WINDOW);

    if (mss_option) {
        seg[20] = 2;
        seg[21] = 4;
        be16(seg + 22, MSS);
    }

    memcpy(seg + hl, data, n);
    memcpy(pseudo, l->dip, 4);
    memcpy(pseudo + 4, GUEST_IP, 4);
    pseudo[8] = 0;
    pseudo[9] = 6;
    be16(pseudo + 10, (uint16_t)(hl + n));
    sum = sum16(pseudo, 12, 0);
    be16(seg + 16, fold(sum16(seg, hl + n, sum)));
    return ip_to_guest(6, l->dip, seg, hl + n);
}

/* ---------------------------------------------------------------------
 * From the guest.
 * --------------------------------------------------------------------- */

static bool ours(const uint8_t ip[4])
{
    return memcmp(ip, GATE_IP, 4) == 0 || memcmp(ip, DNS_IP, 4) == 0;
}

static void arp(const uint8_t *f, uint32_t n)
{
    uint8_t r[42];

    if (n < 42 || rd16(f + 20) != 1 || !ours(f + 38)) {
        return;                             /* not a request for us */
    }

    memcpy(r, f + 6, 6);
    memcpy(r + 6, GATE_MAC, 6);
    be16(r + 12, 0x0806);
    be16(r + 14, 1);                        /* Ethernet */
    be16(r + 16, 0x0800);
    r[18] = 6;
    r[19] = 4;
    be16(r + 20, 2);                        /* a reply */
    memcpy(r + 22, GATE_MAC, 6);
    memcpy(r + 28, f + 38, 4);              /* the address asked about */
    memcpy(r + 32, f + 22, 6);
    memcpy(r + 38, f + 28, 4);
    to_guest(r, sizeof(r));
}

static void ping(const uint8_t dst[4], const uint8_t *icmp, uint32_t n)
{
    static uint8_t r[1480];

    if (n < 8 || icmp[0] != 8 || n > sizeof(r)) {
        return;                             /* an echo request, and nothing else */
    }

    memcpy(r, icmp, n);
    r[0] = 0;
    be16(r + 2, 0);
    be16(r + 2, fold(sum16(r, n, 0)));
    ip_to_guest(1, dst, r, n);
}

/*
 * **A name, answered** by Kosmos's resolver: the question copied back with
 * an A record, or with none - NXDOMAIN for a name Kosmos could not find,
 * no answer at all for a question that is not for an address.
 */
static void dns(uint16_t sport, const uint8_t *q, uint32_t n)
{
    static uint8_t r[8 + 512];
    uint8_t *m = r + 8, addr[4];
    char name[256];
    uint32_t at = 12, len = 0, qend;
    uint16_t qtype;
    bool found = false;

    if (n < 17 || n > 512 - 16) {
        return;
    }

    while (at < n && q[at] != 0 && len < sizeof(name) - 64) {
        uint32_t k = q[at];

        if (k > 63 || at + 1 + k > n) {
            return;
        }

        if (len > 0) {
            name[len++] = '.';
        }

        memcpy(name + len, q + at + 1, k);
        len += k;
        at += 1 + k;
    }

    name[len] = '\0';
    qend = at + 5;

    if (qend > n) {
        return;
    }

    qtype = rd16(q + at + 1);

    if (qtype == 1 && len > 0) {
        found = net_client_resolve(name, addr, dns_wait_ticks) == NET_OK;
    }

    memcpy(m, q, qend);                     /* the header and the question */
    be16(m + 2, qtype != 1 || found ? 0x8180 : 0x8183);
    be16(m + 4, 1);
    be16(m + 6, found ? 1 : 0);
    be16(m + 8, 0);
    be16(m + 10, 0);

    if (found) {
        static const uint8_t answer[12] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };

        memcpy(m + qend, answer, sizeof(answer));
        memcpy(m + qend + 12, addr, 4);
        qend += 16;
    }

    be16(r + 0, 53);
    be16(r + 2, sport);
    be16(r + 4, (uint16_t)(8 + qend));
    be16(r + 6, 0);                         /* no checksum, as IPv4 allows */
    ip_to_guest(17, DNS_IP, r, 8 + qend);
}

static struct link *link_find(uint16_t gport, const uint8_t dip[4], uint16_t dport)
{
    unsigned i;

    for (i = 0; i < links_n; i++) {
        struct link *l = &links[i];

        if (l->used && l->gport == gport && l->dport == dport && memcmp(l->dip, dip, 4) == 0) {
            return l;
        }
    }

    return NULL;
}

static struct link *link_new(void)
{
    struct link *more;
    unsigned i;

    for (i = 0; i < links_n; i++) {
        if (!links[i].used) {
            memset(&links[i], 0, sizeof(links[i]));
            links[i].used = true;
            return &links[i];
        }
    }

    more = realloc(links, (links_n + 8) * sizeof(*more));

    if (more == NULL) {
        return NULL;
    }

    memset(more + links_n, 0, 8 * sizeof(*more));
    links = more;
    links_n += 8;
    links[links_n - 8].used = true;
    return &links[links_n - 8];
}

static void link_end(struct link *l)
{
    if (l->conn.open) {
        net_client_close(&l->conn);
    }

    l->used = false;
}

static void tcp(const uint8_t dst[4], const uint8_t *t, uint32_t n)
{
    uint16_t sport, dport;
    uint32_t seq, ack, hl, data;
    uint8_t flags, where[4];
    struct link *l;

    if (n < 20 || (hl = (uint32_t)(t[12] >> 4) * 4) < 20 || hl > n) {
        return;
    }

    sport = rd16(t);
    dport = rd16(t + 2);
    seq = rd32(t + 4);
    ack = rd32(t + 8);
    flags = t[13];
    data = n - hl;
    l = link_find(sport, dst, dport);

    if ((flags & TCP_SYN) != 0 && (flags & TCP_ACK) == 0) {
        if (l == NULL && !outgoing) {
            struct link stray;

            /* Given only ports: it opens nothing of its own. */
            memset(&stray, 0, sizeof(stray));
            stray.gport = sport;
            stray.dport = dport;
            memcpy(stray.dip, dst, 4);
            stray.rcv_nxt = seq + 1;
            tcp_to_guest(&stray, TCP_RST | TCP_ACK, NULL, 0, false);
            return;
        }

        if (l == NULL) {
            /* The gateway's address is this machine. */
            if (memcmp(dst, GATE_IP, 4) == 0) {
                uint8_t mask[4];

                if (net_client_address(where, mask) != NET_OK) {
                    memcpy(where, dst, 4);
                }
            } else {
                memcpy(where, dst, 4);
            }

            if ((l = link_new()) == NULL) {
                return;
            }

            l->gport = sport;
            l->dport = dport;
            memcpy(l->dip, dst, 4);
            l->rcv_nxt = seq + 1;
            l->snd_nxt = l->snd_una = (uint32_t)kosmos_ticks();

            if (net_client_connect(&l->conn, where, dport) != NET_OK) {
                tcp_to_guest(l, TCP_RST | TCP_ACK, NULL, 0, false);
                link_end(l);
                return;
            }
        } else {
            l->snd_nxt = l->snd_una;        /* its SYN again: our SYN-ACK again */
        }

        l->wnd = rd16(t + 14);
        tcp_to_guest(l, TCP_SYN | TCP_ACK, NULL, 0, true);
        l->snd_nxt++;
        return;
    }

    if (l == NULL) {
        if ((flags & TCP_RST) == 0) {
            struct link stray;

            memset(&stray, 0, sizeof(stray));
            stray.gport = sport;
            stray.dport = dport;
            memcpy(stray.dip, dst, 4);
            stray.snd_nxt = ack;
            stray.rcv_nxt = seq + data + ((flags & TCP_FIN) ? 1 : 0);
            tcp_to_guest(&stray, TCP_RST | TCP_ACK, NULL, 0, false);
        }

        return;
    }

    if ((flags & TCP_RST) != 0) {
        link_end(l);
        return;
    }

    /* One carried in: the guest's SYN-ACK to ours, and it is open. */
    if (l->opening) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && l->syn_sent && ack == l->snd_nxt) {
            l->rcv_nxt = seq + 1;
            l->snd_una = ack;
            l->wnd = rd16(t + 14);
            l->opening = false;
            tcp_to_guest(l, TCP_ACK, NULL, 0, false);
        }

        return;
    }

    if ((flags & TCP_ACK) != 0 && (int32_t)(ack - l->snd_una) > 0
        && (int32_t)(ack - l->snd_nxt) <= 0) {
        l->snd_una = ack;
    }

    l->wnd = rd16(t + 14);

    if (data > 0) {
        if (seq == l->rcv_nxt) {
            long took = net_client_write(&l->conn, t + hl, data);

            if (took < 0) {
                tcp_to_guest(l, TCP_RST | TCP_ACK, NULL, 0, false);
                link_end(l);
                return;
            }

            l->rcv_nxt += (uint32_t)took;
            counted_out += (uint64_t)took;
        }

        tcp_to_guest(l, TCP_ACK, NULL, 0, false);
    }

    if ((flags & TCP_FIN) != 0 && seq + data == l->rcv_nxt && !l->guest_fin) {
        l->guest_fin = true;
        l->rcv_nxt++;
        tcp_to_guest(l, TCP_ACK, NULL, 0, false);

        /*
         * Done sending, so Kosmos's side is told - its FIN follows every
         * byte already written - and is over for the guest too: a server in
         * the guest that closes after its answer is waited on by nobody.
         * Unless Kosmos's end had already said all it had to.
         */
        if (!l->fin_sent) {
            net_client_close(&l->conn);     /* and the pump says FIN for it */
        }
    }

    if (l->guest_fin && l->fin_sent && l->snd_una == l->snd_nxt) {
        link_end(l);
    }
}

static void from_guest(const uint8_t *f, uint32_t n)
{
    const uint8_t *ip;
    uint32_t hl, total;

    if (n < 14) {
        return;
    }

    if (rd16(f + 12) == 0x0806) {
        arp(f, n);
        return;
    }

    if (rd16(f + 12) != 0x0800 || n < 34) {
        return;
    }

    ip = f + 14;
    hl = (uint32_t)(ip[0] & 15) * 4;
    total = rd16(ip + 2);

    if ((ip[0] >> 4) != 4 || hl < 20 || total < hl || 14 + total > n
        || memcmp(ip + 12, GUEST_IP, 4) != 0) {
        return;
    }

    switch (ip[9]) {
    case 1:
        if (ours(ip + 16)) {
            ping(ip + 16, ip + hl, total - hl);
        }
        break;
    case 17:
        if (outgoing && memcmp(ip + 16, DNS_IP, 4) == 0 && total - hl >= 8 && rd16(ip + hl + 2) == 53) {
            dns(rd16(ip + hl), ip + hl + 8, total - hl - 8);
        }
        break;
    case 6:
        tcp(ip + 16, ip + hl, total - hl);
        break;
    default:
        break;
    }
}

static void notify(struct vdev *d, unsigned q)
{
    static uint8_t buf[65536 + NET_HDR];
    struct vdev_chain c;

    if (q != TX) {
        vmmnet_pump();                      /* new buffers posted: send what waits */
        return;
    }

    while (vdev_pop(d, TX, &c)) {
        uint32_t n = vdev_gather(&c, buf, sizeof(buf));

        vdev_push(d, TX, c.head, 0);

        if (n > NET_HDR) {
            from_guest(buf + NET_HDR, n - NET_HDR);
        }
    }
}

/* ---------------------------------------------------------------------
 * From Kosmos's side: what arrived on each connection, to the guest.
 * --------------------------------------------------------------------- */

bool vmmnet_pump(void)
{
    static uint8_t data[MSS];
    bool any = false;
    unsigned i;

    if (!on) {
        return false;
    }

    /* Carried in since: each a link, its SYN to the guest's port. */
    while (arrived_tail != __atomic_load_n(&arrived_head, __ATOMIC_ACQUIRE)) {
        struct arrival *a = &arrived[arrived_tail % ARRIVALS];
        struct link *l = link_new();

        if (l == NULL) {
            net_client_close(&a->conn);
        } else {
            l->opening = true;
            l->gport = a->to;
            l->dport = next_port;
            memcpy(l->dip, GATE_IP, 4);
            l->snd_nxt = l->snd_una = (uint32_t)kosmos_ticks();
            l->conn = a->conn;
            next_port = next_port == 65535 ? 49152 : (uint16_t)(next_port + 1);
        }

        __atomic_store_n(&arrived_tail, arrived_tail + 1, __ATOMIC_RELEASE);
    }

    for (i = 0; i < links_n; i++) {
        struct link *l = &links[i];

        if (l->used && l->opening) {
            if (!l->syn_sent && tcp_to_guest(l, TCP_SYN, NULL, 0, true)) {
                l->syn_sent = true;
                l->snd_nxt++;
                any = true;
            }

            continue;                       /* nothing to carry until it answers */
        }

        while (l->used && !l->fin_sent && vdev_waiting(&dev, RX)) {
            uint32_t flight = l->snd_nxt - l->snd_una;
            uint32_t room = l->wnd > flight ? l->wnd - flight : 0;
            long got;

            if (room == 0) {
                break;
            }

            got = l->conn.open ? net_client_read(&l->conn, data, room < MSS ? room : MSS) : -1;

            if (got > 0) {
                tcp_to_guest(l, TCP_PSH | TCP_ACK, data, (uint32_t)got, false);
                l->snd_nxt += (uint32_t)got;
                counted_in += (uint64_t)got;
                any = true;
            } else if (got < 0) {
                /* Over at Kosmos's end, and nothing left: its FIN. */
                tcp_to_guest(l, TCP_FIN | TCP_ACK, NULL, 0, false);
                l->snd_nxt++;
                l->fin_sent = true;
                any = true;
            } else {
                break;
            }
        }

        if (l->used && l->guest_fin && l->fin_sent && l->snd_una == l->snd_nxt) {
            link_end(l);
        }
    }

    return any;
}

bool vmmnet_busy(void)
{
    unsigned i;

    for (i = 0; on && i < links_n; i++) {
        if (links[i].used) {
            return true;
        }
    }

    return false;
}

static void config(struct vdev *d, uint64_t off, uint8_t *out, unsigned size)
{
    unsigned k;

    (void)d;

    for (k = 0; k < size && off + k < 6; k++) {
        out[k] = GUEST_MAC[off + k];        /* `struct virtio_net_config`: its mac */
    }
}

/*
 * **The waiter**: Kosmos's listeners, waited on a second at a time, and
 * whoever arrived accepted and left for the guest's thread. When the ring
 * is full it waits a tick for room rather than accepting what it could not
 * hand on.
 */
static void waiter(unsigned long unused)
{
    struct net_poll_entry set[FORWARDS];

    (void)unused;

    for (;;) {
        unsigned i;

        for (i = 0; i < forwards_n; i++) {
            set[i].handle = forwards[i].listener;
            set[i].want = NET_WANT_ACCEPT;
            set[i].got = 0;
        }

        if (net_client_poll(set, forwards_n, tick_hz_here) != NET_OK) {
            kosmos_sleep(1);
            continue;
        }

        for (i = 0; i < forwards_n; i++) {
            uint32_t head = arrived_head;
            struct arrival *a;

            if ((set[i].got & NET_WANT_ACCEPT) == 0) {
                continue;
            }

            if (head - __atomic_load_n(&arrived_tail, __ATOMIC_ACQUIRE) >= ARRIVALS) {
                kosmos_sleep(1);
                break;
            }

            a = &arrived[head % ARRIVALS];

            if (net_client_accept(forwards[i].listener, 1, &a->conn) == NET_OK) {
                a->to = forwards[i].to;
                __atomic_store_n(&arrived_head, head + 1, __ATOMIC_RELEASE);
            }
        }
    }
}

unsigned vmmnet_start(long net_cap, uint32_t tick_hz, bool out,
                      const uint16_t *from, const uint16_t *to, unsigned n)
{
    unsigned refused = 0;

    net_client_start(net_cap);
    dns_wait_ticks = tick_hz * 2;
    tick_hz_here = tick_hz;
    outgoing = out;
    memset(&dev, 0, sizeof(dev));
    dev.id = VIRTIO_ID_NET;
    dev.features = VIRTIO_NET_F_MAC;
    dev.queues = 2;
    dev.config = config;
    dev.notify = notify;
    on = true;

    for (unsigned i = 0; i < n && i < FORWARDS; i++) {
        struct forward *f = &forwards[forwards_n];

        if (from[i] == 0) {
            continue;
        }

        if (net_client_listen(from[i], &f->listener) != NET_OK) {
            refused |= 1u << i;
            continue;
        }

        f->from = from[i];
        f->to = to[i];
        forwards_n++;
    }

    if (forwards_n > 0 && kosmos_thread_start(waiter, 0) < 0) {
        refused = (1u << n) - 1;
    }

    return refused;
}

bool vmmnet_mmio(uint64_t off, unsigned size, bool write, uint64_t *value)
{
    return on && vdev_mmio(&dev, off, size, write, value);
}

bool vmmnet_raised(void)
{
    return on && vdev_raised(&dev);
}

void vmmnet_counted(uint64_t *in, uint64_t *out)
{
    *in = counted_in;
    *out = counted_out;
}
