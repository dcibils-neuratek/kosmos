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
- **Far** (`x86-tcpfar`, `--far`): the server is `tools/farpeer.py`, on
  the far side of QEMU's `-netdev dgram` rather than behind slirp - whose
  TCP does not scale its window, so nothing behind it could see whether
  this stack's does. It holds every frame it sends 50 ms, a round trip
  like a far server's, and offers RFC 7323's window scale. The guest's SYN
  must offer one too, the guest must say a window past sixteen bits, and
  the 8 MB must come at more than a 64 KB window can bring that far.

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

import farpeer as F                                         # noqa: E402
import run_screenshot as R                                  # noqa: E402
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


def far_mode(image):
    """The far mode: the guest against `farpeer.py`, at the prompt."""
    port, peer_port, qemu_port = (random.randint(20000, 30000), random.randint(30001, 40000),
                                  random.randint(40001, 50000))
    data = random.Random(9112026).randbytes(MB << 20)
    want = hashlib.sha256(data).hexdigest()
    hold = HOLD_US / 1e6
    peer = F.FarPeer(data, port, hold, peer_port, qemu_port)
    said = peer.said                        # read as it goes, not when it ends
    fails = []

    import threading as T
    server = T.Thread(target=peer.run, args=(SECONDS * 3,), daemon=True)
    server.start()                          # listening before the guest can ask

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", ("dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=%d,"
                    "remote.type=inet,remote.host=127.0.0.1,remote.port=%d" % (qemu_port, peer_port)),
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/net_address,string=10.9.0.2/24",
    ])

    try:
        guest = R.Guest(image, 120)
    finally:
        setattr(R, board, saved)

    try:
        guest.wait_for("kosmos> ", "a prompt")
        mark = len(guest.seen)
        guest.type("netspeed get 10.9.0.1 %d" % port)
        deadline = time.monotonic() + SECONDS * 2
        while time.monotonic() < deadline and "netspeed: SHA-256" not in guest.seen[mark:]:
            guest._read_available()
            time.sleep(0.2)
        got = guest.seen[mark:]
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
        got = ""
    finally:
        seen = guest.seen
        guest.close()
        server.join(2)

    took = re.search(r"received ([\d.]+) MB in ([\d.]+) s, ([\d.]+) MB/s", got)
    summed = re.search(r"SHA-256 ([0-9a-f]{64})", got)
    ring = re.findall(r"a ring of (\d+) KB, windows shifted (\d+) here", seen)
    ceiling = 65535 / hold / 1048576            # a 64 KB window, this far

    if not took:
        fails.append("netspeed did not say what it received: %r" % got[-400:])
    elif abs(float(took.group(1)) - MB) > 0.05:
        fails.append("%s MB arrived of %d" % (took.group(1), MB))
    if not summed or summed.group(1) != want:
        fails.append("the bytes are not the bytes sent: SHA-256 %s" % (summed.group(1) if summed else None))
    if said.get("offered_scale") is None or said.get("most_window", 0) <= 65535:
        fails.append("the guest did not scale its window: offered %r, the most it said %d bytes"
                     % (said.get("offered_scale"), said.get("most_window", 0)))
    if took and float(took.group(3)) <= ceiling * 1.5:
        fails.append("%s MB/s, no faster than a 64 KB window can bring from %d ms away (%.2f MB/s)"
                     % (took.group(3), HOLD_US // 1000, ceiling))

    if fails:
        print("the peer: %d frames, by kind %r, %d sent" % (said.get("frames", 0), said.get("kinds"), said.get("sent", 0)))
        print("the guest: " + " | ".join(l.strip() for l in seen.splitlines() if l.startswith("net:") or "virtio-net" in l or "network" in l.lower())[:1500])
        print("FAIL: %d of 4 checks on TCP receiving from far away:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 4 checks on TCP receiving from far away (%d MB from a server %d ms away "
          "that scales its window: the guest offered a shift of %d and said windows up to "
          "%d KB, a ring of %s KB; %s MB/s against a 64 KB window's %.2f; whole by its SHA-256; "
          "%d sent again)" % (MB, HOLD_US // 1000, said["offered_scale"], said["most_window"] // 1024,
                              ring[0][0] if ring else "?", took.group(3), ceiling, said["resent"]))
    return 0


def main():
    far = "--far" in sys.argv[1:]
    named = [a for a in sys.argv[1:] if not a.startswith("--")]
    image = named[0] if named else "build/x86_64/kosmos.elf"

    if far:
        return far_mode(image)
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
    ring = re.findall(r"a ring of (\d+) KB, windows shifted (\d+) here and (\d+) there, (\d+) KB offered last", seen)
    print("the stack: %s" % (ring,))

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
