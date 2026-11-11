/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_CDEFS_H
#define SYS_CDEFS_H

/*
 * **The libc's declarations are C's, from C++ too** (`docs/webkit.md` W1c).
 *
 * A C++ compiler mangles every name it declares unless it is told the name
 * is C's, and this libc is C: without these, `strlen` from a C++ file is a
 * call to `_Z6strlenPKc`, which nothing defines. Every header here opens
 * its declarations with `__BEGIN_DECLS` and closes them with `__END_DECLS`,
 * BSD's and glibc's spelling of the same thing, and in C they are nothing.
 */
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS   }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#endif /* SYS_CDEFS_H */
