#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Alpine Linux for a sandbox: its kernel, unwrapped, from a pinned release.

    python3 tools/alpine.py build/sandboxes/alpine                  aarch64
    python3 tools/alpine.py --arch x86_64 build/sandboxes/alpine-x86_64

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

**And for x86-64** (`docs/sandboxes.md` X2, X3; 13 October), the same
release for a PC: its `vmlinuz-virt` is a bzImage, which a sandbox's
monitor boots as it is (`x86dev.c`), so nothing is unwrapped; `/init` is the
same but for its console, the 16550's `ttyS0`; and `virtio_mmio` is built
into that kernel rather than a module. The sums are Alpine's, from its
`latest-releases.yaml` for x86_64, read 13 October.

Fetched into `build/downloads/alpine` once and kept; written to the folder
given as `Image` and `initramfs` - the minirootfs as a newc archive with an
`/init` of ours - which is an image folder as `/Home/Sandboxes/Images/<name>`
holds one - `/Home/Sandboxes/Images/alpine` once installed as a package.
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
RELEASES = {
    "aarch64": {
        "netboot": ("alpine-netboot-3.24.2-aarch64.tar.gz",
                    "124087f5df9ac698bb4e942d603eaae96721e451cdc4706c71a5e42ff863b589"),
        "minirootfs": ("alpine-minirootfs-3.24.2-aarch64.tar.gz",
                       "9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773"),
        "console": "ttyAMA0",
    },
    "x86_64": {
        "netboot": ("alpine-netboot-3.24.2-x86_64.tar.gz",
                    "bbdd643360fac384a7933d8bebc7132eb720002e58d1321edcde4a5a3c3b83c6"),
        "minirootfs": ("alpine-minirootfs-3.24.2-x86_64.tar.gz",
                       "c5ca053cfe1d85c5b96dff8b9bc57045f7f184a30ffb6b65776409ca90388677"),
        "console": "ttyS0",
    },
}

ARCH = "aarch64"


def mirror():
    return "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/%s/" % ARCH


def release(what):
    return RELEASES[ARCH][what]


def fetched(name, sha):
    """The file, downloaded once and held to its sum every time."""
    path = os.path.join(DOWNLOADS, name)

    if not os.path.exists(path):
        os.makedirs(DOWNLOADS, exist_ok=True)
        print("alpine: fetching %s" % name, flush=True)
        urllib.request.urlretrieve(mirror() + name, path + ".part")
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
    """The arm64 Image inside the netboot tarball's zboot `vmlinuz-virt` -
    or, for x86-64, the bzImage itself."""
    with tarfile.open(fetched(*release("netboot"))) as t:
        zboot = t.extractfile("boot/vmlinuz-virt").read()

    if ARCH == "x86_64":
        if zboot[0x202:0x206] != b"HdrS" or not (zboot[0x236] & 1):
            raise SystemExit("alpine: vmlinuz-virt is not a bzImage with a 64-bit entry")

        return zboot

    if zboot[0:2] != b"MZ" or zboot[4:8] != b"zimg" or zboot[24:28] != b"gzip":
        raise SystemExit("alpine: vmlinuz-virt is not a gzip zboot image")

    offset, size = struct.unpack_from("<II", zboot, 8)
    image = zlib.decompress(zboot[offset:offset + size], 16 + zlib.MAX_WBITS)

    if image[56:60] != b"ARM\x64":
        raise SystemExit("alpine: what vmlinuz-virt holds is not an arm64 Image")

    return image


# **The first thing Alpine runs**, as PID 1 (`docs/sandboxes.md` S1). It
# mounts what a system needs, names the machine after the sandbox
# (`kosmos.name=` on its command line) and says what it is. Then, given a
# command (`kosmos.run=`, as hexadecimal so its spaces and quotes are not the
# command line's), it runs it, says how it ended on a line beginning with
# `kosmos.end=`'s token - which the monitor reads and keeps off the console -
# and switches the machine off: busybox's `poweroff -f` is PSCI's SYSTEM_OFF.
# Given none, it is a shell on the serial port, with job control, and the
# machine switches itself off when the shell ends - PID 1 ending would be a
# kernel panic.
INIT = b"""#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
arg() { sed -n "s/.*$1=\\([^ ]*\\).*/\\1/p" /proc/cmdline; }
name=$(arg kosmos.name)
hostname "${name:-alpine}"
unhex() { echo "$1" | awk '{ h = "0123456789abcdef"
    for (i = 1; i < length($0); i += 2)
        printf "%c", (index(h, substr($0, i, 1)) - 1) * 16 + index(h, substr($0, i + 1, 1)) - 1 }'; }
# The transport its folders and its network both stand on.
if grep -q "kosmos.share\\|kosmos.net\\|kosmos.disk\\|kosmos.screen" /proc/cmdline && [ -f /kosmos/modules/virtio_mmio.ko ]; then
    insmod /kosmos/modules/virtio_mmio.ko
fi
# Its network, when it was given one: virtio-net, and the monitor's gateway
# at 10.0.3.2 with its name server at 10.0.3.3.
if grep -q kosmos.net /proc/cmdline; then
    insmod /kosmos/modules/failover.ko && insmod /kosmos/modules/net_failover.ko \
        && insmod /kosmos/modules/virtio_net.ko
    ip link set lo up
    ip link set eth0 up && ip addr add 10.0.3.15/24 dev eth0 && ip route add default via 10.0.3.2
    echo "nameserver 10.0.3.3" > /etc/resolv.conf
fi
# Its disk, when it was given one: virtio-blk, /dev/vda (`docs/linux.md` L1).
if grep -q kosmos.disk /proc/cmdline && [ -f /kosmos/modules/virtio_blk.ko ]; then
    insmod /kosmos/modules/virtio_blk.ko
fi
# Its screen, when it was given one: virtio-gpu and the DRM it stands on,
# in `modules.dep`'s order, and a framebuffer at /dev/fb0 (`docs/linux.md` L2).
if grep -q kosmos.screen /proc/cmdline; then
    for m in i2c-core backlight fb syscopyarea sysfillrect sysimgblt fb_sys_fops \
             drm_panel_orientation_quirks drm drm_kms_helper drm_client_lib \
             virtio_dma_buf drm_shmem_helper virtio-gpu virtio_input; do
        [ -f /kosmos/modules/$m.ko ] && insmod /kosmos/modules/$m.ko
    done
fi
# Its folders: share<i>, each a virtio-fs device the monitor serves, at the
# path and in the mode its command line says.
if grep -q kosmos.share /proc/cmdline; then
    insmod /kosmos/modules/fuse.ko && insmod /kosmos/modules/virtiofs.ko
    for i in 0 1 2 3; do
        share=$(arg kosmos.share$i)
        [ -n "$share" ] || continue
        at=$(unhex "${share%%:*}")
        mkdir -p "$at"
        mount -t virtiofs "share$i" "$at" -o "${share##*:}" || echo "alpine: share$i would not mount at $at"
    done
fi
# **Linux on Kosmos** (`docs/linux.md` L4). Every module its kernel has,
# from the image's modloop, read through a share - ext4 and evdev among them.
modloop=$(arg kosmos.modloop)
if [ -n "$modloop" ]; then
    insmod /kosmos/modules/squashfs.ko; insmod /kosmos/modules/loop.ko
    mkdir -p /.modloop /lib/modules
    mount -o loop,ro "$(unhex "$modloop")" /.modloop && mount --bind /.modloop/modules /lib/modules \
        || echo "alpine: the modloop would not mount"
fi
# Its disk set up, once: made ext4, Alpine and what it runs installed onto
# it from the repository named, through the sandbox's network.
setup=$(arg kosmos.setup)
if [ -n "$setup" ]; then
    repo=$(unhex "$setup"); wanted=$(unhex "$(arg kosmos.packages)")
    say() { echo "kosmos-setup: $*"; }
    say "preparing the disk"
    modprobe ext4 && apk add -q --no-cache -X "$repo/main" -X "$repo/community" e2fsprogs \
        && mkfs.ext4 -F -q /dev/vda && mkdir -p /mnt && mount -t ext4 /dev/vda /mnt \
        || { say "failed: the disk could not be made"; poweroff -f; }
    mkdir -p /mnt/etc/apk && cp -r /etc/apk/keys /mnt/etc/apk/
    printf '%s/main\\n%s/community\\n' "$repo" "$repo" > /mnt/etc/apk/repositories
    say "installing $wanted"
    apk add --root /mnt --initdb --no-cache --progress-fd 3 $wanted 3>&1 >/dev/null 2>&1 \
        | awk -F/ '{ p = int($1 * 100 / ($2 > 0 ? $2 : 1)); if (p >= shown + 5) { shown = p; print "kosmos-setup: " p "%"; fflush() } }'
    [ -x /mnt/sbin/apk ] || { say "failed: Alpine did not install"; poweroff -f; }
    sync; umount /mnt
    say "done"
    poweroff -f
fi
# Its disk as its root: mounted, the modules carried in, and the session
# started there - seatd, udev, and `cage` showing one application.
if [ "$(arg kosmos.root)" = 1 ]; then
    modprobe ext4; modprobe evdev
    mkdir -p /newroot && mount -t ext4 /dev/vda /newroot || { echo "alpine: no root on the disk"; poweroff -f; }
    mkdir -p /newroot/lib/modules /newroot/kosmos && mount --bind /.modloop/modules /newroot/lib/modules
    # Its folders of /Home, carried into its root where they were - Kosmos's
    # Downloads as its own, among them.
    for i in 0 1 2 3; do
        share=$(arg kosmos.share$i)
        [ -n "$share" ] || continue
        at=$(unhex "${share%%:*}")
        [ "$at" = /kosmos/image ] && continue
        mkdir -p "/newroot$at" && mount --move "$at" "/newroot$at"
    done
    app=$(unhex "$(arg kosmos.app)")
    cat > /newroot/kosmos/session <<SESSION
#!/bin/sh
mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts /run /tmp && mount -t devpts devpts /dev/pts
hostname "${name:-linux}"
ip link set lo up
if [ -e /sys/class/net/eth0 ]; then
    ip link set eth0 up && ip addr add 10.0.3.15/24 dev eth0 && ip route add default via 10.0.3.2
    echo "nameserver 10.0.3.3" > /etc/resolv.conf
fi
# Its size following the window's (docs/linux.md L5): the display changed is
# a new preferred mode on its connector, and cage keeps the mode it began
# with - so a loop beside it, with cage's own environment, sets each new
# preferred mode through wlr-randr. A udev rule ran it in an environment of
# udev's, where wlr-randr said nothing and did nothing.
cat > /kosmos/size <<'SIZE'
#!/bin/sh
last=
while sleep 0.5; do
    for c in /sys/class/drm/card0-*; do
        [ "\\$(cat \\$c/status 2>/dev/null)" = connected ] || continue
        mode=\\$(head -n 1 \\$c/modes 2>/dev/null)
        [ -n "\\$mode" ] && [ "\\$mode" != "\\$last" ] || continue
        if [ -n "\\$last" ]; then
            echo "kosmos-session: size \\$mode"
            wlr-randr --output \\${c##*/card0-} --custom-mode \\$mode 2>&1
        fi
        last=\\$mode
    done
done
SIZE
chmod 755 /kosmos/size
udevd --daemon && udevadm trigger && udevadm settle
seatd -g video >/dev/null 2>&1 &
export XDG_RUNTIME_DIR=/run/xdg HOME=/root WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
mkdir -p \\$XDG_RUNTIME_DIR && chmod 700 \\$XDG_RUNTIME_DIR
# The browser is Kosmos's only one here: not asked to be the default.
if command -v glib-compile-schemas >/dev/null && [ -d /usr/share/glib-2.0/schemas ]; then
    printf '[org.gnome.Epiphany]\\nask-for-default=false\\n' > /usr/share/glib-2.0/schemas/90-kosmos.gschema.override
    glib-compile-schemas /usr/share/glib-2.0/schemas
fi
sleep 1
echo "kosmos-session: cage $app" > /dev/@CONSOLE@
/kosmos/size > /dev/@CONSOLE@ 2>&1 &
cage -- $app > /dev/@CONSOLE@ 2>&1
echo "kosmos-session: ended" > /dev/@CONSOLE@
poweroff -f
SESSION
    chmod 755 /newroot/kosmos/session
    exec switch_root /newroot /kosmos/session
fi
run=$(arg kosmos.run)
if [ -z "$run" ]; then
    echo "alpine: Alpine Linux $(cat /etc/alpine-release), $(uname -sr), in a Kosmos sandbox"
    echo "alpine: $(grep -c ^processor /proc/cpuinfo) processor, $(awk '/MemTotal/ {print $2}' /proc/meminfo) kB"
    sh -c 'exec setsid -c sh -l <>/dev/@CONSOLE@ >&0 2>&0'
    exec poweroff -f
fi
command=$(unhex "$run")
cd /root
sh -c "$command" <>/dev/@CONSOLE@ >&0 2>&0
code=$?
echo "$(arg kosmos.end) $code"
exec poweroff -f
"""


# **The modules a sandbox's devices need**, from Alpine's own initramfs for
# its `virt` kernel - where `virtio_mmio`, `fuse`, `virtiofs` and
# `virtio_net` are modules rather than built in - and `virtio_blk`, a
# sandbox's disk (`docs/linux.md` L1). Copied whole, and loaded by
# `/init` in their order (`modules.dep`: virtiofs needs fuse, virtio_net
# needs net_failover, which needs failover).
MODULES = ("kernel/drivers/virtio/virtio_mmio.ko", "kernel/fs/fuse/fuse.ko",
           "kernel/fs/fuse/virtiofs.ko", "kernel/net/core/failover.ko",
           "kernel/drivers/net/net_failover.ko", "kernel/drivers/net/virtio_net.ko",
           "kernel/drivers/block/virtio_blk.ko",
           # A screen (`docs/linux.md` L2): virtio-gpu and what it needs.
           "kernel/drivers/i2c/i2c-core.ko", "kernel/drivers/video/backlight/backlight.ko",
           "kernel/drivers/video/fbdev/core/fb.ko",
           "kernel/drivers/video/fbdev/core/syscopyarea.ko",
           "kernel/drivers/video/fbdev/core/sysfillrect.ko",
           "kernel/drivers/video/fbdev/core/sysimgblt.ko",
           "kernel/drivers/video/fbdev/core/fb_sys_fops.ko",
           "kernel/drivers/gpu/drm/drm_panel_orientation_quirks.ko",
           "kernel/drivers/gpu/drm/drm.ko", "kernel/drivers/gpu/drm/drm_kms_helper.ko",
           "kernel/drivers/gpu/drm/clients/drm_client_lib.ko",
           "kernel/drivers/virtio/virtio_dma_buf.ko",
           "kernel/drivers/gpu/drm/drm_shmem_helper.ko",
           "kernel/drivers/gpu/drm/virtio/virtio-gpu.ko",
           # Its keyboard and pointer (L3).
           "kernel/drivers/virtio/virtio_input.ko",
           # And its modloop, every other module (L4).
           "kernel/fs/squashfs/squashfs.ko", "kernel/drivers/block/loop.ko")
# Built in on one architecture and a module on another: taken when there.
OPTIONAL = ("backlight.ko",)
KERNEL_VERSION = "6.18.52-0-virt"


def modules():
    """{name: bytes} of `MODULES`, out of the netboot's `initramfs-virt` -
    a gzip of a newc archive."""
    import gzip

    with tarfile.open(fetched(*release("netboot"))) as t:
        archive = gzip.decompress(t.extractfile("boot/initramfs-virt").read())

    found, i = {}, 0
    wanted = {"usr/lib/modules/%s/%s" % (KERNEL_VERSION, m): m.rsplit("/", 1)[1] for m in MODULES
              if not (ARCH == "x86_64" and m.endswith("virtio_mmio.ko"))}

    while archive[i:i + 6] == b"070701":
        namesize = int(archive[i + 94:i + 102], 16)
        filesize = int(archive[i + 54:i + 62], 16)
        name = archive[i + 110:i + 110 + namesize - 1].decode()
        j = i + 110 + namesize
        j += (-j) % 4

        if name in wanted:
            found[wanted[name]] = archive[j:j + filesize]

        if name == "TRAILER!!!":
            break

        i = j + filesize
        i += (-i) % 4

    missing = [m for m in wanted.values() if m not in found and m not in OPTIONAL]

    if missing:
        raise SystemExit("alpine: initramfs-virt has no %s" % ", ".join(missing))

    return found


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


def initramfs():
    """Alpine's minirootfs, as an uncompressed newc archive with an `/init`.

    Hard links are written as copies of what they link to, and anything
    that is neither a file, a folder nor a symbolic link is left out - the
    minirootfs has none, and `/dev` is devtmpfs's."""
    parts, ino, files = [], 1, {}

    with tarfile.open(fetched(*release("minirootfs"))) as t:
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

    parts.append(newc("init", 0o100755, INIT.replace(b"@CONSOLE@", release("console").encode()),
                      ino=ino))

    # In a folder of their own: Alpine's `/lib` is a link to `usr/lib`,
    # which the kernel's unpacker does not follow - archived under it, they
    # were not there.
    parts.append(newc("kosmos", 0o040755, ino=ino + 8, nlink=2))
    parts.append(newc("kosmos/modules", 0o040755, ino=ino + 9, nlink=2))

    for k, (module, data) in enumerate(sorted(modules().items())):
        parts.append(newc("kosmos/modules/" + module, 0o100644, data, ino=ino + 10 + k))

    # **`/dev/console`, which the archive must carry itself**: the kernel
    # opens it for PID 1 before anything has mounted devtmpfs, and without
    # it `/init` starts with no output at all - "unable to open an initial
    # console", then silence. Character device 5, 1.
    parts.append(newc("dev/console", 0o020600, ino=ino + 2, rdev=(5, 1)))

    parts.append(newc("TRAILER!!!", 0))
    return b"".join(parts)


def main():
    global ARCH

    args = sys.argv[1:]

    if "--arch" in args:
        at = args.index("--arch")
        ARCH = args[at + 1] if at + 1 < len(args) else ""
        del args[at:at + 2]

    if ARCH not in RELEASES:
        raise SystemExit("alpine: --arch is aarch64 or x86_64")

    named = [a for a in args if not a.startswith("--")]
    out = named[0] if named else os.path.join(ROOT, "build", "sandboxes", "alpine")
    os.makedirs(out, exist_ok=True)
    image = kernel()

    with open(os.path.join(out, "Image"), "wb") as f:
        f.write(image)

    archive = initramfs()

    with open(os.path.join(out, "initramfs"), "wb") as f:
        f.write(archive)

    # Every module its kernel has (`docs/linux.md` L4), for a sandbox that
    # mounts it - Linux on Kosmos, whose disk wants ext4 and its input evdev.
    with tarfile.open(fetched(*release("netboot"))) as t:
        modloop = t.extractfile("boot/modloop-virt").read()

    with open(os.path.join(out, "modloop"), "wb") as f:
        f.write(modloop)

    print("alpine: Image, %d bytes, initramfs, %d bytes, and modloop, %d bytes - "
          "Alpine 3.24.2 for %s, in %s"
          % (len(image), len(archive), len(modloop), ARCH, out))


if __name__ == "__main__":
    main()
