/*
 * Where a process begins.
 *
 * There is no loader, no dynamic linker and no argv. The kernel maps the
 * image, points the stack at the top of the region, and erets here with
 * every register zeroed.
 */

.section .text.start, "ax"
.global _start

_start:
    /*
     * Zero .bss. The image is a flat binary and the linker script emits .bss
     * as zeroes, so this is already true; doing it anyway costs a handful of
     * instructions once and means the image stops depending on that
     * particular property of how it was built.
     */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:
    cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    bl      main

    /* main returned. Exit with whatever it gave back rather than falling
     * into the bytes that happen to follow. */
    mov     x8, #0                  /* SYS_EXIT */
    svc     #0
3:  b       3b
