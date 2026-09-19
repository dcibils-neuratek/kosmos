#!/bin/sh
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  Kosmos on x86-64 under QEMU with one of this Mac's USB devices handed to
#  it, and everything it says kept in build/usbhost.log.
#
#      sudo sh tools/usbhost.sh 045e:028e 60
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
#  The kernel is build/x86_64/kosmos.bin as it stands - `make x86-build`
#  first. It boots to a prompt and runs for the seconds given; press the
#  pad's buttons meanwhile, and the driver says each one.

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
    -kernel "$HERE/build/x86_64/kosmos.bin" < /dev/null &
QEMU=$!

echo "Kosmos is running with $DEVICE for $SECONDS_TO_RUN seconds."
echo "Press the buttons now: each face button, the D-pad, the sticks, the triggers."
sleep "$SECONDS_TO_RUN"
kill "$QEMU" 2>/dev/null || true
wait "$QEMU" 2>/dev/null || true
chmod a+r "$LOG"

echo "Done. What the USB driver said:"
grep "xhci" "$LOG" || echo "(nothing from the USB driver)"
