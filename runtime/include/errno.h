#ifndef ERRNO_H
#define ERRNO_H

/*
 * errno is a function, not a variable, and that is deliberate.
 *
 * `design.md` §17.3 calls this out as a detail that causes bugs months
 * later: a global errno does not survive coroutines, and from M4 it must be
 * per process. Making it an accessor from the start means the storage can
 * move into the process state without a single caller changing.
 *
 * newlib's libm calls `__errno` for exactly this reason, so its convention
 * and the design's requirement happen to be the same one.
 */
int *__errno(void);

#define errno   (*__errno())

#define EDOM    33
#define ERANGE  34

#endif /* ERRNO_H */
