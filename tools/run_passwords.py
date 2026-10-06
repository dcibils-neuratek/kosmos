#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Passwords, on the screen (`docs/keyring.md`, step K6).

A disk carries a keyring made on the Mac - three made-up shares, sealed by
the machine's own `keyfile.c` (`tools/keyring_seed.c`) - and a copy of the
application in `/Home`. Then, as a person would:

  - **the copy in `/Home` is handed nothing**: run at the prompt, it says it
    was not handed the keyring; the one the image serves, started by the
    desktop, is handed it and lists the three, newest first;
  - **sort**: Title chosen from the sort's menu puts them in title order;
  - **a row pressed** shows that entry on the right;
  - **Show** shows its password - on the screen, and never in the log;
  - **a note**, typed into Notes and kept with Return, is the keyring's;
  - **search**, typed, leaves the one that matches;
  - **Delete...** asks first; Cancel keeps it; Delete forgets it, and two
    are left - and the keyring itself says two.

Usage: run_passwords.py IMAGE
"""

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")
SEED = os.path.join(ROOT, "build", "host", "keyring_seed")
APP = os.path.join(ROOT, "user", "bin", "apps", "passwords.lua")

# Made up, every one: the disk is a test's.
ENTRIES = [
    "smb://192.168.10.20:445|alex|studio-mac (SMB)|Projects,Music|not-a-real-password|1|the one in the studio|12",
    "smb://10.0.0.9:445|sam|Archive (SMB)|Old|also-invented|0|cold storage|3",
    "smb://10.0.0.5:445|scanner|Office NAS (SMB)|Scans|made-up-too|0||0",
]


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("passwords")
    seed = os.path.join(work, "seed")
    disk = os.path.join(work, "disk.img")
    forged = os.path.join(work, "forged.lua")
    os.makedirs(seed, exist_ok=True)

    subprocess.run([SEED, seed], input="\n".join(ENTRIES) + "\n", text=True,
                   check=True, capture_output=True)

    with open(APP) as f, open(forged, "w") as g:
        g.write(f.read())

    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", disk, "32",
                    seed + "/machine-key:/Keyring/machine-key",
                    seed + "/keyring:/Keyring/keyring",
                    forged + ":/Home/passwords.lua"],
                   check=True, capture_output=True, cwd=ROOT)

    import run_screenshot as R
    import run_writeapp as WA

    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    guest = WA.with_disk(image, disk)

    def said(prefix, since, seconds=30):
        deadline = time.monotonic() + seconds

        while time.monotonic() < deadline:
            guest._read_available()
            at = guest.seen.find(prefix, since)

            if at >= 0 and "\n" in guest.seen[at + len(prefix):]:
                return guest.seen[at + len(prefix):].split("\n", 1)[0].strip()

            time.sleep(0.2)

        return None

    try:
        guest.wait_for("kosmos> ", "a prompt")

        mark = len(guest.seen)
        guest.type("run /Home/passwords.lua")
        check(said("passwords: not handed the keyring", mark) is not None,
              "the copy in /Home did not say it was not handed the keyring")

        mark = len(guest.seen)
        guest.type("wm passwords")
        window = said("wm: window Passwords at ", mark, 90)
        places = said("passwords: places ", mark, 90)
        first = said("passwords: sorted by last changed: ", mark, 30)

        if window is None or places is None:
            print("FAIL: Passwords did not open:\n" + guest.seen[mark:][-1500:])
            return 1

        wx, wy = (int(v) for v in re.match(r"(\d+),(\d+)", window).groups())
        p = [int(v) for v in re.findall(r"-?\d+", places)]
        list_x, list_y, rowh = p[0], p[1], p[2]
        search_at, sort_at, sort_h = (p[3], p[4]), (p[5], p[6]), p[7]
        show_at, notes_at, delete_at = (p[8], p[9]), (p[10], p[11]), (p[12], p[13])

        check(first == "Office NAS (SMB) | Archive (SMB) | studio-mac (SMB)",
              "not the newest first: %r" % first)

        sw, sh, _ = R.parse_ppm(guest.screendump())

        def click(x, y, screen=False):
            sx, sy = (x, y) if screen else (wx + x, wy + y)
            guest.mouse_to(*R._to_tablet(sx, sy, sw, sh))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)
            time.sleep(0.4)

        def typed(text):
            for k in [{" ": "spc", "(": "shift-9", ")": "shift-0"}.get(c, c) for c in text]:
                guest.sendkey(k)
                time.sleep(0.05)

        # Sort: the menu opens under the dropdown; Title is its second row.
        mark = len(guest.seen)
        click(*sort_at)
        time.sleep(1.0)
        top = wy + sort_at[1] - sort_h // 2 + sort_h
        click(wx + sort_at[0] - 20, R.menu_row_middle(top, 2), screen=True)
        check(said("passwords: sorted by title: ", mark) ==
              "Archive (SMB) | Office NAS (SMB) | studio-mac (SMB)",
              "Title from the sort's menu did not put them in title order")

        # A row: the first, now Archive.
        mark = len(guest.seen)
        click(list_x + 100, list_y + rowh // 2)
        showing = said("passwords: showing ", mark)
        check(showing is not None and showing.endswith("Archive (SMB)"),
              "pressing the first row did not show Archive: %r" % showing)

        # Show: on the screen, never in the log.
        mark = len(guest.seen)
        click(*show_at)
        check(said("passwords: shown ", mark) is not None, "Show did not show it")
        time.sleep(1)
        _, _, shot = R.parse_ppm(guest.screendump())

        if os.environ.get("KEEP_SHOT"):
            with open(os.environ["KEEP_SHOT"], "wb") as f:
                f.write(WA.png(sw, sh, shot))

        # A note, kept with Return.
        mark = len(guest.seen)
        click(*notes_at)
        guest.sendkey("end")
        typed(" (moved)")
        guest.sendkey("ret")
        check(said("passwords: kept the title and notes of ", mark) is not None,
              "a note typed and Return did not keep it")

        # Search: "nas" leaves Office NAS.
        mark = len(guest.seen)
        click(*search_at)
        typed("nas")
        check(said('passwords: 1 shown for "nas"', mark) is not None,
              "searching for nas did not leave one")

        # Delete...: asked, cancelled, asked again, and done.
        click(list_x + 100, list_y + rowh // 2)
        mark = len(guest.seen)
        click(*delete_at)
        asked = said("passwords: asked to delete ", mark)
        check(asked is not None, "Delete... did not ask first")

        # The confirmation's two buttons, where it says they are.
        q = [int(v) for v in re.findall(r"\d+", asked or "0 0 0 0 0")]
        keep_at, drop_at = (q[1], q[2]), (q[3], q[4])

        mark = len(guest.seen)
        click(*keep_at)
        time.sleep(0.5)
        check("passwords: deleted" not in guest.seen[mark:], "Cancel deleted it")

        mark = len(guest.seen)
        click(*delete_at)
        said("passwords: asked to delete ", mark)
        click(*drop_at)
        check(said("passwords: deleted ", mark) is not None,
              "Delete in the confirmation did not delete it")

        check("also-invented" not in guest.seen and "made-up-too" not in guest.seen
              and "not-a-real-password" not in guest.seen,
              "a password reached the log")
    finally:
        guest.close()

    # The keyring itself, read back on the Mac: two entries' worth.
    listing = subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "ls", disk, "/Keyring"],
                             capture_output=True, text=True, cwd=ROOT).stdout
    size = re.search(r"^\s*keyring\s+(\d+)", listing, re.M)
    check(size is not None and int(size.group(1)) == 44 + 8 + 2 * 832 + 16,
          "the keyring on the disk is not two entries' size:\n" + listing)

    if fails:
        print("FAIL: %d of %d checks on Passwords:\n  %s"
              % (len(fails), checks, "\n  ".join(fails)))
        return 1

    print("PASS: %d checks on Passwords (the copy in /Home handed nothing; three "
          "listed newest first, then by title from the sort's menu; a row "
          "shown, its password shown on the screen and never in the log; a "
          "note kept; searched; Delete asked, cancelled, then done - and the "
          "keyring on the disk two entries)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
