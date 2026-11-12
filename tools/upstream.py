#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**Source fetched, not committed** (README's decision log, 11 November
2026; `docs/w4.md`): the release tarballs JetSki's ports build from, each
named with where it comes from and the SHA-256 its publisher gives for it.

    python3 tools/upstream.py fetch icu         # into build/downloads/, checked
    python3 tools/upstream.py unpack icu        # and into build/upstream/<name>/
    python3 tools/upstream.py path icu          # where it was unpacked

A tarball whose sum is not the one below is refused and removed, before
anything reads it: the sum is the record of exactly what was used, and the
only thing the tree keeps of a release. A new release is a new line here,
its sum copied from the publisher's page, never computed from the download.
Unpacking is repeated only when the tarball changed; what is unpacked is
never edited, as nothing vendored is - patches are files the build applies.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOWNLOADS = os.path.join(ROOT, "build", "downloads")
UNPACKED = os.path.join(ROOT, "build", "upstream")

RELEASES = {
    # ICU 4C 79.1, 22 October 2026: GitHub's digests for the release assets.
    "icu": {
        "url": "https://github.com/unicode-org/icu/releases/download/release-79.1/"
               "icu4c-79.1-sources.tgz",
        "sha256": "06fa63ecb521048f8082f12ad988f6b678d90cbd88aafce24cbc94902b191d99",
        "top": "icu",
    },
    # Its data, built by ICU and little-endian, as both of Kosmos's
    # processors are: `icudt79l.dat` inside.
    "icu-data": {
        "url": "https://github.com/unicode-org/icu/releases/download/release-79.1/"
               "icu4c-79.1-data-bin-l.zip",
        "sha256": "72489fb316a60ae1c1da205866edd01b17e8fbf8354ad0615e6cc60f74e08811",
        "top": None,
    },
    # WebKitGTK 2.54.2, 23 October 2026, the sum from the `.sums` file
    # webkitgtk.org publishes beside it. It carries the JSC-only port.
    "webkit": {
        "url": "https://webkitgtk.org/releases/webkitgtk-2.54.2.tar.xz",
        "sha256": "a76345b391bef6211fb5ab6cfab2e86b285d18cda17cbb9ade595f1252b86065",
        "top": "webkitgtk-2.54.2",
    },
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(name):
    release = RELEASES[name]
    os.makedirs(DOWNLOADS, exist_ok=True)
    path = os.path.join(DOWNLOADS, os.path.basename(release["url"]))

    if not os.path.exists(path):
        print("upstream: fetching %s" % release["url"])
        part = path + ".part"
        with urllib.request.urlopen(release["url"]) as r, open(part, "wb") as f:
            shutil.copyfileobj(r, f)
        os.rename(part, path)

    got = sha256(path)

    if got != release["sha256"]:
        os.remove(path)
        sys.exit("upstream: %s has SHA-256 %s, not %s - removed"
                 % (os.path.basename(path), got, release["sha256"]))

    return path


def unpack(name):
    path = fetch(name)
    into = os.path.join(UNPACKED, name)
    stamp = os.path.join(into, ".unpacked")

    if os.path.exists(stamp) and open(stamp).read() == RELEASES[name]["sha256"]:
        return into

    shutil.rmtree(into, ignore_errors=True)
    os.makedirs(into)

    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            z.extractall(into)
    else:
        subprocess.run(["tar", "-xf", path, "-C", into], check=True)

    with open(stamp, "w") as f:
        f.write(RELEASES[name]["sha256"])

    return into


def where(name):
    top = RELEASES[name]["top"]
    into = os.path.join(UNPACKED, name)
    return os.path.join(into, top) if top else into


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("fetch", "unpack", "path") \
            or sys.argv[2] not in RELEASES:
        sys.exit("usage: upstream.py fetch|unpack|path %s" % "|".join(RELEASES))

    action, name = sys.argv[1], sys.argv[2]

    if action == "fetch":
        print(fetch(name))
    elif action == "unpack":
        unpack(name)
        print(where(name))
    else:
        print(where(name))


if __name__ == "__main__":
    main()
