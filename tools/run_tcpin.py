#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""TCP receiving from far away and through loss (`user/servers/net.c`,
9 November 2026).

The M700 took a file from Hugging Face's CDN at about 160 KB/s where this
Mac takes it at 4.9 MB/s (`testing.md` 18.737): the stack offered a window
of 16 KB, because it kept nothing that arrived out of order, and 16 KB a
round trip is what a server 100 ms away can send. Now what comes ahead of a
gap is kept in the ring and the window is the ring.

A server on this Mac sends 8 MB of bytes it chose, and `netspeed get`
reads them and says their SHA-256, which must be the bytes sent. Two
machines, run as two suites:

- **Losing** (`x86-tcpin`): `opt/kosmos/net_drop_in=N` lets one data
  segment in N that arrives go unread, as if lost on the way, so what
  follows each loss comes ahead of a gap; the stack must say it kept such
  pieces, and the bytes must be whole.
- **Far** (`x86-tcpfar`, `--far`): QEMU's `filter-buffer` holds every
  frame and lets them go every 50 ms, a round trip like a far server's,
  and the 8 MB must come faster than a 16 KB window could bring them at
  that distance.

Not both at once: with a loss in every window, how fast a far sender goes
is its congestion control's - Reno's MSS / RTT x 1.22 / sqrt(p), about
130 KB/s at one in 53 and 100 ms - whatever this end offers.

Usage: run_tcpin.py IMAGE [--far]
"""

import hashlib
import os
import random
import re
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_servers as S                                     # noqa: E402

MB = 8
DROP = 53               # one in so many; a prime
HOLD_US = 50000         # filter-buffer's interval: frames go every 50 ms
SECONDS = 90            # the whole of it, at most


def serve(port, data, said):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(1)
    s.settimeout(SECONDS * 3)

    try:
        c, _ = s.accept()
        started = time.monotonic()
        c.sendall(data)
        c.close()
        said.append(time.monotonic() - started)
    except OSError:
        pass
    finally:
        s.close()


def main():
    far = "--far" in sys.argv[1:]
    named = [a for a in sys.argv[1:] if not a.startswith("--")]
    image = named[0] if named else "build/x86_64/kosmos.elf"
    telnet, web, port = (random.randint(20000, 30000), random.randint(30001, 40000),
                         random.randint(40001, 50000))
    data = random.Random(9112026).randbytes(MB << 20)
    want = hashlib.sha256(data).hexdigest()
    said = []
    fails = []
    got = ""

    threading.Thread(target=serve, args=(port, data, said), daemon=True).start()

    guest = S.boot(image, telnet, web, extra=(
        ["-object", "filter-buffer,id=far,netdev=net0,queue=all,interval=%d" % HOLD_US] if far
        else ["-fw_cfg", "name=opt/kosmos/net_drop_in,string=%d" % DROP]))

    try:
        if not far:
            guest.wait_for("lost on its way in on purpose (opt/kosmos/net_drop_in)",
                           "the stack told to lose")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet, 120)
        session.timeout = SECONDS * 2
        got = session.run("netspeed get 10.0.2.2 %d" % port).decode(errors="replace")
        guest._read_available()
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    took = re.search(r"received ([\d.]+) MB in ([\d.]+) s, ([\d.]+) MB/s", got)
    summed = re.search(r"SHA-256 ([0-9a-f]{64})", got)
    kept = [int(n) for n in re.findall(r"net: 4 MB from [^,]*, (\d+) pieces kept", seen)]

    # A 16 KB window, a round trip at least the filter's hold: no faster
    # than 16 KB a hold, whatever else happens.
    ceiling = 16384 / (HOLD_US / 1e6) / 1048576

    if not took:
        fails.append("netspeed did not say what it received: %r" % got[-400:])
    elif abs(float(took.group(1)) - MB) > 0.05:
        fails.append("%s MB arrived of %d" % (took.group(1), MB))
    if not summed or summed.group(1) != want:
        fails.append("the bytes are not the bytes sent: SHA-256 %s, not %s"
                     % (summed.group(1) if summed else None, want))
    if not far and (not kept or max(kept) == 0):
        fails.append("the stack never kept a piece that came ahead of a gap: %r" % kept)
    if far and took and float(took.group(3)) <= ceiling * 1.5:
        fails.append("%s MB/s, no faster than a 16 KB window can bring at this distance "
                     "(%.2f MB/s)" % (took.group(3), ceiling))
    if took and float(took.group(2)) > SECONDS:
        fails.append("it took %s s" % took.group(2))
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    what = "from far away" if far else "through loss"

    if fails:
        print("FAIL: %d of 4 checks on TCP receiving %s:" % (len(fails), what))
        for f in fails:
            print("  " + f)
        return 1

    if far:
        print("PASS: 4 checks on TCP receiving from far away (%d MB through frames held "
              "%d ms: %s MB/s against a 16 KB window's %.2f, whole by its SHA-256)"
              % (MB, HOLD_US // 1000, took.group(3), ceiling))
    else:
        print("PASS: 4 checks on TCP receiving through loss (%d MB with one segment in %d "
              "lost on the way in, whole by its SHA-256, %d pieces kept that came ahead of "
              "a gap, %s MB/s)" % (MB, DROP, max(kept), took.group(3)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
