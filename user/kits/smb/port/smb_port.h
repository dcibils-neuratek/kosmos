/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The platform libsmb2 runs on in Kosmos: `__KOSMOS__`, as the PS2, the Pico
 * W and the Switch each are one (`docs/sharing.md`, *What the Kosmos port
 * patches*, step N2).
 *
 * **Compatibility inside a process, and nowhere else** (`CLAUDE.md`;
 * `design.md` 17.2). libsmb2 was written against sockets - `socket`,
 * `connect`, `readv`, `writev`, `getaddrinfo` - and Kosmos has none: a
 * connection is a region of two rings the network stack hands back
 * (`tcpring.h`) and the messages that say something happened
 * (`netproto.h`). So the names libsmb2 calls are given here, **as macros
 * onto `smb_kosmos_*`**, and implemented over the ring in
 * `user/kits/smb/smb_transport.c`. They exist in the SMB Kit's build and in
 * no other: no program anywhere is given a `socket()`, and one that wrote
 * `connect` would find it undefined.
 *
 * A socket here is an index into the kit's table of connections - `t_socket`
 * is `int`, as libsmb2's own default is - and the table grows, as `net.c`'s
 * do.
 *
 * **What libsmb2 may not have**, and so is given as a refusal: `bind`,
 * `listen`, `accept`, `poll` and `select`, which only `smb2_bind_and_listen`,
 * `smb2_accept_connection_async` and the synchronous API reach. Those are in
 * `socket.c` with everything else and cannot leave the build without an
 * edit; they are compiled against calls that answer "not here" and nothing
 * in Kosmos calls them. The server side (step N12) gets a listener of its
 * own on `NET_OP_LISTEN` when its time comes.
 *
 * And **credentials from a file named by an environment variable**
 * (`NTLM_USER_FILE`): `getenv` is no environment at all for libsmb2, so the
 * branch that would read one is dead code the compiler drops.
 */
#ifndef KOSMOS_SMB_PORT_H
#define KOSMOS_SMB_PORT_H

#define __KOSMOS__ 1

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

typedef long ssize_t;
typedef uint32_t socklen_t;
typedef unsigned short sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

/* The errors libsmb2 names that `runtime/include/errno.h` does not, with
 * Linux's numbers; only their being distinct matters. */
#ifndef EWOULDBLOCK
#define EWOULDBLOCK   EAGAIN
#endif
#ifndef EINPROGRESS
#define EINPROGRESS   115
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED  111
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT     110
#endif
#ifndef ECONNRESET
#define ECONNRESET    104
#endif
#ifndef ENETRESET
#define ENETRESET     102
#endif
#ifndef ENETUNREACH
#define ENETUNREACH   101
#endif
#ifndef ENETDOWN
#define ENETDOWN      100
#endif
#ifndef EADDRINUSE
#define EADDRINUSE     98
#endif
#ifndef ENOTCONN
#define ENOTCONN      107
#endif
#ifndef ENODATA
#define ENODATA        61
#endif
#ifndef ENOLINK
#define ENOLINK        67
#endif
#ifndef ELOOP
#define ELOOP          40
#endif
#ifndef ETXTBSY
#define ETXTBSY        26
#endif
#ifndef ECANCELED
#define ECANCELED     125
#endif

/* Addresses: IPv4, which is what the stack speaks. No `AF_INET6`, so
 * libsmb2's IPv6 branches are not compiled. */
#define AF_INET        2
#define PF_INET        AF_INET
#define SOCK_STREAM    1
#define IPPROTO_TCP    6
#define SOL_TCP        IPPROTO_TCP
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_ERROR       4
#define SO_LINGER     13
#define TCP_NODELAY    1
#define F_GETFL        3
#define F_SETFL        4
#define O_NONBLOCK  04000

#define INADDR_ANY     0u

/* How libsmb2 says what an open should do, translated into SMB's CREATE by
 * libsmb2 itself; Linux's numbers, as `compat.h` gives the access modes. */
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC    01000

#define EAI_AGAIN      (-3)
#define EAI_FAIL       (-4)
#define EAI_MEMORY     (-10)
#define EAI_NONAME     (-2)
#define EAI_SERVICE    (-8)

struct in_addr {
    in_addr_t s_addr;               /* in the order it goes on the wire */
};

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;        /* in the order it goes on the wire */
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        ss_pad[126];
};

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

struct linger {
    int l_onoff;
    int l_linger;
};

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010

struct pollfd {
    int   fd;
    short events;
    short revents;
};

static inline uint16_t smb_kosmos_swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

#define htons(v)   smb_kosmos_swap16((uint16_t)(v))
#define ntohs(v)   smb_kosmos_swap16((uint16_t)(v))
#define htonl(v)   __builtin_bswap32((uint32_t)(v))
#define ntohl(v)   __builtin_bswap32((uint32_t)(v))

/* The connection, over the ring (`smb_transport.c`). */
int     smb_kosmos_socket(int family, int type, int protocol);
int     smb_kosmos_connect(int fd, const struct sockaddr *to, socklen_t len);
int     smb_kosmos_close(int fd);
int     smb_kosmos_getsockopt(int fd, int level, int name, void *value,
                              socklen_t *len);
int     smb_kosmos_setsockopt(int fd, int level, int name, const void *value,
                              socklen_t len);
int     smb_kosmos_fcntl(int fd, int command, ...);
ssize_t smb_kosmos_readv(int fd, const struct iovec *iov, int count);
ssize_t smb_kosmos_writev(int fd, const struct iovec *iov, int count);
int     smb_kosmos_getaddrinfo(const char *node, const char *service,
                               const struct addrinfo *hints,
                               struct addrinfo **out);
void    smb_kosmos_freeaddrinfo(struct addrinfo *list);

/* What is not here: answered "no", and never reached in Kosmos. */
int     smb_kosmos_refused(void);

/* The process's own facts, which libsmb2 uses for a default account name
 * and to seed a fallback this build does not compile. */
int     smb_kosmos_getlogin_r(char *buf, size_t size);
int     smb_kosmos_gethostname(char *buf, size_t size);

/* `asprintf`, which libsmb2 builds a share's UNC name with and the runtime
 * does not have: `vsnprintf` twice, into `malloc`. */
int     smb_kosmos_asprintf(char **out, const char *format, ...)
            __attribute__((format(printf, 2, 3)));

#define socket(f, t, p)              smb_kosmos_socket((f), (t), (p))
#define connect(fd, to, len)         smb_kosmos_connect((fd), (to), (len))
#define close(fd)                    smb_kosmos_close(fd)
#define getsockopt(fd, l, n, v, len) smb_kosmos_getsockopt((fd), (l), (n), (v), (len))
#define setsockopt(fd, l, n, v, len) smb_kosmos_setsockopt((fd), (l), (n), (v), (len))
#define fcntl                        smb_kosmos_fcntl
#define readv(fd, iov, n)            smb_kosmos_readv((fd), (iov), (n))
#define writev(fd, iov, n)           smb_kosmos_writev((fd), (iov), (n))
#define getaddrinfo(n, s, h, out)    smb_kosmos_getaddrinfo((n), (s), (h), (out))
#define freeaddrinfo(list)           smb_kosmos_freeaddrinfo(list)
#define bind(fd, a, l)               ((void)(fd), (void)(a), (void)(l), smb_kosmos_refused())
#define listen(fd, n)                ((void)(fd), (void)(n), smb_kosmos_refused())
#define accept(fd, a, l)             ((void)(fd), (void)(a), (void)(l), smb_kosmos_refused())
#define poll(set, n, t)              ((void)(set), (void)(n), (void)(t), smb_kosmos_refused())
#define select(n, r, w, e, t)        ((void)(n), (void)(r), (void)(w), (void)(e), (void)(t), \
                                      smb_kosmos_refused())
#define getlogin_r(b, n)             smb_kosmos_getlogin_r((b), (n))
#define gethostname(b, n)            smb_kosmos_gethostname((b), (n))
#define asprintf                     smb_kosmos_asprintf
#define getpid()                     0
#define srandom(seed)                ((void)(seed))
#define getenv(name)                 ((void)(name), (char *)0)

#endif /* KOSMOS_SMB_PORT_H */
