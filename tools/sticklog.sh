#!/bin/bash
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  A file from `/home` on a Kosmos stick, onto this Mac: `make stick-log`.
#
#  **Why.** On the ThinkPad a log reached this Mac as photographs of forty
#  lines of a screen. `diagnose` writes what a diagnosis needs - the whole log
#  among it - to `/home/diagnose.txt`, and this takes it off the stick into
#  `build/stick-diagnose.txt`, as text somebody can search.
#  `make stick-log FILE=/home/log.txt` takes what `log save` wrote instead, and
#  any other file the same way. **A name ending in `/` is a folder**, and
#  every file in it comes back into `build/stick-<folder>/`: `acpi save`
#  leaves one file a table in `/home/acpi/`.
#
#  **It reads the stick and never writes it.** Nothing is unmounted, nothing
#  is ejected, and the one thing opened on the drive is `sticklog.py`'s read of
#  its raw device. That needs root - macOS lets nobody else read a whole disk -
#  so `sudo` asks for a password; everything after the read runs as you, on a
#  copy of the Kosmos partition in a temporary file removed at the end.
#
#  The drives offered are the ones `mkusb.sh` offers - what this computer says
#  is external and physical - and the drive this Mac runs from is refused. A
#  read could not harm it, but a stick is what this is for, and reading the
#  wrong disk is still a mistake.
#
#  macOS only, for `mkusb.sh`'s reasons.

set -euo pipefail

FILE="${1:-/home/diagnose.txt}"
LUA="${2:-build/host/lua}"
OUT="build/stick-$(basename "$FILE")"
HERE="$(dirname "$0")"

die() { printf '%s\n' "$*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "stick-log: macOS only - see the comment at the top of this file."
[ -x "$LUA" ] || die "stick-log: no $LUA, which reads the filesystem. Run \`make $LUA\` first."
#  A path rather than a name, because `sudo` looks names up in its own PATH.
PYTHON="$(command -v python3)" || die "stick-log: no python3, which reads the stick."

DRIVES="$(diskutil list -plist external physical 2>/dev/null | python3 -c '
import plistlib, subprocess, sys

top = plistlib.loads(sys.stdin.buffer.read())

for name in top.get("WholeDisks", []):
    raw = subprocess.run(["diskutil", "info", "-plist", name],
                         capture_output=True).stdout
    info = plistlib.loads(raw)

    if info.get("Internal", True):
        continue

    print("%s\t%.1f GB\t%s" % (name, info.get("TotalSize", 0) / 1e9,
                               info.get("MediaName", "unknown")))
')"

[ -n "$DRIVES" ] || die "stick-log: no external drives. Plug the stick in."

if [ "$(printf '%s\n' "$DRIVES" | wc -l | tr -d ' ')" = 1 ]; then
    CHOSEN="$(printf '%s\n' "$DRIVES" | cut -f1)"
    printf 'The one external drive: %s\n' "$(printf '%s' "$DRIVES" | tr '\t' ' ')"
else
    printf '\nExternal drives:\n\n'
    printf '%s\n' "$DRIVES" | while IFS=$'\t' read -r id size label; do
        printf '  %-10s %-10s %s\n' "$id" "$size" "$label"
    done

    printf '\nWhich one holds Kosmos? (its identifier, e.g. disk4): '
    read -r CHOSEN
    CHOSEN="${CHOSEN#/dev/}"
    CHOSEN="${CHOSEN#r}"

    printf '%s\n' "$DRIVES" | grep -q "^${CHOSEN}	" \
        || die "stick-log: $CHOSEN is not one of the external drives listed."
fi

BOOT="$(diskutil info -plist / | python3 -c '
import plistlib, sys
d = plistlib.loads(sys.stdin.buffer.read())
print(d.get("ParentWholeDisk", d.get("DeviceIdentifier", "")))')"

[ "$CHOSEN" != "$BOOT" ] || die "stick-log: $CHOSEN is the drive this Mac is running from."

COPY="$(mktemp -t kosmos-home)"
trap 'rm -f "$COPY"' EXIT

printf 'Reading the Kosmos partition of /dev/%s - read only; sudo asks for your password.\n' "$CHOSEN"
sudo "$PYTHON" "$HERE/sticklog.py" "/dev/r$CHOSEN" > "$COPY"

if [ "${FILE%/}" != "$FILE" ]; then
    mkdir -p "$OUT"
    "$LUA" "$HERE/kfs.lua" getdir "$COPY" "${FILE%/}" "$OUT"
    printf 'stick-log: %s is in %s/, %s files\n' "$FILE" "$OUT" "$(ls "$OUT" | wc -l | tr -d ' ')"
else
    mkdir -p "$(dirname "$OUT")"

    #
    #  When the file is not there, say what *is* - because the next
    #  question is always "then what did it write, and where".
    #
    #  This used to be one `get` whose failure was a Lua traceback about
    #  comparing a number with nil (`kfs.lua`'s own note has why), and the
    #  answer to it needed a second run with a different verb. Reading the
    #  stick costs a password and a minute of copying, so a run that ends
    #  in "no such file" should end with the folder listed as well.
    #
    if ! "$LUA" "$HERE/kfs.lua" get "$COPY" "$FILE" "$OUT"; then
        printf '\nstick-log: %s is not on the stick. What is in %s:\n' \
               "$FILE" "$(dirname "$FILE")" >&2
        "$LUA" "$HERE/kfs.lua" ls "$COPY" "$(dirname "$FILE")" >&2 || true
        exit 1
    fi

    printf 'stick-log: %s is in %s, %s lines\n' "$FILE" "$OUT" "$(wc -l < "$OUT" | tr -d ' ')"
fi
