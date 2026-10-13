/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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
     * **.bss is not zeroed here, and it was.** The image carries it as
     * zeroes (`user.ld` emits it rather than leaving it NOLOAD) and the
     * kernel gives each page of the writable half as a copy of the image's
     * page there the first time it is touched (`process_fill`), so it is
     * already zero. A loop writing it again touched every page of it in
     * every process - 3.4 MB each, most of it FFmpeg's tables in processes
     * that never decode anything - so as not to depend on how the image was
     * built. That is the kernel's guarantee now, and the kernel suite's
     * `el0: memory is given on first touch` holds it. The boot word is
     * still where the kernel put it, since nothing here uses a register.
     */
    bl      main

    /* main returned. Exit with whatever it gave back rather than falling
     * into the bytes that happen to follow. */
    mov     x8, #0                  /* SYS_EXIT */
    svc     #0
3:  b       3b
