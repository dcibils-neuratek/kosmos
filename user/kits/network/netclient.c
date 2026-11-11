/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Network Kit from C** (`netclient.h`, `docs/maps.md` M6b).
 *
 * The SMB Kit spoke to the network server by hand for itself alone
 * (`smb_transport.c`): a request in `netproto.h`'s shape, the region of a
 * connection's two rings mapped, bytes copied round the rings and the stack
 * told. The map's `tiles` server needed exactly the same, so it is here
 * once, and the SMB Kit is its first caller and `tiles` its second - no
 * second copy of the rings' arithmetic.
 *
 * **Control by message, data by the region**: a connect, a push and a close
 * are messages; the bytes themselves go round the connection's rings
 * (`tcpring.h`), one side writing and the other reading, the indices the
 * only thing both touch.
 */

#include "netclient.h"

#include <string.h>

#include "kosmos.h"
#include "netproto.h"

/*
 * Set once, before any thread asks: the runner hands it over as it hands
 * the Storage Kit its mounts. Everything else this kit keeps is either the
 * caller's (a `struct net_conn`, one thread's at a time) or behind
 * `polls_lock` below, so any thread of a process may use it (`w3.md` W3e).
 * There was a `net_client_last`, the last answer as a global; nothing
 * called it, and two threads would have read each other's.
 */
static long net = -1;

void net_client_start(long net_cap)
{
    net = net_cap;
}

/* One exchange with the stack. */
static int ask(const struct net_request *req, struct net_reply *rep, struct message *out)
{
    struct message msg;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*req);
    memcpy(msg.data, req, sizeof(*req));

    if (net < 0 || kosmos_call(net, &msg, out) != 0 || out->length < sizeof(*rep)) {
        return 0;
    }

    memcpy(rep, out->data, sizeof(*rep));
    return 1;
}

uint32_t net_client_connect(struct net_conn *c, const uint8_t address[4], uint16_t port)
{
    return net_client_connect_ring(c, address, port, 0);
}

static uint32_t connect_with(struct net_conn *c, const uint8_t address[4], uint16_t port,
                             uint32_t ring_bytes, uint32_t flags);

uint32_t net_client_connect_ring(struct net_conn *c, const uint8_t address[4], uint16_t port,
                                 uint32_t ring_bytes)
{
    return connect_with(c, address, port, ring_bytes, NET_CONNECT_AT_ONCE);
}

uint32_t net_client_connect_wait(struct net_conn *c, const uint8_t address[4], uint16_t port,
                                 uint32_t ring_bytes)
{
    return connect_with(c, address, port, ring_bytes, 0);
}

static uint32_t connect_with(struct net_conn *c, const uint8_t address[4], uint16_t port,
                             uint32_t ring_bytes, uint32_t flags)
{
    struct net_request req;
    struct net_reply rep;
    struct message out;
    long at;

    memset(c, 0, sizeof(*c));
    c->region = -1;

    memset(&req, 0, sizeof(req));
    req.op    = NET_OP_CONNECT;
    req.flags = flags;
    req.port  = port;
    req.ring_bytes = ring_bytes;
    memcpy(req.to.byte, address, 4);

    if (!ask(&req, &rep, &out)) {
        return ~0u;
    }

    if (rep.status != NET_OK || out.cap_plus_one == 0) {
        return rep.status != NET_OK ? rep.status : NET_ERR_CLOSED;
    }

    c->region = (long)out.cap_plus_one - 1;
    at = kosmos_mem_map(c->region);

    if (at < 0) {
        (void)kosmos_cap_drop(c->region);
        c->region = -1;
        return NET_ERR_FULL;
    }

    c->ring = (struct tcp_ring *)(uintptr_t)at;
    c->handle = rep.handle;
    c->open = 1;
    return NET_OK;
}

long net_client_write(struct net_conn *c, const void *p, size_t n)
{
    struct tcp_ring *r = c->ring;
    const uint8_t *from = p;
    uint32_t write, space, taken = 0;

    if (!c->open) {
        return -1;
    }

    if (tcp_ring_acquire(&r->closed) != 0) {
        return -1;
    }

    write = r->out_write;
    space = tcp_ring_space(r->bytes, write, tcp_ring_acquire(&r->out_read));

    if (n > space) {
        n = space;
    }

    while (taken < n) {
        uint32_t at = (write + taken) % r->bytes;
        uint32_t run = r->bytes - at;

        if (run > n - taken) {
            run = (uint32_t)(n - taken);
        }

        memcpy(tcp_ring_out(r) + at, from + taken, run);
        taken += run;
    }

    if (taken == 0) {
        return 0;
    }

    tcp_ring_publish(&r->out_write, write + taken);

    {
        struct net_request req;
        struct net_reply rep;
        struct message out;

        memset(&req, 0, sizeof(req));
        req.op     = NET_OP_PUSH;
        req.handle = c->handle;
        (void)ask(&req, &rep, &out);
    }

    return (long)taken;
}

long net_client_read(struct net_conn *c, void *buf, size_t max)
{
    struct tcp_ring *r = c->ring;
    uint8_t *to = buf;
    uint32_t read, ready, given = 0, over;

    if (!c->open) {
        return -1;
    }

    /* `closed` before the index: bytes published before the close are then
     * certainly seen, and none is taken for "the end". */
    over = tcp_ring_acquire(&r->closed);
    read = r->in_read;
    ready = tcp_ring_ready(tcp_ring_acquire(&r->in_write), read);

    if (ready == 0) {
        return over != 0 ? -1 : 0;
    }

    if (ready > max) {
        ready = (uint32_t)max;
    }

    while (given < ready) {
        uint32_t at = (read + given) % r->bytes;
        uint32_t run = r->bytes - at;

        if (run > ready - given) {
            run = ready - given;
        }

        memcpy(to + given, tcp_ring_in(r) + at, run);
        given += run;
    }

    tcp_ring_publish(&r->in_read, read + given);
    c->received += given;
    return (long)given;
}

int net_client_over(const struct net_conn *c)
{
    return !c->open || tcp_ring_acquire(&c->ring->closed) != 0;
}

void net_client_shutdown(struct net_conn *c)
{
    if (c->open) {
        struct net_request req;
        struct net_reply rep;
        struct message out;

        memset(&req, 0, sizeof(req));
        req.op     = NET_OP_CLOSE;
        req.handle = c->handle;
        (void)ask(&req, &rep, &out);
    }
}

void net_client_close(struct net_conn *c)
{
    if (c->open) {
        struct net_request req;
        struct net_reply rep;
        struct message out;

        memset(&req, 0, sizeof(req));
        req.op     = NET_OP_CLOSE;
        req.handle = c->handle;
        (void)ask(&req, &rep, &out);

        /* Unmapped first and the capability after, as the Lua door lets a
         * ring go (`net_kosmos.c`, `l_gc`). */
        if (kosmos_share_unmap((unsigned long)(uintptr_t)c->ring,
                               tcp_ring_pages(c->ring->bytes)) == 0) {
            (void)kosmos_cap_drop(c->region);
        }
    }

    memset(c, 0, sizeof(*c));
    c->region = -1;
}

/* Four numbers and three dots, each under 256. */
static int four_numbers(const char *text, uint8_t out[4])
{
    unsigned part = 0, value = 0, digits = 0;

    for (const char *p = text; ; p++) {
        if (*p >= '0' && *p <= '9') {
            value = value * 10 + (unsigned)(*p - '0');
            if (++digits > 3 || value > 255) return 0;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0 || part > 3) return 0;
            out[part++] = (uint8_t)value;
            value = digits = 0;
            if (*p == '\0') return part == 4;
        } else {
            return 0;
        }
    }
}

uint32_t net_client_resolve(const char *name, uint8_t address[4], uint32_t wait_ticks)
{
    struct net_request req;
    struct net_reply rep;
    struct message out;
    size_t n = strlen(name);

    if (four_numbers(name, address)) {
        return NET_OK;
    }

    if (n == 0 || n >= NET_PAYLOAD_MAX) {
        return NET_ERR_BAD_ADDRESS;
    }

    memset(&req, 0, sizeof(req));
    req.op         = NET_OP_RESOLVE;
    req.wait_ticks = wait_ticks;
    req.length     = (uint32_t)n;
    memcpy(req.payload, name, n);

    if (!ask(&req, &rep, &out)) {
        return ~0u;
    }

    if (rep.status == NET_OK) {
        memcpy(address, rep.address.byte, 4);
    }

    return rep.status;
}

uint32_t net_client_listen(uint16_t port, uint64_t *listener)
{
    struct net_request req;
    struct net_reply rep;
    struct message out;

    memset(&req, 0, sizeof(req));
    req.op   = NET_OP_LISTEN;
    req.port = port;

    if (!ask(&req, &rep, &out)) {
        return ~0u;
    }

    if (rep.status == NET_OK) {
        *listener = rep.handle;
    }

    return rep.status;
}

uint32_t net_client_accept(uint64_t listener, uint32_t wait_ticks, struct net_conn *c)
{
    struct net_request req;
    struct net_reply rep;
    struct message out;
    long at;

    memset(c, 0, sizeof(*c));
    c->region = -1;

    memset(&req, 0, sizeof(req));
    req.op         = NET_OP_ACCEPT;
    req.handle     = listener;
    req.wait_ticks = wait_ticks;

    if (!ask(&req, &rep, &out)) {
        return ~0u;
    }

    if (rep.status != NET_OK || out.cap_plus_one == 0) {
        return rep.status != NET_OK ? rep.status : NET_ERR_CLOSED;
    }

    c->region = (long)out.cap_plus_one - 1;
    at = kosmos_mem_map(c->region);

    if (at < 0) {
        (void)kosmos_cap_drop(c->region);
        c->region = -1;
        return NET_ERR_FULL;
    }

    c->ring = (struct tcp_ring *)(uintptr_t)at;
    c->handle = rep.handle;
    c->open = 1;
    memcpy(c->peer, rep.from.byte, sizeof c->peer);
    return NET_OK;
}

uint32_t net_client_address(uint8_t address[4], uint8_t netmask[4])
{
    struct net_request req;
    struct net_reply rep;
    struct message out;

    memset(address, 0, 4);
    memset(netmask, 0, 4);
    memset(&req, 0, sizeof(req));
    req.op = NET_OP_INFO;

    if (!ask(&req, &rep, &out)) {
        return ~0u;
    }

    if (rep.status == NET_OK) {
        memcpy(address, rep.address.byte, 4);
        memcpy(netmask, rep.netmask.byte, 4);
    }

    return rep.status;
}

/*
 * The poll sets' regions. A wait parks in the stack for as long as it is
 * allowed, so one region behind a lock would make every other thread's
 * poll wait behind it; instead each wait borrows a region of its own for
 * the length of the call. Up to POLL_KEPT are kept and lent again - one
 * thread polling in a loop makes one, once, as it always did - and a wait
 * that finds them all lent makes one for itself and gives it back after.
 * The lock is held only to lend and to return, never across the call.
 */
#define POLL_KEPT 8

struct poll_region {
    struct net_poll_entry *at;
    long     region;               /* -1: not made yet */
    uint32_t room;                 /* entries it holds */
    uint32_t pages;
    int      lent;
};

static kosmos_lock_t polls_lock;
static struct poll_region polls[POLL_KEPT] = {
    [0 ... POLL_KEPT - 1] = { .region = -1 },
};

static void poll_drop(struct poll_region *p)
{
    if (p->region >= 0 &&
        kosmos_share_unmap((unsigned long)(uintptr_t)p->at, p->pages) == 0) {
        (void)kosmos_cap_drop(p->region);
    }

    p->region = -1;
    p->at = 0;
    p->room = 0;
}

static int poll_make(struct poll_region *p, uint32_t n)
{
    unsigned long pages = ((unsigned long)n * sizeof(struct net_poll_entry) + 4095u) / 4096u;
    long region, at;

    if (pages < 1) {
        pages = 1;
    }

    region = kosmos_mem_create(pages);

    if (region < 0) {
        return 0;
    }

    at = kosmos_mem_map(region);

    if (at < 0) {
        (void)kosmos_cap_drop(region);
        return 0;
    }

    p->region = region;
    p->at = (struct net_poll_entry *)(uintptr_t)at;
    p->pages = (uint32_t)pages;
    p->room = (uint32_t)(pages * 4096u / sizeof(struct net_poll_entry));
    return 1;
}

/* A kept region for this wait, or 0 when every one is lent. */
static struct poll_region *poll_borrow(void)
{
    struct poll_region *p = 0;
    unsigned i;

    kosmos_lock(&polls_lock);

    for (i = 0; i < POLL_KEPT && p == 0; i++) {
        if (!polls[i].lent) {
            polls[i].lent = 1;
            p = &polls[i];
        }
    }

    kosmos_unlock(&polls_lock);
    return p;
}

static void poll_return(struct poll_region *p)
{
    kosmos_lock(&polls_lock);
    p->lent = 0;
    kosmos_unlock(&polls_lock);
}

uint32_t net_client_poll(struct net_poll_entry *set, uint32_t n, uint32_t wait_ticks)
{
    struct net_request req;
    struct net_reply rep;
    struct message msg, out;
    struct poll_region own = { .region = -1 };
    struct poll_region *p = poll_borrow();
    uint32_t i, status;

    if (p == 0) {
        p = &own;
    }

    /* Too small: made again, big enough - a kept one keeps the bigger. */
    if (n > p->room) {
        poll_drop(p);

        if (!poll_make(p, n)) {
            status = NET_ERR_FULL;
            goto done;
        }
    }

    for (i = 0; i < n; i++) {
        p->at[i] = set[i];
        p->at[i].got = 0;
    }

    memset(&req, 0, sizeof(req));
    req.op         = NET_OP_POLL;
    req.wait_ticks = wait_ticks;
    req.length     = n;

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    msg.cap_plus_one = (uint32_t)(p->region + 1);
    memcpy(msg.data, &req, sizeof(req));

    if (net < 0 || kosmos_call(net, &msg, &out) != 0 || out.length < sizeof(rep)) {
        status = ~0u;
        goto done;
    }

    memcpy(&rep, out.data, sizeof(rep));
    status = rep.status;

    if (rep.status == NET_OK) {
        for (i = 0; i < n; i++) {
            set[i].got = p->at[i].got;
        }
    }

done:
    if (p == &own) {
        poll_drop(&own);
    } else {
        poll_return(p);
    }

    return status;
}
