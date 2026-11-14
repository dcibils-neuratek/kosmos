#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W7b's pages from the network (`docs/w7.md`), `x86-sites`.

`webkit2test.elf` - W7a's UI process with no window - is given addresses
rather than pages. The network process fetches each page, and every font
and picture it refers to, through WebKit's own loader: `NetworkDataTaskCurl`,
over the POSIX layer's sockets on the Network Kit, from a server on this
Mac that QEMU's network shows the guest at 10.0.2.2.

The pages are WebKit's own reference tests of WOFF2 and AVIF, which
`x86-webcore` paints from files: each page beside its `-expected.html`,
the two held pixel for pixel. A page here passes only if the font or the
picture it names arrived over the network, was decoded, and painted as its
reference says - since the reference draws the same thing without it.

The control: `KOSMOS_SITES_BREAK=missing` has the server answer 404 for
the first page's font: that page then differs from its reference, FAIL.

Usage: run_sites.py IMAGE [APPS]
"""

import functools
import http.server
import os
import shutil
import socketserver
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import pngread                                               # noqa: E402
import upstream                                              # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("sites")
SERVED = os.path.join(WORK, "served")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
BROKEN = os.environ.get("KOSMOS_SITES_BREAK")

TESTS = upstream.WEBKIT_REF_TESTS
# The references' own files, and Ahem as a TrueType font: what
# `woff2-expected.html` sets its box in, from the LayoutTests root.
FILES = upstream.WEBKIT_REF_FILES + ["resources/Ahem.ttf"]
# What the control takes away: the first page's font.
MISSING = "/fast/text/resources/ahem.woff2"

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W7b's pages from the network (`tools/run_sites.py`).
-- kosmos: image webkit2test.elf
local kit = use("webkit2test.elf")
local folder = "/Home/apps/sites"
for _, load in ipairs({%s}) do
  local name, url = load[1], load[2]
  local surface = gfx.surface{ w = 800, h = 600 }
  print("load " .. name)
  for _, line in ipairs(kit.load(url, surface, 120000, "url")) do
    print("said " .. line)
  end
  local ok, why = fs.write(folder .. "/pictures/" .. name .. ".png", gfx.encode_png(surface))
  print(ok and ("saved " .. name) or ("unsaved " .. name .. ": " .. tostring(why)))
end
print("sites: done")
"""


class Quiet(http.server.SimpleHTTPRequestHandler):
    """The served folder, every request remembered, and the control's 404."""
    asked = []

    def do_GET(self):
        Quiet.asked.append(self.path)
        if BROKEN == "missing" and self.path == MISSING:
            self.send_error(404)
            return
        super().do_GET()

    def log_message(self, *args):
        pass


def serve():
    """A plain HTTP server on this Mac for the served folder; its port."""
    handler = functools.partial(Quiet, directory=SERVED)
    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


def loads(port):
    """Each load as (its picture's name, the address)."""
    out = []
    for t in TESTS:
        stem = t.rsplit(".", 1)[0]
        name = stem.rsplit("/", 1)[1]
        out.append((name, "http://10.0.2.2:%d/%s.html" % (port, stem)))
        out.append((name + "-expected", "http://10.0.2.2:%d/%s-expected.html" % (port, stem)))
    return out


def differs(a, b):
    """Two pictures: why they differ, or None."""
    if not (os.path.exists(a) and os.path.exists(b)):
        return "no picture"
    w, h, have = pngread.pixels(a)
    ew, eh, want = pngread.pixels(b)
    if (w, h) != (ew, eh):
        return "%dx%d against %dx%d" % (w, h, ew, eh)
    wrong = [(x, y) for y in range(h) if have[y] != want[y]
             for x in range(w) if have[y][x * 3:x * 3 + 3] != want[y][x * 3:x * 3 + 3]]
    if wrong:
        x, y = wrong[0]
        return "%d of %d pixels differ, the first (%d,%d)" % (len(wrong), w * h, x, y)
    return None


def main():
    built = os.path.join(APPS, "webkit2test.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1
    tests = os.path.join(upstream.webkit_tests(), "LayoutTests")
    shutil.rmtree(SERVED, ignore_errors=True)
    for t in TESTS:
        stem = t.rsplit(".", 1)[0]
        for f in (t, stem + "-expected.html"):
            os.makedirs(os.path.join(SERVED, os.path.dirname(f)), exist_ok=True)
            shutil.copyfile(os.path.join(tests, f), os.path.join(SERVED, f))
    for f in FILES:
        os.makedirs(os.path.join(SERVED, os.path.dirname(f)), exist_ok=True)
        shutil.copyfile(os.path.join(tests, f), os.path.join(SERVED, f))

    server, port = serve()
    planned = loads(port)
    image = os.path.join(WORK, "webkit2test.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "sites.lua"), "w") as f:
        f.write(PROGRAM % ", ".join('{"%s", "%s"}' % p for p in planned))
    readme = os.path.join(WORK, "README")
    with open(readme, "w") as f:
        f.write("W7b: each page as it came from the network and was painted.\n")
    files = [(os.path.join(WORK, "sites.lua"), "/Home/apps/sites/sites.lua"),
             (image, "/Home/apps/sites/webkit2test.elf"),
             (readme, "/Home/apps/sites/pictures/README")]
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402 - after the disk
    board = "X86_ARGS" if R.machine(IMAGE) == "x86_64" else "QEMU_ARGS"
    setattr(R, board, getattr(R, board) + [
        "-netdev", "user,id=net0", "-device", R.device(IMAGE, "net") + ",netdev=net0"])

    guest = R.Guest(IMAGE, 400)
    said, done, mark = [], False, 0
    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/sites/sites.lua")
        while time.monotonic() < started + 360 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines
                    if ln.startswith(("load ", "said ", "sites:", "saved ", "unsaved "))]
            done = "sites: done" in said
    finally:
        guest.close()
        server.shutdown()

    if os.environ.get("KOSMOS_SITES_SERIAL"):
        with open(os.environ["KOSMOS_SITES_SERIAL"], "w") as f:
            f.write(guest.seen)
    if not done:
        print("FAIL: the pages did not finish\n--- what the server was asked ---\n"
              + "\n".join(Quiet.asked) + "\n--- the guest's last words ---\n"
              + guest.seen[mark:][-3000:])
        return 1

    pictures = os.path.join(ROOT, "build", "sites", "pictures")
    shutil.rmtree(pictures, ignore_errors=True)
    os.makedirs(pictures)
    for name in [ln[len("saved "):] for ln in said if ln.startswith("saved ")]:
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK,
                        "/Home/apps/sites/pictures/" + name + ".png",
                        os.path.join(pictures, name + ".png")],
                       check=True, capture_output=True, cwd=ROOT)

    told, current = {}, None
    for ln in said:
        if ln.startswith("load "):
            current = ln[5:]
            told[current] = []
        elif ln.startswith("said ") and current:
            told[current].append(ln[5:])
    failed = []
    for name, url in planned:
        lines = told.get(name, [])
        print("%s: %s" % (name, "; ".join(lines) or "nothing said"))
        if "painted" not in lines:
            failed.append("%s: not painted" % name)
    for f in FILES:
        if "/" + f not in Quiet.asked:
            failed.append("%s was never asked for" % f)
    for t in TESTS:
        name = t.rsplit(".", 1)[0].rsplit("/", 1)[1]
        why = differs(os.path.join(pictures, name + ".png"),
                      os.path.join(pictures, name + "-expected.png"))
        if why:
            failed.append("%s: %s" % (name, why))

    if failed:
        print("FAIL: %d of %d checks on pages from the network:\n  " % (
            len(failed), len(planned) + len(FILES) + len(TESTS)) + "\n  ".join(failed))
        return 1
    print("PASS: %d pages from the network, by WebKit's own loader in a network process - "
          "%d fonts and pictures fetched, every page painted as its reference"
          % (len(planned), len(FILES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
