/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What expat's `configure` would have found on this system, written down
 * rather than discovered (`runtime/upstream/expat/`, `roadmap.md` 6zz j5):
 * expat reads it when built with `HAVE_EXPAT_CONFIG_H`.
 *
 * - **Little-endian**, which both of this system's processors are.
 * - **Its hash salt from `arc4random_buf`**, which the libc answers from
 *   the kernel's entropy (`SYS_ENTROPY`, `user/init/misc_user.c`). expat
 *   salts its hash tables so that a document cannot be written to make
 *   them collide, and refuses to build without a source it trusts.
 * - **Namespaces**, which libdom's XML binding asks for; **DTDs and general
 *   entities**, which documents declare, with expat's own limits on how far
 *   an entity may amplify - a billion laughs is refused, not obeyed.
 */

#ifndef KOSMOS_EXPAT_CONFIG_H
#define KOSMOS_EXPAT_CONFIG_H

#define BYTEORDER           1234
#define HAVE_ARC4RANDOM_BUF 1
#define HAVE_STDINT_H       1
#define HAVE_STDLIB_H       1
#define HAVE_STRING_H       1
#define HAVE_INTTYPES_H     1

#define XML_NS              1
#define XML_DTD             1
#define XML_GE              1
#define XML_CONTEXT_BYTES   1024

#endif
