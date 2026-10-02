#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The window manager sleeps to the tick a window asked for (`testing.md` 18.345).

On 2 October the M700 showed a processor at a hundred per cent with nothing
happening, and the profile said whose: the window manager and the console,
68,000 passes a second between them, every `wait` coming straight back. A
Terminal was running a program - `neofetch`, stuck on the network - and a
Terminal running something asks to be woken every tick. The window manager
worked out how long to sleep by rounding the time to the next deadline
*down*, so a deadline less than a tick away was a sleep of nought, and it
went round again until the deadline passed: a spin for as long as any window
polled every tick.

So one window that polls every tick, for three seconds, and two numbers:

- **its polls are still answered at the tick**: at least `LEAST` of the 750
  three seconds holds at 250 a second - rounding up must not turn into a
  window that is answered late;
- **the window manager and the console use less than `MOST` of the
  processor between them** over those three seconds. Spinning, they use all
  of one.

Usage: run_wmwait.py IMAGE
"""

import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"

LEAST = 300             # of 750 ticks in three seconds: answered within two
MOST = 0.5              # of one processor, the window manager and the console

# A window that polls every tick, as a Terminal does while a program runs in
# it, and counts what that costs the two processes that answer it.
TICKER = (
    'local ui = use("/Kosmos/Libraries/ui.lua") '
    'local hz = fs.read("/Devices/cpu").counter_hz '
    'local tick_hz = (sys.info() or {}).tick_hz or 250 '
    'local win = ui.window{ title = "Ticker", w = 160, h = 80 } '
    'local function busy() local n = 0 for _, p in ipairs(sys.processes()) do '
    'if p.name == "wm" or p.name == "console" then n = n + (p.ticks or 0) end end '
    'return n end '
    'local passes, t0, b0 = 0, nil, nil '
    'win.poll_wait_ticks = 1 '
    'win.on_frame = function() passes = passes + 1 '
    'if not t0 then t0, b0, passes = sys.ticks(), busy(), 0 end '
    'if sys.ticks() - t0 >= 3 * hz then '
    'print(("WAIT" .. "S %d answered, %d busy, %d ticks"):format(passes, '
    'busy() - b0, (sys.ticks() - t0) * tick_hz // hz)) win.running = false end '
    'return false end '
    'win:run()\n')


def main():
    import run_screenshot as R

    guest = R.Guest(IMAGE, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        guest.type("fs.write('/Temporary/ticker.lua', %r) print('ticker' .. "
                   "'-written')" % TICKER)
        guest.wait_for("ticker-written", "wrote the ticker")
        guest.type("wm /Temporary/ticker.lua")

        deadline = time.monotonic() + 90

        while time.monotonic() < deadline and "WAITS " not in guest.seen:
            time.sleep(0.3)
            guest._read_available()

        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()

    found = re.search(r"WAITS (\d+) answered, (\d+) busy, (\d+) ticks",
                      guest.seen)

    check(found is not None,
          "the ticker never said what it measured:\n" + guest.seen[-800:])

    if found is not None:
        answered, busy, ticks = (int(found.group(i)) for i in (1, 2, 3))
        share = busy / max(ticks, 1)

        print("  %d polls answered in %d ticks; the window manager and the "
              "console %d ticks, %.0f%% of a processor"
              % (answered, ticks, busy, share * 100))

        check(answered >= LEAST,
              "a window that polls every tick was answered %d times in %d "
              "ticks - fewer than %d, so rounding the sleep up has made polls "
              "late rather than ending the spin" % (answered, ticks, LEAST))

        check(share < MOST,
              "the window manager and the console used %.0f%% of a processor "
              "while one window polled every tick - %d ticks of %d. That is "
              "the spin: `sleep_for` rounding a deadline less than a tick away "
              "down to a sleep of nought" % (share * 100, busy, ticks))

    if failed:
        print("FAIL: %d of %d checks on the window manager's sleep:"
              % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on the window manager's sleep (a window polling "
          "every tick is answered at the tick, and nothing spins)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
