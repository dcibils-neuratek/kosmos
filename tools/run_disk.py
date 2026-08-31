#!/usr/bin/env python3
"""Does what was written to the disk survive the machine being turned off?

That question cannot be answered inside one boot, which is why this is a
separate harness rather than another entry in `run_tests.py`. It boots the
machine twice against the same image file: the first run formats, the second
run is a brand new machine that has never seen a disk before, and it has to
find a filesystem there.

Everything else about M8 can be tested in memory and would pass with a
filesystem that quietly forgot everything at power off. This is the check
that cannot.

The disk is made fresh here, so a pass can never be a leftover from a
previous run - which is the same failure as not having written anything.
"""

import os
import select
import subprocess
import sys
import tempfile
import time

QEMU = "qemu-system-aarch64"
PROMPT = "kosmos>"


class Failure(Exception):
    pass


def qemu_args(disk):
    return [
        QEMU,
        "-M", "virt,gic-version=3", "-cpu", "cortex-a72", "-m", "512M",
        "-nographic", "-device", "ramfb",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={disk},format=raw,if=none,id=disk",
        "-device", "virtio-blk-device,drive=disk",
    ]


def boot(image, disk, commands, boot_timeout=90, each=25):
    """One run of the machine. Returns everything printed after the prompt."""
    proc = subprocess.Popen(
        [*qemu_args(disk), "-kernel", image],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, bufsize=0,
    )

    seen = ""

    def pump(seconds, until=None):
        """Read for a while, or until `until` shows up.

        Raw bytes, never `readline`. **The shell's prompt has no trailing
        newline**, so `readline` blocks for ever on the one string this
        harness most needs to see: select says the pipe is ready, and then
        the read waits for a line ending that is not coming until something
        else is printed. That cost an hour, and it is the same reason
        run_screenshot.py reads the way it does.
        """
        nonlocal seen
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [], 0.2)

            if not ready:
                continue

            chunk = os.read(proc.stdout.fileno(), 65536)

            if not chunk:
                return                      # QEMU exited

            seen += chunk.decode("utf-8", "replace")

            if until is not None and until in seen:
                return

    try:
        pump(boot_timeout, until=PROMPT)

        if PROMPT not in seen:
            raise Failure("the machine never reached a shell prompt.\n" + seen[-800:])

        start = len(seen)

        for command in commands:
            proc.stdin.write((command + "\n").encode())
            proc.stdin.flush()
            pump(each)

        return seen[start:]
    finally:
        proc.kill()
        proc.wait()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"

    handle = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
    handle.truncate(64 * 1024 * 1024)
    handle.close()
    disk = handle.name

    checks = 0

    try:
        # ---- first boot: there is nothing, then there is, then a file ----
        first = boot(image, disk, [
            "diskinfo",
            "mkfs --yes",
            "save notes.txt written before the reboot",
            "ls /home",
        ])

        if "filesystem: none" not in first:
            raise Failure(
                "a freshly zeroed disk did not report an empty filesystem. "
                "Either the superblock check accepts zeroes - which would "
                "make every unformatted disk look like a filesystem - or "
                "diskinfo could not reach the disk server.\n" + first
            )

        checks += 1

        if "Formatted." not in first:
            raise Failure("mkfs --yes did not report a format.\n" + first)

        checks += 1

        if "saved notes.txt" not in first:
            raise Failure("the file was not written.\n" + first)

        checks += 1

        if "read back: written before the reboot" not in first:
            raise Failure(
                "the file did not read back as what was written, in the same "
                "boot that wrote it. That is the filesystem, not "
                "persistence.\n" + first
            )

        checks += 1

        if "notes.txt" not in first.split("ls /home")[-1]:
            raise Failure("the file is not in the directory listing.\n" + first)

        checks += 1

        # ---- second boot: a machine that has never seen this disk --------
        second = boot(image, disk, ["diskinfo", "ls /home",
                                    "cat /home/notes.txt"])

        if "filesystem: none" in second or "filesystem: version" not in second:
            raise Failure(
                "the filesystem did not survive the reboot. The format "
                "wrote something the second boot could not read back, which "
                "is the one thing this whole milestone is about.\n" + second
            )

        checks += 1

        # The layout has to come back as itself, not merely as something.
        for field in ("bitmap  at block", "inodes  at block",
                      "journal at block", "data    at block"):
            if field not in second:
                raise Failure(
                    f"the superblock came back without `{field.strip()}`.\n"
                    + second
                )

            checks += 1

        if "notes.txt" not in second.split("ls /home")[-1]:
            raise Failure(
                "the file is not in the directory listing after the reboot. "
                "The superblock survived and the directory did not, which "
                "means the root inode or its data blocks were not written.\n"
                + second
            )

        checks += 1

        if "written before the reboot" not in second.split("cat ")[-1]:
            raise Failure(
                "the file is listed after the reboot but its contents did "
                "not come back. The directory entry survived and the data "
                "blocks did not.\n" + second
            )

        checks += 1

        print(second.strip())
        print(f"\nPASS: {checks} disk checks across two boots of one image.")
        return 0

    except Failure as e:
        print(f"\nFAIL: {e}", file=sys.stderr)
        return 1
    finally:
        os.unlink(disk)


if __name__ == "__main__":
    raise SystemExit(main())
