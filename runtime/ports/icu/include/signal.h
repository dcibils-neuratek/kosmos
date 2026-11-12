/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **For ICU alone, and empty on purpose.** decNumber's `decContext.h`
 * includes `<signal.h>` "for traps", and the one line that would raise one
 * is `#if 0` in ICU's copy ("ICU: Do not raise signals"). So nothing from
 * this header is used - it only has to exist.
 *
 * It is here rather than in `runtime/posix/include/` because Kosmos has no
 * signals and the POSIX layer does not pretend to (`design.md` §17.1): a
 * `signal.h` there would be a promise to every port. This directory is on
 * ICU's include path and nobody else's (`tools/icu_build.py`).
 */
#ifndef KOSMOS_ICU_SIGNAL_H
#define KOSMOS_ICU_SIGNAL_H
#endif
