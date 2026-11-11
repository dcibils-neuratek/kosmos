/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **`stdio` for ported code** (`docs/w3.md` W3d3): streams over the
 * layer's descriptors, in place of the runtime's, which read only what a
 * kit provided by name and wrote nothing.
 *
 * It replaces the runtime's stream functions *all together* - they are
 * weak there (`runtime/libc/stdio.c`) - because a `FILE` is each file's
 * own type: this one's is a descriptor and a buffer, the runtime's a
 * pointer into provided bytes. A provided file is still opened, when no
 * file of that name is in the namespace, so code that relied on it keeps
 * working, read only, as before.
 *
 * **Buffered, 4 KB a stream**, because a `getc` that asked a server for
 * one byte would be a message a character. One buffer holds either what
 * was read ahead or what waits to be written, never both: changing
 * direction flushes, or gives the unread bytes back to the descriptor's
 * offset, which is what the standard asks of a `fseek` between them and
 * this does for a caller who forgot. The console's streams are not
 * buffered; what is printed arrives in order with `printf`'s.
 *
 * A stream is one thread's at a time. A port that shares one between
 * threads locks it itself - POSIX's `flockfile` is not here yet, and no
 * step of the port has needed it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER 4096

enum { CONSOLE = 1, DESCRIPTOR, PROVIDED };
enum { IDLE, READING, WRITING };

struct _kosmos_file {
    int     kind;
    int     fd;
    int     readable, writable, append;
    int     eof, error;
    int     ungot;                  /* a character given back, or -1 */
    int     direction;              /* IDLE, READING, WRITING */
    char   *temporary;              /* a tmpfile's name, removed at the end */

    /* A provided file: its bytes and where the reader is. */
    const unsigned char *bytes;
    size_t  length, at;

    /* A descriptor's buffer: `start` to `end` is read ahead, or `0` to
     * `end` waits to be written. */
    size_t  start, end;
    unsigned char buffer[BUFFER];
};

static struct _kosmos_file console[3] = {
    { .kind = CONSOLE, .fd = 0, .readable = 1, .ungot = -1 },
    { .kind = CONSOLE, .fd = 1, .writable = 1, .ungot = -1 },
    { .kind = CONSOLE, .fd = 2, .writable = 1, .ungot = -1 },
};

FILE *stdin  = &console[0];
FILE *stdout = &console[1];
FILE *stderr = &console[2];

/* NULL means the console's output, as it does to the runtime. */
static FILE *out_of(FILE *f)
{
    return f != NULL ? f : stdout;
}

/*
 *--------------------------------------------------------------------------
 * The buffer.
 */

/* What waits to be written, written. 0, or EOF with the error kept. */
static int drain(FILE *f)
{
    size_t done = 0;

    if (f->direction != WRITING) {
        return 0;
    }

    while (done < f->end) {
        ssize_t n = write(f->fd, f->buffer + done, f->end - done);

        if (n <= 0) {
            f->error = 1;
            return EOF;
        }

        done += (size_t)n;
    }

    f->start = f->end = 0;
    f->direction = IDLE;
    return 0;
}

/* What was read ahead and not used, given back to the offset. */
static void unread(FILE *f)
{
    if (f->direction == READING) {
        size_t left = f->end - f->start + (f->ungot >= 0 ? 1 : 0);

        if (left > 0) {
            (void)lseek(f->fd, -(off_t)left, SEEK_CUR);
        }

        f->start = f->end = 0;
        f->ungot = -1;
        f->direction = IDLE;
    }
}

/* Ready to read: writes drained, the buffer refilled when empty. Whether
 * there is a byte. */
static int fill(FILE *f)
{
    ssize_t n;

    if (f->direction == WRITING && drain(f) != 0) {
        return 0;
    }

    f->direction = READING;

    if (f->start < f->end) {
        return 1;
    }

    n = read(f->fd, f->buffer, BUFFER);

    if (n < 0) {
        f->error = 1;
        return 0;
    }

    if (n == 0) {
        f->eof = 1;
        return 0;
    }

    f->start = 0;
    f->end = (size_t)n;
    return 1;
}

/*
 *--------------------------------------------------------------------------
 * Opening.
 */

/* `fopen`'s mode as `open`'s flags; -1 for one that is not a mode. */
static int flags_of(const char *mode, int *readable, int *writable, int *append)
{
    int flags, plus = strchr(mode, '+') != NULL;

    switch (mode[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default:  errno = EINVAL; return -1;
    }

    if (strchr(mode, 'x') != NULL && mode[0] == 'w') {
        flags |= O_EXCL;
    }

    if (strchr(mode, 'e') != NULL) {
        flags |= O_CLOEXEC;
    }

    *readable = mode[0] == 'r' || plus;
    *writable = mode[0] != 'r' || plus;
    *append = mode[0] == 'a';
    return flags;
}

static FILE *stream(int kind, int fd, int readable, int writable, int append)
{
    FILE *f = calloc(1, sizeof(*f));

    if (f == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    f->kind = kind;
    f->fd = fd;
    f->readable = readable;
    f->writable = writable;
    f->append = append;
    f->ungot = -1;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int readable, writable, append, flags, fd;
    FILE *f;

    if (path == NULL || mode == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if ((flags = flags_of(mode, &readable, &writable, &append)) < 0) {
        return NULL;
    }

    fd = open(path, flags, 0644);

    if (fd < 0) {
        const void *bytes;
        size_t length;

        /* Not in the namespace: what a kit provided by that name, read
         * only, as the runtime's `fopen` gives it. */
        if (errno == ENOENT && !writable && kosmos_provided(path, &bytes, &length) == 0) {
            if ((f = stream(PROVIDED, -1, 1, 0, 0)) != NULL) {
                f->bytes = bytes;
                f->length = length;
            }

            return f;
        }

        return NULL;
    }

    if ((f = stream(DESCRIPTOR, fd, readable, writable, append)) == NULL) {
        close(fd);
    }

    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    int readable, writable, append;

    if (mode == NULL || flags_of(mode, &readable, &writable, &append) < 0) {
        return NULL;
    }

    if (fcntl(fd, F_GETFL) < 0) {
        return NULL;                    /* EBADF, from fcntl */
    }

    if (fd >= 0 && fd < 3) {
        return fd == 0 ? stdin : fd == 1 ? stdout : stderr;
    }

    return stream(DESCRIPTOR, fd, readable, writable, append);
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    FILE *fresh;

    if (f == NULL || path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if ((fresh = fopen(path, mode)) == NULL) {
        return NULL;
    }

    if (f->kind == CONSOLE) {
        return fresh;                   /* the console's three stay */
    }

    (void)fflush(f);

    if (f->kind == DESCRIPTOR) {
        close(f->fd);
    }

    free(f->temporary);
    *f = *fresh;
    free(fresh);
    return f;
}

FILE *tmpfile(void)
{
    static unsigned counter;
    char path[64];
    FILE *f;
    int tries;

    for (tries = 0; tries < 100; tries++) {
        int fd;

        snprintf(path, sizeof path, "/Temporary/tmp-%d-%u", (int)getpid(),
                 __atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED));

        if ((fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600)) < 0) {
            if (errno == EEXIST) {
                continue;
            }

            return NULL;
        }

        if ((f = stream(DESCRIPTOR, fd, 1, 1, 0)) == NULL) {
            close(fd);
            unlink(path);
            return NULL;
        }

        f->temporary = strdup(path);
        return f;
    }

    errno = EEXIST;
    return NULL;
}

int fclose(FILE *f)
{
    int r = 0;

    if (f == NULL) {
        errno = EBADF;
        return EOF;
    }

    if (f->kind == CONSOLE) {
        return 0;
    }

    if (f->kind == DESCRIPTOR) {
        r = drain(f);

        if (close(f->fd) != 0) {
            r = EOF;
        }
    }

    if (f->temporary != NULL) {
        unlink(f->temporary);
        free(f->temporary);
    }

    free(f);
    return r;
}

int fileno(FILE *f)
{
    if (f == NULL || f->kind == PROVIDED) {
        errno = EBADF;
        return -1;
    }

    return f->fd;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    (void)f; (void)buf; (void)mode; (void)size;
    return 0;                           /* every stream keeps its own */
}

void setbuf(FILE *f, char *buf)
{
    (void)f; (void)buf;
}

/*
 *--------------------------------------------------------------------------
 * Reading.
 */

int getc(FILE *f)
{
    if (f == NULL || !f->readable) {
        return EOF;
    }

    if (f->ungot >= 0) {
        int c = f->ungot;

        f->ungot = -1;
        return c;
    }

    switch (f->kind) {
    case PROVIDED:
        if (f->at >= f->length) {
            f->eof = 1;
            return EOF;
        }

        return f->bytes[f->at++];

    case DESCRIPTOR:
        return fill(f) ? f->buffer[f->start++] : EOF;

    default:
        f->eof = 1;                     /* the console's input is empty */
        return EOF;
    }
}

int fgetc(FILE *f)
{
    return getc(f);
}

int getchar(void)
{
    return getc(stdin);
}

int ungetc(int c, FILE *f)
{
    if (f == NULL || c == EOF || f->ungot >= 0) {
        return EOF;
    }

    f->ungot = (unsigned char)c;
    f->eof = 0;
    return (unsigned char)c;
}

size_t fread(void *b, size_t size, size_t count, FILE *f)
{
    unsigned char *to = b;
    size_t want, got = 0;

    if (f == NULL || size == 0 || count == 0 || !f->readable) {
        return 0;
    }

    want = size * count;

    if (f->ungot >= 0 && got < want) {
        to[got++] = (unsigned char)f->ungot;
        f->ungot = -1;
    }

    if (f->kind == PROVIDED) {
        size_t n = f->length - f->at;

        if (n > want - got) {
            n = want - got;
        }

        memcpy(to + got, f->bytes + f->at, n);
        f->at += n;
        got += n;

        if (got < want) {
            f->eof = 1;
        }
    } else if (f->kind == DESCRIPTOR) {
        while (got < want) {
            size_t n;

            /* A large read goes straight to the descriptor, once what was
             * read ahead is used. */
            if (f->direction != WRITING && f->start == f->end && want - got >= BUFFER) {
                ssize_t r = read(f->fd, to + got, want - got);

                f->direction = IDLE;

                if (r <= 0) {
                    if (r < 0) {
                        f->error = 1;
                    } else {
                        f->eof = 1;
                    }

                    break;
                }

                got += (size_t)r;
                continue;
            }

            if (!fill(f)) {
                break;
            }

            n = f->end - f->start;

            if (n > want - got) {
                n = want - got;
            }

            memcpy(to + got, f->buffer + f->start, n);
            f->start += n;
            got += n;
        }
    } else {
        f->eof = 1;
    }

    return got / size;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0, c = 0;

    if (s == NULL || size <= 0 || f == NULL) {
        return NULL;
    }

    while (i < size - 1 && (c = getc(f)) != EOF) {
        s[i++] = (char)c;

        if (c == '\n') {
            break;
        }
    }

    if (i == 0 && c == EOF) {
        return NULL;
    }

    s[i] = '\0';
    return s;
}

ssize_t getdelim(char **line, size_t *room, int delimiter, FILE *f)
{
    size_t n = 0;
    int c;

    if (line == NULL || room == NULL || f == NULL) {
        errno = EINVAL;
        return -1;
    }

    while ((c = getc(f)) != EOF) {
        if (n + 2 > *room || *line == NULL) {
            size_t grown = *room < 64 ? 128 : *room * 2;
            char *more = realloc(*line, grown);

            if (more == NULL) {
                errno = ENOMEM;
                return -1;
            }

            *line = more;
            *room = grown;
        }

        (*line)[n++] = (char)c;

        if (c == delimiter) {
            break;
        }
    }

    if (n == 0) {
        return -1;
    }

    (*line)[n] = '\0';
    return (ssize_t)n;
}

ssize_t getline(char **line, size_t *room, FILE *f)
{
    return getdelim(line, room, '\n', f);
}

/*
 *--------------------------------------------------------------------------
 * Writing.
 */

size_t fwrite(const void *b, size_t size, size_t count, FILE *f)
{
    const unsigned char *from = b;
    size_t want, done = 0;

    f = out_of(f);

    if (size == 0 || count == 0) {
        return 0;
    }

    if (!f->writable) {
        f->error = 1;
        errno = EBADF;
        return 0;
    }

    want = size * count;

    if (f->kind == CONSOLE) {
        return write(f->fd, b, want) == (ssize_t)want ? count : 0;
    }

    unread(f);

    if (want >= BUFFER) {
        ssize_t n;

        if (drain(f) != 0) {
            return 0;
        }

        n = write(f->fd, from, want);

        if (n < 0) {
            f->error = 1;
            return 0;
        }

        return (size_t)n / size;
    }

    while (done < want) {
        size_t n = BUFFER - f->end;

        if (n == 0) {
            if (drain(f) != 0) {
                break;
            }

            continue;
        }

        if (n > want - done) {
            n = want - done;
        }

        memcpy(f->buffer + f->end, from + done, n);
        f->end += n;
        f->direction = WRITING;
        done += n;
    }

    return done / size;
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;

    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}

int putc(int c, FILE *f)
{
    return fputc(c, f);
}

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);

    return n == 0 || fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int fflush(FILE *f)
{
    if (f == NULL) {
        return 0;                       /* every stream: the console is unbuffered,
                                           and files drain as they fill */
    }

    if (f->kind != DESCRIPTOR) {
        return 0;
    }

    if (f->direction == WRITING) {
        return drain(f);
    }

    unread(f);
    return 0;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char line[512], *text = line;
    va_list again;
    int n;

    va_copy(again, ap);
    n = vsnprintf(line, sizeof line, fmt, ap);

    if (n >= (int)sizeof line && (text = malloc((size_t)n + 1)) != NULL) {
        (void)vsnprintf(text, (size_t)n + 1, fmt, again);
    } else if (n >= (int)sizeof line) {
        text = line;
        n = (int)sizeof line - 1;
    }

    va_end(again);

    if (n > 0 && fwrite(text, 1, (size_t)n, f) != (size_t)n) {
        n = -1;
    }

    if (text != line) {
        free(text);
    }

    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

void perror(const char *s)
{
    if (s != NULL && s[0] != '\0') {
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    } else {
        fprintf(stderr, "%s\n", strerror(errno));
    }
}

/*
 *--------------------------------------------------------------------------
 * Positions.
 */

off_t ftello(FILE *f)
{
    off_t at;

    if (f == NULL) {
        errno = EBADF;
        return -1;
    }

    if (f->kind == PROVIDED) {
        return (off_t)f->at - (f->ungot >= 0 ? 1 : 0);
    }

    if (f->kind != DESCRIPTOR) {
        errno = ESPIPE;
        return -1;
    }

    if ((at = lseek(f->fd, 0, SEEK_CUR)) < 0) {
        return -1;
    }

    if (f->direction == READING) {
        at -= (off_t)(f->end - f->start) + (f->ungot >= 0 ? 1 : 0);
    } else if (f->direction == WRITING) {
        at += (off_t)f->end;
    }

    return at;
}

long ftell(FILE *f)
{
    return (long)ftello(f);
}

int fseeko(FILE *f, off_t offset, int whence)
{
    if (f == NULL) {
        errno = EBADF;
        return -1;
    }

    if (f->kind == PROVIDED) {
        off_t base = whence == SEEK_SET ? 0
                   : whence == SEEK_CUR ? (off_t)f->at - (f->ungot >= 0 ? 1 : 0)
                   : (off_t)f->length;

        if (whence < SEEK_SET || whence > SEEK_END || base + offset < 0
            || base + offset > (off_t)f->length) {
            errno = EINVAL;
            return -1;
        }

        f->at = (size_t)(base + offset);
        f->ungot = -1;
        f->eof = 0;
        return 0;
    }

    if (f->kind != DESCRIPTOR) {
        errno = ESPIPE;
        return -1;
    }

    /* SEEK_CUR from where the caller is, not where the descriptor is. */
    if (whence == SEEK_CUR) {
        off_t here = ftello(f);

        if (here < 0) {
            return -1;
        }

        offset += here;
        whence = SEEK_SET;
    }

    if (drain(f) != 0) {
        return -1;
    }

    f->start = f->end = 0;
    f->ungot = -1;
    f->direction = IDLE;
    f->eof = 0;
    return lseek(f->fd, offset, whence) < 0 ? -1 : 0;
}

int fseek(FILE *f, long offset, int whence)
{
    return fseeko(f, (off_t)offset, whence);
}

void rewind(FILE *f)
{
    (void)fseek(f, 0, SEEK_SET);

    if (f != NULL) {
        f->error = 0;
    }
}

int feof(FILE *f)
{
    return f != NULL && f->eof;
}

int ferror(FILE *f)
{
    return f != NULL && f->error;
}

void clearerr(FILE *f)
{
    if (f != NULL) {
        f->eof = 0;
        f->error = 0;
    }
}

/*
 *--------------------------------------------------------------------------
 * Scanning: the runtime's scanner over what is in hand.
 *
 * A provided file is all in hand, as the runtime's is. A descriptor's is
 * the buffer, topped up first so at least what is left of it - up to 4 KB
 * - is there: a conversion that would cross past that is cut at it. Quake's
 * "%i\n" is what this was written for; a port that scans more uses its own
 * parser, as curl and WebKit do.
 */
int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    size_t used = 0;
    int n;

    if (f == NULL) {
        return EOF;
    }

    if (f->kind == PROVIDED) {
        if (f->ungot >= 0) {
            f->at--;
            f->ungot = -1;
        }

        n = kosmos_vscan((const char *)f->bytes + f->at, f->length - f->at, fmt, ap, &used);
        f->at += used;
        return n;
    }

    if (f->kind != DESCRIPTOR || !fill(f)) {
        return EOF;
    }

    /* Top up: what is left moved to the front, and the rest read in. */
    if (f->start > 0) {
        memmove(f->buffer, f->buffer + f->start, f->end - f->start);
        f->end -= f->start;
        f->start = 0;
    }

    while (f->end < BUFFER) {
        ssize_t r = read(f->fd, f->buffer + f->end, BUFFER - f->end);

        if (r <= 0) {
            break;
        }

        f->end += (size_t)r;
    }

    if (f->ungot >= 0) {
        /* Given back in front of the buffer, where it was read from. */
        if (f->end == BUFFER) {
            (void)lseek(f->fd, -1, SEEK_CUR);
            f->end--;
        }

        memmove(f->buffer + 1, f->buffer, f->end);
        f->buffer[0] = (unsigned char)f->ungot;
        f->end++;
        f->ungot = -1;
    }

    n = kosmos_vscan((const char *)f->buffer, f->end, fmt, ap, &used);
    f->start = used;
    return n;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}
