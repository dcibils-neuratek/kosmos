#!/bin/bash
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  The desktop's pictures, made from the photographs as downloaded:
#
#      tools/wallpapers.sh ~/Downloads/kosmos-wallpapers
#
#  Each `*-unsplash.jpg` there is scaled to cover 1920x1080 - whichever side
#  is short is brought to the screen's - cropped to its middle, and saved as
#  JPEG at quality 80 into `assets/wallpapers/`, named without the
#  `-unsplash`. `assets/wallpapers/README.md` has why they are committed made
#  rather than made by the build.
#
#  macOS only: `sips` is the Mac's own image tool, and nothing had to be
#  installed to use it.

set -euo pipefail

FROM="${1:?usage: tools/wallpapers.sh <folder of *-unsplash.jpg>}"
TO="$(dirname "$0")/../assets/wallpapers"

command -v sips >/dev/null || { echo "wallpapers: sips is macOS's; this is not macOS" >&2; exit 1; }

for f in "$FROM"/*-unsplash.jpg; do
    out="$TO/$(basename "$f" -unsplash.jpg).jpg"
    w=$(sips -g pixelWidth "$f" | awk '/pixelWidth/ { print $2 }')
    h=$(sips -g pixelHeight "$f" | awk '/pixelHeight/ { print $2 }')

    # Cover, not fit: the side that is short against 16:9 reaches the screen.
    if [ $((w * 1080)) -ge $((h * 1920)) ]; then
        sips --resampleHeight 1080 "$f" --out "$out" >/dev/null
    else
        sips --resampleWidth 1920 "$f" --out "$out" >/dev/null
    fi

    sips -c 1080 1920 "$out" >/dev/null
    sips -s format jpeg -s formatOptions 80 "$out" --out "$out" >/dev/null
    printf '%s  %s KB\n' "$out" "$(( $(stat -f %z "$out") / 1024 ))"
done
