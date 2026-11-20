#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""JetSki's padlock (`docs/padlock.html`), `x86-padlock`.

JetSki opened on pages from servers on this Mac, each over HTTPS with a
certificate of `run_tls.pki`'s, or over plain HTTP, and what its address
field says of each read from the log, where the window says it:

1. A certificate an authority nobody added signed: refused, curl's error
   60, and JetSki's own page in the site's place - "JetSki did not open",
   with the reason in this machine's words. *Open anyway* pressed: the page
   loads in that tab, and the field says Not secure.
2. A certificate the test's authority signed, which is in
   `/Home/Settings/JetSki/Certificates/` where a person adds one: the green
   padlock alone, the certificate's name and issuer as the network process
   saw them, and the padlock pressed opens the connection's panel.
3. The same authority's certificate out of its dates: refused, "its
   certificate has expired".
4. Plain HTTP, typed on the refused page: Not encrypted, and its padlock
   pressed.
5. The shield (`adblock.md` B4): a page whose `ad.js` and `track.gif` a
   rule list on the disk keeps out. The shield counts 2, the server is
   asked for neither, the month's total is written as 2, and the shield
   pressed names the host most blocked. Its switch pressed turns blocking
   off for the site: the page loads again with WebKit's blockers off for
   it, the server is asked for `ad.js`, the shield says off, and the
   month's total does not move.

The controls: `KOSMOS_PADLOCK_BREAK=anywayless` - Open anyway pressed and
never asking the kit - leaves the refused page where it was, FAIL;
`KOSMOS_PADLOCK_BREAK=trusting` adds the other authority too, so nothing is
refused, FAIL; and `KOSMOS_PADLOCK_BREAK=keyless` leaves the keys in the
field when an address is typed on the refused page - as JetSki did until
the second control found it - so step 4's padlock cannot be pressed, FAIL; and
`KOSMOS_PADLOCK_BREAK=unallowed` never hands the kit the sites allowed, so
the switch reloads the page with blocking still on and `ad.js` is never
asked for, FAIL.

Usage: run_padlock.py [kernel image] [apps folder]
"""

import http.server
import os
import re
import shutil
import socketserver
import ssl
import subprocess
import sys
import threading
import time

import jetski_files

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import run_tls                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "user-x86_64", "apps")
WORK = scratch.directory("padlock")
PKI = os.path.join(WORK, "pki")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
KEPT = os.path.join(ROOT, "build", "padlock")
BROKEN = os.environ.get("KOSMOS_PADLOCK_BREAK")

TITLE = "Padlock test page"
PAGE = ("<!doctype html><html><head><title>%s</title></head>"
        "<body style=\"background:#fff\"><h1>A page</h1></body></html>\n" % TITLE).encode()
SHIELDED = ("<!doctype html><html><head><title>Shielded page</title>"
            "<script src=\"/ad.js\"></script></head><body style=\"background:#fff\">"
            "<h1>Adverts kept out</h1><img src=\"/track.gif\" width=\"1\" height=\"1\" alt=\"\">"
            "</body></html>\n").encode()
GIF = (b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00"
       b"\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;")
RULES = ('[{"trigger": {"url-filter": "/ad\\\\.js$"}, "action": {"type": "block"}},'
         ' {"trigger": {"url-filter": "/track\\\\.gif$"}, "action": {"type": "block"}}]')
QEMU_KEYS = {":": "shift-semicolon", "/": "slash", ".": "dot", "-": "minus", "\n": "ret"}


class Page(http.server.BaseHTTPRequestHandler):
    asked = []

    def do_GET(self):
        Page.asked.append((self.server.server_address[1], self.path))
        body, kind = {"/shielded": (SHIELDED, "text/html"),
                      "/ad.js": (b"var advert = 1;", "text/javascript"),
                      "/track.gif": (GIF, "image/gif")}.get(self.path, (PAGE, "text/html"))
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, address):
        pass                                # a refused handshake: the point


def serve(cert=None):
    """A server on this Mac - HTTP, or HTTPS with `cert` - and its port."""
    server = Server(("127.0.0.1", 0), Page)
    if cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(os.path.join(PKI, cert + ".pem"),
                                os.path.join(PKI, "server.key"))
        server.socket = context.wrap_socket(server.socket, server_side=True,
                                            do_handshake_on_connect=False)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


def said(guest, mark, pattern, seconds):
    """The first match of `pattern` the guest says after `mark` within
    `seconds`, or None."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest._read_available()
        # Only whole lines: one still arriving can match short
        # (testing.md 18.939).
        text = guest.seen[mark:]
        m = re.search(pattern, text[:text.rfind("\n") + 1])
        if m:
            return m
        time.sleep(0.25)
    return None


def last_words(guest, mark):
    return re.findall(r"jetski: [^\r\n]*", guest.seen[mark:])[-4:] or ["nothing said"]



# **The controls' changes to JetSki's files** (`jetski_files.py`), each
# tried on a copy before every run; the one named by the break is made.
CONTROLS = {
    # Open anyway pressed, and never asking the kit.
    "anywayless": [("    web.anyway()\n", "    do end\n")],
    # The keys left in the field when an address is typed on the refused page.
    "keyless": [("  page.wants_keys = not win:focus_on(view)\n  web.focus(true)\n",
                 "  win:focus_on(view)\n  web.focus(true)\n")],
    # The sites allowed never handed to the kit.
    "unallowed": [("  if have then webkit.allow(table.concat(allowed, \"\\n\")) end\n", "  do end\n")],
}


def main():
    built = os.path.join(APPS, "webkit.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1

    shutil.rmtree(PKI, ignore_errors=True)
    os.makedirs(PKI)
    run_tls.pki(PKI, "10.0.2.2")
    servers = {name: serve(cert) for name, cert in
               (("untrusted", "untrusted"), ("good", "good"), ("expired", "expired"),
                ("plain", None))}
    at = {name: ("%s://10.0.2.2:%d/" % ("http" if name == "plain" else "https", port))
          for name, (_, port) in servers.items()}

    image = os.path.join(WORK, "webkit.elf")
    subprocess.run(["x86_64-elf-objcopy" if "x86_64" in IMAGE else "aarch64-elf-objcopy",
                    "--strip-debug", built, image], check=True)
    texts = jetski_files.read()
    jetski_files.tried(CONTROLS)
    for old, new in CONTROLS.get(BROKEN, []):
        jetski_files.change(texts, old, new, "the control " + BROKEN)
    rules = os.path.join(WORK, "test.json")
    with open(rules, "w") as f:
        f.write(RULES)
    files = jetski_files.install(texts, WORK) + [(rules, "/Home/Settings/Filters/test.json"),
             (image, "/Home/System/WebKit/webkit.elf"),
             (os.path.join(PKI, "ca.pem"), "/Home/Settings/JetSki/Certificates/test-authority.pem")]
    if BROKEN == "trusting":
        files.append((os.path.join(PKI, "other.pem"),
                      "/Home/Settings/JetSki/Certificates/other-authority.pem"))
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402 - after the disk
    board = "X86_ARGS" if R.machine(IMAGE) == "x86_64" else "QEMU_ARGS"
    setattr(R, board, [("1G" if a == "512M" else a) for a in getattr(R, board)])
    setattr(R, board, getattr(R, board) + [
        "-netdev", "user,id=net0", "-device", R.device(IMAGE, "net") + ",netdev=net0"])

    os.makedirs(KEPT, exist_ok=True)
    failed, mark = [], 0
    guest = R.Guest(IMAGE, 400)
    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        sw = sh = None

        def shot(name):
            nonlocal sw, sh
            dump = guest.screendump()
            sw, sh, _ = R.parse_ppm(dump)
            R.write_png(os.path.join(KEPT, name + ".png"), dump)

        def click(px, py):
            guest.mouse_to(*R._to_tablet(wx + px, wy + py, sw, sh))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.15)
            guest.mouse_button(False)

        def visit(address):
            since = len(guest.seen)
            guest.sendkey("ctrl-l")
            time.sleep(0.4)
            for key in [QEMU_KEYS.get(c, c) for c in address + "\n"]:
                guest.sendkey(key)
            return since

        # 1. Refused, and opened anyway.
        mark = len(guest.seen)
        guest.type("wm jetski:" + at["untrusted"])
        window = said(guest, mark, r"wm: window JetSki at (\d+),(\d+) (\d+)x(\d+)", 120)
        if not window:
            return fail(["JetSki's window never opened: %s" % last_words(guest, mark)], guest, mark)
        wx, wy = int(window.group(1)), int(window.group(2))
        refused = said(guest, mark, r"jetski: refused 10\.0\.2\.2(?::\d+)?: ([^\r\n]*)", 90)
        anyway_at = said(guest, mark, r"jetski: open anyway at (\d+),(\d+)", 10)
        time.sleep(1)
        shot("refused")
        if not refused or not anyway_at:
            failed.append("a certificate nobody vouches for was not refused, or no page said so: %s"
                          % last_words(guest, mark))
        else:
            print("refused: %s" % refused.group(1))
            if "(curl's 60," not in refused.group(1):
                failed.append("refused, and not curl's 60: %s" % refused.group(1))
            if not said(guest, mark, r"jetski: padlock refused, Refused at", 10):
                failed.append("the refused page's address field did not say Refused")
            since = len(guest.seen)
            click(int(anyway_at.group(1)), int(anyway_at.group(2)))
            if not said(guest, since, r"jetski: came anyway", 60) \
               or not said(guest, since, r"jetski: title " + TITLE, 30):
                failed.append("Open anyway was pressed and the page did not load: %s"
                              % last_words(guest, since))
            elif not said(guest, since, r"jetski: padlock refused, Not secure at", 10):
                failed.append("a page opened anyway did not say Not secure: %s" % last_words(guest, since))
            time.sleep(1)
            shot("anyway")

        # 2. Secure, and its panel.
        since = visit(at["good"])
        came = said(guest, since, r"jetski: came secure, certificate for ([^\r\n]*) by ([^\r\n]*)", 60)
        padlock = said(guest, since, r"jetski: padlock secure at (\d+),(\d+)", 30)
        if not came or not padlock:
            failed.append("a certificate of an authority in /Home did not show the green padlock: %s"
                          % last_words(guest, since))
        else:
            print("secure: certificate for %s, by %s" % (came.group(1), came.group(2)))
            # Its DNS name first, then its address (patch 0036: a DNS name
            # had come out as "true").
            if came.group(1) != "10.0.2.2, 10.0.2.2" or came.group(2) != "Kosmos test authority ca":
                failed.append("the certificate's names are not what the server sent: for %s, by %s"
                              % (came.group(1), came.group(2)))
            time.sleep(1)
            pressed = len(guest.seen)
            click(int(padlock.group(1)), int(padlock.group(2)))
            if not said(guest, pressed, r"jetski: the padlock pressed: The connection to 10\.0\.2\.2(?::\d+)? is secure", 10):
                failed.append("the padlock was pressed and its panel did not say the connection is secure: %s"
                              % last_words(guest, pressed))
            time.sleep(1)
            shot("secure")

        # 3. Expired.
        since = visit(at["expired"])
        expired = said(guest, since, r"jetski: refused 10\.0\.2\.2(?::\d+)?: ([^\r\n]*)", 60)
        if not expired or "has expired" not in expired.group(1):
            failed.append("a certificate out of its dates was not refused as expired: %s"
                          % (expired.group(1) if expired else last_words(guest, since)))
        else:
            print("expired: %s" % expired.group(1))

        # 4. Plain HTTP - typed from the refused page, which must give the
        # keys back to the page, or the field stays focused and its padlock
        # cannot be pressed.
        since = visit(at["plain"])
        plain = said(guest, since, r"jetski: padlock plain, Not encrypted at (\d+),(\d+)", 60)
        if not said(guest, since, r"jetski: came plain", 60) or not plain:
            failed.append("a page over plain HTTP did not say Not encrypted: %s" % last_words(guest, since))
        else:
            said(guest, since, r"jetski: title " + TITLE, 30)
            time.sleep(1)
            pressed = len(guest.seen)
            click(int(plain.group(1)), int(plain.group(2)))
            if not said(guest, pressed, r"jetski: the padlock pressed: The connection to 10\.0\.2\.2(?::\d+)? is not encrypted", 10):
                failed.append("an address typed on the refused page left the field with the keys: "
                              "its padlock pressed said nothing: %s" % last_words(guest, pressed))
        time.sleep(1)
        shot("plain")

        # 5. The shield: what a list kept out, the month's total, and
        # blocking off for the site.
        plain_port = servers["plain"][1]
        since = visit(at["plain"] + "shielded")
        counted = said(guest, since, r"jetski: shield 2 at (\d+),(\d+)", 120)
        loaded = said(guest, since, r"jetski: loaded [^\r\n]*/shielded", 60)
        month = said(guest, since, r"jetski: kept this month (\d+) requests, (\d+) cookies", 20)
        asked = [p for port, p in Page.asked if port == plain_port and p in ("/ad.js", "/track.gif")]
        if not counted or not loaded:
            failed.append("a page with two requests a list keeps out did not count 2 on the shield: %s"
                          % last_words(guest, since))
        elif asked:
            failed.append("the list did not keep out %s" % asked)
        elif not month or month.group(1) != "2":
            failed.append("the month's total was not written as 2: %s"
                          % (month.group(0) if month else last_words(guest, since)))
        else:
            time.sleep(1)
            pressed = len(guest.seen)
            click(int(counted.group(1)), int(counted.group(2)))
            words = said(guest, pressed, r"jetski: the shield pressed: ([^\r\n]*)", 10)
            switch = said(guest, pressed, r"jetski: blocking switch at (\d+),(\d+)", 10)
            want = "2 kept out, 0 cookies refused; most blocked 10.0.2.2 2; this month 2, 0"
            if not words or words.group(1) != want or not switch:
                failed.append("the shield pressed did not say %r: %s"
                              % (want, words.group(1) if words else last_words(guest, pressed)))
                shot("shield-failed")
            else:
                print("the shield: %s" % words.group(1))
                time.sleep(1)
                shot("shield")
                flipped = len(guest.seen)
                click(int(switch.group(1)), int(switch.group(2)))
                if not said(guest, flipped, r"jetski: blocking off for 10\.0\.2\.2\b", 10):
                    failed.append("the shield's switch pressed did not turn blocking off: %s"
                                  % last_words(guest, flipped))
                else:
                    off = said(guest, flipped, r"jetski: shield 0, off at", 60)
                    said(guest, flipped, r"jetski: loaded [^\r\n]*/shielded", 60)
                    time.sleep(2)
                    asked = [p for port, p in Page.asked if port == plain_port and p == "/ad.js"]
                    if not asked:
                        failed.append("blocking turned off for the site, and its page reloaded "
                                      "still without ad.js: %s" % last_words(guest, flipped))
                    elif not off:
                        failed.append("blocking off, and the shield did not say so: %s"
                                      % last_words(guest, flipped))
                    elif said(guest, flipped, r"jetski: kept this month (?!2 requests)", 3):
                        failed.append("the month's total moved with blocking off")
                    else:
                        print("blocking off: ad.js asked for, the shield off")
                    shot("unshielded")
    finally:
        guest.close()
        for server, _ in servers.values():
            server.shutdown()

    return fail(failed, guest, mark) if failed else passed()


def fail(failed, guest, mark):
    with open(os.path.join(KEPT, "serial.txt"), "w") as f:
        f.write(guest.seen)
    print("FAIL: %d of JetSki's padlock checks:\n  %s\n  (the machine's words in %s)"
          % (len(failed), "\n  ".join(failed), os.path.join(KEPT, "serial.txt")))
    return 1


def passed():
    print("PASS: JetSki's padlock - a certificate nobody vouches for refused, curl's 60, "
          "with JetSki's own page and Open anyway, which loaded it Not secure; one of an "
          "authority in /Home a green padlock, its names as sent and its panel; one out of "
          "its dates refused as expired; plain HTTP Not encrypted; and the shield, which counted "
          "what a list kept out, kept the month's total and turned blocking off for the site")
    return 0


if __name__ == "__main__":
    sys.exit(main())
