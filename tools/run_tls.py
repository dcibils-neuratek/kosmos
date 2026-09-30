#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""HTTPS from Kosmos: `fetch https://` through the TLS Kit (BearSSL).

`roadmap.md`, the browser: TLS, steps 2 to 5 - Diego, 30 September: "Go for
it". A TLS server runs on this Mac, three of them, reached from the guest at
10.0.2.2 through QEMU's network, with a certificate authority made for the
test here and handed to the guest as `/Home/ca.der`:

  - a page fetched whole, the certificate named for `kosmos-test.local` and
    signed by that authority - the right answer;
  - refused, with the reason a person would want: the same server asked for
    another name; a certificate out of its dates; one signed by an authority
    the guest was never given; and the right server with no `--cacert`,
    since Mozilla's roots, which the image carries, do not include a test's;
  - and the roots counted: 121, as `assets/ca/cacert.pem` holds them.

The authority, the certificates and the servers are made fresh each run
with OpenSSL, and live in a scratch folder.

Usage: run_tls.py IMAGE
"""

import os
import random
import socket
import ssl
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

PAGE = b"served over TLS to Kosmos\n"


def openssl(*args, cwd):
    subprocess.run(["openssl", *args], cwd=cwd, check=True, capture_output=True)


def pki(work):
    """An authority, a certificate for kosmos-test.local it signed, the same
    out of its dates, and one another authority signed."""
    for ca in ("ca", "other"):
        openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3",
                "-subj", "/CN=Kosmos test authority " + ca,
                "-addext", "basicConstraints=critical,CA:TRUE",
                "-addext", "keyUsage=critical,keyCertSign,cRLSign",
                "-keyout", ca + ".key", "-out", ca + ".pem", cwd=work)

    openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=kosmos-test.local",
            "-keyout", "server.key", "-out", "server.csr", cwd=work)

    with open(os.path.join(work, "ext.cnf"), "w") as f:
        f.write("subjectAltName=DNS:kosmos-test.local\n"
                "basicConstraints=CA:FALSE\n"
                "keyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\n")

    for name, ca, dates in (("good", "ca", ["-days", "2"]),
                            ("expired", "ca", ["-not_before", "20200101000000Z",
                                               "-not_after", "20201231000000Z"]),
                            ("untrusted", "other", ["-days", "2"])):
        openssl("x509", "-req", "-in", "server.csr", "-CA", ca + ".pem",
                "-CAkey", ca + ".key", "-CAcreateserial", "-extfile", "ext.cnf",
                *dates, "-out", name + ".pem", cwd=work)

    openssl("x509", "-in", "ca.pem", "-outform", "DER", "-out", "ca.der", cwd=work)


def serve(work, cert):
    """A TLS server on this Mac, on a port of its own; the port."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(os.path.join(work, cert + ".pem"),
                            os.path.join(work, "server.key"))
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(8)

    def loop():
        while True:
            try:
                raw, _ = listener.accept()
            except OSError:
                return

            try:
                raw.settimeout(30)
                conn = context.wrap_socket(raw, server_side=True)
                asked = b""

                while b"\r\n\r\n" not in asked:
                    piece = conn.recv(4096)

                    if not piece:
                        break

                    asked += piece

                conn.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                             b"Content-Length: %d\r\n\r\n" % len(PAGE) + PAGE)

                try:
                    conn.unwrap()           # a close_notify, as a server should
                except (ssl.SSLError, OSError):
                    pass

                conn.close()
            except (ssl.SSLError, OSError):
                raw.close()                 # a refused handshake: the point

    threading.Thread(target=loop, daemon=True).start()
    return listener.getsockname()[1]


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("tls")
    pki(work)

    disk = os.path.join(work, "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32",
                    os.path.join(work, "ca.der") + ":/Home/ca.der"],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    import run_network                                      # after the disk
    import run_screenshot as R

    good, expired, untrusted = serve(work, "good"), serve(work, "expired"), serve(work, "untrusted")
    at = "https://10.0.2.2:%d/hello --name %s"
    commands = [
        (at % (good, "kosmos-test.local")) + " --cacert /Home/ca.der",
        (at % (good, "other.local")) + " --cacert /Home/ca.der",
        (at % (expired, "kosmos-test.local")) + " --cacert /Home/ca.der",
        (at % (untrusted, "kosmos-test.local")) + " --cacert /Home/ca.der",
        at % (good, "kosmos-test.local"),
        'print("ANCH" .. "ORS " .. sys.kit("tls").anchors)',
    ]

    # One command at a time, each read from where it was typed to the line
    # the shell prints when its process ends - not to the next prompt, which
    # an empty line typed after it can print early and put every later
    # answer one command out.
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        guest = R.Guest(image, 120)
    finally:
        setattr(R, board, saved)

    answers = {}

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")

        for command in commands:
            line = command if command.startswith("print") else "fetch " + command
            mark = len(guest.seen)
            guest.type(line)
            ends = "ANCHORS " if command.startswith("print") else ") ended, code"
            guest.wait_for_line(ends, "answered " + line, since=mark)
            answers[command] = guest.seen[mark:].replace("\r", "")
    except Exception as e:                  # noqa: BLE001 - said below
        answers["error"] = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        guest.close()

    transcript = "".join(answers.values())
    fails = []

    if "error" in answers:
        fails.append("the machine stopped: " + answers["error"])

    def answer_to(fragment):
        return answers.get(fragment[len("fetch "):] if fragment.startswith("fetch ") else fragment, "")

    first = answer_to("fetch " + commands[0])

    if "200 OK" not in first or PAGE.decode().strip() not in first:
        fails.append("the page did not come over TLS: %r" % first[-400:])

    for command, reason, what in (
            (commands[1], "the certificate is for another name", "another name"),
            (commands[2], "the certificate is out of its dates", "an expired certificate"),
            (commands[3], "the certificate is signed by nobody this machine trusts",
             "an authority it was not given"),
            (commands[4], "the certificate is signed by nobody this machine trusts",
             "no --cacert, where Mozilla's roots do not include the test's")):
        said = answer_to("fetch " + command)

        if reason not in said or PAGE.decode().strip() in said:
            fails.append("%s was not refused with %r: %r" % (what, reason, said[-300:]))

    if "ANCHORS 121" not in transcript:
        fails.append("the image did not carry Mozilla's 121 roots: %r"
                     % answers.get(commands[5], "")[-200:])

    if " died: " in transcript:
        fails.append("something died: " + transcript[transcript.find(" died: ") - 80:][:300])

    checks = 6

    if fails:
        print("FAIL: %d of %d checks on HTTPS:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on HTTPS through the TLS Kit (a page fetched whole "
          "from a server on this Mac, and refused for another name, for an "
          "expired certificate, for an authority it was not given, and with no "
          "--cacert against Mozilla's 121 roots)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
