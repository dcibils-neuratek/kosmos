#!/bin/bash
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
#  Downloads a released Kosmos stick and writes it to a USB drive, in one
#  command, on any Mac - not only the one Kosmos is built on:
#
#    bash getstick.sh 0.10.115                  download, check, write
#    bash getstick.sh 0.10.115 --download-only  download and check, no write
#    bash getstick.sh                           list the releases with a stick
#
#  Diego, 22 September 2026, writing his first stick from GitHub on his
#  MacBook Pro: "can we make 1 single script that does all this commands at
#  once? just passing by argument the version number to download and write
#  to the stick?" It was six commands: download two files, unpack both,
#  check a sum by eye, and run `mkusb.sh`.
#
#  **The version is named every time**, never "the latest", for the reason
#  `mkusb.sh` names its image: there is more than one build a person might
#  mean, and the one written should be the one chosen.
#
#  **Nothing is written that was not checked.** Each release carries
#  `SHA256SUMS`; the download is held to it and a release without one is
#  refused. That says the bytes arrived as they were published. That the
#  stick then holds them is `mkusb.sh`'s read-back, which is the same script
#  from the same release, run from the folder it came in.
#
#  **Erasing a drive stays `mkusb.sh`'s decision.** It lists only external
#  drives and asks which, and `sudo` asks for a password; this script adds a
#  download in front of that and nothing after it.
#
#  Into `~/Downloads/kosmos-<version>`, kept, so a stick can be written again
#  without downloading again - a second run checks what is there and fetches
#  only what does not match.
#
#  `GETSTICK_API` and `GETSTICK_INTO` point it somewhere else, which is how
#  `tools/test_getstick.py` runs it against a release on the local disk.

set -euo pipefail

REPO="dcibils-neuratek/kosmos"
API="${GETSTICK_API:-https://api.github.com/repos/$REPO/releases}"

die() { printf 'getstick: %s\n' "$*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "macOS only, as mkusb.sh is."
command -v curl >/dev/null 2>&1 || die "no curl."
command -v python3 >/dev/null 2>&1 \
    || die "no python3 - run xcode-select --install, then try again."

VERSION="${1:-}"
MODE="${2:-}"

[ -z "$MODE" ] || [ "$MODE" = "--download-only" ] \
    || die "the second argument can only be --download-only, not $MODE."

#
# No version: say which there are, and stop.
#
if [ -z "$VERSION" ]; then
    printf 'Releases with a stick:\n\n'
    curl -fsSL "$API?per_page=20" | python3 -c '
import json, sys
for r in json.load(sys.stdin):
    if any(a["name"].startswith("kosmos-usb-") for a in r.get("assets", [])):
        print("  %-10s %s" % (r["tag_name"].lstrip("v"), r["name"]))
' || die "could not reach $API."
    printf '\nName one: bash getstick.sh <version>\n'
    exit 1
fi

VERSION="${VERSION#v}"

#
# The release's files, by what they are rather than by a name spelled here:
# the image's name says -development or -stable, and this should not care.
#
ASSETS="$(curl -fsSL "$API/tags/v$VERSION" 2>/dev/null)" \
    || die "no release v$VERSION - or GitHub would not say. Run with no version to list them."

pick() {
    printf '%s' "$ASSETS" | python3 -c '
import json, re, sys
want = re.compile(sys.argv[1])
for a in json.load(sys.stdin).get("assets", []):
    if want.match(a["name"]):
        print(a["name"] + "\t" + a["browser_download_url"])
        break
' "$1"
}

IMG_GZ="$(pick '^kosmos-usb-.*\.img\.gz$')"
TOOLS="$(pick '^kosmos-mkusb-.*\.tar\.gz$')"
SUMS="$(pick '^SHA256SUMS$')"

[ -n "$IMG_GZ" ] || die "release v$VERSION has no stick image."
[ -n "$TOOLS" ] || die "release v$VERSION has no kosmos-mkusb tools."
[ -n "$SUMS" ] || die "release v$VERSION has no SHA256SUMS, so nothing it holds can be checked. Not writing it."

INTO="${GETSTICK_INTO:-$HOME/Downloads}/kosmos-$VERSION"
mkdir -p "$INTO"
cd "$INTO"

#
# Fetched unless what is already here matches its sum - so a second run
# downloads nothing, and a file cut short last time is fetched again.
#
fetch() {
    local name="${1%%	*}" url="${1#*	}"

    if [ -f "$name" ] && grep " $name\$" SHA256SUMS | shasum -a 256 -c --status 2>/dev/null; then
        printf '  %s is here already, and checks.\n' "$name"
        return
    fi

    printf '  %s\n' "$name"
    curl -fL --progress-bar -o "$name" "$url" || die "could not download $name."
}

printf '\nKosmos %s, into %s\n\n' "$VERSION" "$INTO"

curl -fsSL -o SHA256SUMS "${SUMS#*	}" || die "could not download SHA256SUMS."
fetch "$IMG_GZ"
fetch "$TOOLS"

check() {
    grep -q " $1\$" SHA256SUMS || die "SHA256SUMS does not list $1."
    grep " $1\$" SHA256SUMS | shasum -a 256 -c --status \
        || die "$1 does not match SHA256SUMS - the download is not what was published. Delete $INTO/$1 and run this again."
}

IMG_GZ_NAME="${IMG_GZ%%	*}"
IMG_NAME="${IMG_GZ_NAME%.gz}"
TOOLS_NAME="${TOOLS%%	*}"

check "$IMG_GZ_NAME"
check "$TOOLS_NAME"

#
# Unpacked, and the image checked again: gzip's own check says the archive is
# whole, and the sum says it is the image that was built.
#
printf '\nUnpacking...\n'
gunzip -c "$IMG_GZ_NAME" > "$IMG_NAME" || die "$IMG_GZ_NAME would not unpack."
tar xzf "$TOOLS_NAME" || die "$TOOLS_NAME would not unpack."
check "$IMG_NAME"

printf '\n%s checks against SHA256SUMS.\n' "$IMG_NAME"

if [ "$MODE" = "--download-only" ]; then
    printf '\nNot written. To write it:\n\n  cd %s && bash tools/mkusb.sh %s\n\n' "$INTO" "$IMG_NAME"
    exit 0
fi

printf '\nNow the stick: mkusb.sh asks which drive, and erases the one you name.\n'
exec bash tools/mkusb.sh "$IMG_NAME"
