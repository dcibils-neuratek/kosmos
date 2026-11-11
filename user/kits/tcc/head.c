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

/*
 * **What `user.ld` names and TinyCC cannot** (`docs/webkit.md` W1b): an
 * image TinyCC links has no thread variables' template, since TinyCC reads
 * no linker script and has no `_Thread_local` - so it says so, under the
 * names `runtime/libc/tls.c` reads: a template of no bytes. The constructor
 * list's two names TinyCC's own linker gives, as `.init_array`'s bounds.
 */
__asm__(".section .rodata\n"
        ".balign 8\n"
        ".globl __tls_align\n"
        "__tls_align: .quad 0\n"
        ".globl __tdata_start\n.globl __tdata_end\n.globl __tbss_end\n"
        "__tdata_start:\n__tdata_end:\n__tbss_end:\n");
