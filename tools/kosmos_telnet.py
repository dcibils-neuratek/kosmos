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
            s.settimeout(3)
            seen = b""

            while PROMPT not in seen and len(seen) < 8192:
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

        print(__doc__)
        return 2
    finally:
        session.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
