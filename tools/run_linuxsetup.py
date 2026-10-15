#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Linux on Kosmos from its own disk (`docs/linux.md` L4).

The ARM machine at EL2, with Alpine installed as an image - its kernel, its
initramfs and its modloop - and a `/Home` large enough for Linux's disk.
The Linux window opened with a small application in place of the browser -
`foot`, a terminal - and Alpine's packages served from this Mac
(`tools/alpine_repo.py`), so nothing here needs the internet. Held to:

  * **its disk made**: the window making it a piece at a time, said;
  * **set up**: the disk made ext4 and Alpine installed onto it through
    Kosmos's network, the window saying how far it has got;
  * **booted from it**: the sandbox started again from its disk, its
    modules from the modloop, and `cage` showing `foot`;
  * **on the screen**: the picture at its middle is foot's own background,
    not Linux's console.

Usage: run_linuxsetup.py <image>
"""

import functools
import http.server
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGES = "alpine-base eudev seatd cage font-dejavu foot"


def main():
    image = sys.argv[1]
    folder = scratch.path("linuxsetup-home")
    disk = scratch.path("linuxsetup-home.img")
    alpine = os.path.join(folder, "Sandboxes", "Images", "alpine")

    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(alpine)

    with open(os.path.join(alpine, "package.txt"), "w") as f:
        f.write("name        alpine\ntitle       Alpine Linux\nkind        sandbox\n")

    for step in ([sys.executable, os.path.join(HERE, "alpine.py"), alpine],
                 [sys.executable, os.path.join(HERE, "alpine_repo.py"), "aarch64"]
                 + PACKAGES.split() + ["e2fsprogs"],
                 [sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "1024"]):
        made = subprocess.run(step, capture_output=True, text=True)

        if made.returncode != 0:
            print("FAIL: %s would not run:\n%s%s" % (os.path.basename(step[1]), made.stdout, made.stderr))
            return 1

        if step[1].endswith("alpine_repo.py"):
            repo = made.stdout.strip()

    httpd = http.server.ThreadingHTTPServer(
        ("0.0.0.0", 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=repo))
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    R.virtualize("1G")
    R.extra_args(image, ["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                         "-device", R.device(image, "blk") + ",drive=home",
                         "-netdev", "user,id=net0",
                         "-device", R.device(image, "net") + ",netdev=net0"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def until(pattern, since, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            found = re.search(pattern, guest.seen[since:].replace("\r", ""))
            if found:
                return found
            time.sleep(0.3)
            guest._read_available()
        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        # The network first: the setup reaches this Mac through it.
        at = len(guest.seen)
        guest.type('for _ = 1, 100 do local i = fs.net_info("/Network") if i and i.address '
                   'and i.address ~= "\\0\\0\\0\\0" then break end sys.sleep(25) end print("NET" .. "-UP")')
        until(r"NET-UP", at, 60)

        at = len(guest.seen)
        guest.type('wm linux:--tell 640x480 --memory 512 --disk-mb 512 --app foot '
                   '--packages "%s" --repo http://10.0.2.2:%d/v3.24' % (PACKAGES, port))

        check(until(r"linux: Making Linux's disk, 512 of 512 MB", at, 120) is not None,
              "the window did not make Linux's disk: %r" % guest.seen[-800:])
        installing = until(r"linux: Installing Linux, ([1-9]\d*)%", at, 240)
        check(installing is not None,
              "the setup did not say what it was installing: %r" % guest.seen[-1500:])
        check(until(r"linux: set up", at, 480) is not None,
              "Alpine was not installed onto Linux's disk: %r" % guest.seen[-2000:])
        check(until(r"linux: serial: kosmos-session: cage foot", at, 180) is not None,
              "Linux did not start from its disk into cage: %r" % guest.seen[-2000:])

        shown = until(r"linux: picture at (\d+),(\d+), (\d+)x(\d+)", at, 30)
        middle = None
        deadline = time.monotonic() + 90

        # foot's own background, #242424, filling the screen once it draws.
        while shown and time.monotonic() < deadline:
            px, py, pw, ph = (int(v) for v in shown.groups())
            width, height, pixel = R.pixel_reader(guest.screendump())
            middle = pixel(px + pw // 2, py + ph // 2)

            if all(abs(c - 0x24) <= 4 for c in middle):
                break

            time.sleep(3)

        check(middle is not None and all(abs(c - 0x24) <= 4 for c in middle),
              "the picture's middle was not foot's background, #242424: %r" % (middle,))

        os.makedirs(os.path.join("build", "gate"), exist_ok=True)

        with open(os.path.join("build", "gate", "arm-sandbox-linuxsetup.ppm"), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    # The whole of what it said, kept: the setup's lines are long gone from
    # the tail a failure prints.
    os.makedirs(os.path.join("build", "gate"), exist_ok=True)

    with open(os.path.join("build", "gate", "arm-sandbox-linuxsetup.serial"), "w",
              errors="replace") as f:
        f.write(transcript)

    if failed:
        print("FAIL: %d of %d checks on Linux from its own disk:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-5000:])
        return 1

    print("PASS: %d checks on Linux from its own disk (made, Alpine installed onto it "
          "from this Mac through Kosmos's network, booted from it into cage and foot)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
