/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H

/*
 * **Sockets, for ported code** (`docs/w3.md` W3e): a socket is a descriptor
 * in this process's table, on the network kit (`netclient.c`) and the
 * `/Network` this program was given - and no other way out. IPv4 and TCP;
 * the names and the numbers are Linux's, so a port reads them as it was
 * written to. A program not given `/Network` gets `socket` and an error at
 * `connect`: what it was not handed, it cannot reach.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        ss_pad[126];
};

struct iovec;

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    size_t        msg_iovlen;
    void         *msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

/* No `AF_UNIX`: two processes here talk over endpoints and regions, and
 * `socket` takes TCP over IPv4 only. It was declared and refused, and a
 * port that sees it declared believes in `sys/un.h` - OpenSSL did
 * (`docs/w5.md` W5d) - so it is not declared. */
#define AF_UNSPEC     0
#define AF_INET       2
#define AF_INET6     10
#define PF_UNSPEC    AF_UNSPEC
#define PF_INET      AF_INET
#define PF_INET6     AF_INET6

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_NONBLOCK  04000           /* O_NONBLOCK */
#define SOCK_CLOEXEC   02000000        /* O_CLOEXEC */

#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_LINGER     13
#define SO_REUSEPORT  15
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

#define MSG_PEEK       0x02
#define MSG_DONTWAIT   0x40
#define MSG_WAITALL    0x100
#define MSG_NOSIGNAL   0x4000          /* there are no signals: always so */

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 128

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr *address, socklen_t length);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *address, socklen_t *length);
int     accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
int     connect(int fd, const struct sockaddr *address, socklen_t length);
ssize_t send(int fd, const void *buf, size_t bytes, int flags);
ssize_t recv(int fd, void *buf, size_t bytes, int flags);
ssize_t sendto(int fd, const void *buf, size_t bytes, int flags,
               const struct sockaddr *to, socklen_t length);
ssize_t recvfrom(int fd, void *buf, size_t bytes, int flags,
                 struct sockaddr *from, socklen_t *length);
int     shutdown(int fd, int how);
int     getsockopt(int fd, int level, int name, void *value, socklen_t *length);
int     setsockopt(int fd, int level, int name, const void *value, socklen_t length);
int     getsockname(int fd, struct sockaddr *address, socklen_t *length);
int     getpeername(int fd, struct sockaddr *address, socklen_t *length);

__END_DECLS

#endif /* SYS_SOCKET_H */
