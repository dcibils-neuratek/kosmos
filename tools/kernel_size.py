#!/usr/bin/env python3
"""How big is the kernel, against the budget CLAUDE.md sets?

The budget is 10,000 lines and it is counted **without comments or blank
lines**, which is a distinction worth stating because it changes the answer
by more than a factor of two. `design.md` says what the number is for - "if
it goes past 10k lines, something crept in" - so it is a tripwire for scope,
not a limit on explaining yourself. This codebase explains itself at length
on purpose, and a budget that punished that would be a budget that made the
code worse in order to satisfy itself.

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
        print(f"OVER the {BUDGET}-line budget by {-left} lines of code.")
        print("CLAUDE.md: if something pushes past that, it goes to userland.")
        return 1

    print(f"{grand_code} of {BUDGET} lines of code. {left} left.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
