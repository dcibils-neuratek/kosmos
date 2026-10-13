#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Alpine Linux for a sandbox: its kernel, unwrapped, from a pinned release.

    python3 tools/alpine.py build/sandboxes/alpine [--shell]

`docs/virtualization.md` V3 - Diego, 12 October 2026: "the initial goal for
sandboxes is to run a linux distro like alpine". Alpine 3.24.2 for aarch64,
from Alpine's own mirror, each file held to the SHA-256 Alpine publishes
for it (`latest-releases.yaml`) before anything is read out of it:

  * the netboot tarball's `vmlinuz-virt` - Linux 6.18 built for virtual
    machines. It is an EFI "zboot" image: a PE stub around a gzip-compressed
    arm64 `Image`, the header saying where (`zimg` at 4, the payload's offset
    and size at 8). Unwrapped here, so a sandbox loads a plain Image and its
    monitor needs no decompressor;
  * the minirootfs, Alpine's userland - for the initramfs, V3c.

Fetched into `build/downloads/alpine` once and kept; written to the folder
given as `Image` and `initramfs` - the minirootfs as a newc archive with an
`/init` of ours - which is a sandbox folder as `/Home/Sandboxes/<name>`
holds one. `--shell` makes the initramfs end in a shell rather than switch
itself off.
"""

import hashlib
import os
import struct
import sys
import tarfile
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOWNLOADS = os.path.join(ROOT, "build", "downloads", "alpine")
MIRROR = "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/"

NETBOOT = ("alpine-netboot-3.24.2-aarch64.tar.gz",
           "124087f5df9ac698bb4e942d603eaae96721e451cdc4706c71a5e42ff863b589")
MINIROOTFS = ("alpine-minirootfs-3.24.2-aarch64.tar.gz",
              "9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773")


def fetched(name, sha):
    """The file, downloaded once and held to its sum every time."""
    path = os.path.join(DOWNLOADS, name)

    if not os.path.exists(path):
        os.makedirs(DOWNLOADS, exist_ok=True)
        print("alpine: fetching %s" % name, flush=True)
        urllib.request.urlretrieve(MIRROR + name, path + ".part")
        os.replace(path + ".part", path)

    h = hashlib.sha256()

    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)

    if h.hexdigest() != sha:
        raise SystemExit("alpine: %s is not the file Alpine published (sha256 %s)"
                         % (path, h.hexdigest()))

    return path


def kernel():
    """The arm64 Image inside the netboot tarball's zboot `vmlinuz-virt`."""
    with tarfile.open(fetched(*NETBOOT)) as t:
        zboot = t.extractfile("boot/vmlinuz-virt").read()

    if zboot[0:2] != b"MZ" or zboot[4:8] != b"zimg" or zboot[24:28] != b"gzip":
        raise SystemExit("alpine: vmlinuz-virt is not a gzip zboot image")

    offset, size = struct.unpack_from("<II", zboot, 8)
    image = zlib.decompress(zboot[offset:offset + size], 16 + zlib.MAX_WBITS)

    if image[56:60] != b"ARM\x64":
        raise SystemExit("alpine: what vmlinuz-virt holds is not an arm64 Image")

    return image


# **The first thing Alpine runs**, as PID 1. It says what it is, which is
# what a sandbox's suite looks for, and switches the machine off - busybox's
# `poweroff -f` is PSCI's SYSTEM_OFF, which the monitor answers by stopping.
# `INIT_SHELL` is the other one: the same, then a shell on the console.
INIT = b"""#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
hostname alpine
echo "alpine: Alpine Linux $(cat /etc/alpine-release), $(uname -sr), in a Kosmos sandbox"
echo "alpine: $(grep -c ^processor /proc/cpuinfo) processor, $(awk '/MemTotal/ {print $2}' /proc/meminfo) kB"
if [ -e /sandbox-shell ]; then exec setsid sh -c 'exec sh -l <>/dev/ttyAMA0 >&0 2>&0'; fi
echo "alpine: switching off"
exec poweroff -f
"""


def newc(name, mode, data=b"", ino=0, nlink=1, mtime=0, rdev=(0, 0)):
    """One entry of a `newc` cpio archive - the format an initramfs is
    (`Documentation/driver-api/early-userspace/buffer-format.rst`): a header
    of hexadecimal fields, the name, the data, each padded to four bytes."""
    name_b = name.encode() + b"\0"
    head = b"070701" + b"".join(b"%08X" % v for v in (
        ino, mode, 0, 0, nlink, mtime, len(data), 0, 0, rdev[0], rdev[1], len(name_b), 0))
    out = head + name_b
    out += b"\0" * (-len(out) % 4)
    out += data
    out += b"\0" * (-len(out) % 4)
    return out


def initramfs(shell=False):
    """Alpine's minirootfs, as an uncompressed newc archive with an `/init`.

    Hard links are written as copies of what they link to, and anything
    that is neither a file, a folder nor a symbolic link is left out - the
    minirootfs has none, and `/dev` is devtmpfs's."""
    parts, ino, files = [], 1, {}

    with tarfile.open(fetched(*MINIROOTFS)) as t:
        for m in t.getmembers():
            name = m.name.lstrip("./")

            if not name or name == "init":
                continue

            perm = m.mode & 0o7777

            if m.isdir():
                parts.append(newc(name, 0o040000 | perm, ino=ino, nlink=2, mtime=int(m.mtime)))
            elif m.isreg():
                data = t.extractfile(m).read()
                files[name] = (data, perm)
                parts.append(newc(name, 0o100000 | perm, data, ino=ino, mtime=int(m.mtime)))
            elif m.issym():
                parts.append(newc(name, 0o120000 | 0o777, m.linkname.encode(), ino=ino,
                                  mtime=int(m.mtime)))
            elif m.islnk() and m.linkname.lstrip("./") in files:
                data, p = files[m.linkname.lstrip("./")]
                parts.append(newc(name, 0o100000 | p, data, ino=ino, mtime=int(m.mtime)))
            else:
                continue

            ino += 1

    parts.append(newc("init", 0o100755, INIT, ino=ino))

    # **`/dev/console`, which the archive must carry itself**: the kernel
    # opens it for PID 1 before anything has mounted devtmpfs, and without
    # it `/init` starts with no output at all - "unable to open an initial
    # console", then silence. Character device 5, 1.
    parts.append(newc("dev/console", 0o020600, ino=ino + 2, rdev=(5, 1)))

    if shell:
        parts.append(newc("sandbox-shell", 0o100644, b"", ino=ino + 1))

    parts.append(newc("TRAILER!!!", 0))
    return b"".join(parts)


def main():
    named = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = named[0] if named else os.path.join(ROOT, "build", "sandboxes", "alpine")
    os.makedirs(out, exist_ok=True)
    image = kernel()

    with open(os.path.join(out, "Image"), "wb") as f:
        f.write(image)

    archive = initramfs(shell="--shell" in sys.argv)

    with open(os.path.join(out, "initramfs"), "wb") as f:
        f.write(archive)

    print("alpine: Image, %d bytes, and initramfs, %d bytes - Alpine 3.24.2, in %s"
          % (len(image), len(archive), out))


if __name__ == "__main__":
    main()
