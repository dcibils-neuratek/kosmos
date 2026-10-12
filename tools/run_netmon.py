#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Network Monitor: what goes in and out, read off the window while it moves.

`docs/netmonitor.md` step N3. A server on this Mac answers a megabyte at a
time, a program in the guest named `pull` asks for it again and again, and
Network Monitor - open beside it - is held to what it says:

  * **a rate in**, a megabyte a request, well above what an idle machine
    hears, and **the busiest connection is `pull`'s** - the program the
    stack named when the connection was opened (N1), by its name;
  * **its graph mirrored**: the kit's `ui.history` (N2) has drawn the theme's
    `io_in` blue above the centre line, and nothing of it below;
  * **its menu**: "Bits a second" chosen, it says so.

Usage: run_netmon.py <image>
"""

import http.server
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402

# The theme's `io_in`, in the dark palette and the light: whichever look the
# desktop is in, it is one of the two.
IO_IN = ((0x4f, 0xb3, 0xff), (0x1f, 0x7f, 0xd0))


def main():
    image = sys.argv[1]
    body = b"n" * (1 << 20)

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    R.extra_args(image, ["-netdev", "user,id=net0",
                         "-device", R.device(image, "net") + ",netdev=net0"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def said(text, since, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            at = guest.seen.find(text, since)
            if at >= 0 and "\n" in guest.seen[at + len(text):]:
                return guest.seen[at + len(text):].split("\n", 1)[0].strip()
            time.sleep(0.1)
        return None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        pull = ('sys.name("pull") local http = use("/Kosmos/Libraries/http.lua") '
                f'for _ = 1, 3000 do http.get("http://10.0.2.2:{port}/mb") end')
        guest.type(f'fs.write("/Temporary/pull.lua", [==[{pull}]==])')
        time.sleep(1)

        # In the background before the desktop, which takes the prompt.
        guest.type("/Temporary/pull.lua &")
        time.sleep(1)

        start = len(guest.seen)
        guest.type("wm netmon")
        opened = said("netmon: window at ", start, 90)
        m = re.match(r"(\d+),(\d+); more at (\d+),(\d+); graph at (\d+),(\d+), (\d+)x(\d+)",
                     opened or "")
        if m is None:
            print("FAIL: Network Monitor never opened its window: %r\n%s"
                  % (opened, guest.seen[-1500:]))
            return 1

        ox, oy, mx, my, gx, gy, gw, gh = (int(v) for v in m.groups())
        card = said("netmon: card ", start, 5)
        check(card is not None and "MTU" in card,
              "the card did not say its address and MTU: %r" % card)

        # The window watched while the pulling runs.
        mark = len(guest.seen)

        best = None
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            for line in re.findall(r"netmon: in (\d+) B/s, out (\d+) B/s; (\d+) open; "
                                   r"busiest (\S+) (\d+) (\d+)", guest.seen[mark:]):
                rin, busiest = int(line[0]), line[3]
                if busiest == "pull" and (best is None or rin > best[0]):
                    best = (rin, line)
            if best and best[0] > 200000:
                break
            time.sleep(0.5)

        check(best is not None and best[0] > 200000,
              "Network Monitor never saw a rate in from pull's megabytes: %r" % (best,))
        check(best is not None and int(best[1][4]) > 100000,
              "pull's connection did not carry the rate: %r" % (best,))

        # The graph: blue columns above the centre line, none below it.
        shot = guest.screendump()
        width, height, at = R.pixel_reader(shot)

        # Kept, for a person to look at what was checked.
        os.makedirs("build/gate", exist_ok=True)
        with open("build/gate/x86-netmon.ppm", "wb") as f:
            f.write(shot)
        x0, y0 = ox + gx, oy + gy
        centre = y0 + 2 + (gh - 4) // 2
        def blue(*box):
            return next((p for p in (R.find_colour(at, c, *box) for c in IO_IN) if p), None)

        above = blue(x0 + gw // 2, y0 + 4, x0 + gw - 4, centre - 1)
        below = blue(x0 + 4, centre + 2, x0 + gw - 4, y0 + gh - 4)
        check(above is not None, "no io_in blue above the graph's centre line")
        check(below is None, "io_in blue below the centre line, where out belongs: %r" % (below,))

        # The menu: bits a second.
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(ox + mx, oy + my, width, height))
        time.sleep(0.4)
        guest.mouse_button(True)
        time.sleep(0.2)
        guest.mouse_button(False)
        menu = said("netmon: more menu at ", mark, 10)
        mm = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", menu or "")
        check(mm is not None, "the dots did not open the menu: %r" % menu)

        if mm:
            x, y, w, row = (int(v) for v in mm.groups())
            # Three paces, a separator a third of a row, bytes, then bits.
            guest.mouse_to(*R._to_tablet(x + 30, y + 2 + 4 * row + row // 3 + row // 2,
                                         width, height))
            time.sleep(0.4)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            check(said("netmon: bits", mark, 10) is not None,
                  "choosing Bits a second was not heard")
    finally:
        transcript = guest.seen
        guest.close()
        httpd.shutdown()

    if failed:
        print("FAIL: %d of %d checks on Network Monitor:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + "\n".join(l for l in transcript.splitlines() if "wm: button" not in l and "netmon: in 0 B/s" not in l)[-4000:])
        return 1

    print("PASS: %d checks on Network Monitor (a megabyte a request from this Mac, "
          "its rate in on the window and pull's connection the busiest, by its "
          "program's name; the mirrored graph blue above its line and not below; "
          "Bits a second from the dots)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
