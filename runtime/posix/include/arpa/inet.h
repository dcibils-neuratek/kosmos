/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ARPA_INET_H
#define ARPA_INET_H

#include <netinet/in.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

int         inet_pton(int family, const char *text, void *out);
const char *inet_ntop(int family, const void *address, char *out, socklen_t room);
in_addr_t   inet_addr(const char *text);
int         inet_aton(const char *text, struct in_addr *out);
char       *inet_ntoa(struct in_addr address);

__END_DECLS

#endif /* ARPA_INET_H */
