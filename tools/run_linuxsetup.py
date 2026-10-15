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
  * **Downloads shared**: a file Linux wrote to its `~/Downloads` read back
    from Kosmos's `/Home/Downloads`;
  * **on the screen**: the picture at its middle is foot's own background,
    not Linux's console;
  * **its size following the window's** (L5): the window's corner dragged
    larger, and foot's background reaching past where the old picture
    ended.

**`--browser`**: the same with Epiphany, the browser itself (decision 4) -
its 277 packages, a 2 GB disk, waits four times as long, and the picture
held to being a window drawn with many colours rather than foot's one.
Not in the gate: under TCG it takes most of an hour.

Usage: run_linuxsetup.py <image> [--browser]
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
BROWSER = "--browser" in sys.argv
# foot, after a file written to Linux's Downloads - Kosmos's own, shared.
APP = ("dbus-run-session epiphany" if BROWSER
       else "sh -c 'echo made in Linux > /root/Downloads/from-linux.txt; exec foot'")
PACKAGES = ("alpine-base eudev seatd cage wlr-randr mesa-dri-gallium font-dejavu dbus epiphany"
            if BROWSER else "alpine-base eudev seatd cage wlr-randr font-dejavu foot")
DISK_MB, HOME_MB, SLOW = (2048, 3072, 4) if BROWSER else (512, 1024, 1)


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
                 [sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, str(HOME_MB)]):
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

    R.virtualize("2G" if BROWSER else "1G")
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
        guest.type('wm linux:--tell 640x480 --memory %d --disk-mb %d --app "%s" '
                   '--packages "%s" --repo http://10.0.2.2:%d/v3.24'
                   % (768 if BROWSER else 512, DISK_MB, APP, PACKAGES, port))

        check(until(r"linux: Making Linux's disk, %d of %d MB" % (DISK_MB, DISK_MB), at, 120 * SLOW)
              is not None,
              "the window did not make Linux's disk: %r" % guest.seen[-800:])
        installing = until(r"linux: Installing Linux, ([1-9]\d*)%", at, 240 * SLOW)
        check(installing is not None,
              "the setup did not say what it was installing: %r" % guest.seen[-1500:])
        check(until(r"linux: set up", at, 480 * SLOW) is not None,
              "Alpine was not installed onto Linux's disk: %r" % guest.seen[-2000:])
        check(until(r"linux: serial: kosmos-session: cage " + re.escape(APP), at, 180 * SLOW) is not None,
              "Linux did not start from its disk into cage: %r" % guest.seen[-2000:])

        shown = until(r"linux: picture at (\d+),(\d+), (\d+)x(\d+)", at, 30)
        middle = None
        deadline = time.monotonic() + 90 * SLOW

        # The browser: a window drawn - many colours over the picture - and
        # still running, rather than one colour or Linux's console.
        while BROWSER and shown and time.monotonic() < deadline:
            px, py, pw, ph = (int(v) for v in shown.groups())
            width, height, pixel = R.pixel_reader(guest.screendump())
            colours = {pixel(x, y) for y in range(py, py + ph, 8) for x in range(px, px + pw, 8)}
            middle = len(colours)

            if middle >= 40:
                break

            time.sleep(10)

        if BROWSER:
            check(middle is not None and middle >= 40
                  and "kosmos-session: ended" not in guest.seen[at:],
                  "Epiphany did not draw a window in cage: %r colours, the session %s"
                  % (middle, "ended" if "kosmos-session: ended" in guest.seen[at:] else "running"))
            shown = None

        # foot's own background, #242424, filling the screen once it draws.
        while shown and time.monotonic() < deadline:
            px, py, pw, ph = (int(v) for v in shown.groups())
            width, height, pixel = R.pixel_reader(guest.screendump())
            middle = pixel(px + pw // 2, py + ph // 2)

            if all(abs(c - 0x24) <= 4 for c in middle):
                break

            time.sleep(3)

        if not BROWSER:
            check(middle is not None and all(abs(c - 0x24) <= 4 for c in middle),
                  "the picture's middle was not foot's background, #242424: %r" % (middle,))

        # **Its size following the window's** (L5): the window's corner
        # dragged 200 right and 150 down; the window says its picture's new
        # size, and foot's background reaches where the old picture ended -
        # Linux told its display changed, and cage taking the new size.
        if not BROWSER and shown:
            px, py, pw, ph = (int(v) for v in shown.groups())
            width, height, pixel = R.pixel_reader(guest.screendump())
            corner_x, corner_y = px + pw - 6, py + ph - 6

            def to(x, y):
                guest.mouse_to(*R._to_tablet(x, y, width, height))
                time.sleep(0.15)

            mark = len(guest.seen)
            to(corner_x, corner_y)
            guest.mouse_button(True)
            time.sleep(0.2)

            for step in range(1, 11):
                to(corner_x + 20 * step, corner_y + 15 * step)

            guest.mouse_button(False)
            sized = until(r"linux: size (\d+)x(\d+)", mark, 30)
            check(sized is not None and int(sized.group(1)) > pw and int(sized.group(2)) > ph,
                  "the window did not take a larger picture when its corner was dragged: %r"
                  % (sized.group(0) if sized else guest.seen[-600:]))

            beyond = None
            deadline = time.monotonic() + 90

            # Two points only a resized Linux reaches: right of the old
            # width, and below the old height - each well inside the new
            # picture, clear of the strip along its foot (roadmap).
            def foot_bg(p):
                return p is not None and all(abs(c - 0x24) <= 4 for c in p)

            while sized and time.monotonic() < deadline:
                width, height, pixel = R.pixel_reader(guest.screendump())
                beyond = (pixel(px + pw + 60, py + 100), pixel(px + 100, py + ph + 60))

                if all(foot_bg(p) for p in beyond):
                    break

                time.sleep(3)

            check(beyond is not None and all(foot_bg(p) for p in beyond),
                  "Linux did not fill the larger picture - foot's background past where "
                  "the old one ended: %r" % (beyond,))

        os.makedirs(os.path.join("build", "gate"), exist_ok=True)

        with open(os.path.join("build", "gate", "arm-sandbox-linux%s.ppm" % ("browser" if BROWSER else "setup")), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    # **Downloads shared**: what Linux saved in its Downloads, in Kosmos's -
    # read off the `/Home` disk on this Mac once the machine has stopped,
    # since the desktop has the serial line's keys while it runs.
    if not BROWSER:
        got = disk + ".from-linux.txt"

        if os.path.exists(got):
            os.remove(got)

        subprocess.run([os.path.join("build", "host", "lua"), os.path.join(HERE, "kfs.lua"),
                        "get", disk, "/Home/Downloads/from-linux.txt", got], capture_output=True)
        found = open(got).read().strip() if os.path.exists(got) else None
        check(found == "made in Linux",
              "a file Linux wrote to its Downloads was not in Kosmos's: %r" % (found,))

        # What the guest's size follower said, kept beside the run's serial
        # line - its only witness, since udev keeps a rule's output.
        sizelog = os.path.join("build", "gate", "arm-sandbox-linuxsetup.size.log")
        subprocess.run([os.path.join("build", "host", "lua"), os.path.join(HERE, "kfs.lua"),
                        "get", disk, "/Home/Downloads/.kosmos-size.log", sizelog],
                       capture_output=True)

    # The whole of what it said, kept: the setup's lines are long gone from
    # the tail a failure prints.
    os.makedirs(os.path.join("build", "gate"), exist_ok=True)

    with open(os.path.join("build", "gate", "arm-sandbox-linux%s.serial" % ("browser" if BROWSER else "setup")), "w",
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
