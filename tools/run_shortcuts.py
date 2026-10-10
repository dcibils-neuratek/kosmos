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

import run_servers as S                                     # noqa: E402

CHECKS = 9

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
        if not again or again.group(2) != "1":
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
          "the setting read when the desktop started again)" % (CHECKS, said.get("hid")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
