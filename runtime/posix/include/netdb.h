/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef NETDB_H
#define NETDB_H

/*
 * Names to addresses (`docs/w3.md` W3e): `getaddrinfo` asks the stack's own
 * resolver (`NET_OP_RESOLVE`), so a port's lookups are the system's, with
 * its cache and its servers - and one address a name, IPv4.
 */

#include <netinet/in.h>
#include <sys/cdefs.h>
#include <sys/socket.h>

__BEGIN_DECLS

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

#define AI_PASSIVE      0x01
#define AI_CANONNAME    0x02
#define AI_NUMERICHOST  0x04
#define AI_V4MAPPED     0x08
#define AI_ALL          0x10
#define AI_ADDRCONFIG   0x20
#define AI_NUMERICSERV  0x400

#define NI_NUMERICHOST  1
#define NI_NUMERICSERV  2
#define NI_MAXHOST      1025
#define NI_MAXSERV      32

#define EAI_BADFLAGS   (-1)
#define EAI_NONAME     (-2)
#define EAI_AGAIN      (-3)
#define EAI_FAIL       (-4)
#define EAI_FAMILY     (-6)
#define EAI_SOCKTYPE   (-7)
#define EAI_SERVICE    (-8)
#define EAI_MEMORY     (-10)
#define EAI_SYSTEM     (-11)

int         getaddrinfo(const char *node, const char *service,
                        const struct addrinfo *hints, struct addrinfo **out);
void        freeaddrinfo(struct addrinfo *list);
const char *gai_strerror(int code);
int         getnameinfo(const struct sockaddr *address, socklen_t length,
                        char *host, socklen_t host_room,
                        char *service, socklen_t service_room, int flags);

__END_DECLS

#endif /* NETDB_H */
