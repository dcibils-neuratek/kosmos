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
 * **TCP, and the half of it a client needs.** This end connects out; there
 * is no LISTEN, no SYN_RECEIVED and no simultaneous open, because telnet and
 * SSH ask and a server answers. What is here is the path a client takes
 * through RFC 793's diagram - SYN_SENT, ESTABLISHED, and a close from either
 * side - with one retransmission timer and a window that is the ring's free
 * space rather than a number this end made up.
 *
 * **A connection's bytes never travel in a message.** `tcpring.h` is the
 * region both sides hold, and that decision was written down before a byte
 * moved rather than after somebody found a message worked for the first ten
 * kilobytes. The audio server is what the rule was learned from.
 *
 * **Fixed pools, no allocator**, the same as every other server here. Eight
 * echoes in flight, sixteen ARP entries, four connections. Running out is an
 * error at a known limit rather than a failure at an unknown one.
 *
 * **Written against RFC 791, 792, 793 and 826 from knowledge**, so the field
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
#include "tcpring.h"

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
#define IP_PROTO_TCP    6u

/*
 * RFC 793, and the twenty-byte header this stack builds.
 *
 * Options are sent on the SYN only - the maximum segment size, which a far
 * end needs in order not to send something this cannot take - and are
 * skipped on the way in. Skipping is right here where it was wrong for IP:
 * TCP options are ordinary and every stack sends them, where an IP option is
 * something somebody built deliberately.
 */
#define TCP_SPORT       0
#define TCP_DPORT       2
#define TCP_SEQ         4
#define TCP_ACK         8
#define TCP_OFF         12
#define TCP_FLAGS       13
#define TCP_WINDOW      14
#define TCP_CHECKSUM    16
#define TCP_URGENT      18
#define TCP_HEADER      20

#define TCP_FIN         0x01u
#define TCP_SYN         0x02u
#define TCP_RST         0x04u
#define TCP_PSH         0x08u
#define TCP_ACK_FLAG    0x10u

/*
 * The states this stack has, which are not all nine of TCP's.
 *
 * There is no LISTEN, SYN_RECEIVED or CLOSING, because this end always
 * connects out and `netproto.h` says why. What is left is the client's path
 * through the diagram: ask, talk, and shut down from either side.
 */
#define ST_FREE         0
#define ST_SYN_SENT     1
#define ST_OPEN         2
#define ST_FIN_WAIT     3       /* we sent FIN, waiting for theirs */
#define ST_CLOSE_WAIT   4       /* they sent FIN, we may still send */
#define ST_LAST_ACK     5       /* both sent; waiting to be acknowledged */
#define ST_DEAD         6

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

/*
 * One connection.
 *
 * `snd_una` is the oldest byte this end has sent and not had acknowledged,
 * `snd_nxt` the next it will send, and `rcv_nxt` what it expects from the
 * far end. Those three names are RFC 793's and are worth keeping: every
 * sentence in that document about what is legal is written in them.
 *
 * **One segment in flight, retransmitted on a timer.** A real stack keeps a
 * queue of unacknowledged segments and slides a window over it; this sends
 * what fits, remembers where it started, and sends it again if nothing
 * acknowledges it. That costs throughput on a long fat link and costs
 * nothing on a line protocol, which is what this is for - and it is one
 * timer to get right instead of four.
 */
struct conn {
    unsigned state;
    uint64_t opened_by;             /* the caller parked in CONNECT */
    uint64_t waiter;                /* the caller parked in WAIT, or 0 */
    uint64_t wait_until;

    struct net_addr remote;
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint32_t peer_window;

    uint64_t sent_at;               /* when the unacknowledged segment went */
    uint32_t tries;
    bool     fin_sent;

    /* The region, and where it is mapped in this process. */
    long     region;
    struct tcp_ring *ring;
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

    struct conn conn[NET_CONN_MAX];
    uint16_t next_port;
    uint32_t next_seq;              /* where the next connection starts */

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

/* Written below, with the rest of TCP; declared here because a packet is
 * dispatched before it is decoded. */
static void tcp_receive(const uint8_t *packet, unsigned total,
                        const struct net_addr *from);

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
    } else if (packet[IP_PROTO] == IP_PROTO_TCP) {
        tcp_receive(packet, total, &from);
    }
}

/*------------------------------------------------------------------------
 * TCP.
 *----------------------------------------------------------------------*/

static uint32_t get32(const uint8_t *at)
{
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16)
         | ((uint32_t)at[2] << 8)  | (uint32_t)at[3];
}

static void put32(uint8_t *at, uint32_t v)
{
    at[0] = (uint8_t)(v >> 24);
    at[1] = (uint8_t)(v >> 16);
    at[2] = (uint8_t)(v >> 8);
    at[3] = (uint8_t)v;
}

/*
 * Is `a` at or after `b`, in sequence space?
 *
 * Sequence numbers wrap at 2^32 and comparing them as plain integers is
 * wrong exactly once per four gigabytes - which is a bug that appears after
 * an hour of traffic and nowhere in a test. The subtraction is the standard
 * answer: the difference is what matters and it is correct across the wrap
 * as long as the two are within 2^31 of each other, which RFC 793 requires
 * anyway.
 */
static bool seq_ge(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

/*
 * TCP's checksum covers a header that is not on the wire.
 *
 * The pseudo-header - source, destination, protocol, length - is prepended
 * for the sum and then thrown away. It exists so that a segment delivered to
 * the wrong address fails its checksum rather than being accepted by
 * whoever received it, which is a property IP alone does not give.
 *
 * Computed here in two passes rather than by building a buffer with the
 * pseudo-header in front, because that buffer would be a copy of the whole
 * segment for the sake of twelve bytes.
 */
static uint16_t tcp_checksum(const struct net_addr *src,
                             const struct net_addr *dst,
                             const uint8_t *segment, unsigned length)
{
    uint32_t sum = 0;
    unsigned i;

    for (i = 0; i < 4; i += 2) {
        sum += (uint32_t)((src->byte[i] << 8) | src->byte[i + 1]);
        sum += (uint32_t)((dst->byte[i] << 8) | dst->byte[i + 1]);
    }

    sum += IP_PROTO_TCP;
    sum += length;

    for (i = 0; i + 1 < length; i += 2) {
        sum += get16(segment + i);
    }

    if ((length & 1u) != 0) {
        sum += (uint32_t)segment[length - 1] << 8;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }

    return (uint16_t)(~sum & 0xffffu);
}

static struct conn *conn_at(uint32_t handle)
{
    if (handle >= NET_CONN_MAX || net.conn[handle].state == ST_FREE) {
        return NULL;
    }

    return &net.conn[handle];
}

/*
 * How much this end is prepared to receive, which is the ring's free space.
 *
 * **Not a number this stack invented.** The window is a promise: whatever is
 * advertised, the far end may send, and anything that arrives with nowhere
 * to go is dropped and retransmitted - which looks exactly like a lossy
 * network. So it is computed from the one thing that decides it, and a
 * client that stops reading really does slow the sender down. That is flow
 * control, and it is free when the window is the ring.
 */
static uint16_t window_of(const struct conn *c)
{
    uint32_t space;

    if (c->ring == NULL) {
        return 0;
    }

    space = tcp_ring_space(c->ring->bytes, c->ring->in_write,
                           tcp_ring_acquire(&c->ring->in_read));

    return (space > 65535u) ? 65535u : (uint16_t)space;
}

/*
 * One segment out. `data` may be NULL for a pure ACK, a SYN or a FIN.
 *
 * The maximum segment size goes on the SYN and nowhere else, which is what
 * RFC 793 says and also the only option this stack sends. Without it a far
 * end assumes 536 bytes, which works and wastes most of every frame.
 */
static bool tcp_send(struct conn *c, uint32_t flags, uint32_t seq,
                     const uint8_t *data, unsigned length)
{
    uint8_t segment[TCP_HEADER + 4 + 1460];
    unsigned header = TCP_HEADER;

    if (length > sizeof(segment) - TCP_HEADER - 4) {
        return false;
    }

    memset(segment, 0, TCP_HEADER + 4);

    put16(segment + TCP_SPORT, c->local_port);
    put16(segment + TCP_DPORT, c->remote_port);
    put32(segment + TCP_SEQ, seq);
    put32(segment + TCP_ACK, c->rcv_nxt);
    segment[TCP_FLAGS] = (uint8_t)flags;
    put16(segment + TCP_WINDOW, window_of(c));

    if ((flags & TCP_SYN) != 0) {
        /* Kind 2, length 4, then the value: 1460, which is the MTU less an
         * IP header and a TCP header. */
        segment[TCP_HEADER + 0] = 2;
        segment[TCP_HEADER + 1] = 4;
        put16(segment + TCP_HEADER + 2, 1460);
        header = TCP_HEADER + 4;
    }

    /* The data offset is in 32-bit words, in the high nibble. A stack that
     * writes the byte count here builds a segment whose payload starts
     * eighty bytes in, which the far end reads as garbage. */
    segment[TCP_OFF] = (uint8_t)((header / 4u) << 4);

    if (data != NULL && length > 0) {
        memcpy(segment + header, data, length);
    }

    put16(segment + TCP_CHECKSUM, 0);
    put16(segment + TCP_CHECKSUM,
          tcp_checksum(&net.address, &c->remote, segment, header + length));

    return send_ip(&c->remote, IP_PROTO_TCP, segment, header + length);
}

/*
 * Whatever is waiting in `out`, up to what the far end will take.
 *
 * Nothing is sent while a segment is unacknowledged: this is the one-in-
 * flight rule the structure comment describes, and it is what makes a single
 * retransmission timer sufficient.
 */
static void tcp_pump(struct conn *c)
{
    uint32_t have;
    uint32_t read;
    uint32_t send;
    unsigned i;
    uint8_t  piece[1460];

    if (c->ring == NULL || (c->state != ST_OPEN && c->state != ST_CLOSE_WAIT)) {
        return;
    }

    if (c->snd_nxt != c->snd_una) {
        return;                     /* something is already in flight */
    }

    read = c->ring->out_read;
    have = tcp_ring_ready(tcp_ring_acquire(&c->ring->out_write), read);

    if (have == 0) {
        return;
    }

    send = have;

    if (send > sizeof(piece)) {
        send = sizeof(piece);
    }

    if (send > c->peer_window) {
        send = c->peer_window;
    }

    if (send == 0) {
        return;                     /* the far end has nowhere to put it */
    }

    for (i = 0; i < send; i++) {
        piece[i] = tcp_ring_out(c->ring)[(read + i) % c->ring->bytes];
    }

    if (!tcp_send(c, TCP_ACK_FLAG | TCP_PSH, c->snd_nxt, piece, send)) {
        return;
    }

    c->snd_nxt += send;
    c->sent_at = kosmos_ticks();
    c->tries = 0;

    /*
     * The bytes stay in the ring until they are acknowledged, which is what
     * makes retransmission possible without a second buffer: `out_read` only
     * advances when the far end says it has them.
     */
}

/* Whoever is parked in WAIT on this connection is told something happened.
 * The reply carries no data - the data is in the ring - only the state, so
 * a client that was woken by a close knows to stop. */
static void wake_waiter(struct conn *c)
{
    struct net_reply reply;

    if (c->waiter == 0) {
        return;
    }

    memset(&reply, 0, sizeof(reply));
    reply.status = NET_OK;
    reply.handle = (uint32_t)(c - net.conn);
    reply.state  = (c->state == ST_OPEN || c->state == ST_FIN_WAIT)
                   ? NET_TCP_OPEN : NET_TCP_CLOSED;

    answer(c->waiter, &reply);
    c->waiter = 0;
}

static void conn_die(struct conn *c, uint32_t status)
{
    if (c->opened_by != 0) {
        fail(c->opened_by, status);
        c->opened_by = 0;
    }

    if (c->ring != NULL) {
        tcp_ring_publish(&c->ring->closed, 1);
    }

    c->state = ST_DEAD;
    wake_waiter(c);
}

/*
 * A segment arrived.
 *
 * The order of the checks is RFC 793's and matters: a reset is honoured
 * before anything else, a SYN-ACK only means something in SYN_SENT, and data
 * is only taken when it is the byte expected next. Out-of-order segments are
 * *dropped* rather than queued, which costs a retransmission and saves a
 * reassembly buffer - the same trade as refusing IP fragments, and defensible
 * for the same reason.
 */
static void tcp_receive(const uint8_t *packet, unsigned total,
                        const struct net_addr *from)
{
    const uint8_t *tcp = packet + IP_HEADER;
    unsigned tcp_len = total - IP_HEADER;
    unsigned header;
    unsigned payload;
    uint32_t flags;
    uint32_t seq, ack;
    struct conn *c = NULL;
    unsigned i;

    if (tcp_len < TCP_HEADER) {
        return;
    }

    header = (unsigned)(tcp[TCP_OFF] >> 4) * 4u;

    if (header < TCP_HEADER || header > tcp_len) {
        return;
    }

    for (i = 0; i < NET_CONN_MAX; i++) {
        struct conn *k = &net.conn[i];

        if (k->state != ST_FREE && k->state != ST_DEAD
            && k->local_port == get16(tcp + TCP_DPORT)
            && k->remote_port == get16(tcp + TCP_SPORT)
            && same_addr(&k->remote, from)) {
            c = k;
            break;
        }
    }

    if (c == NULL) {
        return;                     /* nobody's; a real stack would RST */
    }

    flags   = tcp[TCP_FLAGS];
    seq     = get32(tcp + TCP_SEQ);
    ack     = get32(tcp + TCP_ACK);
    payload = tcp_len - header;

    if ((flags & TCP_RST) != 0) {
        conn_die(c, (c->state == ST_SYN_SENT) ? NET_ERR_REFUSED
                                              : NET_ERR_CLOSED);
        return;
    }

    if (c->state == ST_SYN_SENT) {
        struct net_reply reply;

        if ((flags & (TCP_SYN | TCP_ACK_FLAG)) != (TCP_SYN | TCP_ACK_FLAG)) {
            return;                 /* not the answer to our question */
        }

        if (ack != c->snd_nxt) {
            return;                 /* acknowledging something we never sent */
        }

        /* Their SYN takes one sequence number, so the next byte we expect is
         * one past it. Forgetting that is a connection that opens and then
         * acknowledges everything one short for ever. */
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->peer_window = get16(tcp + TCP_WINDOW);
        c->state = ST_OPEN;

        (void)tcp_send(c, TCP_ACK_FLAG, c->snd_nxt, NULL, 0);

        memset(&reply, 0, sizeof(reply));
        reply.status     = NET_OK;
        reply.handle     = (uint32_t)(c - net.conn);
        reply.ring_bytes = TCP_RING_BYTES;
        reply.state      = NET_TCP_OPEN;

        {
            struct message msg;

            memset(&msg, 0, sizeof(msg));
            msg.length = sizeof(reply);
            msg.cap_plus_one = (uint32_t)c->region + 1u;
            memcpy(msg.data, &reply, sizeof(reply));

            (void)kosmos_reply(c->opened_by, &msg);
        }

        c->opened_by = 0;
        return;
    }

    if ((flags & TCP_ACK_FLAG) != 0 && seq_ge(ack, c->snd_una)
        && seq_ge(c->snd_nxt, ack)) {
        uint32_t acked = ack - c->snd_una;

        /*
         * The far end has these bytes, so they leave the ring. This is the
         * only place `out_read` moves, which is what makes the client the
         * sole writer of `out_write` and this the sole writer of
         * `out_read` - the discipline `tcpring.h` describes.
         */
        if (acked > 0 && c->ring != NULL) {
            uint32_t data_acked = acked;

            /* A FIN takes a sequence number and is not a byte in the ring. */
            if (c->fin_sent && seq_ge(ack, c->snd_nxt) && data_acked > 0) {
                data_acked--;
            }

            tcp_ring_publish(&c->ring->out_read,
                             c->ring->out_read + data_acked);
        }

        c->snd_una = ack;

        if (c->state == ST_LAST_ACK && seq_ge(ack, c->snd_nxt)) {
            conn_die(c, NET_OK);
            return;
        }
    }

    c->peer_window = get16(tcp + TCP_WINDOW);

    /*
     * Data, but only if it is the next byte expected.
     *
     * Anything else is dropped without acknowledging it, so the far end
     * sends it again. That is correct and slow; queuing it would be correct
     * and fast, and would be a reassembly buffer with a policy about
     * overlapping pieces - which is the well-known way to get a stack wrong.
     */
    if (payload > 0 && seq == c->rcv_nxt && c->ring != NULL) {
        uint32_t write = c->ring->in_write;
        uint32_t space = tcp_ring_space(c->ring->bytes, write,
                                        tcp_ring_acquire(&c->ring->in_read));
        unsigned take = (payload > space) ? space : payload;

        for (i = 0; i < take; i++) {
            tcp_ring_in(c->ring)[(write + i) % c->ring->bytes]
                = tcp[header + i];
        }

        tcp_ring_publish(&c->ring->in_write, write + take);
        c->rcv_nxt += take;

        (void)tcp_send(c, TCP_ACK_FLAG, c->snd_nxt, NULL, 0);
        wake_waiter(c);
    }

    if ((flags & TCP_FIN) != 0 && seq_ge(c->rcv_nxt, seq)) {
        /* Their FIN takes one sequence number too. */
        c->rcv_nxt = seq + payload + 1;

        if (c->ring != NULL) {
            tcp_ring_publish(&c->ring->closed, 1);
        }

        if (c->state == ST_OPEN) {
            c->state = ST_CLOSE_WAIT;
        } else if (c->state == ST_FIN_WAIT) {
            c->state = ST_LAST_ACK;
        }

        (void)tcp_send(c, TCP_ACK_FLAG, c->snd_nxt, NULL, 0);
        wake_waiter(c);
    }

    tcp_pump(c);
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

    case NET_OP_CONNECT: {
        struct conn *c = NULL;
        unsigned i;
        long region;
        long at;

        if (!net.has_card || !net.configured) {
            fail(sender, net.has_card ? NET_ERR_NO_ROUTE : NET_ERR_NO_CARD);
            return;
        }

        for (i = 0; i < NET_CONN_MAX; i++) {
            if (net.conn[i].state == ST_FREE) {
                c = &net.conn[i];
                break;
            }
        }

        if (c == NULL) {
            fail(sender, NET_ERR_FULL);
            return;
        }

        /*
         * The region first, because a connection with nowhere to put bytes
         * is a connection that would have to be torn down after the far end
         * had already answered.
         */
        region = kosmos_mem_create((TCP_RING_REGION + 4095u) / 4096u);

        if (region < 0) {
            fail(sender, NET_ERR_FULL);
            return;
        }

        at = kosmos_mem_map(region);

        if (at < 0) {
            (void)kosmos_cap_drop(region);
            fail(sender, NET_ERR_FULL);
            return;
        }

        memset(c, 0, sizeof(*c));

        c->region = region;
        c->ring   = (struct tcp_ring *)(uintptr_t)at;

        memset(c->ring, 0, TCP_RING_DATA);
        c->ring->magic = TCP_RING_MAGIC;
        c->ring->bytes = TCP_RING_BYTES;

        /*
         * A port nobody else is using, from the range IANA leaves to
         * whoever is asking. Counted rather than random, which a real stack
         * would not do - a predictable port is one an off-path attacker can
         * guess - and there is no source of randomness here to do better
         * with. Worth naming rather than leaving as an unexamined choice.
         */
        c->local_port  = net.next_port++;

        if (net.next_port < 49152u) {
            net.next_port = 49152u;
        }

        c->remote      = req.to;
        c->remote_port = (uint16_t)req.port;
        c->snd_una     = net.next_seq;
        c->snd_nxt     = net.next_seq;
        c->peer_window = 1460;
        c->state       = ST_SYN_SENT;
        c->opened_by   = sender;
        c->sent_at     = kosmos_ticks();

        net.next_seq += 0x01000000u;

        if (!tcp_send(c, TCP_SYN, c->snd_nxt, NULL, 0)) {
            (void)kosmos_cap_drop(region);
            memset(c, 0, sizeof(*c));
            fail(sender, NET_ERR_UNREACHABLE);
            return;
        }

        /* Our SYN takes one sequence number. */
        c->snd_nxt++;

        /*
         * And no reply. The caller stays parked until the far end answers,
         * is refused, or the timer gives up - which is what `connect` means
         * everywhere else, and what lets this process go on serving.
         */
        return;
    }

    case NET_OP_PUSH: {
        struct conn *c = conn_at(req.handle);

        if (c == NULL) {
            fail(sender, NET_ERR_NO_HANDLE);
            return;
        }

        tcp_pump(c);

        reply.status = NET_OK;
        reply.handle = req.handle;
        reply.state  = (c->state == ST_OPEN || c->state == ST_CLOSE_WAIT)
                       ? NET_TCP_OPEN : NET_TCP_CLOSED;
        answer(sender, &reply);
        return;
    }

    case NET_OP_WAIT: {
        struct conn *c = conn_at(req.handle);
        uint32_t ready;

        if (c == NULL) {
            fail(sender, NET_ERR_NO_HANDLE);
            return;
        }

        ready = tcp_ring_ready(c->ring->in_write,
                               tcp_ring_acquire(&c->ring->in_read));

        /*
         * Answered at once when there is something, parked when there is
         * not. A client that always got parked would take a round trip to
         * learn about bytes that had already arrived.
         */
        if (ready > 0 || c->state == ST_DEAD || c->ring->closed != 0) {
            reply.status = NET_OK;
            reply.handle = req.handle;
            reply.state  = (c->state == ST_OPEN || c->state == ST_FIN_WAIT)
                           ? NET_TCP_OPEN : NET_TCP_CLOSED;
            answer(sender, &reply);
            return;
        }

        c->waiter     = sender;
        c->wait_until = kosmos_ticks();
        return;
    }

    case NET_OP_CLOSE: {
        struct conn *c = conn_at(req.handle);

        if (c == NULL) {
            fail(sender, NET_ERR_NO_HANDLE);
            return;
        }

        /*
         * This end is done *sending*. The far end may still have things to
         * say, which is why this is a FIN rather than a teardown: a client
         * that has sent its request and closes should still read the answer.
         */
        if (c->state == ST_OPEN || c->state == ST_CLOSE_WAIT) {
            (void)tcp_send(c, TCP_ACK_FLAG | TCP_FIN, c->snd_nxt, NULL, 0);
            c->snd_nxt++;
            c->fin_sent = true;
            c->state = (c->state == ST_OPEN) ? ST_FIN_WAIT : ST_LAST_ACK;
        }

        reply.status = NET_OK;
        reply.handle = req.handle;
        answer(sender, &reply);
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

    /*
     * And the connections.
     *
     * **One timer, and it does three jobs**: it retransmits what has not
     * been acknowledged, gives up on a connection nobody answered, and
     * pushes anything the client has written that has not gone yet.
     *
     * The interval doubles - one second, two, four - which is the backoff
     * every stack does and for the reason every stack does it: a network
     * that lost the first copy is a network that is probably busy, and
     * retrying faster makes it busier. Five attempts and then the
     * connection is dead, which is about thirty seconds.
     */
    for (i = 0; i < NET_CONN_MAX; i++) {
        struct conn *c = &net.conn[i];
        uint64_t wait;

        if (c->state == ST_FREE || c->state == ST_DEAD) {
            continue;
        }

        /* Anything the client wrote while a segment was in flight. */
        tcp_pump(c);

        if (c->snd_una == c->snd_nxt && c->state != ST_SYN_SENT) {
            continue;               /* nothing outstanding */
        }

        wait = hz << (c->tries > 3 ? 3 : c->tries);

        if (now - c->sent_at < wait) {
            continue;
        }

        c->tries++;

        if (c->tries > 5) {
            conn_die(c, (c->state == ST_SYN_SENT) ? NET_ERR_TIMEOUT
                                                  : NET_ERR_CLOSED);
            continue;
        }

        c->sent_at = now;

        /*
         * Sent again from `snd_una`, not `snd_nxt`. That is the whole point
         * of keeping the bytes in the ring until they are acknowledged: the
         * retransmission is the same bytes at the same sequence number, and
         * a stack that resent from `snd_nxt` would be sending new data with
         * an old segment's number.
         */
        if (c->state == ST_SYN_SENT) {
            (void)tcp_send(c, TCP_SYN, c->snd_una, NULL, 0);
        } else {
            uint32_t behind = c->snd_nxt - c->snd_una;
            uint8_t  piece[1460];
            uint32_t read = c->ring->out_read;
            unsigned k;

            if (c->fin_sent && behind > 0) {
                behind--;           /* the FIN is not a byte in the ring */
            }

            if (behind > sizeof(piece)) {
                behind = sizeof(piece);
            }

            for (k = 0; k < behind; k++) {
                piece[k] = tcp_ring_out(c->ring)[(read + k) % c->ring->bytes];
            }

            (void)tcp_send(c, TCP_ACK_FLAG | (c->fin_sent ? TCP_FIN : 0u),
                           c->snd_una, behind ? piece : NULL, behind);
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
