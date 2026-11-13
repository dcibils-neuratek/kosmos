/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ERRNO_H
#define ERRNO_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/*
 * errno is a function, not a variable, and that is deliberate.
 *
 * `design.md` §17.3 calls this out as a detail that causes bugs months
 * later: a global errno does not survive coroutines, or threads. Because it
 * was an accessor from the start, its storage could move - into each
 * process, and then into each thread's own block (`user/init/misc_user.c`)
 * - without a single caller changing.
 *
 * The maths library is musl's, which sets no errno at all; what sets it here
 * is the libc's own I/O, and the ports that read it back by name.
 */
int *__errno(void);

#define errno   (*__errno())

#define EDOM    33
#define ERANGE  34


/*
 * The two a port asks about by name.
 *
 * `EISDIR` is how Doom decides a failed `fopen` means "that is a directory,
 * so it exists" - the only place it inspects errno rather than a return
 * value. `ENOTDIR` is its mirror and comes with it so the pair is not half
 * defined.
 */
#ifndef EISDIR
#define EISDIR   21
#endif
#ifndef ENOTDIR
#define ENOTDIR  20
#endif

/*
 * And the three Quake's `com_stdio.c` sets on a bad stream: `EBADF` for a
 * null handle, `EFAULT` for a null buffer, `EINVAL` for a seek it will not
 * do. Linux's numbers, like the rest.
 */
#ifndef EBADF
#define EBADF    9
#endif
#ifndef EFAULT
#define EFAULT   14
#endif
#ifndef EINVAL
#define EINVAL   22
#endif

/*
 * And the rest of the set FFmpeg names, all at once, because it names them
 * all at once: `libavutil/error.c` keeps a table of every errno it knows a
 * sentence for, and a decoder answers `AVERROR(ENOMEM)` or
 * `AVERROR(EAGAIN)` - which is `-ENOMEM` - on the way out of nearly every
 * function. The numbers are only compared, never shown to a person as
 * numbers, and they are Linux's so that one read against FFmpeg's own
 * documentation says the same thing.
 *
 * Nothing in Kosmos sets any of these. They are names a vendored library
 * returns to its own caller, inside one process.
 */
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define ENXIO         6
#define E2BIG         7
#define ENOEXEC       8
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EMLINK       31
#define EPIPE        32
#define EDEADLK      35
#define ENAMETOOLONG 36
#define ENOLCK       37
#define ENOSYS       38
#define ENOTEMPTY    39
#define EILSEQ       84
#define ETIMEDOUT    85     /* a wait with a deadline that came (W3a) */

/* The network's, for the POSIX layer's sockets (`docs/w3.md` W3e), with
 * Linux's numbers like the rest. */
#define EWOULDBLOCK      EAGAIN
#define EPROTO           71     /* a peer broke the protocol (OpenSSL, W5d) */
#define EOVERFLOW        75     /* a count past its largest (sem_post, W5e) */
#define ENOTSOCK         88
#define EDESTADDRREQ     89
#define EMSGSIZE         90
#define EPROTOTYPE       91
#define ENOPROTOOPT      92
#define EPROTONOSUPPORT  93
#define EOPNOTSUPP       95
#define ENOTSUP          EOPNOTSUPP
#define EAFNOSUPPORT     97
#define EADDRINUSE       98
#define EADDRNOTAVAIL    99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ECONNABORTED    103
#define ECONNRESET      104
#define EISCONN         106
#define ENOTCONN        107
#define ECONNREFUSED    111
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115

/* The largest number above, under BSD's name: C++'s library asks it, to
 * know which numbers `strerror` has a sentence for. A new code above it
 * moves it. */
#define ELAST        EINPROGRESS

__END_DECLS

#endif /* ERRNO_H */
