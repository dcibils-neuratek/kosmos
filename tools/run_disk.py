#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
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

PROMPT = "kosmos>"


class Failure(Exception):
    pass


#
# Which machine this image is for, told from its path.
#
# The same trick `run_screenshot.py` uses and for the same reason: an image
# is the only argument these harnesses take, and `build/x86_64/` is where
# the Makefile puts the other architecture's. Duplicated rather than
# imported because `run_interchange` and `run_queries` build on this file
# and nothing else here needs `run_screenshot`.
#
def machine(image):
    return "x86_64" if "x86_64" in image else "aarch64"


def qemu_args(disk, image):
    """The board this image is for, with one disk attached.

    The two lists are the same machine described twice: q35 finds its
    devices by walking PCI, `virt` by reading a device tree, and the drive
    behind them is the same file. `-vga none` is not optional on q35 - it
    adds a VGA adapter unless told not to, and QEMU then scans out that one
    instead of ramfb.
    """
    if machine(image) == "x86_64":
        return [
            "qemu-system-x86_64",
            "-M", "q35", "-m", "512M",
            "-nographic", "-vga", "none", "-device", "ramfb",
            "-drive", f"file={disk},format=raw,if=none,id=disk",
            "-device", "virtio-blk-pci,drive=disk",
        ]

    return [
        "qemu-system-aarch64",
        "-M", "virt,gic-version=3", "-cpu", "cortex-a72", "-m", "512M",
        "-nographic", "-device", "ramfb",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={disk},format=raw,if=none,id=disk",
        "-device", "virtio-blk-device,drive=disk",
    ]


def boot(image, disk, commands, boot_timeout=90, each=25):
    """One run of the machine. Returns everything printed after the prompt."""
    proc = subprocess.Popen(
        [*qemu_args(disk, image), "-kernel", image],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, bufsize=0,
    )

    seen = ""

    def pump(seconds, until=None, since=0):
        """Read for a while, or until `until` shows up after `since`.

        `since` is a length into `seen`, and it is what makes waiting for
        the prompt work more than once: the prompt is already in `seen` from
        the last time, so a plain `until in seen` would return instantly
        and every command would be sent into a shell still busy with the
        one before it.

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

            if until is not None and until in seen[since:]:
                return

    try:
        pump(boot_timeout, until=PROMPT)

        if PROMPT not in seen:
            raise Failure("the machine never reached a shell prompt.\n" + seen[-800:])

        start = len(seen)

        #
        # **`each` is a deadline, not a duration**, and that is worth the
        # two lines it costs. It used to be a duration: every command
        # waited the full twenty-five seconds whether the shell answered in
        # a tenth of a second or not at all, so two boots of ten commands
        # took eight minutes of doing nothing. The prompt says when the
        # shell is ready, and it is the same signal the boot already waits
        # on - there was no reason for the two to differ.
        #
        for command in commands:
            mark = len(seen)

            proc.stdin.write((command + "\n").encode())
            proc.stdin.flush()
            pump(each, until=PROMPT, since=mark)

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
            "attr /home/papers/deep.txt kind=note",
            # A directory larger than one message. `list` answers in
            # pieces the way `read` does, and before it did, `ls` on a
            # directory like this showed nothing at all.
            # In a directory of their own. They were in /home first, and
            # that made `ls /home` three hundred lines long - which the
            # harness waits on over a serial line, and gave up on. A test
            # that changes the thing another test measures is a test that
            # breaks its neighbours.
            'fs.send("/home/many", { type = "mkdir" }) '
            'for i = 1, 300 do fs.write("/home/many/f" .. i, "x") end '
            'print("MADE MANY")',
            'local l, e = fs.list("/home/many") '
            'print("LISTED", l and #l or -1, tostring(e))',
            # A file a hundred times larger than a message, out and back
            # through pages the caller owns. This is `read(fd, buf, n)`
            # with the buffer named by a capability, and without it
            # nothing above about two kilobytes could be written at all.
            'local buf = sys.memory(64) '
            'for i = 0, 49 do sys.region_write(buf, i * 4096, '
            'string.rep(string.char(65 + i % 26), 4096)) end '
            'print("BIGWROTE", fs.write_from("/home/big", buf, 200 * 1024))',
            "ls /home",
        ])

        #
        # **A blank disk formats itself, and this used to insist it did
        # not.**
        #
        # The check was `filesystem: none`, and it was right when it was
        # written: a zeroed disk reported no filesystem and somebody had to
        # type `mkfs`. `init.lua`'s `mounted()` replaced that deliberately -
        # a disk of all zeros has nothing to lose, and making a person
        # perform a ceremony over an empty box was worse than theoretical,
        # because anyone booting straight to the desktop never saw the line
        # telling them to.
        #
        # So the two have contradicted each other since, on both boards.
        # Nothing caught it because this harness is not in `make test` - it
        # was reached only through `disktest` - which is what running it on
        # a second architecture is good for and why it is in `make test`
        # now.
        #
        # `mounted()` does print "it was blank, so it has been formatted",
        # and that is deliberately *not* what this looks for: the mount is
        # lazy, the first thing to touch the filesystem is init itself, and
        # the line is written before the console server is serving anyone.
        # It goes nowhere. The consequence is what can be checked.
        #
        # What the old check was really protecting is still protected, and
        # by the other branch: a disk whose block 0 holds something that is
        # not a superblock reports `filesystem: none (not a kosmos
        # filesystem)` and is left alone. A superblock check that accepted
        # anything would fail *that*, and it is the case worth having,
        # because it is the one where somebody's data is at stake.
        #
        if "filesystem: version" not in first:
            raise Failure(
                "a freshly zeroed disk did not come up with a filesystem. "
                "A blank disk is supposed to format itself once - see "
                "`mounted()` in init.lua - so either it was not recognised "
                "as blank, the format failed, or diskinfo could not reach "
                "the disk server.\n" + first
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
                                    "attr /home/notes.txt",
                                    # The index is not on the disk. These
                                    # answers can only come from a scan of
                                    # the attributes done on this boot.
                                    "find kind=note",
                                    "find name=deep.txt",
                                    'local b = sys.memory(64) '
                                    'local got, size = fs.read_into('
                                    '"/home/big", b, 0, 200 * 1024) '
                                    'print("BIGREAD", got, size, '
                                    'sys.region_read(b, 0, 3), '
                                    'sys.region_read(b, 8192, 3))'])

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

        # The output of `ls /home` and nothing else. Splitting on the
        # command and taking the last piece used to work by accident: it
        # landed after `ls /home/papers`, which also contains "ls /home",
        # and found the name in a later `cat` line instead of in the
        # listing. It stopped working the moment the directory grew.
        home_listing = second.split("ls /home/papers")[0].split("ls /home")[-1]

        if "notes.txt" not in home_listing:
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

        # ---- the index, rebuilt rather than stored ----
        #
        # Nothing about a query is written down. The answers below exist
        # because this boot walked the filesystem and read every
        # attribute block, which is the whole design: derived state that
        # is also stored is state that can disagree with itself, and on a
        # filesystem that disagreement is a query returning a file that
        # is not there.
        matched = second.split("find kind=note")[-1]

        for path in ("/home/notes.txt", "/home/papers/deep.txt"):
            if path not in matched:
                raise Failure(
                    f"a query after the reboot did not find {path}. The "
                    "attribute survived - the check above proves it - so "
                    "the index was not rebuilt from what is on the disk.\n"
                    + second
                )

            checks += 1

        # And it is a query, not a walk that returns everything.
        if "/home/kept" in matched.split("find name=deep.txt")[0]:
            raise Failure(
                "the query returned a file that does not match it. That "
                "is a walk wearing a query's name, and it means the "
                "filter is not being applied at all.\n" + second
            )

        checks += 1

        # `name` is indexed without anyone having declared it, which is
        # what makes a query by name fast however many files there are.
        if "/home/papers/deep.txt" not in second.split("find name=deep.txt")[-1]:
            raise Failure(
                "a query by name found nothing. Name is supposed to be "
                "indexed for every file without being declared - it is "
                "not stored on the node at all, it comes from the "
                "directory entry during the scan.\n" + second
            )

        checks += 1

        # ---- a directory bigger than a message ----
        listed = first.split("LISTED")[-1].split()

        if not listed or int(listed[0]) < 300:
            raise Failure(
                "a directory of three hundred files did not list. A reply "
                "is 2048 bytes and three hundred names are not, so `list` "
                "has to answer in pieces the way `read` does - without it "
                "the files are all still there and nothing can see them.\n"
                + first[-600:]
            )

        checks += 1

        # ---- a file far larger than a message, across a reboot ----
        big = second.replace("\t", " ").split("BIGREAD")[-1].split()

        if len(big) < 4 or big[0] != "204800" or big[1] != "204800":
            raise Failure(
                "a 200 KB file did not come back through a shared buffer. "
                "A message is 2048 bytes, so this is the only way anything "
                "larger than that can be read or written at all.\n"
                + second[-700:]
            )

        checks += 1

        if big[2] != "AAA" or big[3] != "CCC":
            raise Failure(
                "the 200 KB file came back the right length and the wrong "
                "contents: expected AAA at offset 0 and CCC at 8192, got "
                f"{big[2]} and {big[3]}. The extents are being walked "
                "wrongly, which is a file that reads as somebody else's "
                "data.\n" + second[-700:]
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
