/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Sockets and `poll`, for ported code** (`docs/w3.md` W3e), on the
 * network kit (`netclient.c`) and the `/Network` this program was given.
 *
 * **A socket is a descriptor** in `fd.c`'s table, with a `struct
 * posix_socket` behind it: a connection's ring, shared with the stack, or a
 * listener's handle. Reading and writing are the ring's, in this process
 * with no message at all until a ring is empty or full - which is what the
 * kit was built for - and a wait is `NET_OP_POLL`, the stack holding the
 * caller until the ring has something or its deadline comes.
 *
 * **What is here, and what is not yet.** IPv4 and TCP: `connect`, blocking
 * or not, `listen` and `accept`, `send` and `recv` with `MSG_PEEK` and
 * `MSG_DONTWAIT`, `shutdown`, the options a port sets (taken, and the ring's
 * size from `SO_RCVBUF`), names through the stack's resolver, and `poll`
 * over sockets, files and the console together. Not here: datagrams, which
 * `getaddrinfo` does not need because the stack resolves; IPv6; `select`.
 * Each arrives with the port that asks for it.
 *
 * **Two things a port will notice.** A non-blocking `connect` is writable at
 * once, as the stack takes bytes into the ring while the connection opens
 * and sends them when it has; a refusal then shows as the connection over
 * before a byte arrived, which `SO_ERROR` reports as `ECONNREFUSED`. And a
 * socket closed in one thread while another waits on it is the caller's
 * error, as it is everywhere: the waiter is not woken.
 *
 * **There are no signals**, so writing to a connection that is over is
 * `EPIPE` and nothing else, `MSG_NOSIGNAL` or not.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "kosmos.h"
#include "posix.h"
#include "kits/network/netclient.h"

enum { SOCK_NEW, SOCK_CONNECTED, SOCK_LISTENING };

struct posix_socket {
    int             state;
    int             nonblock_connect;   /* connected without waiting */
    int             shut_write;
    uint32_t        ring_bytes;         /* SO_RCVBUF's, 0 for the stack's */
    long long       recv_timeout_ns;    /* SO_RCVTIMEO; 0: none */
    struct net_conn conn;
    uint64_t        listener;
    uint16_t        bound_port;         /* bind's, host order */
    uint16_t        peer_port;          /* connect's, host order */
    uint8_t         peer[4];
};

/* A wait's longest single stretch in the stack: a minute, asked again. */
#define WAIT_STRETCH_NS  (60LL * 1000000000LL)

static int fails(int e)
{
    errno = e;
    return -1;
}

static int net_errno(uint32_t status)
{
    switch (status) {
    case NET_OK:              return 0;
    case NET_ERR_NO_CARD:     return ENETDOWN;
    case NET_ERR_NO_ROUTE:    return ENETUNREACH;
    case NET_ERR_UNREACHABLE: return EHOSTUNREACH;
    case NET_ERR_FULL:        return ENOMEM;
    case NET_ERR_BAD_ADDRESS: return EADDRNOTAVAIL;
    case NET_ERR_REFUSED:     return ECONNREFUSED;
    case NET_ERR_CLOSED:      return ECONNRESET;
    case NET_ERR_TIMEOUT:     return ETIMEDOUT;
    case NET_ERR_IN_USE:      return EADDRINUSE;
    case ~0u:                 return EACCES;    /* no `/Network` was given */
    default:                  return EIO;
    }
}

static long long now_ns(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return 0;
    }

    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Scheduler ticks for the next stretch of a wait that ends at `until`
 * (0: never), at least one. */
static uint32_t stretch(long long until)
{
    long long left = WAIT_STRETCH_NS;
    unsigned long ticks;

    if (until != 0) {
        left = until - now_ns();

        if (left > WAIT_STRETCH_NS) {
            left = WAIT_STRETCH_NS;
        }
    }

    ticks = posix_ticks_for_ns(left > 0 ? left : 1);
    return ticks > 0 ? (uint32_t)ticks : 1u;
}

static struct posix_socket *socket_of(int fd, int *flags)
{
    struct posix_socket *s = posix_fd_socket(fd, flags);

    if (s == NULL) {
        errno = posix_fd_open(fd) ? ENOTSOCK : EBADF;
    }

    return s;
}

/* The address a caller handed, as four bytes and a port. */
static int address_in(const struct sockaddr *a, socklen_t length,
                      uint8_t to[4], uint16_t *port)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;

    if (a == NULL || length < sizeof(struct sockaddr_in)) {
        return fails(EINVAL);
    }

    if (a->sa_family != AF_INET) {
        return fails(EAFNOSUPPORT);
    }

    memcpy(to, &in->sin_addr.s_addr, 4);
    *port = ntohs(in->sin_port);
    return 0;
}

static void address_out(struct sockaddr *a, socklen_t *length,
                        const uint8_t from[4], uint16_t port)
{
    struct sockaddr_in in;
    socklen_t n;

    if (a == NULL || length == NULL) {
        return;
    }

    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    memcpy(&in.sin_addr.s_addr, from, 4);

    n = *length < sizeof(in) ? *length : (socklen_t)sizeof(in);
    memcpy(a, &in, n);
    *length = sizeof(in);
}

/*
 *--------------------------------------------------------------------------
 * Waiting in the stack.
 */

/* Until `handle` has what `want` asks, or `until` (0: never). 1 when it
 * has, 0 at the deadline, -1 and errno when the stack would not. */
static int wait_for(uint64_t handle, uint32_t want, long long until)
{
    for (;;) {
        struct net_poll_entry e = { handle, want, 0 };
        uint32_t r = net_client_poll(&e, 1, stretch(until));

        if (r != NET_OK) {
            return fails(net_errno(r));
        }

        if (e.got != 0) {
            return 1;
        }

        if (until != 0 && now_ns() >= until) {
            return 0;
        }
    }
}

/*
 *--------------------------------------------------------------------------
 * Making, connecting, listening.
 */

int socket(int domain, int type, int protocol)
{
    struct posix_socket *s;
    int kind = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    int fd;

    if (domain != AF_INET) {
        return fails(EAFNOSUPPORT);
    }

    if (kind != SOCK_STREAM) {
        return fails(kind == SOCK_DGRAM ? EPROTONOSUPPORT : EPROTOTYPE);
    }

    if (protocol != 0 && protocol != IPPROTO_TCP) {
        return fails(EPROTONOSUPPORT);
    }

    if ((s = calloc(1, sizeof(*s))) == NULL) {
        return fails(ENOMEM);
    }

    s->conn.region = -1;
    fd = posix_fd_socket_new(s, ((type & SOCK_NONBLOCK) ? O_NONBLOCK : 0)
                                | ((type & SOCK_CLOEXEC) ? O_CLOEXEC : 0));

    if (fd < 0) {
        free(s);
    }

    return fd;
}

void posix_socket_close(struct posix_socket *s)
{
    if (s->state == SOCK_CONNECTED) {
        net_client_close(&s->conn);
    }

    /* A listener the stack is told nothing about: there is no operation
     * that ends one, and it goes with the process. */
    free(s);
}

int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    struct posix_socket *s = socket_of(fd, NULL);
    uint8_t at[4];
    uint16_t port;

    if (s == NULL) {
        return -1;
    }

    if (address_in(address, length, at, &port) != 0) {
        return -1;
    }

    if (s->state != SOCK_NEW || s->bound_port != 0) {
        return fails(EINVAL);
    }

    /* Every address this machine has, or this one's: the stack answers on
     * a port, not an address. */
    s->bound_port = port;
    return 0;
}

int listen(int fd, int backlog)
{
    struct posix_socket *s = socket_of(fd, NULL);
    uint32_t r;

    (void)backlog;

    if (s == NULL) {
        return -1;
    }

    if (s->state == SOCK_LISTENING) {
        return 0;
    }

    if (s->state != SOCK_NEW || s->bound_port == 0) {
        return fails(s->state != SOCK_NEW ? EISCONN : EDESTADDRREQ);
    }

    if ((r = net_client_listen(s->bound_port, &s->listener)) != NET_OK) {
        return fails(net_errno(r));
    }

    s->state = SOCK_LISTENING;
    return 0;
}

int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{
    int descriptor_flags;
    struct posix_socket *s = socket_of(fd, &descriptor_flags);
    struct posix_socket *got;
    uint32_t r;
    int nonblock, made;

    if (s == NULL) {
        return -1;
    }

    if (s->state != SOCK_LISTENING) {
        return fails(EINVAL);
    }

    nonblock = (descriptor_flags & O_NONBLOCK) != 0;

    if ((got = calloc(1, sizeof(*got))) == NULL) {
        return fails(ENOMEM);
    }

    /* Waited for in the stack's poll, which wakes on an arrival without
     * taking it; `accept` then has one at once. Not waiting is one tick,
     * the shortest wait the stack keeps. */
    for (;;) {
        int w = wait_for(s->listener, NET_WANT_ACCEPT,
                         nonblock ? now_ns() + 1 : 0);

        if (w < 0) {
            free(got);
            return -1;
        }

        if (w == 0) {
            free(got);
            return fails(EAGAIN);
        }

        r = net_client_accept(s->listener, 1, &got->conn);

        if (r == NET_OK) {
            break;
        }

        if (r != NET_ERR_TIMEOUT) {
            free(got);
            return fails(net_errno(r));
        }
    }

    got->state = SOCK_CONNECTED;
    memcpy(got->peer, got->conn.peer, 4);
    made = posix_fd_socket_new(got, ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0)
                                    | ((flags & SOCK_CLOEXEC) ? O_CLOEXEC : 0));

    if (made < 0) {
        net_client_close(&got->conn);
        free(got);
        return -1;
    }

    address_out(address, length, got->peer, got->peer_port);
    return made;
}

int accept(int fd, struct sockaddr *address, socklen_t *length)
{
    return accept4(fd, address, length, 0);
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    int flags;
    struct posix_socket *s = socket_of(fd, &flags);
    uint8_t to[4];
    uint16_t port;
    uint32_t r;

    if (s == NULL) {
        return -1;
    }

    if (address_in(address, length, to, &port) != 0) {
        return -1;
    }

    if (s->state == SOCK_CONNECTED) {
        return fails(s->nonblock_connect ? EALREADY : EISCONN);
    }

    if (s->state != SOCK_NEW) {
        return fails(EINVAL);
    }

    if (flags & O_NONBLOCK) {
        r = net_client_connect_ring(&s->conn, to, port, s->ring_bytes);
    } else {
        r = net_client_connect_wait(&s->conn, to, port, s->ring_bytes);
    }

    if (r != NET_OK) {
        return fails(net_errno(r));
    }

    s->state = SOCK_CONNECTED;
    s->peer_port = port;
    memcpy(s->peer, to, 4);

    if (flags & O_NONBLOCK) {
        s->nonblock_connect = 1;
        return fails(EINPROGRESS);
    }

    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Bytes.
 */

/* What the ring holds, copied and left there. */
static long peek(struct posix_socket *s, void *buf, size_t max)
{
    struct tcp_ring *r = s->conn.ring;
    uint32_t read = r->in_read;
    uint32_t ready = tcp_ring_ready(tcp_ring_acquire(&r->in_write), read);
    uint32_t given = 0;

    if (ready > max) {
        ready = (uint32_t)max;
    }

    while (given < ready) {
        uint32_t at = (read + given) % r->bytes;
        uint32_t run = r->bytes - at;

        if (run > ready - given) {
            run = ready - given;
        }

        memcpy((uint8_t *)buf + given, tcp_ring_in(r) + at, run);
        given += run;
    }

    return (long)given;
}

static long receive(struct posix_socket *s, void *buf, size_t bytes, int nonblock, int flags)
{
    long long until = 0;

    if (s->state != SOCK_CONNECTED) {
        return fails(ENOTCONN);
    }

    if (bytes == 0) {
        return 0;
    }

    if (s->recv_timeout_ns > 0) {
        until = now_ns() + s->recv_timeout_ns;
    }

    for (;;) {
        long n;
        int w;

        if (flags & MSG_PEEK) {
            n = peek(s, buf, bytes);

            if (n == 0 && net_client_over(&s->conn)) {
                return 0;
            }
        } else {
            n = net_client_read(&s->conn, buf, bytes);

            if (n < 0) {
                return 0;               /* over, and everything read */
            }
        }

        if (n > 0) {
            return n;
        }

        if (nonblock || (flags & MSG_DONTWAIT)) {
            return fails(EAGAIN);
        }

        if ((w = wait_for(s->conn.handle, NET_WANT_READ, until)) < 0) {
            return -1;
        }

        if (w == 0) {
            return fails(EAGAIN);       /* SO_RCVTIMEO's, as Linux says it */
        }
    }
}

static long transmit(struct posix_socket *s, const void *buf, size_t bytes, int nonblock, int flags)
{
    size_t sent = 0;

    if (s->state != SOCK_CONNECTED) {
        return fails(ENOTCONN);
    }

    if (s->shut_write) {
        return fails(EPIPE);
    }

    while (sent < bytes) {
        long n = net_client_write(&s->conn, (const uint8_t *)buf + sent, bytes - sent);

        if (n < 0) {
            return sent > 0 ? (long)sent : fails(EPIPE);
        }

        sent += (size_t)n;

        if (sent == bytes) {
            break;
        }

        if (n == 0) {
            if (nonblock || (flags & MSG_DONTWAIT)) {
                return sent > 0 ? (long)sent : fails(EAGAIN);
            }

            if (wait_for(s->conn.handle, NET_WANT_WRITE, 0) < 0) {
                return sent > 0 ? (long)sent : -1;
            }
        }
    }

    return (long)sent;
}

long posix_socket_recv(struct posix_socket *s, void *buf, unsigned long bytes, int nonblock)
{
    return receive(s, buf, bytes, nonblock, 0);
}

long posix_socket_send(struct posix_socket *s, const void *buf, unsigned long bytes, int nonblock)
{
    return transmit(s, buf, bytes, nonblock, 0);
}

ssize_t recv(int fd, void *buf, size_t bytes, int flags)
{
    int descriptor_flags;
    struct posix_socket *s = socket_of(fd, &descriptor_flags);

    if (s == NULL) {
        return -1;
    }

    return receive(s, buf, bytes, (descriptor_flags & O_NONBLOCK) != 0, flags);
}

ssize_t send(int fd, const void *buf, size_t bytes, int flags)
{
    int descriptor_flags;
    struct posix_socket *s = socket_of(fd, &descriptor_flags);

    if (s == NULL) {
        return -1;
    }

    return transmit(s, buf, bytes, (descriptor_flags & O_NONBLOCK) != 0, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t bytes, int flags,
                 struct sockaddr *from, socklen_t *length)
{
    ssize_t n = recv(fd, buf, bytes, flags);

    if (n >= 0 && from != NULL) {
        (void)getpeername(fd, from, length);
    }

    return n;
}

ssize_t sendto(int fd, const void *buf, size_t bytes, int flags,
               const struct sockaddr *to, socklen_t length)
{
    (void)length;

    if (to != NULL) {
        return fails(EISCONN);          /* a stream has its far end already */
    }

    return send(fd, buf, bytes, flags);
}

int shutdown(int fd, int how)
{
    struct posix_socket *s = socket_of(fd, NULL);

    if (s == NULL) {
        return -1;
    }

    if (s->state != SOCK_CONNECTED) {
        return fails(ENOTCONN);
    }

    if (how < SHUT_RD || how > SHUT_RDWR) {
        return fails(EINVAL);
    }

    /* Reading stopped is only this end not reading; the stack has no word
     * for it, and what arrives waits in the ring until the close. */
    if (how != SHUT_RD && !s->shut_write) {
        s->shut_write = 1;
        net_client_shutdown(&s->conn);
    }

    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Options and names.
 */

int setsockopt(int fd, int level, int name, const void *value, socklen_t length)
{
    struct posix_socket *s = socket_of(fd, NULL);

    if (s == NULL) {
        return -1;
    }

    if (value == NULL && length > 0) {
        return fails(EFAULT);
    }

    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_RCVBUF:
        case SO_SNDBUF:
            /* The ring each way, for a connection not made yet. */
            if (length >= sizeof(int) && *(const int *)value > 0) {
                s->ring_bytes = (uint32_t)*(const int *)value;
            }
            return 0;

        case SO_RCVTIMEO:
            if (length >= sizeof(struct timeval)) {
                const struct timeval *t = value;
                s->recv_timeout_ns = (long long)t->tv_sec * 1000000000LL
                                   + (long long)t->tv_usec * 1000LL;
            }
            return 0;

        case SO_REUSEADDR: case SO_REUSEPORT: case SO_KEEPALIVE:
        case SO_LINGER: case SO_SNDTIMEO:
            return 0;                   /* taken: the stack decides */

        default:
            return fails(ENOPROTOOPT);
        }
    }

    if (level == IPPROTO_TCP) {
        switch (name) {
        case TCP_NODELAY: case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT:
            return 0;

        default:
            return fails(ENOPROTOOPT);
        }
    }

    return fails(ENOPROTOOPT);
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *length)
{
    struct posix_socket *s = socket_of(fd, NULL);
    int answer;

    if (s == NULL) {
        return -1;
    }

    if (value == NULL || length == NULL || *length < sizeof(int)) {
        return fails(EINVAL);
    }

    if (level == SOL_SOCKET && name == SO_ERROR) {
        /* Over before a byte arrived, on a connection not waited for: the
         * far end said no (the comment at the top says why so). */
        answer = s->state == SOCK_CONNECTED && s->nonblock_connect
              && s->conn.received == 0 && net_client_over(&s->conn)
              && tcp_ring_ready(tcp_ring_acquire(&s->conn.ring->in_write),
                                s->conn.ring->in_read) == 0
               ? ECONNREFUSED : 0;
    } else if (level == SOL_SOCKET && name == SO_TYPE) {
        answer = SOCK_STREAM;
    } else if (level == SOL_SOCKET && (name == SO_RCVBUF || name == SO_SNDBUF)) {
        answer = s->state == SOCK_CONNECTED ? (int)s->conn.ring->bytes
               : s->ring_bytes != 0 ? (int)s->ring_bytes : (int)TCP_RING_BYTES;
    } else if ((level == SOL_SOCKET && (name == SO_REUSEADDR || name == SO_KEEPALIVE))
               || (level == IPPROTO_TCP && name == TCP_NODELAY)) {
        answer = 0;
    } else {
        return fails(ENOPROTOOPT);
    }

    memcpy(value, &answer, sizeof(answer));
    *length = sizeof(answer);
    return 0;
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length)
{
    struct posix_socket *s = socket_of(fd, NULL);

    if (s == NULL) {
        return -1;
    }

    if (s->state != SOCK_CONNECTED) {
        return fails(ENOTCONN);
    }

    address_out(address, length, s->peer, s->peer_port);
    return 0;
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
    struct posix_socket *s = socket_of(fd, NULL);
    uint8_t mine[4] = { 0, 0, 0, 0 }, mask[4];

    if (s == NULL) {
        return -1;
    }

    if (s->state == SOCK_CONNECTED) {
        (void)net_client_address(mine, mask);
    }

    address_out(address, length, mine, s->bound_port);
    return 0;
}

/*
 *--------------------------------------------------------------------------
 * `poll`.
 */

/* What a socket can say without asking the stack: a connection's ring is
 * here to read. A listener cannot - an arrival is the stack's to know. */
static short socket_now(struct posix_socket *s, short events)
{
    struct tcp_ring *r;
    short got = 0;

    if (s->state == SOCK_LISTENING) {
        return 0;
    }

    if (s->state != SOCK_CONNECTED) {
        return 0;
    }

    r = s->conn.ring;

    if (tcp_ring_ready(tcp_ring_acquire(&r->in_write), r->in_read) > 0) {
        got |= (short)(events & (POLLIN | POLLRDNORM));
    }

    if (tcp_ring_acquire(&r->closed) != 0) {
        got |= POLLHUP | (short)(events & (POLLIN | POLLRDNORM | POLLRDHUP));
    } else if (!s->shut_write
               && tcp_ring_space(r->bytes, r->out_write,
                                 tcp_ring_acquire(&r->out_read)) > 0) {
        got |= (short)(events & (POLLOUT | POLLWRNORM));
    }

    return got;
}

int poll(struct pollfd *set, nfds_t count, int timeout_ms)
{
    struct net_poll_entry few[16], *entries = few;
    nfds_t *which = NULL, few_which[16];
    long long until = timeout_ms > 0 ? now_ns() + (long long)timeout_ms * 1000000LL : 0;
    nfds_t i;
    int ready;

    if (set == NULL && count > 0) {
        return fails(EFAULT);
    }

    if (count > 16) {
        entries = malloc(count * sizeof(*entries));
        which = malloc(count * sizeof(*which));

        if (entries == NULL || which == NULL) {
            free(entries);
            free(which);
            return fails(ENOMEM);
        }
    } else {
        which = few_which;
    }

    for (;;) {
        uint32_t asked = 0;

        ready = 0;

        /* What is ready here, and what to ask the stack about. */
        for (i = 0; i < count; i++) {
            struct posix_socket *s;
            int fd = set[i].fd;

            set[i].revents = 0;

            if (fd < 0) {
                continue;
            }

            s = posix_fd_socket(fd, NULL);

            if (s == NULL) {
                if (!posix_fd_open(fd)) {
                    set[i].revents = POLLNVAL;
                } else if (fd == 0) {
                    set[i].revents = (short)(set[i].events & (POLLIN | POLLRDNORM)) | POLLHUP;
                } else {
                    set[i].revents = (short)(set[i].events & (POLLIN | POLLOUT
                                                              | POLLRDNORM | POLLWRNORM));
                }
            } else {
                set[i].revents = socket_now(s, set[i].events);

                if (s->state == SOCK_LISTENING && (set[i].events & (POLLIN | POLLRDNORM))) {
                    entries[asked].handle = s->listener;
                    entries[asked].want = NET_WANT_ACCEPT;
                    entries[asked].got = 0;
                    which[asked++] = i;
                } else if (s->state == SOCK_CONNECTED && set[i].revents == 0) {
                    uint32_t want = 0;

                    if (set[i].events & (POLLIN | POLLRDNORM | POLLRDHUP)) want |= NET_WANT_READ;
                    if (set[i].events & (POLLOUT | POLLWRNORM)) want |= NET_WANT_WRITE;

                    entries[asked].handle = s->conn.handle;
                    entries[asked].want = want;
                    entries[asked].got = 0;
                    which[asked++] = i;
                }
            }

            if (set[i].revents != 0) {
                ready++;
            }
        }

        /* A listener is the stack's to answer even when something here is
         * ready, so a set with both is asked once, for the shortest time. */
        if (asked > 0) {
            int listeners = 0;
            uint32_t k, r;

            for (k = 0; k < asked; k++) {
                listeners += entries[k].want == NET_WANT_ACCEPT;
            }

            if (ready > 0 || timeout_ms == 0) {
                if (listeners == 0) {
                    break;
                }

                r = net_client_poll(entries, asked, 1);
            } else {
                r = net_client_poll(entries, asked, stretch(until));
            }

            if (r != NET_OK) {
                if (ready == 0) {
                    ready = fails(net_errno(r));
                }
                break;
            }

            for (k = 0; k < asked; k++) {
                struct pollfd *p = &set[which[k]];
                uint32_t got = entries[k].got;
                short add = 0;

                if (got & NET_WANT_ACCEPT) add |= (short)(p->events & (POLLIN | POLLRDNORM));
                if (got & NET_WANT_READ)   add |= (short)(p->events & (POLLIN | POLLRDNORM));
                if (got & NET_WANT_WRITE)  add |= (short)(p->events & (POLLOUT | POLLWRNORM));
                if (got & NET_GOT_OVER)    add |= POLLHUP | (short)(p->events & (POLLIN | POLLRDNORM));

                if (add != 0 && p->revents == 0) {
                    ready++;
                }

                p->revents |= add;
            }

            if (ready > 0 || timeout_ms == 0 || (until != 0 && now_ns() >= until)) {
                break;
            }

            continue;
        }

        if (ready > 0 || timeout_ms == 0) {
            break;
        }

        /* Nothing that can become ready: the time, slept. */
        if (until == 0) {
            struct timespec forever = { 3600, 0 };

            (void)nanosleep(&forever, NULL);
            continue;
        }

        {
            long long left = until - now_ns();
            struct timespec t = { (time_t)(left / 1000000000LL), (long)(left % 1000000000LL) };

            if (left > 0) {
                (void)nanosleep(&t, NULL);
            }
        }

        break;
    }

    if (entries != few) {
        free(entries);
        free(which);
    }

    return ready;
}

/*
 *--------------------------------------------------------------------------
 * Addresses as text, and names.
 */

/* Four numbers and three dots, each under 256. */
static int four_numbers(const char *text, uint8_t out[4])
{
    unsigned part = 0, value = 0, digits = 0;

    for (;; text++) {
        if (*text >= '0' && *text <= '9') {
            value = value * 10u + (unsigned)(*text - '0');

            if (++digits > 3 || value > 255u) {
                return 0;
            }
        } else if ((*text == '.' || *text == '\0') && digits > 0 && part < 4) {
            out[part++] = (uint8_t)value;
            value = digits = 0;

            if (*text == '\0') {
                return part == 4;
            }
        } else {
            return 0;
        }
    }
}

int inet_pton(int family, const char *text, void *out)
{
    if (family != AF_INET) {
        return fails(EAFNOSUPPORT);
    }

    return text != NULL && four_numbers(text, out) ? 1 : 0;
}

const char *inet_ntop(int family, const void *address, char *out, socklen_t room)
{
    const uint8_t *b = address;
    char text[INET_ADDRSTRLEN];
    int n;

    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }

    n = snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);

    if (out == NULL || n < 0 || (socklen_t)n >= room) {
        errno = ENOSPC;
        return NULL;
    }

    memcpy(out, text, (size_t)n + 1);
    return out;
}

int inet_aton(const char *text, struct in_addr *out)
{
    uint8_t b[4];

    if (text == NULL || !four_numbers(text, b)) {
        return 0;
    }

    if (out != NULL) {
        memcpy(&out->s_addr, b, 4);
    }

    return 1;
}

in_addr_t inet_addr(const char *text)
{
    struct in_addr a;

    return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr address)
{
    static char text[INET_ADDRSTRLEN];      /* POSIX's: one, and not a thread's */

    return (char *)inet_ntop(AF_INET, &address.s_addr, text, sizeof(text));
}

static int service_port(const char *service, int numeric_only, uint16_t *port)
{
    static const struct { const char *name; uint16_t port; } known[] = {
        { "http", 80 }, { "https", 443 }, { "ftp", 21 }, { "ssh", 22 },
        { "telnet", 23 }, { "smtp", 25 }, { "domain", 53 }, { "imap", 143 },
        { "imaps", 993 }, { "pop3", 110 }, { "smb", 445 }, { "microsoft-ds", 445 },
    };
    unsigned long value = 0;
    const char *c;
    size_t i;

    if (service == NULL || *service == '\0') {
        *port = 0;
        return 1;
    }

    for (c = service; *c >= '0' && *c <= '9'; c++) {
        value = value * 10u + (unsigned long)(*c - '0');

        if (value > 65535u) {
            return 0;
        }
    }

    if (*c == '\0') {
        *port = (uint16_t)value;
        return 1;
    }

    if (numeric_only) {
        return 0;
    }

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(service, known[i].name) == 0) {
            *port = known[i].port;
            return 1;
        }
    }

    return 0;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **out)
{
    int flags = hints != NULL ? hints->ai_flags : 0;
    int family = hints != NULL ? hints->ai_family : AF_UNSPEC;
    int type = hints != NULL ? hints->ai_socktype : 0;
    uint8_t at[4] = { 0, 0, 0, 0 };
    uint16_t port;
    struct {
        struct addrinfo    info;
        struct sockaddr_in in;
    } *one;
    size_t name_bytes = 0;

    if (out == NULL || (node == NULL && service == NULL)) {
        return EAI_NONAME;
    }

    *out = NULL;

    if (family != AF_UNSPEC && family != AF_INET) {
        return EAI_FAMILY;
    }

    if (type != 0 && type != SOCK_STREAM) {
        return EAI_SOCKTYPE;
    }

    if (!service_port(service, (flags & AI_NUMERICSERV) != 0, &port)) {
        return EAI_SERVICE;
    }

    if (node == NULL) {
        if (!(flags & AI_PASSIVE)) {
            at[0] = 127; at[3] = 1;
        }
    } else if (!four_numbers(node, at)) {
        uint32_t r;

        if (flags & AI_NUMERICHOST) {
            return EAI_NONAME;
        }

        if (strcmp(node, "localhost") == 0) {
            at[0] = 127; at[3] = 1;
        } else if ((r = net_client_resolve(node, at,
                                           (uint32_t)posix_ticks_for_ns(5000000000LL)))
                   != NET_OK) {
            return r == NET_ERR_NO_NAME || r == NET_ERR_BAD_ADDRESS ? EAI_NONAME
                 : r == NET_ERR_TIMEOUT ? EAI_AGAIN : EAI_FAIL;
        }
    }

    if ((flags & AI_CANONNAME) && node != NULL) {
        name_bytes = strlen(node) + 1;
    }

    if ((one = calloc(1, sizeof(*one) + name_bytes)) == NULL) {
        return EAI_MEMORY;
    }

    one->in.sin_family = AF_INET;
    one->in.sin_port = htons(port);
    memcpy(&one->in.sin_addr.s_addr, at, 4);

    one->info.ai_flags = flags;
    one->info.ai_family = AF_INET;
    one->info.ai_socktype = SOCK_STREAM;
    one->info.ai_protocol = IPPROTO_TCP;
    one->info.ai_addrlen = sizeof(one->in);
    one->info.ai_addr = (struct sockaddr *)&one->in;

    if (name_bytes > 0) {
        one->info.ai_canonname = (char *)(one + 1);
        memcpy(one->info.ai_canonname, node, name_bytes);
    }

    *out = &one->info;
    return 0;
}

void freeaddrinfo(struct addrinfo *list)
{
    while (list != NULL) {
        struct addrinfo *next = list->ai_next;

        free(list);                     /* one allocation, its address inside */
        list = next;
    }
}

const char *gai_strerror(int code)
{
    switch (code) {
    case 0:            return "no error";
    case EAI_NONAME:   return "no such name";
    case EAI_AGAIN:    return "the resolver did not answer in time";
    case EAI_FAIL:     return "the name could not be resolved";
    case EAI_FAMILY:   return "only IPv4 addresses";
    case EAI_SOCKTYPE: return "only streams";
    case EAI_SERVICE:  return "no such service";
    case EAI_MEMORY:   return "out of memory";
    case EAI_BADFLAGS: return "bad flags";
    default:           return "unknown error";
    }
}

/*
 * `select` is `poll` (`sys/select.h`): each descriptor named in any set is
 * one `pollfd`, and what `poll` says comes back as the sets. A descriptor
 * that is not open is EBADF, as POSIX has it, rather than a quiet "ready".
 */
int select(int count, fd_set *readable, fd_set *writable, fd_set *failed,
           struct timeval *timeout)
{
    struct pollfd *set;
    nfds_t used = 0;
    int timeout_ms = -1;
    int ready;
    int r;

    if (count < 0 || count > FD_SETSIZE
        || (timeout != NULL && (timeout->tv_sec < 0 || timeout->tv_usec < 0))) {
        errno = EINVAL;
        return -1;
    }
    if (timeout != NULL) {
        long long ms = (long long)timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000;

        timeout_ms = ms > 0x7fffffff ? 0x7fffffff : (int)ms;
    }
    if ((set = calloc(count > 0 ? (size_t)count : 1, sizeof(*set))) == NULL) {
        errno = ENOMEM;
        return -1;
    }

    for (int fd = 0; fd < count; fd++) {
        short events = 0;

        if (readable != NULL && FD_ISSET(fd, readable)) events |= POLLIN;
        if (writable != NULL && FD_ISSET(fd, writable)) events |= POLLOUT;
        if (failed != NULL && FD_ISSET(fd, failed))     events |= POLLPRI;
        if (events != 0) {
            set[used].fd = fd;
            set[used].events = events;
            used++;
        }
    }

    if ((r = poll(set, used, timeout_ms)) < 0) {
        free(set);
        return -1;
    }
    for (nfds_t i = 0; i < used; i++) {
        if (set[i].revents & POLLNVAL) {
            free(set);
            errno = EBADF;
            return -1;
        }
    }

    if (readable != NULL) FD_ZERO(readable);
    if (writable != NULL) FD_ZERO(writable);
    if (failed != NULL)   FD_ZERO(failed);
    ready = 0;
    for (nfds_t i = 0; i < used; i++) {
        short said = set[i].revents;
        int fd = set[i].fd;

        /* An error or a hang-up wakes a reader and a writer both: the
         * call they make next is what reports it, as on Linux. */
        if ((set[i].events & POLLIN) && (said & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fd, readable);
            ready++;
        }
        if ((set[i].events & POLLOUT) && (said & (POLLOUT | POLLHUP | POLLERR))) {
            FD_SET(fd, writable);
            ready++;
        }
        if ((set[i].events & POLLPRI) && (said & POLLPRI)) {
            FD_SET(fd, failed);
            ready++;
        }
    }
    free(set);
    return ready;
}

int h_errno;

struct hostent *gethostbyname(const char *name)
{
    static char named[256];
    static struct in_addr address;
    static char *addresses[2];
    static char *no_aliases[1];
    static struct hostent host;
    struct addrinfo hints, *found;
    int r;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (name == NULL || strlen(name) >= sizeof(named)) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    if ((r = getaddrinfo(name, NULL, &hints, &found)) != 0) {
        h_errno = r == EAI_NONAME ? HOST_NOT_FOUND : r == EAI_AGAIN ? TRY_AGAIN : NO_RECOVERY;
        return NULL;
    }

    address = ((struct sockaddr_in *)found->ai_addr)->sin_addr;
    freeaddrinfo(found);
    strcpy(named, name);
    addresses[0] = (char *)&address;
    addresses[1] = NULL;
    no_aliases[0] = NULL;
    host.h_name = named;
    host.h_aliases = no_aliases;
    host.h_addrtype = AF_INET;
    host.h_length = 4;
    host.h_addr_list = addresses;
    return &host;
}

struct servent *getservbyname(const char *name, const char *protocol)
{
    static char named[32];
    static char tcp[] = "tcp";
    static char *no_aliases[1];
    static struct servent service;
    uint16_t port;

    /* Streams only, and names only - a number is not a service's name. */
    if (name == NULL || strlen(name) >= sizeof(named)
        || (protocol != NULL && strcmp(protocol, "tcp") != 0)
        || (*name >= '0' && *name <= '9') || !service_port(name, 0, &port)) {
        return NULL;
    }

    strcpy(named, name);
    no_aliases[0] = NULL;
    service.s_name = named;
    service.s_aliases = no_aliases;
    service.s_port = htons(port);
    service.s_proto = tcp;
    return &service;
}

int getnameinfo(const struct sockaddr *address, socklen_t length,
                char *host, socklen_t host_room,
                char *service, socklen_t service_room, int flags)
{
    uint8_t at[4];
    uint16_t port;

    (void)flags;

    if (address_in(address, length, at, &port) != 0) {
        return EAI_FAMILY;
    }

    /* Numbers, always: the stack resolves names forward and not back. */
    if (host != NULL && host_room > 0 && inet_ntop(AF_INET, at, host, host_room) == NULL) {
        return EAI_MEMORY;
    }

    if (service != NULL && service_room > 0
        && snprintf(service, service_room, "%u", port) >= (int)service_room) {
        return EAI_MEMORY;
    }

    return 0;
}
