#!/usr/bin/env python3
"""How big is the kernel?

Reported, not enforced. `CLAUDE.md` is explicit that the 10,000-line figure
is a smoke alarm rather than a rule: what the kernel may *contain* is the
rule - threads, address spaces, IPC, capabilities, and no allocator - and
the size is a symptom of it. Nothing should ever leave the kernel to satisfy
this number. Things leave because they do not belong.

Counted without comments or blank lines, which changes the answer by more
than a factor of two, and this codebase explains itself at length on
purpose.

Both numbers are printed anyway. The comment ratio is interesting on its own,
and hiding half the file from the only tool that measures it would be a
strange thing to do in a repository that cares this much about comments.

What counts as the kernel: everything linked into the kernel image. That is
kernel/, arch/, hal/ and boot/, headers and assembly included - not
runtime/, not user/, not lua/, none of which run at EL1.
"""

import glob
import re
import sys

BUDGET = 10000

GROUPS = {
    "kernel/":      ["kernel/*.c", "kernel/*.h"],
    "arch/aarch64/": ["arch/aarch64/*.c", "arch/aarch64/*.h",
                      "arch/aarch64/*.S"],
    "hal/":         ["hal/*.h", "hal/qemu-virt/*.c", "hal/qemu-virt/*.h"],
    "boot/":        ["boot/*.S"],
}


def measure(path):
    """(total lines, code lines) for one file.

    Block comments are tracked across lines, and a `/*` inside a string
    literal would fool this. There is no such thing in this tree, and a
    counter that needed a C parser to be trusted would be a worse tool than
    reading the number with that caveat attached.
    """
    total = code = 0
    in_block = False

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            total += 1
            stripped = line.strip()

            if in_block:
                if "*/" in stripped:
                    in_block = False
                    stripped = stripped.split("*/", 1)[1].strip()
                else:
                    continue

            # Assembly comments as well as C ones: boot/ and the vectors are
            # .S files and they are as heavily commented as everything else.
            stripped = re.sub(r"//.*$", "", stripped)
            stripped = re.sub(r"/\*.*?\*/", "", stripped)

            if "/*" in stripped:
                in_block = True
                stripped = stripped.split("/*", 1)[0]

            if stripped.startswith("*"):
                continue

            if not stripped or stripped.startswith("#") and False:
                continue

            if not stripped:
                continue

            code += 1

    return total, code


def main():
    grand_total = grand_code = 0
    rows = []

    for name, patterns in GROUPS.items():
        files = sorted({p for pat in patterns for p in glob.glob(pat)})
        total = code = 0

        for path in files:
            t, c = measure(path)
            total += t
            code += c

        rows.append((name, len(files), total, code))
        grand_total += total
        grand_code += code

    print(f"{'':16} {'files':>6} {'lines':>8} {'code':>8} {'comment':>8}")

    for name, files, total, code in rows:
        share = 100 * (total - code) / total if total else 0
        print(f"{name:16} {files:6} {total:8} {code:8} {share:7.0f}%")

    share = 100 * (grand_total - grand_code) / grand_total if grand_total else 0
    print(f"{'':16} {'':6} {'':8} {'':8} {'':8}")
    print(f"{'the kernel':16} {'':6} {grand_total:8} {grand_code:8} {share:7.0f}%")

    left = BUDGET - grand_code
    print()

    if left < 0:
        print(f"{grand_code} lines of code, {-left} past the {BUDGET} the "
              "smoke alarm is set at.")
        print("Worth a look for what crept in. If nothing did, the number "
              "was the wrong thing to look at.")
        return 0

    print(f"{grand_code} of {BUDGET} lines of code. {left} before the smoke "
          "alarm.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
