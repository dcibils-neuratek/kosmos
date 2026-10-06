#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A relay between a Kosmos machine and an SMB server, which changes one byte.

    python3 tools/smbrelay.py --listen 4489 --to 4484
        then a line on its input: `arm` changes the next answer that
        qualifies, `off` stops, `quit` ends it

`docs/sharing.md` step N4, the control that bites. The machine connects to
the relay's port on this Mac's loopback (10.0.2.2 from the guest), and the
relay forwards every byte to the peer and back - unchanged, until it is
**armed**. Armed, it changes **one byte** of the next answer from the server
that qualifies, and then is disarmed again:

  - **a signed answer**: an SMB 2 message (`FE 'SMB'`), a READ's successful
    reply of at least `least` bytes - its last byte, which is the file's;
  - **a sealed answer**: an SMB 3 transform (`FD 'SMB'`) of at least
    `least` bytes - its last byte, which is ciphertext, so the sealed READ
    inside it would open with one byte of the file changed if the seal's
    tag were not checked.

Everything else passes as it came, so with the relay never armed a session
through it is a session to the peer: that is the control's other half,
which the suite holds too - a relay that broke things whether armed or not
would prove nothing.

The relay reads the server's side a whole message at a time (the four-byte
length SMB 2 frames every message with over TCP), so it can find the byte
to change; the client's side it passes as it arrives. It is used by
`tools/run_share.py`'s first part in the same process, as `Relay`, armed
between one command typed into the machine and the next.
"""

import argparse
import socket
import struct
import sys
import threading

READ = 0x0008
FROM_SERVER = 0x00000001        # SMB2_FLAGS_SERVER_TO_REDIR
STATUS_PENDING = 0x00000103


class Relay:
    def __init__(self, listen, to, least=4096):
        self.to = to
        self.least = least
        self.armed = 0
        self.skip = 0               # qualifying answers to let pass first
        self.altered = []           # (kind, length, offset) of each change
        self.dialect = None         # what the server chose, from NEGOTIATE
        self.signed = 0             # READ answers signed,
        self.sealed = 0             # transforms, whatever is inside,
        self.plain = 0              # READ answers neither
        self.pending = 0            # interim READ answers, not counted
        self.passed = 0             # messages from the server passed unchanged
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", listen))
        self.sock.listen(8)
        self.port = listen
        self.alive = True
        threading.Thread(target=self.accept, daemon=True).start()

    def arm(self, count=1, skip=0):
        """Change the next `count` answers that qualify, after `skip` of
        them have passed unchanged."""
        with self.lock:
            self.armed = count
            self.skip = skip

    def seen(self):
        """What crossed: the dialect, and how the answers were carried."""
        with self.lock:
            return {"dialect": self.dialect, "signed": self.signed,
                    "sealed": self.sealed, "plain": self.plain}

    def off(self):
        with self.lock:
            self.armed = 0

    def close(self):
        self.alive = False

        try:
            self.sock.close()
        except OSError:
            pass

    def accept(self):
        while self.alive:
            try:
                client, _ = self.sock.accept()
            except OSError:
                return

            try:
                server = socket.create_connection(("127.0.0.1", self.to), timeout=10)
                server.settimeout(None)
            except OSError:
                client.close()
                continue

            for s in (client, server):
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

            threading.Thread(target=self.upward, args=(client, server),
                             daemon=True).start()
            threading.Thread(target=self.downward, args=(server, client),
                             daemon=True).start()

    @staticmethod
    def shut(*socks):
        for s in socks:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

            try:
                s.close()
            except OSError:
                pass

    def upward(self, client, server):
        """The machine's side, to the server, as it arrives."""
        try:
            while True:
                data = client.recv(65536)

                if not data:
                    break

                server.sendall(data)
        except OSError:
            pass

        self.shut(client, server)

    @staticmethod
    def exactly(sock, n):
        out = bytearray()

        while len(out) < n:
            got = sock.recv(n - len(out))

            if not got:
                return None

            out += got

        return bytes(out)

    DIALECTS = {0x0202: "2.0.2", 0x0210: "2.1", 0x0300: "3.0",
                0x0302: "3.0.2", 0x0311: "3.1.1"}

    def watch(self, message):
        """How an answer was carried: the dialect from NEGOTIATE's, and each
        READ's answer - the file's bytes - signed or not, and every transform
        counted as sealed (what is inside one cannot be seen, which is the
        point of it). That is what `share status` is held to."""
        if message[:4] == b"\xfdSMB":
            self.sealed += 1
            return

        if message[:4] != b"\xfeSMB" or len(message) < 64:
            return

        status, command = struct.unpack_from("<IH", message, 8)
        flags = struct.unpack_from("<I", message, 16)[0]

        if command == 0 and len(message) >= 70:
            revision = struct.unpack_from("<H", message, 68)[0]
            self.dialect = self.DIALECTS.get(revision, "0x%04x" % revision)
        elif command != READ:
            pass
        elif status == STATUS_PENDING:
            # An interim answer - "I am still reading" - which is not
            # signed, and which Samba sends once a read
            # goes asynchronous, as one does on a loaded machine. It carries
            # no bytes; the final answer that does is the one counted.
            self.pending += 1
        elif flags & 0x00000008:            # SMB2_FLAGS_SIGNED
            self.signed += 1
        else:
            self.plain += 1

    def qualifies(self, message):
        """Which kind of answer this is, if it is one to change."""
        if len(message) < self.least:
            return None

        if message[:4] == b"\xfdSMB":
            return "sealed"

        if message[:4] == b"\xfeSMB" and len(message) >= 64:
            status, command = struct.unpack_from("<IH", message, 8)
            flags = struct.unpack_from("<I", message, 16)[0]

            if command == READ and status == 0 and flags & FROM_SERVER:
                return "signed"

        return None

    def downward(self, server, client):
        """The server's side, a message at a time: the one to change, changed."""
        try:
            while True:
                head = self.exactly(server, 4)

                if head is None:
                    break

                length = struct.unpack(">I", head)[0] & 0xFFFFFF
                body = self.exactly(server, length)

                if body is None:
                    break

                kind = self.qualifies(body)

                with self.lock:
                    self.watch(body)
                    change = False

                    if kind is not None and self.armed > 0:
                        if self.skip > 0:
                            self.skip -= 1
                        else:
                            change = True
                            self.armed -= 1

                if change:
                    body = bytearray(body)
                    body[-1] ^= 0x01
                    body = bytes(body)
                    self.altered.append((kind, length, length - 1))
                else:
                    self.passed += 1

                client.sendall(head + body)
        except OSError:
            pass

        self.shut(server, client)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--listen", type=int, required=True)
    ap.add_argument("--to", type=int, required=True)
    ap.add_argument("--least", type=int, default=4096)
    args = ap.parse_args()

    relay = Relay(args.listen, args.to, args.least)
    print("smbrelay: 127.0.0.1:%d to 127.0.0.1:%d; arm, off or quit"
          % (args.listen, args.to), flush=True)

    for line in sys.stdin:
        word = line.strip()

        if word == "arm":
            relay.arm()
        elif word == "off":
            relay.off()
        elif word == "quit":
            break

        print("smbrelay: armed %d, changed %d, passed %d"
              % (relay.armed, len(relay.altered), relay.passed), flush=True)

    relay.close()


if __name__ == "__main__":
    main()
