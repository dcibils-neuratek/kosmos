#!/bin/bash
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# Run a released Kosmos on a Mac, with nothing installed but QEMU.
#
#   bash run-kosmos.sh
#
# It fetches the kernel and the disk beside it into this directory the
# first time, then boots them. Run it again and it reuses what it has.
#
# **The kernel is the whole system**: the desktop, the window manager,
# every application, the browser, Doom's and Quake's engines and the Super
# Nintendo's are all linked into that one file. The disk is only `/home` -
# here, the solar system's baked textures, so the planets have surfaces.
# Without it Kosmos still boots and the planets render flat-shaded.
#
# What lands in this directory: `kosmos.elf`, `kosmos-home.img`, and
# whatever Kosmos writes to that disk afterwards. Nothing else, and nothing
# outside it.

set -euo pipefail

VERSION="${KOSMOS_VERSION:-v0.10.95}"
BASE="https://github.com/dcibils-neuratek/kosmos/releases/download/${VERSION}"

KERNEL="kosmos.elf"
DISK="kosmos-home.img"

#
# Where this runs from, so a double-click from Finder does not scatter a
# kernel and a disk image across the home directory.
#
cd "$(dirname "$0")"

#
# QEMU, which is the one thing this cannot supply.
#
if ! command -v qemu-system-aarch64 >/dev/null 2>&1; then
    echo "qemu-system-aarch64 is not installed."
    echo
    echo "  brew install qemu"
    echo
    echo "Then run this again."
    exit 1
fi

fetch() {
    local name="$1"

    if [ -f "$name" ]; then
        echo "$name: already here"
        return
    fi

    echo "$name: fetching from ${VERSION}..."
    curl -fL --progress-bar -o "$name.part" "${BASE}/${name}"
    mv "$name.part" "$name"
}

fetch "$KERNEL"
fetch "$DISK"

echo
echo "Kosmos ${VERSION}, on QEMU's aarch64 'virt' board."
echo
echo "  The window is the machine. Close it, or press Control-C here, to stop."
echo "  The Deskbar is top-left; Demos holds the solar system."
echo "  'wm solar' at a Terminal opens it directly."
echo

#
# **Emulated, not virtualised, and that is deliberate.**
#
# QEMU can run an aarch64 guest on an Apple Silicon Mac's own cores with
# `-accel hvf`, and it is several times quicker. It also does not boot
# Kosmos on the Mac this was built on - no kernel does, including released
# binaries that predate any of this work, so it is QEMU's bug rather than
# the kernel's. TCG is the path that is known to work, so TCG is what this
# uses; `KOSMOS_ACCEL=hvf` is here for whoever wants to find out whether a
# newer QEMU fixed it.
#
# `-cpu cortex-a72` goes with TCG for the same reason `make qemu` uses it:
# HVF cannot pretend to be a core the host is not, so the two choices are
# a pair rather than two flags.
#
ACCEL_FLAGS=(-cpu cortex-a72)

if [ "${KOSMOS_ACCEL:-tcg}" = "hvf" ]; then
    echo "  (trying -accel hvf; if the screen stays black, unset KOSMOS_ACCEL)"
    echo
    ACCEL_FLAGS=(-accel hvf -cpu host)
fi

#
# The same machine `make qemu` builds, minus the host-specific parts:
# four processors, a linear framebuffer the firmware hands over, a
# keyboard and a tablet, sound, the network, and the disk.
#
exec qemu-system-aarch64 \
    -M virt,gic-version=3 \
    "${ACCEL_FLAGS[@]}" \
    -m 512M \
    -smp 4 \
    -global virtio-mmio.force-legacy=false \
    -device ramfb \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -audiodev coreaudio,id=snd0 \
    -device virtio-sound-device,audiodev=snd0 \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -drive "file=${DISK},format=raw,if=none,id=disk" \
    -device virtio-blk-device,drive=disk \
    -display cocoa \
    -serial mon:stdio \
    -kernel "$KERNEL"
