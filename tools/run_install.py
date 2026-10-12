#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`install`: packages fetched from a server, on demand.

A server on this Mac offers two packages in `tools/packages.py`'s shape - an
index and a zip each - and the guest, its server named on the command line
as `make netboot` names the Mac (`opt/kosmos/packages`):

  * **lists them**, each "not installed";
  * **installs one** - its file where the index says, readable, and the list
    saying it is installed from the build the index gave;
  * **installs it again** over itself, the file replaced by the new one's;
  * **is offered a zip that will not open** for the other, and leaves what
    was there as it was, saying why.

Usage: run_install.py <image>
"""

import http.server
import io
import os
import re
import sys
import threading
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402


import hashlib                                               # noqa: E402

import packages as P                                         # noqa: E402

STAMP = P.stamp()


def facts(name, title, into, protocols=None):
    return {"name": name, "title": title, "kind": "application", "about": "a test's",
            "version": "1.0", "build": "test", "protocols": protocols or STAMP,
            "into": into, "icon": "", "needs": "", "depends": ""}


def package(folder, files, f, tamper=False):
    """A zip in `tools/packages.py`'s shape: the folder, and its manifest -
    with `tamper`, one file's bytes changed after its sum was taken."""
    out = io.BytesIO()
    lines = "".join("file        %d %s %s\n" % (len(t), hashlib.sha256(t.encode()).hexdigest(), n)
                    for n, t in sorted(files.items()))
    f = dict(f, bytes=sum(len(t) for t in files.values()))
    manifest = P.facts_text(f) + lines

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(folder + "/package.txt", manifest)

        for name, text in files.items():
            z.writestr(folder + "/" + name, (text.upper() if tamper else text))

    return out.getvalue(), f


def main():
    image = sys.argv[1]
    offered = {}
    served = {}

    def offer(name, zipped, f):
        served[name + ".zip"] = zipped
        offered[name] = dict(f, zip=len(zipped))

    offer("hello", *package("Hello", {"hello.txt": "the first\n"}, facts("hello", "Hello", "/Home/Apps/Hello")))
    served["broken.zip"] = b"PK\x03\x04 this is not a zip"
    offered["broken"] = dict(facts("broken", "Broken", "/Home/Apps/Broken"), bytes=0, zip=28)
    offer("bent", *package("Bent", {"bent.txt": "as it was made\n"},
                           facts("bent", "Bent", "/Home/Apps/Bent"), tamper=True))
    offer("other", *package("Other", {"other.txt": "elsewhere\n"},
                            facts("other", "Other", "/Home/Apps/Other", protocols="0123456789abcdef")))

    def index():
        return "".join("\n" + P.facts_text(f) + "zip         %d\n" % f["zip"]
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

    def run(command, until, seconds=60):
        mark = len(guest.seen)
        guest.type(command)
        deadline = time.monotonic() + seconds

        while until not in guest.seen[mark:] and time.monotonic() < deadline:
            time.sleep(0.2)
            guest._read_available()

        time.sleep(0.5)
        guest._read_available()
        return guest.seen[mark:]

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        # The network's address first: `install` asks as soon as it starts.
        run('for _ = 1, 100 do local i = fs.net_info("/Network") '
            'if i and i.address and i.address ~= "\\0\\0\\0\\0" then break end sys.sleep(25) end '
            'print("NET" .. "-UP")', "NET-UP", 60)

        listed = run("install", "\nkosmos> ")
        check(re.search(r"install:\s+hello\s+Hello\s+\S+ \w+\s+not installed", listed) is not None
              and "4 packages from 10.0.2.2:%d" % port in listed,
              "the list did not offer hello, not installed, from the boot's server: %r"
              % listed[-600:])

        once = run("install hello", "install: hello")
        got = run('print("GOT " .. tostring(fs.read("/Home/Apps/Hello/hello.txt")))', "GOT ")
        check("install: hello - 1.0 in /Home/Apps/Hello" in once
              and "GOT the first" in got,
              "hello was not installed with its file: %r %r" % (once[-300:], got[-200:]))

        again_list = run("install", "\nkosmos> ")
        check(re.search(r"hello\s+Hello\s+\S+ \w+\s+installed - ", again_list) is not None,
              "the list did not say hello is installed: %r" % again_list[-600:])

        offer("hello", *package("Hello", {"hello.txt": "the second\n"},
                                facts("hello", "Hello", "/Home/Apps/Hello")))
        run("install hello", "install: hello")
        got = run('print("GOT " .. tostring(fs.read("/Home/Apps/Hello/hello.txt")))', "GOT ")
        check("GOT the second" in got and "Hello.old" not in run("ls /Home/Apps", "kosmos> ", 10),
              "installing again did not replace the file, or left the old folder: %r"
              % got[-200:])

        run('fs.write("/Home/Apps/Broken/kept.txt", "kept")', "kosmos> ", 10)
        broken = run("install broken", "install: broken")
        kept = run('print("KEPT " .. tostring(fs.read("/Home/Apps/Broken/kept.txt")))', "KEPT ")
        check("not installed: it would not open" in broken and "KEPT kept" in kept,
              "a zip that would not open was not refused, or what was there went: %r %r"
              % (broken[-300:], kept[-200:]))

        # **Checked before anything is touched** (`packages.lua`): a file
        # whose bytes are not what its sum says, and a package made for
        # other protocols, each refused with why, and nothing installed.
        bent = run("install bent", "install: bent")
        check("not installed: bent.txt is not what was made" in bent,
              "a file whose SHA-256 differs was not refused: %r" % bent[-300:])
        other = run("install other", "install: other")
        check("not installed: it was made for other protocols" in other,
              "a package for other protocols was not refused: %r" % other[-300:])
        gone = run('print("GONE " .. tostring(fs.getattr("/Home/Apps/Bent")) .. " " '
                   '.. tostring(fs.getattr("/Home/Apps/Other")))', "GONE ")
        check("GONE nil nil" in gone, "a refused package left its folder: %r" % gone[-200:])

        # **Dependencies, removal and update** (P2): `addon` depends on
        # `hello`, which is removed first so that installing `addon` has to
        # bring it; removing `hello` alone is refused while `addon` needs it,
        # and both go together, a file a person put beside them kept; then a
        # newer `hello` on the server is what `pkg update` installs.
        offer("addon", *package("Addon", {"addon.txt": "needs hello\n"},
                                dict(facts("addon", "Addon", "/Home/Apps/Addon"), depends="hello")))
        run("pkg remove hello", "\nkosmos> ")
        dep = run("pkg install addon", "pkg: addon - ")
        have = run('print("HAVE " .. tostring(fs.read("/Home/Apps/Hello/hello.txt")) .. tostring(fs.read("/Home/Apps/Addon/addon.txt")))', "HAVE ")
        check("pkg: hello first, which another depends on" in dep and "HAVE the second\nneeds hello" in have.replace("\r", ""),
              "installing addon did not bring hello first: %r %r" % (dep[-400:], have[-200:]))

        refused = run("pkg remove hello", "\nkosmos> ")
        check("hello is needed by addon - remove both with: pkg remove hello addon" in refused,
              "removing hello while addon needs it was not refused: %r" % refused[-300:])

        run('fs.write("/Home/Apps/Hello/mine.txt", "a person\'s")', "\nkosmos> ")
        both = run("pkg remove hello addon", "\nkosmos> ")
        after = run('print("AFTER " .. tostring(fs.getattr("/Home/Apps/Addon")) .. " " '
                    '.. tostring(fs.read("/Home/Apps/Hello/hello.txt")) .. " " '
                    '.. tostring(fs.read("/Home/Apps/Hello/mine.txt")))', "AFTER ")
        check("pkg: hello - removed from /Home/Apps/Hello" in both
              and "AFTER nil nil a person's" in after,
              "pkg remove did not take exactly what each installed, keeping what was "
              "put beside them: %r %r" % (both[-400:], after[-200:]))

        run("pkg install hello", "pkg: hello - ")
        offer("hello", *package("Hello", {"hello.txt": "the third\n"},
                                dict(facts("hello", "Hello", "/Home/Apps/Hello"), version="2.0")))
        updated = run("pkg update", "\nkosmos> ")
        got = run('print("GOT " .. tostring(fs.read("/Home/Apps/Hello/hello.txt")))', "GOT ")
        check("pkg: hello - 1.0 -> 2.0 in /Home/Apps/Hello" in updated and "GOT the third" in got,
              "pkg update did not bring hello from 1.0 to 2.0: %r %r" % (updated[-300:], got[-200:]))
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    if failed:
        print("FAIL: %d of %d checks on install:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-2500:])
        return 1

    print("PASS: %d checks on install (packages listed from the boot's server, one "
          "installed with its file and said installed, installed again over itself "
          "with nothing left behind, a zip that would not open refused with what was "
          "there kept, and a file whose SHA-256 differs and a package for other "
          "protocols refused, leaving nothing; a dependency installed first, a removal "
          "refused while another needs it, both removed together keeping what a "
          "person put beside them, and pkg update bringing a newer version)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
