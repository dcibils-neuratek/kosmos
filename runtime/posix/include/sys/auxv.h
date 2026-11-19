/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef SYS_AUXV_H
#define SYS_AUXV_H

/*
 * `getauxval`, for ported code asking what the processor can do (`docs/w5.md`
 * W5d: OpenSSL's `armcap.c` chooses AES, SHA and PMULL from it). Kosmos has
 * no auxiliary vector; the answer is computed from the ID registers Nebula
 * hands every process through `sysinfo` (`process.c`), in Linux's bits, the
 * ones such code is written against. `AT_HWCAP` and `AT_HWCAP2` on ARM only:
 * an x86-64 program asks CPUID itself. `AT_SECURE` - started with more
 * authority than its starter, as setuid does - is 0 everywhere: nothing here
 * can be (OpenSSL's `uid.c` asks). `AT_PAGESZ` is a page's bytes, 4096
 * on both processors (LLVM's `Process::getPageSize` asks, `docs/gpu3d.md`
 * G1). Anything else is 0, ENOENT.
 */

#define AT_PAGESZ 6
#define AT_HWCAP  16
#define AT_SECURE 23
#define AT_HWCAP2 26

/* The numbers alone for assembly, which OpenSSL's ARM build includes this
 * into with the rest (`openssl-kosmos.conf`). */
#ifndef __ASSEMBLER__
#include <sys/cdefs.h>

__BEGIN_DECLS

unsigned long getauxval(unsigned long type);

__END_DECLS
#endif

#endif /* SYS_AUXV_H */
