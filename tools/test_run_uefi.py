#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`run_uefi.py` on a machine where it cannot boot anything.

**A skip that had never run.** `capture()` answered four values when it found
no OVMF and `main` unpacked two, so the `SKIP` written for a machine without
the firmware was a `ValueError`. And the same `None` from a boot that did
start and gave no picture was printed as a skip as well, so a gate whose
screendump failed passed without a single check.

So this runs the harness with its firmware and its boots replaced, and asks
what it says. Nothing is booted.

Usage: test_run_uefi.py
"""

import contextlib
import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_uefi  # noqa: E402


def run(argv):
    """`main` with these arguments: its exit code, what it printed, and what
    it raised - which is the failure this file was written about."""
    out = io.StringIO()
    saved = sys.argv

    try:
        sys.argv = argv

        with contextlib.redirect_stdout(out):
            code = run_uefi.main()

        return code, out.getvalue(), None
    except Exception as e:
        return None, out.getvalue(), e
    finally:
        sys.argv = saved


def main():
    checks = 0
    fails = []

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    real_firmware, real_capture = run_uefi.firmware, run_uefi.capture
    handle, image = tempfile.mkstemp(prefix="kosmos-uefi-skip-", suffix=".img")
    os.close(handle)

    try:
        #
        # 1. With no firmware, `capture` answers in the two values `main`
        #    takes from it - the bug, stated as the contract it broke.
        #
        run_uefi.firmware = lambda: (None, "OVMF is not installed beside qemu")

        try:
            answer = run_uefi.capture(image, (30.0,))
        except Exception as e:
            answer = e

        check(isinstance(answer, tuple) and len(answer) == 2
              and answer[0] is None and "OVMF" in str(answer[1]),
              "capture() without OVMF did not answer (None, why), which is "
              "what main unpacks: %r" % (answer,))

        # 2. And the harness skips, and says why.
        code, said, raised = run([run_uefi.__file__, image])

        check(raised is None and code == 0
              and said.startswith("SKIP:") and "OVMF" in said,
              "run_uefi.py without OVMF did not skip naming it: exit %r, %r%s"
              % (code, said.strip()[:200],
                 "" if raised is None else ", and raised %r" % raised))

        #
        # 3. With the firmware there and a boot that gives no picture, it
        #    fails. A skip here is a gate that passed without booting.
        #
        run_uefi.firmware = lambda: (("code.fd", "vars.fd"), None)
        run_uefi.capture = lambda iso, moments: (
            None, "the monitor wrote no screendump")

        code, said, raised = run([run_uefi.__file__, image])

        check(raised is None and code == 1 and said.startswith("FAIL:")
              and "no screendump" in said,
              "run_uefi.py with OVMF and a boot that gave no picture did not "
              "fail naming it: exit %r, %r%s"
              % (code, said.strip()[:200],
                 "" if raised is None else ", and raised %r" % raised))
    finally:
        run_uefi.firmware, run_uefi.capture = real_firmware, real_capture
        os.unlink(image)

    if fails:
        print("FAIL: %d of %d checks on run_uefi.py where it cannot boot:"
              % (len(fails), checks + len(fails)))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on run_uefi.py where it cannot boot (no OVMF is a "
          "skip that says so, and a boot with no picture is a failure)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
