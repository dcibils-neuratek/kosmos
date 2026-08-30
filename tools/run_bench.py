#!/usr/bin/env python3
"""
Host-side benchmark runner.

Boots the benchmark image under QEMU, reads the numbers off the serial line,
and compares them against bench/baselines.json.

Two things make this different from run_tests.py, and both come from
testing.md 18.3.

QEMU runs with `-icount shift=0`, which ties the emulated clock to
instructions retired rather than to host time. Without it the same spin loop
measures 88187, 88687 and 89062 across three runs; with it, 75001, 75000,
75001. Only the second kind of number can detect a regression.

And the numbers are not performance. QEMU is a translator, so a tick here has
no relationship to a cycle on real hardware. They answer "did this get worse
since yesterday" and nothing else. testing.md 18.8: do not optimise against
them.

Usage:
  run_bench.py <image.elf>            compare against the baselines
  run_bench.py <image.elf> --record   write the current numbers as the new
                                      baselines, which is a deliberate act
"""

import argparse
import json
import os
import re
import subprocess
import sys

QEMU = "qemu-system-aarch64"

QEMU_ARGS = [
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a72",
    "-m", "512M",
    "-nographic",
    "-semihosting-config", "enable=on,target=native",
    # The whole point. Everything below is meaningless without it.
    "-icount", "shift=0",
]

BASELINES = os.path.join(os.path.dirname(__file__), "..", "bench", "baselines.json")

# bench <name> <thousandths of a tick per operation> <iterations>
RESULT_RE = re.compile(r"^bench\s+(\S+)\s+(\d+)\s+(\d+)\s*$")
FAIL_RE = re.compile(r"^bench-fail\s+(\S+)\s*$")


def run(image, timeout):
    """Boot the image and return {name: (per_op, iterations)}."""
    try:
        proc = subprocess.run(
            [QEMU, *QEMU_ARGS, "-kernel", image],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError:
        sys.exit(f"FAIL: {QEMU} not found. See docs/setup.md.")
    except subprocess.TimeoutExpired as e:
        partial = e.stdout or ""
        if isinstance(partial, bytes):
            partial = partial.decode(errors="replace")
        sys.stdout.write(partial)
        sys.exit(
            f"\nFAIL: the benchmarks did not finish within {timeout}s.\n"
            "-icount makes QEMU several times slower; if this is simply a "
            "long run rather than a hang, raise --timeout."
        )

    results = {}
    failed = []

    for line in proc.stdout.splitlines():
        line = line.strip()

        m = RESULT_RE.match(line)
        if m:
            results[m.group(1)] = (int(m.group(2)) / 1000.0, int(m.group(3)))
            continue

        m = FAIL_RE.match(line)
        if m:
            failed.append(m.group(1))

    if failed:
        sys.exit("FAIL: benchmarks that did not run: " + ", ".join(failed))

    if not results:
        sys.stdout.write(proc.stdout)
        sys.exit("\nFAIL: the guest produced no benchmark output.")

    return results


def load_baselines():
    try:
        with open(BASELINES) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def record(results):
    """Write the current numbers as the new baselines.

    By hand and never automatically. testing.md 18.6: a baseline that updates
    itself detects nothing, because every regression silently becomes the new
    normal. Raising one is a deliberate act and belongs in a commit that says
    why the number moved.
    """
    commit = "unknown"
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except Exception:
        pass

    existing = load_baselines()
    out = {}

    for name, (value, iterations) in sorted(results.items()):
        previous = existing.get(name, {})
        out[name] = {
            "value": value,
            "iterations": iterations,
            # Two percent, per testing.md 18.6: under -icount the noise is
            # far below that, so anything larger is a real change in work.
            "tol": previous.get("tol", 0.02),
            "commit": commit,
            "unit": "counter ticks per operation, QEMU -icount shift=0",
        }

    with open(BASELINES, "w") as f:
        json.dump(out, f, indent=2, sort_keys=True)
        f.write("\n")

    print(f"Recorded {len(out)} baselines at {commit}:\n")
    for name, entry in sorted(out.items()):
        print(f"  {name:<18} {entry['value']:>12.3f}")
    print(f"\nWritten to {os.path.normpath(BASELINES)}.")
    print("Commit it, and say in the message why the numbers moved.")


def compare(results):
    baselines = load_baselines()

    if not baselines:
        print("No baselines yet. Current numbers:\n")
        for name, (value, iterations) in sorted(results.items()):
            print(f"  {name:<18} {value:>12.3f}   ({iterations} iterations)")
        print("\nRun `make bench-record` to make these the baseline.")
        return 0

    worst = 0.0
    regressions = []
    unknown = []

    print(f"{'benchmark':<18} {'now':>12} {'baseline':>12} {'change':>9}")
    print("-" * 55)

    for name, (value, _) in sorted(results.items()):
        entry = baselines.get(name)

        if entry is None:
            unknown.append(name)
            print(f"{name:<18} {value:>12.3f} {'-':>12} {'new':>9}")
            continue

        base = entry["value"]
        tol = entry.get("tol", 0.02)
        change = (value - base) / base if base else 0.0
        worst = max(worst, change)

        flag = ""
        if change > tol:
            flag = "  REGRESSION"
            regressions.append((name, base, value, change, tol))
        elif change < -tol:
            flag = "  faster"

        print(f"{name:<18} {value:>12.3f} {base:>12.3f} {change:>+8.1%}{flag}")

    print()

    if unknown:
        print("New benchmarks with no baseline: " + ", ".join(unknown))
        print("Run `make bench-record` once they are settled.\n")

    if regressions:
        print(f"FAIL: {len(regressions)} benchmark(s) got worse:\n")
        for name, base, value, change, tol in regressions:
            print(f"  {name}: {base:.3f} -> {value:.3f} "
                  f"({change:+.1%}, tolerance {tol:.0%})")
        print("\nIf the number moved on purpose, raise the baseline in the "
              "same commit and say why.")
        return 1

    print("PASS: everything within tolerance.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image")
    ap.add_argument("--record", action="store_true",
                    help="write these numbers as the new baselines")
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="seconds before the run is considered hung "
                         "(default: 600; -icount is slow)")
    args = ap.parse_args()

    results = run(args.image, args.timeout)

    if args.record:
        record(results)
        return 0

    return compare(results)


if __name__ == "__main__":
    sys.exit(main())
