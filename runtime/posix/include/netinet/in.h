/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef NETINET_IN_H
#define NETINET_IN_H

/* IPv4's addresses, as Linux lays them out (`sys/socket.h` says why). */

#include <stdint.h>
#include <sys/cdefs.h>
#include <sys/socket.h>

__BEGIN_DECLS

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr {
    in_addr_t s_addr;                   /* network order */
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;            /* network order */
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t     sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

#define IPPROTO_IP    0
#define IPPROTO_TCP   6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

__END_DECLS

#endif /* NETINET_IN_H */
