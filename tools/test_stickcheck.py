#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`stickcheck.py` asked about sticks whose faults are known.

`mkusb.sh` trusts its answer about a real stick, and a real stick with a known
fault is not something a test can own - so this makes the faults. The image
`make test` has just built is streamed to `stickcheck.py` the way a stick is
read back, with bytes changed where each kind of fault puts them. **Nothing is
copied**: two hundred megabytes twice over is what fills this Mac's disk, and
a full disk has already taken the gate down once.

**Where to put each fault comes from mtools, not from `stickcheck.py`.** The
checker reads the FAT to name what it finds; if this read the FAT with the
same code, a wrong offset would put the fault in the wrong place and the
checker would name the wrong place correctly, and both would agree. `minfo`
and `mshowfat` are a second reading of the same filesystem.

Every variant has the verdict it must get and a phrase that has to be in the
answer, because a checker that says "differs" about the right stick and the
wrong place is the failure worth catching.

Given a third image, one `mkusb_image.py --home` made, it asks about that
one too: the image itself, and a byte of its Kosmos partition changed - which
has to be damage, named as `/home`'s partition rather than as the backup GPT.

Usage: test_stickcheck.py IMAGE [KERNEL.ELF [HOME-IMAGE]]
"""

import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512
CHUNK = 4 * 1024 * 1024


def mtools(*argv):
    return subprocess.run(list(argv), capture_output=True, text=True).stdout


def last_cluster(spec, path):
    """The highest cluster mtools lists for a file, over all its runs."""
    listing = mtools("mshowfat", "-i", spec, path)
    runs = re.findall(r"<(\d+)(?:-(\d+))?>", listing)

    if not runs:
        raise SystemExit("test_stickcheck: mshowfat has no clusters for %s: %r"
                         % (path, listing))

    return max(int(b or a) for a, b in runs)


def first_cluster(spec, path):
    listing = mtools("mshowfat", "-i", spec, path)
    found = re.search(r"<(\d+)", listing)

    if found is None:
        raise SystemExit("test_stickcheck: mshowfat has no clusters for %s: %r"
                         % (path, listing))

    return int(found.group(1))


def ask(image, elf, patches=(), stop=None):
    """Streams the image with `patches` applied - (offset, bytes) - and
    stopped short at `stop` if given; answers the exit code and the text."""
    argv = [sys.executable, os.path.join(HERE, "stickcheck.py"), image, "-"]
    p = subprocess.Popen(argv + ([elf] if elf else []), stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    limit = os.path.getsize(image) if stop is None else stop

    try:
        with open(image, "rb") as f:
            position = 0

            while position < limit:
                chunk = bytearray(f.read(min(CHUNK, limit - position)))

                for at, data in patches:
                    for i, value in enumerate(data):
                        if position <= at + i < position + len(chunk):
                            chunk[at + i - position] = value

                p.stdin.write(chunk)
                position += len(chunk)

        p.stdin.close()
    except BrokenPipeError:
        pass

    return p.wait(), p.stdout.read().decode("utf-8", "replace")


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("SKIP: no stick image to check. Run `make test`, which builds "
              "build/x86_64/kosmos-uefi.img.")
        return 0

    image = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else None

    with open(image, "rb") as f:
        f.seek(SECTOR)
        header = f.read(92)
        f.seek(struct.unpack_from("<Q", header, 72)[0] * SECTOR)
        esp = struct.unpack_from("<Q", f.read(128), 32)[0] * SECTOR

    spec = "%s@@%d" % (image, esp)
    info = mtools("minfo", "-i", spec)

    def number(pattern):
        found = re.search(pattern, info)

        if found is None:
            raise SystemExit("test_stickcheck: minfo said nothing matching %r"
                             % pattern)

        return int(found.group(1))

    bps = number(r"sector size: (\d+) bytes")
    spc = number(r"cluster size: (\d+) sectors")
    reserved = number(r"reserved \(boot\) sectors: (\d+)")
    fats = number(r"fats: (\d+)")
    fat_len = number(r"Big fatlen=(\d+)")

    fat = esp + reserved * bps
    data = esp + (reserved + fats * fat_len) * bps
    cluster = spc * bps

    def at_cluster(n):
        return data + (n - 2) * cluster

    kernel = at_cluster(first_cluster(spec, "::/boot/kosmos.bin"))
    disk = at_cluster(first_cluster(spec, "::/boot/disk.img"))
    boot_dir = at_cluster(first_cluster(spec, "::/boot"))
    root_dir = at_cluster(number(r"rootCluster=(\d+)"))

    with open(image, "rb") as f:
        f.seek(boot_dir)
        listing = f.read(cluster)
        f.seek(root_dir)
        root = f.read(cluster)
        f.seek(esp + bps)
        fsinfo = f.read(bps)

    entry = listing.find(b"KOSMOS  BIN")
    free_slot = next(o for o in range(0, cluster, 32) if root[o] == 0)

    #
    # **A cluster the image left free**, found in its FAT rather than guessed.
    # This was a fixed eight megabytes past the start of the disk, which is
    # free on `make test`'s 4 MB disk and inside the 32 MB one a ThinkPad stick
    # carries - where the "mount" wrote into the disk, and the checker quite
    # rightly called it damage. The FAT is read here, past the last cluster
    # mtools lists for any file, not by `stickcheck.py`.
    #
    with open(image, "rb") as f:
        f.seek(fat)
        table = f.read(fat_len * bps)

    after = max(last_cluster(spec, path) for path in
                ("::/EFI/BOOT/BOOTX64.EFI", "::/boot/kosmos.bin",
                 "::/boot/disk.img")) + 64
    spare = next(c for c in range(after, len(table) // 4)
                 if struct.unpack_from("<I", table, c * 4)[0] & 0x0FFFFFFF == 0)
    free_count = struct.unpack_from("<I", fsinfo, 488)[0]

    checks = 0
    fails = []

    def expect(what, answer, code, phrase):
        nonlocal checks
        got, text = answer

        if got == code and phrase.lower() in text.lower():
            checks += 1
        else:
            fails.append("%s: wanted exit %d and %r, got exit %d:\n    %s"
                         % (what, code, phrase, got,
                            "\n    ".join(text.splitlines()[:6])))

    expect("the image itself", ask(image, elf), 0, "the stick holds the image")

    #
    # **And read through a path, which is how `mkusb.sh` reads a stick** -
    # the raw device, unbuffered, with no pipe. The first version piped `dd`
    # in, and on a real stick `dd` read past the image, died of SIGPIPE when
    # the checker was done, and its exit code refused a stick that had just
    # been called perfect. There is no pipe to break now; this is the path
    # that replaced it.
    #
    direct = subprocess.run([sys.executable, os.path.join(HERE, "stickcheck.py"),
                             image, image] + ([elf] if elf else []),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    expect("the image read through a path, as mkusb.sh reads a stick",
           (direct.returncode, direct.stdout.decode("utf-8", "replace")), 0,
           "the stick holds the image")

    expect("a byte of the kernel's .text",
           ask(image, elf, [(kernel + 0x2000, b"\xa5")]), 1,
           "kernel page 2 at 0x01002000" + (" in .text" if elf else ""))

    expect("a byte of the userland image",
           ask(image, elf, [(kernel + 0x200010, b"\xa5")]), 1,
           "kernel page 512 at 0x01200000"
           + (" in .rodata (the userland image)" if elf else ""))

    expect("a byte of the disk",
           ask(image, elf, [(disk + 0x10000, b"\xa5")]), 1,
           "/boot/disk.img, byte 0x10000")

    expect("the GPT header's checksum",
           ask(image, elf, [(SECTOR + 16, b"\x00\x00\x00\x00")]), 1,
           "damage")

    expect("a stick that gives back half",
           ask(image, elf, stop=os.path.getsize(image) // 2 // SECTOR * SECTOR),
           1, "gave back")

    expect("the kernel's directory entry",
           ask(image, elf, [(boot_dir + entry, b"X")]), 1,
           "the directory /boot")

    expect("a link in the kernel's FAT chain",
           ask(image, elf,
               [(fat + (first_cluster(spec, "::/boot/kosmos.bin") + 5) * 4,
                 struct.pack("<I", 0x0FFFFFFF))]), 1, "damage")

    #
    # **What a macOS mount writes**, made by hand: a free cluster taken in
    # both FATs, a directory entry in a slot the root left empty pointing at
    # it, something in the cluster, and FSInfo's free count one lower. All of
    # it bookkeeping, and none of it may be called damage.
    #
    mount = [(fat + spare * 4, struct.pack("<I", 0x0FFFFFFF)),
             (fat + fat_len * bps + spare * 4, struct.pack("<I", 0x0FFFFFFF)),
             (root_dir + free_slot,
              b"FSEVEN~1   \x10" + b"\x00" * 8
              + struct.pack("<H", spare >> 16) + b"\x00" * 4
              + struct.pack("<H", spare & 0xFFFF) + b"\x00" * 4),
             (at_cluster(spare), b"fseventsd-uuid\n"),
             (esp + bps + 488, struct.pack("<I", free_count - 1))]

    expect("what mounting the stick writes", ask(image, elf, mount), 3,
           "which is what mounting the stick writes")

    home = sys.argv[3] if len(sys.argv) > 3 else None

    if home is not None:
        with open(home, "rb") as f:
            f.seek(SECTOR)
            header = f.read(92)
            f.seek(struct.unpack_from("<Q", header, 72)[0] * SECTOR + 128)
            second = f.read(128)

        start = struct.unpack_from("<Q", second, 32)[0] * SECTOR

        expect("the home stick itself", ask(home, None), 0,
               "the stick holds the image")
        expect("a byte of /home's partition",
               ask(home, None, [(start + 5000, b"\xA5")]), 1,
               "the Kosmos partition, /home")

    if fails:
        print("FAIL: %d of %d stickcheck checks:" % (len(fails),
                                                      len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d stickcheck checks (the image itself, streamed and read "
          "through a path as mkusb.sh reads a stick, six kinds of damage named "
          "where they are, a short read, and a mount's bookkeeping told from "
          "damage)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
