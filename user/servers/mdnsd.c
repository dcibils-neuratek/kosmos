/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `mdnsd`: what is on the network, by name (`docs/sharing.md` N11c,
 * `user/include/mdnsproto.h`).
 *
 * Multicast DNS (RFC 6762) and DNS service discovery (RFC 6763): a
 * question to 224.0.0.251 port 5353 - "who offers `_smb._tcp.local`?",
 * "what is `studio.local`?" - and the answers anybody on the network sends
 * there, kept for as long as their time to live says. Started by init at
 * boot; holds the port and the group through the stack's datagrams
 * (`NET_OP_UDP_BIND`, N11a) once the machine has an address, and reads the
 * messages with `dnsmsg.c`, the stack's own reader.
 *
 * **Only what somebody here asked about is kept**: an instance of a type
 * being browsed, the host and port of an instance known, the address of a
 * host known or of a name being resolved. A record nobody asked for is
 * read and forgotten, so a stranger on the network cannot fill the tables.
 *
 * **It never waits on the network inside a request.** `BROWSE` and
 * `INSTANCE` answer at once from what is kept; `RESOLVE` parks its caller
 * and answers when the address comes or its wait ends, while this goes on
 * answering everybody else.
 *
 * **And it answers for this machine** (N13): what a server here publishes
 * (`PUBLISH` - `smbd`'s file sharing) is answered for when the network asks
 * - the service list, the type, the instance's SRV and TXT, what kind of
 * machine it is (`_device-info._tcp`), and the host's own address - and
 * announced twice, a second apart, when it is published or the address
 * changes (RFC 6762 8.3), and said gone with a time to live of 0 when it
 * is taken back (10.1). This machine's own are never listed among what was
 * seen, so a window browsing here does not find the machine it is on.
 * Not done: probing for a name another machine has (8.1); two machines
 * named alike answer alike.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "netproto.h"
#include "tcpring.h"
#include "mdnsproto.h"
#include "dnsmsg.h"
#include "init/say.h"

#define MDNS_PORT   5353u
#define BROWSE_FOR    10u           /* seconds a browse is kept asking after it was asked */

static const struct net_addr GROUP = { { 224, 0, 0, 251 } };

static long console = -1;
static long net = -1;
static uint64_t hz = 62500000u;     /* the counter, a second */
static uint64_t tick_hz = 250u;     /* the scheduler, a second */

/* The port: a handle and the region of its rings, once bound. */
static struct {
    bool bound;
    uint64_t handle;
    struct tcp_ring *ring;
    uint64_t next_try;
} port;

struct instance {
    char type[MDNS_NAME_MAX + 1];       /* `_smb._tcp` */
    char name[MDNS_INSTANCE_MAX + 1];   /* `Studio` */
    char host[MDNS_NAME_MAX + 1];       /* `studio.local`, once its SRV came */
    uint16_t port;
    bool has_srv;
    bool fresh;                         /* new in the message being read */
    bool asked_kind;                    /* its `_device-info._tcp` asked for */
    char model[MDNS_MODEL_MAX + 1];     /* what kind of machine: `Macmini9,1` */
    uint64_t until;
};

struct hostaddr {
    char host[MDNS_NAME_MAX + 1];
    struct net_addr address;
    uint64_t until;
};

struct browse {
    char type[MDNS_NAME_MAX + 1];
    uint64_t wanted_until;              /* asked again while now is before this */
    uint64_t next_ask;
    unsigned asks;
};

/*
 * A name asked for. **The question outlives the wait**: a caller that asked
 * with a short wait - a window asking on its clock, which may not wait in
 * a paint - is told "not yet" when its wait ends (`waited`), and the
 * question goes on being asked until `until`, so the answer is kept for its
 * next look rather than read and forgotten.
 */
#define ASKED_FOR     5u            /* seconds a name is asked for */

struct resolving {
    char name[MDNS_NAME_MAX + 1];
    uint64_t who;                   /* the caller still parked, or 0 */
    uint64_t waited;                /* when its wait ends */
    uint64_t until;                 /* when the question does */
    uint64_t next_ask;
    unsigned asks;
};

/* The tables, grown when full and never past what the machine gives. */
#define TABLE(T, name) static T *name; static unsigned name##_n, name##_cap
TABLE(struct instance, instances);
TABLE(struct hostaddr, hosts);
TABLE(struct browse, browses);
TABLE(struct resolving, resolvings);

/* What this machine offers (N13). */
struct published {
    char type[MDNS_NAME_MAX + 1];       /* `_smb._tcp` */
    char instance[MDNS_INSTANCE_MAX + 1];
    uint16_t port;
    unsigned announced;                 /* of ANNOUNCE_TIMES */
    uint64_t next_announce;
};

TABLE(struct published, publisheds);

/* This machine's address, asked of the stack now and then; and its host
 * name, `kosmos.local`, from the first instance published. */
static struct {
    bool known;
    struct net_addr address;
    uint64_t next_look;
} self;

static char host_name[MDNS_INSTANCE_MAX + 8];

static void *grow(void *list, unsigned *cap, size_t each)
{
    unsigned want = *cap ? *cap * 2u : 8u;
    void *more = realloc(list, (size_t)want * each);

    if (more != NULL) {
        *cap = want;
    }

    return more;
}

#define ROOM(name) (name##_n < name##_cap \
    || ((name = grow(name, &name##_cap, sizeof(*name))) != NULL && name##_n < name##_cap))

static void say_line(const char *text)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, text);
    say_send(console, &line);
}

static uint64_t now(void)
{
    return kosmos_ticks();
}

/* DNS names are the same whatever their letters' case (RFC 1035 2.3.3). */
static bool same_name(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = *a, y = *b;

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }

    return *a == *b;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* A service type as RFC 6763 7 writes one: `_name._tcp` or `_name._udp`. */
static bool service_type(const char *t)
{
    size_t n = strlen(t);

    if (n < 7 || n > 63 || t[0] != '_') {
        return false;
    }

    return strcmp(t + n - 5, "._tcp") == 0 || strcmp(t + n - 5, "._udp") == 0;
}

static bool local_name(const char *name)
{
    size_t n = strlen(name);

    return n > 6 && n <= MDNS_NAME_MAX && same_name(name + n - 6, ".local");
}

/*------------------------------------------------------------------------
 * The port, through the stack.
 *----------------------------------------------------------------------*/

static bool net_ask(struct net_request *req, struct net_reply *rep, long *cap)
{
    struct message msg, out;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*req);
    memcpy(msg.data, req, sizeof(*req));

    if (kosmos_call(net, &msg, &out) != 0 || out.length < sizeof(*rep)) {
        return false;
    }

    memcpy(rep, out.data, sizeof(*rep));

    if (cap != NULL) {
        *cap = out.cap_plus_one > 0 ? (long)out.cap_plus_one - 1 : -1;
    }

    return true;
}

/* Port 5353 and the group, once the machine has an address: tried at
 * start and every second after until it is. */
static void bind_port(void)
{
    struct net_request req;
    struct net_reply rep;
    long region = -1, at;

    if (port.bound || net < 0 || now() < port.next_try) {
        return;
    }

    port.next_try = now() + hz;
    memset(&req, 0, sizeof(req));
    req.op = NET_OP_UDP_BIND;
    req.port = MDNS_PORT;
    req.to = GROUP;

    if (!net_ask(&req, &rep, &region) || rep.status != NET_OK || region < 0) {
        if (region >= 0) {
            (void)kosmos_cap_drop(region);
        }

        return;
    }

    at = kosmos_mem_map(region);

    if (at < 0) {
        (void)kosmos_cap_drop(region);
        return;
    }

    port.bound = true;
    port.handle = rep.handle;
    port.ring = (struct tcp_ring *)(uintptr_t)at;
    say_line("mdnsd: listening on 224.0.0.251 port 5353");
}

/* A message, to the group. */
static void send_datagram(const uint8_t *m, unsigned n)
{
    struct net_datagram d;
    struct net_request req;
    struct net_reply rep;
    unsigned k;
    uint32_t write, space;

    if (!port.bound || n == 0) {
        return;
    }

    memset(&d, 0, sizeof(d));
    d.addr = GROUP;
    d.port = MDNS_PORT;
    d.length = (uint16_t)n;

    write = port.ring->out_write;
    space = tcp_ring_space(port.ring->bytes, write, tcp_ring_acquire(&port.ring->out_read));

    if (space < sizeof(d) + n) {
        return;
    }

    for (k = 0; k < sizeof(d); k++) {
        tcp_ring_out(port.ring)[(write + k) % port.ring->bytes] = ((uint8_t *)&d)[k];
    }

    for (k = 0; k < n; k++) {
        tcp_ring_out(port.ring)[(write + (uint32_t)sizeof(d) + k) % port.ring->bytes] = m[k];
    }

    tcp_ring_publish(&port.ring->out_write, write + (uint32_t)sizeof(d) + n);

    memset(&req, 0, sizeof(req));
    req.op = NET_OP_PUSH;
    req.handle = port.handle;
    (void)net_ask(&req, &rep, NULL);
}

/* One question, to the group. */
static void ask(const char *name, uint16_t type)
{
    uint8_t m[NET_DATAGRAM_MAX];
    unsigned n;

    if (!port.bound) {
        return;
    }

    memset(m, 0, DNS_HEADER);
    m[5] = 1;                           /* one question; id and flags 0 (RFC 6762 18) */
    n = dns_put_name(m + DNS_HEADER, sizeof(m) - DNS_HEADER - 4, name, (unsigned)strlen(name));

    if (n == 0) {
        return;
    }

    n += DNS_HEADER;
    m[n++] = (uint8_t)(type >> 8);
    m[n++] = (uint8_t)type;
    m[n++] = 0;
    m[n++] = DNS_CLASS_IN;

    send_datagram(m, n);
}

/*------------------------------------------------------------------------
 * What is kept.
 *----------------------------------------------------------------------*/

static void type_local(char *out, const char *type)
{
    size_t n = strlen(type);

    memcpy(out, type, n);
    memcpy(out + n, ".local", 7);
}

static struct browse *browse_of(const char *type)
{
    unsigned i;

    for (i = 0; i < browses_n; i++) {
        if (same_name(browses[i].type, type)) {
            return &browses[i];
        }
    }

    return NULL;
}

static struct instance *instance_of(const char *type, const char *name)
{
    unsigned i;

    for (i = 0; i < instances_n; i++) {
        if (same_name(instances[i].type, type) && same_name(instances[i].name, name)) {
            return &instances[i];
        }
    }

    return NULL;
}

static struct hostaddr *host_of(const char *host)
{
    unsigned i;

    for (i = 0; i < hosts_n; i++) {
        if (same_name(hosts[i].host, host) && now() < hosts[i].until) {
            return &hosts[i];
        }
    }

    return NULL;
}

/* Whether somebody here wants to know about `host`: an instance's host,
 * or a name being resolved. */
static bool host_wanted(const char *host)
{
    unsigned i;

    for (i = 0; i < instances_n; i++) {
        if (instances[i].has_srv && same_name(instances[i].host, host)) {
            return true;
        }
    }

    for (i = 0; i < resolvings_n; i++) {
        if (same_name(resolvings[i].name, host)) {
            return true;
        }
    }

    return false;
}

static void answer(uint64_t to, const struct mdns_reply *rep)
{
    struct message out;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rep);
    memcpy(out.data, rep, sizeof(*rep));
    (void)kosmos_reply(to, &out);
}

static void remove_at(void *list, unsigned *n, unsigned i, size_t each)
{
    uint8_t *base = list;

    memmove(base + (size_t)i * each, base + (size_t)(i + 1) * each, (size_t)(*n - i - 1) * each);
    (*n)--;
}

/* An address for `host`: kept, and given to whoever is waiting for it. */
static void host_known(const char *host, const uint8_t *a, uint32_t ttl)
{
    struct hostaddr *h = host_of(host);
    unsigned i = 0;

    if (h == NULL) {
        if (!ROOM(hosts)) {
            return;
        }

        h = &hosts[hosts_n++];
        memset(h, 0, sizeof(*h));
        strncpy(h->host, host, MDNS_NAME_MAX);
    }

    memcpy(h->address.byte, a, 4);
    h->until = now() + (uint64_t)(ttl ? ttl : 1) * hz;

    while (i < resolvings_n) {
        if (same_name(resolvings[i].name, host)) {
            struct mdns_reply rep;

            memset(&rep, 0, sizeof(rep));
            rep.error = MDNS_OK;
            rep.has_address = 1;
            memcpy(rep.address.byte, a, 4);
            strncpy(rep.host, host, MDNS_NAME_MAX);

            if (resolvings[i].who != 0) {
                answer(resolvings[i].who, &rep);
            }

            remove_at(resolvings, &resolvings_n, i, sizeof(*resolvings));
        } else {
            i++;
        }
    }
}

/* What an instance still lacks, asked for: its SRV, then its host's A -
 * and once, what kind of machine it is (its `_device-info._tcp` TXT, which
 * a Mac answers for though nobody browses it). */
static void follow_up(struct instance *in)
{
    if (!in->asked_kind) {
        char kind[MDNS_INSTANCE_MAX + 32];

        in->asked_kind = true;
        strcpy(kind, in->name);
        strcat(kind, "._device-info._tcp.local");
        ask(kind, DNS_TYPE_TXT);
    }

    if (!in->has_srv) {
        char full[MDNS_NAME_MAX * 2 + 8];

        strcpy(full, in->name);
        strcat(full, ".");
        strcat(full, in->type);
        strcat(full, ".local");
        ask(full, DNS_TYPE_SRV);
    } else if (host_of(in->host) == NULL) {
        ask(in->host, DNS_TYPE_A);
    }
}

/*------------------------------------------------------------------------
 * What this machine offers (N13): answered for, and announced.
 *----------------------------------------------------------------------*/

#define TTL_HOST        120u        /* an address and an SRV: RFC 6762 10 */
#define TTL_OTHER      4500u        /* a PTR and a TXT */
#define FLUSH        0x8000u        /* the cache-flush bit: this record is all of it */
#define ANNOUNCE_TIMES    2u        /* RFC 6762 8.3: at least two, a second apart */
#define SERVICES "_services._dns-sd._udp.local"

static struct published *published_of(const char *type, const char *instance)
{
    unsigned i;

    for (i = 0; i < publisheds_n; i++) {
        if (same_name(publisheds[i].type, type) && same_name(publisheds[i].instance, instance)) {
            return &publisheds[i];
        }
    }

    return NULL;
}

/* `Kosmos M700` -> `kosmos-m700.local`: letters and digits kept, lower
 * case, anything else a hyphen, none at either end. */
static void host_from(const char *instance)
{
    unsigned n = 0, i;

    for (i = 0; instance[i] != '\0' && n < MDNS_INSTANCE_MAX; i++) {
        char c = instance[i];

        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            host_name[n++] = c;
        } else if (n > 0 && host_name[n - 1] != '-') {
            host_name[n++] = '-';
        }
    }

    while (n > 0 && host_name[n - 1] == '-') n--;

    if (n == 0) {
        memcpy(host_name, "kosmos", 6);
        n = 6;
    }

    memcpy(host_name + n, ".local", 7);
}

/* This machine's address, asked of the stack every five seconds: a new
 * one - a lease from another router - is announced at once. */
static void learn_self(void)
{
    struct net_request req;
    struct net_reply rep;
    unsigned i;

    if (net < 0 || now() < self.next_look) {
        return;
    }

    self.next_look = now() + 5u * hz;
    memset(&req, 0, sizeof(req));
    req.op = NET_OP_INFO;

    if (!net_ask(&req, &rep, NULL) || rep.status != NET_OK
        || (rep.address.byte[0] | rep.address.byte[1] | rep.address.byte[2]
            | rep.address.byte[3]) == 0) {
        return;
    }

    if (self.known && memcmp(&self.address, &rep.address, sizeof(rep.address)) == 0) {
        return;
    }

    self.known = true;
    self.address = rep.address;

    for (i = 0; i < publisheds_n; i++) {
        publisheds[i].announced = 0;
        publisheds[i].next_announce = now();
    }
}

/* An answer being made: every record in the answer section, as RFC 6762
 * 6 allows, each of this machine's at most once. */
struct out_msg {
    uint8_t m[NET_DATAGRAM_MAX];
    unsigned n, count;
    bool host, services;
};

static void out_begin(struct out_msg *o)
{
    memset(o->m, 0, DNS_HEADER);
    o->m[2] = 0x84;                     /* a response, authoritative (RFC 6762 18) */
    o->n = DNS_HEADER;
    o->count = 0;
    o->host = o->services = false;
}

static void out_add(struct out_msg *o, const char *name, uint16_t type, uint16_t class,
                    uint32_t ttl, const uint8_t *data, unsigned len)
{
    unsigned w = dns_put_record(o->m + o->n, sizeof(o->m) - o->n, name, type, class, ttl,
                                data, len);

    if (w != 0) {
        o->n += w;
        o->count++;
    }
}

static void out_send(struct out_msg *o)
{
    if (o->count == 0) {
        return;
    }

    o->m[6] = (uint8_t)(o->count >> 8);
    o->m[7] = (uint8_t)o->count;
    send_datagram(o->m, o->n);
}

static void full_of(char *out, const struct published *p, const char *type)
{
    strcpy(out, p->instance);
    strcat(out, ".");
    strcat(out, type);
    strcat(out, ".local");
}

static void add_host(struct out_msg *o, uint32_t ttl)
{
    if (!self.known || host_name[0] == '\0' || o->host) {
        return;
    }

    o->host = true;
    out_add(o, host_name, DNS_TYPE_A, FLUSH | DNS_CLASS_IN, ttl, self.address.byte, 4);
}

static void add_srv(struct out_msg *o, const struct published *p, uint32_t ttl)
{
    char full[MDNS_NAME_MAX * 2 + 8];
    uint8_t data[8 + DNS_NAME_MAX];
    unsigned n;

    full_of(full, p, p->type);
    memset(data, 0, 4);                 /* priority, weight */
    data[4] = (uint8_t)(p->port >> 8);
    data[5] = (uint8_t)p->port;
    n = dns_put_name(data + 6, sizeof(data) - 6, host_name, (unsigned)strlen(host_name));

    if (n != 0) {
        out_add(o, full, DNS_TYPE_SRV, FLUSH | DNS_CLASS_IN, ttl, data, 6 + n);
    }
}

static void add_txt(struct out_msg *o, const struct published *p, uint32_t ttl)
{
    static const uint8_t empty[1] = { 0 };     /* RFC 6763 6.1: one empty string */
    char full[MDNS_NAME_MAX * 2 + 8];

    full_of(full, p, p->type);
    out_add(o, full, DNS_TYPE_TXT, FLUSH | DNS_CLASS_IN, ttl, empty, 1);
}

static void add_ptr(struct out_msg *o, const struct published *p, uint32_t ttl)
{
    char type[MDNS_NAME_MAX + 8], full[MDNS_NAME_MAX * 2 + 8];
    uint8_t data[DNS_NAME_MAX + 1];
    unsigned n;

    type_local(type, p->type);
    full_of(full, p, p->type);
    n = dns_put_name(data, sizeof(data), full, (unsigned)strlen(full));

    if (n != 0) {
        out_add(o, type, DNS_TYPE_PTR, DNS_CLASS_IN, ttl, data, n);
    }
}

/* What kind of machine this is, which Finder draws by (`_device-info._tcp`,
 * a TXT `model=`): Kosmos, which it does not know, so a computer. */
static void add_device(struct out_msg *o, const struct published *p, uint32_t ttl)
{
    static const char model[] = "model=Kosmos";
    char full[MDNS_NAME_MAX * 2 + 8];
    uint8_t data[sizeof(model)];

    full_of(full, p, "_device-info._tcp");
    data[0] = (uint8_t)(sizeof(model) - 1);
    memcpy(data + 1, model, sizeof(model) - 1);
    out_add(o, full, DNS_TYPE_TXT, FLUSH | DNS_CLASS_IN, ttl, data, sizeof(model));
}

static void add_service(struct out_msg *o, const struct published *p, uint32_t ttl)
{
    char type[MDNS_NAME_MAX + 8];
    uint8_t data[DNS_NAME_MAX + 1];
    unsigned n;

    type_local(type, p->type);
    n = dns_put_name(data, sizeof(data), type, (unsigned)strlen(type));

    if (n != 0) {
        out_add(o, SERVICES, DNS_TYPE_PTR, DNS_CLASS_IN, ttl, data, n);
    }
}

/* Everything of `p`, at `ttl` - 0 for a goodbye. */
static void add_all(struct out_msg *o, const struct published *p, bool goodbye)
{
    add_ptr(o, p, goodbye ? 0 : TTL_OTHER);
    add_srv(o, p, goodbye ? 0 : TTL_HOST);
    add_txt(o, p, goodbye ? 0 : TTL_OTHER);
    add_device(o, p, goodbye ? 0 : TTL_OTHER);
    add_service(o, p, goodbye ? 0 : TTL_OTHER);

    if (!goodbye) {
        add_host(o, TTL_HOST);
    }
}

/* A question from the network about what is this machine's: answered to
 * the group, everything asked for in one message. */
static void respond(const uint8_t *m, unsigned len, const struct dns_header *h)
{
    static struct out_msg o;
    unsigned at = DNS_HEADER, i, k;

    if (publisheds_n == 0 || !self.known) {
        return;
    }

    out_begin(&o);

    for (i = 0; i < h->questions; i++) {
        char name[MDNS_NAME_MAX + 1];
        uint16_t type, class;
        bool any;

        if (!dns_read_question(m, len, &at, name, sizeof(name), &type, &class)) {
            break;
        }

        any = type == DNS_TYPE_ANY;

        for (k = 0; k < publisheds_n; k++) {
            const struct published *p = &publisheds[k];
            char tl[MDNS_NAME_MAX + 8], full[MDNS_NAME_MAX * 2 + 8];

            type_local(tl, p->type);

            if ((type == DNS_TYPE_PTR || any) && same_name(name, SERVICES)) {
                add_service(&o, p, TTL_OTHER);
            }

            if ((type == DNS_TYPE_PTR || any) && same_name(name, tl)) {
                add_ptr(&o, p, TTL_OTHER);
                add_srv(&o, p, TTL_HOST);
                add_txt(&o, p, TTL_OTHER);
                add_host(&o, TTL_HOST);
            }

            full_of(full, p, p->type);

            if (same_name(name, full)) {
                if (type == DNS_TYPE_SRV || any) add_srv(&o, p, TTL_HOST);
                if (type == DNS_TYPE_TXT || any) add_txt(&o, p, TTL_OTHER);
                add_host(&o, TTL_HOST);
            }

            full_of(full, p, "_device-info._tcp");

            if ((type == DNS_TYPE_TXT || any) && same_name(name, full)) {
                add_device(&o, p, TTL_OTHER);
            }
        }

        if ((type == DNS_TYPE_A || any) && host_name[0] != '\0' && same_name(name, host_name)) {
            add_host(&o, TTL_HOST);
        }
    }

    out_send(&o);
}

/* Announcements due, sent: each published thing twice, a second apart. */
static void announce(void)
{
    static struct out_msg o;
    unsigned i;

    if (!port.bound || !self.known) {
        return;
    }

    for (i = 0; i < publisheds_n; i++) {
        struct published *p = &publisheds[i];

        if (p->announced >= ANNOUNCE_TIMES || now() < p->next_announce) {
            continue;
        }

        out_begin(&o);
        add_all(&o, p, false);
        out_send(&o);
        p->announced++;
        p->next_announce = now() + hz;

        if (p->announced == 1) {
            say_line("mdnsd: announcing this machine's file sharing");
        }
    }
}

/*------------------------------------------------------------------------
 * What the network said.
 *----------------------------------------------------------------------*/

/* `addresses`: the second reading of a message, for its A records - after
 * its PTRs and SRVs, so an address is taken whatever order they came in. */
static void record(const uint8_t *m, unsigned len, const struct dns_record *r, bool addresses)
{
    if (addresses != (r->type == DNS_TYPE_A)) {
        return;
    }

    unsigned i;

    if (r->type == DNS_TYPE_PTR) {
        /* `_smb._tcp.local` -> `Studio._smb._tcp.local`: an instance of a
         * type somebody browses. */
        for (i = 0; i < browses_n; i++) {
            char full[MDNS_NAME_MAX + 8], target[MDNS_NAME_MAX + 1];
            size_t tl, fl;
            struct instance *in;

            type_local(full, browses[i].type);

            if (!same_name(r->name, full)
                || !dns_read_name(m, len, r->data, target, sizeof(target), NULL)) {
                continue;
            }

            tl = strlen(target);
            fl = strlen(full);

            if (tl <= fl + 1 || target[tl - fl - 1] != '.' || !same_name(target + tl - fl, full)
                || tl - fl - 1 > MDNS_INSTANCE_MAX) {
                continue;
            }

            target[tl - fl - 1] = '\0';

            /* This machine's own, heard back: not something seen. */
            if (published_of(browses[i].type, target) != NULL) {
                continue;
            }

            in = instance_of(browses[i].type, target);

            if (r->ttl == 0) {                   /* a goodbye: RFC 6762 10.1 */
                if (in != NULL) {
                    remove_at(instances, &instances_n, (unsigned)(in - instances),
                              sizeof(*instances));
                }

                continue;
            }

            if (in == NULL) {
                if (!ROOM(instances)) {
                    continue;
                }

                in = &instances[instances_n++];
                memset(in, 0, sizeof(*in));
                strncpy(in->type, browses[i].type, MDNS_NAME_MAX);
                strncpy(in->name, target, MDNS_INSTANCE_MAX);
                in->fresh = true;       /* followed up after the message, if it lacks */
            }

            in->until = now() + (uint64_t)r->ttl * hz;
        }
    } else if (r->type == DNS_TYPE_SRV && r->length >= 7) {
        /* `Studio._smb._tcp.local` -> port and host, for an instance kept. */
        for (i = 0; i < instances_n; i++) {
            char full[MDNS_NAME_MAX * 2 + 8];
            struct instance *in = &instances[i];

            strcpy(full, in->name);
            strcat(full, ".");
            strcat(full, in->type);
            strcat(full, ".local");

            if (same_name(r->name, full)
                && dns_read_name(m, len, r->data + 6, in->host, sizeof(in->host), NULL)) {
                in->port = be16(m + r->data + 4);
                in->has_srv = true;
            }
        }
    } else if (r->type == DNS_TYPE_A && r->length == 4 && host_wanted(r->name)) {
        host_known(r->name, m + r->data, r->ttl);
    } else if (r->type == DNS_TYPE_TXT) {
        /* `Studio._device-info._tcp.local`: `model=` for every instance kept
         * under that name - strings, each its length and its bytes. */
        static const char suffix[] = "._device-info._tcp.local";
        size_t nl = strlen(r->name), sl = sizeof(suffix) - 1;

        if (nl > sl && same_name(r->name + nl - sl, suffix) && nl - sl <= MDNS_INSTANCE_MAX) {
            char who[MDNS_INSTANCE_MAX + 1];
            unsigned at = 0;

            memcpy(who, r->name, nl - sl);
            who[nl - sl] = '\0';

            while (at < r->length) {
                unsigned n = m[r->data + at];
                const char *text = (const char *)m + r->data + at + 1;

                if (at + 1 + n > r->length) {
                    break;
                }

                if (n > 6 && n - 6 <= MDNS_MODEL_MAX && strncmp(text, "model=", 6) == 0) {
                    for (i = 0; i < instances_n; i++) {
                        if (same_name(instances[i].name, who)) {
                            memcpy(instances[i].model, text + 6, n - 6);
                            instances[i].model[n - 6] = '\0';
                        }
                    }
                }

                at += 1 + n;
            }
        }
    }
}

static void message(const uint8_t *m, unsigned len)
{
    struct dns_header h;
    unsigned at = DNS_HEADER, i, records;

    if (!dns_read_header(m, len, &h)) {
        return;
    }

    if ((h.flags & 0x8000u) == 0) {
        respond(m, len, &h);            /* a question: this machine's, answered (N13) */
        return;
    }

    for (i = 0; i < h.questions; i++) {
        char name[MDNS_NAME_MAX + 1];
        uint16_t type, class;

        if (!dns_read_question(m, len, &at, name, sizeof(name), &type, &class)) {
            return;
        }
    }

    records = (unsigned)h.answers + h.authorities + h.additionals;

    {
        unsigned first = at, pass;

        for (pass = 0; pass < 2; pass++) {
            at = first;

            for (i = 0; i < records; i++) {
                struct dns_record r;

                if (!dns_read_record(m, len, &at, &r)) {
                    break;
                }

                record(m, len, &r, pass == 1);
            }
        }
    }

    /* What a new instance still lacks after all of it, asked for. */
    for (i = 0; i < instances_n; i++) {
        if (instances[i].fresh) {
            instances[i].fresh = false;
            follow_up(&instances[i]);
        }
    }
}

/* Every datagram that has come, read. */
static void drain(void)
{
    uint8_t m[NET_DATAGRAM_MAX];

    while (port.bound) {
        uint32_t read = port.ring->in_read;
        uint32_t ready = tcp_ring_ready(tcp_ring_acquire(&port.ring->in_write), read);
        struct net_datagram d;
        uint32_t k;

        if (ready < sizeof(d)) {
            return;
        }

        for (k = 0; k < sizeof(d); k++) {
            ((uint8_t *)&d)[k] = tcp_ring_in(port.ring)[(read + k) % port.ring->bytes];
        }

        if (d.length > sizeof(m) || sizeof(d) + d.length > ready) {
            tcp_ring_publish(&port.ring->in_read, read + ready);    /* not a datagram: let go */
            return;
        }

        for (k = 0; k < d.length; k++) {
            m[k] = tcp_ring_in(port.ring)[(read + (uint32_t)sizeof(d) + k) % port.ring->bytes];
        }

        tcp_ring_publish(&port.ring->in_read, read + (uint32_t)sizeof(d) + d.length);
        message(m, d.length);
    }
}

/*------------------------------------------------------------------------
 * Asking again, and forgetting.
 *----------------------------------------------------------------------*/

/* RFC 6762 5.2: a question asked at once, then after one, two and four
 * seconds, then every minute while it is still wanted. */
static uint64_t next_after(unsigned asks)
{
    return now() + (asks < 4 ? ((uint64_t)1 << (asks - 1)) : 60u) * hz;
}

static void tick(void)
{
    uint64_t t = now();
    unsigned i = 0;

    bind_port();
    learn_self();
    announce();

    for (i = 0; i < browses_n; i++) {
        struct browse *b = &browses[i];
        char full[MDNS_NAME_MAX + 8];

        if (t >= b->wanted_until || t < b->next_ask) {
            continue;
        }

        type_local(full, b->type);
        ask(full, DNS_TYPE_PTR);
        b->asks++;
        b->next_ask = next_after(b->asks);

        /* And with each round, what its instances still lack. */
        {
            unsigned k;

            for (k = 0; k < instances_n; k++) {
                if (same_name(instances[k].type, b->type)) {
                    follow_up(&instances[k]);
                }
            }
        }
    }

    i = 0;

    while (i < resolvings_n) {
        struct resolving *r = &resolvings[i];

        if (r->who != 0 && t >= r->waited) {
            struct mdns_reply rep;

            memset(&rep, 0, sizeof(rep));
            rep.error = MDNS_ERR_TIMEOUT;
            answer(r->who, &rep);
            r->who = 0;                 /* told; the question goes on */
        }

        if (t >= r->until) {
            remove_at(resolvings, &resolvings_n, i, sizeof(*resolvings));
            continue;
        }

        if (t >= r->next_ask) {
            ask(r->name, DNS_TYPE_A);
            r->asks++;
            r->next_ask = next_after(r->asks);
        }

        i++;
    }

    /* What the network said only for a while, forgotten when it is over. */
    i = 0;

    while (i < instances_n) {
        if (t >= instances[i].until) {
            remove_at(instances, &instances_n, i, sizeof(*instances));
        } else {
            i++;
        }
    }

    i = 0;

    while (i < hosts_n) {
        if (t >= hosts[i].until) {
            remove_at(hosts, &hosts_n, i, sizeof(*hosts));
        } else {
            i++;
        }
    }
}

static bool busy(void)
{
    unsigned i;

    if (resolvings_n > 0 || !port.bound) {
        return true;
    }

    for (i = 0; i < publisheds_n; i++) {
        if (publisheds[i].announced < ANNOUNCE_TIMES) {
            return true;
        }
    }

    for (i = 0; i < browses_n; i++) {
        if (now() < browses[i].wanted_until) {
            return true;
        }
    }

    return false;
}

/*------------------------------------------------------------------------
 * The door.
 *----------------------------------------------------------------------*/

static void serve(const struct message *msg, uint64_t sender)
{
    struct mdns_request req;
    struct mdns_reply rep;

    memset(&rep, 0, sizeof(rep));

    if (msg->length < sizeof(req)) {
        rep.error = MDNS_ERR_BAD_OP;
        answer(sender, &rep);
        return;
    }

    memcpy(&req, msg->data, sizeof(req));
    req.name[MDNS_NAME_MAX] = '\0';
    req.instance[MDNS_INSTANCE_MAX] = '\0';

    switch (req.op) {
    case MDNS_OP_PUBLISH:
    case MDNS_OP_UNPUBLISH: {
        struct published *p;

        /* An instance is one label (RFC 6763 4.1.1): no dot in it, as the
         * name writer would split it. */
        if (!service_type(req.name) || req.instance[0] == '\0' || strchr(req.instance, '.')
            || (req.op == MDNS_OP_PUBLISH && (req.index == 0 || req.index > 0xffffu))) {
            rep.error = MDNS_ERR_BAD_NAME;
            break;
        }

        p = published_of(req.name, req.instance);

        if (req.op == MDNS_OP_UNPUBLISH) {
            if (p != NULL) {
                static struct out_msg o;

                out_begin(&o);
                add_all(&o, p, true);           /* a goodbye: RFC 6762 10.1 */
                out_send(&o);
                remove_at(publisheds, &publisheds_n, (unsigned)(p - publisheds),
                          sizeof(*publisheds));
                say_line("mdnsd: no longer announcing it");
            }

            if (publisheds_n == 0) {
                host_name[0] = '\0';
            }

            break;
        }

        if (p == NULL) {
            if (!ROOM(publisheds)) {
                rep.error = MDNS_ERR_FULL;
                break;
            }

            p = &publisheds[publisheds_n++];
            memset(p, 0, sizeof(*p));
            strncpy(p->type, req.name, MDNS_NAME_MAX);
            strncpy(p->instance, req.instance, MDNS_INSTANCE_MAX);
        }

        if (host_name[0] == '\0') {
            host_from(req.instance);
        }

        p->port = (uint16_t)req.index;
        p->announced = 0;
        p->next_announce = now();
        self.next_look = 0;                     /* its address, asked now */
        tick();
        break;
    }

    case MDNS_OP_BROWSE: {
        struct browse *b;
        unsigned i;

        if (!service_type(req.name)) {
            rep.error = MDNS_ERR_BAD_NAME;
            break;
        }

        if (!port.bound) {
            rep.error = MDNS_ERR_NO_NETWORK;
            break;
        }

        b = browse_of(req.name);

        if (b == NULL) {
            if (!ROOM(browses)) {
                rep.error = MDNS_ERR_FULL;
                break;
            }

            b = &browses[browses_n++];
            memset(b, 0, sizeof(*b));
            strncpy(b->type, req.name, MDNS_NAME_MAX);
        }

        /* A browse not asked about lately starts its questions again. */
        if (now() >= b->wanted_until) {
            b->asks = 0;
            b->next_ask = now();
        }

        b->wanted_until = now() + BROWSE_FOR * hz;

        for (i = 0; i < instances_n; i++) {
            rep.count += same_name(instances[i].type, req.name) ? 1u : 0u;
        }

        break;
    }

    case MDNS_OP_INSTANCE: {
        unsigned i, seen = 0;

        rep.error = MDNS_ERR_NO_SUCH;

        for (i = 0; i < instances_n; i++) {
            const struct instance *in = &instances[i];
            const struct hostaddr *h;

            if (!same_name(in->type, req.name) || seen++ != req.index) {
                continue;
            }

            rep.error = MDNS_OK;
            strncpy(rep.instance, in->name, MDNS_INSTANCE_MAX);
            strncpy(rep.model, in->model, MDNS_MODEL_MAX);

            if (in->has_srv) {
                strncpy(rep.host, in->host, MDNS_NAME_MAX);
                rep.port = in->port;
                h = host_of(in->host);

                if (h != NULL) {
                    rep.has_address = 1;
                    rep.address = h->address;
                }
            }

            break;
        }

        break;
    }

    case MDNS_OP_RESOLVE: {
        const struct hostaddr *h;
        struct resolving *r;

        if (!local_name(req.name)) {
            rep.error = MDNS_ERR_BAD_NAME;
            break;
        }

        h = host_of(req.name);

        if (h != NULL) {
            rep.has_address = 1;
            rep.address = h->address;
            strncpy(rep.host, req.name, MDNS_NAME_MAX);
            break;
        }

        if (!port.bound) {
            rep.error = MDNS_ERR_NO_NETWORK;
            break;
        }

        /* Asked already, by somebody whose wait ended: parked on that
         * question, which is still being asked. */
        r = NULL;

        {
            unsigned k;

            for (k = 0; k < resolvings_n; k++) {
                if (same_name(resolvings[k].name, req.name) && resolvings[k].who == 0) {
                    r = &resolvings[k];
                }
            }
        }

        if (r == NULL) {
            if (!ROOM(resolvings)) {
                rep.error = MDNS_ERR_FULL;
                break;
            }

            r = &resolvings[resolvings_n++];
            memset(r, 0, sizeof(*r));
            strncpy(r->name, req.name, MDNS_NAME_MAX);
            r->next_ask = now();
        }

        /* Parked: answered when the address comes or the wait ends. */
        r->who = sender;
        r->waited = now() + (uint64_t)(req.wait_ticks ? req.wait_ticks : 5u * tick_hz)
                            * hz / tick_hz;
        r->until = r->waited > now() + ASKED_FOR * hz ? r->waited : now() + ASKED_FOR * hz;
        tick();
        return;
    }

    default:
        rep.error = MDNS_ERR_BAD_OP;
        break;
    }

    answer(sender, &rep);
}

void mdnsd_server(long endpoint, long net_cap, long console_cap)
{
    struct sysinfo info;
    struct schedinfo s;

    console = console_cap;
    net = net_cap;

    if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) {
        hz = info.counter_hz;
    }

    if (kosmos_sched_info(&s) == 0 && s.tick_hz != 0) {
        tick_hz = s.tick_hz;
    }

    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        /* A twentieth of a second while there is something to ask or wait
         * for, which is how soon an answer is read; a second otherwise. */
        unsigned long wait = busy() ? (tick_hz / 20 ? tick_hz / 20 : 1) : tick_hz;
        long got = kosmos_receive(endpoint, &msg, &sender, 0, wait);

        if (got == 0) {
            drain();
            serve(&msg, sender);
        } else if (got != -7) {
            return;
        }

        drain();
        tick();
    }
}
