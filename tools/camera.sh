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
#
#  **And what QEMU says about the camera**, in build/camera-qemu.log: its
#  USB host's trace events - the device opened and claimed, the setting
#  chosen, the isochronous stream started and stopped or short of buffers -
#  and the xHCI's transfer errors. The first run with root, on 24 September,
#  streamed and made no frames, and Kosmos's side of it could not say
#  whether the camera sent nothing or QEMU passed nothing on. Per-transfer
#  events are left out: thirty thousand lines a second tell nobody anything.
#
#  **`isobufs=8`**: QEMU's USB host keeps four transfers of 32 microframes
#  of the camera, 16 ms, and drops what the guest has not taken by then.
#  Eight is 32 ms, room for an emulated machine's slower moments.
#
#  **And a disk, kept between runs**, build/camera-home.img, so a recording
#  - Record, or R - is kept in /home/videos (`roadmap.md` 6d 8f). A blank
#  one is formatted by the machine itself. When QEMU ends, every recording on
#  it is copied out to build/camera-videos/, which QuickTime opens.

set -eu

DEVICE=${1:-046d:08e5}
HERE=$(cd "$(dirname "$0")/.." && pwd)
LOG="$HERE/build/camera.log"
QLOG="$HERE/build/camera-qemu.log"
DISK="$HERE/build/camera-home.img"
OUT="$HERE/build/camera-videos"
VENDOR=${DEVICE%%:*}
PRODUCT=${DEVICE##*:}

trap 'chmod a+rw "$LOG" "$QLOG" "$DISK" 2>/dev/null || true' EXIT

if [ ! -f "$DISK" ]; then
    dd if=/dev/zero of="$DISK" bs=1m count=256 2>/dev/null
fi

qemu-system-x86_64 -M q35,vmport=off -m 1G -smp 4 -no-reboot \
    -vga none -device ramfb -display cocoa \
    -device virtio-tablet-pci \
    -serial "file:$LOG" \
    -device qemu-xhci,id=xhci \
    -device "usb-host,bus=xhci.0,vendorid=0x$VENDOR,productid=0x$PRODUCT,isobufs=8" \
    -fw_cfg "name=opt/kosmos/boot,string=wm deskbar,,camera" \
    -drive "file=$DISK,format=raw,if=none,id=disk" \
    -device virtio-blk-pci,drive=disk \
    -D "$QLOG" \
    -trace 'usb_host_open*' -trace 'usb_host_*_interface' \
    -trace 'usb_host_iso*' -trace 'usb_host_*kernel' \
    -trace usb_host_parse_endpoint -trace usb_host_parse_error \
    -trace usb_xhci_xfer_error -trace usb_xhci_ep_enable \
    -kernel "$HERE/build/x86_64/kosmos.bin"

echo "Done. What the camera's driver and the app said:"
grep -a "camera" "$LOG" | head -40 || echo "(nothing about a camera)"
echo
echo "What QEMU said about the camera ($QLOG):"
head -30 "$QLOG" 2>/dev/null || echo "(nothing)"

# The recordings, out of the disk and onto the Mac.
if [ -x "$HERE/build/host/lua" ]; then
    mkdir -p "$OUT"
    ( cd "$HERE" && build/host/lua tools/kfs.lua ls "$DISK" /home/videos 2>/dev/null ) \
        | sed -n 's/^  \(.*\.mp4\)  *[0-9][0-9]*$/\1/p' \
        | while IFS= read -r name; do
            ( cd "$HERE" && build/host/lua tools/kfs.lua get "$DISK" \
                  "/home/videos/$name" "$OUT/$name" ) >/dev/null 2>&1 \
                && echo "Recorded: $OUT/$name"
          done
    chmod -R a+rw "$OUT" 2>/dev/null || true
fi
