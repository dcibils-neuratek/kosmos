#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Quake III Arena, started (`docs/quake3e.md` Q2-Q3).

The application as a person runs it: `quake3.lua` on `quake3.elf` -
Quake3e, its Vulkan renderer and the platform under it, with lavapipe -
installed in `/Home/Apps/Quake3`, opened on the desktop over Telnet as the
Deskbar opens it, and its console lines read:

- the engine starts, finds its game and runs its configuration;
- the renderer opens a Vulkan window on the Window Kit's surface
  (`VK_KHR_kosmos_surface`) and says which device it drew with;
- frames keep coming, and the window holds a picture that is not one
  colour.

**Which game**: with `--demo PAK`, the Quake III demo's own pak - which
is not in the tree, so the suite in the gate does without it - and
otherwise a pak this script makes, holding only a `default.cfg`: enough for the engine to believe it has a game, start the
renderer and draw its console over a black screen, which is the whole of
what the platform layer is responsible for.

- the user interface is the native one compiled into the image (Q3), not
  a QVM: `VM_LoadDll(ui) succeeded!`.

`--qvm` is the control: the user interface asked for as a QVM, which the
made pak does not carry, so nothing native is loaded, the engine says it
could not make the user interface, and the suite must fail.

`--map NAME` starts that map (with the demo's pak), for a picture.

Usage: run_quake3.py [--qvm] [--demo PAK] [--map NAME] [--shot PNG] IMAGE [APPS]
"""

import io
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402


def option(name):
    if name in sys.argv:
        i = sys.argv.index(name)
        value = sys.argv[i + 1]
        del sys.argv[i:i + 2]
        return value
    return None


DEMO = option("--demo")
SHOT = option("--shot")
MAP = option("--map")
QVM = "--qvm" in sys.argv
sys.argv = [a for a in sys.argv if a != "--qvm"]
IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "user-x86_64-test", "apps")
WORK = scratch.directory("quake3")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
FOLDER = "/Home/Apps/Quake3"
KEPT = os.path.join(ROOT, "build", "quake3-failed")


def made_pak():
    """A pak with a configuration and nothing else: a game, as far as the
    engine's file system can tell."""
    path = os.path.join(WORK, "pak0.pk3")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("default.cfg", "// Kosmos's suite: no game, only this\n"
                                  "bind ` toggleconsole\n")
    return path


def colours(ppm, x, y, w, h):
    """How many colours a rectangle of a screendump holds, sampled."""
    import run_screenshot as R                               # noqa: E402
    width, height, px = R.parse_ppm(ppm)
    seen = set()
    for row in range(y, min(y + h, height), 3):
        start = (row * width + x) * 3
        for col in range(0, min(w, width - x), 3):
            seen.add(px[start + col * 3:start + col * 3 + 3])
    return len(seen)


def main():
    built = os.path.join(APPS, "quake3.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1
    image = os.path.join(WORK, "quake3.elf")
    subprocess.run(["x86_64-elf-objcopy", "--strip-debug", built, image], check=True)

    pak = DEMO or made_pak()
    # What the run is told, in `quake3.args` beside `quake3.lua`, after
    # its defaults (`open` hands a program one word): small, so TCG draws
    # it quickly, and the demo's menu at a size a person can read.
    w, h = (640, 480) if DEMO else (320, 240)
    said = "+set r_customwidth %d +set r_customheight %d" % (w, h)
    if QVM:
        said += " +set vm_ui 1"
    if MAP:
        said += " +map " + MAP
    args = os.path.join(WORK, "quake3.args")
    with open(args, "w") as f:
        f.write(said)
    files = [(args, FOLDER + "/quake3.args"),
             (image, FOLDER + "/quake3.elf"),
             (os.path.join(ROOT, "user", "installed", "Quake3", "quake3.lua"), FOLDER + "/quake3.lua"),
             (pak, FOLDER + "/demoq3/pak0.pk3")]
    megabytes = (os.path.getsize(image) + os.path.getsize(pak)) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK, str(megabytes + 80)]
                   + ["%s:%s" % f for f in files], check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import random
    import run_screenshot as R                               # noqa: E402
    import run_servers as SV                                 # noqa: E402

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    i = R.X86_ARGS.index("-m")
    R.X86_ARGS = R.X86_ARGS[:i + 1] + ["1536M"] + R.X86_ARGS[i + 2:]
    if "-smp" not in R.X86_ARGS:
        R.X86_ARGS = R.X86_ARGS + ["-smp", "2"]
    guest = SV.boot(IMAGE, telnet, web)
    fails = []
    mark = 0
    try:
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = SV.connect(telnet)
        mark = len(guest.seen)
        session.run("open " + FOLDER + "/quake3.lua")
        guest.wait_for("quake3: " + FOLDER, "quake3.lua started")

        def seen():
            guest._read_available()
            return guest.seen[mark:]

        until = time.monotonic() + 400
        while time.monotonic() < until:
            s = seen()
            if re.search(r"quake3: (quit|error|.*Error)", s) or "Sys_Error" in s:
                break
            if "finished R_Init" in s:
                break
            time.sleep(0.5)
        # Frames after the renderer, then the picture.
        time.sleep(90 if MAP else 30 if DEMO else 10)
        s = seen()
        rgb = guest.screendump()
        if SHOT:
            R.write_png(SHOT, rgb)
            with open(os.path.splitext(SHOT)[0] + ".txt", "w") as f:
                f.write(s)

        found = re.search(r"wm: window Quake III Arena at (\d+),(\d+) (\d+)x(\d+)", s)
        window = tuple(int(v) for v in found.groups()) if found else None
        checks = [
            ("the engine started", "Q3 1.32e kosmos-x86_64" in s),
            ("it found its game", "files in 1 pk3 files" in s),
            ("the renderer opened Vulkan", "VK_RENDERER:" in s and "finished R_Init" in s),
            ("the user interface is native", "VM_LoadDll(ui) succeeded!" in s),
            ("it did not stop", not re.search(r"quake3: (quit|error)|^ERROR:", s, re.M)),
            # The window's picture - its last `w` by `h` below the title
            # bar, which the window manager draws whatever the game does.
            ("the game's window holds a picture", window is not None
             and colours(rgb, window[0], window[1] + window[3] - h, window[2], h) > 8),
        ]
        for name, ok in checks:
            print(("ok   " if ok else "FAIL ") + name)
            if not ok:
                fails.append(name)
        if fails:
            os.makedirs(KEPT, exist_ok=True)
            with open(os.path.join(KEPT, "console.txt"), "w") as f:
                f.write(s)
            print("console kept in", KEPT)
            print(s[-3000:])
    except R.Failure as oops:
        fails.append(str(oops))
        print("FAIL", oops)
        print(guest.seen[mark:][-3000:])
    finally:
        guest.close()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
