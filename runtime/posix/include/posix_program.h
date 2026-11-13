/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_PROGRAM_H
#define POSIX_PROGRAM_H

/*
 * **A ported program's `main`, run inside this process**
 * (`runtime/posix/program.c`): a test program from a library's own tree -
 * SQLite's `fuzzcheck`, libxml2's `runtest`, libpsl's tests - called as a
 * function by the kit that holds it, and given back what it returned.
 *
 * Two things a program assumes and a call does not give it, so this does:
 *
 * - **Its own stack.** A process's is 256 KB, and a program written for a
 *   desktop assumes eight megabytes; it gets what it asks for here, with an
 *   unmapped page below it that an overflow faults on.
 * - **`exit` comes back.** The program is compiled with
 *   `-Dexit=posix_program_exit` (the Makefile), which jumps back to
 *   `posix_program_run` with the code. Kosmos's own `exit` ends the
 *   process, and a test program calls it on the failure it has just
 *   described. What the program left open stays open: a run is one call.
 *
 * What the program prints waits in the spill ring (`kosmos.h`) for the
 * kit to collect - `kit_push_spill` (`user/kits/kitenv.h`) - since a
 * program at the prompt does not own the console.
 *
 * One program at a time: a second run while one is going is refused.
 */

#include <sys/cdefs.h>
#include <stddef.h>

__BEGIN_DECLS

/* `entry(argc, argv)` on a stack of `stack_pages` pages, or what it
 * passed to `exit`. -1 with `errno` when it could not be started: EBUSY
 * inside another run, ENOMEM with no room for the stack. */
int posix_program_run(int (*entry)(int, char **), int argc, char **argv,
                      unsigned long stack_pages);

/* The same, with the program's `stdout` and `stderr` a file made at
 * `path` for the length of the run. -1 with `errno` when the file could
 * not be made, as well. */
int posix_program_run_to(int (*entry)(int, char **), int argc, char **argv,
                         unsigned long stack_pages, const char *path);

/* `exit`, as a program run here calls it. Outside a run, the process's. */
_Noreturn void posix_program_exit(int code);

__END_DECLS

#endif /* POSIX_PROGRAM_H */
