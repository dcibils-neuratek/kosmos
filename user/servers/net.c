/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /net: Ethernet, ARP, IPv4 and ICMP echo.
 *
 * The one process that holds the card. `SPAWN_NET` is the disk's grant
 * pointed outwards - whoever can put a raw frame on a wire can claim any
 * address on the network and read every frame that reaches the machine - so
 * exactly one process gets it and everything else asks this one.
 *
 * **C, because it is a server and it is on the packet path.** `CLAUDE.md`
 * draws that line by layer rather than by judgement, and the reason it does
 * is the audio server: written in Lua because somebody decided it did not
 * look like a frame path, and it was a period path, which is a frame path
 * with a different clock. A stack is not going to be the third case.
 *
 * **There is no TCP here and that is not an omission.** Ping needs
 * Ethernet, ARP, IPv4 and ICMP, and that is what this is. TCP is a state
 * machine, retransmission, windows and four timers - a different piece of
 * work with different mistakes in it, and half of one is worse than none.
 * `netproto.h` records where the shared ring goes when it arrives, so that
 * decision is made before somebody finds a message worked for the first ten
 * kilobytes.
 *
 * **Fixed pools, no allocator**, the same as every other server here. Eight
 * echoes in flight, sixteen ARP entries. Running out is an error at a known
 * limit rather than a failure at an unknown one.
 *
 * **Written against RFC 791, 792 and 826 from knowledge**, so the field
 * offsets are the part to distrust. What establishes them is a reply coming
 * back from a real host: a wrong offset produces no answer at all rather
 * than a wrong one, because the far end is checking the same fields.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "netproto.h"

/*------------------------------------------------------------------------
 * The wire, as offsets.
 *
 * Byte offsets and explicit assembly rather than packed structs. A struct
 * with `__attribute__((packed))` would read better and would put this file's
 * correctness in the hands of a compiler attribute; every one of these
 * fields is big-endian on the wire and this machine is not, so the
 * conversion has to be written down somewhere and writing it here makes it
 * visible.
 *----------------------------------------------------------------------*/

#define ETH_DST         0
#define ETH_SRC         6
#define ETH_TYPE        12
#define ETH_HEADER      14

#define ETHERTYPE_IP    0x0800u
#define ETHERTYPE_ARP   0x0806u

/* RFC 826. Only Ethernet-and-IPv4 is understood, which is the only pair
 * anything here will meet. */
#define ARP_HTYPE       0
#define ARP_PTYPE       2
#define ARP_HLEN        4
#define ARP_PLEN        5
#define ARP_OP          6
#define ARP_SHA         8
#define ARP_SPA         14
#define ARP_THA         18
#define ARP_TPA         24
#define ARP_LENGTH      28

#define ARP_REQUEST     1u
#define ARP_REPLY       2u

/* RFC 791, and no options: this stack builds a twenty-byte header and
 * refuses to parse one with options rather than half-understanding it. */
#define IP_VER_IHL      0
#define IP_TOS          1
#define IP_LENGTH       2
#define IP_ID           4
#define IP_FLAGS        6
#define IP_TTL          8
#define IP_PROTO        9
#define IP_CHECKSUM     10
#define IP_SRC          12
#define IP_DST          16
#define IP_HEADER       20

#define IP_PROTO_ICMP   1u

/* RFC 792. */
#define ICMP_TYPE       0
#define ICMP_CODE       1
#define ICMP_CHECKSUM   2
#define ICMP_ID         4
#define ICMP_SEQ        6
#define ICMP_HEADER     8

#define ICMP_ECHO_REPLY     0u
#define ICMP_ECHO_REQUEST   8u

#define ARP_CACHE       16u

/*------------------------------------------------------------------------
 * State. All of it fixed, all of it here.
 *----------------------------------------------------------------------*/

struct arp_entry {
    struct net_addr ip;
    uint8_t         mac[6];
    bool            known;
};

/*
 * An echo that has gone out and not come back.
 *
 * `who` is a sender the kernel gave us and we have not replied to - the same
 * trick the console plays with `read` and ramfs with `watch`. The caller is
 * parked inside `call` and nothing here is blocked on it, which is what lets
 * one process ping while another reads a file.
 */
struct pending {
    bool     used;
    uint64_t who;
    uint16_t id;
    uint16_t seq;
    uint64_t sent_at;               /* counter ticks, undecoded */
    struct net_addr to;
};

static struct {
    bool     has_card;
    uint8_t  mac[6];
    uint32_t mtu;

    struct net_addr address;
    struct net_addr netmask;
    struct net_addr gateway;
    bool     configured;

    struct arp_entry arp[ARP_CACHE];
    struct pending   pending[NET_PENDING_MAX];

    uint16_t next_id;               /* the IP header's, and the echo's */

    uint8_t  frame[NET_FRAME_MAX];  /* one at a time; nothing here queues */
} net;

/*------------------------------------------------------------------------
 * Numbers on the wire.
 *----------------------------------------------------------------------*/

static void put16(uint8_t *at, uint16_t v)
{
    at[0] = (uint8_t)(v >> 8);
    at[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *at)
{
    return (uint16_t)((uint16_t)at[0] << 8 | at[1]);
}

/*
 * The one's-complement checksum RFC 1071 describes, and the two things
 * about it that catch people.
 *
 * It is computed over 16-bit words with the carries folded back in, and the
 * *complement* is what goes in the field - so running it again over a buffer
 * that already holds its own checksum gives zero, which is how a receiver
 * verifies one without knowing where the field is.
 *
 * An odd length pads with a zero byte on the right rather than the left.
 * Getting that backwards produces a checksum that is correct for every
 * even-length packet, which is most of them.
 */
static uint16_t checksum(const uint8_t *data, unsigned length)
{
    uint32_t sum = 0;
    unsigned i;

    for (i = 0; i + 1 < length; i += 2) {
        sum += get16(data + i);
    }

    if ((length & 1u) != 0) {
        sum += (uint32_t)data[length - 1] << 8;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }

    return (uint16_t)(~sum & 0xffffu);
}

static bool same_addr(const struct net_addr *a, const struct net_addr *b)
{
    return memcmp(a->byte, b->byte, 4) == 0;
}

/*------------------------------------------------------------------------
 * ARP.
 *----------------------------------------------------------------------*/

static const uint8_t BROADCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static struct arp_entry *arp_find(const struct net_addr *ip)
{
    unsigned i;

    for (i = 0; i < ARP_CACHE; i++) {
        if (net.arp[i].known && same_addr(&net.arp[i].ip, ip)) {
            return &net.arp[i];
        }
    }

    return NULL;
}

/*
 * Remember an address.
 *
 * Every entry is learned from a frame that arrived, which is what an ARP
 * cache is and also what makes it the oldest attack in networking: anybody
 * on the wire can tell this machine that they are the router. There is
 * nothing to do about that at this layer and it is worth writing down rather
 * than leaving as an unexamined assumption - the answer is upstairs, in not
 * trusting a network you did not authenticate.
 *
 * The oldest is overwritten when the table is full, by position rather than
 * by age, because keeping an age means a clock read on a path that does not
 * otherwise need one and sixteen entries is more than a home network has.
 */
static void arp_learn(const struct net_addr *ip, const uint8_t *mac)
{
    struct arp_entry *e = arp_find(ip);
    static unsigned next;

    if (e == NULL) {
        unsigned i;

        for (i = 0; i < ARP_CACHE; i++) {
            if (!net.arp[i].known) {
                e = &net.arp[i];
                break;
            }
        }

        if (e == NULL) {
            e = &net.arp[next % ARP_CACHE];
            next++;
        }
    }

    e->ip = *ip;
    memcpy(e->mac, mac, 6);
    e->known = true;
}

static bool send_frame(const uint8_t *dst, uint16_t type,
                       const uint8_t *body, unsigned length)
{
    if (ETH_HEADER + length > NET_FRAME_MAX) {
        return false;
    }

    memcpy(net.frame + ETH_DST, dst, 6);
    memcpy(net.frame + ETH_SRC, net.mac, 6);
    put16(net.frame + ETH_TYPE, type);
    memcpy(net.frame + ETH_HEADER, body, length);

    return kosmos_net_send(net.frame, ETH_HEADER + length) == 0;
}

static bool arp_ask(const struct net_addr *ip)
{
    uint8_t body[ARP_LENGTH];

    memset(body, 0, sizeof(body));

    put16(body + ARP_HTYPE, 1);             /* Ethernet */
    put16(body + ARP_PTYPE, ETHERTYPE_IP);
    body[ARP_HLEN] = 6;
    body[ARP_PLEN] = 4;
    put16(body + ARP_OP, ARP_REQUEST);

    memcpy(body + ARP_SHA, net.mac, 6);
    memcpy(body + ARP_SPA, net.address.byte, 4);
    /* THA stays zero: that is the question. */
    memcpy(body + ARP_TPA, ip->byte, 4);

    return send_frame(BROADCAST, ETHERTYPE_ARP, body, sizeof(body));
}

static void arp_receive(const uint8_t *body, unsigned length)
{
    struct net_addr sender;

    if (length < ARP_LENGTH) {
        return;
    }

    if (get16(body + ARP_PTYPE) != ETHERTYPE_IP
        || body[ARP_HLEN] != 6 || body[ARP_PLEN] != 4) {
        return;                     /* not a pair this stack speaks */
    }

    memcpy(sender.byte, body + ARP_SPA, 4);
    arp_learn(&sender, body + ARP_SHA);

    /*
     * And answer, if it was for us.
     *
     * Answering is not optional on a real network: a host that never
     * replies to ARP is a host nothing can send to, so it can ping out and
     * nothing can ping in - which looks like a firewall rather than a bug.
     */
    if (get16(body + ARP_OP) == ARP_REQUEST && net.configured
        && memcmp(body + ARP_TPA, net.address.byte, 4) == 0) {
        uint8_t reply[ARP_LENGTH];

        memset(reply, 0, sizeof(reply));

        put16(reply + ARP_HTYPE, 1);
        put16(reply + ARP_PTYPE, ETHERTYPE_IP);
        reply[ARP_HLEN] = 6;
        reply[ARP_PLEN] = 4;
        put16(reply + ARP_OP, ARP_REPLY);

        memcpy(reply + ARP_SHA, net.mac, 6);
        memcpy(reply + ARP_SPA, net.address.byte, 4);
        memcpy(reply + ARP_THA, body + ARP_SHA, 6);
        memcpy(reply + ARP_TPA, body + ARP_SPA, 4);

        (void)send_frame(body + ARP_SHA, ETHERTYPE_ARP, reply, sizeof(reply));
    }
}

/*
 * Which machine to hand a frame to for a given address.
 *
 * On this subnet it is that machine; off it, the router. That one comparison
 * is the whole of routing here, and it is enough: a home network has one way
 * out. A routing *table* is what you need when there is more than one, and
 * there is not.
 */
static const struct net_addr *next_hop(const struct net_addr *to)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        if ((to->byte[i] & net.netmask.byte[i])
            != (net.address.byte[i] & net.netmask.byte[i])) {
            return &net.gateway;
        }
    }

    return to;
}

/*------------------------------------------------------------------------
 * IPv4 and ICMP.
 *----------------------------------------------------------------------*/

static bool send_ip(const struct net_addr *to, uint8_t protocol,
                    const uint8_t *body, unsigned length)
{
    uint8_t packet[IP_HEADER + NET_PAYLOAD_MAX + ICMP_HEADER];
    const struct net_addr *hop;
    struct arp_entry *e;

    if (IP_HEADER + length > sizeof(packet)) {
        return false;
    }

    hop = next_hop(to);
    e = arp_find(hop);

    if (e == NULL) {
        /*
         * Nobody knows where that is yet. The question goes out and this
         * fails - it does not queue the packet waiting for an answer.
         *
         * A real stack holds one packet per unresolved address and sends it
         * when the reply lands. Here the caller is a person typing `ping`,
         * the second attempt is a second later, and by then the cache is
         * warm. Queuing would be a buffer, a timeout and a retry policy for
         * a case that resolves itself.
         */
        (void)arp_ask(hop);
        return false;
    }

    memset(packet, 0, IP_HEADER);

    packet[IP_VER_IHL] = 0x45;      /* version 4, twenty-byte header */
    packet[IP_TOS]     = 0;
    put16(packet + IP_LENGTH, (uint16_t)(IP_HEADER + length));
    put16(packet + IP_ID, net.next_id++);
    put16(packet + IP_FLAGS, 0x4000u);   /* don't fragment, offset zero */
    packet[IP_TTL]     = 64;
    packet[IP_PROTO]   = protocol;

    memcpy(packet + IP_SRC, net.address.byte, 4);
    memcpy(packet + IP_DST, to->byte, 4);

    /* Over the header only, and with the field itself zero - which it is,
     * from the memset. IPv4's checksum does not cover the payload; ICMP's
     * covers its own. */
    put16(packet + IP_CHECKSUM, checksum(packet, IP_HEADER));

    memcpy(packet + IP_HEADER, body, length);

    return send_frame(e->mac, ETHERTYPE_IP, packet, IP_HEADER + length);
}

static struct pending *pending_find(uint16_t id, uint16_t seq)
{
    unsigned i;

    for (i = 0; i < NET_PENDING_MAX; i++) {
        if (net.pending[i].used && net.pending[i].id == id
            && net.pending[i].seq == seq) {
            return &net.pending[i];
        }
    }

    return NULL;
}

static void answer(uint64_t who, const struct net_reply *reply)
{
    struct message msg;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*reply);
    memcpy(msg.data, reply, sizeof(*reply));

    (void)kosmos_reply(who, &msg);
}

static void fail(uint64_t who, uint32_t status)
{
    struct net_reply reply;

    memset(&reply, 0, sizeof(reply));
    reply.status = status;

    answer(who, &reply);
}

/*
 * An echo reply came back. Find whoever is waiting for it and answer them.
 *
 * Matched on the identifier *and* the sequence: the identifier says the echo
 * is ours rather than another machine's, and the sequence says which one.
 * Matching on sequence alone would have two programs pinging two hosts
 * answer each other's.
 */
static void icmp_receive(const uint8_t *packet, unsigned length,
                         const struct net_addr *from, uint8_t ttl)
{
    const uint8_t *icmp = packet + IP_HEADER;
    unsigned icmp_len;

    if (length < IP_HEADER + ICMP_HEADER) {
        return;
    }

    icmp_len = length - IP_HEADER;

    if (icmp[ICMP_TYPE] == ICMP_ECHO_REQUEST) {
        /*
         * Somebody pinged us. Answered, because a host that does not is a
         * host nobody can check is alive - and because it is four lines:
         * the reply is the request with the type changed and the checksum
         * recomputed.
         */
        uint8_t reply[NET_PAYLOAD_MAX + ICMP_HEADER];

        if (icmp_len > sizeof(reply)) {
            return;
        }

        memcpy(reply, icmp, icmp_len);
        reply[ICMP_TYPE] = ICMP_ECHO_REPLY;
        put16(reply + ICMP_CHECKSUM, 0);
        put16(reply + ICMP_CHECKSUM, checksum(reply, icmp_len));

        (void)send_ip(from, IP_PROTO_ICMP, reply, icmp_len);
        return;
    }

    if (icmp[ICMP_TYPE] == ICMP_ECHO_REPLY) {
        struct pending *p = pending_find(get16(icmp + ICMP_ID),
                                         get16(icmp + ICMP_SEQ));
        struct net_reply reply;
        unsigned payload;

        if (p == NULL) {
            return;                 /* not ours, or already answered */
        }

        memset(&reply, 0, sizeof(reply));

        reply.status = NET_OK;
        reply.seq    = p->seq;
        reply.from   = *from;
        reply.ttl    = ttl;
        reply.ticks  = kosmos_ticks() - p->sent_at;

        payload = icmp_len - ICMP_HEADER;

        if (payload > NET_PAYLOAD_MAX) {
            payload = NET_PAYLOAD_MAX;
        }

        reply.length = payload;
        memcpy(reply.payload, icmp + ICMP_HEADER, payload);

        answer(p->who, &reply);
        p->used = false;
    }
}

static void ip_receive(const uint8_t *packet, unsigned length)
{
    struct net_addr from;
    struct net_addr to;
    unsigned header;
    unsigned total;

    if (length < IP_HEADER) {
        return;
    }

    if ((packet[IP_VER_IHL] >> 4) != 4) {
        return;                     /* not IPv4, and there is no IPv6 here */
    }

    header = (unsigned)(packet[IP_VER_IHL] & 0x0fu) * 4u;

    /*
     * Options are refused rather than skipped.
     *
     * Skipping them is one line and would be wrong in a way that hides: a
     * packet with options is a packet somebody built deliberately, and a
     * stack that ignores what it does not understand is a stack that
     * silently accepts what it should have questioned. Nothing sends them.
     */
    if (header != IP_HEADER) {
        return;
    }

    total = get16(packet + IP_LENGTH);

    if (total > length || total < IP_HEADER) {
        return;                     /* the frame is shorter than it claims */
    }

    /*
     * Fragments are refused, and this is where the honesty matters. A
     * fragmented packet needs a reassembly buffer, a timer and a policy
     * about overlapping pieces - which is a well-known way to get a stack
     * wrong. Nothing here sends the don't-fragment bit clear, and an echo
     * is sixty-four bytes.
     */
    if ((get16(packet + IP_FLAGS) & 0x3fffu) != 0) {
        return;
    }

    /* The header carries its own checksum, so running it over the whole
     * header including the field gives zero. */
    if (checksum(packet, IP_HEADER) != 0) {
        return;
    }

    memcpy(from.byte, packet + IP_SRC, 4);
    memcpy(to.byte, packet + IP_DST, 4);

    if (net.configured && !same_addr(&to, &net.address)) {
        return;                     /* somebody else's, on a shared wire */
    }

    if (packet[IP_PROTO] == IP_PROTO_ICMP) {
        icmp_receive(packet, total, &from, packet[IP_TTL]);
    }
}

/* Everything the card has, decoded. Drained rather than one at a time: a
 * burst arrives together and leaving frames in the ring is leaving the
 * device with fewer buffers than it needs for the next one. */
static void drain(void)
{
    for (;;) {
        long got = kosmos_net_recv(net.frame, sizeof(net.frame));
        uint16_t type;

        if (got < (long)ETH_HEADER) {
            return;
        }

        type = get16(net.frame + ETH_TYPE);

        if (type == ETHERTYPE_ARP) {
            arp_receive(net.frame + ETH_HEADER,
                        (unsigned)got - ETH_HEADER);
        } else if (type == ETHERTYPE_IP) {
            ip_receive(net.frame + ETH_HEADER, (unsigned)got - ETH_HEADER);
        }
    }
}

/*------------------------------------------------------------------------
 * The protocol.
 *----------------------------------------------------------------------*/

static void serve(const struct message *msg, uint64_t sender)
{
    struct net_request req;
    struct net_reply reply;

    if (msg->length < sizeof(req)) {
        fail(sender, NET_ERR_BAD_OP);
        return;
    }

    memcpy(&req, msg->data, sizeof(req));
    memset(&reply, 0, sizeof(reply));

    switch (req.op) {
    case NET_OP_INFO:
        reply.status   = NET_OK;
        reply.has_card = net.has_card ? 1u : 0u;
        reply.mtu      = net.mtu;
        reply.address  = net.address;
        reply.netmask  = net.netmask;
        reply.gateway  = net.gateway;
        memcpy(reply.mac, net.mac, sizeof(reply.mac));

        answer(sender, &reply);
        return;

    case NET_OP_CONFIG:
        net.address    = req.address;
        net.netmask    = req.netmask;
        net.gateway    = req.gateway;
        net.configured = true;

        /*
         * And ask for the gateway straight away, rather than waiting for
         * the first packet to fail.
         *
         * Almost every first packet goes through the router, so without
         * this the first `ping` always reports one lost - which reads as a
         * flaky network rather than as a cache that had not been filled.
         */
        (void)arp_ask(&net.gateway);

        reply.status = NET_OK;
        answer(sender, &reply);
        return;

    case NET_OP_PING: {
        uint8_t echo[ICMP_HEADER + NET_PAYLOAD_MAX];
        struct pending *p = NULL;
        unsigned length;
        unsigned i;

        if (!net.has_card) {
            fail(sender, NET_ERR_NO_CARD);
            return;
        }

        if (!net.configured) {
            fail(sender, NET_ERR_NO_ROUTE);
            return;
        }

        for (i = 0; i < NET_PENDING_MAX; i++) {
            if (!net.pending[i].used) {
                p = &net.pending[i];
                break;
            }
        }

        if (p == NULL) {
            fail(sender, NET_ERR_FULL);
            return;
        }

        length = req.length;

        if (length > NET_PAYLOAD_MAX) {
            length = NET_PAYLOAD_MAX;
        }

        memset(echo, 0, ICMP_HEADER);
        echo[ICMP_TYPE] = ICMP_ECHO_REQUEST;
        echo[ICMP_CODE] = 0;
        put16(echo + ICMP_ID, net.next_id);
        put16(echo + ICMP_SEQ, (uint16_t)req.seq);
        memcpy(echo + ICMP_HEADER, req.payload, length);

        /* Over the whole message, header and payload, with the field zero.
         * ICMP has no pseudo-header, which is the one thing that makes it
         * simpler than everything above it. */
        put16(echo + ICMP_CHECKSUM,
              checksum(echo, ICMP_HEADER + length));

        p->used    = true;
        p->who     = sender;
        p->id      = net.next_id;
        p->seq     = (uint16_t)req.seq;
        p->to      = req.to;
        p->sent_at = kosmos_ticks();

        if (!send_ip(&req.to, IP_PROTO_ICMP, echo, ICMP_HEADER + length)) {
            p->used = false;
            fail(sender, NET_ERR_UNREACHABLE);
            return;
        }

        /*
         * And no reply. The caller stays parked inside `call` until an echo
         * comes back or the loop below gives up on it - which is the whole
         * point: this process is not blocked, and something else can be
         * served while a ping is in the air.
         */
        return;
    }

    default:
        fail(sender, NET_ERR_BAD_OP);
        return;
    }
}

/*
 * Give up on echoes nobody answered.
 *
 * A ping that is never replied to would otherwise park its caller for ever,
 * which is not a lost packet - it is a hung program. One second, in counter
 * ticks read from the machine rather than assumed, because that frequency is
 * 62.5 MHz under TCG and 24 MHz under `hvf` and a constant here would be a
 * timeout four times too long on one of them.
 */
static void expire(uint64_t hz)
{
    unsigned i;
    uint64_t now = kosmos_ticks();

    for (i = 0; i < NET_PENDING_MAX; i++) {
        struct pending *p = &net.pending[i];

        if (p->used && now - p->sent_at > hz) {
            p->used = false;
            fail(p->who, NET_ERR_UNREACHABLE);
        }
    }
}

void net_server(long endpoint)
{
    struct netinfo card;
    uint64_t hz;

    memset(&net, 0, sizeof(net));

    if (kosmos_net_info(&card) == 0 && card.present != 0) {
        net.has_card = true;
        net.mtu      = card.mtu;
        memcpy(net.mac, card.mac, sizeof(net.mac));
    }

    {
        struct sysinfo info;

        hz = (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0)
             ? info.counter_hz : 62500000UL;
    }

    net.next_id = 1;

    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        long status;

        /*
         * A deadline, because two different things can happen: a message,
         * or a frame. The card wakes this process through
         * `process_wake_net` when one arrives, so the deadline is a backstop
         * for the pings that have to be given up on rather than the
         * mechanism - the same arrangement the audio server has, and for the
         * same reason.
         */
        status = kosmos_receive(endpoint, &msg, &sender, 0, 25UL);

        drain();
        expire(hz);

        if (status == 0) {
            serve(&msg, sender);
        }
    }
}
