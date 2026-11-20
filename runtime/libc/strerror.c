/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `strerror`: the sentence for an `errno`.
 *
 * A file of its own, needing nothing, since 20 November 2026: `snprintf`'s
 * `%m` says it, and the test kernel links `snprintf.c` without the rest of
 * the C library - so the sentence had to be somewhere that kernel could
 * link too, rather than in `stdio.c` beside the console it does not have
 * (`testing.md` 18.932).
 */
#include <errno.h>
#include <string.h>

char *strerror(int errnum)
{
    switch (errnum) {
    case 0:      return "no error";
    case ENOENT: return "no such path in this namespace";
    case EDOM:   return "argument outside the domain";
    case ERANGE: return "result outside the representable range";
    /* The ones ported code reports most - Mesa's "mmap failed: %m" said
     * "unknown error" for EINVAL (`testing.md` 18.932). */
    case EPERM:  return "not permitted";
    case EIO:    return "input or output failed";
    case EBADF:  return "not an open descriptor";
    case EAGAIN: return "try again";
    case ENOMEM: return "out of memory";
    case EACCES: return "not handed the authority for it";
    case EFAULT: return "an address outside the process";
    case EBUSY:  return "busy";
    case EEXIST: return "already exists";
    case ENODEV: return "no such device";
    case EINVAL: return "invalid argument";
    case ENOSPC: return "no space left";
    case ENOSYS: return "not offered here";
    case ETIMEDOUT: return "timed out";
    default:     return "unknown error";
    }
}
