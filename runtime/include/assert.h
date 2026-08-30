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

#endif /* ASSERT_H */
