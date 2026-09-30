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
     line is running with the one session that opened it, the web server
     stopped, and the screen not built; `telnetd`'s name in `/Running`
     answers a Disconnect by ending the session asked about; and the web
     server is set to start with the machine on port 8080, as the window
     keeps it, in `/Home/Preferences/servers`.
  2. The same disk booted again: the web server started by itself, and a
     page fetched from it by this Mac.

Usage: run_servers.py IMAGE
"""

import http.client
import os
import random
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
                 'folder = "/Home/www", at_start = true } })\n'
                 'print("KEPT " .. tostring(fs.read("/Home/Preferences/servers").web.port))\n'),
    # The window's Disconnect, as it sends it.
    "kick.lua": ('local r = fs.send("/Running/telnetd", { type = "disconnect", from = "10.0.2.2" })\n'
                 'print("KICKED " .. tostring(r and r.ended))\n'),
}


def boot(image, telnet, web):
    import run_screenshot as R

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", "user,id=net0,hostfwd=tcp::%d-:23,hostfwd=tcp::%d-:8080" % (telnet, web),
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/telnetd,string=23",
        "-fw_cfg", "name=opt/kosmos/boot,string=wm",
    ])

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
    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = boot(image, telnet, web)

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("wm: window", "the desktop")

        session = connect(telnet)
        said["help"] = session.run("help").decode(errors="replace")
        said["open"] = session.run("open servers").decode(errors="replace")
        guest.wait_for("servers: web=", "the Servers window's first look")
        guest.wait_for("telnet=running · 1 session", "the window seeing this session")
        said["keep"] = session.run("/Home/keep.lua").decode(errors="replace")

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

    if "this session's own:" not in said.get("help", ""):
        fails.append("help did not list the session's own words: %r" % said.get("help"))

    if "open: started servers" not in said.get("open", ""):
        fails.append("open servers did not start the window: %r" % said.get("open"))

    if "vnc=not built yet" not in first or "web=stopped" not in first:
        fails.append("the window did not say the web server stopped and the "
                     "screen not built:\n" + "\n".join(l for l in first.splitlines()
                                                       if "servers:" in l)[:600])

    if "KEPT 8080" not in said.get("keep", ""):
        fails.append("the settings were not kept: %r" % said.get("keep"))

    if "disconnected from this machine" not in said.get("kick", ""):
        fails.append("a Disconnect did not end the session: %r" % said.get("kick", "")[-300:])

    # ---- 2: the same disk again, the web server started by itself ----
    guest = boot(image, random.randint(20000, 40000), web)
    fetched = {}

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("starting httpd 8080 /Home/www", "the web server started at boot")

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

    checks = 7

    if fails:
        print("FAIL: %d of %d checks on the Servers window:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on the Servers window, on the M700's own boot and "
          "from the Mac's client (help; the window opened by open; the "
          "command line running with this session, the web server stopped, "
          "the screen not built; the settings kept; a Disconnect ending a "
          "session; and the web server kept to start with the machine "
          "serving its page after a boot)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
