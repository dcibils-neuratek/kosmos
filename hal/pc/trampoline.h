/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where a processor this board starts begins, and the two words it is given.
 *
 * `trampoline.S` is copied to TRAMPOLINE_BASE before the first STARTUP
 * inter-processor interrupt, whose vector is that page's number: a processor
 * started that way begins in real mode at vector * 0x1000. The kernel writes
 * the two words below into the copy before each start.
 */
#ifndef KOSMOS_HAL_PC_TRAMPOLINE_H
#define KOSMOS_HAL_PC_TRAMPOLINE_H

#define TRAMPOLINE_BASE     0x8000
#define TRAMPOLINE_VECTOR   (TRAMPOLINE_BASE >> 12)

/* From the start of the page: where the words live, both 32 bits. */
#define TRAMPOLINE_DATA     0x100
#define TRAMPOLINE_ENTRY    (TRAMPOLINE_DATA + 0)   /* jumped to, flat 32-bit */
#define TRAMPOLINE_CONTEXT  (TRAMPOLINE_DATA + 4)   /* handed over in edi */
#define TRAMPOLINE_REACHED  (TRAMPOLINE_DATA + 8)   /* its address, in esi */

/*
 * How far a started processor got, which it writes into TRAMPOLINE_REACHED
 * as it climbs - so that a machine with no serial port can say where one
 * stopped, rather than only that it did not arrive.
 *
 * The board writes the first two. The architecture is handed the word's
 * address in esi and writes the rest as numbers, without knowing what the
 * page is; these names are the protocol, kept on the side that prints them.
 */
#define REACHED_NOTHING      0
#define REACHED_REAL_MODE    1      /* trampoline.S, before anything */
#define REACHED_PROTECTED    2      /* trampoline.S, flat 32-bit */
#define REACHED_ENTRY32      3      /* start.S, _secondary_start32 */
#define REACHED_LONG_MODE    4      /* start.S, after paging */
#define REACHED_C            5      /* entry.c, x86_secondary_entry */
#define REACHED_GDT          6      /* its own GDT and TSS loaded */
#define REACHED_PAGE_TABLES  7      /* on the kernel's page tables */
#define REACHED_KERNEL       8      /* syscall and FP armed, into secondary_main */

#ifndef __ASSEMBLER__
extern const char trampoline_start[];
extern const char trampoline_end[];
#endif

#endif
