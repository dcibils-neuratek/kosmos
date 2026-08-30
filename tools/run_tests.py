#!/usr/bin/env python3
"""
Host-side test runner.

The runner lives on the host, the tests run in the guest, and the serial line
is the channel between them. This launches QEMU, reads the TAP stream the
guest prints, and turns the whole thing into an exit code.

Three independent signals have to agree before this exits 0:

  - the boot banner arrived, so the UART came up at all
  - the TAP stream is complete: a plan, exactly that many results, numbered
    in order, none of them failing
  - QEMU's own exit code, which the guest sets through semihosting

Any one of them alone is too easy to pass by accident. A guest that faults
before printing produces no TAP. A guest that prints a flawless plan and then
hangs never sets an exit code. A guest that exits 0 from the wrong place
never printed a plan.

Usage: run_tests.py <image.elf> [--timeout SECONDS]
"""

import argparse
import re
import select
import subprocess
import sys
import time

QEMU = "qemu-system-aarch64"

QEMU_ARGS = [
    # Not the default: plain `-M virt` gives a GICv2 and this kernel drives a
    # GICv3. It has to match the Makefile's line.
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a72",
    "-m", "512M",
    "-nographic",
    # Without this the guest cannot set the exit code and `hlt #0xf000`
    # becomes an undefined instruction trap into a vector that does not
    # exist yet.
    "-semihosting-config", "enable=on,target=native",
]

BANNER = "Kosmos"

# What the kernel prints when it takes an exception it cannot recover from.
# Seeing it means the run is over: the kernel halts, and waiting out the
# timeout would only delay a result that is already known.
PANIC = "PANIC:"

PLAN_RE = re.compile(r"^1\.\.(\d+)\s*$")
RESULT_RE = re.compile(r"^(not ok|ok)\s+(\d+)\s*-\s*(.*?)\s*$")


class Failure(Exception):
    """Something about the run was wrong. The message is the report."""


def fail(message):
    """Report a failure after everything already printed.

    stdout is flushed first on purpose. Without it the two streams interleave
    however the terminal feels like it, and the failure line lands above the
    output that explains it, which is the opposite of useful.
    """
    sys.stdout.flush()
    print(f"\nFAIL: {message}", file=sys.stderr)
    sys.stderr.flush()


def run_qemu(image, timeout):
    """Boot the image and return (output, exit_code).

    Read line by line rather than waiting for the process, so a kernel panic
    ends the run immediately instead of burning the whole timeout. The kernel
    halts after printing its dump, so without this every panic costs the full
    wait and the output arrives long after it was useful.
    """
    try:
        proc = subprocess.Popen(
            [QEMU, *QEMU_ARGS, "-kernel", image],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
    except FileNotFoundError:
        raise Failure(f"{QEMU} not found. See docs/setup.md.")

    lines = []
    panicked = False
    deadline = time.monotonic() + timeout

    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                sys.stdout.write("".join(lines))
                raise Failure(
                    f"the guest did not exit within {timeout}s.\n"
                    "Either a test hung or semihosting never fired. There is "
                    "no watchdog inside the guest until the timer lands at M1."
                )

            ready, _, _ = select.select([proc.stdout], [], [], remaining)
            if not ready:
                continue

            line = proc.stdout.readline()
            if line == "":
                break                       # QEMU closed the pipe: it exited

            lines.append(line)

            if line.startswith(PANIC):
                panicked = True
                # Let the rest of the dump arrive; it is the useful part.
                for _ in range(16):
                    r, _, _ = select.select([proc.stdout], [], [], 0.5)
                    if not r:
                        break
                    more = proc.stdout.readline()
                    if more == "":
                        break
                    lines.append(more)
                break
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.wait()

    output = "".join(lines)

    if panicked:
        sys.stdout.write(output)
        raise Failure(
            "the kernel panicked. The dump above says which instruction "
            "faulted (elr) and on what address (far)."
        )

    return output, proc.returncode


def parse_tap(output):
    """Pull the plan and the results out of the serial stream.

    The stream is not pure TAP: the boot banner and anything else the kernel
    prints share the line with it. Lines that do not look like TAP are
    ignored rather than treated as errors.
    """
    plan = None
    results = []

    for line in output.splitlines():
        line = line.strip()

        m = PLAN_RE.match(line)
        if m:
            if plan is not None:
                raise Failure("the guest printed more than one TAP plan.")
            plan = int(m.group(1))
            continue

        m = RESULT_RE.match(line)
        if m:
            results.append(
                {
                    "ok": m.group(1) == "ok",
                    "number": int(m.group(2)),
                    "name": m.group(3),
                }
            )

    return plan, results


def check(output, exit_code):
    """Raise Failure on the first thing that is wrong."""
    if BANNER not in output:
        raise Failure(
            f'the boot banner "{BANNER}" never appeared. '
            "The guest died before the UART was usable."
        )

    plan, results = parse_tap(output)

    if plan is None:
        raise Failure(
            "no TAP plan in the output. The guest booted but never reached "
            "the test suite."
        )

    if len(results) != plan:
        raise Failure(
            f"the plan promised {plan} test(s) and {len(results)} arrived. "
            "The run was cut short."
        )

    for i, r in enumerate(results, start=1):
        if r["number"] != i:
            raise Failure(
                f"TAP results are out of order: expected {i}, got {r['number']}."
            )

    failed = [r for r in results if not r["ok"]]
    if failed:
        names = "\n".join(f'  not ok {r["number"]} - {r["name"]}' for r in failed)
        raise Failure(f"{len(failed)} of {plan} test(s) failed:\n{names}")

    if exit_code != 0:
        raise Failure(
            f"every test passed but QEMU exited {exit_code}. "
            "The guest reported success through the serial line and failure "
            "through semihosting, so one of the two paths is broken."
        )

    return plan


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help="the test image to boot")
    ap.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="seconds before the guest is considered hung (default: 30)",
    )
    args = ap.parse_args()

    try:
        output, exit_code = run_qemu(args.image, args.timeout)
    except Failure as e:
        fail(e)
        return 1

    sys.stdout.write(output)
    if output and not output.endswith("\n"):
        sys.stdout.write("\n")

    try:
        passed = check(output, exit_code)
    except Failure as e:
        fail(e)
        return 1

    print(f"\nPASS: {passed}/{passed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
