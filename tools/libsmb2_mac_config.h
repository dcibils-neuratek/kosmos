/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * libsmb2's `config.h` for building it on this Mac (`docs/sharing.md` step
 * N0): what its cmake would have found, written out because the build has no
 * cmake and needs none. Used only for the host's copy that
 * `tools/test_smbpeer.py` holds to the peer; the machine's build is the
 * port's own (step N2).
 */
#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_LEN 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_ADDRINFO 1
#define HAVE_STRUCT_IOVEC 1
#define HAVE_LINGER 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_ERRNO_H 1
#define STDC_HEADERS 1
#define _U_ __attribute__((unused))
