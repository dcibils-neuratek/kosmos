/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The sockets' checks** (`docs/w3.md` W3e), in `posixtest.elf` beside the
 * POSIX layer's and the Storage Kit's, against a web server the harness runs
 * on the host (`tools/run_loader.py`) and reached through QEMU's network:
 *
 *   - names: `getaddrinfo` on a number, refusing a name with
 *     `AI_NUMERICHOST`, and `inet_pton` / `inet_ntop` there and back;
 *   - a fetch: a blocking `connect`, a request, and 200000 bytes read to the
 *     end, every one of them the pattern the server sends;
 *   - two at once: non-blocking, `EINPROGRESS`, and `poll` over both until
 *     both are over, each body its own;
 *   - four threads, each fetching on its own socket at the same moment -
 *     the network kit's poll regions lent to all four;
 *   - a refusal: `connect` to a port nobody answers is `ECONNREFUSED`;
 *   - a timeout: `poll` on a listener nobody calls, 200 ms, answers 0;
 *   - pipes (W7b): `poll` with no deadline on a pipe, alone and beside that
 *     listener, ended by another thread's write; a blocking `read` woken;
 *     `EAGAIN`, the end, `EPIPE` and `S_IFIFO`;
 *   - and a listener: `bind`, `listen`, `poll`, `accept`, and a line each
 *     way with whoever the harness sends through QEMU's port forward;
 *   - descriptors: `read` and `write` on a socket, `fstat` says a socket,
 *     `lseek` says `ESPIPE`, `send` on a file says `ENOTSOCK`.
 *
 * `kosmos_sockettest_run` writes one sentence: every check's result when all
 * of them held, or the first that did not and why.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SAY(...) do { snprintf(out, size, __VA_ARGS__); return 1; } while (0)

#define BIG_BODY   200000               /* /big: (i % 251) for each byte */
#define SMALL_BODY 30000                /* /small/N: 'A' + N, every byte */

static char host[32];
static int  port;

static long long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int address(struct sockaddr_in *a, int at)
{
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)at);
    return inet_pton(AF_INET, host, &a->sin_addr) == 1 ? 0 : -1;
}

/* A connection to the server, waiting for the network to have its address
 * first: DHCP may still be answering when the program starts. */
static int connected(int at)
{
    struct sockaddr_in a;
    long long until = now_ms() + 30000;

    if (address(&a, at) != 0) {
        return -1;
    }

    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            return fd;
        }

        close(fd);

        if ((errno != ENETUNREACH && errno != ENETDOWN) || now_ms() > until) {
            return -1;
        }

        usleep(200000);
    }
}

/* Everything the far end says until it is over, after the header. */
static long body_of(int fd, unsigned char *body, long room, int use_read)
{
    long got = 0, header = -1;
    unsigned char *all = malloc((size_t)room + 256);

    if (all == NULL) {
        return -1;
    }

    for (;;) {
        long n = use_read ? (long)read(fd, all + got, (size_t)(room + 255 - got))
                          : (long)recv(fd, all + got, (size_t)(room + 255 - got), 0);

        if (n < 0) {
            free(all);
            return -2;
        }

        if (n == 0) {
            break;
        }

        got += n;
    }

    for (long i = 0; i + 3 < got; i++) {
        if (memcmp(all + i, "\r\n\r\n", 4) == 0) {
            header = i + 4;
            break;
        }
    }

    if (header < 0 || memcmp(all, "HTTP/1.0 200", 12) != 0) {
        free(all);
        return -3;
    }

    memcpy(body, all + header, (size_t)(got - header));
    free(all);
    return got - header;
}

static int ask(int fd, const char *path)
{
    char request[128];
    int n = snprintf(request, sizeof request, "GET %s HTTP/1.0\r\nHost: k\r\n\r\n", path);

    return send(fd, request, (size_t)n, MSG_NOSIGNAL) == n ? 0 : -1;
}

/*
 *--------------------------------------------------------------------------
 * Four threads, each a fetch of its own.
 */

struct fetch {
    int  which;
    long got;
    int  error;
    int  right;
};

static void *fetch_one(void *arg)
{
    struct fetch *f = arg;
    unsigned char *body = malloc(SMALL_BODY + 16);
    char path[32];
    int fd;
    long i;

    if (body == NULL) {
        f->error = ENOMEM;
        return NULL;
    }

    snprintf(path, sizeof path, "/small/%d", f->which);

    if ((fd = connected(port)) < 0 || ask(fd, path) != 0) {
        f->error = errno;
    } else {
        f->got = body_of(fd, body, SMALL_BODY + 16, 0);
        f->right = f->got == SMALL_BODY;

        for (i = 0; f->right && i < f->got; i++) {
            f->right = body[i] == (unsigned char)('A' + f->which);
        }
    }

    if (fd >= 0) {
        close(fd);
    }

    free(body);
    return NULL;
}

/*
 *--------------------------------------------------------------------------
 */

/* A byte down the pipe, after 100 ms: for a thread waiting on it. */
static void *write_later(void *fd)
{
    struct timespec t = { 0, 100 * 1000000L };

    nanosleep(&t, NULL);
    (void)write(*(int *)fd, "w", 1);
    return NULL;
}

int kosmos_sockettest_run(char *out, unsigned long size, const char *to, int at,
                          int refused_at, int listen_at)
{
    struct addrinfo hints, *found = NULL;
    struct sockaddr_in a;
    unsigned char *body = NULL;
    char text[INET_ADDRSTRLEN];
    int listener = -1, quiet = -1, fd = -1, r, rounds = 0;
    long got, i;

    snprintf(host, sizeof host, "%s", to);
    port = at;

    /* Names. */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char service[8];
    snprintf(service, sizeof service, "%d", at);

    if ((r = getaddrinfo(host, service, &hints, &found)) != 0) {
        SAY("sockets: getaddrinfo(%s) said %d (%s)", host, r, gai_strerror(r));
    }

    if (found->ai_family != AF_INET
        || ntohs(((struct sockaddr_in *)found->ai_addr)->sin_port) != at
        || inet_ntop(AF_INET, &((struct sockaddr_in *)found->ai_addr)->sin_addr,
                     text, sizeof text) == NULL
        || strcmp(text, host) != 0) {
        SAY("sockets: getaddrinfo(%s, %s) gave %s", host, service, text);
    }

    freeaddrinfo(found);
    hints.ai_flags = AI_NUMERICHOST;

    if ((r = getaddrinfo("kosmos.invalid", "80", &hints, &found)) != EAI_NONAME) {
        SAY("sockets: a name with AI_NUMERICHOST was %d, not EAI_NONAME", r);
    }

    /* The listener first, so whoever the harness sends finds it there. */
    listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)listen_at);

    if (listener < 0 || bind(listener, (struct sockaddr *)&a, sizeof a) != 0) {
        SAY("sockets: bind to %d: errno %d", listen_at, errno);
    }

    /* The fetch: blocking, to the end, and read(2) rather than recv. */
    if ((body = malloc(BIG_BODY + 64)) == NULL) {
        SAY("sockets: no memory");
    }

    if ((fd = connected(at)) < 0) {
        SAY("sockets: connect to %s:%d: errno %d", host, at, errno);
    }

    if (listen(listener, 4) != 0) {
        SAY("sockets: listen on %d: errno %d", listen_at, errno);
    }

    {
        struct stat st;
        struct sockaddr_in peer;
        socklen_t length = sizeof peer;

        if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) {
            SAY("sockets: fstat did not say a socket");
        }

        if (lseek(fd, 0, SEEK_SET) != -1 || errno != ESPIPE) {
            SAY("sockets: lseek on a socket was not ESPIPE (errno %d)", errno);
        }

        if (getpeername(fd, (struct sockaddr *)&peer, &length) != 0
            || ntohs(peer.sin_port) != at) {
            SAY("sockets: getpeername did not give port %d", at);
        }
    }

    if (write(fd, "GET /big HTTP/1.0\r\n\r\n", 21) != 21) {
        SAY("sockets: write of the request: errno %d", errno);
    }

    got = body_of(fd, body, BIG_BODY + 64, 1);
    close(fd);
    fd = -1;

    if (got != BIG_BODY) {
        SAY("sockets: the fetch read %ld bytes of %d", got, BIG_BODY);
    }

    for (i = 0; i < got; i++) {
        if (body[i] != (unsigned char)(i % 251)) {
            SAY("sockets: byte %ld of the fetch was %u, not %u", i, body[i],
                (unsigned)(i % 251));
        }
    }

    /* Two at once, under poll. */
    {
        int two[2];
        unsigned char *bodies[2];
        long have[2] = { 0, 0 };
        int over[2] = { 0, 0 }, asked[2] = { 0, 0 };
        long long until = now_ms() + 60000;

        if (address(&a, at) != 0) {
            SAY("sockets: the address");
        }

        for (i = 0; i < 2; i++) {
            bodies[i] = malloc(SMALL_BODY + 256);
            two[i] = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

            if (bodies[i] == NULL || two[i] < 0) {
                SAY("sockets: two sockets");
            }

            if (connect(two[i], (struct sockaddr *)&a, sizeof a) != -1 || errno != EINPROGRESS) {
                SAY("sockets: a non-blocking connect was not EINPROGRESS (errno %d)", errno);
            }

            if (recv(two[i], bodies[i], 16, 0) != -1 || errno != EAGAIN) {
                SAY("sockets: recv with nothing there was not EAGAIN (errno %d)", errno);
            }
        }

        while (!(over[0] && over[1])) {
            struct pollfd set[2];

            for (i = 0; i < 2; i++) {
                set[i].fd = over[i] ? -1 : two[i];
                set[i].events = asked[i] ? POLLIN : POLLOUT;
            }

            if ((r = poll(set, 2, 10000)) <= 0) {
                SAY("sockets: poll over two said %d (errno %d) after %d rounds", r, errno, rounds);
            }

            rounds++;

            for (i = 0; i < 2; i++) {
                if (set[i].revents & POLLOUT) {
                    char path[16];

                    snprintf(path, sizeof path, "/small/%ld", i + 4);

                    if (ask(two[i], path) != 0) {
                        SAY("sockets: a request under poll: errno %d", errno);
                    }

                    asked[i] = 1;
                } else if (set[i].revents & (POLLIN | POLLHUP)) {
                    long n = recv(two[i], bodies[i] + have[i],
                                  (size_t)(SMALL_BODY + 256 - have[i]), 0);

                    if (n < 0 && errno != EAGAIN) {
                        SAY("sockets: recv under poll: errno %d", errno);
                    }

                    if (n == 0) {
                        over[i] = 1;
                    } else if (n > 0) {
                        have[i] += n;
                    }
                }
            }

            if (now_ms() > until) {
                SAY("sockets: two under poll took a minute");
            }
        }

        for (i = 0; i < 2; i++) {
            unsigned char *b = bodies[i];
            long n = have[i], h = -1, k;

            for (k = 0; k + 3 < n; k++) {
                if (memcmp(b + k, "\r\n\r\n", 4) == 0) {
                    h = k + 4;
                    break;
                }
            }

            if (h < 0 || n - h != SMALL_BODY) {
                SAY("sockets: connection %ld under poll read %ld bytes", i, h < 0 ? -1 : n - h);
            }

            for (k = h; k < n; k++) {
                if (b[k] != (unsigned char)('A' + 4 + i)) {
                    SAY("sockets: connection %ld under poll read another's bytes", i);
                }
            }

            close(two[i]);
            free(bodies[i]);
        }
    }

    /* Four threads. */
    {
        pthread_t t[4];
        struct fetch f[4];

        for (i = 0; i < 4; i++) {
            memset(&f[i], 0, sizeof f[i]);
            f[i].which = (int)i;

            if (pthread_create(&t[i], NULL, fetch_one, &f[i]) != 0) {
                SAY("sockets: thread %ld", i);
            }
        }

        for (i = 0; i < 4; i++) {
            pthread_join(t[i], NULL);
        }

        for (i = 0; i < 4; i++) {
            if (!f[i].right) {
                SAY("sockets: thread %ld read %ld (errno %d)", i, f[i].got, f[i].error);
            }
        }
    }

    /* A refusal. */
    if (address(&a, refused_at) != 0 || (fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        SAY("sockets: a socket for the refusal");
    }

    if (connect(fd, (struct sockaddr *)&a, sizeof a) != -1 || errno != ECONNREFUSED) {
        SAY("sockets: a port nobody answers was errno %d, not ECONNREFUSED", errno);
    }

    if (recv(fd, body, 4, 0) != -1 || errno != ENOTCONN) {
        SAY("sockets: recv on what never connected was errno %d, not ENOTCONN", errno);
    }

    close(fd);
    fd = -1;

    {
        int file = open("/Temporary/sockettest", O_RDWR | O_CREAT | O_TRUNC, 0644);

        if (file < 0 || send(file, "x", 1, 0) != -1 || errno != ENOTSOCK) {
            SAY("sockets: send on a file was errno %d, not ENOTSOCK", errno);
        }

        close(file);
        unlink("/Temporary/sockettest");
    }

    /* A timeout: a listener nobody calls. */
    {
        struct pollfd one = { -1, POLLIN, 0 };
        long long began;

        quiet = socket(AF_INET, SOCK_STREAM, 0);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)(listen_at + 1));
        a.sin_addr.s_addr = INADDR_ANY;

        if (quiet < 0 || bind(quiet, (struct sockaddr *)&a, sizeof a) != 0
            || listen(quiet, 1) != 0) {
            SAY("sockets: a quiet listener: errno %d", errno);
        }

        one.fd = quiet;
        began = now_ms();
        r = poll(&one, 1, 200);

        if (r != 0 || now_ms() - began < 190) {
            SAY("sockets: poll on nobody said %d after %lld ms", r, now_ms() - began);
        }

        /* A pipe (W7b): `poll` with no deadline, on the pipe alone and
         * then beside this listener - where the stack holds the wait, as
         * curl's thread is held - is ended by a write from another thread.
         * A wake that does not arrive is the deadline, 3 s, and says so. */
        {
            int fds[2], rounds;
            struct stat st;
            char c = 0;

            if (pipe(fds) != 0 || fstat(fds[0], &st) != 0 || !S_ISFIFO(st.st_mode)) {
                SAY("pipes: pipe or fstat: errno %d", errno);
            }

            for (rounds = 0; rounds < 2; rounds++) {
                struct pollfd both[2] = { { fds[0], POLLIN, 0 }, { quiet, POLLIN, 0 } };
                pthread_t writer;
                long long waited;

                if (pthread_create(&writer, NULL, write_later, &fds[1]) != 0) {
                    SAY("pipes: pthread_create");
                }

                began = now_ms();
                r = poll(both, rounds == 0 ? 1 : 2, 3000);
                waited = now_ms() - began;
                pthread_join(writer, NULL);

                if (r != 1 || !(both[0].revents & POLLIN) || waited < 90 || waited > 2000) {
                    SAY("pipes: poll on a pipe%s said %d (%#x) after %lld ms",
                        rounds ? " beside a listener" : "", r, both[0].revents, waited);
                }

                if (read(fds[0], &c, 1) != 1 || c != 'w') {
                    SAY("pipes: read after poll: errno %d", errno);
                }
            }

            /* Blocking read, woken; then what an empty, a finished and a
             * readerless pipe say. */
            {
                pthread_t writer;

                if (pthread_create(&writer, NULL, write_later, &fds[1]) != 0
                    || read(fds[0], &c, 1) != 1 || c != 'w') {
                    SAY("pipes: a blocking read: errno %d", errno);
                }

                pthread_join(writer, NULL);
            }

            fcntl(fds[0], F_SETFL, O_NONBLOCK);

            if (read(fds[0], &c, 1) != -1 || errno != EAGAIN) {
                SAY("pipes: an empty pipe was errno %d, not EAGAIN", errno);
            }

            if (write(fds[1], "end", 3) != 3 || close(fds[1]) != 0) {
                SAY("pipes: write and close: errno %d", errno);
            }

            {
                char got[8];
                struct pollfd hup = { fds[0], POLLIN, 0 };

                if (read(fds[0], got, sizeof got) != 3 || memcmp(got, "end", 3) != 0
                    || poll(&hup, 1, 0) != 1 || !(hup.revents & POLLHUP)
                    || read(fds[0], got, sizeof got) != 0) {
                    SAY("pipes: the bytes before the end, then the end, not said");
                }
            }

            close(fds[0]);

            if (pipe(fds) != 0 || close(fds[0]) != 0
                || write(fds[1], "x", 1) != -1 || errno != EPIPE) {
                SAY("pipes: a write with no reader was errno %d, not EPIPE", errno);
            }

            close(fds[1]);
        }

        close(quiet);
    }

    /* The listener, and whoever the harness sent. */
    {
        struct pollfd one = { listener, POLLIN, 0 };
        struct sockaddr_in from;
        socklen_t length = sizeof from;
        char line[8];
        long n, have = 0;
        int answered;

        if ((r = poll(&one, 1, 60000)) != 1 || !(one.revents & POLLIN)) {
            SAY("sockets: nobody arrived on %d (poll %d)", listen_at, r);
        }

        if ((answered = accept(listener, (struct sockaddr *)&from, &length)) < 0) {
            SAY("sockets: accept: errno %d", errno);
        }

        while (have < 5 && (n = recv(answered, line + have, (size_t)(5 - have), 0)) > 0) {
            have += n;
        }

        if (have != 5 || memcmp(line, "ping\n", 5) != 0) {
            SAY("sockets: the caller said %ld bytes, not ping", have);
        }

        if (send(answered, "pong\n", 5, 0) != 5 || shutdown(answered, SHUT_WR) != 0) {
            SAY("sockets: the answer: errno %d", errno);
        }

        close(answered);
        close(listener);
    }

    free(body);
    snprintf(out, size, "sockets: names, fetched %d, two under poll, four threads, "
             "refused, timed out, pipes woken alone and beside a listener, accepted, "
             "descriptors", BIG_BODY);
    return 0;
}
