/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_REFUSED_SIGNAL_H
#define POSIX_REFUSED_SIGNAL_H

/*
 * **Signals, refused** (`docs/w5.md` W5d). They are one of the four things
 * Kosmos does not have even inside a process (CLAUDE.md, *Compatibility
 * inside a process*; `design.md` §17): a fault is the process's end, and
 * nothing interrupts a thread to run something else on its stack.
 *
 * This header is here, on a port's include path alone, because OpenSSL's
 * `armcap.c` includes it unconditionally and then uses nothing from it once
 * it is told the processor's features come from `getauxval`
 * (`sys/auxv.h`), which is how it is built here (`openssl-kosmos.conf`).
 * It declares nothing: a port that does reach for a signal fails to
 * compile, which is where it should find out.
 */

#endif
