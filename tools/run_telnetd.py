#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Mac runs commands on a Kosmos machine, by Telnet, and fetches a file.

`telnetd` on the guest (`user/bin/programs/telnetd.lua`) and the Mac's own
client, `tools/kosmos_telnet.py` - the one Diego and this session use -
talking through a port QEMU forwards from this computer, as `run_network.py`
reaches `httpd` (`roadmap.md`, remote; Diego, 29 September: "Can we just
implement a Telnet server and client?").

Each check is something the M700 will be asked to do:

  - the banner and a prompt, which is how the client knows it is Kosmos;
  - a program's output, back;
  - `cd` and `pwd`, the session's own;
  - a file with every byte value in it, whole - `get`, through base64;
  - a program that reads a line, given the next one typed;
  - a program that fails, and its exit code said;
  - Control-C, as Telnet's interrupt, reaching a program that asks;
  - a name that is no program, said so.

Usage: run_telnetd.py IMAGE
"""

import os
import random
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

BLOB = bytes(range(256)) * 300          # every byte, 255 among them

PROBES = {
    "ask.lua": 'print("ASK?")\nprint("GOT " .. tostring(fs.read("/Devices/console")))\n',
    "fail.lua": 'print("FAILING")\nerror("on purpose")\n',
    "spin.lua": ('print("SPINNING")\n'
                 'while not interrupted() do sys.sleep(1) end\n'
                 'print("STOPPED")\n'),
}

results = {}
WORK = None


def talk(port):
    """Everything asked of the guest, through the real client."""
    import kosmos_telnet as K

    # Started by the shell before its first prompt; listening a moment
    # later, and QEMU's forward closes on us until it is.
    deadline = time.monotonic() + 30

    while True:
        try:
            session = K.Session("127.0.0.1:%d" % port, timeout=60)
            break
        except (ConnectionError, OSError):
            if time.monotonic() > deadline:
                raise

            time.sleep(0.5)

    results["banner"] = True

    results["hello"] = session.run("hello").decode(errors="replace")
    results["pwd"] = session.run("pwd").decode().strip()
    session.run("cd /Kosmos")
    results["cd"] = session.run("pwd").decode().strip()

    try:
        results["blob"] = session.get("/Home/blob.bin")
    except Exception as e:                  # noqa: BLE001 - said below
        results["blob_error"] = str(e)

    # A program that reads: the line typed while it waits is its.
    session.sock.sendall(b"/Home/ask.lua\r\n")
    wait_for(session, b"ASK?")
    session.sock.sendall(b"banana\r\n")
    results["ask"] = session.until_prompt().decode(errors="replace")

    results["fail"] = session.run("/Home/fail.lua").decode(errors="replace")

    # Control-C as Telnet sends it: IAC IP.
    session.sock.sendall(b"/Home/spin.lua\r\n")
    wait_for(session, b"SPINNING")
    session.sock.sendall(bytes([255, 244]))
    results["spin"] = session.until_prompt().decode(errors="replace")

    results["nothing"] = session.run("no-such-thing").decode(errors="replace")

    # **Sent the other way**: every byte value, into folders that are not
    # there yet, and read back (`put`, Diego: "write Lua apps in the Mac and
    # push them to the m700").
    try:
        session.put(bytes(reversed(BLOB)), "/Home/up/deep/up.bin")
        results["up"] = session.get("/Home/up/deep/up.bin")
    except Exception as e:                  # noqa: BLE001 - said below
        results["up_error"] = str(e)

    # A program written here, pushed, and run there: its output comes back.
    import contextlib
    import io

    pushed = os.path.join(WORK, "pushed.lua")

    with open(pushed, "w") as f:
        f.write('print("PUSHED " .. 6 * 7)\n')

    heard = io.StringIO()

    with contextlib.redirect_stdout(heard):
        results["push_code"] = K.push(session, pushed)

    results["push"] = heard.getvalue()

    # And `open` with no desktop says so, and fails.
    results["open"] = session.run("open tracker").decode(errors="replace")
    session.close()


def on_the_desktop(image, port):
    """**The M700's own boot**: the desktop by itself and `telnetd` beside
    it. An application written here is pushed and opened on its screen -
    `open` asking the window manager, as the Deskbar does."""
    import kosmos_telnet as K
    import run_screenshot as R
    import contextlib
    import io

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", "user,id=net0,hostfwd=tcp::%d-:23" % port,
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/telnetd,string=23",
        "-fw_cfg", "name=opt/kosmos/boot,string=wm",
    ])

    try:
        guest = R.Guest(image, 120)
    finally:
        setattr(R, board, saved)

    app = os.path.join(WORK, "hellowin.lua")

    with open(os.path.join(ROOT, "user", "bin", "apps", "hello-win.lua")) as f:
        source = f.read()

    with open(app, "w") as f:
        f.write(source)

    try:
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.wait_for("wm: window", "the desktop's first window")

        deadline = time.monotonic() + 30

        while True:
            try:
                session = K.Session("127.0.0.1:%d" % port, timeout=60)
                break
            except (ConnectionError, OSError):
                if time.monotonic() > deadline:
                    raise

                time.sleep(0.5)

        heard = io.StringIO()

        with contextlib.redirect_stdout(heard):
            K.push(session, app)

        results["desktop_push"] = heard.getvalue()
        session.close()
        guest.wait_for("wm: launched /Home/Apps/hellowin/hellowin.lua -> true",
                       "the pushed application launched")
        results["desktop"] = True
    except Exception as e:                  # noqa: BLE001 - said below
        results["desktop_error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        guest.close()


def wait_for(session, text, seconds=30):
    deadline = time.monotonic() + seconds

    while text not in session.buffer and time.monotonic() < deadline:
        session.sock.settimeout(1)

        try:
            session.buffer += session.sock.recv(4096)
        except socket.timeout:
            pass


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    global WORK

    work = scratch.directory("telnetd")
    WORK = work
    disk = os.path.join(work, "disk.img")
    pairs = []

    with open(os.path.join(work, "blob.bin"), "wb") as f:
        f.write(BLOB)

    pairs.append(os.path.join(work, "blob.bin") + ":/Home/blob.bin")

    for name, source in PROBES.items():
        with open(os.path.join(work, name), "w") as f:
            f.write(source)

        pairs.append(os.path.join(work, name) + ":/Home/" + name)

    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32", *pairs],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    import run_network                                      # noqa: E402
    import run_screenshot                                   # noqa: E402

    port = random.randint(20000, 60000)
    fails = []

    def then():
        try:
            talk(port)
        except Exception as e:              # noqa: BLE001 - said below
            results["error"] = "%s: %s" % (type(e).__name__, e)

    #
    # **Started as a development stick starts it**: `opt/kosmos/telnetd`, by
    # the shell before its first prompt - not typed - so the path the M700
    # takes is the one held here.
    #
    out = run_network.boot(image, [
        "-netdev", "user,id=net0,hostfwd=tcp::%d-:23" % port,
        "-device", run_screenshot.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/telnetd,string=23",
    ], [], seconds=120, then=then)


    if "error" in results:
        fails.append("the client stopped: " + results["error"])

    if not results.get("banner"):
        fails.append("no banner and prompt:\n" + out[-800:])

    if "Hello from a process of my own." not in results.get("hello", ""):
        fails.append("hello's output did not come back: %r" % results.get("hello"))

    if results.get("pwd") != "/Home" or results.get("cd") != "/Kosmos":
        fails.append("the session's place: pwd %r, after cd /Kosmos %r"
                     % (results.get("pwd"), results.get("cd")))

    blob = results.get("blob")

    if blob != BLOB:
        fails.append("the file did not come back as it was: %s" % (
            results.get("blob_error")
            or "%d bytes of %d, the first difference at byte %d" % (
                len(blob or b""), len(BLOB),
                next((i for i, (a, b) in enumerate(zip(blob or b"", BLOB)) if a != b),
                     min(len(blob or b""), len(BLOB))))))

    if "GOT banana" not in results.get("ask", ""):
        fails.append("a program's read did not get the line typed: %r" % results.get("ask"))

    if "FAILING" not in results.get("fail", "") or "(exit code" not in results.get("fail", ""):
        fails.append("a failing program's exit code was not said: %r" % results.get("fail"))

    if "STOPPED" not in results.get("spin", ""):
        fails.append("Control-C did not reach the program: %r" % results.get("spin"))

    if "no such program" not in results.get("nothing", ""):
        fails.append("a name that is no program: %r" % results.get("nothing"))

    on_the_desktop(image, random.randint(20000, 60000))

    if not results.get("desktop") or "open: started" not in results.get("desktop_push", ""):
        fails.append("an application pushed to a machine running its desktop "
                     "did not open there: %s %r" % (results.get("desktop_error", ""),
                                                     results.get("desktop_push")))

    if results.get("up") != bytes(reversed(BLOB)):
        fails.append("a file put there did not come back as it was: %s"
                     % (results.get("up_error") or "%d bytes" % len(results.get("up") or b"")))

    if ("PUSHED 42" not in results.get("push", "")
            or "/Home/Apps/pushed/pushed.lua, " not in results.get("push", "")):
        fails.append("a program pushed from here did not land and run: %r"
                     % results.get("push"))

    if ("the desktop is not running" not in results.get("open", "")
            or "(exit code" not in results.get("open", "")):
        fails.append("open with no desktop did not say so and fail: %r"
                     % results.get("open"))

    if " died: " in out:
        fails.append("something died: " + out[out.find(" died: ") - 80:][:400])

    checks = 13

    if fails:
        print("FAIL: %d of %d checks on telnetd:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on telnetd, through the Mac's own client (the banner "
          "and a prompt; a program's output; cd and pwd; %d bytes with every "
          "value in them, whole; a read given the next line typed; a failure's "
          "exit code; Control-C as Telnet's interrupt; no such program; a file "
          "put into new folders and back; a program pushed and run; open with "
          "no desktop refused; and one pushed to a desktop, opened there; "
          "nothing dead)." % (checks, len(BLOB)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
