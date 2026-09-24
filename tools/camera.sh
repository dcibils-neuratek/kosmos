#!/bin/sh
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  Kosmos on x86-64 under QEMU, in a window, with this Mac's USB camera
#  handed to it and the Camera app open on the desktop (`roadmap.md` 6d,
#  `usb.md` §11).
#
#      sudo sh tools/camera.sh                  the C920, 046d:08e5
#      sudo sh tools/camera.sh 046d:0825        another camera
#
#  **Why `sudo`**: macOS keeps a camera's video interfaces for its own
#  driver, and libusb can take them only as root - `tools/usbhost.sh` says
#  the rest. Close anything on the Mac that is using the camera first.
#
#  The comma in `deskbar,,camera` is doubled because QEMU splits an
#  option's value at a single one.
#
#  The kernel is build/x86_64/kosmos.bin as it stands - `make x86-build`
#  first. Everything the machine says is kept in build/camera.log, which
#  is made readable when QEMU ends. Close the window, or Control-C here, to
#  end it; macOS takes the camera back.

set -eu

DEVICE=${1:-046d:08e5}
HERE=$(cd "$(dirname "$0")/.." && pwd)
LOG="$HERE/build/camera.log"
VENDOR=${DEVICE%%:*}
PRODUCT=${DEVICE##*:}

trap 'chmod a+r "$LOG" 2>/dev/null || true' EXIT

qemu-system-x86_64 -M q35,vmport=off -m 1G -smp 4 -no-reboot \
    -vga none -device ramfb -display cocoa \
    -device virtio-tablet-pci \
    -serial "file:$LOG" \
    -device qemu-xhci,id=xhci \
    -device "usb-host,bus=xhci.0,vendorid=0x$VENDOR,productid=0x$PRODUCT" \
    -fw_cfg "name=opt/kosmos/boot,string=wm deskbar,,camera" \
    -kernel "$HERE/build/x86_64/kosmos.bin"

echo "Done. What the camera's driver and the app said:"
grep -a "camera" "$LOG" | head -40 || echo "(nothing about a camera)"
