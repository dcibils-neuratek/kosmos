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
import glob
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

    #
    # **A stick that starts the desktop, judged on the boot screen.** Every
    # stick handed over carries `USB_BOOT=wm`, so by the time the capture is
    # taken the desktop has replaced the boot screen - and the three colour
    # checks looked for the *kernel's* ground, the boot log's green and the
    # wordmark. 0.10.70-stable failed all three on 16 September while its
    # serial line showed Tracker, the Deskbar, Monitor, Log and Processes up
    # at 1280x800: a build Diego had used on the ThinkPad and called stable,
    # scored as "the picture is still the firmware's".
    #
    # So the rule is now "a desktop stick must show a desktop", counted in
    # distinct colours rather than named in one constant - because the
    # desktop's ground is a colour the user picks and `theme.lua` says so.
    # The stick measured 2347 the day this was written.
    #
    blank = bytes([13, 17, 23]) * 4000
    desk = bytes(bytearray(i % 251 for i in range(3 * 4000)))

    check(run_uefi.drawn(blank) < run_uefi.DRAWN_ENOUGH,
          "a flat screen counted as drawn: %d colours" % run_uefi.drawn(blank))
    check(run_uefi.drawn(desk) >= run_uefi.DRAWN_ENOUGH,
          "a many-coloured screen did not count as drawn: %d colours"
          % run_uefi.drawn(desk))

    #
    # **The negative control, driven through the decision rather than stated
    # beside it.** The first version of this check was
    # `not (drawn(blank) >= DRAWN_ENOUGH)`, which is the line above it
    # rewritten - it restated its premise and watched nothing fail. So this
    # runs `main()` twice with a desktop stick: once showing a blank screen,
    # which must be complained about, and once showing a drawn one, which
    # must not. A branch that only ever says yes would pass a stick that
    # never drew, which is this fault in the other direction.
    #
    #
    # A file of its own, because the cases above delete theirs to reach the
    # "no image" skip - and `main()` skips before it looks at a screen, so
    # reusing that path made this control say nothing at all. It said so.
    #
    spare, standing = tempfile.mkstemp(prefix="kosmos-desktop-stick-",
                                       suffix=".img")
    os.close(spare)

    real_boot_args = run_uefi.boot_args
    wide = (1280, 800)

    def framed(pixels):
        def fake(iso, moments):
            return [(wide[0], wide[1], pixels)] * len(moments), ""

        return fake

    try:
        run_uefi.firmware = lambda: ("ovmf", None)
        run_uefi.boot_args = lambda iso: "opt/kosmos/boot=wm\n"

        run_uefi.capture = framed(bytes([13, 17, 23]) * (wide[0] * wide[1]))
        _, dark_said, _ = run(["run_uefi.py", standing])

        run_uefi.capture = framed(
            bytes(bytearray(i % 251 for i in range(3 * wide[0] * wide[1]))))
        _, lit_said, _ = run(["run_uefi.py", standing])
    finally:
        run_uefi.firmware, run_uefi.capture = real_firmware, real_capture
        run_uefi.boot_args = real_boot_args
        os.unlink(standing)

    complaint = "colours are on the screen"

    check(complaint in dark_said,
          "a desktop stick showing a blank screen drew no complaint, so the "
          "branch is a rubber stamp: " + dark_said[:160])
    check(complaint not in lit_said,
          "a desktop stick showing a drawn desktop was complained about "
          "anyway: " + lit_said[:160])

    #
    # And the detection itself: a stick says what it tells the kernel, and a
    # plain image says nothing. Only run where the images exist, because a
    # clean tree has neither and a skip is honest where a lie is not.
    #
    #
    # **The stable stick, not this version's.** The first version of this
    # looked for `kosmos-usb-<VERSION>-development.img`, and `VERSION` moves
    # on every push while a stick is built only when one is handed over - so
    # after 0.10.76's bump it found nothing, and this check skipped itself in
    # silence: 9 checks became 8 in `make test` and nothing said why. There is
    # exactly one stable stick at a time, its bytes are never rebuilt, and it
    # carries `opt/kosmos/boot=wm`, read back on 16 September. Not every
    # development stick does - 0.10.69's predates `USB_BOOT` - so a glob over
    # all of them would assert something false about the older ones.
    #
    known = [(s, True) for s in sorted(glob.glob(
        "build/x86_64/kosmos-usb-*-stable.img"))]
    known.append(("build/x86_64/kosmos-uefi.img", False))

    for image, wants in known:
        if not os.path.exists(image):
            continue

        said = "opt/kosmos/boot=" in run_uefi.boot_args(image)

        check(said == wants,
              "%s %s said it starts the desktop"
              % (image, "did not" if wants else "wrongly"))

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
