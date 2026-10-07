/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What TinyCC asks of a Unix, answered for one build (`shim.h`).
 *
 * **Reading**: `tcck_open` asks the reader the kit was handed - a Lua
 * function, `reader(path)`, answering the address and size of the file
 * mapped in a region, or nothing - and a descriptor reads from that memory.
 * The Lua side keeps the regions and lets them go when the build is over;
 * nothing here frees what it did not make.
 *
 * **Writing**: a descriptor opened for writing is a buffer here, grown as
 * TinyCC writes; `fdopen` on it answers a handle that `fwrite`, `fputc` and
 * `fclose` recognise. When the build is done the kit takes the buffer
 * (`tcck_output`) - the image, before its header is stamped.
 *
 * One build at a time: the kit's state, reset by `tcck_begin`.
 */

#include <math.h>
#include <stdarg.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "shim.h"

#undef open
#undef read
#undef lseek
#undef close
#undef fdopen
#undef fwrite
#undef fputc
#undef fclose
#undef unlink
#undef getcwd
#undef realpath
#undef getenv
#undef gettimeofday
#undef strtold
#undef ldexpl

#define FIRST_FD 3

struct tcck_fd {
    int used;
    int writing;
    const unsigned char *bytes;     /* reading: the region's */
    size_t len;
    size_t at;
};

static lua_State *lua;
static int reader_ref = LUA_NOREF;

static struct tcck_fd *fds;
static int fd_count;

static unsigned char *out;
static size_t out_len, out_room;
static int out_fd = -1;

/* The handle `fdopen` answers for the output: its address is all it is. */
static int out_handle;

void tcck_begin(lua_State *L, int reader_index)
{
    lua = L;
    lua_pushvalue(L, reader_index);
    reader_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    free(fds);
    fds = NULL;
    fd_count = 0;

    free(out);
    out = NULL;
    out_len = out_room = 0;
    out_fd = -1;
}

void tcck_end(void)
{
    if (lua != NULL && reader_ref != LUA_NOREF) {
        luaL_unref(lua, LUA_REGISTRYINDEX, reader_ref);
    }

    reader_ref = LUA_NOREF;
    free(fds);
    fds = NULL;
    fd_count = 0;
}

/* The output, taken: the caller owns the bytes and frees them. */
unsigned char *tcck_output(size_t *len)
{
    unsigned char *taken = out;

    *len = out_len;
    out = NULL;
    out_len = out_room = 0;
    return taken;
}

static int new_fd(void)
{
    struct tcck_fd *more;
    int i;

    for (i = 0; i < fd_count; i++) {
        if (!fds[i].used) {
            memset(&fds[i], 0, sizeof fds[i]);
            fds[i].used = 1;
            return i;
        }
    }

    more = realloc(fds, (size_t)(fd_count + 8) * sizeof *fds);
    if (more == NULL) {
        return -1;
    }

    memset(more + fd_count, 0, 8 * sizeof *more);
    fds = more;
    fd_count += 8;
    fds[i].used = 1;
    return i;
}

static struct tcck_fd *of(int fd)
{
    int i = fd - FIRST_FD;

    return (i >= 0 && i < fd_count && fds[i].used) ? &fds[i] : NULL;
}

int tcck_open(const char *path, int flags, ...)
{
    int i;

    if (flags & (O_WRONLY | O_RDWR)) {
        if ((i = new_fd()) < 0) {
            return -1;
        }

        fds[i].writing = 1;
        out_len = 0;
        out_fd = i + FIRST_FD;
        return out_fd;
    }

    if (lua == NULL || reader_ref == LUA_NOREF) {
        return -1;
    }

    /* reader(path) -> address, size; or nil. Whatever it answers, the
     * stack is left as it was found. */
    {
        int top = lua_gettop(lua);
        uintptr_t at;
        size_t len;

        lua_rawgeti(lua, LUA_REGISTRYINDEX, reader_ref);
        lua_pushstring(lua, path);

        if (lua_pcall(lua, 1, 2, 0) != LUA_OK || !lua_isinteger(lua, -2)
            || !lua_isinteger(lua, -1)) {
            lua_settop(lua, top);
            return -1;
        }

        at = (uintptr_t)lua_tointeger(lua, -2);
        len = (size_t)lua_tointeger(lua, -1);
        lua_settop(lua, top);

        if ((i = new_fd()) < 0) {
            return -1;
        }

        fds[i].bytes = (const unsigned char *)at;
        fds[i].len = len;
        return i + FIRST_FD;
    }
}

ssize_t tcck_read(int fd, void *buf, size_t n)
{
    struct tcck_fd *f = of(fd);

    if (f == NULL || f->writing) {
        return -1;
    }

    if (n > f->len - f->at) {
        n = f->len - f->at;
    }

    memcpy(buf, f->bytes + f->at, n);
    f->at += n;
    return (ssize_t)n;
}

static int out_put(const void *p, size_t n)
{
    if (n > out_room - out_len) {
        size_t room = out_room ? out_room : 65536;
        unsigned char *more;

        while (room - out_len < n) {
            room *= 2;
        }

        if ((more = realloc(out, room)) == NULL) {
            return -1;
        }

        out = more;
        out_room = room;
    }

    memcpy(out + out_len, p, n);
    out_len += n;
    return 0;
}

long tcck_lseek(int fd, long offset, int whence)
{
    struct tcck_fd *f = of(fd);
    size_t len, to;

    if (f == NULL) {
        return -1;
    }

    len = f->writing ? out_len : f->len;
    to = whence == SEEK_SET ? (size_t)offset
       : whence == SEEK_CUR ? f->at + (size_t)offset
       : len + (size_t)offset;

    /* Writing past the end leaves zeroes, as a file's gap does. */
    if (f->writing && to > out_len) {
        static const unsigned char zero[256];

        while (out_len < to) {
            size_t n = to - out_len < sizeof zero ? to - out_len : sizeof zero;

            if (out_put(zero, n) != 0) {
                return -1;
            }
        }
    }

    if (!f->writing && to > f->len) {
        return -1;
    }

    f->at = to;
    return (long)to;
}

int tcck_close(int fd)
{
    struct tcck_fd *f = of(fd);

    if (f == NULL) {
        return -1;
    }

    f->used = 0;
    return 0;
}

FILE *tcck_fdopen(int fd, const char *mode)
{
    (void)mode;
    return (fd == out_fd && of(fd) != NULL) ? (FILE *)(void *)&out_handle : NULL;
}

/* TinyCC writes its output where the descriptor stands, which `lseek`
 * moves; so a write goes at that place, not always at the end. */
static int out_write_at(const void *p, size_t n)
{
    struct tcck_fd *f = of(out_fd);
    size_t at = f != NULL ? f->at : out_len;

    static const unsigned char zero[256];

    /* Short of where it lands: zeroes up to there, then the bytes over
     * whatever is already written. */
    while (out_len < at) {
        size_t k = at - out_len < sizeof zero ? at - out_len : sizeof zero;

        if (out_put(zero, k) != 0) {
            return -1;
        }
    }

    if (at == out_len) {
        if (out_put(p, n) != 0) {
            return -1;
        }
    } else {
        size_t over = out_len - at < n ? out_len - at : n;

        memcpy(out + at, p, over);
        if (n > over && out_put((const unsigned char *)p + over, n - over) != 0) {
            return -1;
        }
    }

    if (f != NULL) {
        f->at = at + n;
    }

    return 0;
}

size_t tcck_fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    if (f != (FILE *)(void *)&out_handle) {
        return fwrite(p, size, n, f);
    }

    return out_write_at(p, size * n) == 0 ? n : 0;
}

int tcck_fputc(int c, FILE *f)
{
    unsigned char b = (unsigned char)c;

    if (f != (FILE *)(void *)&out_handle) {
        return EOF;
    }

    return out_write_at(&b, 1) == 0 ? c : EOF;
}

int tcck_fclose(FILE *f)
{
    if (f == (FILE *)(void *)&out_handle) {
        tcck_close(out_fd);
        out_fd = -1;
        return 0;
    }

    return fclose(f);
}

/* The output replaces whatever had the name, when the Lua side writes it. */
int tcck_unlink(const char *path)
{
    (void)path;
    return 0;
}

char *tcck_getcwd(char *buf, size_t size)
{
    if (size < 2) {
        return NULL;
    }

    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

char *tcck_realpath(const char *path, char *resolved)
{
    size_t n = strlen(path) + 1;
    char *to = resolved != NULL ? resolved : malloc(n);

    if (to != NULL) {
        memcpy(to, path, n);
    }

    return to;
}

char *tcck_getenv(const char *name)
{
    (void)name;
    return NULL;
}

int tcck_gettimeofday(void *tv, void *tz)
{
    (void)tz;
    memset(tv, 0, 2 * sizeof(long));
    ((long *)tv)[0] = (long)time(NULL);
    return 0;
}

long double tcck_strtold(const char *s, char **end)
{
    return (long double)strtod(s, end);
}

long double tcck_ldexpl(long double x, int e)
{
    return (long double)ldexp((double)x, e);
}
