#!/bin/sh
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  Kosmos on x86-64 under QEMU with one of this Mac's USB devices handed to
#  it, and everything it says kept in build/usbhost.log.
#
#      sudo sh tools/usbhost.sh 045e:028e 60      the SN30 Pro pad
#      sudo sh tools/usbhost.sh 046d:08e5 30      the C920 camera
#
#  **Why this exists**: QEMU has no game controller of its own, so the only
#  way to try the USB driver on a real one without the ThinkPad is to give it
#  one from here - QEMU's `usb-host`, through libusb.
#
#  **Why `sudo`**: macOS has a driver for an Xbox 360 pad of its own
#  (`com.apple.gamecontroller.driver.XboxGamepad`, found on 19 September),
#  and it holds the pad's gamepad interface. libusb can take a device from a
#  macOS driver only as root; as anyone else QEMU cannot, the pad refuses
#  SET_CONFIGURATION, and Kosmos says so. macOS takes the pad back when QEMU
#  ends.
#
#  **A camera needs it for the same reason** (`usb.md` §11): macOS keeps the
#  C920's video interfaces for its own driver. Without root the driver names
#  the camera and is refused at SET_CONFIGURATION; with it, the camera
#  streams - `opt/kosmos/camera=count` asks it to at once, with no window to
#  open it - and the driver says how many frames arrive every five seconds.
#  `tools/camera.sh` is the same camera in the Camera app, in a window.
#
#  The kernel is build/x86_64/kosmos.bin as it stands - `make x86-build`
#  first. It boots to a prompt and runs for the seconds given; use the
#  device meanwhile - a pad's buttons, say - and the driver says what it
#  sees.

set -eu

DEVICE=${1:?"which device: vendor:product, as 045e:028e"}
SECONDS_TO_RUN=${2:-60}
HERE=$(cd "$(dirname "$0")/.." && pwd)
LOG="$HERE/build/usbhost.log"
VENDOR=${DEVICE%%:*}
PRODUCT=${DEVICE##*:}

qemu-system-x86_64 -M q35,vmport=off -m 512M -no-reboot \
    -display none -vga none -device ramfb -serial "file:$LOG" \
    -device qemu-xhci,id=xhci \
    -device "usb-host,bus=xhci.0,vendorid=0x$VENDOR,productid=0x$PRODUCT" \
    -fw_cfg name=opt/kosmos/camera,string=count \
    -kernel "$HERE/build/x86_64/kosmos.bin" < /dev/null &
QEMU=$!

echo "Kosmos is running with $DEVICE for $SECONDS_TO_RUN seconds."
echo "Use it now - a pad's buttons, say; a camera needs nothing."
sleep "$SECONDS_TO_RUN"
kill "$QEMU" 2>/dev/null || true
wait "$QEMU" 2>/dev/null || true
chmod a+r "$LOG"

echo "Done. What the USB driver said:"
grep "xhci" "$LOG" || echo "(nothing from the USB driver)"
