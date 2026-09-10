#!/bin/bash
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  Writes the bootable Kosmos image to a USB stick.
#
#  **This erases the drive it is given, and that is the whole reason this
#  file exists rather than a line of `dd` in a document.** A `dd` command
#  copied out of a README is one keystroke from the disk the machine boots
#  from, and it gives no warning at all: it succeeds, and the mistake is
#  discovered at the next restart. So nothing here is positional, nothing is
#  guessed, and the only drives offered are ones this computer says are
#  external and physical.
#
#  Three refusals, in order of how much they matter:
#
#    - internal drives are never listed and are rejected again by name if
#      one is typed anyway, because the list and the answer are two separate
#      moments and the drive set can change between them;
#    - the drive that holds the running system is rejected by a third route,
#      in case a machine ever reports its own boot disk as external;
#    - and the confirmation is the drive's *name*, typed out. A `y/n` prompt
#      is answered by reflex; a name has to be read first.
#
#  macOS only. It is what this project is developed on, and `diskutil` is
#  what makes the checks above possible - Linux wants `lsblk` and different
#  answers, and half a port would be more dangerous than none.

set -euo pipefail

IMG="${1:-build/x86_64/kosmos-usb.img}"

die() { printf '%s\n' "$*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "mkusb: macOS only - see the comment at the top of this file."

[ -f "$IMG" ] || die "mkusb: no $IMG. Run \`make x86-usb-image\` first."

command -v diskutil >/dev/null 2>&1 || die "mkusb: no diskutil."

#
#  What this computer says is external and physical, with the size and name
#  a person would recognise the drive by.
#
list_drives() {
    diskutil list -plist external physical 2>/dev/null | python3 -c '
import plistlib, subprocess, sys

top = plistlib.loads(sys.stdin.buffer.read())

for name in top.get("WholeDisks", []):
    raw = subprocess.run(["diskutil", "info", "-plist", name],
                         capture_output=True).stdout
    info = plistlib.loads(raw)

    # Belt and braces: `diskutil list external` should never return an
    # internal disk, and this asks each one again anyway.
    if info.get("Internal", True):
        continue

    size = info.get("TotalSize", 0)
    label = info.get("MediaName", "unknown")
    print("%s\t%.1f GB\t%s" % (name, size / 1e9, label))
'
}

DRIVES="$(list_drives)"

[ -n "$DRIVES" ] || die "mkusb: no external drives. Plug the stick in."

printf '\nExternal drives:\n\n'
printf '%s\n' "$DRIVES" | while IFS=$'\t' read -r id size label; do
    printf '  %-10s %-10s %s\n' "$id" "$size" "$label"
done

printf '\nWhich drive? (type its identifier, e.g. disk4, or Enter to stop): '
read -r CHOSEN

[ -n "$CHOSEN" ] || die "mkusb: nothing chosen; nothing written."

CHOSEN="${CHOSEN#/dev/}"
CHOSEN="${CHOSEN#r}"

#
#  Asked again rather than trusted from the list above. The two are separate
#  moments and a drive can be unplugged between them; and a name typed by
#  hand has never been checked at all.
#
printf '%s\n' "$DRIVES" | grep -q "^${CHOSEN}	" \
    || die "mkusb: $CHOSEN is not one of the external drives listed. Nothing written."

diskutil info -plist "$CHOSEN" >/dev/null 2>&1 \
    || die "mkusb: $CHOSEN is not a disk this computer knows about."

INTERNAL="$(diskutil info -plist "$CHOSEN" | python3 -c '
import plistlib, sys
print(plistlib.loads(sys.stdin.buffer.read()).get("Internal", True))')"

[ "$INTERNAL" = "False" ] || die "mkusb: $CHOSEN is an internal drive. Refusing."

#
#  And the drive the running system is on, by a third route. Nothing should
#  ever get this far; a disk that does is a disk worth refusing loudly.
#
BOOT="$(diskutil info -plist / | python3 -c '
import plistlib, sys
d = plistlib.loads(sys.stdin.buffer.read())
print(d.get("ParentWholeDisk", d.get("DeviceIdentifier", "")))')"

[ "$CHOSEN" != "$BOOT" ] || die "mkusb: $CHOSEN is the drive this Mac is running from. Refusing."

LABEL="$(printf '%s\n' "$DRIVES" | grep "^${CHOSEN}	" | cut -f3-)"
SIZE="$(printf '%s\n' "$DRIVES" | grep "^${CHOSEN}	" | cut -f2)"

printf '\n'
printf '  About to ERASE  /dev/%s\n' "$CHOSEN"
printf '  Which is        %s, %s\n' "$LABEL" "$SIZE"
printf '  Writing         %s (%s)\n' "$IMG" "$(du -h "$IMG" | cut -f1)"
printf '\n'
printf '  Everything on that drive will be gone.\n'
printf '  Type the drive name exactly to confirm: %s\n' "$LABEL"
printf '  > '
read -r CONFIRM

[ "$CONFIRM" = "$LABEL" ] || die "mkusb: that is not the name. Nothing written."

printf '\nUnmounting...\n'
diskutil unmountDisk "/dev/$CHOSEN"

#
#  `rdisk` rather than `disk`: the raw device skips the buffer cache and is
#  many times faster for a linear write of a whole image. `bs=4m` is macOS
#  spelling. Ctrl-T while it runs prints how far it has got, which is what
#  this `dd` has instead of a progress bar.
#
printf 'Writing (this needs your password, and Ctrl-T shows progress)...\n'
sudo dd if="$IMG" of="/dev/r$CHOSEN" bs=4m

sync
printf '\nEjecting...\n'
diskutil eject "/dev/$CHOSEN"

printf '\nDone. Boot it with Secure Boot disabled - an unsigned GRUB is\n'
printf 'refused by firmware with a "Security Violation" and nothing of\n'
printf 'Kosmos runs at all. On a ThinkPad: F1 at power-on, Security,\n'
printf 'Secure Boot, Disabled. F12 picks the boot device.\n'
