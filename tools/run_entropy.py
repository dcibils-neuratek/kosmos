#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Randomness, from each source a machine can have, and none (HTTPS step 1).

`hal_entropy` and the kernel's health test (`kernel/entropy.c`), `sys.entropy`
and the Crypto Kit's generator (`crypto.random`) - the ground TLS will stand
on (`roadmap.md`, the browser: TLS). Each machine boots to the shell and
answers one line typed there:

  - **virtio-rng**, as the harness gives both boards: the boot says so, two
    asks of 256 bytes differ, and their sixty-four 8-byte words are all
    different - a stuck source repeats them - and the generator's two
    answers differ;
  - **RDRAND**, on x86 with QEMU's `max` processor and no virtio-rng: the
    M700's own source, the one its Skylake answers from;
  - **none**, on x86 with neither: the boot says so, `sys.entropy` is refused
    with a reason, and `crypto.random` raises rather than handing out
    anything - randomness that is quietly not random looks like success.

Usage: run_entropy.py ARM_IMAGE X86_IMAGE
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# Nothing in it raises, so a machine that has lost its randomness answers at
# once, with its lengths at nought, rather than leaving the harness to wait.
PROBE = ('local c = sys.kit("crypto") '
         'local a, b = sys.entropy(256) or "", sys.entropy(256) or "" '
         'local w, n = {}, 0 '
         'for s in (a .. b):gmatch("........") do if not w[s] then n = n + 1; w[s] = true end end '
         'local ok1, r1 = pcall(c.random, 64) local ok2, r2 = pcall(c.random, 64) '
         'r1 = ok1 and r1 or "" r2 = ok2 and r2 or "" '
         'print("ENT" .. "ROPY " .. #a .. " " .. #b .. " " .. tostring(a ~= b) .. " " .. n '
         '.. " " .. #r1 .. " " .. tostring(r1 ~= r2))')

NONE = ('local c = sys.kit("crypto") '
        'local a, why = sys.entropy(32) '
        'local ok, err = pcall(c.random, 16) '
        'print("NO" .. "NE " .. tostring(a) .. " / " .. tostring(why) .. " / " .. tostring(ok) '
        '.. " / " .. tostring(err))')


def boot(image, cpu=None, rng=True):
    """A machine to the shell, with or without the device and the processor."""
    import run_screenshot as R

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    args = list(saved)

    if not rng:
        at = args.index("virtio-rng-pci" if board == "X86_ARGS" else "virtio-rng-device")
        del args[at - 1:at + 1]

    if cpu:
        args += ["-cpu", cpu]

    setattr(R, board, args)

    try:
        guest = R.Guest(image, 90)
    finally:
        setattr(R, board, saved)

    guest.wait_for(R.PROMPT, "reached the shell prompt")
    return guest


# The markers are printed in two halves, so the shell's echo of the line
# typed - which has the probe's text in it - is never taken for the answer.
def answer(guest, line, marker):
    mark = len(guest.seen)
    guest.type(line)
    guest.wait_for_line(marker, "answered the probe", since=mark)
    at = guest.seen.find(marker, mark)
    return guest.seen[at:guest.seen.find("\n", at)].strip()


def main():
    arm = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    x86 = sys.argv[2] if len(sys.argv) > 2 else "build/x86_64/kosmos.elf"
    fails, said = [], {}

    runs = [
        ("ARM, virtio-rng", arm, None, True, "entropy: virtio-rng"),
        ("x86, virtio-rng", x86, None, True, "entropy: virtio-rng"),
        ("x86, RDRAND", x86, "max", False, "entropy: RDRAND"),
    ]

    for name, image, cpu, rng, line in runs:
        guest = boot(image, cpu, rng)

        try:
            if line not in guest.seen:
                fails.append("%s: the boot did not say %r: %s" % (
                    name, line, [l.strip() for l in guest.seen.splitlines()
                                 if "entropy" in l]))

            got = answer(guest, PROBE, "ENTROPY ")
            said[name] = got

            if got != "ENTROPY 256 256 true 64 64 true":
                fails.append("%s: %s - wanted two different 256-byte answers "
                             "of 64 different words, and two different 64-byte "
                             "ones from the generator" % (name, got))
        except Exception as e:              # noqa: BLE001 - said as a failure
            fails.append("%s: %s: %s" % (name, type(e).__name__, str(e).splitlines()[0]))
        finally:
            guest.close()

    guest = boot(x86, None, False)

    try:
        if "no entropy: no RDRAND on this processor and no virtio-rng device" not in guest.seen:
            fails.append("x86 with no source: the boot did not say so")

        got = answer(guest, NONE, "NONE ")
        said["none"] = got

        if not got.startswith("NONE nil / this machine has nothing of that kind / ") \
                or " / false / " not in got or "no source of randomness" not in got:
            fails.append("x86 with no source: %s - wanted sys.entropy refused and "
                         "crypto.random raising" % got)
    except Exception as e:                  # noqa: BLE001 - said as a failure
        fails.append("x86 with no source: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        guest.close()

    checks = 8

    if fails:
        print("FAIL: %d of %d checks on randomness:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on randomness (virtio-rng on both boards and RDRAND "
          "on x86, each said at boot and each two different answers of 64 "
          "different words, and the generator's two; and a machine with none "
          "saying so, sys.entropy refused and crypto.random raising)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
