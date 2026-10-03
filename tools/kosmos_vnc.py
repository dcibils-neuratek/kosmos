#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A VNC viewer without a window: a Kosmos machine's screen, as a picture.

The Mac's half of `vncd` for a script (`user/bin/programs/vncd.lua`;
`roadmap.md` remote 7). A person uses any VNC viewer - Diego, 29 September:
"i can use any other open vnc clients in the mac", "there are many available
free". This is for what is checked, or taken, from here:

  kosmos_vnc.py ADDRESS shot OUT.png [--password P]    the screen, as a PNG
  kosmos_vnc.py ADDRESS do "STEP; STEP; ..." [--password P]
                                                       several, on one connection

A STEP is one of:

  shot FILE.png          the screen as it is now
  click X Y              the left button, down and up, at X,Y
  rclick X Y             the right button
  dclick X Y             two clicks
  move X Y               the pointer there, no button
  drag X1 Y1 X2 Y2       the left button held from one place to the other
  type TEXT              the rest of the step, as keys (`\\n` is Return)
  key NAME[+NAME...]     Return, Escape, Tab, BackSpace, Delete, Home, End,
                         Up, Down, Left, Right, PageUp, PageDown, F1-F12,
                         a character - with ctrl, shift, alt or super held
                         first: `key ctrl+s`, `key super+Tab`
  wait SECONDS           for the machine to draw what was asked

`click`, `type`, `key` and the rest may also be given alone, without `do`.
Keys and the pointer reach the desktop only when the Servers window lets a
viewer use them; otherwise the machine looks and does not listen.

ADDRESS may be `host:port`; the port is 5900 otherwise. RFB 3.3, the Raw
encoding, and VNC Authentication when the machine asks for it, answered with
OpenSSL's DES - a second implementation beside the one `vncd` checks with,
which is what makes a test of it mean something.
"""

import os
import socket
import struct
import subprocess
import sys
import time
import zlib


class RFBError(Exception):
    pass


def vnc_answer(password, challenge):
    """VNC Authentication: the challenge under DES, the password's bits
    reversed a byte at a time as the protocol has it."""
    key = bytes(int(f"{b:08b}"[::-1], 2) for b in password.encode()[:8].ljust(8, b"\0"))
    out = subprocess.run(["openssl", "enc", "-des-ecb", "-provider", "legacy",
                          "-provider", "default", "-K", key.hex(), "-nopad"],
                         input=challenge, capture_output=True)

    if len(out.stdout) != 16:
        raise RFBError("openssl could not answer the challenge: "
                       + out.stderr.decode(errors="replace")[:200])

    return out.stdout


class Viewer:
    """One connection: the handshake, then updates into `self.frame`, a
    bytearray of 0xRRGGBB pixels three bytes each, row after row."""

    def __init__(self, address, password=None, answer=None, timeout=30):
        host, _, port = address.partition(":")
        self.sock = socket.create_connection((host, int(port or 5900)), timeout=timeout)
        self.version = self.exact(12)

        if not self.version.startswith(b"RFB "):
            raise RFBError("not a VNC server: %r" % self.version)

        self.sock.sendall(b"RFB 003.003\n")
        (self.security,) = struct.unpack(">I", self.exact(4))

        if self.security == 0:
            (n,) = struct.unpack(">I", self.exact(4))
            raise RFBError("refused: " + self.exact(n).decode(errors="replace"))

        if self.security == 2:
            challenge = self.exact(16)

            if answer is None:
                if password is None:
                    raise RFBError("the machine asks for a password")

                answer = vnc_answer(password, challenge)

            self.sock.sendall(answer)
            (result,) = struct.unpack(">I", self.exact(4))

            if result != 0:
                raise RFBError("the password was refused")
        elif self.security != 1:
            raise RFBError("a security type this does not speak: %d" % self.security)

        self.sock.sendall(b"\x01")          # ClientInit: shared
        self.width, self.height = struct.unpack(">HH", self.exact(4))
        self.server_format = self.exact(16)
        (n,) = struct.unpack(">I", self.exact(4))
        self.name = self.exact(n).decode(errors="replace")
        self.frame = bytearray(self.width * self.height * 3)
        self.format = dict(bpp=32, big=False, rmax=255, gmax=255, bmax=255,
                           rshift=16, gshift=8, bshift=0)
        self.sock.sendall(struct.pack(">BxH i", 2, 1, 0))   # SetEncodings: Raw

    def exact(self, n):
        data = b""

        while len(data) < n:
            piece = self.sock.recv(n - len(data))

            if not piece:
                raise RFBError("the machine closed the connection after %d of "
                               "%d bytes" % (len(data), n))

            data += piece

        return data

    def set_format(self, bpp, big, rmax, gmax, bmax, rshift, gshift, bshift):
        depth = 24 if bpp == 32 else bpp
        self.format = dict(bpp=bpp, big=big, rmax=rmax, gmax=gmax, bmax=bmax,
                           rshift=rshift, gshift=gshift, bshift=bshift)
        self.sock.sendall(struct.pack(">Bxxx BBBB HHH BBB xxx", 0, bpp, depth,
                                      1 if big else 0, 1, rmax, gmax, bmax,
                                      rshift, gshift, bshift))

    def key(self, keysym, down):
        self.sock.sendall(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    def pointer(self, x, y, mask=0):
        self.sock.sendall(struct.pack(">BBHH", 5, mask, x, y))

    def type_text(self, text):
        """Keys as a viewer sends them: Shift held round a capital or a
        shifted mark, Return for a newline."""
        for ch in text:
            if ch == "\n":
                self.key(0xff0d, True)
                self.key(0xff0d, False)
                continue

            shifted = ch.isupper() or ch in '~!@#$%^&*()_+{}|:"<>?'

            if shifted:
                self.key(0xffe1, True)

            self.key(ord(ch), True)
            self.key(ord(ch), False)

            if shifted:
                self.key(0xffe1, False)

    def request(self, incremental, x=0, y=0, w=None, h=None):
        self.sock.sendall(struct.pack(">BBHHHH", 3, 1 if incremental else 0, x, y,
                                      self.width if w is None else w,
                                      self.height if h is None else h))

    def update(self):
        """One FramebufferUpdate, into the frame; its rectangles returned."""
        while True:
            kind = self.exact(1)[0]

            if kind == 0:
                break

            if kind == 2:                           # Bell
                continue

            if kind == 3:                           # ServerCutText
                self.exact(3)
                (n,) = struct.unpack(">I", self.exact(4))
                self.exact(n)
                continue

            raise RFBError("a server message this does not know: %d" % kind)

        self.exact(1)
        (count,) = struct.unpack(">H", self.exact(2))
        rects = []
        f = self.format
        per = f["bpp"] // 8
        order = ">" if f["big"] else "<"
        code = {1: "B", 2: "H", 4: "I"}[per]

        for _ in range(count):
            x, y, w, h, encoding = struct.unpack(">HHHHi", self.exact(12))

            if encoding != 0:
                raise RFBError("an encoding this did not ask for: %d" % encoding)

            data = self.exact(w * h * per)

            # Whole bytes a channel, 32 bits: sliced, not unpacked - a
            # frame is two million pixels and Python's loop is not.
            if (per == 4 and f["rmax"] == f["gmax"] == f["bmax"] == 255
                    and all(f[k] % 8 == 0 for k in ("rshift", "gshift", "bshift"))):
                def lane(shift):
                    return (shift // 8) if not f["big"] else 3 - shift // 8

                for row in range(h):
                    line = data[row * w * 4:(row + 1) * w * 4]
                    at = ((y + row) * self.width + x) * 3
                    out = bytearray(w * 3)
                    out[0::3] = line[lane(f["rshift"])::4]
                    out[1::3] = line[lane(f["gshift"])::4]
                    out[2::3] = line[lane(f["bshift"])::4]
                    self.frame[at:at + w * 3] = out

                rects.append((x, y, w, h))
                continue

            values = struct.unpack("%s%d%s" % (order, w * h, code), data)

            for row in range(h):
                at = ((y + row) * self.width + x) * 3

                for col in range(w):
                    v = values[row * w + col]
                    r = ((v >> f["rshift"]) & f["rmax"]) * 255 // f["rmax"]
                    g = ((v >> f["gshift"]) & f["gmax"]) * 255 // f["gmax"]
                    b = ((v >> f["bshift"]) & f["bmax"]) * 255 // f["bmax"]
                    self.frame[at:at + 3] = bytes((r, g, b))
                    at += 3

            rects.append((x, y, w, h))

        return rects

    def close(self):
        self.sock.close()


def png(path, width, height, rgb):
    rows = b"".join(b"\0" + bytes(rgb[y * width * 3:(y + 1) * width * 3])
                    for y in range(height))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(rows, 6))
                + chunk(b"IEND", b""))


# X keysyms (`keysymdef.h`) by the names a step uses: the ones `vncd`
# turns into a keyboard's keys.
KEYS = {
    "return": 0xff0d, "enter": 0xff0d, "escape": 0xff1b, "esc": 0xff1b,
    "tab": 0xff09, "backspace": 0xff08, "delete": 0xffff, "insert": 0xff63,
    "home": 0xff50, "end": 0xff57, "left": 0xff51, "up": 0xff52,
    "right": 0xff53, "down": 0xff54, "pageup": 0xff55, "pagedown": 0xff56,
    "space": 0x20,
}
KEYS.update({"f%d" % n: 0xffbe + n - 1 for n in range(1, 13)})
MODIFIERS = {"shift": 0xffe1, "ctrl": 0xffe3, "control": 0xffe3,
             "alt": 0xffe9, "super": 0xffeb, "cmd": 0xffeb}


def keysym(name):
    if name.lower() in KEYS:
        return KEYS[name.lower()]

    if len(name) == 1:
        return ord(name)

    raise RFBError("no key called %r" % name)


def step(viewer, text):
    """One step of `do`; what it said, if anything."""
    verb, _, rest = text.strip().partition(" ")
    words = rest.split()

    def numbers(n):
        if len(words) != n:
            raise RFBError("%s takes %d numbers: %r" % (verb, n, text))

        return [int(w) for w in words]

    if verb == "shot":
        viewer.request(False)
        viewer.update()
        png(rest.strip(), viewer.width, viewer.height, viewer.frame)
        return "%dx%d -> %s" % (viewer.width, viewer.height,
                                os.path.abspath(rest.strip()))
    elif verb in ("click", "rclick", "dclick"):
        x, y = numbers(2)
        button = 4 if verb == "rclick" else 1
        viewer.pointer(x, y)

        for _ in range(2 if verb == "dclick" else 1):
            viewer.pointer(x, y, button)
            viewer.pointer(x, y, 0)
    elif verb == "move":
        viewer.pointer(*numbers(2))
    elif verb == "drag":
        # Spaced as a hand moves, as the browser's resize test drags its
        # grip: `vncd` folds pointer events with the same buttons into the
        # last, so a press and its moves sent at once arrive as one place.
        x1, y1, x2, y2 = numbers(4)
        viewer.pointer(x1, y1)
        time.sleep(0.3)
        viewer.pointer(x1, y1, 1)
        time.sleep(0.3)

        for i in range(1, 9):
            viewer.pointer(x1 + (x2 - x1) * i // 8, y1 + (y2 - y1) * i // 8, 1)
            time.sleep(0.1)

        time.sleep(0.3)
        viewer.pointer(x2, y2, 0)
    elif verb == "type":
        viewer.type_text(rest.replace("\\n", "\n"))
    elif verb == "key":
        names = rest.strip().split("+")
        held = [MODIFIERS[n.lower()] for n in names[:-1] if n.lower() in MODIFIERS]

        if len(held) != len(names) - 1:
            raise RFBError("held keys are shift, ctrl, alt and super: %r" % text)

        for k in held:
            viewer.key(k, True)

        k = keysym(names[-1])
        viewer.key(k, True)
        viewer.key(k, False)

        for k in reversed(held):
            viewer.key(k, False)
    elif verb == "wait":
        time.sleep(float(rest))
    else:
        raise RFBError("no step called %r" % verb)

    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2

    password = None

    if "--password" in argv:
        at = argv.index("--password")
        password = argv[at + 1]
        argv = argv[:at] + argv[at + 2:]

    address, verb = argv[0], argv[1]

    if verb == "do" and len(argv) == 3:
        steps = [s for s in argv[2].split(";") if s.strip()]
    elif verb in ("shot", "click", "rclick", "dclick", "move", "drag", "type",
                  "key", "wait") and len(argv) >= 3:
        steps = [" ".join(argv[1:])]
    else:
        print(__doc__)
        return 2

    viewer = Viewer(address, password=password)

    try:
        for s in steps:
            said = step(viewer, s)

            if said:
                print("%s: %s" % (address, said))
    except RFBError as e:
        print("%s: %s" % (address, e))
        return 1
    finally:
        viewer.close()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
