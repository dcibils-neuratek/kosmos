#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The x86-64 clocks, measured right when the machine is interrupted.

On a PC nothing says how fast the TSC or the local APIC's timer runs, so
`hal/pc/timer.c` measures the TSC against the 8253 and `hal/pc/apic.c` the
local APIC's timer against the TSC. On 25 September the gate caught the TSC
at 4.8 times its speed: the Mac was running twenty-six suites, QEMU's thread
was set aside across the moment the 8253 finished, and the loop watching it
noticed tens of milliseconds late. Every clock in that machine was wrong for
the rest of its life - a film of three seconds took 0.62 by its own - and
nothing said so.

Both measurements are bracketed now, and one that was interrupted is taken
again. The local APIC's reads the TSC either side of each reading of its
count, so a stop in its wait only lengthens the span both clocks measure -
its line says how long, and a span of a stop's length is a stop that landed.
The stop is sent two milliseconds after a line begins, inside the ten its
measuring takes. This does to them what the Mac did by accident, and on purpose: each
timer line is begun before its measuring, and the moment one appears QEMU
is stopped for a tenth of a second (SIGSTOP, so the guest's clocks run on
while it does not). The measurement has to notice and take itself again,
and come out where a boot left alone does.

A stop that lands after the measuring has finished tests nothing, and the
host has ten milliseconds to land one - which a Mac running the whole gate
does not always manage. So a clock measured on its first try, and right, is
counted as untested and the machine booted again, up to BOOTS times at a
third of a second each; a clock that never had a stop land in any of them
is a failure rather than a pass. A clock that came out wrong fails at once,
whatever it says about its tries.

Usage: run_timer.py IMAGE
"""

import os
import re
import select
import signal
import subprocess
import sys
import time

ARGS = ["-M", "q35", "-m", "512M", "-nographic", "-vga", "none"]

# The two lines, begun before the measuring and finished after it.
TSC = b"timer: the TSC at "
APIC = b"timer: the local APIC's at "
SAID = re.compile(r"timer: the (TSC|local APIC's) at (\d+) kHz, within (\d+) "
                  r"ppm(?:, over ([\d.]+) ms)?, against the \S+ in (\d+) "
                  r"tr(?:y|ies)(.*)")

STALL = 0.1             # seconds QEMU is stopped for
LAND = 0.002            # and how long after a line begins, so it lands inside
BOOTS = 10              # stopped boots allowed, to land a stop in each clock
SLACK_PPM = 1000000 // 256 // 2   # the brackets' half-width allowed, `timer.c`
AGREE = 0.005           # a stalled boot and a quiet one, within 0.5%
TIMEOUT = 60.0


def boot(image, stall):
    """Boots until both clocks are measured, stopping QEMU as each measuring
    begins if `stall`; returns what the two lines said, by clock."""
    p = subprocess.Popen(["qemu-system-x86_64"] + ARGS + ["-kernel", image],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL)
    out = b""
    stalled = set()
    start = time.time()

    try:
        while time.time() - start < TIMEOUT:
            ready, _, _ = select.select([p.stdout], [], [], 0.5)

            if not ready:
                continue

            chunk = os.read(p.stdout.fileno(), 65536)

            if not chunk:
                break

            out += chunk

            # A line's beginning is out, so the guest is measuring or about
            # to be. Looked for in what just arrived and the few bytes before
            # it, and nothing else - the UART sends a byte at a time, so this
            # runs for every few. And stopped LAND later rather than at once:
            # this side can answer within microseconds, which is before the
            # measuring has started and tests nothing - the first version of
            # this suite did that nine boots in ten for the local APIC.
            recent = out[-(len(chunk) + len(APIC)):]

            for begun in (TSC, APIC):
                if stall and begun not in stalled and begun in recent:
                    stalled.add(begun)
                    time.sleep(LAND)
                    os.kill(p.pid, signal.SIGSTOP)
                    time.sleep(STALL)
                    os.kill(p.pid, signal.SIGCONT)

            if b"\n" in chunk and (len(SAID.findall(out.decode(
                    "utf-8", "replace"))) == 2 or b"kosmos>" in out):
                break
    finally:
        p.kill()
        p.wait()

    text = out.decode("utf-8", "replace")
    said = {}

    for m in SAID.finditer(text):
        said[m.group(1)] = {"khz": int(m.group(2)), "ppm": int(m.group(3)),
                            "ms": float(m.group(4) or 0),
                            "tries": int(m.group(5)),
                            "clean": "none of them" not in m.group(6)}

    return said, text


def main():
    image = sys.argv[1]
    failed = []
    checks = 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    quiet, text = boot(image, stall=False)

    if len(quiet) != 2:
        print("FAIL: a boot left alone did not say both clocks' rates; "
              "its timer lines:")
        for line in text.splitlines():
            if "timer:" in line:
                print("  " + line.strip())
        return 1

    for clock, m in sorted(quiet.items()):
        check(m["clean"] and m["ppm"] <= SLACK_PPM,
              "left alone, the %s was measured to %d ppm in %d tries, none "
              "of them clean" % (clock, m["ppm"], m["tries"]))

    # Stopped as each measuring begins, until a stop has landed inside each.
    landed = {}

    for boots in range(1, BOOTS + 1):
        stalled, text = boot(image, stall=True)

        if len(stalled) != 2:
            print("FAIL: a boot stopped while measuring did not say both "
                  "clocks' rates; its timer lines:")
            for line in text.splitlines():
                if "timer:" in line:
                    print("  " + line.strip())
            return 1

        for clock, m in stalled.items():
            base = quiet[clock]["khz"]

            if (abs(m["khz"] - base) > AGREE * base or m["tries"] >= 2
                    or m["ms"] >= STALL * 1000 / 2):
                landed.setdefault(clock, m)

        if len(landed) == 2:
            break
    else:
        missed = sorted(set(quiet) - set(landed))
        print("FAIL: %d boots, and a stop never landed inside the measuring "
              "of the %s - nothing was tested" % (BOOTS, " or ".join(missed)))
        return 1

    for clock, m in sorted(landed.items()):
        base = quiet[clock]["khz"]
        off = abs(m["khz"] - base) / base

        check(m["clean"] and m["ppm"] <= SLACK_PPM,
              "stopped while measuring, the %s kept a measurement of %d ppm "
              "after %d tries" % (clock, m["ppm"], m["tries"]))
        check(off <= AGREE,
              "stopped while measuring, the %s came out at %d kHz, and "
              "at %d kHz left alone - %.1f%% out"
              % (clock, m["khz"], base, off * 100))

    if failed:
        print("FAIL: %d of %d checks on the x86-64 clocks:"
              % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: the x86-64 clocks, %d checks - the TSC at %d kHz and the "
          "local APIC's at %d kHz, and the same within %.1f%% with QEMU "
          "stopped for %.1f s as each was measured (the TSC in %d tries, "
          "the local APIC's over %.1f ms; %d boot%s to land both)"
          % (checks, quiet["TSC"]["khz"], quiet["local APIC's"]["khz"],
             AGREE * 100, STALL, landed["TSC"]["tries"],
             landed["local APIC's"]["ms"], boots,
             "" if boots == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
