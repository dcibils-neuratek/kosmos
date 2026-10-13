#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The virtualization probe (`docs/sandboxes.md`, S5): said at boot.

Before VT-x is written, the kernel says what the processor and its firmware
allow - VT-x or not, locked on or off - so a netboot of the M700 tells
whether its setup needs changing. QEMU's emulated x86 has no VMX, so here the
answer is "nothing to run a guest with", and the probe must neither fault
nor stay silent getting it.

Usage: run_virtprobe.py IMAGE
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    guest = R.Guest(image, 120)

    try:
        said = guest.wait_for_line("vm: ", "the kernel to say what it can run guests with")
        prompt = guest.wait_for("kosmos> ", "the machine to reach its prompt after the probe")
    except Exception as e:                  # noqa: BLE001 - said below
        print("FAIL: the virtualization probe: %s" % str(e).splitlines()[0])
        return 1
    finally:
        guest.close()

    del prompt

    if said.strip() != "nothing to run a guest with":
        print("FAIL: QEMU's x86 has no VMX, and the probe said %r" % said)
        return 1

    print("PASS: 2 checks on the virtualization probe (said at boot: nothing to run a guest "
          "with, as QEMU's x86 has no VMX; and the machine went on to its prompt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
