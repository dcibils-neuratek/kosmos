/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A pipe, inside one process** (`docs/w7.md` W7b, `docs/w3.md`).
 *
 * Ported code talks to its own threads down a pipe: curl's thread sleeps in
 * `poll` with no deadline, and the thread that hands it a transfer writes a
 * byte to wake it (`curl_multi_wakeup`). The POSIX layer had none, so curl
 * was built without the wake-up, and WebKit's network process fetched one
 * page and never the font it named.
 *
 * A pipe never leaves the process - no descriptor does here (*Compatibility
 * inside a process*) - so it is 64 KB of bytes and two counts behind a
 * lock, and a descriptor for each end in `fd.c`'s table.
 *
 * **Waiting is the whole of it.** A reader on an empty pipe, a writer on a
 * full one and `poll` on either wait for *something to change*, and every
 * change - bytes in, bytes out, an end closed - bumps one word for the
 * process and wakes everyone on it (a futex), then marks every poll parked
 * in the network stack and asks the stack to look again
 * (`net_client_wake`), since a `poll` with sockets in it is waiting there
 * rather than on the word. A waiter looks again and waits again if its own
 * pipe did not change. That wakes more than it needs to, and costs one
 * word: there are a handful of pipes in a process, and changes are rare
 * next to the bytes they carry.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kosmos.h"
#include "kits/network/netclient.h"
#include "posix.h"

#define PIPE_BYTES (64u * 1024u)            /* Linux's, as a pipe's room */

struct posix_pipe {
    kosmos_lock_t lock;
    unsigned char *bytes;
    uint64_t read, written;                 /* counts; their difference is in it */
    int readers, writers;                   /* ends still open */
};

/* Bumped at every change to any pipe in this process. */
static uint32_t changed;

static int fails(int e)
{
    errno = e;
    return -1;
}

uint32_t posix_pipe_generation(void)
{
    return __atomic_load_n(&changed, __ATOMIC_ACQUIRE);
}

/* Something about a pipe changed: whoever waits on one looks again. */
static void wake(void)
{
    __atomic_add_fetch(&changed, 1, __ATOMIC_SEQ_CST);
    (void)kosmos_futex_wake(&changed, ~0UL >> 1);

    if (net_client_cap() >= 0) {
        net_client_wake();
    }
}

int posix_pipe_wait(uint32_t seen, long long until_ns)
{
    unsigned long ticks = 0;

    if (until_ns != 0) {
        struct timespec now;
        long long left;

        clock_gettime(CLOCK_MONOTONIC, &now);
        left = until_ns - ((long long)now.tv_sec * 1000000000LL + now.tv_nsec);

        if (left <= 0) {
            return 0;
        }

        ticks = posix_ticks_for_ns(left);
    }

    (void)kosmos_futex_wait(&changed, seen, ticks);
    return 1;
}

int pipe2(int fds[2], int flags)
{
    struct posix_pipe *p;

    if (fds == NULL) {
        return fails(EFAULT);
    }

    if ((flags & ~(O_NONBLOCK | O_CLOEXEC)) != 0) {
        return fails(EINVAL);
    }

    p = calloc(1, sizeof(*p));

    if (p == NULL || (p->bytes = malloc(PIPE_BYTES)) == NULL) {
        free(p);
        return fails(ENOMEM);
    }

    p->readers = p->writers = 1;

    if ((fds[0] = posix_fd_pipe_new(p, 0, flags)) < 0) {
        free(p->bytes);
        free(p);
        return -1;
    }

    if ((fds[1] = posix_fd_pipe_new(p, 1, flags)) < 0) {
        int e = errno;

        (void)close(fds[0]);                /* the reader's end */
        posix_pipe_close(p, 1);             /* and the writer's, which frees it */
        errno = e;
        return -1;
    }

    return 0;
}

int pipe(int fds[2])
{
    return pipe2(fds, 0);
}

void posix_pipe_close(struct posix_pipe *p, int end)
{
    int gone;

    kosmos_lock(&p->lock);

    if (end && p->writers > 0) {
        p->writers--;
    } else if (!end && p->readers > 0) {
        p->readers--;
    }

    gone = p->readers == 0 && p->writers == 0;
    kosmos_unlock(&p->lock);

    if (gone) {
        free(p->bytes);
        free(p);
    }

    wake();
}

long posix_pipe_read(struct posix_pipe *p, void *buf, unsigned long bytes, int nonblock)
{
    for (;;) {
        uint32_t seen = posix_pipe_generation();
        uint64_t have;

        kosmos_lock(&p->lock);
        have = p->written - p->read;

        if (have > 0 || bytes == 0) {
            unsigned long n = have < bytes ? (unsigned long)have : bytes;
            unsigned long i;

            for (i = 0; i < n; i++) {
                ((unsigned char *)buf)[i] = p->bytes[(p->read + i) % PIPE_BYTES];
            }

            p->read += n;
            kosmos_unlock(&p->lock);

            if (n > 0) {
                wake();
            }

            return (long)n;
        }

        if (p->writers == 0) {
            kosmos_unlock(&p->lock);
            return 0;                       /* the end of it */
        }

        kosmos_unlock(&p->lock);

        if (nonblock) {
            return fails(EAGAIN);
        }

        (void)posix_pipe_wait(seen, 0);
    }
}

long posix_pipe_write(struct posix_pipe *p, const void *buf, unsigned long bytes, int nonblock)
{
    unsigned long done = 0;

    while (done < bytes) {
        uint32_t seen = posix_pipe_generation();
        uint64_t room;

        kosmos_lock(&p->lock);

        if (p->readers == 0) {
            kosmos_unlock(&p->lock);
            return done > 0 ? (long)done : fails(EPIPE);
        }

        room = PIPE_BYTES - (p->written - p->read);

        if (room > 0) {
            unsigned long n = bytes - done < room ? bytes - done : (unsigned long)room;
            unsigned long i;

            for (i = 0; i < n; i++) {
                p->bytes[(p->written + i) % PIPE_BYTES] = ((const unsigned char *)buf)[done + i];
            }

            p->written += n;
            done += n;
            kosmos_unlock(&p->lock);
            wake();
            continue;
        }

        kosmos_unlock(&p->lock);

        if (nonblock) {
            return done > 0 ? (long)done : fails(EAGAIN);
        }

        (void)posix_pipe_wait(seen, 0);
    }

    return (long)done;
}

short posix_pipe_now(struct posix_pipe *p, int end, short events)
{
    short got = 0;

    kosmos_lock(&p->lock);

    if (!end) {
        if (p->written != p->read) {
            got |= (short)(events & (POLLIN | POLLRDNORM));
        }

        if (p->writers == 0) {
            got |= POLLHUP | (short)(events & (POLLIN | POLLRDNORM));
        }
    } else if (p->readers == 0) {
        got |= POLLERR;
    } else if (p->written - p->read < PIPE_BYTES) {
        got |= (short)(events & (POLLOUT | POLLWRNORM));
    }

    kosmos_unlock(&p->lock);
    return got;
}
