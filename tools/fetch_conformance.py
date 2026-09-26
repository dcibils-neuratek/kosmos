#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The conformance streams the decoders are held to, fetched once.

    fetch_conformance.py h264      tools/h264_conformance.txt
    fetch_conformance.py aac       tools/aac_conformance.txt
    fetch_conformance.py fbx       tools/fbx_conformance.txt

Each manifest lists files and their sums. A file already in
`build/downloads/<kind>-conformance/` with the right sum is left alone, so
after the first run this reads what is there and does nothing else; a
missing one is fetched from where its kind lives - FFmpeg's FATE server for
the decoders' streams, ufbx's repository at the vendored commit for FBX -
and one whose sum is wrong is refused, not used: a checksum test run
against the wrong bytes would say the decoder is wrong.

A manifest line is a name, then pairs of a file and its sha256 - one pair
for H.264, whose references are FFmpeg's own and in the tree, two for AAC,
whose references are PCM too large to carry, one or two for FBX, the second
an OBJ of the same scene - then a description.
"""

import hashlib
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = "https://fate-suite.ffmpeg.org/"
UFBX = ("https://raw.githubusercontent.com/ufbx/ufbx/"
        "26a482ae66871d7de36eb722aa060bce95bce274/data/")
KINDS = {
    "h264": ("tools/h264_conformance.txt", SERVER + "h264-conformance/"),
    "aac": ("tools/aac_conformance.txt", SERVER + "aac/"),
    "fbx": ("tools/fbx_conformance.txt", UFBX),
}
DIGEST = re.compile(r"^[0-9a-f]{64}$")


def files(manifest):
    for line in open(os.path.join(ROOT, manifest)):
        if not line.strip() or line.startswith("#"):
            continue
        words = line.split()[1:]
        while len(words) >= 2 and DIGEST.match(words[1]):
            yield words[0], words[1]
            words = words[2:]


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in KINDS:
        sys.exit("usage: fetch_conformance.py " + "|".join(sorted(KINDS)))

    kind = sys.argv[1]
    manifest, remote = KINDS[kind]
    here = os.path.join(ROOT, "build", "downloads", kind + "-conformance")
    fetched = 0

    for path, digest in files(manifest):
        local = os.path.join(here, path)
        if os.path.exists(local) and sha256(local) == digest:
            continue
        os.makedirs(os.path.dirname(local), exist_ok=True)
        url = remote + path
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
        except Exception as e:
            sys.exit(f"fetch_conformance: {path} is not in "
                     f"{os.path.relpath(here, ROOT)} and could not be "
                     f"fetched from {url}: {e}")
        got = hashlib.sha256(data).hexdigest()
        if got != digest:
            sys.exit(f"fetch_conformance: {path} has sha256 {got}, and "
                     f"{manifest} says {digest}")
        # Through a file of its own and a rename, which is atomic: the gate
        # runs this from three suites at once, and on a machine that has
        # never fetched them they would otherwise write one file together.
        partial = "%s.%d" % (local, os.getpid())
        with open(partial, "wb") as f:
            f.write(data)
        os.replace(partial, local)
        fetched += 1
        print(f"fetched {path}, {len(data)} bytes, sha256 ok")

    if fetched == 0:
        print(f"{kind} conformance: every file present, every sum right")


if __name__ == "__main__":
    main()
