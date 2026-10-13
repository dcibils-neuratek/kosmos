#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""TCP sending through loss (`user/servers/net.c`, 10 October 2026).

The stack keeps a window of segments in flight, and a segment lost on the
way has to be sent again - on three repeated acknowledgements, or on the
timer - from the oldest byte not acknowledged. QEMU's network loses nothing,
so the stack is booted with `opt/kosmos/net_drop=N`, which leaves every Nth
data segment unsent as if the wire had lost it, and a whole screen is taken
by VNC three times: each must arrive whole and match what QEMU scans out,
in a bounded time, with segments said lost along the way.

**And through a narrow router** (`--mtu`, 13 October): booted with
`opt/kosmos/net_mtu=1400`, a router on the path that passes nothing over
1400 bytes and answers "fragmentation needed" - as a PPPoE line's does at
1492, which dropped every full segment Kosmos sent past it. The frames must
arrive whole all the same, the connection's segments made 1360.

Usage: run_tcploss.py IMAGE [--mtu]
"""

import os
import random
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import kosmos_vnc as V                                      # noqa: E402
import run_screenshot as R                                  # noqa: E402
import run_servers as S                                     # noqa: E402

DROP = 37               # one in so many; a prime, so it does not beat with a frame's rows
FRAMES = 3
SECONDS = 30            # a whole frame, at most, with the losses


MTU = 1400


def main():
    narrow = "--mtu" in sys.argv[1:]
    named = [a for a in sys.argv[1:] if not a.startswith("--")]
    image = named[0] if named else "build/x86_64/kosmos.elf"
    telnet, web, vnc = (random.randint(20000, 30000), random.randint(30001, 40000),
                        random.randint(40001, 50000))
    guest = S.boot(image, telnet, web, vnc=vnc,
                   extra=["-fw_cfg", ("name=opt/kosmos/net_mtu,string=%d" % MTU) if narrow
                          else ("name=opt/kosmos/net_drop,string=%d" % DROP)])
    fails, times, matched, where = [], [], [], []

    try:
        guest.wait_for("on purpose (opt/kosmos/net_mtu)" if narrow
                       else "lost on purpose (opt/kosmos/net_drop)", "the stack told to lose")
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)
        # The windows that change every second closed, so what the frame
        # is compared with holds still while it travels.
        for busy in ("procs", "sysmon", "logview"):
            session.run("kill " + busy)

        session.run("open vncd")
        guest.wait_for("vncd: on port", "vncd")
        time.sleep(2)

        # One viewer, and the banner its connecting puts up gone before the
        # first frame, so the screen holds still under the frames.
        viewer = V.Viewer("127.0.0.1:%d" % vnc, timeout=SECONDS * 2)
        guest.wait_for("wm: closed Notifications", "the banner gone")
        time.sleep(2)

        for _ in range(FRAMES):
            _, _, before = R.parse_ppm(guest.screendump())
            started = time.monotonic()
            viewer.request(False)
            viewer.update()
            times.append(time.monotonic() - started)
            width, height, px = R.parse_ppm(guest.screendump())
            whole, still = S.still_share(viewer.frame, width, before, px)
            matched.append(whole)

            if whole < 0.999:
                where.append(S.where_differs(viewer.frame, width, height, px))

        viewer.close()

        guest._read_available()
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    lost = [int(n) for n in re.findall(r"net: 4 MB to .*; lost (\d+)", seen)]

    if narrow:
        if len(times) < FRAMES:
            fails.append("only %d of %d whole frames arrived through a %d-byte router"
                         % (len(times), FRAMES, MTU))
        if any(t > SECONDS for t in times):
            fails.append("a whole frame took %s s through a %d-byte router"
                         % (", ".join("%.1f" % t for t in times), MTU))
        if any(m < 0.999 for m in matched):
            fails.append("a frame did not arrive whole through a %d-byte router: %s" % (MTU, "; ".join(where)))
        if "segments made %d" % (MTU - 40) not in seen:
            fails.append("the stack never made a connection's segments %d" % (MTU - 40))

        if fails:
            print("FAIL: %d of 4 checks on TCP through a narrow router:" % len(fails))
            for f in fails:
                print("  " + f)
            return 1

        print("PASS: 4 checks on TCP through a narrow router (a %d-byte path saying fragmentation "
              "needed; segments made %d; %d whole frames by VNC, each the screen QEMU scans out, "
              "in %s s)" % (MTU, MTU - 40, FRAMES, ", ".join("%.1f" % t for t in times)))
        return 0

    if len(times) < FRAMES:
        fails.append("only %d of %d whole frames arrived" % (len(times), FRAMES))
    if any(t > SECONDS for t in times):
        fails.append("a whole frame took %s s with one segment in %d lost"
                     % (", ".join("%.1f" % t for t in times), DROP))
    if any(m < 0.999 for m in matched):
        fails.append("a frame did not arrive whole: %s of the screen, differing within %s"
                     % (", ".join("%.2f%%" % (100 * m) for m in matched), "; ".join(where)))
    if not lost or max(lost) == 0:
        fails.append("the stack never said it lost a segment on purpose")
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of 4 checks on TCP sending through loss:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 4 checks on TCP sending through loss (one data segment in %d lost on purpose, "
          "%d of them by the last count; %d whole frames by VNC, each the screen QEMU scans out, "
          "in %s s)" % (DROP, max(lost), FRAMES, ", ".join("%.1f" % t for t in times)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
