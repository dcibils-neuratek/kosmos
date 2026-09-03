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


/*
 * The two a port asks about by name.
 *
 * `EISDIR` is how Doom decides a failed `fopen` means "that is a directory,
 * so it exists" - the only place it inspects errno rather than a return
 * value. `ENOTDIR` is its mirror and comes with it so the pair is not half
 * defined.
 */
#ifndef EISDIR
#define EISDIR   21
#endif
#ifndef ENOTDIR
#define ENOTDIR  20
#endif

#endif /* ERRNO_H */
