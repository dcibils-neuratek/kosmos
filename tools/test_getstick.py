#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`getstick.sh`, against a release made here on the local disk.

The script downloads a released stick, checks it against the release's
`SHA256SUMS`, unpacks it and hands it to the release's own `mkusb.sh`.
GitHub is not reached: `GETSTICK_API` points at a folder holding what the
API would answer, with `file://` addresses, and the release's `mkusb.sh`
is one that says it was called and with what - so no drive is ever near.

  - a good release is downloaded, unpacked, checked, and handed to its
    `mkusb.sh` with the image, whose bytes are the ones published;
  - run again, nothing is downloaded that is already here and checks;
  - `--download-only` stops before `mkusb.sh`;
  - a download that does not match its sum is refused, and `mkusb.sh` is
    never called - the check the whole script is for;
  - and a release with no `SHA256SUMS` is refused outright.
"""

import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile

import scratch

HERE = os.path.dirname(os.path.abspath(__file__))
GETSTICK = os.path.join(HERE, "getstick.sh")

CHECKS = 8


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def make_release(at, version, sums=True, wrong=False):
    """A release in `at`: the files, and the API's answer about them."""
    files = os.path.join(at, "files")
    api = os.path.join(at, "api")
    os.makedirs(files)
    os.makedirs(os.path.join(api, "tags"))

    image = "kosmos-usb-%s-development.img" % version
    body = bytes(range(256)) * 64 + b"the stick\n"

    with open(os.path.join(files, image), "wb") as f:
        f.write(body)

    with gzip.open(os.path.join(files, image + ".gz"), "wb") as f:
        f.write(body)

    # The tools, with an mkusb.sh that only says what it was given.
    tools = os.path.join(at, "tools-src", "tools")
    os.makedirs(tools)

    with open(os.path.join(tools, "mkusb.sh"), "w") as f:
        f.write('#!/bin/bash\necho "FAKE-MKUSB $1 $(shasum -a 256 "$1" '
                '| cut -c1-16)"\n')

    tarball = "kosmos-mkusb-%s.tar.gz" % version

    with tarfile.open(os.path.join(files, tarball), "w:gz") as t:
        t.add(tools, arcname="tools")

    assets = [image + ".gz", tarball]

    if sums:
        lines = []

        for name in (image, image + ".gz", tarball):
            digest = sha(os.path.join(files, name))

            if wrong and name.endswith(".gz") and "usb" in name:
                digest = "0" * 64

            lines.append("%s  %s\n" % (digest, name))

        with open(os.path.join(files, "SHA256SUMS"), "w") as f:
            f.writelines(lines)

        assets.append("SHA256SUMS")

    answer = {
        "tag_name": "v" + version,
        "name": "Kosmos " + version,
        "assets": [{"name": n,
                    "browser_download_url": "file://" + os.path.join(files, n)}
                   for n in assets],
    }

    with open(os.path.join(api, "tags", "v" + version), "w") as f:
        json.dump(answer, f)

    return "file://" + api, sha(os.path.join(files, image))[:16]


def run(api, into, *args):
    env = dict(os.environ, GETSTICK_API=api, GETSTICK_INTO=into)
    p = subprocess.run(["bash", GETSTICK] + list(args), env=env,
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout + p.stderr


def main():
    work = scratch.directory("getstick")
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    try:
        # A good release, written.
        api, want = make_release(os.path.join(work, "good"), "1.2.3")
        into = os.path.join(work, "into")
        code, said = run(api, into, "1.2.3")

        check(code == 0, "a good release did not reach mkusb.sh:\n" + said)
        check("FAKE-MKUSB kosmos-usb-1.2.3-development.img " + want in said,
              "mkusb.sh was not handed the published image:\n" + said)

        # Again: nothing fetched that is already here and checks.
        code, said = run(api, into, "1.2.3")

        check(code == 0 and said.count("is here already, and checks") == 2,
              "a second run fetched again what it had:\n" + said)

        # Download only: no mkusb.sh.
        code, said = run(api, into, "1.2.3", "--download-only")

        check(code == 0 and "FAKE-MKUSB" not in said
              and "Not written" in said,
              "--download-only reached mkusb.sh:\n" + said)

        # A download that does not match its sum.
        api, _ = make_release(os.path.join(work, "wrong"), "2.0.0",
                              wrong=True)
        code, said = run(api, os.path.join(work, "into2"), "2.0.0")

        check(code != 0, "a download that does not match was written")
        check("FAKE-MKUSB" not in said,
              "mkusb.sh was called for a download that does not match")
        check("does not match SHA256SUMS" in said,
              "the refusal did not say why:\n" + said)

        # No SHA256SUMS at all.
        api, _ = make_release(os.path.join(work, "nosums"), "3.0.0",
                              sums=False)
        code, said = run(api, os.path.join(work, "into3"), "3.0.0")

        check(code != 0 and "no SHA256SUMS" in said
              and "FAKE-MKUSB" not in said,
              "a release with no sums was not refused:\n" + said)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if fails:
        for f in fails:
            print("  " + f)

        print("FAIL: %d of %d checks on getstick.sh." % (len(fails), CHECKS))
        return 1

    print("PASS: %d checks on getstick.sh against a release on this disk (a "
          "good one written, nothing fetched twice, --download-only, a wrong "
          "sum refused before mkusb.sh, and no sums refused)." % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
