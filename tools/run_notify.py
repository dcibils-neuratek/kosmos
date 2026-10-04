#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Notifications on the desktop (`roadmap.md`, *Notifications*, step 2;
the drawing is `docs/notifications.html`).

One boot at the M700's 1720x1440, driven over Telnet and by QEMU's pointer:

  1. A banner: `notify` posts, and the `notifications` program - started
     with the desktop - shows it at the top right under the bar, in a
     banner window: the Terminal typed in keeps the keys, and the card is
     on the screen where nothing was.
  2. Gone by itself after its five seconds, the screen as it was.
  3. An alert stays past those five seconds and goes when its cross is
     pressed.
  4. A press on a banner that names a folder opens it.
  5. Do Not Disturb holds the banner; the history still has it.
  6. The history, from the clock: an alert showing goes as it opens;
     everything said, Clear all, and Do Not Disturb turned off from it.

Usage: run_notify.py IMAGE
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
import subprocess                                           # noqa: E402

# A disk for /Home, as the M700's stick is: the rules are a file there.
_DISK = os.path.join(scratch.directory("notify"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "16"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

CARD_W = 372        # `notifications.lua`'s CARD_W
EDGE = 8            # and EDGE: room round the cards

FOCUS = ('local r = use("/Kosmos/Libraries/wmproto.lua").windows()\n'
         'for _, w in ipairs(r and r.windows or {}) do\n'
         '  if w.focused then print("FOCUS " .. tostring(w.title)) end\n'
         'end\n')

ROOM = ('local r = fs.send("/Running/wm", { type = "workarea" })\n'
        'print(("ROOM %d %d %d %d"):format(r.x, r.y, r.w, r.h))\n')

TERMINAL = ('local r = fs.send("/Running/wm", { type = "launch", '
            'program = "/Kosmos/Apps/terminal.lua" })\n'
            'print("TERMINAL " .. tostring(r and r.ok))\n')

DND = ('local ok = fs.write("/Home/Preferences/notifications", { dnd = (args == "on") })\n'
       'print("DND " .. tostring(ok))\n')

RULES = ('local r = fs.read("/Home/Preferences/notifications")\n'
         'print("RULES dnd=" .. tostring(type(r) == "table" and r.dnd))\n')

# Whose each window is: the window manager's process for it, by the name
# the process table gives that process.
OWNERS = ('local names = {}\n'
          'for _, p in ipairs(sys.processes() or {}) do names[p.id] = p.name end\n'
          'local r = use("/Kosmos/Libraries/wmproto.lua").windows()\n'
          'for _, w in ipairs(r and r.windows or {}) do\n'
          '  print(("OWNER %s %s"):format(tostring(w.title), tostring(names[w.pid])))\n'
          'end\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web,
                   extra=("-fw_cfg", "name=opt/kosmos/fb,string=1720x1440"))
    checks, fails = 0, []
    said = {}

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    def maybe(text, what, since):
        try:
            return guest.wait_for_line(text, what, since)
        except Exception:                  # noqa: BLE001 - its check says
            return None

    def click(x, y, width, height):
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.3)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        time.sleep(0.4)

    def banner_place(since):
        line = maybe("wm: window Notifications at ", "a banner window", since)
        m = re.match(r"(\d+),(\d+) (\d+)x(\d+)", line or "")
        return tuple(int(v) for v in m.groups()) if m else None

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("notifications: banners from ", "the notifications program")
        guest.wait_for("wm: window Deskbar at ", "the bar")
        session = S.connect(telnet)

        for name, text in (("focus", FOCUS), ("room", ROOM), ("terminal", TERMINAL),
                           ("dnd", DND), ("rules", RULES), ("owners", OWNERS)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)

        width, height, _ = R.parse_ppm(guest.screendump())
        room = session.run("/Temporary/room.lua").decode(errors="replace")
        m = re.search(r"ROOM (\d+) (\d+) (\d+) (\d+)", room)
        top = int(m.group(2)) if m else 32

        # ---- 1: a banner, and the keys kept ----
        mark = len(guest.seen)
        session.run("/Temporary/terminal.lua")
        guest.wait_for("wm: window Terminal at ", "a Terminal")
        time.sleep(2)
        _, _, before = R.parse_ppm(guest.screendump())

        mark = len(guest.seen)
        said["post"] = session.run("notify Render finished | Kitchen.c3d, 1920 by 1080, "
                                   "256 samples, in 4 min 12 s.").decode(errors="replace")
        said["banner"] = maybe("notifications: banner 1, Render finished", "the banner", mark)
        place = banner_place(mark)
        said["place"] = place
        time.sleep(0.5)
        _, _, shown = R.parse_ppm(guest.screendump())
        said["focus"] = session.run("/Temporary/focus.lua").decode(errors="replace")
        said["owners"] = session.run("/Temporary/owners.lua").decode(errors="replace")

        # ---- 2: gone by itself ----
        said["gone"] = maybe("wm: closed Notifications", "the banner going", mark)
        time.sleep(1)
        _, _, after = R.parse_ppm(guest.screendump())

        # ---- 3: an alert stays, and its cross closes it ----
        mark = len(guest.seen)
        session.run("notify --alert Timer | The 25 minutes are up")
        alert = banner_place(mark)
        time.sleep(7)
        guest._read_available()
        said["alert stayed"] = "wm: closed Notifications" not in guest.seen[mark:]

        if alert:
            ax, ay, _, _ = alert
            click(ax + EDGE + 4, ay + EDGE + 4, width, height)

        said["crossed"] = maybe("notifications: closed 2", "the cross", mark)

        # ---- 4: a press opens what it names ----
        mark = len(guest.seen)
        session.run("notify --open /Home KINGSTON connected | 57.3 GB, FAT32")
        opened = banner_place(mark)

        if opened:
            ox, oy, ow, _ = opened
            click(ox + ow // 2, oy + EDGE + 30, width, height)

        said["opened"] = maybe("notifications: opened /Home", "a press on a banner", mark)

        # ---- 5: Do Not Disturb ----
        session.run("/Temporary/dnd.lua on")
        mark = len(guest.seen)
        session.run("notify Download finished | BeOS_Bible.pdf, 2.4 MB")
        said["held"] = maybe("notifications: 4 held, Do Not Disturb", "Do Not Disturb", mark)
        time.sleep(1)
        guest._read_available()
        said["no window"] = "wm: window Notifications at " not in guest.seen[mark:]

        # ---- 6: the history, from the clock - and the banners go ----
        session.run("/Temporary/dnd.lua off")
        mark = len(guest.seen)
        session.run("notify --alert Still here | An alert, showing when the history opens")
        said["alert 5"] = maybe("notifications: banner 5, Still here", "an alert before the history", mark)
        session.run("/Temporary/dnd.lua on")

        mark = len(guest.seen)
        click(width - 40, top // 2, width, height)
        said["history"] = maybe("notifications: the history at ", "the history", mark)
        said["hidden"] = maybe("notifications: banners hidden for the history", "the banners going", mark)
        said["hidden closed"] = maybe("wm: closed Notifications", "the banner window closing", mark)
        m = re.match(r"(\d+),(\d+) (\d+)x(\d+), (\d+) kept in (\d+) groups",
                     said["history"] or "")

        if m:
            hx, hy, hw, _, kept, groups = (int(v) for v in m.groups())
            said["kept"] = (kept, groups)

            # Do Not Disturb off, from its row: the switch at the row's end.
            mark = len(guest.seen)
            click(hx + hw - 14 - 12 - 22, hy + 14 + 36 + 26, width, height)
            said["dnd off"] = maybe("notifications: Do Not Disturb off", "the switch", mark)
            said["rules"] = session.run("/Temporary/rules.lua").decode(errors="replace")

            # Clear all, at the heading's end.
            click(hx + hw - 14 - 6 - 20, hy + 14 + 8, width, height)
            said["cleared"] = maybe("notifications: the history cleared", "Clear all", mark)
            said["list"] = session.run("notify --list").decode(errors="replace")

            # And a press outside it closes it.
            click(width // 3, height // 2, width, height)
            said["closed"] = maybe("wm: closed Notification history", "the history closing", mark)

    finally:
        guest.close()

    # ---- what was seen ----
    check(said.get("banner") is not None,
          "the notifications program never showed the banner: %r" % said.get("post"))

    expect_x = width - 16 + EDGE - (CARD_W + 2 * EDGE)
    place = said.get("place")
    check(place is not None and place[0] == expect_x and place[1] == top + 10 - EDGE,
          "the banner was not at the top right under the bar (%d,%d): %r"
          % (expect_x, top + 10 - EDGE, place))

    check("FOCUS Terminal" in (said.get("focus") or ""),
          "the banner took the keys from the Terminal: %r" % said.get("focus"))

    # The window manager ties a window to its process by the kernel's word
    # (`SYS_SENDER`) - a guess about the last one started would give a
    # banner nobody's, or somebody else's.
    owners = said.get("owners") or ""
    check("OWNER Notifications notifications" in owners
          and "OWNER Terminal terminal" in owners,
          "a window was not its own process's: %r"
          % [l for l in owners.splitlines() if l.startswith("OWNER")])

    # The card's rectangle, compared whole: what is behind it may be the
    # card's own colour - a white header in a light look - so a pixel
    # cannot say, and the picture, the name and the text can. And gone, its
    # picture is not where it was: the rest of the card is over windows -
    # the Log, Processes - that go on changing while it shows, so "as it
    # was before" is not a thing the screen can be asked.
    def differing(a, b, x0, y0, w, h):
        n = 0

        for y in range(y0, y0 + h):
            row = (y * width + x0) * 3

            if a[row:row + w * 3] != b[row:row + w * 3]:
                for x in range(w):
                    i = row + x * 3
                    n += a[i:i + 3] != b[i:i + 3]

        return n

    if place:
        px, py, pw, ph = place
        x0, y0, w0, h0 = px + EDGE, py + EDGE, pw - 2 * EDGE, ph - 2 * EDGE
        shown_n = differing(before, shown, x0, y0, w0, h0)
        icon_n = differing(shown, after, x0 + 14, y0 + 12, 40, 40)
        check(shown_n > w0 * h0 // 20,
              "the banner's card was not on the screen: %d of %d pixels changed"
              % (shown_n, w0 * h0))
        check(icon_n > 40 * 40 // 2,
              "the banner's picture was still on the screen once it had gone: "
              "%d of 1600 pixels changed" % icon_n)

    check(said.get("gone") is not None, "the banner did not go by itself")
    check(said.get("alert stayed") is True, "an alert went by itself")
    check(said.get("crossed") is not None, "the alert's cross did not close it")
    check(said.get("opened") is not None, "a press on a banner did not open what it names")
    check(said.get("held") is not None and said.get("no window"),
          "Do Not Disturb did not hold the banner: %r" % (said.get("held"),))
    check(said.get("alert 5") is not None and said.get("hidden") is not None
          and said.get("hidden closed") is not None,
          "an alert showing did not go when the history opened: %r"
          % ((said.get("alert 5"), said.get("hidden"), said.get("hidden closed")),))
    check(said.get("kept") == (5, 1),
          "the history was not the five said, by one sender: %r" % (said.get("kept"),))
    check(said.get("dnd off") is not None and "RULES dnd=false" in (said.get("rules") or ""),
          "Do Not Disturb was not turned off from the history: %r" % (said.get("rules"),))
    check(said.get("cleared") is not None and "notify: 0 kept" in (said.get("list") or ""),
          "Clear all did not empty the history: %r" % (said.get("list"),))
    check(said.get("closed") is not None, "a press outside the history did not close it")

    if fails:
        print("FAIL: %d of %d checks on notifications on the desktop:"
              % (len(fails), checks + len(fails)))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on notifications on the desktop at %dx%d - a banner at the "
          "top right that keeps nobody's keys, gone by itself, an alert that stays "
          "until its cross, a press that opens, Do Not Disturb, and the history from "
          "the clock." % (checks, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
