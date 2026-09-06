#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Boots the x86-64 image and checks what the second architecture built.

`arch/x86_64/main.c` is a staging kmain: it walks through everything the
port has - the memory map, the page tables, an address space, a context
switch, a process at ring 3 - and prints what it found at each step. That
made the bring-up visible and it proved nothing that would stay proved.

**So this does not read the sentences back.** Every check here compares two
things that were arrived at separately: a page table entry against the
address the linker chose for that section, the allocator's page count
against the memory map the firmware reported, a printed answer against the
arithmetic done here, and a fault's error code against what the *processor*
pushed rather than against what the kernel said about it.

That distinction is the one `run_headless.py` learned the expensive way: it
counts `/bin` on this side and on that side, because a listing that agrees
with itself agrees with itself no matter how wrong it is.
"""

import os
import re
import subprocess
import sys
import time

QEMU = "qemu-system-x86_64"
NM = "x86_64-elf-nm"

# `make x86`'s own line. What a person runs, not a shape invented for a test.
ARGS = [
    "-M", "q35",
    "-m", "512M",
    "-nographic",
    "-no-reboot",
]

PAGE_SIZE = 4096

# Entry bits, from arch/x86_64/mmu.h. Repeated here on purpose: this file is
# the second opinion, and a second opinion that includes the first one's
# header is not one.
PTE_P = 1 << 0
PTE_RW = 1 << 1
PTE_US = 1 << 2
PTE_NX = 1 << 63
PTE_ADDR = 0x000FFFFFFFFFF000

# Page fault error code bits, Intel SDM volume 3, section 4.7.
ERR_PRESENT = 1 << 0
ERR_WRITE = 1 << 1
ERR_USER = 1 << 2


def boot(image, timeout):
    """Runs until the machine halts, and returns everything it printed."""
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")

    if not os.path.exists(binary):
        print("FAIL: no %s beside the ELF. Run `make x86-build`." % binary)
        return None

    p = subprocess.Popen([QEMU] + ARGS + ["-kernel", binary],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL)
    os.set_blocking(p.stdout.fileno(), False)

    out = b""
    start = time.time()

    try:
        while time.time() - start < timeout:
            chunk = p.stdout.read()

            if chunk:
                out += chunk
            else:
                time.sleep(0.05)

            # Either ending: the deliberate fault, or the line that says
            # the deliberate fault did not happen. Waiting only for the
            # first turns "CR0.WP is off" into a timeout, and a timeout
            # names nothing - which is the opposite of what a test is for.
            if b"halted." in out or b"the write succeeded" in out:
                # A moment more, so nothing that arrives just after is cut off.
                time.sleep(0.3)
                out += p.stdout.read() or b""
                break
    finally:
        p.kill()
        p.wait()

    return out.decode("utf-8", "replace")


def symbols(image):
    """The addresses the linker chose, read from the ELF rather than the run.

    This is the half of every comparison below that the machine under test
    had no part in.
    """
    try:
        out = subprocess.run([NM, image], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print("FAIL: could not read symbols with %s: %s" % (NM, e))
        return None

    found = {}

    for line in out.splitlines():
        parts = line.split()

        if len(parts) == 3:
            found[parts[2]] = int(parts[0], 16)

    return found


def value(out, label):
    """The first hex word on the line beginning `  <label>`."""
    for line in out.splitlines():
        stripped = line.strip()

        if stripped.startswith(label):
            m = re.search(r"0x([0-9a-f]+)", stripped[len(label):])

            if m:
                return int(m.group(1), 16)

    return None


def fault(out, which):
    """One of the two deliberate faults, as the fields the processor pushed.

    `which` is 0 for the first block in the output and 1 for the second.
    Returns a dict of the register lines under it, or None.
    """
    blocks = []
    current = None

    for line in out.splitlines():
        stripped = line.strip()

        if stripped.startswith("process died:") or stripped.startswith("*** "):
            current = {"header": stripped}
            blocks.append(current)
            continue

        if current is None:
            continue

        m = re.match(r"([a-z0-9]+)\s+0x([0-9a-f]+)$", stripped)

        if m:
            current[m.group(1)] = int(m.group(2), 16)
        elif stripped == "":
            current = None

    return blocks[which] if len(blocks) > which else None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"

    sym = symbols(image)

    if sym is None:
        return 1

    for name in ("__text_start", "__rodata_start", "__stack_guard"):
        if name not in sym:
            print("FAIL: the ELF has no %s. The linker script changed." % name)
            return 1

    out = boot(image, 40.0)

    if out is None:
        return 1

    if "halted." not in out and "the write succeeded" not in out:
        print("FAIL: the machine stopped before the end of its own bring-up.")
        print("  last of what it did say: " + repr(out[-300:]))
        return 1

    if "PANIC" in out:
        print("FAIL: it panicked: "
              + [l for l in out.splitlines() if "PANIC" in l][0].strip())
        return 1

    checks = 0
    fails = []

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    # 1. Long mode, from the two bits that mean different things.
    #
    #    EFER.LME is what was *asked* for and CR0.PG is what the processor
    #    did about it. They are set by different instructions, and a boot
    #    that got one and not the other is the failure worth naming.
    cr0 = value(out, "cr0")
    efer = value(out, "efer")

    check(cr0 is not None and (cr0 >> 31) & 1, "paging is off")
    check(efer is not None and (efer >> 10) & 1, "long mode is not active")

    # 2. The allocator manages the RAM the firmware reported.
    #
    #    `ram at` comes from the multiboot memory map and `pmm` from the
    #    allocator's own bitmap. Two answers to the same question, from two
    #    subsystems that do not consult each other.
    m = re.search(r"ram at\s+0x([0-9a-f]+) for 0x([0-9a-f]+)", out)
    span = (int(m.group(1), 16), int(m.group(2), 16)) if m else None

    m = re.search(r"pmm\s+0x([0-9a-f]+) of 0x([0-9a-f]+) pages free", out)
    free, total = (int(m.group(1), 16), int(m.group(2), 16)) if m else (0, 0)

    check(span is not None and total == span[1] // PAGE_SIZE,
          "the allocator manages %d pages and the memory map reports %s"
          % (total, span[1] // PAGE_SIZE if span else "nothing"))

    check(0 < free < total,
          "the allocator has %d of %d pages free, which is not a boot"
          % (free, total))

    # 3. The map is an identity map, and the two narrowed regions are
    #    narrowed. Both entries are checked against the address the *linker*
    #    put the section at, so a map that is self-consistently wrong fails.
    text = value(out, "text")
    rodata = value(out, "rodata")

    check(text is not None and (text & PTE_ADDR) == sym["__text_start"],
          "the entry for .text points at 0x%x and the linker put it at 0x%x"
          % ((text or 0) & PTE_ADDR, sym["__text_start"]))

    check(text is not None
          and (text & PTE_P) and not (text & PTE_RW) and not (text & PTE_NX),
          "'.text' is not present, read-only and executable: 0x%x" % (text or 0))

    check(rodata is not None and (rodata & PTE_ADDR) == sym["__rodata_start"],
          "the entry for .rodata points at 0x%x and the linker put it at 0x%x"
          % ((rodata or 0) & PTE_ADDR, sym["__rodata_start"]))

    check(rodata is not None
          and (rodata & PTE_P) and not (rodata & PTE_RW) and (rodata & PTE_NX),
          "'.rodata' is not present, read-only and non-executable: 0x%x"
          % (rodata or 0))

    # 4. The stack guard, at the address the linker chose for it.
    guard = value(out, "guard")

    check(guard == sym["__stack_guard"],
          "the guard page reported is 0x%x and the linker put it at 0x%x"
          % (guard or 0, sym["__stack_guard"]))

    check("unmapped" in out, "the stack guard is still mapped")

    # 5. An address space: a page ring 3 may write, the kernel still in it,
    #    and every page handed back.
    user = value(out, "as ")

    check(user is not None
          and (user & PTE_US) and (user & PTE_RW) and (user & PTE_NX),
          "a user page is not present, writable, user and non-executable: "
          "0x%x" % (user or 0))

    check("shared with the kernel" in out,
          "an address space does not contain the kernel")

    check("all returned" in out, "an address space leaked pages")

    # 6. The context switch.
    check("two round trips" in out, "the context switch did not round trip")

    # 7. Ring 3, and the arithmetic done here rather than read back.
    m = re.search(r"ring3\s+0x([0-9a-f]+) from 0x([0-9a-f]+)", out)
    answer, marker = (int(m.group(1), 16), int(m.group(2), 16)) if m else (0, 1)

    check(m is not None and answer == 2 * marker,
          "a process asked for 0x%x doubled and got 0x%x" % (marker, answer))

    # 8. And what it may not do, from what the *processor* pushed.
    #
    #    This is the check the whole architecture exists to pass, and none
    #    of it is the kernel's opinion: cs is the privilege level the
    #    processor was at, cr2 is where the access went, and the error code
    #    is its own account of why it refused.
    user_fault = fault(out, 0)

    check(user_fault is not None and (user_fault.get("cs", 0) & 3) == 3,
          "the first fault did not come from ring 3")

    check(user_fault is not None
          and user_fault.get("cr2") == sym["__text_start"],
          "a process faulted at 0x%x and the kernel's text is at 0x%x"
          % ((user_fault or {}).get("cr2", 0), sym["__text_start"]))

    err = (user_fault or {}).get("error", 0)

    check(err & ERR_PRESENT and err & ERR_USER and not err & ERR_WRITE,
          "the kernel's text was not present-and-unreadable to a process: "
          "error 0x%x" % err)

    # 9. And the kernel's own read-only text, which is a *different* error
    #    code from the same address: present and written, at ring 0. Without
    #    CR0.WP that write succeeds and there is no fault here at all.
    kernel_fault = fault(out, 1)

    check(kernel_fault is not None and (kernel_fault.get("cs", 0) & 3) == 0,
          "the second fault did not come from ring 0")

    err = (kernel_fault or {}).get("error", 0)

    check(err & ERR_PRESENT and err & ERR_WRITE and not err & ERR_USER,
          "the kernel wrote to its own read-only text without a protection "
          "fault: error 0x%x. CR0.WP is what makes that fault." % err)

    if fails:
        print("FAIL: %d of %d checks on x86-64:" % (len(fails),
                                                    len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on x86-64 (long mode, the memory map against the "
          "allocator, an identity map with .text and .rodata narrowed, a "
          "guard page, an address space that leaks nothing, a context "
          "switch, and a process at ring 3 that cannot read the kernel)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
