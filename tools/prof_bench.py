#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Where a benchmark's instructions go, by kernel function - exactly.

    prof_bench.py ELF ipc|switch [--blocks FUNCTION] [--kinds]

Runs the benchmark image under QEMU with `-icount` and the plugin
`tools/qemu_pcprof.c`, counting every instruction executed between the
benchmark starting and the next one starting, and prints instructions per
operation by function, the function the symbol table puts each block in.
`--blocks` shows one function's blocks with their instructions and how often
each ran; `--kinds` counts locks, interrupt masks, per-core register reads
and calls. How the IPC round trip was taken apart (`testing.md` 18.343);
`make bench-profile` runs both benchmarks.
"""

import argparse
import bisect
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.join(os.path.dirname(HERE), "build", "host", "qemu_pcprof.dylib")

# Each benchmark: where it starts, where the next one starts, how many
# operations it counts (`bench/bench.c`).
WINDOWS = {
    "ipc": ("bench_ipc_roundtrip", "bench_context_switch", 100000),
    "switch": ("bench_context_switch", "bench_exception", 200000),
}

KINDS = [("locks taken", r"^stxr|^stlxr|^cas|^swp"),
         ("interrupts masked", r"^msr\s+daifset"),
         ("interrupts restored", r"^msr\s+daif,"),
         ("per-core register reads", r"tpidr_el1"),
         ("barriers", r"^dmb|^dsb|^isb"),
         ("calls", r"^bl\s|^blr")]


def symbols(elf):
    out = []
    for line in subprocess.run(["aarch64-none-elf-nm", "-n", elf], capture_output=True,
                               text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            out.append((int(parts[0], 16), parts[2]))
    return out


def disassembly(elf):
    dis = {}
    for line in subprocess.run(["aarch64-none-elf-objdump", "-d", "--no-show-raw-insn", elf],
                               capture_output=True, text=True, check=True).stdout.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
        if m:
            dis[int(m.group(1), 16)] = m.group(2)
    return dis


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("which", choices=sorted(WINDOWS))
    ap.add_argument("--blocks", help="one function's blocks")
    ap.add_argument("--kinds", action="store_true", help="kinds of instruction")
    args = ap.parse_args()

    if not os.path.exists(PLUGIN):
        sys.exit("prof_bench: no %s - `make bench-profile` builds it" % PLUGIN)

    syms = symbols(args.elf)
    addrs = [a for a, _ in syms]
    by_name = {n: a for a, n in syms}
    start, stop, ops = WINDOWS[args.which]

    with tempfile.TemporaryDirectory() as work:
        raw = os.path.join(work, "blocks.txt")
        subprocess.run(["qemu-system-aarch64", "-M", "virt,gic-version=3", "-cpu", "cortex-a72",
                        "-m", "512M", "-nographic",
                        "-semihosting-config", "enable=on,target=native",
                        "-icount", "shift=0",
                        "-plugin", "%s,start=%#x,stop=%#x,out=%s"
                        % (PLUGIN, by_name[start], by_name[stop], raw),
                        "-kernel", args.elf],
                       capture_output=True, text=True, timeout=1800)
        blocks = []
        with open(raw) as f:
            for line in f:
                pc, n, runs = line.split()
                blocks.append((int(pc, 16), int(n), int(runs)))

    def function_of(pc):
        i = bisect.bisect_right(addrs, pc) - 1
        return syms[i][1] if i >= 0 else hex(pc)

    if args.blocks or args.kinds:
        dis = disassembly(args.elf)

    if args.blocks:
        for pc, n, runs in sorted(blocks):
            if function_of(pc) == args.blocks and runs:
                print("%#x  %d instructions, %.2f times an operation" % (pc, n, runs / ops))
                for i in range(n):
                    print("    %s" % dis.get(pc + 4 * i, "?"))
        return 0

    if args.kinds:
        count = dict((k, 0) for k, _ in KINDS)
        for pc, n, runs in blocks:
            for i in range(n):
                ins = dis.get(pc + 4 * i, "")
                for k, rx in KINDS:
                    if re.search(rx, ins):
                        count[k] += runs
        for k, _ in KINDS:
            print("%8.2f  %s" % (count[k] / ops, k))

    totals = {}
    for pc, n, runs in blocks:
        name = function_of(pc)
        totals[name] = totals.get(name, 0) + n * runs

    print("%s: %.2f instructions an operation" % (args.which, sum(totals.values()) / ops))
    for name, n in sorted(totals.items(), key=lambda kv: -kv[1]):
        if n / ops >= 1:
            print("%8.2f  %s" % (n / ops, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
