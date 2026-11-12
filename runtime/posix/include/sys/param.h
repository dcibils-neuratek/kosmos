/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SYS_PARAM_H
#define POSIX_SYS_PARAM_H

/* The BSDs' old name for the longest path, which ported code still
 * includes this for (WTF's `FileSystem.cpp`). */
#include <limits.h>

#define MAXPATHLEN PATH_MAX

#endif /* POSIX_SYS_PARAM_H */
