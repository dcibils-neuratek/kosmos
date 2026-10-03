#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A Kosmos machine's screen in a window on the Mac, to look at and use.

Diego, 3 October 2026: "Can we do a small and fast vnc client in python for
macOS using all the code we have already?", "So I can use it to connect to
my m700 remotely". `kosmos_vnc.py` already speaks RFB - the handshake, the
password, Raw updates into a frame - for a script; this puts that frame in a
Tk window and sends the window's keys, buttons and wheel back.

  python3 tools/kosmos_view.py                   the first Kosmos machine here
  python3 tools/kosmos_view.py 192.168.1.40      that one
  python3 tools/kosmos_view.py ADDRESS --half    at half its size
  python3 tools/kosmos_view.py ADDRESS --password SECRET

**It finds the machine and starts its screen.** With no address, the first
machine on this network whose `telnetd` says Kosmos. If nothing answers on
the screen's port, `open vncd` is asked over Telnet - so after a restart,
which every turn of the build, boot and test loop is, the window comes back
by itself. **The size** is the machine's, or half of it when the machine's
is larger than this screen (`--full` and `--half` say which).

**Keys and the pointer reach the desktop only where it lends them**: the
Servers window's switch, or `opt/kosmos/vnc=control` on its command line,
which `make netboot` serves. Otherwise the window shows and the machine does
not listen.

**What is drawn is what changed.** A thread reads the machine and hands the
window each updated rectangle as a picture; the window copies it into place.
Tk 9 (`brew install tcl-tk`, and `python-tk@3.14` for Python's half of it).
"""

import os
import queue
import socket
import struct
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import kosmos_telnet as T                                       # noqa: E402
import kosmos_vnc as V                                          # noqa: E402

try:
    import tkinter as tk
except ImportError:
    sys.exit("kosmos_view: Python here has no Tk - "
             "`brew install tcl-tk python-tk@3.14`")


class Locked:
    """A socket two threads write to: the reader asks for updates while the
    window sends keys and the pointer, and a message is never split."""

    def __init__(self, sock):
        self._sock = sock
        self._lock = threading.Lock()

    def sendall(self, data):
        with self._lock:
            self._sock.sendall(data)

    def __getattr__(self, name):
        return getattr(self._sock, name)


def find_machine():
    """The first machine on this network whose telnetd says Kosmos."""
    import concurrent.futures

    prefix = T.local_prefix()

    with concurrent.futures.ThreadPoolExecutor(64) as pool:
        for found in pool.map(T.answers, ["%s.%d" % (prefix, i) for i in range(1, 255)]):
            if found:
                return found[0]

    return None


def start_screen(host):
    """`open vncd` on the machine, over Telnet; whether it said it started."""
    try:
        session = T.Session(host, timeout=20.0)
        said = session.run("open vncd").decode(errors="replace")
        session.sock.close()
        return "started" in said or "already" in said
    except (OSError, ConnectionError, TimeoutError):
        return False


def ppm(frame, width, x, y, w, h):
    """A rectangle of the frame as a binary PPM, which Tk reads directly."""
    rows = b"".join(bytes(frame[((y + r) * width + x) * 3:((y + r) * width + x + w) * 3])
                    for r in range(h))
    return b"P6\n%d %d\n255\n" % (w, h) + rows


class Reader(threading.Thread):
    """The machine's side: connect, keep the frame, and put each changed
    rectangle - as a picture - on the queue for the window."""

    def __init__(self, host, port, password, out):
        super().__init__(daemon=True)
        self.host, self.port, self.password, self.out = host, port, password, out
        self.viewer = None
        self.half = None                # set by the window once it knows
        self.ready = threading.Event()

    def run(self):
        asked_at = 0.0

        while True:
            try:
                viewer = V.Viewer("%s:%d" % (self.host, self.port),
                                  password=self.password, timeout=15)
            except ConnectionRefusedError:
                if time.monotonic() - asked_at > 15:
                    asked_at = time.monotonic()
                    self.out.put(("status", "starting the screen on %s" % self.host))
                    start_screen(self.host)
                time.sleep(2)
                continue
            except (OSError, V.RFBError) as e:
                self.out.put(("status", "%s: %s - trying again" % (self.host, e)))
                time.sleep(3)
                continue

            viewer.sock = Locked(viewer.sock)
            viewer.sock.settimeout(None)
            self.viewer = viewer
            self.out.put(("size", viewer.width, viewer.height, viewer.name))
            self.ready.wait()

            try:
                viewer.request(False)

                while True:
                    rects = viewer.update()
                    step = 2 if self.half else 1

                    for x, y, w, h in rects:
                        if step == 2:       # whole pairs, so halving is exact
                            x1, y1 = min(viewer.width, x + w + (x + w) % 2), \
                                     min(viewer.height, y + h + (y + h) % 2)
                            x, y = x - x % 2, y - y % 2
                            w, h = x1 - x, y1 - y

                        self.out.put(("rect", x, y, w, h,
                                      ppm(viewer.frame, viewer.width, x, y, w, h)))

                    viewer.request(True)
            except (OSError, V.RFBError, struct.error) as e:
                self.viewer = None
                self.out.put(("status", "%s gone (%s) - waiting for it" % (self.host, e)))

                try:
                    viewer.close()
                except OSError:
                    pass

                time.sleep(2)


class Window:
    def __init__(self, host, port, password, size):
        self.root = tk.Tk()
        self.root.title("Kosmos - %s" % host)
        self.root.configure(background="#1d2a44")
        self.host, self.size = host, size
        self.queue = queue.Queue()
        self.reader = Reader(host, port, password, self.queue)
        self.image = None
        self.shown = None
        self.label = tk.Label(self.root, background="#1d2a44", foreground="#c9d3e6",
                              text="looking for %s's screen..." % host,
                              font=("Menlo", 13), padx=40, pady=40, borderwidth=0)
        self.label.pack(fill="both", expand=True)
        self.scale = 1
        self.mask = 0
        self.at = None                  # where the pointer is, to send
        self.sent_at = None
        self.held = set()               # keys down, let go on losing focus
        aqua = self.root.tk.call("tk", "windowingsystem") == "aqua"
        # Tk 9 numbers a Mac's buttons as everywhere else; 8 swapped 2 and 3.
        self.right = 3 if (not aqua or tk.TkVersion >= 9) else 2
        self.bind()
        self.reader.start()
        self.root.after(10, self.pump)

    def bind(self):
        r = self.root
        r.bind("<Motion>", self.motion)
        r.bind("<ButtonPress>", self.press)
        r.bind("<ButtonRelease>", self.release)
        r.bind("<MouseWheel>", self.wheel)
        r.bind("<KeyPress>", lambda e: self.key(e, True))
        r.bind("<KeyRelease>", lambda e: self.key(e, False))
        r.bind("<FocusOut>", self.let_go)

        try:
            r.bind("<TouchpadScroll>", self.touchpad)
        except tk.TclError:
            pass                        # Tk before 9 has no such event

    # -- the machine's pictures, into the window --

    def pump(self):
        try:
            for _ in range(400):
                item = self.queue.get_nowait()

                if item[0] == "size":
                    self.sized(*item[1:])
                elif item[0] == "rect":
                    self.draw(*item[1:])
                elif item[0] == "status":
                    self.root.title("Kosmos - %s - %s" % (self.host, item[1]))
        except queue.Empty:
            pass

        self.send_pointer()
        self.root.after(10, self.pump)

    def sized(self, width, height, name):
        fits = (width <= self.root.winfo_screenwidth() * 0.96
                and height <= self.root.winfo_screenheight() * 0.90)
        self.scale = 2 if self.size == "half" or (self.size is None and not fits) else 1
        self.reader.half = self.scale == 2
        self.image = tk.PhotoImage(width=width // self.scale, height=height // self.scale)
        self.label.configure(image=self.image, text="", padx=0, pady=0)
        self.root.title("Kosmos - %s - %s, %dx%d%s" % (self.host, name, width, height,
                                                      " at half" if self.scale == 2 else ""))
        self.root.focus_force()
        self.reader.ready.set()

    def draw(self, x, y, w, h, data):
        piece = tk.PhotoImage(data=data, format="PPM")

        if self.scale == 1:
            self.image.tk.call(self.image, "copy", piece, "-to", x, y)
        else:
            self.image.tk.call(self.image, "copy", piece, "-to", x // 2, y // 2,
                               "-subsample", 2, 2)

    # -- the window's keys and pointer, to the machine --

    def where(self, event):
        v = self.reader.viewer

        if v is None:
            return None

        x = max(0, min(v.width - 1, event.x * self.scale))
        y = max(0, min(v.height - 1, event.y * self.scale))
        return x, y

    def send(self, fn, *args):
        v = self.reader.viewer

        if v is not None:
            try:
                fn(v, *args)
            except OSError:
                pass

    def motion(self, event):
        self.at = self.where(event)

    def send_pointer(self):
        """The newest place, once a pass: a move is a message, and the
        machine needs where the pointer is, not every place it went."""
        if self.at is not None and self.at != self.sent_at:
            self.sent_at = self.at
            self.send(V.Viewer.pointer, self.at[0], self.at[1], self.mask)

    def button_bit(self, num):
        if num == 1:
            return 1
        if num == self.right:
            return 4
        if num in (2, 3):
            return 2
        return 0

    def press(self, event):
        self.at = self.where(event)

        if self.at is not None:
            self.mask |= self.button_bit(event.num)
            self.sent_at = self.at
            self.send(V.Viewer.pointer, self.at[0], self.at[1], self.mask)

    def release(self, event):
        self.at = self.where(event)

        if self.at is not None:
            self.mask &= ~self.button_bit(event.num)
            self.sent_at = self.at
            self.send(V.Viewer.pointer, self.at[0], self.at[1], self.mask)

    def notch(self, up):
        """A wheel's notch: RFB's button 4 or 5, pressed and let go."""
        if self.at is None:
            return

        bit = 8 if up else 16
        self.send(V.Viewer.pointer, self.at[0], self.at[1], self.mask | bit)
        self.send(V.Viewer.pointer, self.at[0], self.at[1], self.mask)

    def wheel(self, event):
        self.at = self.where(event) or self.at

        if event.delta:
            self.notch(event.delta > 0)

    def touchpad(self, event):
        """A trackpad's scroll, in Tk 9: many small deltas, a notch for every
        twenty points of them."""
        self.at = self.where(event) or self.at

        try:
            dx, dy = (int(n) for n in
                      str(self.root.tk.call("tk::PreciseScrollDeltas", event.delta)).split())
        except (tk.TclError, ValueError):
            return

        self.travel = getattr(self, "travel", 0) + dy

        while abs(self.travel) >= 20:
            self.notch(self.travel > 0)
            self.travel -= 20 if self.travel > 0 else -20

    def key(self, event, down):
        sym = event.keysym_num

        if not sym:
            return "break"

        if down:
            self.held.add(sym)
        else:
            self.held.discard(sym)

        self.send(V.Viewer.key, sym, down)
        return "break"

    def let_go(self, event=None):
        for sym in list(self.held):
            self.send(V.Viewer.key, sym, False)

        self.held.clear()

    def run(self):
        self.root.mainloop()


def main(argv):
    password, size, host = None, None, None
    args = list(argv)

    while args:
        a = args.pop(0)

        if a == "--password" and args:
            password = args.pop(0)
        elif a == "--half":
            size = "half"
        elif a == "--full":
            size = "full"
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            host = a

    port = 5900

    if host and ":" in host:
        host, _, p = host.partition(":")
        port = int(p)

    if host is None:
        print("kosmos_view: looking for a Kosmos machine on this network...")
        host = find_machine()

        if host is None:
            print("kosmos_view: no Kosmos telnetd answered here; give the address")
            return 1

        print("kosmos_view: %s" % host)

    Window(host, port, password, size).run()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
