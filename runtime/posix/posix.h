/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_INTERNAL_H
#define POSIX_INTERNAL_H

/*
 * What the POSIX layer's own files share (`docs/w3.md`), and nothing a
 * port sees: it is not in `runtime/posix/include/`.
 */

/* The scheduler's ticks a second - every wait's unit - from `SYS_SYSINFO`,
 * asked once (`time.c`). */
unsigned long posix_tick_hz(void);

/* Ticks for `ns` nanoseconds, rounded up and at least one when `ns` is
 * above nought: a wait is never shorter than it was asked to be. */
unsigned long posix_ticks_for_ns(long long ns);

/*
 * Sockets and the descriptor table (W3e): `socket.c` makes one and
 * `fd.c` holds it. A socket in the table is closed by `fd.c` when its last
 * descriptor goes, and read and written through it.
 */
struct posix_socket;

/* A descriptor for `s`, `flags` its O_NONBLOCK and O_CLOEXEC; or -1 and
 * errno. */
int posix_fd_socket_new(struct posix_socket *s, int flags);

/* The socket behind `fd`, and its descriptor's flags; NULL for anything
 * else, with errno untouched. */
struct posix_socket *posix_fd_socket(int fd, int *flags);

/* Whether `fd` is open: the console, a file or a socket. */
int posix_fd_open(int fd);

void    posix_socket_close(struct posix_socket *s);
long    posix_socket_recv(struct posix_socket *s, void *buf, unsigned long bytes, int nonblock);
long    posix_socket_send(struct posix_socket *s, const void *buf, unsigned long bytes, int nonblock);

#endif /* POSIX_INTERNAL_H */
