#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cuts the power in the middle of writing, and checks what came back.

This is M8's definition of done, and it is the only test here that cannot
be written as a question about one boot. The machine is killed - SIGKILL,
no shutdown, no flush, nothing given a chance to tidy up - part way through
a run of writes, and then booted again to see what the filesystem says.

**What is being checked is not that no data was lost.** Data written a
moment before the power went is gone, and that is correct. What must hold
is that the filesystem is not left *disagreeing with itself*:

  * every name that appears in a directory can be read
  * every file that can be read holds exactly what was written to it,
    never a piece of it and never somebody else's bytes
  * the filesystem mounts at all

A file that is absent is fine. A file that is half there is not, and
neither is a name that lists but cannot be opened - that is a directory
entry pointing at an inode that was never written, which is the classic
thing a journal exists to prevent.

The kill happens at several different delays, because the interesting
failures live at particular instants - between the directory write and the
inode write, between the inode and the bitmap - and one delay only ever
lands in one place. The delays are fixed rather than random so a failing
run can be repeated, but which instruction the guest was on when it died is
not reproducible, and that is inherent to what this measures.
"""

import os
import signal
import subprocess
import sys
import tempfile
import time

QEMU = "qemu-system-aarch64"

COUNT = 400        # files the writer tries to write
LENGTH = 200       # bytes in each, so a torn one is obvious


def args(disk):
    return [
        "-M", "virt,gic-version=3", "-cpu", "cortex-a72", "-m", "512M",
        "-nographic", "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={disk},format=raw,if=none,id=disk",
        "-device", "virtio-blk-device,drive=disk",
        "-kernel", "build/kosmos.elf",
    ]


class Failure(Exception):
    pass


def boot(image, disk, commands, kill_after=None, each=30.0):
    """Boots, types, and either waits or is killed part way through."""
    p = subprocess.Popen([QEMU, *args(disk)], stdout=subprocess.PIPE,
                         stdin=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(p.stdout.fileno(), False)
    out = b""

    def pump(seconds, until=None, mark=0):
        """Reads for a while, or until something new appears.

        `mark` is not optional in spirit. The prompt is already in the
        buffer from boot, so waiting for `kosmos>` without saying "after
        this point" returns immediately and the command never gets a chance
        to run - which is what the first version did, and it read exactly
        like the machine failing to come back.
        """
        nonlocal out
        start = time.time()

        while time.time() - start < seconds:
            chunk = p.stdout.read()

            if chunk:
                out += chunk
            else:
                time.sleep(0.02)

            if until and until in out[mark:]:
                return True

        return False

    try:
        if not pump(90, b"kosmos>"):
            raise Failure("the machine never reached a prompt.\n"
                          + out.decode("utf-8", "replace")[-800:])

        for command in commands:
            mark = len(out)
            p.stdin.write(command.encode() + b"\n")
            p.stdin.flush()

            if kill_after is not None:
                # The point of the exercise. No signal the guest can catch,
                # no chance to finish the block it was writing.
                pump(kill_after)
                p.send_signal(signal.SIGKILL)
                return out.decode("utf-8", "replace")

            pump(each, b"kosmos>", mark)
    finally:
        if p.poll() is None:
            p.kill()

        p.wait()

    return out.decode("utf-8", "replace")


WRITER = ('for i = 1, %d do fs.write("/home/t" .. i, '
          'string.rep(tostring(i %% 10), %d)) end print("WRITER DONE")'
          % (COUNT, LENGTH))

# Reads every name the directory claims to have, and every file the writer
# might have written. Both directions matter: the first catches an entry
# pointing at nothing, the second catches a file that came back wrong.
VERIFY = (
    'local listed, unreadable, torn, present = 0, 0, 0, 0 '
    'for _, name in ipairs(fs.list("/home") or {}) do '
    '  listed = listed + 1 '
    '  local v = fs.read("/home/" .. name) '
    '  if v == nil then unreadable = unreadable + 1 end '
    'end '
    'for i = 1, %d do '
    '  local v = fs.read("/home/t" .. i) '
    '  if v ~= nil then '
    '    present = present + 1 '
    '    if v ~= string.rep(tostring(i %% 10), %d) then torn = torn + 1 end '
    '  end '
    'end '
    'print("VERIFY", listed, unreadable, torn, present)' % (COUNT, LENGTH)
)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"

    handle = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
    handle.truncate(64 * 1024 * 1024)
    handle.close()
    disk = handle.name

    checks = 0
    replays = 0

    try:
        boot(image, disk, ["mkfs --yes"])

        # Several instants, because one only ever lands in one place.
        for at in (2.0, 3.5, 5.0, 6.5, 8.0):
            boot(image, disk, [WRITER], kill_after=at)

            back = boot(image, disk, [VERIFY])

            if "VERIFY" not in back:
                raise Failure(
                    "the machine did not come back after the power was cut "
                    f"{at}s into writing. The filesystem it was left with "
                    "cannot be mounted, which is the failure this whole "
                    "milestone is about.\n" + back[-1200:]
                )

            line = [l for l in back.splitlines() if "VERIFY" in l][-1]
            fields = line.replace("\t", " ").split()
            listed, unreadable, torn, present = (int(x) for x in fields[1:5])

            if unreadable > 0:
                raise Failure(
                    f"after the power was cut {at}s in, {unreadable} of "
                    f"{listed} names in the directory could not be read. "
                    "That is a directory entry pointing at an inode that "
                    "was never written - the filesystem disagreeing with "
                    "itself, which is exactly what the journal is for.\n"
                    + back[-800:]
                )

            checks += 1

            if torn > 0:
                raise Failure(
                    f"after the power was cut {at}s in, {torn} file(s) came "
                    "back holding something other than what was written to "
                    "them. A file is allowed to be missing; it is not "
                    "allowed to be half there.\n" + back[-800:]
                )

            checks += 1

            if "replayed" in back:
                replays += 1

            print(f"  cut at {at:>4}s: {present:>3} files survived, "
                  f"{listed} names, all readable, none torn"
                  + ("  (journal replayed)" if "replayed" in back else ""))

        # Recovery itself is not tested here, and deliberately not.
        #
        # These five runs prove the filesystem is never left inconsistent,
        # which is the property that matters and is what a power cut can
        # actually demonstrate. They cannot prove the journal was *replayed*:
        # that needs the machine to die between the commit block landing and
        # the last data block reaching its home, which is a few milliseconds
        # inside a fifty-millisecond write. Five kills will usually miss it,
        # and an assertion that fails when a coin comes up tails is worse
        # than no assertion.
        #
        # `tools/test_kfs.lua` tests replay by choosing the instant instead
        # of aiming at it.
        print(f"\nPASS: {checks} checks across five power cuts"
              + (f", {replays} of which replayed a transaction" if replays
                 else "")
              + ".")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        os.unlink(disk)


if __name__ == "__main__":
    sys.exit(main())
