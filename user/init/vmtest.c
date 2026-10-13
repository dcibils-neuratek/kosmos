/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A monitor in miniature**, on either architecture: a guest made empty, a
 * region given to it, a few instructions written into it through this
 * process's own mapping of the same region, and the guest run until it
 * stops twice - once touching what nothing maps, once on purpose.
 *
 * The guest reads the number the monitor left in its data page, writes one
 * more beside it, and loads from an address nothing maps: it stops with
 * that address, and the monitor sees the guest's write in its own memory.
 * Stepped past the load, it stops again on an instruction that always
 * traps. A region capability that names nothing is refused.
 *
 * The code names the first thing answered wrongly; 50 is "this machine
 * cannot enter a guest" - an ARM one started below EL2, or a PC without
 * VT-x - and `detail`, when asked for, says what the vCPU said last: the
 * exit reason or syndrome, the VM instruction error, the address, and
 * where the guest was.
 *
 * **Two callers**: the test image's guest-side suite, as `CTEST_GUEST`, on
 * ARM under QEMU, which emulates EL2; and `ROLE_VMTEST` in the image a
 * machine boots, started by init when the boot line says
 * `opt/kosmos/vmtest`, because VT-x is not something QEMU's TCG emulates -
 * the x86-64 half is proved on a real PC or not at all
 * (`docs/sandboxes.md`, X1).
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "init/say.h"

static long run_guest(long vcpu, struct vcpu_regs *r)
{
    long why;

    while ((why = kosmos_vcpu_run(vcpu, r)) == VCPU_EXIT_HOST) {
        /* The host's tick: taken on the way back here; run it again. */
    }

    return why;
}

#if defined(__x86_64__)
/*
 * **On x86-64**, five pages at guest address 0x10000 - code, data, and
 * the guest's own page tables, a PML4, a PDPT and a page directory of 2 MB
 * pages over its first gigabyte - so it starts in long mode, as Linux's
 * 64-bit boot protocol has it start. Its first gigabyte is mapped by its
 * own tables and only five pages of it by EPT: the load from 0x200000 is a
 * translation the guest has and the machine does not, which is an EPT
 * violation. `hlt` always exits.
 */
#define AT          0x10000ul

#define EXIT_HLT            12u
#define EXIT_EPT_VIOLATION  48u

static void say_detail(uint64_t detail[4], const struct vcpu_regs *r)
{
    if (detail != NULL) {
        detail[0] = r->reason;
        detail[1] = r->error;
        detail[2] = r->gpa;
        detail[3] = r->rip;
    }
}

int vm_selftest(uint64_t detail[4])
{
    static const uint8_t code[] = {
        0x48, 0x8b, 0x01,               /* mov rax, [rcx]     the monitor's */
        0x48, 0x83, 0xc0, 0x01,         /* add rax, 1                       */
        0x48, 0x89, 0x41, 0x08,         /* mov [rcx+8], rax   the guest's   */
        0x48, 0x8b, 0x13,               /* mov rdx, [rbx]     nothing there */
        0xf4,                           /* hlt                              */
    };
    struct vcpu_regs r;
    long guest, region, at, vcpu;
    volatile uint64_t *data;
    uint64_t *pml4, *pdpt, *pd;
    unsigned i;

    guest = kosmos_guest_create();

    if (guest == SYS_ERR_NO_DEVICE) {
        return 50;
    }

    if (guest < 0 || (region = kosmos_mem_create(5)) < 0
        || (at = kosmos_mem_map(region)) < 0) {
        return 1;
    }

    memcpy((void *)at, code, sizeof(code));
    data = (volatile uint64_t *)(at + 0x1000);
    pml4 = (uint64_t *)(at + 0x2000);
    pdpt = (uint64_t *)(at + 0x3000);
    pd = (uint64_t *)(at + 0x4000);
    data[0] = 0x1234;
    pml4[0] = (AT + 0x3000) | 3;                    /* present, writable */
    pdpt[0] = (AT + 0x4000) | 3;

    for (i = 0; i < 512; i++) {
        pd[i] = ((uint64_t)i << 21) | 0x83;         /* 2 MB, writable, present */
    }

    if (kosmos_guest_map(guest, 999, AT) != SYS_ERR_DENIED) {
        return 2;
    }

    if (kosmos_guest_map(guest, region, AT) != 0
        || (vcpu = kosmos_vcpu_create(guest)) < 0) {
        return 3;
    }

    memset(&r, 0, sizeof r);
    r.rip = AT;
    r.rflags = 2;
    r.cr0 = 0x80000031ul;                   /* PG, NE, ET, PE */
    r.cr3 = AT + 0x2000;
    r.cr4 = 0x20;                           /* PAE */
    r.efer = 0x500;                         /* LME, LMA */
    r.flags = VCPU_SET_SYSTEM;
    r.gpr[VCPU_GPR_RCX] = AT + 0x1000;      /* its data page */
    r.gpr[VCPU_GPR_RBX] = 0x200000;         /* nothing */

    if (run_guest(vcpu, &r) != VCPU_EXIT_TRAP) {
        say_detail(detail, &r);
        return 4;
    }

    say_detail(detail, &r);

    if (r.reason != EXIT_EPT_VIOLATION || r.gpa != 0x200000 || r.rip != AT + 11
        || (r.qualification & 1) == 0) {
        return 5;
    }

    if (data[1] != 0x1235 || r.gpr[VCPU_GPR_RAX] != 0x1235) {
        return 6;
    }

    /*
     * Stepped by its known length: the exit's instruction length is defined
     * for exits an instruction causes and not for an EPT violation, which
     * is why the monitor decodes the instruction it stopped on (X2).
     */
    r.rip += 3;
    r.flags = 0;

    if (run_guest(vcpu, &r) != VCPU_EXIT_TRAP) {
        say_detail(detail, &r);
        return 7;
    }

    say_detail(detail, &r);

    if (r.reason != EXIT_HLT || r.rip != AT + 14) {
        return 8;
    }

    return 0;
}
#else
/*
 * **On AArch64**, two pages at guest address 0x40000000 - code, then data
 * - entered at EL1 with its translation off. The load from 0x50000000 is a
 * stage-2 fault; `hvc #7` always traps to EL2.
 */
int vm_selftest(uint64_t detail[4])
{
    static const uint32_t code[] = {
        0xf9400022,                     /* ldr x2, [x1]     the monitor's   */
        0x91000442,                     /* add x2, x2, #1                   */
        0xf9000422,                     /* str x2, [x1, #8] the guest's     */
        0xf9400083,                     /* ldr x3, [x4]     nothing there   */
        0xd40000e2,                     /* hvc #7                           */
    };
    struct vcpu_regs r;
    long guest, region, at, vcpu;
    volatile uint64_t *data;

    guest = kosmos_guest_create();

    if (guest == SYS_ERR_NO_DEVICE) {
        return 50;
    }

    if (guest < 0 || (region = kosmos_mem_create(2)) < 0
        || (at = kosmos_mem_map(region)) < 0) {
        return 1;
    }

    memcpy((void *)at, code, sizeof(code));
    data = (volatile uint64_t *)(at + 4096);
    data[0] = 0x1234;

    if (kosmos_guest_map(guest, 999, 0x40000000UL) != SYS_ERR_DENIED) {
        return 2;
    }

    if (kosmos_guest_map(guest, region, 0x40000000UL) != 0
        || (vcpu = kosmos_vcpu_create(guest)) < 0) {
        return 3;
    }

    memset(&r, 0, sizeof r);
    r.pc = 0x40000000UL;
    r.pstate = 0x3c5;                   /* EL1h, D A I F masked */
    r.x[1] = 0x40001000UL;              /* its data page */
    r.x[4] = 0x50000000UL;              /* nothing */

    if (run_guest(vcpu, &r) != VCPU_EXIT_TRAP || (r.syndrome >> 26) != 0x24
        || r.ipa != 0x50000000UL || r.pc != 0x4000000cUL) {
        if (detail != NULL) {
            detail[0] = r.syndrome, detail[1] = 0, detail[2] = r.ipa, detail[3] = r.pc;
        }

        return 4;
    }

    if (data[1] != 0x1235) {
        return 5;
    }

    r.pc += 4;

    if (run_guest(vcpu, &r) != VCPU_EXIT_TRAP || (r.syndrome >> 26) != 0x16
        || (r.syndrome & 0xffff) != 7) {
        if (detail != NULL) {
            detail[0] = r.syndrome, detail[1] = 0, detail[2] = r.ipa, detail[3] = r.pc;
        }

        return 6;
    }

    return 0;
}
#endif

/*
 * **Said, and ended with the answer as its code**: one line on the
 * console, which a machine with nothing but a network shows through
 * `log` - "vmtest: a guest ran and stopped as it should", or the check
 * that failed and what the vCPU said last.
 */
void vmtest_role(long console)
{
    uint64_t detail[4] = { 0, 0, 0, 0 };
    struct say_line line;
    int code = vm_selftest(detail);

    say_begin(&line);

    if (code == 0) {
        say_text(&line, "vmtest: a guest ran and stopped as it should");
    } else if (code == 50) {
        say_text(&line, "vmtest: this machine cannot enter a guest");
    } else {
        say_text(&line, "vmtest: failed at ");
        say_dec(&line, (unsigned long)code);
        say_text(&line, ", reason ");
        say_hex(&line, detail[0], 8);
        say_text(&line, ", error ");
        say_dec(&line, detail[1]);
        say_text(&line, ", address ");
        say_hex(&line, detail[2], 0);
        say_text(&line, ", at ");
        say_hex(&line, detail[3], 0);
    }

    say_send(console, &line);
    kosmos_exit(code);
}
