#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Packages, the window: install, update and remove by the pointer.

`docs/packages.md` step P3. A server on this Mac offers three packages in
`tools/packages.py`'s shape - `hello`, `base`, and `addon`, which depends on
`base` - and the guest installs `hello` 1.0 at the prompt before the server
offers 2.0 of it. Then the window, opened beside, is held to:

  * **its list**: three packages from the boot's server;
  * **Update** on `hello`'s row: `pkg` run in a process of its own, its
    report followed to "done ok", and the file the new version's;
  * **Install** on `addon`'s row: asked first, since `base` comes with it -
    "Install 2" - and both installed;
  * **Remove** of `base` in the details: asked first, since `addon` needs
    it - "Remove 2" - and both gone.

Usage: run_packages.py <image>
"""

import http.server
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import run_install as I                                      # noqa: E402


def main():
    image = sys.argv[1]
    offered, served = {}, {}

    def offer(name, zipped, f):
        served[name + ".zip"] = zipped
        offered[name] = dict(f, zip=len(zipped))

    offer("hello", *I.package("Hello", {"hello.txt": "the first\n"},
                              I.facts("hello", "Hello", "/Home/Apps/Hello")))
    offer("base", *I.package("Base", {"base.txt": "underneath\n"},
                             I.facts("base", "Base", "/Home/Apps/Base")))
    offer("addon", *I.package("Addon", {"addon.txt": "needs base\n"},
                              dict(I.facts("addon", "Addon", "/Home/Apps/Addon"), depends="base")))

    def index():
        return "".join("\n" + I.P.facts_text(f) + "zip         %d\n" % f["zip"]
                       for f in offered.values()).encode()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = index() if self.path == "/index.txt" else served.get(self.path[1:])

            if body is None:
                self.send_error(404)
                return

            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    R.extra_args(image, ["-netdev", "user,id=net0",
                         "-device", R.device(image, "net") + ",netdev=net0",
                         "-fw_cfg", "name=opt/kosmos/packages,string=10.0.2.2:%d" % port])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def said(text, since, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)
            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()
            time.sleep(0.1)
            guest._read_available()
        return None

    def run(command, until, seconds=60):
        mark = len(guest.seen)
        guest.type(command)
        deadline = time.monotonic() + seconds
        while until not in guest.seen[mark:] and time.monotonic() < deadline:
            time.sleep(0.2)
            guest._read_available()
        time.sleep(0.5)
        guest._read_available()
        return guest.seen[mark:].replace("\r", "")

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        run('for _ = 1, 100 do local i = fs.net_info("/Network") '
            'if i and i.address and i.address ~= "\\0\\0\\0\\0" then break end sys.sleep(25) end '
            'print("NET" .. "-UP")', "NET-UP", 60)
        run("pkg install hello", "pkg: hello - ")
        offer("hello", *I.package("Hello", {"hello.txt": "the second\n"},
                                  dict(I.facts("hello", "Hello", "/Home/Apps/Hello"), version="2.0")))

        start = len(guest.seen)
        guest.type("wm packages:--tell")
        opened = said("packages: window at ", start, 90)
        m = re.match(r"(\d+),(\d+); list at (\d+),(\d+), (\d+) wide, rows of (\d+)", opened or "")

        if m is None:
            print("FAIL: Packages never opened its window: %r\n%s" % (opened, guest.seen[-1500:]))
            return 1

        ox, oy, lx, ly, lw, row = (int(v) for v in m.groups())
        listed = said("packages: listed ", start, 30)
        check(listed == "3 from 10.0.2.2:%d" % port, "the window did not list three packages: %r" % listed)

        width, height, _ = R.pixel_reader(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(x, y, width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)

        def row_button(i):
            click(ox + lx + lw - 16 - 42, oy + ly + i * row + row // 2)

        def buttons(since, want, seconds=20):
            """The details' buttons, once one of them is `want`: {text: (x, y)}."""
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                for line in reversed(re.findall(r"packages: buttons \S+: ([^\r\n]*)", guest.seen[since:])):
                    found = {}
                    for part in line.split(" | "):
                        bm = re.match(r"(.+)@(\d+),(\d+)$", part)
                        if bm:
                            found[bm.group(1)] = (int(bm.group(2)), int(bm.group(3)))
                    if want in found:
                        return found
                    break
                time.sleep(0.2)
                guest._read_available()
            return {}

        def done(since, seconds=60):
            return said("packages: done ", since, seconds)

        # **Update**, on hello's row: the first.
        mark = len(guest.seen)
        row_button(0)
        check(said("packages: pressed ", mark, 10) == "Update on hello",
              "the first row's button was not Update on hello")
        finished = done(mark)
        check(finished is not None and finished.startswith("ok") and "1.0 -> 2.0" in finished,
              "updating hello from the window did not finish well: %r" % finished)
        relisted = said("packages: listed ", mark, 30)
        check(relisted is not None, "the window did not look again after the update")

        # **Install** addon: asked, since base comes with it.
        mark = len(guest.seen)
        row_button(2)
        check(said("packages: pressed ", mark, 10) == "Install on addon",
              "the third row's button was not Install on addon")
        asked = buttons(mark, "Install 2")
        check("Install 2" in asked and "Cancel" in asked,
              "installing addon did not ask first, with base beside it: %r" % asked)
        if "Install 2" in asked:
            click(ox + asked["Install 2"][0], oy + asked["Install 2"][1])
            finished = done(mark)
            check(finished is not None and finished.startswith("ok"),
                  "installing addon and base did not finish well: %r" % finished)
            said("packages: listed ", mark, 30)

        # **Remove** base, in its details: asked, since addon needs it.
        mark = len(guest.seen)
        click(ox + lx + 80, oy + ly + 1 * row + row // 2)
        check(said("packages: chose ", mark, 10) == "base", "a click on the second row did not choose base")
        have = buttons(mark, "Remove")
        if "Remove" in have:
            click(ox + have["Remove"][0], oy + have["Remove"][1])
        asked = buttons(mark, "Remove 2")
        check("Remove 2" in asked, "removing base did not ask to take addon with it: %r" % asked)
        if "Remove 2" in asked:
            click(ox + asked["Remove 2"][0], oy + asked["Remove 2"][1])
            finished = done(mark)
            check(finished is not None and finished.startswith("ok"),
                  "removing base and addon did not finish well: %r" % finished)

            # The states the window read *after* this removal was done: the
            # install's own relist prints a states line too, and under the
            # gate's load it arrived after `mark` and was read as this one -
            # "the others still installed" (testing.md 18.589, 18.615).
            after = guest.seen.find("packages: done ", mark)
            states = said("packages: states ", after if after >= 0 else mark, 30)
            check(states == "hello=installed 2.0, base=absent, addon=absent",
                  "after removing, the window does not read hello 2.0 installed and the "
                  "others absent: %r" % states)

        shot = guest.screendump()
        with open(os.path.join("build", "gate", "x86-packages.ppm"), "wb") as f:
            f.write(shot)
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    if failed:
        print("FAIL: %d of %d checks on Packages:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on Packages (three listed from the boot's server; Update on "
          "a row ran pkg in its own process and followed its report to 2.0; Install "
          "asked first with the dependency beside it and installed both; Remove in the "
          "details asked to take what needs it and removed both)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
