/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * libsmb2's `config.h` for Kosmos: what its build would have found here, and
 * the platform it builds on (`docs/sharing.md`, *The SMB Kit*, step N2).
 *
 * **Every file of libsmb2 includes this first** (`HAVE_CONFIG_H`), which is
 * what lets the port be a header of Kosmos's own rather than an edit to
 * `runtime/upstream/libsmb2/`: the library is compiled as released, against
 * these answers. `tools/libsmb2_mac_config.h` is the same file for the Mac's
 * copy, which `tools/test_smbpeer.py` holds to Samba.
 *
 * What is said here, and why each:
 *
 *   - the C headers `runtime/include/` has, and none it does not: libsmb2
 *     guards every one of its includes with a `HAVE_`, so a header this
 *     machine has no use for is a `HAVE_` left out rather than a stub;
 *   - **no socket headers at all**: `smb_port.h` below is the platform -
 *     its socket is a connection on the network stack's ring;
 *   - `HAVE_ARC4RANDOM_BUF`: libsmb2's one door for randomness,
 *     `smb2_random_bytes`, takes `arc4random_buf`, which in Kosmos is the
 *     kernel's entropy (`SYS_ENTROPY`, `user/init/misc_user.c`). Its
 *     fallback - `random()` seeded with `time() ^ getpid()` - is then not
 *     compiled at all, which is what "removed rather than left to be
 *     reached" means without editing the file it is in;
 *   - not `HAVE_LIBKRB5`: there is no domain here to sign in to.
 */
#ifndef KOSMOS_SMB_CONFIG_H
#define KOSMOS_SMB_CONFIG_H

#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STDIO_H 1
#define HAVE_TIME_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1

#define HAVE_ARC4RANDOM_BUF 1

/* What `smb_port.h` defines for it, so `compat.h` does not again. */
#define HAVE_STRUCT_IOVEC 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_ADDRINFO 1
#define HAVE_ADDRINFO 1
#define HAVE_LINGER 1

#define STDC_HEADERS 1
#define _U_ __attribute__((unused))

#include "smb_port.h"

#endif /* KOSMOS_SMB_CONFIG_H */
