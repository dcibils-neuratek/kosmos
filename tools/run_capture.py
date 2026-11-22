#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Pointer capture (`docs/quake3e.md`, Q1): a window looks about with the mouse.

A game turns with the mouse, and a pointer on a screen stops at the screen's
edge. So a window may ask the window manager for the pointer to itself: while
it has it, the arrow stays where it was and every movement comes to the
window as `motion`, unclamped. The board counts the movement beside the
position (`pointer_state`'s `dx`, `dy`), the console server stashes it as it
does the wheel, and the window manager hands it on.

One boot, with **a PS/2 mouse alone** - relative, as the M700's USB mouse
is; a tablet says where it is and never how far. A program opens a window and
asks for capture (`wmproto.capture`), and then:

  1. **It has it**: the window manager says so, and the program is told.
  2. **Movement past the edge**: QEMU's mouse is moved right by far more
     than the screen is wide, then down. The program has to have been sent
     more than a screen's width of motion - which a clamped position cannot
     give - and the arrow must not have moved.
  3. **A press goes to it** at the window's middle, wherever the arrow is -
     the middle of the whole window, header and all, since the window
     manager does not know how tall a header the kit drew: across, 120 of
     240, and down, somewhere in the content.
  4. **Super gives it back**: the program is told, and movement after that
     is the arrow's again and none of it is the program's.

Its control is `--no-capture`: the same program not asking. No motion
arrives at all, and the check of step 2 fails.

Usage: run_capture.py IMAGE [--no-capture]
"""

import random
import re
import sys
import time

import run_screenshot as R
import run_servers as S

PROGRAM = '''-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local ASK = %s
local win = ui.window{ title = "capturetest", w = 240, h = 160, x = 300, y = 200, direct = true }
if not win then print("capturetest: no window") return end
local s = win:surface()
s:fill(0, 0, 240, 160, 0xff335577)
win:commit{ x = 0, y = 0, w = 240, h = 160 }
print("capturetest: open")
if ASK then wmproto.capture(win.handle, true) end
local sx, sy, said = 0, 0, 0
while win.running do
  local r = wmproto.poll(win.handle, 2)
  if not r then break end
  for _, ev in ipairs(r.events or {}) do
    if ev.type == "close" then win:close()
    elseif ev.type == "capture" then print("capturetest: capture " .. tostring(ev.on))
    elseif ev.type == "motion" then sx, sy = sx + ev.dx, sy + ev.dy
    elseif ev.type == "mouse" and ev.action ~= "move" then
      print(("capturetest: mouse %%s %%s at %%d,%%d"):format(ev.action, tostring(ev.button), ev.x, ev.y))
    end
  end
  if sx + sy ~= said then
    said = sx + sy
    print(("capturetest: moved %%d %%d"):format(sx, sy))
  end
end
'''

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", '
          'program = "/Temporary/capture.lua" })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')


def rel(guest, axis, value):
    guest._qmp("input-send-event", {"events": [
        {"type": "rel", "data": {"axis": axis, "value": value}}]})


def button(guest, down):
    guest._qmp("input-send-event", {"events": [
        {"type": "btn", "data": {"down": down, "button": "left"}}]})


def key(guest, name, down):
    guest._qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": name}}}]})


def last_moved(seen):
    m = re.findall(r"capturetest: moved (-?\d+) (-?\d+)", seen)
    return (int(m[-1][0]), int(m[-1][1])) if m else (0, 0)


def settle(guest, seconds=1.5):
    """Until the program's count of motion stops changing."""
    last, quiet = None, time.time()
    deadline = time.time() + 20

    while time.time() < deadline:
        time.sleep(0.2)
        guest._read_available()
        now = last_moved(guest.seen)

        if now != last:
            last, quiet = now, time.time()
        elif time.time() - quiet > seconds:
            break

    return last_moved(guest.seen)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
        else "build/x86_64/kosmos.elf"
    ask = "--no-capture" not in sys.argv
    fails, said = [], {}
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)

    # A PS/2 mouse alone: the tablet every other suite has is taken out.
    saved = list(R.X86_ARGS)
    i = R.X86_ARGS.index("virtio-tablet-pci")
    del R.X86_ARGS[i - 1:i + 1]

    try:
        guest = S.boot(image, telnet, web)
    finally:
        R.X86_ARGS[:] = saved

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        session.put((PROGRAM % ("true" if ask else "false")).encode(), "/Temporary/capture.lua")
        session.put(LAUNCH.encode(), "/Temporary/launch.lua")
        session.run("/Temporary/launch.lua")
        guest.wait_for("capturetest: open", "the program's window")

        # 1. Had.
        deadline = time.time() + 10
        while ask and time.time() < deadline and "capturetest: capture true" not in guest.seen:
            time.sleep(0.2)
            guest._read_available()

        said["had"] = "capturetest: capture true" in guest.seen
        said["told by wm"] = "wm: capturetest has the pointer" in guest.seen
        arrow = re.findall(r"wm: button (?:down|up) at (\d+),(\d+)", guest.seen)

        # 2. Far past the right edge, then down: 80 reports of 100 counts.
        for _ in range(80):
            rel(guest, "x", 100)
        for _ in range(20):
            rel(guest, "y", 100)

        said["moved"] = settle(guest)
        width, height, _ = R.parse_ppm(guest.screendump())
        said["screen"] = (width, height)

        # 3. A press, and its release.
        mark = len(guest.seen)
        button(guest, True)
        time.sleep(0.3)
        button(guest, False)
        deadline = time.time() + 5
        while time.time() < deadline and "capturetest: mouse release" not in guest.seen[mark:]:
            time.sleep(0.2)
            guest._read_available()
        said["press"] = re.findall(r"capturetest: mouse (press|release) (\w+) at (\d+),(\d+)",
                                   guest.seen[mark:])
        said["arrow at the press"] = re.findall(r"wm: button (?:down|up) at (\d+),(\d+)",
                                                guest.seen[mark:])

        # 4. Super gives it back; movement after that is the arrow's.
        mark = len(guest.seen)
        key(guest, "meta_l", True)
        time.sleep(0.2)
        key(guest, "meta_l", False)
        deadline = time.time() + 5
        while time.time() < deadline and "capturetest: capture false" not in guest.seen[mark:]:
            time.sleep(0.2)
            guest._read_available()
        said["given back"] = "capturetest: capture false" in guest.seen[mark:]
        said["wm said Super"] = "gave back the pointer: Super" in guest.seen[mark:]
        key(guest, "esc", True)
        key(guest, "esc", False)
        before = last_moved(guest.seen)
        for _ in range(10):
            rel(guest, "x", -50)
        said["after"] = settle(guest, 1.0)
        said["after before"] = before
        said["arrow before"] = arrow
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the boot stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    print("capture: %r" % {k: v for k, v in said.items() if k != "arrow before"})

    width = (said.get("screen") or (0, 0))[0]
    moved = said.get("moved") or (0, 0)

    if not said.get("had") or not said.get("told by wm"):
        fails.append("the window asked for the pointer and was not given it: %r %r"
                     % (said.get("had"), said.get("told by wm")))
    if not (width and moved[0] > width * 2 and moved[1] > 0):
        fails.append("8,000 counts right and 2,000 down came to the window as %r, on a "
                     "screen %d wide - no more than a clamped position could give" % (moved, width))
    press = said.get("press") or []
    if [p[0] for p in press] != ["press", "release"] or any(p[1] != "left" for p in press) \
            or any(int(p[2]) != 120 or not 0 <= int(p[3]) < 160 for p in press):
        fails.append("the press under capture did not reach the window at its middle: %r"
                     % press)
    if not said.get("given back") or not said.get("wm said Super"):
        fails.append("Super did not give the pointer back, or the window was not told: %r %r"
                     % (said.get("given back"), said.get("wm said Super")))
    if said.get("after") != said.get("after before"):
        fails.append("after Super the window still had motion: %r then %r"
                     % (said.get("after before"), said.get("after")))
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 5

    if fails:
        print("FAIL: %d of %d checks on pointer capture:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on pointer capture (a PS/2 mouse: the window given the pointer and "
          "told; %d,%d pixels of motion past a screen %d wide; a press at its middle; Super gave "
          "it back and the window was told; no motion after)" % (checks, moved[0], moved[1], width))
    return 0


if __name__ == "__main__":
    sys.exit(main())
