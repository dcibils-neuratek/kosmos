#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Sandboxes: Alpine Linux, booted in a virtual machine on Kosmos.

`docs/virtualization.md` V3 - Diego, 12 October 2026: "the initial goal for
sandboxes is to run a linux distro like alpine". The ARM machine is started
at EL2 with the processor's virtualization on and a `/Home` disk holding
`Sandboxes/alpine` - Alpine 3.24.2's kernel and an initramfs of its
userland, made by `tools/alpine.py` from Alpine's published files, held to
their sums - and `sandbox run alpine` is typed at the prompt. Held to:

  * **Linux booting**: its banner, with Alpine's build in it;
  * **its interrupt controller**: the PL011 claimed as `ttyAMA0` through the
    emulated GICv3;
  * **its timer**, by what needs it: without the virtual timer's interrupt
    Linux stops before its userland, and the lines below never come. A
    timestamp is not the proof - it is read from the counter, which moves
    with no interrupt at all, and the first version of this suite checked
    it and passed without the timer;
  * **Alpine's userland**: its `/init` saying which Alpine and which kernel,
    and that it has one processor;
  * **switching itself off**: PSCI's SYSTEM_OFF, the monitor stopping, and
    the `sandbox` program back at the prompt with code 0.

The screen at the end is kept in `build/gate/arm-sandbox-linux.ppm`.

Usage: run_sandbox.py <image>
"""

import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    image = sys.argv[1]
    folder = scratch.path("sandbox-home")
    disk = scratch.path("sandbox-home.img")
    alpine = os.path.join(folder, "Sandboxes", "alpine")

    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(alpine)

    made = subprocess.run([sys.executable, os.path.join(HERE, "alpine.py"), alpine],
                          capture_output=True, text=True)

    if made.returncode != 0:
        print("FAIL: tools/alpine.py would not make Alpine's files:\n" + made.stdout + made.stderr)
        return 1

    made = subprocess.run([sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "128"],
                          capture_output=True, text=True)

    if made.returncode != 0:
        print("FAIL: homeimage.py would not make the disk:\n" + made.stdout + made.stderr)
        return 1

    R.virtualize("1G")
    R.extra_args(image, ["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                         "-device", R.device(image, "blk") + ",drive=home"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        guest.type("sandbox run alpine")
        deadline = time.monotonic() + 240

        while "sandbox: alpine ended" not in guest.seen[mark:] and time.monotonic() < deadline:
            time.sleep(0.5)
            guest._read_available()

        time.sleep(1)
        guest._read_available()
        said = guest.seen[mark:].replace("\r", "")

        with open(os.path.join("build", "gate", "arm-sandbox-linux.ppm"), "wb") as f:
            f.write(guest.screendump())

        check(re.search(r"Linux version 6\.18\.\S+-virt .*Alpine", said) is not None,
              "Linux's banner, with Alpine's build in it, never came")
        check("ttyAMA0 at MMIO 0x9000000" in said,
              "the PL011 was never claimed as ttyAMA0 - no interrupt controller for it")
        check(re.search(r"alpine: Alpine Linux 3\.24\.2, Linux 6\.18\.\S+-virt, in a Kosmos sandbox", said)
              is not None, "Alpine's /init never said what it is")
        check("alpine: 1 processor" in said, "Alpine did not see its one processor")
        check("vmm: the guest switched itself off" in said
              and "sandbox: alpine ended, code 0" in said,
              "the guest did not switch itself off and leave the sandbox with code 0")
    finally:
        transcript = guest.seen
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on a sandbox:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-4000:])
        return 1

    print("PASS: %d checks on a sandbox (Alpine Linux 3.24.2 booted at EL2 under "
          "Kosmos: Linux's banner, ttyAMA0 through the emulated GICv3, Alpine's "
          "/init - reached only with the virtual timer - saying what it is, and the "
          "guest switching itself off through PSCI)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
