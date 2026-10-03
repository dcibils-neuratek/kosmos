#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Run a command on a Kosmos machine, or fetch a file from it, over Telnet.

The Mac's half of `telnetd` (`user/bin/programs/telnetd.lua`; `roadmap.md`,
remote - Diego, 29 September: "Can we just implement a Telnet server and
client?"). A person can use `nc <address> 23` by hand; this is for what is
run from a script, where the end of a command's output has to be known
rather than seen: it is the next prompt.

  kosmos_telnet.py find                        which machines here run telnetd
  kosmos_telnet.py ADDRESS run "profile 30"    run it, print what it printed
  kosmos_telnet.py ADDRESS get PATH [OUT]      a file, whole, into OUT
  kosmos_telnet.py ADDRESS put FILE PATH       a file of this Mac's, to PATH
  kosmos_telnet.py ADDRESS push APP            an application written here,
                                               into /Home/Apps, and started
  kosmos_telnet.py ADDRESS restart [SECONDS]   restart it, wait for it to
                                               answer again, say which build

`push` takes a Lua file or a folder. A file `hello.lua` goes to
`/Home/Apps/hello/hello.lua`; a folder `Hello/` goes to `/Home/Apps/Hello/`
whole, its program the Lua file named after it. It is then opened on the
desktop if its header says `kosmos: application`, and run here otherwise,
so a program's output comes back (Diego, 29 September: "We could also even
write Lua apps in the Mac and push them to the m700").

ADDRESS may be `host:port`; the port is 23 otherwise. `run` exits with the
program's own exit code, which `telnetd` says when it is not zero.
"""

import base64
import concurrent.futures
import os
import re
import socket
import sys
import time

PROMPT = b"kosmos> "


class Session:
    def __init__(self, address, timeout=600.0):
        host, _, port = address.partition(":")
        self.sock = socket.create_connection((host, int(port or 23)), timeout=10)
        self.timeout = timeout
        self.buffer = b""
        self.until_prompt()                 # the banner, and the first prompt

    def until_prompt(self):
        """Everything up to the next prompt, which is how a command ends."""
        deadline = time.monotonic() + self.timeout

        while True:
            at = self.buffer.find(PROMPT)

            if at >= 0:
                said, self.buffer = self.buffer[:at], self.buffer[at + len(PROMPT):]
                return said.replace(b"\r\n", b"\n")

            if time.monotonic() > deadline:
                raise TimeoutError("no prompt after %d s; so far:\n%s"
                                   % (self.timeout, self.buffer[-600:].decode(errors="replace")))

            self.sock.settimeout(max(0.1, deadline - time.monotonic()))
            piece = self.sock.recv(65536)

            if not piece:
                raise ConnectionError("the machine closed the connection:\n"
                                      + self.buffer[-600:].decode(errors="replace"))

            self.buffer += piece

    def run(self, command):
        self.sock.sendall(command.encode() + b"\r\n")
        return self.until_prompt()

    def get(self, path):
        said = self.run("get " + path)
        found = re.search(rb"^BEGIN (\d+) .*?\n(.*?)^END$", said, re.M | re.S)

        if not found:
            raise IOError(said.decode(errors="replace").strip())

        data = base64.b64decode(b"".join(found.group(2).split()))

        if len(data) != int(found.group(1)):
            raise IOError("%s: %d bytes arrived of %s" % (path, len(data), found.group(1).decode()))

        return data

    def put(self, data, path):
        """`data`, whole, to `path` on the machine; what it said back."""
        text = base64.b64encode(data)
        lines = [text[at:at + 76] for at in range(0, len(text), 76)]
        self.sock.sendall(("put %s %d\r\n" % (path, len(data))).encode()
                          + b"".join(line + b"\r\n" for line in lines)
                          + b"END\r\n")
        said = self.until_prompt().decode(errors="replace")

        if ("put: %s, %d bytes" % (path, len(data))) not in said:
            raise IOError(said.strip())

        return said

    def close(self):
        try:
            self.sock.sendall(b"exit\r\n")
        except OSError:
            pass

        self.sock.close()


def local_prefix():
    """This Mac's address on the network, as its first three numbers."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        probe.connect(("192.0.2.1", 9))      # nothing is sent: it picks a route
        return probe.getsockname()[0].rsplit(".", 1)[0]
    finally:
        probe.close()


def answers(host):
    """Whether a Kosmos telnetd is at this address: its banner says so."""
    try:
        with socket.create_connection((host, 23), timeout=0.4) as s:
            # The banner is `neofetch`, a dozen seconds on the M700 with its
            # stick; the version line is what is wanted, and enough.
            s.settimeout(30)
            seen = b""

            while (PROMPT not in seen and len(seen) < 8192
                   and re.search(rb"Kosmos \d+\.\d+\.\d+", seen) is None):
                piece = s.recv(4096)

                if not piece:
                    break

                seen += piece

            if b"Kosmos" in seen:
                version = re.search(rb"Kosmos (\d+\.\d+\.\d+)", seen)
                return host, version.group(1).decode() if version else "?"
    except OSError:
        pass

    return None


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2

    if argv[0] == "find":
        prefix = local_prefix()

        with concurrent.futures.ThreadPoolExecutor(64) as pool:
            found = [r for r in pool.map(answers, ["%s.%d" % (prefix, n)
                                                   for n in range(1, 255)]) if r]

        for host, version in found:
            print("%s  Kosmos %s" % (host, version))

        if not found:
            print("no telnetd on %s.0/24" % prefix)

        return 0 if found else 1

    if len(argv) >= 2 and argv[1] == "restart":
        return restart(argv[0], float(argv[2]) if len(argv) > 2 else 300.0)

    if len(argv) < 3:
        print(__doc__)
        return 2

    address, verb = argv[0], argv[1]
    session = Session(address)

    try:
        if verb == "run":
            said = session.run(" ".join(argv[2:]))
            sys.stdout.write(said.decode(errors="replace"))
            code = re.search(rb"\(exit code (-?\d+)\)\s*$", said)
            return int(code.group(1)) if code else 0

        if verb == "get":
            data = session.get(argv[2])
            out = argv[3] if len(argv) > 3 else os.path.basename(argv[2])

            with open(out, "wb") as f:
                f.write(data)

            print("%s: %d bytes -> %s" % (argv[2], len(data), out))
            return 0

        if verb == "put" and len(argv) > 3:
            with open(argv[2], "rb") as f:
                sys.stdout.write(session.put(f.read(), argv[3]))
            return 0

        if verb == "push":
            return push(session, argv[2])

        print(__doc__)
        return 2
    finally:
        session.close()


def restart(address, seconds):
    """**Restart the machine, and wait for it to come back** (`roadmap.md`,
    build, boot and test the M700 in a loop; `testing.md` 18.352).

    `restart` at its prompt, then the machine gone - `telnetd` not answering
    - and then answering again, with the build its banner names. A machine
    on network boot comes back with whatever the Mac serves now, so the
    version is the proof that the new build is the one running. Not coming
    back within `seconds` is a result too, and said.
    """
    host = address.partition(":")[0]
    before = answers(host)

    if before is None:
        print("restart: no Kosmos telnetd at %s" % host)
        return 1

    # The banner is `neofetch`, which takes a dozen seconds on the M700 with
    # `/Home` on its stick; a shorter wait gave up before asking.
    session = Session(address, timeout=60.0)
    asked = time.monotonic()
    session.sock.sendall(b"restart\r\n")

    try:
        session.until_prompt()
    except (ConnectionError, TimeoutError, OSError):
        pass                                # gone, which is the point

    try:
        session.sock.close()
    except OSError:
        pass

    started = asked                         # from the asking, not the hanging up
    gone = False

    while time.monotonic() - started < seconds:
        now = answers(host)

        if now is None:
            gone = True
        elif gone:
            print("restart: Kosmos %s -> Kosmos %s, answering again after %.0f s"
                  % (before[1], now[1], time.monotonic() - started))
            return 0

        time.sleep(2.0)

    print("restart: %s did not %s within %.0f s"
          % (host, "come back" if gone else "go away", seconds))
    return 1


def push(session, local):
    """An application from this Mac into /Home/Apps, then started."""
    local = local.rstrip("/")
    name = os.path.splitext(os.path.basename(local))[0]
    folder = "/Home/Apps/" + name

    if os.path.isdir(local):
        files = []

        for top, _, names in os.walk(local):
            for n in sorted(names):
                if not n.startswith("."):
                    files.append(os.path.join(top, n))

        program = os.path.join(local, name.lower() + ".lua")

        if program not in files:
            program = next((f for f in files if f.endswith(".lua")), None)
    else:
        files = [local]
        program = local

    if not program:
        print("push: %s has no Lua file to run" % local)
        return 2

    for f in files:
        remote = folder + "/" + os.path.relpath(f, local if os.path.isdir(local)
                                                else os.path.dirname(f))

        with open(f, "rb") as h:
            sys.stdout.write(session.put(h.read(), remote))

    remote_program = folder + "/" + os.path.relpath(program, local if os.path.isdir(local)
                                                    else os.path.dirname(program))

    with open(program, "rb") as h:
        head = h.read(2048).decode(errors="replace")

    windowed = re.search(r"^--\s*kosmos:\s*application\b", head, re.M) is not None
    said = session.run(("open " if windowed else "") + remote_program)
    sys.stdout.write(said.decode(errors="replace"))
    code = re.search(rb"\(exit code (-?\d+)\)\s*$", said)

    return int(code.group(1)) if code else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
