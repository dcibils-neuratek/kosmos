/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ASSERT_H
#define ASSERT_H

#include <panic.h>

/*
 * An assert that fails is a panic: there is nobody to report to and nothing
 * to unwind to. NDEBUG compiles it out, as the standard requires.
 */
#ifdef NDEBUG
#define assert(e)   ((void)0)
#else
#define assert(e)   ((e) ? (void)0 : panic("assertion failed: " #e))
#endif

/*
 * C11's name for `_Static_assert` (7.2 3), which a program may use without
 * knowing the keyword - FFmpeg does, and its `configure` refused a compiler
 * whose `<assert.h>` did not say it.
 */
#ifndef __cplusplus
#define static_assert _Static_assert
#endif

#endif /* ASSERT_H */
