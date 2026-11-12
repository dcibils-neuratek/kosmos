/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_MMAN_H
#define SYS_MMAN_H

/*
 * **Memory, inside the process** (`docs/w3.md` W3b), on W2's reservations:
 * `mmap` of anonymous memory reserves addresses and gives them an access,
 * `mprotect` changes it, `munmap` gives them back and `madvise
 * (MADV_DONTNEED)` gives the pages behind them back and keeps the access.
 *
 * For ported code only (`runtime/posix/include/`). **Never writable and
 * executable at once**: `PROT_WRITE | PROT_EXEC` is `EACCES`, as a system
 * that forbids it answers, and JavaScriptCore's two-mapping heap uses
 * `kosmos_mem_map_rx` through its Kosmos file. A file is not mapped in W3:
 * nothing has asked yet, and `mmap` of one is `ENODEV`. `MAP_FIXED` works
 * over addresses this process already has from `mmap`, and nowhere else.
 */

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01      /* the same as private: nothing is shared */
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FILE      0         /* BSD's word for "not anonymous"; mmap of a file is ENODEV */
#define MAP_NORESERVE 0x4000    /* every reservation is that already */

#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8       /* as DONTNEED: given back at once */

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t length);
int   mprotect(void *addr, size_t length, int prot);

/* Nothing to write back: no file is mapped (`mman.c`). */
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4
int   msync(void *addr, size_t length, int flags);
int   madvise(void *addr, size_t length, int advice);

__END_DECLS

#endif /* SYS_MMAN_H */
