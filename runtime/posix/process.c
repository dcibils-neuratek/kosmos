/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The process's own facts, for ported code** (`docs/w3.md` W3c): its id,
 * the users it is not, what `sysconf` and `uname` say about the machine,
 * and random bytes.
 *
 * `getenv` is not here and stays the runtime's: there is no environment,
 * permanently (`user/init/misc_user.c`) - a bag of names a child inherits
 * without being handed them. What WebKit and curl read from one, a proxy,
 * a home directory, a debugging switch, is unset here, which every one of
 * them handles.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "kosmos.h"

/* The build's own words (`$(GEN)/version.c`), as About and `uname` at the
 * prompt say them. */
extern const char kosmos_name[];
extern const char kosmos_version[];
extern const char kosmos_build[];

#define PAGE 4096L

/* Refused: there is no environment to put a name in (`stdlib.h`). libxml2's
 * `runtest` sets `XML_CATALOG_FILES` empty to have no catalogs, and has
 * none here either way - nothing is set, and the default catalog's place
 * (`/Kosmos/Settings/xml/catalog`) has nothing in it. */
int setenv(const char *name, const char *value, int overwrite)
{
    (void)name;
    (void)value;
    (void)overwrite;
    errno = ENOTSUP;
    return -1;
}

int unsetenv(const char *name)
{
    (void)name;
    errno = ENOTSUP;
    return -1;
}

pid_t getpid(void)
{
    return (pid_t)kosmos_self();
}

uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

long sysconf(int name)
{
    struct sysinfo info;

    switch (name) {
    case _SC_PAGESIZE:
        return PAGE;

    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        if (kosmos_sysinfo(&info) != 0 || info.cpus_online == 0) {
            return 1;
        }

        return name == _SC_NPROCESSORS_ONLN ? (long)info.cpus_online : (long)info.cpus;

    /* What the machine has, for an allocator or a cache sizing itself -
     * WTF's `ramSize` (`docs/w4.md` W4c). In pages, as POSIX has them. */
    case _SC_PHYS_PAGES:
    case _SC_AVPHYS_PAGES:
        if (kosmos_sysinfo(&info) != 0) {
            errno = EINVAL;
            return -1;
        }

        return name == _SC_PHYS_PAGES ? (long)info.pages_total : (long)info.pages_free;

    default:
        errno = EINVAL;
        return -1;
    }
}

/* Nothing measured yet, said as nought (`sys/resource.h` has why). */
int getrusage(int who, struct rusage *out)
{
    if (who != RUSAGE_SELF || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    return 0;
}

/*
 * **How much memory this process holds, in bytes** - a Kosmos extension,
 * as the `_np` calls are: WebKit's `memoryFootprint`, which on Linux reads
 * /proc. Nebula's process table says it of every process (`struct
 * proc_info`: the pages it mapped and everything else it holds), so this
 * reads the table and finds its own row. 0 when the table does not have
 * it, which only a process that is ending could see.
 */
size_t posix_footprint_np(void)
{
    unsigned long room = 64;
    long self = kosmos_self();

    for (;;) {
        struct proc_info *rows = malloc(room * sizeof(*rows));
        long n, i;
        size_t bytes = 0;

        if (rows == NULL) {
            return 0;
        }

        n = kosmos_proctable(rows, room);

        if (n >= (long)room) {          /* perhaps more than fitted */
            free(rows);
            room *= 2;
            continue;
        }

        for (i = 0; i < n; i++) {
            if ((long)rows[i].id == self && !rows[i].exited) {
                bytes = ((size_t)rows[i].pages + rows[i].held) * 4096u;
                break;
            }
        }

        free(rows);
        return bytes;
    }
}

int getpagesize(void)
{
    return (int)PAGE;
}

/* `text` into `out`, cut to fit and always ended. */
static void put(char out[UTSNAME_LENGTH], const char *text)
{
    size_t n = strlen(text);

    if (n >= UTSNAME_LENGTH) {
        n = UTSNAME_LENGTH - 1;
    }

    memcpy(out, text, n);
    out[n] = '\0';
}

int uname(struct utsname *out)
{
    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }

    put(out->sysname, kosmos_name);
    put(out->nodename, "kosmos");
    put(out->release, kosmos_version);
    put(out->version, kosmos_build);
#if defined(__x86_64__)
    put(out->machine, "x86_64");
#else
    put(out->machine, "aarch64");
#endif
    return 0;
}

ssize_t getrandom(void *out, size_t bytes, unsigned flags)
{
    (void)flags;

    if (bytes > 0x7fffffffu) {
        bytes = 0x7fffffffu;            /* what one call promises, as Linux's */
    }

    arc4random_buf(out, bytes);
    return (ssize_t)bytes;
}

int getentropy(void *out, size_t bytes)
{
    if (bytes > 256) {
        errno = EIO;
        return -1;
    }

    arc4random_buf(out, bytes);
    return 0;
}

/*
 * **`_exit`: the process ends now.** Nothing is run on the way - no
 * `atexit`, no stream flushed - which is what POSIX means by it and what
 * WebKit's `terminateProcess` wants on the way out of a fatal error;
 * Nebula takes every thread and everything the process held
 * (`SYS_EXIT`).
 */
void _exit(int status)
{
    kosmos_exit(status);
}

/*
 * **A static object's destructor, registered and never run** (`docs/w4.md`
 * W4d). C++ registers the destructor of every static with a non-trivial
 * one through `__cxa_atexit`, naming the image it is in by `__dso_handle` -
 * which a toolchain's `crtbegin.o` would define, and a Kosmos image has no
 * such file. A process here ends by asking Nebula, which takes back every
 * page and every thread at once, so there is nothing a destructor at exit
 * could give back that is not already being taken: the registration is
 * accepted and kept nowhere, as a static object's memory is never freed.
 */
void *__dso_handle = &__dso_handle;

int __cxa_atexit(void (*destructor)(void *), void *object, void *image)
{
    (void)destructor;
    (void)object;
    (void)image;
    return 0;
}
