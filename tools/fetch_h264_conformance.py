#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The H.264 conformance streams `tools/test_h264.c` decodes, fetched once.

`tools/h264_conformance.txt` lists them with their sums. A stream already in
`build/downloads/h264-conformance/` with the right sum is left alone, so
after the first run this reads eighteen files and does nothing else; a
missing one is fetched from FFmpeg's FATE server, and one whose sum is wrong
is refused, not used - a checksum test run against the wrong bytes would say
the decoder is wrong.

Usage: fetch_h264_conformance.py
"""

import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tools", "h264_conformance.txt")
HERE = os.path.join(ROOT, "build", "downloads", "h264-conformance")
SERVER = "https://fate-suite.ffmpeg.org/h264-conformance/"


def streams():
    for line in open(MANIFEST):
        if line.strip() and not line.startswith("#"):
            name, path, digest = line.split()[:3]
            yield name, path, digest


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    fetched = 0
    for name, path, digest in streams():
        local = os.path.join(HERE, path)
        if os.path.exists(local) and sha256(local) == digest:
            continue
        os.makedirs(os.path.dirname(local), exist_ok=True)
        try:
            with urllib.request.urlopen(SERVER + path, timeout=60) as r:
                data = r.read()
        except Exception as e:
            sys.exit(f"fetch_h264_conformance: {path} is not in "
                     f"{os.path.relpath(HERE, ROOT)} and could not be "
                     f"fetched from {SERVER}: {e}")
        got = hashlib.sha256(data).hexdigest()
        if got != digest:
            sys.exit(f"fetch_h264_conformance: {path} has sha256 {got}, "
                     f"and h264_conformance.txt says {digest}")
        with open(local, "wb") as f:
            f.write(data)
        fetched += 1
        print(f"fetched {path}, {len(data)} bytes, sha256 ok")
    if fetched == 0:
        print("h264 conformance: every stream present, every sum right")


if __name__ == "__main__":
    main()
