#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Runs the machine hard for a while and asks whether it gave everything back.

`make test` checks components from inside. `make screenshot` drives the
machine the way a person does - once. Both pass on a system that works
perfectly the first time and runs out of something the fiftieth, and every
resource bug this project has had was exactly that shape:

  * capability slots ran out on the *sixteenth* ranged read
  * a region was allocated per font per size and never released, so page two
    of a document failed while page one was fine
  * two `memobj` races needed a second thread allocating while the first was
    freeing
  * the process table filled at round twenty-two, which this test found on
    its first run

None of those is visible in one pass. All are obvious in a loop that counts.

The counting is the test. `sys.info` reports every fixed pool the kernel has,
so "nothing leaked" is a number rather than an impression.

**Not run on every build.** It boots a machine and works it for minutes;
`make test` stays the fast gate. This is the gate on a *release* - a binary
that goes in `builds/` and gets run on another computer has to have survived
being used for a while first.
"""

import subprocess
import sys
import time

QEMU = "qemu-system-aarch64"

ARGS = [
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a72",
    "-m", "512M",
    "-nographic",
    "-global", "virtio-mmio.force-legacy=false",
]


def run(image, rounds, timeout):
    cmd = list(ARGS)
    cmd += ["-fw_cfg", "name=opt/kosmos/boot,string=stress %d" % rounds]
    cmd += ["-kernel", image]

    p = subprocess.Popen([QEMU] + cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)

    import os
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

            if b"STRESS PASS" in out or b"STRESS FAIL" in out:
                time.sleep(0.3)
                out += p.stdout.read() or b""
                break
    finally:
        p.kill()
        p.wait()

    return out.decode("utf-8", "replace")


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 60

    out = run(image, rounds, 600.0)

    # The pool counts, which are the interesting part whether it passed or
    # not: a run that passes with the numbers printed is a run somebody can
    # check, and one that only says PASS is a claim.
    for line in out.splitlines():
        line = line.rstrip()

        if line.startswith("  after ") or line.startswith("  regions") \
           or line.startswith("  endpoints") or line.startswith("  threads") \
           or line.startswith("  processes") or line.startswith("  pages free") \
           or "STRESS" in line:
            print(line)

    if "STRESS PASS" in out:
        return 0

    if "STRESS FAIL" in out:
        print("\nFAIL: the machine did not survive being used.")
        return 1

    print("\nFAIL: the machine never reported. It hung, or never booted.")

    for line in out.splitlines():
        if "PANIC" in line or "could not start" in line:
            print("  the machine said: " + line.strip())

    return 1


if __name__ == "__main__":
    sys.exit(main())
