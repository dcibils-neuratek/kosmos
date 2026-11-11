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
#include <sys/utsname.h>
#include <unistd.h>

#include "kosmos.h"

/* The build's own words (`$(GEN)/version.c`), as About and `uname` at the
 * prompt say them. */
extern const char kosmos_name[];
extern const char kosmos_version[];
extern const char kosmos_build[];

#define PAGE 4096L

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

    default:
        errno = EINVAL;
        return -1;
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
