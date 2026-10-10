#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Shortcuts (`docs/shortcuts.md`): a person's own global keys.

K2, the window manager's part. A desktop, and through the prompt the
setting `/Home/Preferences/shortcuts` written and handed to the window
manager as Shortcuts will hand it:

  - Super T opens a Terminal, a shortcut of the person's own;
  - "get the window in front out of the way" moved from Super H to Super M:
    Super H then does nothing and Super M does it;
  - an entry that moves the switcher, and one on Control, set aside and
    said, nothing of them applied;
  - a window with the keyboard listening: Super Q and Super Tab posted to
    it as chords, the window not closed and no switcher shown;
  - another window given the keyboard, and the listening over: Super Q
    closes that window again;
  - a window without the keyboard refused when it asks to listen;
  - the desktop ended and started again: the setting read at the start,
    Super T still a Terminal.

Keys go in as QEMU's key events, Super held while the other is pressed.

Usage: run_shortcuts.py IMAGE
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

CHECKS = 16

SETTING = '''local prefs = use("/Kosmos/Libraries/prefs.lua")
local entries = {
  { keys = "Super+T", action = "open", program = "/Kosmos/Apps/terminal.lua" },
  { keys = "Super+M", action = "hide" },
  { keys = "Super+K", action = "switcher" },
  { keys = "Ctrl+N", action = "open", program = "/Kosmos/Apps/calc.lua" },
}
print("SAVED " .. tostring(prefs.write("shortcuts", entries)))
local r = fs.send("/Running/wm", { type = "keys", shortcuts = entries })
print("TOLD " .. tostring(r and r.ok))
'''

LISTENER = '''-- kosmos: application
local ui = use("/Kosmos/Libraries/ui.lua")
local win = ui.window{ title = "Listener", w = 320, h = 140, centre = true }
local said

for _ = 1, 40 do
  local r = fs.send("/Running/wm", { type = "keys", listen = win.handle })
  said = r and (r.ok and "true" or tostring(r.error))
  if r and r.ok then break end
  sys.sleep(5)
end

print("listener: listening " .. tostring(said))

function win:on_event(ev)
  if ev.type == "chord" then print("listener: heard " .. tostring(ev.keys)) end
  return false
end

win:run()
'''

# A window that is not in front asking to listen: Calculator is opened
# after it, so by the time it asks it no longer has the keyboard.
SNEAK = '''local r, why = fs.send("/Running/wm", { type = "keys", listen = tonumber(args) })
print("SNEAK " .. tostring(r and r.ok) .. " " .. tostring(why))
'''

HANDLE = ('local wm = use("/Kosmos/Libraries/wmproto.lua")\n'
          'for _, w in ipairs((wm.windows() or {}).windows or {}) do\n'
          '  if w.title == args then print("HANDLE " .. tostring(w.handle or w.window)) end\n'
          'end\n')

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = args })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    fails, said = [], {}

    def keys(down, *names):
        guest._qmp("input-send-event", {"events": [
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": n}}}
            for n in names]})
        time.sleep(0.25)

    def super_(*names):
        keys(True, "meta_l")
        keys(True, *names)
        keys(False, *reversed(names))
        keys(False, "meta_l")

    # Pictures kept for a person to hold beside `docs/shortcuts.html`.
    def keep(name):
        R.write_png("build/x86_64/shortcuts-%s.png" % name, guest.screendump())

    def line(pattern, mark, seconds=12):
        end = time.time() + seconds
        while time.time() < end:
            guest._read_available()
            m = re.search(pattern, guest.seen[mark:])
            if m:
                return m
            time.sleep(0.2)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        for name, text in (("setting", SETTING), ("listener", LISTENER), ("sneak", SNEAK),
                           ("handle", HANDLE), ("launch", LAUNCH)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)
        time.sleep(3)

        # ---- the setting, handed over ----
        mark = len(guest.seen)
        out = session.run("/Temporary/setting.lua").decode(errors="replace")
        applied = line(r"wm: shortcuts: (\d+) keys, (\d+) of them a person's own", mark)
        aside = re.findall(r"wm: a shortcut set aside: ([^\r\n]*)", guest.seen[mark:])
        if "SAVED true" not in out or "TOLD true" not in out or not applied \
           or applied.group(2) != "1":
            fails.append("the setting was not saved and applied: %r, %r"
                         % (out.strip(), applied and applied.group(0)))
        if len(aside) != 2:
            fails.append("the switcher's entry and the Control one were not both set aside: %r"
                         % aside)

        # ---- Super T: a Terminal ----
        mark = len(guest.seen)
        super_("t")
        if not line(r"wm: window Terminal at ", mark, 20):
            fails.append("Super T did not open a Terminal")
        time.sleep(2)

        # ---- Super H moved to Super M ----
        mark = len(guest.seen)
        super_("h")
        time.sleep(1.5)
        guest._read_available()
        if "wm: minimised" in guest.seen[mark:]:
            fails.append("Super H still hid the window after it was moved")
        mark = len(guest.seen)
        super_("m")
        hid = line(r"wm: minimised ([^\r\n]*)", mark)
        if not hid:
            fails.append("Super M did not hide the window in front")
        else:
            said["hid"] = hid.group(1)

        # ---- listening ----
        mark = len(guest.seen)
        session.run("/Temporary/launch.lua /Temporary/listener.lua")
        listening = line(r"listener: listening ([^\r\n]*)", mark, 20)
        if not listening or listening.group(1).strip() != "true":
            fails.append("the window in front could not listen: %r"
                         % (listening and listening.group(1)))
        time.sleep(1)
        mark = len(guest.seen)
        super_("q")
        heard_q = line(r"listener: heard Super\+Q", mark)
        super_("tab")
        heard_tab = line(r"listener: heard Super\+Tab", mark)
        time.sleep(1)
        guest._read_available()
        if not heard_q or not heard_tab:
            fails.append("Super Q and Super Tab were not posted to the listening window")
        if "wm: closed Listener" in guest.seen[mark:] or "wm: switcher at" in guest.seen[mark:]:
            fails.append("a chord heard was acted on as well")

        # ---- another window in front: listening over ----
        mark = len(guest.seen)
        session.run("/Temporary/launch.lua /Kosmos/Apps/calc.lua")
        line(r"wm: window Calculator at ", mark, 20)
        time.sleep(2)
        out = session.run("/Temporary/handle.lua Listener").decode(errors="replace")
        h = re.search(r"HANDLE (\d+)", out)
        mark = len(guest.seen)
        sneak = session.run("/Temporary/sneak.lua %s" % (h.group(1) if h else "0")).decode(errors="replace")
        refused = line(r"wm: listening refused for Listener, which does not have the keyboard", mark)
        if not h or not refused or "SNEAK nil" not in sneak \
           or "wm: listening for keys" in guest.seen[mark:]:
            fails.append("a window without the keyboard was let listen: %r, %r"
                         % (sneak.strip(), refused and refused.group(0)))
        mark = len(guest.seen)
        super_("q")
        if not line(r"wm: closed Calculator", mark):
            fails.append("with another window in front Super Q did not close it")
        if "listener: heard" in guest.seen[mark:]:
            fails.append("the listening window still heard chords without the keyboard")

        # ---- K3: the window, as drawn (`docs/shortcuts.html`) ----
        mark = len(guest.seen)
        super_("slash")
        groups = line(r"shortcuts: groups All (\d+), The desktop (\d+), Windows (\d+), "
                      r"The screen (\d+), Text (\d+), Yours (\d+); (\d+) not keys", mark, 20)
        shown = line(r"shortcuts: showing desktop: ([^\r\n]*)", mark, 10)
        if not groups or groups.group(6) != "1" or groups.group(7) != "0" \
           or int(groups.group(1)) < 20:
            fails.append("Shortcuts did not open with every group, Yours holding Super T: %r"
                         % (groups and groups.group(0)))
        time.sleep(1.5)
        keep("opened")
        if not shown or "Super+Tab" not in shown.group(1):
            fails.append("the desktop's keys did not include Super Tab: %r"
                         % (shown and shown.group(1)))
        time.sleep(1.5)

        def type_(text):
            for ch in text:
                name = {" ": "spc"}.get(ch, ch)
                keys(True, name)
                keys(False, name)

        mark = len(guest.seen)
        type_("ctrl w")
        found = line(r"shortcuts: showing search \"ctrl w\": ([^\r\n]*)", mark, 10)
        time.sleep(1)
        keep("searched")
        listed = re.findall(r"\(([^)]*)\)", found.group(1)) if found else []
        every = " ".join(listed).split()
        if not every or not all(k.startswith("Ctrl+W") for k in every):
            fails.append("searching \"ctrl w\" did not find only the Control-W rows: %r"
                         % (found and found.group(1)))
        else:
            said["search"] = len(every)
        mark = len(guest.seen)
        tap = lambda *n: (keys(True, *n), keys(False, *reversed(n)))
        tap("esc")
        if not line(r"shortcuts: showing desktop: ", mark, 10):
            fails.append("Escape did not clear the search")

        # Control-N, Super J, Enter: a shortcut of one's own, which opens a
        # Terminal the moment it is added.
        mark = len(guest.seen)
        tap("ctrl", "n")
        sheet = line(r"shortcuts: the sheet open", mark, 10)
        time.sleep(1)
        super_("j")
        heard = line(r"shortcuts: heard Super\+J", mark, 10)
        time.sleep(1)
        keep("sheet")
        tap("ret")
        added = line(r"shortcuts: added Super\+J, (\S+)", mark, 10)
        saved = line(r"shortcuts: saved, (\d+) entries", mark, 10)
        time.sleep(1.5)
        mark2 = len(guest.seen)
        super_("j")
        opened = line(r"wm: window Terminal at ", mark2, 20)
        if not (sheet and heard and added and saved and opened) \
           or "terminal" not in added.group(1):
            fails.append("Add a shortcut did not make Super J open a Terminal: sheet %s, "
                         "heard %s, added %r, saved %r, opened %s"
                         % (bool(sheet), bool(heard), added and added.group(1),
                            saved and saved.group(1), bool(opened)))
        time.sleep(1.5)

        # Change on "out of the way", Super Q pressed: taken, said, and Use
        # it here takes it - Close the window in front is left without keys.
        # The Terminal it opened closed again, so Shortcuts is in front.
        mark = len(guest.seen)
        super_("q")
        line(r"wm: closed Terminal", mark, 10)
        time.sleep(1.5)
        links = re.findall(r"shortcuts: links ([^\r\n]*)", guest.seen)
        at = re.search(r"change hide at (\d+),(\d+)", links[-1]) if links else None
        took = None
        if at:
            width, height, _ = R.parse_ppm(guest.screendump())

            def click(x, y):
                guest.mouse_to(*R._to_tablet(x, y, width, height))
                time.sleep(0.3)
                guest.mouse_button(True)
                time.sleep(0.1)
                guest.mouse_button(False)

            mark = len(guest.seen)
            click(int(at.group(1)), int(at.group(2)))
            line(r"shortcuts: changing hide", mark, 10)
            time.sleep(0.5)
            super_("q")
            taken = line(r"shortcuts: Super Q is already for close the window in front", mark, 10)
            use = line(r"shortcuts: links [^\r\n]*use at (\d+),(\d+)", mark, 10)
            time.sleep(1)
            keep("changing")
            if use:
                click(int(use.group(1)), int(use.group(2)))
                took = line(r"shortcuts: hide took Super\+Q", mark, 10)
            if not (taken and took):
                fails.append("Change with Super Q did not say it was taken and take it: "
                             "said %s, use %s, took %s" % (bool(taken), bool(use), bool(took)))
        else:
            fails.append("Shortcuts did not say where Change for hiding is: %r" % links[-1:])

        # Default: back to Super H.
        if took:
            time.sleep(1)
            mark = len(guest.seen)
            links = re.findall(r"shortcuts: links ([^\r\n]*)", guest.seen)
            at = re.search(r"change hide at (\d+),(\d+)", links[-1]) if links else None
            if at:
                click(int(at.group(1)), int(at.group(2)))
                line(r"shortcuts: changing hide", mark, 10)
                time.sleep(0.5)
                d = line(r"shortcuts: links [^\r\n]*default hide at (\d+),(\d+)", mark, 10)
                if d:
                    click(int(d.group(1)), int(d.group(2)))
            if not line(r"shortcuts: back to its default, hide", mark, 10):
                fails.append("Default did not put hiding back")
            else:
                said["default"] = True
        time.sleep(1)

        # ---- the desktop again: the setting read at its start ----
        mark = len(guest.seen)
        guest.proc.stdin.write(b"\x17q")
        guest.proc.stdin.flush()
        line(r"kosmos> ", mark, 20)
        time.sleep(1)
        mark = len(guest.seen)
        guest.type("wm")
        again = line(r"wm: shortcuts: (\d+) keys, (\d+) of them a person's own", mark, 60)
        line(r"wm: window Deskbar at ", mark, 60)
        time.sleep(4)
        if not again or again.group(2) != "2":
            fails.append("the setting was not read when the desktop started: %r"
                         % (again and again.group(0)))
        mark = len(guest.seen)
        super_("t")
        if not line(r"wm: window Terminal at ", mark, 20):
            fails.append("after a new start Super T did not open a Terminal")
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of %d checks on a person's shortcuts:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        # The window manager's own words about it, which the gate keeps.
        for l in seen.splitlines():
            if re.search(r"wm: (reply for|a shortcut|listening|not listening|heard)|error|failed", l):
                print("  | " + l.strip()[:200])
        return 1

    print("PASS: %d checks on a person's shortcuts (the setting saved and applied, Super T "
          "opening a Terminal; hiding moved from Super H to Super M, the old chord doing "
          "nothing - %s hidden; the switcher's entry and a Control one set aside; Super Q "
          "and Super Tab posted to the listening window and not acted on; a window "
          "without the keyboard refused; listening over once another window had it; and "
          "the setting read when the desktop started again; and the window as drawn - every "
          "group, Super Tab among the desktop's keys, \"ctrl w\" finding %s Control-W rows "
          "and Escape clearing it, Control-N, Super J and Enter making Super J open a "
          "Terminal, Change with Super Q said taken and Use it here taking it, and Default "
          "putting it back)" % (CHECKS, said.get("hid"), said.get("search")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
