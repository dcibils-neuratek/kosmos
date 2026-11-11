/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef NETINET_TCP_H
#define NETINET_TCP_H

/* Taken and kept: the stack decides when a segment goes (`net.c`). */
#define TCP_NODELAY    1
#define TCP_KEEPIDLE   4
#define TCP_KEEPINTVL  5
#define TCP_KEEPCNT    6

#endif /* NETINET_TCP_H */
