#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Sandboxes: Alpine Linux in virtual machines on Kosmos, through `sandbox`.

`docs/virtualization.md` V3 and `docs/sandboxes.md` S1 - Diego, 12 October
2026: "the initial goal for sandboxes is to run a linux distro like alpine",
then "continue with the sandbox app", "as designed in the mockup". The ARM
machine is started at EL2 with the processor's virtualization on and a
`/Home` disk with Alpine installed as an image - `Sandboxes/Images/alpine`,
its kernel and an initramfs of its userland, made by `tools/alpine.py` from
Alpine's published files, held to their sums - and `sandbox` is typed at
the prompt. Held to:

  * **a sandbox that starts with Kosmos**: `boot1`, on the disk with
    `starts with Kosmos`, running without anyone starting it;
  * **its images**: `sandbox images` names Alpine;
  * **a task**: `sandbox run alpine -- echo $((6*7)); uname -m; exit 3` -
    the command's output, and its exit code, 3, taken from the guest's end
    line (which never reaches the console) and made the program's own; the
    guest booted, ran and switched itself off through PSCI, which needs its
    timer, its interrupt controller and its serial port all working;
  * **a shell**: `sandbox run alpine` reaches a prompt named for the
    sandbox and answers what is typed at Kosmos's console - `cat
    /etc/alpine-release`, 3.24.2 - and `exit` ends it;
  * **a service**: `sandbox new web --service -- <a loop>`, then `start` -
    found in `/Running`, `list`ed as running, its console attached and
    printing, Control-W then D leaving it running, `stop`ped, and `list`ed
    as stopped;
  * **a folder of `/Home`**, by virtio-fs: read only - listed, read, and a
    write refused - and read and write, a file the guest made there read
    back by Kosmos at the prompt;
  * **the network**, by virtio-net and the monitor's gateway: its gateway
    pinged, a page on this Mac fetched through Kosmos's own TCP, and a name
    that cannot exist answered NXDOMAIN by its name server; and a port
    carried in - busybox's nc in a sandbox given only Kosmos's 8081,
    its page fetched from this Mac and its connection ended by the guest,
    and nothing reached out from it;
  * no terminal escape reaching Kosmos's console in any of it.

The screen at the end is kept in `build/gate/arm-sandbox-linux.ppm`.

Usage: run_sandbox.py <image>
"""

import functools
import http.server
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    image = sys.argv[1]
    folder = scratch.path("sandbox-home")
    disk = scratch.path("sandbox-home.img")
    alpine = os.path.join(folder, "Sandboxes", "Images", "alpine")

    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(alpine)

    # A project to share: a file and a folder in it (S3).
    os.makedirs(os.path.join(folder, "Projects", "site", "pages"))

    with open(os.path.join(folder, "Projects", "site", "hello.txt"), "w") as f:
        f.write("hello from Kosmos\n")

    with open(os.path.join(folder, "Projects", "site", "pages", "one.txt"), "w") as f:
        f.write("page one\n")

    # A server of one answer, for a port carried in (S4): busybox's `nc`
    # runs it for the connection, and it reads the request before answering
    # so the guest closes it with a FIN rather than a reset.
    os.makedirs(os.path.join(folder, "Projects", "serve"))

    with open(os.path.join(folder, "Projects", "serve", "serve.sh"), "w") as f:
        f.write("while read -r l; do l=$(echo \"$l\" | tr -d '\\r'); [ -z \"$l\" ] && break; done\n"
                "printf 'HTTP/1.0 200 OK\\r\\n\\r\\nserved by alpine\\n'\n")

    # A sandbox that starts with Kosmos (S5): nobody starts it here.
    os.makedirs(os.path.join(folder, "Sandboxes", "boot1"))

    with open(os.path.join(folder, "Sandboxes", "boot1", "sandbox.txt"), "w") as f:
        f.write("image       alpine\nkind        service\nmemory      128\n"
                "command     while true; do echo booted; sleep 2; done\nstarts      with Kosmos\n")

    # What `pkg install alpine` leaves beside an image: its manifest, which
    # is where its title comes from (`packages.lua`, `tools/packages.py`).
    with open(os.path.join(alpine, "package.txt"), "w") as f:
        f.write("name        alpine\ntitle       Alpine Linux\nkind        sandbox\n"
                "about       Alpine Linux 3.24.2 for a sandbox\n")

    for step in ([sys.executable, os.path.join(HERE, "alpine.py"), alpine],
                 [sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "128"]):
        made = subprocess.run(step, capture_output=True, text=True)

        if made.returncode != 0:
            print("FAIL: %s would not run:\n%s%s" % (os.path.basename(step[1]), made.stdout, made.stderr))
            return 1

    # A page on this Mac, for a sandbox's network to fetch through Kosmos's
    # (S4): QEMU gives Kosmos this Mac at 10.0.2.2.
    www = scratch.path("sandbox-www")
    os.makedirs(www, exist_ok=True)

    with open(os.path.join(www, "hello.txt"), "w") as f:
        f.write("hello from this Mac, through Kosmos\n")

    httpd = http.server.ThreadingHTTPServer(
        ("0.0.0.0", 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=www))
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    # **And the same over TLS** (13 October): a sandbox's HTTPS through the
    # gateway - `apk`'s way to its packages - with a certificate made here,
    # since what is tested is the connection and not who signed it.
    tls_dir = scratch.path("sandbox-tls")
    os.makedirs(tls_dir, exist_ok=True)
    key, cert = os.path.join(tls_dir, "key.pem"), os.path.join(tls_dir, "cert.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                    "-subj", "/CN=10.0.2.2", "-keyout", key, "-out", cert],
                   check=True, capture_output=True)
    httpsd = http.server.ThreadingHTTPServer(
        ("0.0.0.0", 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=www))
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    httpsd.socket = tls.wrap_socket(httpsd.socket, server_side=True)
    tls_port = httpsd.server_address[1]
    threading.Thread(target=httpsd.serve_forever, daemon=True).start()

    # And this Mac's port into Kosmos's 8081, which a sandbox's port is
    # carried from (S4, ports).
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        forwarded = probe.getsockname()[1]

    R.virtualize("1G")
    R.extra_args(image, ["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                         "-device", R.device(image, "blk") + ",drive=home",
                         "-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:8081" % forwarded,
                         "-device", R.device(image, "net") + ",netdev=net0"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def until(text, since, seconds):
        # The serial line ends "\r\n"; what is matched is what is checked.
        end = time.monotonic() + seconds
        while (text not in guest.seen[since:].replace("\r", "")
               and time.monotonic() < end):
            time.sleep(0.3)
            guest._read_available()
        return guest.seen[since:].replace("\r", "")

    def do(command, wait_for, seconds=60):
        at = len(guest.seen)
        guest.type(command)
        return until(wait_for, at, seconds)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        start = len(guest.seen)

        # The machine's RAM, asked of QEMU rather than assumed (SMBIOS type
        # 19, `hal/qemu-virt/memory.c`): it was 512 MB whatever `-m` said.
        check(re.search(r"Memory\s+\d+ of 1024 MB used", guest.seen) is not None,
              "a 1 GB machine was not seen as 1024 MB: %r"
              % re.findall(r"Memory\s+[^\n]*", guest.seen)[:1])

        # Started at boot, by `init`, and running by the time it is asked.
        found = None
        deadline = time.monotonic() + 180

        while found is None and time.monotonic() < deadline:
            said = do("sandbox list", "\nkosmos> ", 30)
            found = re.search(r"\nboot1\s+alpine\s+running", said)

            if found is None:
                time.sleep(3)

        check(found is not None, "boot1, which starts with Kosmos, was not running: %r" % said[-400:])
        do("sandbox stop boot1", "\nkosmos> ", 30)

        said = do("sandbox images", "\nkosmos> ", 30)
        check(re.search(r"\nalpine\s+Alpine Linux\s+\d+ MB", said) is not None,
              "sandbox images did not name Alpine: %r" % said[-300:])

        said = do("sandbox run alpine -- echo $((6*7)); uname -m; exit 3",
                  "the sandbox is gone", 240)
        check("\n42\n" in said and "\naarch64\n" in said,
              "the task's output did not come back: %r" % said[-600:])
        check(re.search(r"sandbox: echo exited 3 after [\d.]+ s, up in [\d.]+ s; the sandbox is gone",
                        said) is not None and "ended, code 3" in said,
              "the task's exit code, 3, was not the program's: %r" % said[-400:])
        check("kosmos-end-" not in said, "the guest's end line reached the console")

        said = do("sandbox run alpine", ":~# ", 240)
        check(re.search(r"\nshell-\d+:~# ", said) is not None,
              "a shell sandbox's prompt never came: %r" % said[-400:])
        said = do("cat /etc/alpine-release", "\n3.24.2\n", 30)
        check("\n3.24.2\n" in said, "the shell did not answer cat: %r" % said[-300:])
        said = do("exit", "\nkosmos> ", 60)
        check("\nkosmos> " in said, "exit did not end the shell's sandbox: %r" % said[-300:])

        do("sandbox new web --image alpine --service -- while true; do echo tick; sleep 1; done",
           "\nkosmos> ", 30)
        said = do("sandbox start web", "\nkosmos> ", 240)
        check("sandbox: web started" in said, "the service would not start: %r" % said[-300:])
        said = do("sandbox list", "\nkosmos> ", 30)
        check(re.search(r"\nweb\s+alpine\s+running\s+256 MB", said) is not None,
              "sandbox list did not show web running: %r" % said[-400:])

        at = len(guest.seen)
        guest.type("sandbox console web")
        said = until("tick\ntick\n", at, 60)
        check("tick\ntick\n" in said, "the service's console did not come, attached: %r" % said[-300:])
        guest.sendkey("ctrl-w")
        time.sleep(0.3)
        guest.type("d")
        said = until("\nkosmos> ", at, 30)
        check("left web running" in said, "Control-W then D did not leave it running: %r" % said[-300:])

        do("sandbox stop web", "\nkosmos> ", 30)
        time.sleep(3)
        said = do("sandbox list", "\nkosmos> ", 30)
        check(re.search(r"\nweb\s+alpine\s+stopped", said) is not None,
              "web was not stopped: %r" % said[-300:])

        # **A folder of /Home** (S3), by virtio-fs: read only - listed, read,
        # a write refused - then read and write, a file the guest made read
        # back by Kosmos.
        said = do("sandbox run alpine --share /Home/Projects/site:/work:ro -- "
                  "ls -R /work; cat /work/hello.txt; echo x > /work/new.txt",
                  "the sandbox is gone", 240)
        check("/work/pages:\none.txt" in said and "\nhello from Kosmos\n" in said,
              "a read-only folder was not listed and read: %r" % said[-500:])
        check("Read-only file system" in said, "a write to a read-only folder was not refused: %r" % said[-300:])
        said = do("sandbox run alpine --share /Home/Projects/site:/work -- "
                  "echo made in alpine > /work/out.txt; mkdir /work/d; ls /work",
                  "the sandbox is gone", 240)
        check(re.search(r"\nd\s+hello.txt\s+out.txt\s+pages\n", said) is not None,
              "a file and a folder made in a read-write folder were not there: %r" % said[-400:])
        # 120 s, not 30: under the whole gate's load a first `cat` answered
        # after 30 (13 October), with the right bytes.
        said = do("cat /Home/Projects/site/out.txt", "\nkosmos> ", 120)
        check("\nmade in alpine\n" in said, "what the guest wrote did not reach /Home: %r" % said[-300:])

        # **The network** (S4): the gateway pinged, a page on this Mac
        # fetched through Kosmos's own connection, and a name that cannot
        # exist answered NXDOMAIN by the monitor's name server - a question
        # asked of Kosmos's resolver, needing nothing beyond this Mac.
        do('for _ = 1, 100 do local i = fs.net_info("/Network") if i and i.address '
           'and i.address ~= "\\0\\0\\0\\0" then break end sys.sleep(25) end print("NET" .. "-UP")',
           "NET-UP", 60)
        said = do("sandbox run alpine --network -- ping -c 1 10.0.3.2; "
                  "wget -q -O- http://10.0.2.2:%d/hello.txt; nslookup nothing.invalid; "
                  "wget -q --no-check-certificate -O- https://10.0.2.2:%d/hello.txt | sed s/^/tls:/"
                  % (port, tls_port),
                  "the sandbox is gone", 240)
        check("64 bytes from 10.0.3.2" in said, "the gateway did not answer a ping: %r" % said[-500:])
        check("\nhello from this Mac, through Kosmos\n" in said,
              "a page on this Mac was not fetched through Kosmos: %r" % said[-500:])
        check("NXDOMAIN" in said, "the name server did not answer a name that cannot exist: %r" % said[-400:])
        check("\ntls:hello from this Mac, through Kosmos\n" in said,
              "a page on this Mac was not fetched over HTTPS through Kosmos: %r" % said[-500:])

        # What it carried, counted by the gateway and said at the end: the
        # page in at least, and the request for it out.
        carried = re.search(r"it carried ([\d.]+) (B|KB) in and ([\d.]+) (B|KB) out", said)
        check(carried is not None
              and float(carried.group(1)) * (1024 if carried.group(2) == "KB" else 1) >= 36
              and float(carried.group(3)) * (1024 if carried.group(4) == "KB" else 1) >= 40,
              "what the network carried was not counted: %r" % (carried.group(0) if carried else said[-300:]))

        # **A port carried in**: a sandbox given only Kosmos's 8081, for its
        # 80, answering once with busybox's `nc` - fetched from this Mac
        # through QEMU, Kosmos's stack, the monitor and the guest, and ended
        # by the guest closing it. Given no network beyond that, it cannot
        # reach out.
        at = len(guest.seen)
        guest.type("sandbox run alpine --port 8081:80 --share /Home/Projects/serve:/srv:ro -- "
                   "timeout 60 nc -l -p 80 -e sh /srv/serve.sh & sleep 1; "
                   "wget -q -T 3 -O- http://10.0.2.2:%d/hello.txt || echo NO-OUT\"\"GOING; "
                   "echo SERV\"\"ING; wait" % port)
        said = until("\nSERVING\n", at, 120)
        check("\nNO-OUTGOING\n" in said, "a sandbox given only a port reached out: %r" % said[-400:])
        page = b""

        for _ in range(10):
            try:
                with socket.create_connection(("127.0.0.1", forwarded), timeout=10) as c:
                    c.sendall(b"GET /index.html HTTP/1.0\r\n\r\n")
                    page = b""

                    while True:
                        got = c.recv(4096)

                        if not got:
                            break

                        page += got
            except OSError as e:
                page = repr(e).encode()

            if b"served by alpine" in page:
                break

            time.sleep(1)

        check(page == b"HTTP/1.0 200 OK\r\n\r\nserved by alpine\n",
              "a page in a sandbox was not fetched through Kosmos's port: %r" % page[-300:])
        until("the sandbox is gone", at, 60)

        check("\x1b" not in guest.seen[start:], "a terminal escape reached Kosmos's console")

        with open(os.path.join("build", "gate", "arm-sandbox-linux.ppm"), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    if failed:
        print("FAIL: %d of %d checks on sandboxes:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-4000:])
        return 1

    print("PASS: %d checks on sandboxes (Alpine Linux 3.24.2 at EL2 under Kosmos: its "
          "image listed; a task's output and its exit code, 3; a shell answering what "
          "was typed and ending with exit; a service started, found running, its "
          "console attached and left with Control-W D, and stopped; a folder of "
          "/Home read only and read-write, by virtio-fs; the network - a ping, a page "
          "fetched through Kosmos, a name answered; a port carried in, a page served from "
          "the guest)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
