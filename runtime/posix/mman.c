/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Memory, inside the process** (`docs/w3.md` W3b): `mmap`, `munmap`,
 * `mprotect` and `madvise` on W2's reservations (`kernel/reserve.c`).
 *
 * Each is one or two calls and a translation. `mmap` of anonymous memory
 * reserves the pages - addresses only, no page behind them - and gives them
 * the access asked for, so a gigabyte `PROT_NONE` costs one record and a
 * page made writable is a zeroed one at its first touch. `mprotect` is
 * `SYS_PROTECT`; `munmap` is `SYS_RELEASE`, which gives back a piece from
 * the middle as readily as the whole; `madvise(MADV_DONTNEED)` is
 * `RESERVE_DISCARD`, the pages given back and their access kept, so the
 * next touch reads zero as Linux's does. `MAP_FIXED` is a discard and a
 * protect over addresses the process already has.
 *
 * **No bytes is no work, and succeeds**, for `mprotect` and `madvise` as
 * on Linux: JavaScriptCore protects what a resizable buffer may grow
 * into, and when its most rounds to the pages it has, that is nothing
 * - and it ends the program on a refusal (`testing.md` 18.780). `munmap`
 * of nothing is still EINVAL, as it is there.
 *
 * What it refuses, and how: writable and executable at once is `EACCES`; a
 * file is `ENODEV`, since W3 maps none; `MAP_FIXED` over addresses that
 * are not this process's reservations is `ENOMEM`, since there is nothing
 * there it may take.
 */

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kosmos.h"

#define PAGE 4096UL

#define KNOWN_FLAGS (MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE)

/* The reservation's access for `prot`, or -1 with `errno` set. */
static long access_for(int prot)
{
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        errno = EINVAL;
        return -1;
    }

    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        errno = EACCES;                 /* never both at once */
        return -1;
    }

    if (prot & PROT_EXEC) {
        return RESERVE_RX;
    }

    if (prot & PROT_WRITE) {
        return RESERVE_RW;
    }

    return (prot & PROT_READ) ? RESERVE_READ : RESERVE_NONE;
}

/* How many pages `length` bytes are, or 0 when it is none or too many. */
static unsigned long pages_in(size_t length)
{
    if (length == 0 || length > SIZE_MAX - (PAGE - 1)) {
        return 0;
    }

    return (length + PAGE - 1) / PAGE;
}

static int aligned(const void *addr)
{
    return ((uintptr_t)addr & (PAGE - 1)) == 0;
}

/* A refusal from the kernel as `errno`: addresses that are not this
 * process's are ENOMEM, as POSIX has it for a range not mapped. */
static int fail(long code, int fault)
{
    errno = code == SYS_ERR_FAULT ? fault
          : code == SYS_ERR_NO_ROOM ? ENOMEM
          : code == SYS_ERR_FLAGS ? EACCES
          : EINVAL;
    return -1;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    unsigned long pages = pages_in(length);
    long access, at, r;

    (void)fd;

    if (length == 0 || (flags & ~KNOWN_FLAGS) != 0 || (offset & (off_t)(PAGE - 1)) != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    if (pages == 0) {
        errno = ENOMEM;                 /* more than the addresses there are */
        return MAP_FAILED;
    }

    if ((flags & MAP_ANONYMOUS) == 0) {
        errno = ENODEV;                 /* a file: not in W3 */
        return MAP_FAILED;
    }

    if ((access = access_for(prot)) < 0) {
        return MAP_FAILED;
    }

    if (flags & MAP_FIXED) {
        if (!aligned(addr)) {
            errno = EINVAL;
            return MAP_FAILED;
        }

        /* What was there given back, and the access asked for: the pages
         * read zero, as a new mapping's do. */
        r = kosmos_protect(addr, pages, RESERVE_DISCARD);

        if (r == 0) {
            r = kosmos_protect(addr, pages, (unsigned)access);
        }

        if (r != 0) {
            fail(r, ENOMEM);
            return MAP_FAILED;
        }

        return addr;
    }

    at = kosmos_reserve(pages);

    if (at < 0) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    if (access != RESERVE_NONE
        && (r = kosmos_protect((void *)(uintptr_t)at, pages, (unsigned)access)) != 0) {
        (void)kosmos_release((void *)(uintptr_t)at, pages);
        fail(r, ENOMEM);
        return MAP_FAILED;
    }

    return (void *)(uintptr_t)at;
}

int munmap(void *addr, size_t length)
{
    unsigned long pages = pages_in(length);
    long r;

    if (pages == 0 || !aligned(addr)) {
        errno = EINVAL;
        return -1;
    }

    r = kosmos_release(addr, pages);
    return r == 0 ? 0 : fail(r, EINVAL);
}

int mprotect(void *addr, size_t length, int prot)
{
    unsigned long pages = pages_in(length);
    long access, r;

    if (length == 0 && aligned(addr)) {
        return 0;                       /* none of the pages, as Linux has it */
    }

    if (pages == 0 || !aligned(addr)) {
        errno = EINVAL;
        return -1;
    }

    if ((access = access_for(prot)) < 0) {
        return -1;
    }

    r = kosmos_protect(addr, pages, (unsigned)access);
    return r == 0 ? 0 : fail(r, ENOMEM);
}

int madvise(void *addr, size_t length, int advice)
{
    unsigned long pages = pages_in(length);
    long r;

    if (length == 0 && aligned(addr)) {
        return 0;                       /* none of the pages, as Linux has it */
    }

    if (pages == 0 || !aligned(addr)) {
        errno = EINVAL;
        return -1;
    }

    switch (advice) {
    case MADV_DONTNEED:
    case MADV_FREE:
        r = kosmos_protect(addr, pages, RESERVE_DISCARD);
        return r == 0 ? 0 : fail(r, ENOMEM);

    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
        return 0;                       /* advice, and taken as such */

    default:
        errno = EINVAL;
        return -1;
    }
}

/*
 * **`msync` has nothing to write.** No file is ever mapped here (`mmap`
 * of one is ENODEV), so every page a process has is its own memory, and
 * that is what Linux does with `msync` on anonymous memory too: nothing,
 * successfully. The address is still held to a page, as POSIX asks.
 */
int msync(void *addr, size_t length, int flags)
{
    (void)length;

    if (!aligned(addr) || (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) != 0
        || ((flags & MS_ASYNC) && (flags & MS_SYNC))) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}
