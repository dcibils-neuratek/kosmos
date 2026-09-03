#ifndef SYS_TYPES_H
#define SYS_TYPES_H

/*
 * Ours, so that the toolchain's is never reached.
 *
 * `-Iruntime/include` comes before the compiler's own include path, and
 * that ordering is the whole point of this file existing. newlib ships a
 * `<sys/types.h>` and it is a POSIX one: it declares `clock_t`, `pid_t`,
 * `off_t` and a hundred other things this system does not have, and its
 * `clock_t` disagrees with the one in our `<time.h>`, so a file that
 * included both would not compile.
 *
 * The rule in `CLAUDE.md` is that compatibility lives *inside* a process
 * and never at system level. A port that includes `<sys/types.h>` gets a
 * header; it does not get a POSIX personality.
 */

#include <stddef.h>

typedef unsigned int mode_t;

#endif /* SYS_TYPES_H */
