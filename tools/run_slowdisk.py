#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A disk that answers slowly is waited for, and stays in step.

On 1 October `/Home` turned to zero-byte files in the middle of a session:
Tracker drew every file at 0 B and every folder as a file, and a film
opened and closed in a second. The bytes on the disk were whole. The
kernel's virtio drivers waited for a request by a count - a hundred million
turns of a loop, about a second under TCG - and with this Mac full of the
gate's QEMUs a read took longer. The driver gave the request up while the
device still had it, and from then on ran one behind: each request took the
last one's late completion for its own and failed. Blocks already cached
kept answering, so a folder still listed (`testing.md` 18.339,
`hal/virtio/wait.c`).

This makes that host on purpose. The machine boots at full speed, lists
`/Home`, and then QEMU is told through QMP to let the disk move sixteen
kilobytes a second. With that:

  - a file of 160 KB nothing has read is read whole, and its bytes are the
    ones written - two requests of the disk server's, each several seconds
    long, where the old wait gave up after about one;
  - two small files are asked for, and answer with their sizes;
  - the console never says a device was given up on;
  - and the read took the time the throttle says it must, which is what
    makes the rest mean anything: a throttle that did not take would pass
    everything above on an ordinary disk. (Small files cannot show it: the
    listing and the disk server's cache have their blocks before the
    throttle starts.)

Usage: run_slowdisk.py IMAGE
"""

import json
import os
import select
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_disk                                              # noqa: E402
import scratch                                               # noqa: E402

PROMPT = "kosmos>"
LUA = os.path.join(os.path.dirname(HERE), "build", "host", "lua")

# Sixteen kilobytes a second: the disk server reads up to 124 KB a request
# (`SYS_DISK_INFO`'s `most`), so each of the big file's requests takes
# seconds. The old wait gave up after about one on an idle Mac, which is a
# margin a busy one cannot close.
BPS = 16384

FILES = 12
ASKED = ("f05", "f11")


def size_of(i):
    return 37 * i + 11


def content(i):
    return bytes((i * 7 + k * 13) & 0xff for k in range(size_of(i)))


PAGE = bytes((k * 31 + 5 + (k >> 9)) & 0xff for k in range(160 * 1024))


def checksum(data):
    s = 0
    for b in data:
        s = (s * 31 + b) % 1000000007
    return s


class Failure(Exception):
    pass


def make_disk(work):
    pairs = []

    for i in range(FILES):
        host = os.path.join(work, "f%02d" % i)
        with open(host, "wb") as f:
            f.write(content(i))
        pairs.append("%s:/Home/f%02d" % (host, i))

    page = os.path.join(work, "page.bin")
    with open(page, "wb") as f:
        f.write(PAGE)
    pairs.append(page + ":/Home/page.bin")

    disk = os.path.join(work, "slow.img")
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "16",
                    *pairs], check=True, capture_output=True,
                   cwd=os.path.dirname(HERE))
    return disk


class Machine:
    def __init__(self, image, disk, qmp_path):
        self.qmp_path = qmp_path
        self.proc = subprocess.Popen(
            [*run_disk.qemu_args(disk, image),
             "-qmp", f"unix:{qmp_path},server,nowait", "-kernel", image],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, bufsize=0)
        self.seen = ""

    def pump(self, seconds, until, since):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.proc.stdout], [], [], 0.2)

            if ready:
                chunk = os.read(self.proc.stdout.fileno(), 65536)

                if not chunk:
                    return False
                self.seen += chunk.decode("utf-8", "replace")

            if until in self.seen[since:]:
                return True

        return False

    def command(self, line, seconds):
        mark = len(self.seen)
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

        if not self.pump(seconds, PROMPT, mark + len(line)):
            raise Failure(f"no prompt after {line!r} in {seconds} s:\n"
                          + self.seen[mark:][-800:])
        return self.seen[mark:]

    def throttle(self, bps):
        """QMP's `block_set_io_throttle` on the drive the harness names
        `disk` - every bucket but the total left at zero, which is none."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(self.qmp_path)
        f = s.makefile("rwb")

        def send(obj):
            f.write((json.dumps(obj) + "\n").encode())
            f.flush()

            while True:
                reply = json.loads(f.readline())
                if "return" in reply:
                    return reply
                if "error" in reply:
                    raise Failure(f"QMP refused {obj['execute']}: {reply['error']}")

        json.loads(f.readline())                 # the greeting
        send({"execute": "qmp_capabilities"})
        send({"execute": "block_set_io_throttle",
              "arguments": {"device": "disk", "bps": bps, "bps_rd": 0,
                            "bps_wr": 0, "iops": 0, "iops_rd": 0,
                            "iops_wr": 0}})
        s.close()

    def close(self):
        self.proc.kill()
        self.proc.wait()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("slowdisk")
    disk = make_disk(work)
    machine = Machine(image, disk, os.path.join(work, "qmp"))
    checks = failures = 0

    def check(ok, what):
        nonlocal checks, failures
        checks += 1
        if not ok:
            failures += 1
            print(f"not ok {checks} - {what}")

    try:
        if not machine.pump(120, PROMPT, 0):
            raise Failure("the machine never reached a shell prompt:\n"
                          + machine.seen[-800:])

        # The folder read at full speed - and with it, by the disk
        # server's cache, the small files' blocks.
        listed = machine.command(
            'local l = fs.list("/Home") local n = 0 for _, x in ipairs(l) do '
            'if x == "page.bin" or x:match("^f%d%d$") then n = n + 1 end end '
            'print("LISTED", n)', 60)
        check(f"LISTED\t{FILES + 1}" in listed,
              f"/Home lists the {FILES + 1} files before the disk is slowed")

        machine.throttle(BPS)

        # Timed by the guest's own counter around the read alone, so the
        # checksum after it - a Lua loop over every byte - is not counted.
        read = machine.command(
            'local hz = (fs.read("/Devices/cpu") or {}).counter_hz '
            'local t = sys.ticks() local v, e = fs.read("/Home/page.bin") '
            'local ms = (sys.ticks() - t) * 1000 // hz local s = 0 '
            'for i = 1, #(v or "") do s = (s * 31 + v:byte(i)) % 1000000007 end '
            'print("READ", v and #v, s, e) print("TOOK", ms)', 180)

        check(f"READ\t{len(PAGE)}\t{checksum(PAGE)}\tnil" in read,
              f"page.bin reads back whole: {read.strip()[-200:]!r}")

        took = 0.0
        for line in read.splitlines():
            if line.startswith("TOOK\t"):
                took = int(line.split("\t")[1]) / 1000.0

        # 160 KB at 16 KB a second is ten; held to half of it.
        check(took >= 5.0, f"the slowed disk took {took:.1f} s for 160 KB - "
              "at least 5 s, or the throttle did not take")

        asked = machine.command(
            'for _, f in ipairs({%s}) do local a, e = fs.getattr("/Home/" .. f) '
            'print("ASKED", f, a and a.size, a and a.kind, e) end'
            % ", ".join('"%s"' % n for n in ASKED), 120)

        for name in ASKED:
            i = int(name[1:])
            want = f"ASKED\t{name}\t{size_of(i)}\tfile\tnil"
            check(want in asked, f"{name} answers {size_of(i)} bytes, a file, "
                  f"from the slowed disk: {asked.strip()[-300:]!r}")

        check("no answer in" not in machine.seen,
              "no device given up on: " + machine.seen[-300:])

        # And after it, the disk at full speed again, still in step.
        machine.throttle(0)
        again = machine.command(
            'local a = fs.getattr("/Home/f10") print("AGAIN", a and a.size)', 60)
        check(f"AGAIN\t{size_of(10)}" in again,
              "a file answers at full speed afterwards")
    except Failure as e:
        print("FAIL:", e)
        return 1
    finally:
        machine.close()

    if failures:
        print(f"FAIL: {failures} of {checks} checks on a slow disk "
              f"({run_disk.machine(image)}).")
        return 1

    print(f"PASS: {checks} checks on a disk sixteen kilobytes a second "
          f"({run_disk.machine(image)}), {took:.0f} s for 160 KB.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
