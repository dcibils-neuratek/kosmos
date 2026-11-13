#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5d's curl: pages fetched over HTTPS from inside the guest (`docs/w5.md`).

curl's own program, `curltest.elf`, fetches from HTTPS servers on this Mac,
at 10.0.2.2 through QEMU's network, and the same program built for this
Mac from the same release fetches the same things from the same servers at
127.0.0.1. Every command's exit code, what `--write-out` says about it and
the SHA-256 of every byte it saved are held to this Mac's.

What is asked, and what each holds:

  - **a page**, 300 KB, over TLS 1.3 against the test authority: the bytes,
    the status, the size;
  - **the same, gzip'd** (`--compressed`): curl's zlib, W5a's;
  - **chunked**, over HTTP/1.1, the page in pieces of every size;
  - **a redirect** (`-L`): one hop, then the page;
  - **cookies and the public suffix list**: a server at
    `www.kosmos.co.uk` sets one cookie for itself and one for all of
    `co.uk`; the jar keeps the first, refuses the second - libpsl's
    answer - and sends the first back to the echo;
  - **refusals**, as `run_tls.py` asks Kosmos's own TLS: the certificate
    for another name, out of its dates, and signed by an authority the
    guest was never given - each exit 60, curl's "peer certificate cannot
    be authenticated";
  - **8 MB**, its bytes and how long they took.

The authority, the certificates and the servers are `run_tls.py`'s, made
fresh each run. The guest and this Mac each have servers of their own,
which serve the same bytes - so a control can change what the guest is
served and not this Mac:

  KOSMOS_CURL_BREAK=page    one byte of the guest's page
  KOSMOS_CURL_BREAK=cookie  the guest's second cookie for the host's own
                            domain, so the jar keeps it
"""

import gzip
import hashlib
import http.server
import os
import re
import socketserver
import ssl
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
import upstream                                             # noqa: E402
from run_tls import pki                                     # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    "build", "user-x86_64" if X86 else "user", "apps")
BUILT = os.path.join(APPS, "curltest.elf")
WORK = scratch.directory("curl")
GUEST = "/Home/apps/curl"
NAME = "www.kosmos.co.uk"
BROKEN = os.environ.get("KOSMOS_CURL_BREAK")

# The page: 300 KB of text no compressor flattens to nothing, the same on
# every run.
PAGE = b"".join(b"%06d %s\n" % (i, hashlib.sha256(b"%d" % i).hexdigest().encode())
                for i in range(4400))
BIG = hashlib.shake_256(b"kosmos").digest(8 << 20)

SCRIPT = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One of W5d's curl commands (`tools/run_curl.py`).
-- kosmos: image curltest.elf
local kit = use("curltest.elf")
local crypto = use("/Kosmos/Kits/crypto")
local code = kit.run({%s}, "/Temporary/%s.log")
write(fs.read("/Temporary/%s.log") or "")
local saved = fs.read("/Temporary/%s.out")
print("curl-run %s: exit " .. code .. " sha " .. (saved and crypto.sha256(saved) or "-"))
"""


def handler(broken):
    page = PAGE
    if broken == "page":
        page = PAGE[:1000] + bytes([PAGE[1000] ^ 1]) + PAGE[1001:]

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def send(self, body, headers=()):
            self.send_response(200)
            for k, v in headers:
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/page":
                self.send(page, [("Content-Type", "text/plain")])
            elif self.path == "/gzip":
                self.send(gzip.compress(page, mtime=0),
                          [("Content-Type", "text/plain"), ("Content-Encoding", "gzip")])
            elif self.path == "/chunked":
                self.send_response(200)
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                at, size = 0, 1
                while at < len(page):
                    piece = page[at:at + size]
                    self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
                    at += len(piece)
                    size = size * 3 % 9973 + 1
                self.wfile.write(b"0\r\n\r\n")
            elif self.path == "/redirect":
                self.send_response(302)
                self.send_header("Location", "/page")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path == "/cookie":
                wide = "kosmos.co.uk" if broken == "cookie" else "co.uk"
                self.send(b"set\n", [("Set-Cookie", "mine=1; Path=/"),
                                     ("Set-Cookie", "wide=2; Domain=%s; Path=/" % wide)])
            elif self.path == "/echo":
                self.send(("cookie: %s\n" % self.headers.get("Cookie", "")).encode())
            elif self.path == "/big":
                self.send(BIG, [("Content-Type", "application/octet-stream")])
            else:
                self.send_error(404)

    return Handler


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # A client that refused the certificate closes in the handshake,
        # which is the point of three of the commands; anything else is
        # said, since it is not.
        error = sys.exc_info()[1]
        if not isinstance(error, (ConnectionResetError, ssl.SSLError, BrokenPipeError)):
            super().handle_error(request, client_address)


def serve(work, cert, broken):
    """An HTTPS server on this Mac, a port of its own; the port."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(os.path.join(work, cert + ".pem"),
                            os.path.join(work, "server.key"))
    server = Server(("127.0.0.1", 0), handler(broken))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


def commands(ports, address, cacert, folder):
    """Every command, as (name, arguments): `folder` is where it saves."""
    def at(port, path, name=NAME):
        return ["--resolve", "%s:%d:%s" % (name, port, address),
                "https://%s:%d%s" % (name, port, path)]

    good, expired, untrusted = ports
    out = lambda n: ["-o", "%s/%s.out" % (folder, n)]          # noqa: E731
    said = ["-sS", "--cacert", cacert, "-w",
            "%{http_code} %{size_download} %{num_redirects} %{ssl_verify_result}\\n"]
    jar = "%s/jar.txt" % folder
    return [
        ("page", said + out("page") + at(good, "/page")),
        ("gzip", said + out("gzip") + ["--compressed"] + at(good, "/gzip")),
        ("chunked", said + out("chunked") + at(good, "/chunked")),
        ("redirect", said + out("redirect") + ["-L"] + at(good, "/redirect")),
        ("cookie", said + ["-c", jar, "-o", "%s/cookie-set.txt" % folder]
         + at(good, "/cookie")),
        ("echo", said + ["-b", jar] + out("echo") + at(good, "/echo")),
        ("name", said + at(good, "/page", "other.kosmos.co.uk")),
        ("expired", said + at(expired, "/page")),
        ("untrusted", said + at(untrusted, "/page")),
        ("big", said + out("big") + at(good, "/big")),
    ]


def mac_curl():
    """curl's program, built for this Mac from the same release."""
    subprocess.run(["python3", os.path.join(HERE, "w5_build.py"), "host",
                    "zlib", "openssl", "curl"], check=True, capture_output=True)
    return os.path.join(ROOT, "build", "w5", "host", "curl", "src", "curl")


def outcome(text):
    """A command's lines that do not depend on where it ran: what
    `--write-out` said, curl's own error line less its paths, the exit."""
    kept = []
    for line in text.splitlines():
        line = line.rstrip()
        if re.match(r"^\d{3} \d+ \d+ \d+$", line):
            kept.append(line)
        elif line.startswith("curl: ("):
            kept.append(re.sub(r"curl: \((\d+)\).*", r"curl: (\1)", line))
    return kept


def main():
    if BROKEN not in (None, "page", "cookie"):
        print("FAIL: KOSMOS_CURL_BREAK is page or cookie, not %s" % BROKEN)
        return 1
    if not os.path.exists(BUILT):
        print("FAIL: no %s - make app-images" % BUILT)
        return 1

    pki(WORK, NAME)
    mac_ports = [serve(WORK, c, None) for c in ("good", "expired", "untrusted")]
    guest_ports = [serve(WORK, c, BROKEN) for c in ("good", "expired", "untrusted")]

    # This Mac's answers.
    curl = mac_curl()
    mac = os.path.join(WORK, "mac")
    os.makedirs(mac)
    expected = {}
    for name, args in commands(mac_ports, "127.0.0.1", os.path.join(WORK, "ca.pem"), mac):
        r = subprocess.run([curl] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=120)
        saved = os.path.join(mac, name + ".out")
        sha = hashlib.sha256(open(saved, "rb").read()).hexdigest() \
            if os.path.exists(saved) else "-"
        expected[name] = (r.returncode, outcome(r.stdout), sha)

    # What is checked against itself before the guest is: this Mac's curl
    # must have done what the suite claims.
    page_sha = hashlib.sha256(PAGE).hexdigest()
    claims = [
        (expected["page"][2] == page_sha, "the page's bytes"),
        (expected["gzip"][2] == page_sha, "the page, gzip'd"),
        (expected["chunked"][2] == page_sha, "the page, chunked"),
        (expected["redirect"][2] == page_sha and expected["redirect"][1][:1] == ["200 %d 1 0"
                                                                              % len(PAGE)],
         "the redirect"),
        (expected["echo"][2] == hashlib.sha256(b"cookie: mine=1\n").hexdigest(),
         "only the host's own cookie sent back"),
        (all(expected[n][0] == 60 for n in ("name", "expired", "untrusted")),
         "every refusal 60"),
        (expected["big"][2] == hashlib.sha256(BIG).hexdigest(), "8 MB"),
    ]
    wrong = [what for ok, what in claims if not ok]
    if wrong:
        print("FAIL: this Mac's curl did not: " + ", ".join(wrong))
        for name, e in expected.items():
            print("  %s: %s" % (name, e))
        return 1

    # The guest's disk: the program, a script a command, the authority.
    files = [(BUILT, GUEST + "/curltest.elf"), (os.path.join(WORK, "ca.pem"), GUEST + "/ca.pem")]
    planned = commands(guest_ports, "10.0.2.2", GUEST + "/ca.pem", "/Temporary")
    for name, args in planned:
        script = os.path.join(WORK, name + ".lua")
        with open(script, "w") as f:
            f.write(SCRIPT % (", ".join('"%s"' % a for a in args), name, name, name, name))
        files.append((script, GUEST + "/" + name + ".lua"))
    disk = os.path.join(WORK, "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"), os.path.join(HERE, "kfs.lua"),
                    "create", disk, "64"] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    import run_screenshot as R                              # after the disk
    board = "X86_ARGS" if R.machine(IMAGE) == "x86_64" else "QEMU_ARGS"
    setattr(R, board, getattr(R, board) + [
        "-netdev", "user,id=net0", "-device", R.device(IMAGE, "net") + ",netdev=net0"]
        + (["-cpu", "max"] if X86 else []))

    guest = R.Guest(IMAGE, 300)
    said, times = {}, {}
    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        guest.type("cd " + GUEST)
        for name, _ in planned:
            mark = len(guest.seen)
            began = time.monotonic()
            guest.type("run %s.lua" % name)
            guest.wait_for_line("curl-run %s: exit" % name, "curl " + name, since=mark)
            times[name] = time.monotonic() - began
            said[name] = guest.seen[mark:].replace("\r", "")
    except Exception as e:                                  # noqa: BLE001 - said below
        said["error"] = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        guest.close()

    fails = []
    if "error" in said:
        fails.append("the machine stopped: " + said["error"])
    for name, _ in planned:
        if name not in said:
            continue
        m = re.search(r"curl-run %s: exit (-?\d+) sha (\S+)" % name, said[name])
        got = (int(m.group(1)), outcome(said[name][:m.start()]), m.group(2))
        if got != expected[name]:
            fails.append("%s: Kosmos %s, this Mac %s" % (name, got, expected[name]))
    if fails:
        print("FAIL: " + "\nFAIL: ".join(fails))
        return 1

    print("PASS: curl 8.23.0 on Kosmos, over HTTPS to this Mac - the page plain, gzip'd, "
          "chunked and redirected, cookies kept and refused by the public suffix list, "
          "three certificates refused (60), 8 MB in %.1f s; every command as on this Mac"
          % times["big"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
