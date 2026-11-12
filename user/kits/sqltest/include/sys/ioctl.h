/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SQLTEST_SYS_IOCTL_H
#define SQLTEST_SYS_IOCTL_H

/*
 * **Empty, on purpose** (`docs/w5.md` W5c). SQLite's Unix layer includes
 * `<sys/ioctl.h>` whatever it is built for, and calls `ioctl` only on
 * Linux with batch atomic writes, for F2FS. `ioctl` is one of the names
 * Kosmos does not have even for ported code (CLAUDE.md, *Compatibility
 * inside a process*), so the POSIX layer does not grow this header: it is
 * here, on SQLite's include path alone, and declares nothing.
 */

#endif
