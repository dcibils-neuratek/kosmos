#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Preferences' Wallpaper row as a carousel (`docs/wallpapers.html`).

Diego, 10 October 2026: "yes to /home/wallpapers and yes to choose a
picture", "we can do like a carrousel of cards of thumbnails of the images
in /home/wallpapers and the ones added by the user". A `/Home` holding a
picture in `/Home/Wallpapers` and another in `/Home/Photos`, Preferences
opened on Appearance, and:

  - the row's cards: None, the folder's picture, the twelve Kosmos carries,
    and Choose a picture... last;
  - thumbnails made by the window manager, each once, at 150 by 94;
  - a click on the folder's picture making it the wallpaper;
  - Choose a picture... opening the Open window, Return choosing the
    picture in Photos, and that picture set and copied into
    `/Home/Wallpapers`;
  - and Tracker's places holding Wallpapers, beside Photos.

Usage: run_wallpapers.py IMAGE
"""

import os
import random
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402
import scratch                                              # noqa: E402

CHECKS = 7

OPEN = ('local r = fs.send("/Running/wm", { type = "launch", program = "preferences", '
        'args = "appearance --row wallpaper" })\n'
        'print("OPENED " .. tostring(r and r.ok))\n')
COPIED = ('local a = fs.getattr("/Home/Wallpapers/sunset.jpg")\n'
          'print("COPIED " .. tostring(a and a.size))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)

    folder, disk = scratch.path("wallpapers-home"), scratch.path("wallpapers-home.img")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(os.path.join(folder, "Wallpapers"))
    os.makedirs(os.path.join(folder, "Photos"))
    walls = os.path.join(ROOT, "assets", "wallpapers")
    shutil.copy(os.path.join(walls, "cloud-pigment.jpg"), os.path.join(folder, "Wallpapers", "mine.jpg"))
    shutil.copy(os.path.join(walls, "dusk-ribbon.jpg"), os.path.join(folder, "Photos", "sunset.jpg"))
    sunset_size = os.path.getsize(os.path.join(folder, "Photos", "sunset.jpg"))
    made = subprocess.run([sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "64"],
                          capture_output=True, text=True)

    if made.returncode != 0:
        print("FAIL: homeimage.py would not make a /Home:\n" + made.stdout + made.stderr)
        return 1

    guest = S.boot(image, telnet, web, extra=["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                                              "-device", "virtio-blk-pci,drive=home"])
    fails = []

    def check(ok, complaint):
        if not ok:
            fails.append(complaint)

    def line(pattern, mark, seconds=15):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")

        # Tracker's places, said by the desktop's at boot.
        side = line(r"tracker: sidebar ([^\r\n]*)", 0, 30)
        check(side is not None and "Photos, Wallpapers," in side.group(1),
              "Tracker's places do not hold Wallpapers beside Photos: %r" % (side and side.group(1)))

        session = S.connect(telnet)
        session.put(OPEN.encode(), "/Temporary/open.lua")
        session.put(COPIED.encode(), "/Temporary/copied.lua")
        time.sleep(1)
        mark = len(guest.seen)
        session.run("/Temporary/open.lua")

        row = line(r"preferences wallpapers: at (\d+),(\d+), (\d+) cards, the second at (\d+),(\d+); "
                   r"choose at (\d+),(\d+)", mark, 40)
        win = line(r"wm: window Preferences at (\d+),(\d+)", mark, 10)

        if not row or not win:
            raise RuntimeError("Preferences never said where its wallpapers are")

        # The last said: the page is built, then scrolled to the row and
        # built again.
        time.sleep(1.5)
        guest._read_available()
        row = list(re.finditer(r"preferences wallpapers: at (\d+),(\d+), (\d+) cards, the second at "
                               r"(\d+),(\d+); choose at (\d+),(\d+)", guest.seen[mark:]))[-1]
        rx, ry, count, sx, sy, cx, cy = (int(v) for v in row.groups())
        wx, wy = int(win.group(1)), int(win.group(2))
        check(count == 15, "the row did not hold None, the folder's one, the twelve carried and "
              "Choose a picture...: %d cards" % count)

        # The thumbnails of the cards in view, each made once, at 150 by 94.
        thumbs = line(r"ui: thumbnail 150x94 of /Home/Wallpapers/mine.jpg, from (\d+)x(\d+), in (\d+) ms",
                      mark, 90)
        check(thumbs is not None, "no thumbnail was made of the folder's picture")
        time.sleep(3)
        guest._read_available()
        made_of = re.findall(r"ui: thumbnail 150x94 of (\S+),", guest.seen[mark:])
        check(len(made_of) >= 3 and len(made_of) == len(set(made_of)),
              "the thumbnails were not each made once: %r" % made_of)

        shot = guest.screendump()
        width, height, _ = R.pixel_reader(shot)

        # Preferences says its places from the window's own corner.
        top = wy

        def press(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.1)
            guest.mouse_button(True)
            time.sleep(0.1)
            guest.mouse_button(False)

        # The folder's picture, chosen by a click.
        mark = len(guest.seen)
        press(wx + sx, top + sy)
        chose = line(r"preferences: wallpaper (\S+)", mark, 10)
        placed = line(r"wm: the wallpaper, (\d+)x(\d+)", mark, 30)
        refused = re.search(r"preferences: wallpaper: ([^\r\n]*)", guest.seen[mark:])
        check(chose is not None and chose.group(1) == "/Home/Wallpapers/mine.jpg"
              and placed is not None and refused is None,
              "a click on the folder's picture did not make it the desktop's wallpaper: %r, %r, %r"
              % (chose and chose.group(0), placed and placed.group(0), refused and refused.group(1)))

        # Choose a picture...: the Open window in Photos, Return choosing.
        time.sleep(1)
        mark = len(guest.seen)
        press(wx + cx, top + cy)
        opened = line(r"preferences: choose a picture at (\d+),(\d+)", mark, 10)
        check(opened is not None, "Choose a picture... did not open the Open window")

        if opened:
            time.sleep(2)
            guest.sendkey("ret")
            kept = line(r"preferences: wallpaper chosen (\S+), kept as (\S+)", mark, 20)
            check(kept is not None and kept.group(1) == "/Home/Photos/sunset.jpg"
                  and kept.group(2) == "/Home/Wallpapers/sunset.jpg",
                  "the picture in Photos was not set and kept in Wallpapers: %r"
                  % (kept and kept.group(0)))
            copied = session.run("/Temporary/copied.lua").decode(errors="replace")
            check(("COPIED %d" % sunset_size) in copied,
                  "/Home/Wallpapers/sunset.jpg is not the picture, whole: %r" % copied.strip()[-80:])
        else:
            check(False, "nothing was chosen")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        try:
            with open(os.path.join("build", "gate", "x86-wallpapers.ppm"), "wb") as f:
                f.write(guest.screendump())
        except Exception:                   # noqa: BLE001 - a picture is extra
            pass
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on the Wallpaper row:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if ("preferences: " in l or "ui: thumbnail" in l or "wm: window Preferences" in l
                    or "error" in l.lower()):
                print("  | " + l.strip()[:200])
        return 1

    times = ", ".join("%s ms" % t for t in re.findall(r"ui: thumbnail 150x94 of \S+, from \S+, in (\d+) ms", seen))
    print("PASS: %d checks on the Wallpaper row (Wallpapers beside Photos; the row's 15 cards; "
          "thumbnails made once each - %s; a click choosing; Choose a picture... setting one "
          "from Photos and copying it into Wallpapers whole)" % (CHECKS, times))
    return 0


if __name__ == "__main__":
    sys.exit(main())
