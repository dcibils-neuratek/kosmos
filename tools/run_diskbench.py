#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Disk Benchmark at the prompt, on a disk this machine formats itself.

`diskbench` is the instrument Kosmos's storage is being made fast with
(`roadmap.md`, *Being built now*), so what this holds it to is honesty rather
than speed - QEMU's numbers measure QEMU. That it finds `/home`; that every
row it can run comes back with a number, and every row it cannot run says
why instead of holding a number for something else; that the run is kept in
`/home/benchmarks`, where a later one can be compared with it; and that the
test file it wrote is gone afterwards, since a benchmark that leaves a file
behind is one you run once.

Usage: run_diskbench.py IMAGE
"""

import os
import re
import sys
import scratch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_disk                                        # noqa: E402

# The rows as `/lib/diskbench.lua` prints them: a name, the queue, then read
# and write - each a number with its unit, or a sentence.
QUEUED = "not yet: one command at a time"

#
# **A file three times the journal**, written from a region and read back
# into another (`design.md` 8.3b, `roadmap.md` 6d 8f). kfs put every block a
# write changed through its 1 MB journal, a file's bytes too, and the disk
# server assembled a write in its own heap and refused past a megabyte - so
# a camera's recording could not be kept. 48 pieces of 64 KB, each a letter
# of its own: the last three bytes are the 48th's, `V`, and piece 27's are
# `B`, which a file cut short or a piece written to the wrong place says.
# Regions rather than strings, because the shell's heap is 2 MB.
#
BIG_WRITE = (
    'local N = 3 * 1024 * 1024 local r = sys.memory(N // 4096) '
    'for i = 0, N // 65536 - 1 do sys.region_write(r, i * 65536, '
    'string.rep(string.char(65 + i % 26), 65536)) end '
    'local wrote = fs.write_from("/home/big.bin", r, N) '
    'local a = fs.getattr("/home/big.bin") or {} '
    'local r2 = sys.memory(N // 4096) '
    'local got = fs.read_into("/home/big.bin", r2, 0, N) '
    'print("BIG", wrote, a.size, got, sys.region_read(r2, N - 3, 3), '
    'sys.region_read(r2, 65536 * 27, 2)) '
    'fs.send("/home/big.bin", { type = "delete" }) '
    'sys.release(r) sys.release(r2)')
RANDOM_WRITE = "not yet: a write replaces the whole file"


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    disk = scratch.disk("diskbench.img", 64 * 1024 * 1024)

    try:
        out = run_disk.boot(image, disk,
                            ["diskbench",
                             "diskbench /home 1 1",
                             "ls /home/benchmarks",
                             "ls /home/.diskbench",
                             BIG_WRITE],
                            boot_timeout=120, each=180)
    finally:
        os.unlink(disk)

    if out is None:
        print("FAIL: the machine did not reach a prompt with a disk")
        return 1

    lines = out.splitlines()
    shown = "\n    ".join(l for l in lines if l.strip())

    check(any(l.strip() == "/home" for l in lines),
          "`diskbench` with no arguments did not list /home as something it "
          "can measure:\n    " + shown)

    def row(name, queue):
        for l in lines:
            if l.startswith("%s x%d" % (name, queue)):
                return l
        return None

    seq = row("sequential 1 MB", 1)
    rnd = row("random 4 KB", 1)

    check(seq is not None and len(re.findall(r"(\d+\.\d) MB/s", seq)) == 2
          and all(float(v) > 0 for v in re.findall(r"(\d+\.\d) MB/s", seq)),
          "sequential 1 MB x1 did not come back with a read and a write in "
          "MB/s, both above zero: %r" % seq)

    check(rnd is not None and re.search(r"\d+\.\d MB/s\s+(\d+) IOPS", rnd)
          and int(re.search(r"(\d+) IOPS", rnd).group(1)) > 0
          and RANDOM_WRITE in rnd,
          "random 4 KB x1 did not read with a number of IOPS above zero and "
          "say %r for its write: %r" % (RANDOM_WRITE, rnd))

    for name, queue in (("sequential 1 MB", 8), ("random 4 KB", 32)):
        r = row(name, queue)
        check(r is not None and r.count(QUEUED) == 2,
              "%s x%d did not say %r for both its read and its write: %r"
              % (name, queue, QUEUED, r))

    # Storage at full speed, step 2: the disk server counts what its device
    # cost, and each side measured on /home says how much of its run that was.
    # Zero is a failure, not a fast device - it is what a server that stopped
    # counting would report.
    shares = re.findall(r"^\s+(sequential|random) (read|write): the device "
                        r"(\d+)%, everything else (\d+)%", out, re.M)
    got = {(kind, side): (int(dev), int(rest)) for kind, side, dev, rest in shares}

    check(set(got) == {("sequential", "read"), ("sequential", "write"),
                       ("random", "read")}
          and all(0 < dev <= 100 and dev + rest == 100
                  for dev, rest in got.values()),
          "where the time went did not give the device's share, above zero, "
          "for the three sides measured on /home: %r" % (shares,))

    saved = re.search(r"saved (/home/benchmarks/([0-9A-Za-z-]+)\.bench)", out)

    check(saved is not None and (saved.group(2) + ".bench") in out.split(
              "ls /home/benchmarks", 1)[-1],
          "the run was not saved in /home/benchmarks, or `ls` did not show "
          "it there:\n    " + shown)

    after = out.split("ls /home/.diskbench", 1)[-1]

    # Held to the rows having run rather than to the run being saved, so a
    # run that measured and then could not save still has its file counted.
    check(seq is not None and "test" not in after,
          "the test file was left in /home/.diskbench after the run:\n    "
          + shown)

    big = re.search(r"^BIG\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)",
                    out, re.M)

    check(big is not None and big.groups() == ("3145728", "3145728",
                                              "3145728", "VVV", "BB"),
          "a 3 MB file - three times the journal - was not written from a "
          "region and read back whole: %r" % ((big.groups() if big else out[-600:]),))

    if fails:
        print("FAIL: %d of %d checks on Disk Benchmark at the prompt:"
              % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on Disk Benchmark at the prompt (/home found, every "
          "row it can run measured with the device's share of it, every row it cannot run saying why, the "
          "run kept, its test file removed, and a file three times the "
          "journal written and read back)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
