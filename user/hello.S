/*
 * The first thing Kosmos runs at EL0.
 *
 * Assembly rather than C, and position independent, because it is copied
 * into a fresh page and mapped at an address that has nothing to do with
 * where it was linked. `adr` is PC-relative, so the string is found wherever
 * the blob ends up; an absolute reference would point back into the kernel
 * image, which this process cannot reach and should not be able to.
 *
 * It has no libc, no stack frame, and no way home. It writes, and it exits.
 */

#include "syscall.h"

.section .user.text, "ax"

.global user_hello_start
user_hello_start:
    adr     x0, message
    mov     x1, #(message_end - message)
    mov     x8, #SYS_WRITE
    svc     #0

    /* And again, with the result of the first call, to prove the return
     * value comes back across the boundary rather than being ignored. */
    cmp     x0, #(message_end - message)
    b.ne    wrong

    mov     x0, #0
    mov     x8, #SYS_EXIT
    svc     #0

wrong:
    mov     x0, #1
    mov     x8, #SYS_EXIT
    svc     #0

    /* SYS_EXIT does not return. If it ever did, spinning here is better than
     * running off the end of the page into whatever follows. */
1:  b       1b

message:
    .ascii  "hello from EL0\n"
message_end:

.global user_hello_end
user_hello_end:
