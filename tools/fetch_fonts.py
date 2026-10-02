#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The faces Kosmos keeps on the disk rather than in its image.

    python3 tools/fetch_fonts.py          # into build/downloads/cjk/

Japanese, Korean and Simplified and Traditional Chinese: IBM Plex Sans JP,
KR, SC and TC, a regular weight each - 22 MB together, against an image of
35, so they live in `/Home/Fonts` and a process loads one the first time it
draws a character only that face has (Diego, 1 October: "go with the fonts
on disk, loaded when needed"; `roadmap.md` 6zz j5).

Each is the one file out of IBM's repository at the tag of its release -
the releases' zips are 73 to 523 MB, of every weight and format, for the
one file each that is wanted - and held to the sum written here, which was
taken when the file was first fetched and checked against the git hash
GitHub reports for it at that tag. A file already here with its sum is not
fetched again; one with another sum is fetched again and must match.

The licence is the SIL Open Font License 1.1, the same text in all four
packages, and goes beside the faces wherever they are installed.
"""

import hashlib
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "build", "downloads", "cjk")

RAW = "https://raw.githubusercontent.com/IBM/plex/"

FILES = [
    ("IBMPlexSansJP-Regular.ttf", "@ibm/plex-sans-jp@3.0.0",
     "packages/plex-sans-jp/fonts/complete/ttf/hinted/IBMPlexSansJP-Regular.ttf",
     "e5e9ee949e05ca25bf75be44d6412c7071fcdeb8ca6c4361a8d6aeb81f96289a"),
    ("IBMPlexSansKR-Regular.ttf", "@ibm/plex-sans-kr@1.1.0",
     "packages/plex-sans-kr/fonts/complete/ttf/hinted/IBMPlexSansKR-Regular.ttf",
     "193af4c0c4f979251edd13708ea4c3eb4d0d07f9352b3d2a58b43b042767183b"),
    ("IBMPlexSansSC-Regular.ttf", "@ibm/plex-sans-sc@1.1.0",
     "packages/plex-sans-sc/fonts/complete/ttf/hinted/IBMPlexSansSC-Regular.ttf",
     "012e587c5a78d25f456057614b880fc7450b748c352573d8052245df2a71158a"),
    ("IBMPlexSansTC-Regular.ttf", "@ibm/plex-sans-tc@1.1.1",
     "packages/plex-sans-tc/fonts/complete/ttf/hinted/IBMPlexSansTC-Regular.ttf",
     "654677156ffc9ba35503ff569a4d55bc7044501f055bd7752a404445ed31cee7"),
    ("LICENSE.IBMPlexSansCJK", "@ibm/plex-sans-jp@3.0.0",
     "packages/plex-sans-jp/LICENSE.txt",
     "7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"),
]


def sha256(path):
    h = hashlib.sha256()

    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)

    return h.hexdigest()


def fetch(name, tag, path, want):
    at = os.path.join(OUT, name)

    if os.path.isfile(at) and sha256(at) == want:
        return at, False

    url = RAW + tag.replace("/", "%2F") + "/" + path
    data = urllib.request.urlopen(url, timeout=120).read()
    got = hashlib.sha256(data).hexdigest()

    if got != want:
        sys.exit(f"fetch_fonts: {name} from {url} has sum {got}, not {want}")

    with open(at + ".part", "wb") as f:
        f.write(data)

    os.replace(at + ".part", at)
    return at, True


def main():
    os.makedirs(OUT, exist_ok=True)

    for name, tag, path, want in FILES:
        at, fetched = fetch(name, tag, path, want)

        # On stderr: `installed.py` prints install pairs on stdout, which
        # the Makefile reads, and fetches through here.
        if fetched:
            print(f"fetched {name}, {os.path.getsize(at)} bytes, its sum as written",
                  file=sys.stderr)


if __name__ == "__main__":
    main()
