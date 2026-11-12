/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SYS_SENDFILE_H
#define POSIX_SYS_SENDFILE_H

/* **Linux's `sendfile`**, for ported code (`docs/w4.md` W4c): bytes from
 * one descriptor to another without the caller holding them - libc++'s
 * `std::filesystem::copy_file`, built without iostreams, has no other
 * way to copy (`fd.c`). */

#include <sys/cdefs.h>
#include <sys/types.h>

__BEGIN_DECLS

ssize_t sendfile(int out, int in, off_t *offset, size_t count);

__END_DECLS

#endif /* POSIX_SYS_SENDFILE_H */
