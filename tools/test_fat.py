#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""FAT volumes mtools made, read back through `fat_decode.c` on this Mac.

`tools/test_fatdecode.c` builds its bytes from the specification, so a field
read at the wrong offset there is also written at the wrong offset, and the
two agree. This is the other half: **mtools**, somebody else's reading of the
same format, makes FAT16 and FAT32 volumes - with no partition table, and in
an MBR partition at sector 2048 - and puts real files in them. Then `fatls`,
which walks a volume with nothing but `fat_decode.c`, has to find every
directory and every file, each with the size and the bytes that went in.

The files are the cases a reader gets wrong: a short name, long names, a name
with an accent in it, an empty file, files one byte either side of a cluster,
a directory inside a directory, and a file mtools had to spread over two runs
of clusters because another file was deleted from the middle first. And one
file is looked up in the wrong case, as FAT looks names up.

Usage: test_fat.py FATLS
"""

import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

SECTOR = 512
IMAGE_SECTORS = 131072                   # 64 MB

# (what, mformat's arguments, the kind fatls must say, sectors a cluster)
KINDS = [
    ("FAT16", ["-c", "4"], "FAT16", 4),
    ("FAT32", ["-F", "-c", "1"], "FAT32", 1),
]

LAYOUTS = [
    ("no partition table", 0),
    ("an MBR partition at sector 2048", 2048),
]


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def run(argv, env=None):
    result = subprocess.run(argv, capture_output=True, text=True, env=env)
    if result.returncode != 0:
        raise SystemExit("test_fat: %s failed: %s" % (" ".join(argv),
                                                       result.stderr.strip()))
    return result.stdout


def mbr(path, first, sectors, kind):
    """One partition of FAT's LBA type, as a PC's partition table holds it:
    the entry at byte 446, its type at +4, first sector at +8, count at +12,
    and 0x55 0xAA at byte 510."""
    table = bytearray(SECTOR)
    table[446 + 4] = 0x0C if kind == "FAT32" else 0x0E
    table[446 + 8:446 + 16] = struct.pack("<II", first, sectors)
    table[510:512] = b"\x55\xAA"

    with open(path, "r+b") as f:
        f.write(table)


def fix_hint(path, first):
    """FSI_Nxt_Free set to 2, in the FSInfo sector BPB_FSInfo (byte 48 of the
    boot sector) names - after checking FSI_LeadSig says it is one."""
    with open(path, "r+b") as f:
        f.seek(first * SECTOR + 48)
        fsinfo = struct.unpack("<H", f.read(2))[0]
        at = (first + fsinfo) * SECTOR
        f.seek(at)
        if struct.unpack("<I", f.read(4))[0] != 0x41615252:
            raise SystemExit("test_fat: mtools wrote no FSInfo sector at sector %d"
                             % fsinfo)
        f.seek(at + 492)
        f.write(struct.pack("<I", 2))


def tree(root, cluster):
    """The files, written on this Mac first; what is expected is read back
    from here, never from the volume."""
    rng = random.Random(6)

    def put(name, data):
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    put("hello.txt", b"hello\n")
    put("The quick brown.fox", b"jumps over the lazy dog\n" * 100)
    put("Café menu.txt", "croissant, café au lait\n".encode("utf-8"))
    put("empty.dat", b"")
    put("one-cluster.bin", bytes(rng.getrandbits(8) for _ in range(cluster)))
    put("one-cluster-less-one.bin", bytes(rng.getrandbits(8) for _ in range(cluster - 1)))
    put("one-cluster-and-one.bin", bytes(rng.getrandbits(8) for _ in range(cluster + 1)))
    put("Photos 2024/Italy/IMG_2213.JPG", bytes(rng.getrandbits(8) for _ in range(300000)))
    put("Photos 2024/Italy/Rome/notes.TXT", b"the Pantheon at nine\n")


def expected(root):
    dirs, files = set(), {}

    for here, subdirs, names in os.walk(root):
        rel = os.path.relpath(here, root)
        prefix = "" if rel == "." else "/" + rel

        for name in subdirs:
            dirs.add(prefix + "/" + name)

        for name in names:
            with open(os.path.join(here, name), "rb") as f:
                data = f.read()
            files[prefix + "/" + name] = (len(data), fnv1a64(data))

    return dirs, files


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print("SKIP: no fatls. Run `make test`, which builds build/host/fatls.")
        return 0

    fatls = sys.argv[1]

    if shutil.which("mformat") is None:
        print("FAIL: mtools is not installed, and FAT's second reading is mtools.")
        return 1

    env = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="en_US.UTF-8",
               LANG="en_US.UTF-8")
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    for what, mformat, kind, spc in KINDS:
        for where, first in LAYOUTS:
            name = "%s, %s" % (what, where)
            work = tempfile.mkdtemp(prefix="kosmos-fat-")

            try:
                image = os.path.join(work, "drive.img")
                volume_sectors = IMAGE_SECTORS - first
                spec = "%s@@%d" % (image, first * SECTOR)
                src = os.path.join(work, "files")

                with open(image, "wb") as f:
                    f.truncate(IMAGE_SECTORS * SECTOR)

                if first:
                    mbr(image, first, volume_sectors, kind)

                # Eleven characters at most: BS_VolLab is eleven bytes.
                label = "KOSMOS " + what.replace("FAT", "F")
                run(["mformat", "-i", spec, "-T", str(volume_sectors),
                     "-v", label] + mformat + ["::"], env)

                cluster = spc * SECTOR

                # A hole in the middle first, so the file after it has to be
                # spread over two runs of clusters.
                hole = os.path.join(work, "hole.bin")
                with open(hole, "wb") as f:
                    f.write(b"\x00" * (3 * cluster))
                run(["mcopy", "-i", spec, hole, "::/hole.bin"], env)

                tree(src, cluster)
                for entry in sorted(os.listdir(src)):
                    run(["mcopy", "-s", "-i", spec, os.path.join(src, entry),
                         "::/"], env)

                run(["mdel", "-i", spec, "::/hole.bin"], env)

                # FAT32 keeps a hint of where to look for a free cluster, and
                # mtools follows it past the hole. 2 is where the specification
                # says a driver with no hint begins (FSI_Nxt_Free, in the
                # sector BPB_FSInfo names), and the hole is the first free run
                # after it.
                if kind == "FAT32":
                    fix_hint(image, first)

                spread = os.path.join(src, "spread.bin")
                with open(spread, "wb") as f:
                    f.write(bytes((i * 7) & 0xFF for i in range(6 * cluster)))
                run(["mcopy", "-i", spec, spread, "::/spread.bin"], env)

                out = run([fatls, image, str(first)])
                lines = out.splitlines()
                head = lines[0].split(" ")

                check(head[1] == kind,
                      "%s: fatls read it as %s, and mtools made %s"
                      % (name, head[1], kind))
                check(out.splitlines()[0].endswith("label " + label),
                      "%s: the label is not %r: %r" % (name, label, lines[0]))

                dirs, files = expected(src)
                got_dirs, got_files, runs = set(), {}, {}

                for line in lines[1:]:
                    if line.startswith("dir "):
                        got_dirs.add(line[4:])
                    elif line.startswith("file "):
                        path, size, digest, count = line[5:].rsplit(" ", 3)
                        got_files[path] = (int(size), int(digest, 16))
                        runs[path] = int(count)

                check(got_dirs == dirs,
                      "%s: directories differ - put in %s, read %s"
                      % (name, sorted(dirs), sorted(got_dirs)))

                wrong = sorted(p for p in set(files) | set(got_files)
                               if files.get(p) != got_files.get(p))
                check(not wrong,
                      "%s: files differ in name, size or bytes: %s"
                      % (name, ["%s put %s read %s" % (p, files.get(p), got_files.get(p))
                                for p in wrong][:6]))

                check(runs.get("/spread.bin", 0) >= 2,
                      "%s: spread.bin is in %s run(s), so no chain here jumped"
                      % (name, runs.get("/spread.bin")))

                size, digest = files["/Photos 2024/Italy/IMG_2213.JPG"]
                found = run([fatls, image, str(first), "--find",
                             "photos 2024/ITALY/img_2213.jpg"]).splitlines()[1]
                check(found == "found /Photos 2024/Italy/IMG_2213.JPG %d %016x"
                      % (size, digest),
                      "%s: photos 2024/ITALY/img_2213.jpg did not find the "
                      "picture: %r" % (name, found))
            finally:
                shutil.rmtree(work, ignore_errors=True)

    if fails:
        print("FAIL: %d of %d checks on FAT volumes mtools made:"
              % (len(fails), len(fails) + checks))
        for complaint in fails:
            print("  " + complaint)
        return 1

    print("PASS: %d checks on FAT volumes mtools made (FAT16 and FAT32, with no "
          "partition table and in an MBR partition: every directory and file "
          "read back with its size and bytes, a chain in two runs, and a name "
          "found in the wrong case)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
