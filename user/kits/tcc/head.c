/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The sixteen bytes Kosmos's header takes at the base of an image TinyCC
 * links (`docs/tinycc.md`, step C1). `user/user.ld` writes them for GCC's
 * images; TinyCC reads no linker script, so this object is linked first, its
 * `.text.start` coming before the runtime's - so `_start` lands at the base
 * plus sixteen, where the kernel starts every image - and `tcc_stamp`
 * (`stamp.c`) fills them in once the link has said how much is code.
 */

__asm__(".section .text.start,\"ax\"\n"
        ".quad 0\n"                 /* "KOSMOS", once stamped */
        ".quad 0\n");               /* bytes that are read-only and executable */
