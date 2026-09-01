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
            "mkdir /home/papers",
            "save papers/deep.txt inside a directory",
            'fs.write("/home/kept", { palette = "light", n = 42 })',
            # Attributes, which is the half of this filesystem that is not
            # ext2. Set on the first boot and asked for on the second: the
            # whole question is whether a thing said *about* a file lasts
            # as long as the file does.
            "attr /home/notes.txt kind=note author=diego",
            "attr /home/notes.txt size=999",
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
        second = boot(image, disk, ["diskinfo", "ls /home", "ls /home/papers",
                                    "cat /home/notes.txt",
                                    "cat /home/papers/deep.txt",
                                    'local t = fs.read("/home/kept"); '
                                    'print("kept:", type(t), t and t.palette, '
                                    't and t.n)',
                                    "attr /home/notes.txt"])

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

        if "papers" not in second.split("ls /home")[1]:
            raise Failure(
                "the directory is gone after the reboot.\n" + second
            )

        checks += 1

        if "inside a directory" not in second:
            raise Failure(
                "a file inside a directory did not survive the reboot. The "
                "root directory came back and the one below it did not, "
                "which means a directory's own data blocks are not being "
                "written the way the root's are.\n" + second
            )

        checks += 1

        if "written before the reboot" not in second.split("cat ")[1]:
            raise Failure(
                "the file is listed after the reboot but its contents did "
                "not come back. The directory entry survived and the data "
                "blocks did not.\n" + second
            )

        checks += 1

        # A *value* stored as one has to come back as one. The disk holds
        # bytes, so a table is serialised on the way down - and the first
        # version did not do that, wrote a nought-byte file, raised nothing,
        # and the desktop's own settings silently never saved.
        if "kept:\ttable\tlight\t42" not in second.replace("  ", "\t"):
            if "kept: table light 42" not in " ".join(second.split()):
                raise Failure(
                    "a table written to the disk did not come back as a "
                    "table with its fields. Either it was stored as bytes "
                    "and never decoded, or it was stored as nothing at "
                    "all.\n" + second
                )

        checks += 1

        # ---- attributes ----
        #
        # A file's contents surviving and the things said about it not
        # surviving would be a filesystem that is ext2 and nothing more.
        # This is the part that makes it worth having written.
        after = second.split("attr /home/notes.txt")[-1]

        for name, value in (("kind", "note"), ("author", "diego")):
            if name not in after or value not in after:
                raise Failure(
                    f"the attribute `{name}` did not survive the reboot. "
                    "The file came back and what was said about it did "
                    "not, which means the attribute block is not being "
                    "written, or the inode is not being pointed at it.\n"
                    + second
                )

            checks += 1

        # And the derived ones are still the file's own. `size=999` was
        # offered on the first boot and has to have been refused: a stored
        # size is a second copy of a fact, and the failure it produces is a
        # listing that disagrees with the file months later.
        if "999" in after:
            raise Failure(
                "`size` was stored as an attribute. It is read out of the "
                "inode, so there are now two answers to how big this file "
                "is, and they will disagree the moment it is written to.\n"
                + second
            )

        checks += 1

        if "not something you can set" not in first:
            raise Failure(
                "setting `size` was not refused out loud. Quietly dropping "
                "it leaves the caller believing it was stored.\n" + first
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
