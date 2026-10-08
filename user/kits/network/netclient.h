/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Network Kit from C (`netclient.c`, `docs/maps.md` M6b): a TCP
 * connection to another machine for code with no Lua - a server, a kit's C.
 * The Lua door is `net_kosmos.c`; this is the same conversation with the
 * network server (`netproto.h`) and the same shared rings (`tcpring.h`).
 *
 * Nothing here waits but `net_client_resolve`, which parks the caller for
 * as long as it is told to: a name is one question, asked once and kept.
 */

#ifndef KOSMOS_NET_CLIENT_H
#define KOSMOS_NET_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "tcpring.h"

struct net_conn {
    struct tcp_ring *ring;          /* shared with the stack */
    long             region;        /* its capability */
    uint64_t         handle;        /* the stack's name for it */
    uint64_t         received;      /* bytes read so far */
    int              open;
};

/* The capability to the network server this process was given. Every
 * call below refuses until it has one. */
void     net_client_start(long net_cap);

/* The last answer the network server gave, `NET_OK` or a `NET_ERR_*`. */
uint32_t net_client_last(void);

/*
 * A connection to `address` (four bytes, in order) at `port`, begun and
 * answered at once: the ring is there to write into while the handshake
 * goes on. `NET_OK`, or what the stack refused it with.
 */
uint32_t net_client_connect(struct net_conn *c, const uint8_t address[4], uint16_t port);

/* Bytes to send: how many the ring took (0 when it is full), -1 when the
 * connection is over. The stack is told there is something to send. */
long     net_client_write(struct net_conn *c, const void *p, size_t n);

/* Bytes that arrived, up to `max`: how many; 0 when none yet; -1 when the
 * connection is over and nothing is left to read. */
long     net_client_read(struct net_conn *c, void *buf, size_t max);

/* Whether the far end has finished (bytes may still be left to read). */
int      net_client_over(const struct net_conn *c);

/* This end done, and the ring given back. */
void     net_client_close(struct net_conn *c);

/* A name's address, waiting at most `wait_ticks` scheduler ticks for the
 * resolver. A name that is four numbers is answered without asking. */
uint32_t net_client_resolve(const char *name, uint8_t address[4], uint32_t wait_ticks);

#endif
