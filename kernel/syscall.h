#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H


/*
 * The syscall interface.
 *
 * `svc #0`, with the number in x8 and arguments in x0 through x5, and the
 * result back in x0. That is the AArch64 Linux convention, borrowed because
 * it is the one every tool and every reader already knows; nothing else about
 * it is inherited.
 *
 * x8 rather than x0 for the number so the arguments start where the C calling
 * convention already puts them, which means a syscall stub is a `mov` and an
 * `svc` rather than a shuffle.
 *
 * These are not the interface Kosmos ends up with. `design.md` §4.4 has
 * processes reaching resources by name through a namespace, and at M5 most of
 * what is here becomes `list`, `read` and `write` against a path. What
 * survives is what genuinely cannot be a message: entering and leaving the
 * kernel, and the capability operations that everything else is expressed in.
 */

#define SYS_EXIT        0   /* (code)                    never returns */
#define SYS_WRITE       1   /* (ptr, len)             -> bytes written */
#define SYS_YIELD       2   /* ()                                      */
#define SYS_ENDPOINT    3   /* ()                     -> cap or error  */
#define SYS_CALL        4   /* (cap, msg, reply)      -> 0 or error    */
#define SYS_RECEIVE     5   /* (cap, msg, &sender)    -> 0 or error    */
#define SYS_REPLY       6   /* (sender, msg)          -> 0 or error    */

#define SYS_MAX         7

/* Errors are negative so `if (result < 0)` reads correctly on both sides. */
#define SYS_ERR_BADCALL   (-100)    /* no such syscall number */
#define SYS_ERR_FAULT     (-101)    /* a pointer the process may not touch */

/*
 * Everything above is plain preprocessor because user programs written in
 * assembly include this header for the numbers. Anything below would be a
 * stream of unknown mnemonics to the assembler.
 */
#ifndef __ASSEMBLER__

#include <stdint.h>

struct trapframe;

/* Called from the trap handler on an SVC from EL0. Writes the result back
 * into the frame, which is where the eret will take x0 from. */
void syscall_dispatch(struct trapframe *tf);

#endif /* !__ASSEMBLER__ */

#endif /* KERNEL_SYSCALL_H */
