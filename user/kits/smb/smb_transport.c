/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * libsmb2's socket, as a connection on the network stack's ring
 * (`docs/sharing.md`, *What the Kosmos port patches*, item 1).
 *
 * `port/smb_port.h` turns libsmb2's `socket`, `connect`, `readv`, `writev`
 * and the rest into the `smb_kosmos_*` here, and nothing outside the SMB
 * Kit's build sees either name. What each becomes:
 *
 *   socket       a slot in this file's table of connections - the index is
 *                libsmb2's `t_socket` - and nothing asked of the stack yet;
 *   connect      `NET_OP_CONNECT` with `NET_CONNECT_AT_ONCE`: the stack
 *                answers at once with the region holding the two rings
 *                (`tcpring.h`), mapped here, while the handshake goes on.
 *                The answer is `EINPROGRESS`, as a non-blocking socket's
 *                is, and what libsmb2 writes waits in the ring until the
 *                connection opens - the stack's arrangement, not this
 *                file's;
 *   writev       the vectors copied into `out` as far as there is room,
 *                and `NET_OP_PUSH`; no room is `EAGAIN`;
 *   readv        `in` copied into the vectors - which, for a READ's data,
 *                are the caller's own buffer, since that is what libsmb2's
 *                zero-copy path hands its socket - and nothing there is
 *                `EAGAIN`, or the end once the stack has said the
 *                connection is over;
 *   getsockopt   `SO_ERROR`: refused when the connection closed before the
 *                far end took a byte, which is what libsmb2 asks a socket
 *                it was told is writable;
 *   getaddrinfo  four numbers and a port, written as they were typed. A
 *                name is the stack's `NET_OP_RESOLVE`, which parks its
 *                caller - so the process that owns this asks it on a
 *                thread that may wait (`smbfs.c`, the waiter) and hands
 *                libsmb2 the address.
 *
 * **One thread uses this**, the one that owns libsmb2's contexts: the table
 * and every ring are touched by it alone, which is why nothing here takes a
 * lock (`docs/threads.md`: there is no futex yet, and `malloc` has none).
 *
 * The table grows with what is asked of it, a slot at a time and doubling,
 * and a slot goes back when libsmb2 closes its socket; nothing compiled in
 * says how many (`CLAUDE.md`, *the pools grow*).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "netproto.h"
#include "kits/network/netclient.h"

#include "smb_kit.h"
#include "ntlm_name.h"

/*
 * **What the server calls itself, heard on the way in** (`testing.md`
 * 18.415). libsmb2 keeps only NTLM's TargetName, which macOS's server fills
 * with a piece of its address; the computer's own names are in the same
 * challenge and libsmb2 lets them go. So the first bytes a connection
 * receives are kept, up to `HEARD_MOST`, until a challenge has been read
 * out of them (`ntlm_name.c`) - in the session's setup, a few hundred bytes
 * in - and nothing is kept after it, or after `LISTEN_MOST` without one.
 * libsmb2 is not touched: this is its socket, and what it reads is what
 * arrived.
 */
#define HEARD_MOST   4096u
#define LISTEN_MOST  65536u
#define NAME_MOST    64u

/*
 * A socket is the Network Kit's C connection (`netclient.h`, `docs/maps.md`
 * M6b) - which this file used to be, for itself alone - and what the SMB
 * Kit hears on it.
 */
struct sock {
    bool             used;
    bool             connected;     /* the stack gave it a ring */
    struct net_conn  conn;

    bool             listened;      /* a challenge read, or given up on */
    uint8_t         *heard;         /* what arrived, until then */
    uint32_t         heard_len;
    char             name[NAME_MOST];
};

static struct sock *socks;
static unsigned sock_count;
static uint32_t last_refusal = NET_OK;

void smb_kit_start(long net_cap)
{
    net_client_start(net_cap);
}

uint32_t smb_kit_last_refusal(void)
{
    return last_refusal;
}

static struct sock *sock_at(int fd)
{
    if (fd < 0 || (unsigned)fd >= sock_count || !socks[fd].used) {
        return NULL;
    }

    return &socks[fd];
}

int smb_kosmos_socket(int family, int type, int protocol)
{
    unsigned i;

    (void)protocol;

    if (family != AF_INET || type != SOCK_STREAM) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < sock_count; i++) {
        if (!socks[i].used) {
            break;
        }
    }

    if (i == sock_count) {
        unsigned more = sock_count == 0 ? 4u : sock_count * 2u;
        struct sock *grown = realloc(socks, more * sizeof(*grown));

        if (grown == NULL) {
            errno = ENOMEM;
            return -1;
        }

        memset(grown + sock_count, 0, (more - sock_count) * sizeof(*grown));
        socks = grown;
        sock_count = more;
    }

    memset(&socks[i], 0, sizeof(socks[i]));
    socks[i].used = true;
    socks[i].conn.region = -1;
    return (int)i;
}

int smb_kosmos_connect(int fd, const struct sockaddr *to, socklen_t len)
{
    struct sock *s = sock_at(fd);
    const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)to;
    uint8_t address[4];
    uint32_t status;

    if (s == NULL || s->connected || len < sizeof(*in)
        || in->sin_family != AF_INET) {
        errno = EINVAL;
        return -1;
    }

    memcpy(address, &in->sin_addr.s_addr, 4);
    status = net_client_connect(&s->conn, address, ntohs(in->sin_port));
    last_refusal = status;

    if (status != NET_OK) {
        errno = status == NET_ERR_REFUSED ? ECONNREFUSED
              : status == NET_ERR_TIMEOUT ? ETIMEDOUT
              : status == NET_ERR_NO_CARD ? ENETDOWN
              : status == NET_ERR_FULL ? ENOMEM
              : status == ~0u ? ENETDOWN
              : ENETUNREACH;
        return -1;
    }

    s->connected = true;
    errno = EINPROGRESS;
    return -1;
}

int smb_kosmos_close(int fd)
{
    struct sock *s = sock_at(fd);

    if (s == NULL) {
        errno = EBADF;
        return -1;
    }

    if (s->connected) {
        net_client_close(&s->conn);
    }

    free(s->heard);
    memset(s, 0, sizeof(*s));
    return 0;
}

bool smb_kit_link(int fd, struct smb_link *out)
{
    struct sock *s = sock_at(fd);

    memset(out, 0, sizeof(*out));

    if (s == NULL || !s->connected) {
        return false;
    }

    out->handle   = s->conn.handle;
    out->taken    = tcp_ring_acquire(&s->conn.ring->out_read) != 0;
    out->closed   = tcp_ring_acquire(&s->conn.ring->closed) != 0;
    out->received = s->conn.received;
    out->room     = tcp_ring_space(s->conn.ring->bytes, s->conn.ring->out_write,
                                   tcp_ring_acquire(&s->conn.ring->out_read));
    return true;
}

int smb_kosmos_getsockopt(int fd, int level, int name, void *value,
                          socklen_t *len)
{
    struct sock *s = sock_at(fd);
    int err = 0;

    if (s == NULL || level != SOL_SOCKET || name != SO_ERROR
        || value == NULL || len == NULL || *len < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }

    /* Over before the far end took a byte: refused, or never answered. */
    if (!s->connected
        || (tcp_ring_acquire(&s->conn.ring->closed) != 0
            && tcp_ring_acquire(&s->conn.ring->out_read) == 0 && s->conn.received == 0)) {
        err = ECONNREFUSED;
    }

    memcpy(value, &err, sizeof(err));
    *len = sizeof(err);
    return 0;
}

/* Nothing to set: the stack's connection has no options, is never
 * blocking, and sends what it has. */
int smb_kosmos_setsockopt(int fd, int level, int name, const void *value,
                          socklen_t len)
{
    (void)level; (void)name; (void)value; (void)len;

    return sock_at(fd) != NULL ? 0 : -1;
}

int smb_kosmos_fcntl(int fd, int command, ...)
{
    (void)command;

    return sock_at(fd) != NULL ? 0 : -1;
}

ssize_t smb_kosmos_writev(int fd, const struct iovec *iov, int count)
{
    struct sock *s = sock_at(fd);
    ssize_t taken = 0;
    int i;

    if (s == NULL || !s->connected) {
        errno = ENOTCONN;
        return -1;
    }

    for (i = 0; i < count; i++) {
        long n = net_client_write(&s->conn, iov[i].iov_base, iov[i].iov_len);

        if (n < 0) {
            if (taken > 0) break;
            errno = EPIPE;
            return -1;
        }

        taken += n;

        if ((size_t)n < iov[i].iov_len) break;      /* the ring is full */
    }

    if (taken == 0) {
        errno = EAGAIN;
        return -1;
    }

    return taken;
}

/* Done listening: what was kept goes, and nothing more is. */
static void stop_listening(struct sock *s)
{
    free(s->heard);
    s->heard = NULL;
    s->heard_len = 0;
    s->listened = true;
}

/*
 * The `given` bytes just read, added to what was heard, and a
 * challenge looked for in all of it - the signature anywhere, since it sits
 * inside SPNEGO inside a SESSION_SETUP reply, and a reply may arrive over
 * several reads.
 */
static void listen_for_name(struct sock *s, const uint8_t *bytes, uint32_t given)
{
    uint32_t i, take = given;

    if (s->conn.received > LISTEN_MOST) {
        stop_listening(s);
        return;
    }

    if (s->heard == NULL) {
        s->heard = malloc(HEARD_MOST);

        if (s->heard == NULL) {
            stop_listening(s);
            return;
        }
    }

    /* The newest bytes are the ones that can finish a challenge. */
    if (take > HEARD_MOST) {
        bytes += take - HEARD_MOST;
        take = HEARD_MOST;
    }

    if (s->heard_len + take > HEARD_MOST) {
        uint32_t drop = s->heard_len + take - HEARD_MOST;

        memmove(s->heard, s->heard + drop, s->heard_len - drop);
        s->heard_len -= drop;
    }

    memcpy(s->heard + s->heard_len, bytes, take);

    s->heard_len += take;

    for (i = 0; i + 8 <= s->heard_len; i++) {
        int got;

        if (memcmp(s->heard + i, "NTLMSSP", 8) != 0) {
            continue;
        }

        got = ntlm_challenge_name(s->heard + i, s->heard_len - i, s->name,
                                  sizeof(s->name));

        if (got == NTLM_NAME_SHORT) {
            return;                     /* the rest is still on its way */
        }

        if (got == NTLM_NAME_FOUND || got == NTLM_NAME_NONE) {
            stop_listening(s);
            return;
        }
    }
}

ssize_t smb_kosmos_readv(int fd, const struct iovec *iov, int count)
{
    struct sock *s = sock_at(fd);
    ssize_t given = 0;
    int i;

    if (s == NULL || !s->connected) {
        errno = ENOTCONN;
        return -1;
    }

    for (i = 0; i < count; i++) {
        long n = net_client_read(&s->conn, iov[i].iov_base, iov[i].iov_len);

        if (n < 0) {
            if (given > 0) break;
            return 0;                           /* over, and nothing left */
        }

        if (n > 0 && !s->listened) {
            listen_for_name(s, iov[i].iov_base, (uint32_t)n);
        }

        given += n;

        if ((size_t)n < iov[i].iov_len) break;  /* nothing more yet */
    }

    if (given == 0) {
        errno = EAGAIN;
        return -1;
    }

    return given;
}

bool smb_kit_server_name(int fd, char *out, size_t room)
{
    struct sock *s = sock_at(fd);

    if (s == NULL || s->name[0] == '\0' || room == 0) {
        return false;
    }

    snprintf(out, room, "%s", s->name);
    return true;
}

/* "10.0.2.2" and "445": four numbers and a port, or nothing. */
static bool four_numbers(const char *text, uint8_t out[4])
{
    unsigned part = 0;

    for (int i = 0; i < 4; i++) {
        unsigned digits = 0;

        part = 0;

        while (*text >= '0' && *text <= '9' && digits < 4) {
            part = part * 10u + (unsigned)(*text++ - '0');
            digits++;
        }

        if (digits == 0 || part > 255) {
            return false;
        }

        out[i] = (uint8_t)part;

        if (i < 3 && *text++ != '.') {
            return false;
        }
    }

    return *text == '\0';
}

int smb_kosmos_getaddrinfo(const char *node, const char *service,
                           const struct addrinfo *hints,
                           struct addrinfo **out)
{
    struct {
        struct addrinfo    info;
        struct sockaddr_in in;
    } *one;
    uint8_t address[4];
    char *end = NULL;
    long port = service != NULL ? strtol(service, &end, 10) : 445;

    (void)hints;

    if (node == NULL || !four_numbers(node, address)) {
        return EAI_NONAME;
    }

    if ((service != NULL && (end == service || *end != '\0'))
        || port <= 0 || port > 65535) {
        return EAI_SERVICE;
    }

    one = calloc(1, sizeof(*one));

    if (one == NULL) {
        return EAI_MEMORY;
    }

    one->in.sin_family = AF_INET;
    one->in.sin_port   = htons((uint16_t)port);
    memcpy(&one->in.sin_addr.s_addr, address, 4);

    one->info.ai_family   = AF_INET;
    one->info.ai_socktype = SOCK_STREAM;
    one->info.ai_protocol = IPPROTO_TCP;
    one->info.ai_addrlen  = sizeof(one->in);
    one->info.ai_addr     = (struct sockaddr *)(void *)&one->in;

    *out = &one->info;
    return 0;
}

void smb_kosmos_freeaddrinfo(struct addrinfo *list)
{
    free(list);                 /* one allocation holds both halves */
}

int smb_kosmos_refused(void)
{
    errno = ENOSYS;
    return -1;
}

/* No account name to offer: the caller always names one (`smbfs.c`). */
int smb_kosmos_getlogin_r(char *buf, size_t size)
{
    (void)buf; (void)size;

    return -1;
}

int smb_kosmos_gethostname(char *buf, size_t size)
{
    if (size == 0) {
        return -1;
    }

    strncpy(buf, "kosmos", size - 1);
    buf[size - 1] = '\0';
    return 0;
}

int smb_kosmos_asprintf(char **out, const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);

    if (n < 0 || (*out = malloc((size_t)n + 1)) == NULL) {
        return -1;
    }

    va_start(ap, format);
    (void)vsnprintf(*out, (size_t)n + 1, format, ap);
    va_end(ap);
    return n;
}

/*
 * **The synchronous API is not in this build** (`sync.c`, a `poll` loop):
 * libsmb2.c's own synchronous helper for change notification names two of
 * its functions, so they are here as refusals - nothing in Kosmos calls that
 * helper, and a server here never waits in a call it makes.
 */
struct smb2fh *smb2_open(struct smb2_context *smb2, const char *path, int flags)
{
    (void)path; (void)flags;

    smb2_set_error(smb2, "the synchronous API is not in Kosmos's libsmb2");
    return NULL;
}

int smb2_close(struct smb2_context *smb2, struct smb2fh *fh)
{
    (void)fh;

    smb2_set_error(smb2, "the synchronous API is not in Kosmos's libsmb2");
    return -ENOSYS;
}
