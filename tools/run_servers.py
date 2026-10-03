#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Servers window sees its servers, and what it keeps starts at boot.

`user/bin/apps/servers.lua`, as `docs/servers.html` draws it (Diego, 29
September: "a servers app that hold all servers like web, telnet, vnc, etc
... so we can config and activate/deactivate network servers"; on the
drawing, "The mockup looks great"). Two boots of the M700's own shape - the
desktop by itself, `telnetd` beside it - driven from here by the Mac's own
client, since this is what it is for:

  1. `open servers` over Telnet opens the window, which says the command
     line is running with the one session that opened it and the web
     server and the screen stopped; `telnetd`'s name in `/Running` answers
     a Disconnect by ending the session asked about; and the web server
     (port 8080) and the screen are set to start with the machine, as the
     window keeps them, in `/Home/Preferences/servers`.
  2. The same disk booted again: the web server started by itself, and a
     page fetched from it by this Mac; and **the screen by VNC**
     (`vncd`, `roadmap.md` remote 7a), started by itself too and looked at
     by `tools/kosmos_vnc.py`: the size the display has, a whole frame as
     QEMU's own screendump has it, a window opened afterwards arriving as
     an update of what changed, a region in 16-bit 565, a Disconnect from
     the Servers window's side, the window manager letting its copy go when
     nobody has looked for five seconds, and a password - refused wrong,
     admitted right, the right answer computed by OpenSSL's DES rather than
     by the one it checks.

Usage: run_servers.py IMAGE
"""

import http.client
import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

PAGE = "<h1>served by itself</h1>\n"

PROBES = {
    # What the window keeps when Start with the machine is switched on for
    # the web server, written as it writes it.
    "keep.lua": ('fs.send("/Home/Preferences", { type = "mkdir" })\n'
                 'fs.write("/Home/Preferences/servers", { web = { port = 8080, '
                 'folder = "/Home/www", at_start = true }, '
                 'vnc = { port = 5900, at_start = true, password = "" } })\n'
                 # And a quiet desktop for the second boot: Processes re-sorts
                 # every second, and two rows that swap and swap back between
                 # two screendumps pass for still while a frame caught them
                 # swapped - which failed a correct frame under a gate's load.
                 'fs.write("/Home/Preferences/startup", { items = { "tracker" } })\n'
                 'print("KEPT " .. tostring(fs.read("/Home/Preferences/servers").web.port))\n'),
    # The window's Disconnect, as it sends it - to the command line, and to
    # the screen.
    "kick.lua": ('local r = fs.send("/Running/telnetd", { type = "disconnect", from = "10.0.2.2" })\n'
                 'print("KICKED " .. tostring(r and r.ended))\n'),
    # Who is lent the desktop: a program launched by the window manager
    # without `needs desktop`, and one on the disk that says it - which is
    # refused as well, since a program's needs are read from the image alone
    # (`binfs`): a header in `/Home` is words anybody could have written, and
    # grants nothing. `vncd`, in the image, is lent it (below). Neither may
    # watch through the public name any more.
    "reach.lua": ('local r = fs.send("/Running/wm/remote", { type = "watch" })\n'
                  'local p = fs.send("/Running/wm", { type = "watch" })\n'
                  'print("REACH plain " .. tostring(type(r) == "table" and r.ok == true) '
                  '.. " public " .. tostring(type(p) == "table" and p.ok == true))\n'),
    "reach2.lua": ('-- kosmos: needs desktop\n'
                   'local r = fs.send("/Running/wm/remote", { type = "watch" })\n'
                   'local p = fs.send("/Running/wm", { type = "watch" })\n'
                   'print("REACH lent " .. tostring(type(r) == "table" and r.ok == true) '
                   '.. " public " .. tostring(type(p) == "table" and p.ok == true))\n'),
    "vnckick.lua": ('local r = fs.send("/Running/vncd", { type = "disconnect", from = "10.0.2.2" })\n'
                    'print("VNCKICKED " .. tostring(r and r.ended))\n'),
    # A password typed on the Screen page, and its switch for a viewer to
    # use the keyboard and the pointer, as the window keeps them.
    "vncpass.lua": ('local all = fs.read("/Home/Preferences/servers")\n'
                    'all.vnc.password = "Kosmos"\n'
                    'all.vnc.control = true\n'
                    'fs.write("/Home/Preferences/servers", all)\n'
                    'print("PASSWORD " .. fs.read("/Home/Preferences/servers").vnc.password)\n'),
}


STRIPES = """-- kosmos: application
local ui = use("/Kosmos/Libraries/ui.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local win = ui.window{ title = "Stripes", w = 4000, h = 120, x = 0, y = 60, direct = true }
local s = win:surface()
local w, h = s:size()
for x = 0, w - 1 do s:fill(x, 0, 1, h, 0xff000000 | ((x * 2654435761) & 0xffffff)) end
win:commit{ x = 0, y = 0, w = w, h = h }
print(("STRIPES %dx%d, granted %dx%d"):format(w, h, win.w, win.h))
while win.running do
  if not wmproto.poll(win.handle, 25) then break end
end
"""


def upright(width, px, placed):
    """Whether the probe's columns are on the screen where it drew them: at
    every 37th column of its window, near its top and near its bottom, the
    very colour it gave that column. Answers how many of the places looked
    at are wrong, and how many were looked at. A blank window, a shear and
    a picture stretched from another size all fail it."""
    m = re.match(r"(\d+),(\d+) (\d+)x(\d+)", placed)
    x0, y0, w, h = (int(m.group(i)) for i in range(1, 5))
    wrong = looked = 0

    for x in range(0, min(w, width - x0), 37):     # what is on the screen
        c = (x * 2654435761) & 0xffffff
        want = bytes(((c >> 16) & 255, (c >> 8) & 255, c & 255))

        for y in (y0 + 10, y0 + h - 10):
            at = (y * width + x0 + x) * 3
            looked += 1

            wrong += px[at:at + 3] != want

    return wrong, looked


def same_share(frame, width, height, px):
    """How much of a viewer's frame is what QEMU scans out, row by row."""
    same = 0
    row = width * 3

    for y in range(height):
        a, b = frame[y * row:(y + 1) * row], px[y * row:(y + 1) * row]

        if a == b:
            same += width
            continue

        for x in range(0, row, 3):
            if a[x:x + 3] == b[x:x + 3]:
                same += 1

    return same / float(width * height)


def still_share(frame, width, before, after):
    """How much of the screen that held still is in the viewer's frame.

    The desktop is alive - Processes re-sorts every second, Monitor draws a
    column - so a frame and a screendump taken after it may honestly
    differ. What held still in a screendump before the frame and one after
    it is what the frame must have: a region the window manager failed to
    copy is still, and shows. Returns the share, and how much held still.
    """
    still = same = 0
    row = width * 3

    for y in range(len(after) // row):
        a, b, c = before[y * row:(y + 1) * row], after[y * row:(y + 1) * row], frame[y * row:(y + 1) * row]

        if a == b == c:
            still += width
            same += width
            continue

        for x in range(0, row, 3):
            if a[x:x + 3] == b[x:x + 3]:
                still += 1

                if c[x:x + 3] == b[x:x + 3]:
                    same += 1

    return same / float(max(1, still)), still


def where_differs(frame, width, height, px):
    """The rectangle round the pixels that differ, for a failure to name."""
    xs, ys = [], []
    row = width * 3

    for y in range(height):
        a, b = frame[y * row:(y + 1) * row], px[y * row:(y + 1) * row]

        if a != b:
            ys.append(y)

            for x in range(0, row, 3):
                if a[x:x + 3] != b[x:x + 3]:
                    xs.append(x // 3)

    if not ys:
        return "nowhere"

    return "%d,%d to %d,%d" % (min(xs), min(ys), max(xs), max(ys))


def boot(image, telnet, web, vnc=None, extra=()):
    import run_screenshot as R

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    forward = "hostfwd=tcp::%d-:23,hostfwd=tcp::%d-:8080" % (telnet, web)

    if vnc:
        forward += ",hostfwd=tcp::%d-:5900" % vnc

    setattr(R, board, saved + [
        "-netdev", "user,id=net0," + forward,
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/telnetd,string=23",
        "-fw_cfg", "name=opt/kosmos/boot,string=wm",
    ] + list(extra))

    try:
        return R.Guest(image, 120)
    finally:
        setattr(R, board, saved)


def connect(port, seconds=40):
    import kosmos_telnet as K

    deadline = time.monotonic() + seconds

    while True:
        try:
            return K.Session("127.0.0.1:%d" % port, timeout=60)
        except (ConnectionError, OSError):
            if time.monotonic() > deadline:
                raise

            time.sleep(0.5)


def look(guest, telnet, vnc, seen, fails):
    """The screen by VNC, on the boot that started it by itself."""
    import kosmos_vnc as V
    import run_screenshot as R

    address = "127.0.0.1:%d" % vnc
    guest.wait_for("wm: window", "the desktop")
    time.sleep(2)

    # No password kept: security type 1, and the display's own size.
    viewer = V.Viewer(address)
    width, height, px = R.parse_ppm(guest.screendump())

    if viewer.security != 1:
        fails.append("with no password the security type was %d, not none" % viewer.security)

    if (viewer.width, viewer.height) != (width, height):
        fails.append("the viewer was told %dx%d and the display is %dx%d"
                     % (viewer.width, viewer.height, width, height))

    # Lent to a program that says it needs the desktop, and to no other.
    session = connect(telnet)
    session.run("open /Home/reach.lua")
    session.run("open /Home/reach2.lua")

    for want, what in (("REACH plain false public false",
                        "a program that does not say it needs the desktop reached it"),
                       ("REACH lent false public false",
                        "a program on the disk was lent the desktop for saying "
                        "it needs it")):
        try:
            guest.wait_for(want, "the probe's answer")
        except Exception:                   # noqa: BLE001 - said as a failure
            fails.append(what + ": " + " | ".join(l for l in guest.seen.splitlines()
                                                  if "REACH" in l)[:200])

    # A whole frame, against what QEMU scans out and held still around it.
    _, _, before = R.parse_ppm(guest.screendump())
    viewer.request(False)
    viewer.update()
    width, height, px = R.parse_ppm(guest.screendump())
    whole, still = still_share(viewer.frame, width, before, px)
    seen["whole"] = "%.2f%% of the %.0f%% that held still" % (
        100 * whole, 100.0 * still / (width * height))

    if whole < 0.999 or still < 0.5 * width * height:
        # Both pictures kept, since the difference is the evidence - in
        # `build/`, which outlives the run as a scratch folder does not.
        os.makedirs(os.path.join(ROOT, "build", "servers"), exist_ok=True)
        kept = os.path.join(ROOT, "build", "servers", "whole-")
        V.png(kept + "viewer.png", width, height, viewer.frame)
        V.png(kept + "screen.png", width, height, px)
        fails.append("a whole frame was %s of the screen QEMU scans out, the "
                     "difference within %s (both in %s*.png)"
                     % (seen["whole"], where_differs(viewer.frame, width, height, px), kept))

    # A window opened after: it arrives as an update of what changed.
    mark = len(guest.seen)
    session.run("open calc")
    guest.wait_for("wm: window Calculator at", "the Calculator's window")
    deadline, best, partial = time.monotonic() + 40, 0.0, False

    while time.monotonic() < deadline:
        viewer.request(True)

        try:
            rects = viewer.update()
        except OSError:
            fails.append("no update came after the Calculator opened")
            break

        partial = partial or any(w * h < width * height for _, _, w, h in rects)
        width, height, px = R.parse_ppm(guest.screendump())
        best = same_share(viewer.frame, width, height, px)

        if best >= 0.995 and "Calculator" in guest.seen[mark:]:
            break

    seen["update"] = "%.2f%%" % (100 * best)

    if (best < 0.995 or not partial) and "no update came" not in " ".join(fails):
        fails.append("after the Calculator opened, the viewer's frame was %.2f%% "
                     "the screen, %s" % (100 * best, "by rectangles of what changed"
                                          if partial else "and no update was of "
                                          "less than the whole screen"))

    # A region in 565, each channel rounded as the viewer will widen it.
    viewer.set_format(16, False, 31, 63, 31, 11, 5, 0)
    viewer.request(False, 0, 0, 320, 200)
    viewer.update()
    width, height, px = R.parse_ppm(guest.screendump())
    good = 0

    for y in range(200):
        for x in range(320):
            at = (y * width + x) * 3
            want = bytes(((c * m + 127) // 255) * 255 // m
                         for c, m in zip(px[at:at + 3], (31, 63, 31)))

            if viewer.frame[at:at + 3] == want:
                good += 1

    seen["565"] = "%.2f%%" % (100 * good / 64000.0)

    if good < 0.99 * 64000:
        fails.append("a region in 565 was %.2f%% the screen" % (100 * good / 64000.0))

    # With no control kept, a viewer only looks: its click goes nowhere.
    viewer.pointer(211, 311, 1)
    viewer.pointer(211, 311, 0)
    time.sleep(1)
    guest._read_available()

    if "wm: button down at 211,311" in guest.seen:
        fails.append("a viewer with no control kept clicked the desktop")

    # The Servers window's Disconnect, to the screen.
    said = session.run("/Home/vnckick.lua").decode(errors="replace")
    viewer.sock.settimeout(10)

    try:
        closed = viewer.sock.recv(65536) == b""

        while not closed:
            closed = viewer.sock.recv(65536) == b""
    except OSError:
        closed = False

    if "VNCKICKED 1" not in said or not closed:
        fails.append("a Disconnect did not end the viewer: %r, closed %s" % (said, closed))

    # Nobody looking: the window manager lets its copy go.
    try:
        guest.wait_for("wm: the screen is no longer watched (not asked for five seconds)",
                       "let the screen's copy go")
    except Exception as e:                  # noqa: BLE001 - said as a failure
        fails.append("the window manager kept copying the screen with nobody "
                     "looking: " + str(e).splitlines()[0])

    # **Something new while nobody looks**, which then holds still: the next
    # viewer's first frame has to have it. Without this a copy the window
    # manager had let go would pass for the screen, since everything that
    # moved is left out of the comparison and nothing else had changed.
    mark = len(guest.seen)
    session.run("open about")
    guest.wait_for("wm: window About Kosmos at", "About's window")
    time.sleep(1.5)

    # A password: refused wrong, admitted right.
    said = session.run("/Home/vncpass.lua").decode(errors="replace")

    if "PASSWORD Kosmos" not in said:
        fails.append("the password was not kept: %r" % said)

    try:
        V.Viewer(address, password="wrong")
        fails.append("a wrong password was admitted")
    except V.RFBError as e:
        if "refused" not in str(e):
            fails.append("a wrong password failed oddly: %s" % e)

    try:
        viewer = V.Viewer(address, password="Kosmos")

        if viewer.security != 2:
            fails.append("with a password kept the security type was %d" % viewer.security)

        _, _, before = R.parse_ppm(guest.screendump())
        viewer.request(False)
        viewer.update()
        width, height, px = R.parse_ppm(guest.screendump())
        share, _ = still_share(viewer.frame, width, before, px)

        # Control kept: the viewer's keys, as a keyboard's, into a Terminal -
        # a command that makes a file - and its click, at its own place.
        session.run("open terminal")
        guest.wait_for("wm: window Terminal at", "the Terminal's window")
        time.sleep(2)
        viewer.type_text("touch /Home/typed-by-a-viewer\n")
        deadline = time.monotonic() + 30
        listing = ""

        while time.monotonic() < deadline and "typed-by-a-viewer" not in listing:
            time.sleep(1)
            listing = session.run("ls /Home").decode(errors="replace")

        if "typed-by-a-viewer" not in listing:
            fails.append("keys typed by a viewer did not run a command in the "
                         "Terminal: %r" % listing[-300:])

        viewer.pointer(200, 300, 0)
        viewer.pointer(200, 300, 1)
        viewer.pointer(200, 300, 0)

        try:
            guest.wait_for("wm: button down at 200,300", "a viewer's click")
        except Exception:                   # noqa: BLE001 - said as a failure
            fails.append("a viewer's click did not reach the desktop at 200,300")

        # **And the pointer stays where the viewer left it**, with the mouse
        # lying still: the cursor drawn at a place, then gone from there when
        # moved away, then back. A mouse at rest that took the pointer back
        # each pass would leave that corner the same all three times. The
        # place is the bare desk near the bottom right, above the version
        # line and below where any window here opens - a corner a Terminal
        # covers blinks by itself, which on x86 it did.
        cx, cy = viewer.width - 80, viewer.height - 120

        def corner():
            viewer.request(False, cx - 10, cy - 10, 40, 40)
            viewer.update()
            return b"".join(bytes(viewer.frame[((y * viewer.width) + cx - 10) * 3:
                                              ((y * viewer.width) + cx + 30) * 3])
                            for y in range(cy - 10, cy + 30))

        # Each look repeated until the corner is what it should become, for
        # at most three seconds - so a step costs what the cursor takes to
        # move, and only a pointer that never arrives waits the whole time.
        # The corner bare, then with the cursor, bare again when it has gone,
        # and with it again when it is back.
        def until(settled):
            deadline = time.monotonic() + 3
            crop = corner()

            while not settled(crop) and time.monotonic() < deadline:
                time.sleep(0.1)
                crop = corner()

            return crop, settled(crop)

        bare = corner()
        steps = []
        viewer.pointer(cx, cy, 0)
        cursor, ok = until(lambda c: c != bare)
        steps.append(("the cursor arrived", ok))
        viewer.pointer(cx - 300, cy, 0)
        steps.append(("the cursor left", until(lambda c: c == bare)[1]))
        viewer.pointer(cx, cy, 0)
        steps.append(("the cursor came back", until(lambda c: c == cursor)[1]))
        missed = [what for what, ok in steps if not ok]

        if missed:
            fails.append("the pointer did not stay where the viewer put it - "
                         "at %d,%d the corner never showed that " % (cx, cy)
                         + ", ".join(missed))

        if share < 0.999:
            os.makedirs(os.path.join(ROOT, "build", "servers"), exist_ok=True)
            kept = os.path.join(ROOT, "build", "servers", "password-")
            V.png(kept + "viewer.png", width, height, viewer.frame)
            V.png(kept + "screen.png", width, height, px)
            fails.append("the frame after the password was %.2f%% the screen "
                         "that held still, the difference within %s (both in %s*.png)"
                         % (100 * share, where_differs(viewer.frame, width, height, px), kept))

        viewer.close()
    except V.RFBError as e:
        fails.append("the right password was not admitted: %s" % e)

    session.close()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("servers")
    disk = os.path.join(work, "disk.img")
    pairs = []

    with open(os.path.join(work, "index.html"), "w") as f:
        f.write(PAGE)

    pairs.append(os.path.join(work, "index.html") + ":/Home/www/index.html")

    for name, source in PROBES.items():
        with open(os.path.join(work, name), "w") as f:
            f.write(source)

        pairs.append(os.path.join(work, name) + ":/Home/" + name)

    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32", *pairs],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    fails = []
    said = {}

    # ---- 1: the window, the Disconnect, and what is kept ----
    # With the screen's keys and pointer lent by the command line, as the
    # M700's network boot lends them (`opt/kosmos/vnc=control`): nothing kept
    # says so on this disk, so a click that arrives is the word's alone.
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    lent = random.randint(60001, 64000)
    guest = boot(image, telnet, web, lent,
                 extra=("-fw_cfg", "name=opt/kosmos/vnc,string=control"))

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("wm: window", "the desktop")

        session = connect(telnet)
        said["help"] = session.run("help").decode(errors="replace")

        # An application written here, pushed and opened on this desktop -
        # `open` asking the window manager, as the Deskbar does. Moved here
        # from `run_telnetd.py`'s boot of its own, which was this boot.
        import contextlib
        import io
        import kosmos_telnet as K

        app = os.path.join(work, "hellowin.lua")

        with open(os.path.join(ROOT, "user", "bin", "apps", "hello-win.lua")) as f:
            source = f.read()

        with open(app, "w") as f:
            f.write(source)

        heard = io.StringIO()

        with contextlib.redirect_stdout(heard):
            K.push(session, app)

        said["push"] = heard.getvalue()
        guest.wait_for("wm: launched /Home/Apps/hellowin/hellowin.lua -> true",
                       "the pushed application launched")

        said["open"] = session.run("open servers").decode(errors="replace")
        guest.wait_for("servers: web=", "the Servers window's first look")
        guest.wait_for("telnet=running · 1 session", "the window seeing this session")
        said["keep"] = session.run("/Home/keep.lua").decode(errors="replace")

        said["vncd"] = session.run("open vncd").decode(errors="replace")
        guest.wait_for("vncd: keys and the pointer lent to every viewer, as "
                       "opt/kosmos/vnc asks", "the boot's word taken")
        import kosmos_vnc as V
        import run_screenshot as R

        viewer = V.Viewer("127.0.0.1:%d" % lent)
        viewer.pointer(233, 333)
        viewer.pointer(233, 333, 1)
        viewer.pointer(233, 333, 0)
        guest.wait_for("wm: button down at 233,333", "a click lent by the boot")

        # **A window that draws its own pixels, granted less than it asked**
        # (`testing.md` 18.355): 4000 wide on any screen, so the window
        # manager gives it the screen less a border. Its buffers must be
        # made again at that size and read at it - the stripes it draws stay
        # upright - where Groove came out in diagonal bands on the M700. Each
        # column its own colour: stripes that repeat let a shear of a whole
        # number of them pass, which the first version's did.
        session.put(STRIPES.encode(), "/Temporary/stripes.lua")
        session.run("open /Temporary/stripes.lua")
        said["stripes"] = guest.wait_for_line("STRIPES ", "the probe's size")
        placed = guest.wait_for_line("wm: window Stripes at ", "the probe's window")
        width, height, px = R.parse_ppm(guest.screendump())
        said["upright"] = upright(width, px, placed)

        # **Groove resized by its grip** (Diego, 3 October: "groove needs to
        # be resizable", "like we did with the browser"): opened at 700 by
        # 500, the bottom right corner dragged 120 right and 80 down, and
        # Groove lays itself out at the size the kit's new buffers are.
        session.run("open groove --size 700x500")
        line = guest.wait_for_line("wm: window Groove at ", "Groove's window")
        m = re.match(r"(\d+),(\d+) (\d+)x(\d+)", line)
        gx, gy = int(m.group(1)) + int(m.group(3)) - 6, int(m.group(2)) + int(m.group(4)) - 6
        time.sleep(3)
        V.step(viewer, "drag %d %d %d %d" % (gx, gy, gx + 120, gy + 80))
        said["groove"] = guest.wait_for_line("groove: resized to ", "Groove laid out again")
        viewer.close()

        # The Disconnect ends this very session, so it is read to its close.
        session.sock.sendall(b"/Home/kick.lua\r\n")
        closed = b""
        session.sock.settimeout(20)

        try:
            while True:
                piece = session.sock.recv(4096)

                if not piece:
                    break

                closed += piece
        except OSError:
            pass

        said["kick"] = (session.buffer + closed).decode(errors="replace")
        time.sleep(1)
        guest._read_available()
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the first boot stopped: %s: %s" % (type(e).__name__, e))
    finally:
        guest.close()

    first = guest.seen

    if "open: started" not in said.get("push", ""):
        fails.append("an application pushed to a machine running its desktop "
                     "did not open there: %r" % said.get("push"))

    if "this session's own:" not in said.get("help", ""):
        fails.append("help did not list the session's own words: %r" % said.get("help"))

    if "open: started servers" not in said.get("open", ""):
        fails.append("open servers did not start the window: %r" % said.get("open"))

    if "vnc=stopped" not in first or "web=stopped" not in first:
        fails.append("the window did not say the web server and the screen "
                     "stopped:\n" + "\n".join(l for l in first.splitlines()
                                                if "servers:" in l)[:600])

    stripes = re.match(r"(\d+)x(\d+), granted (\d+)x(\d+)", said.get("stripes", ""))

    if not stripes or stripes.group(1, 2) != stripes.group(3, 4):
        fails.append("a direct window granted less than it asked drew into "
                     "buffers of another size: %r" % said.get("stripes"))

    wrong, looked = said.get("upright", (None, 0))

    if wrong is None or wrong > 0:
        fails.append("a direct window granted less than it asked was not on "
                     "the screen as it drew it: %s of %d places the wrong "
                     "colour" % (wrong, looked))

    resized = re.match(r"(\d+)x(\d+)", said.get("groove", ""))

    if not resized or int(resized.group(1)) <= 700 or int(resized.group(2)) <= 500:
        fails.append("Groove dragged larger by its grip did not lay itself out "
                     "larger: %r" % said.get("groove"))

    if "wm: button down at 233,333" not in first:
        fails.append("with opt/kosmos/vnc=control and no control kept, a "
                     "viewer's click did not reach the desktop:\n"
                     + "\n".join(l for l in first.splitlines() if "vncd" in l)[:600])

    if "KEPT 8080" not in said.get("keep", ""):
        fails.append("the settings were not kept: %r" % said.get("keep"))

    if "disconnected from this machine" not in said.get("kick", ""):
        fails.append("a Disconnect did not end the session: %r" % said.get("kick", "")[-300:])

    # ---- 2: the same disk again, the web server and the screen by themselves ----
    telnet, vnc = random.randint(20000, 40000), random.randint(60001, 64000)
    guest = boot(image, telnet, web, vnc)
    fetched = {}
    seen = {}

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("starting httpd 8080 /Home/www", "the web server started at boot")
        guest.wait_for("vncd: on port 5900", "the screen's server started at boot")

        for _ in range(20):
            try:
                c = http.client.HTTPConnection("127.0.0.1", web, timeout=10)
                c.request("GET", "/")
                r = c.getresponse()
                fetched = {"code": r.status, "body": r.read().decode(errors="replace")}
                c.close()
                break
            except Exception as e:          # noqa: BLE001 - any means retry
                fetched = {"error": repr(e)}
                time.sleep(1)

        look(guest, telnet, vnc, seen, fails)
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the second boot stopped: %s: %s" % (type(e).__name__, e))
    finally:
        guest.close()

    if fetched.get("code") != 200 or PAGE.strip() not in fetched.get("body", ""):
        fails.append("the web server kept to start with the machine did not "
                     "serve its page: %r" % fetched)

    for out in (first, guest.seen):
        if " died: " in out:
            fails.append("something died: " + out[out.find(" died: ") - 80:][:400])

    for line in ("wm: lending the desktop to vncd", "wm: the screen is watched",
                 "vncd: 10.0.2.2  connected"):
        if line not in guest.seen:
            fails.append("the log never said %r" % line)

    checks = 27

    if fails:
        print("FAIL: %d of %d checks on the Servers window:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on the Servers window, on the M700's own boot and "
          "from the Mac's clients (help; an application pushed from here and "
          "opened on the desktop; the window opened by open; the "
          "command line running with this session, the web server and the "
          "screen stopped; the settings kept; a Disconnect ending a session; "
          "the web server kept to start with the machine serving its page "
          "after a boot; and the screen by VNC started with it - its size, a "
          "whole frame as QEMU scans it out (%s), a window opened after "
          "arriving as an update (%s), 565 (%s), a click that went nowhere "
          "with no control kept, a Disconnect, the copy let go, a password "
          "refused wrong and admitted right, and with control kept a command "
          "typed into a Terminal, a click at its own place and the pointer "
          "staying there with the mouse at rest; the desktop "
          "lent to vncd and to nothing else; a click lent by "
          "opt/kosmos/vnc=control alone; a direct window granted less than "
          "it asked drawing upright at the granted size; and Groove resized "
          "by its grip)."
          % (checks, seen.get("whole"), seen.get("update"), seen.get("565")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
