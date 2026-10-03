#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The M700's suite: real hardware and real performance, run from the Mac.

`roadmap.md`, a suite on the M700 - Diego, 3 October 2026: "We could do a
subset of tests on the m700 mostly for performance tests and real
Hardware", and "i want yo see the make m700 gate run from you via telnet so
i can see all apps working and the tests on the scren of the m700".

  make m700                          build, serve, restart the M700, run this
  python3 tools/run_m700.py          this, on the M700 as it is running now
  python3 tools/run_m700.py --restart [ADDRESS]

**The gate stays on QEMU**, where faults can be injected and the clock is
exact. This is what QEMU cannot say: whether the machine's own network card,
USB devices, sound and screen work, and how fast it really is.

**Everything is done where Diego can watch it.** A Terminal opens on the
M700 and each test's output is shown in it as the test finishes - the test
itself runs over Telnet, which is what brings its exact output back here,
since Kosmos has no redirection to keep a copy - and then every application
opens, a dozen at a time, tiled, pictured by VNC and closed.

**Pass or fail is the hardware's**: the I219 reset and sending, every frame
it counted taken, an address by DHCP, the router and the internet and a
name; the keyboard, the mouse and the stick on USB, and nothing the USB
driver had to give up on; `/Home` on the stick; the screen's mode; sound
with no underrun; a stick that takes a written megabyte; each application
launched with a window and nothing dying. **The numbers are kept**, each
run's beside the one before, in `build/m700/`, and a change is said - they
are not pass or fail, because a number has no right value until there is a
history to read it against.
"""

import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import kosmos_telnet as T                                       # noqa: E402
import kosmos_vnc as V                                          # noqa: E402

ADDRESS = "192.168.1.40"
OUT = os.path.join(ROOT, "build", "m700")

# Every application in the image, but the ones that are the desktop itself,
# that are open already, that hang on purpose (`stuck`), or that need
# something left for them (`adopt`); and the three installed in /Home/Apps.
SKIP = {"deskbar", "desktop", "stuck", "adopt", "logview"}
ARGS = {"browser": "https://en.wikipedia.org/wiki/BeOS"}
INSTALLED = ["/Home/Apps/Doom/doom.lua", "/Home/Apps/Quake/quake.lua",
             "/Home/Apps/SNES/snes.lua"]
BATCH = 12


class Machine:
    """The M700, reached three ways: Telnet for commands and the log, VNC
    for the screen, and the boot server for what it starts with."""

    def __init__(self, host):
        self.host = host
        self.session = None
        self.viewer = None
        self.shown = 0
        self.terminal = None            # where to click to give it the keys

    def connect(self):
        self.session = T.Session(self.host, timeout=240.0)

    def run(self, command):
        return self.session.run(command).decode(errors="replace")

    def put(self, text, path):
        return self.session.put(text.encode(), path)

    def screen(self):
        if self.viewer is None:
            self.run("open vncd")
            deadline = time.monotonic() + 30

            while True:
                try:
                    self.viewer = V.Viewer("%s:5900" % self.host, timeout=30)
                    break
                except (OSError, V.RFBError):
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(1)

        return self.viewer

    def steps(self, text):
        for s in text.split(";"):
            if s.strip():
                V.step(self.screen(), s)

    def windows(self):
        """The newest place the log gives each window, by its title."""
        where = {}

        for m in re.finditer(r"wm: window (.+?) at (\d+),(\d+) (\d+)x(\d+)",
                             self.run("log wm: window")):
            where[m.group(1)] = tuple(int(m.group(i)) for i in range(2, 6))

        return where

    def show(self, title, text):
        """A test's output, on the M700's screen: put in /Temporary and
        `cat` typed into the Terminal, which is given the keys first."""
        if self.terminal is None:
            return

        self.shown += 1
        path = "/Temporary/m700/%02d-%s.txt" % (self.shown, re.sub(r"\W+", "-", title))
        self.put(text, path)
        x, y = self.terminal
        self.steps("click %d %d; type cat %s\\n" % (x, y, path))


def number(pattern, text, cast=float):
    m = re.search(pattern, text, re.M)
    return cast(m.group(1)) if m else None


class Suite:
    def __init__(self, machine, folder):
        self.m = machine
        self.folder = folder
        self.checks = 0
        self.fails = []
        self.numbers = {}
        self.outputs = {}

    def check(self, ok, complaint):
        self.checks += 1

        if not ok:
            self.fails.append(complaint)
            print("  FAIL: " + complaint)

        return ok

    def note(self, key, value):
        if value is not None:
            self.numbers[key] = value

    def test(self, title, command):
        """A command run over Telnet, kept, shown on the M700, returned."""
        started = time.monotonic()
        out = self.m.run(command)
        took = time.monotonic() - started
        self.outputs[title] = out

        with open(os.path.join(self.folder, re.sub(r"\W+", "-", title) + ".txt"), "w") as f:
            f.write(out)

        print("  %-28s %5.1f s" % (title, took))
        self.m.show(title, "--- %s (%s), %.1f s\n%s" % (title, command, took, out))
        return out, took

    # ---- real hardware ----

    def network(self):
        e1000 = self.m.run("log e1000:")
        self.outputs["log e1000"] = e1000
        self.check("its MAC reset as e1000e does it" in e1000,
                   "the I219 was not reset before it was used")
        self.check(re.search(r"a frame to itself went out in \d+ us - it sends", e1000)
                   is not None, "the I219's frame to itself did not go out")
        counted = re.search(r"at 30 s the card counted (\d+) good.*?\n.*?missed (\d+), "
                            r"no buffer (\d+), CRC (\d+); taken (\d+)", e1000, re.S)

        if self.check(counted is not None, "the I219's counts at 30 s were not said"):
            good, missed, _, _, taken = (int(counted.group(i)) for i in range(1, 6))
            self.check(missed == 0, "the I219 missed %d frames in its first 30 s" % missed)
            self.check(taken >= good - 2, "the I219 counted %d good frames and %d "
                       "reached the stack" % (good, taken))

        dhcp = self.m.run("log an address from DHCP")
        address = re.search(r"an address from DHCP: (\d+\.\d+\.\d+\.\d+)", dhcp)
        router = re.search(r"router (\d+\.\d+\.\d+\.\d+)", dhcp)
        self.check(address is not None, "no address from DHCP in the log")
        gateway = router.group(1) if router else "192.168.1.1"

        out, _ = self.test("ping the router", "ping " + gateway)
        self.check("0% lost" in out, "the router did not answer every ping: "
                   + out.strip().splitlines()[-2:][0] if out.strip() else "nothing")
        self.note("ping router ms", number(r"min/avg/max = [\d.]+/([\d.]+)/", out))
        out, _ = self.test("ping the internet", "ping 8.8.8.8")
        received = number(r"(\d+) received", out, int)
        self.check(received is not None and received >= 3,
                   "8.8.8.8 answered %s of 4 pings" % received)
        self.note("ping internet ms", number(r"min/avg/max = [\d.]+/([\d.]+)/", out))
        out, _ = self.test("a name looked up", "host en.wikipedia.org")
        self.check(re.search(r" is \d+\.\d+\.\d+\.\d+", out) is not None,
                   "en.wikipedia.org was not looked up: " + out.strip()[:120])

    def usb_and_disk(self):
        xhci = self.m.run("log xhci:")
        self.outputs["log xhci"] = xhci
        self.check(", a keyboard," in xhci or ": a keyboard" in xhci,
                   "no USB keyboard was named")
        self.check(": a mouse," in xhci, "no USB mouse was named")
        self.check(": a stick:" in xhci, "no USB stick was named")

        out, took = self.test("the machine", "neofetch")
        self.note("neofetch s", round(took, 1))
        self.check(re.search(r"^Disk\s+kfs", out, re.M) is not None,
                   "/Home is not the stick: " + (re.search(r"^Disk.*$", out, re.M) or
                                                 re.search("$", "")).group(0))
        mode = re.search(r"^Display\s+(\d+) x (\d+)", out, re.M)

        if self.check(mode is not None, "the screen's mode was not said"):
            self.check(int(mode.group(1)) >= 1280,
                       "the screen came up at %s x %s" % mode.groups())
            self.numbers["screen"] = "%sx%s" % mode.groups()

        out, _ = self.test("the disk, through /Home", "diskbench /Home 2 1")
        write = re.search(r"^sequential 1 MB x1\s+([\d.]+) MB/s\s+(.+?)\s*$", out, re.M)

        if self.check(write is not None, "diskbench /Home said no sequential row"):
            self.note("/Home read MB/s", float(write.group(1)))
            wrote = re.match(r"([\d.]+) MB/s", write.group(2))
            self.check(wrote is not None, "the stick refused a written megabyte: "
                       + write.group(2))
            self.note("/Home write MB/s", float(wrote.group(1)) if wrote else None)

        self.note("/Home random IOPS", number(r"^random 4 KB x1\s+[\d.]+ MB/s\s+(\d+) IOPS",
                                              out, int))
        out, _ = self.test("the stick's own blocks", "diskbench usb 0 2 1")
        self.note("stick read MB/s", number(r"^sequential 1 MB x1\s+([\d.]+) MB/s", out))

        given_up = [l for l in self.m.run("log xhci:").splitlines()
                    if "failed" in l or "no answer" in l]
        self.check(not given_up, "the USB driver gave up on a transfer:\n    "
                   + "\n    ".join(given_up[-4:]))

    def sound(self):
        hda = self.m.run("log sound:")
        self.check("Intel HDA" in hda, "no sound device was said")
        out, _ = self.test("sound, 400 periods", "audiolag")
        underruns = number(r"UNDERRUNS (\d+)", out, int)
        self.check(underruns == 0, "sound underran %s times" % underruns)
        self.note("sound worst latency us", number(r"worst latency \d+ frames, (\d+) us",
                                                   out, int))

    # ---- real performance ----

    def performance(self):
        out, _ = self.test("pixels", "gfxbench")

        for name, key in (("fill, whole surface", "fill Mpx/s"),
                          ("blit, whole surface", "blit Mpx/s"),
                          ("gfx.disc in C, x1000", "disc in C Mpx/s")):
            self.note(key, number(re.escape(name) + r"\s+\d+ px in\s+\d+ us\s+(\d+) Mpx/s",
                                  out, int))

        out, _ = self.test("the processor taken away", "jitter 2")
        self.note("jitter worst us", number(r"worst gap (\d+) us", out, int))
        self.check(number(r"over 10 ms: (\d+)", out, int) == 0,
                   "something took the processor for more than 10 ms")
        out, _ = self.test("a yield and a round trip", "latency")
        self.note("IPC round trip us", round(1000 * (number(r"an IPC round trip\s+[\d.]+ "
                                                            r"ticks\s+([\d.]+) ms", out) or 0), 1)
                  or None)
        self.check("PASS:" in out, "latency did not pass: " + out.strip()[-200:])

    def frames(self):
        out, _ = self.test("the window manager, busy", "frames 5")
        self.note("frame mean us", number(r"busy\s+[\d.]+ ms total\s+([\d.]+) us mean", out))
        self.note("frame worst ms", number(r"us mean\s+([\d.]+) ms WORST", out))
        self.note("compose ns a pixel", number(r"([\d.]+) ns a pixel", out))

    # ---- every application ----

    def applications(self):
        names = sorted(n[:-4] for n in os.listdir(os.path.join(ROOT, "user", "bin", "apps"))
                       if n.endswith(".lua") and n[:-4] not in SKIP)
        everything = names + INSTALLED
        batches = [everything[i:i + BATCH] for i in range(0, len(everything), BATCH)]
        died_before = self.m.run("log died").count(" died")

        for k, batch in enumerate(batches, 1):
            pids = []

            for app in batch:
                said = self.m.run("open %s %s" % (app, ARGS.get(app, ""))).strip()
                started = "started" in said
                self.check(started, "%s did not open: %s" % (app, said[-160:]))

            time.sleep(4)
            launched = self.m.run("log launched")

            for app in batch:
                base = os.path.basename(app)
                m = None

                for m in re.finditer(r"launched (\S*%s) -> (true|false) ?(\d*)"
                                     % re.escape(base if base.endswith(".lua") else
                                                 base + ".lua"), launched):
                    pass

                if self.check(m is not None and m.group(2) == "true",
                              "%s was not launched" % app) and m.group(3):
                    pids.append(m.group(3))

            self.m.run("tile")
            time.sleep(2)

            if k == 1:
                self.frames()

            picture = os.path.join(self.folder, "apps-%d.png" % k)
            started = time.monotonic()
            self.m.steps("shot " + picture)
            self.note("whole frame by VNC s", round(time.monotonic() - started, 1))
            print("  applications %d of %d: %s" % (k, len(batches), ", ".join(
                os.path.basename(a).replace(".lua", "") for a in batch)))

            for pid in pids:
                self.m.run("kill " + pid)

            time.sleep(1)

        died = [l for l in self.m.run("log died").splitlines() if " died" in l]
        self.check(len(died) <= died_before, "an application died:\n    "
                   + "\n    ".join(died[-4:]))


def history(numbers, version):
    """This run's numbers beside the last run's, and kept."""
    path = os.path.join(OUT, "history.jsonl")
    before = None

    if os.path.exists(path):
        with open(path) as f:
            lines = [l for l in f if l.strip()]

        if lines:
            before = json.loads(lines[-1])

    with open(path, "a") as f:
        f.write(json.dumps({"version": version, "at": time.strftime("%Y-%m-%d %H:%M"),
                            "numbers": numbers}) + "\n")

    rows = []

    for key, value in numbers.items():
        old = before["numbers"].get(key) if before else None
        change = ""

        if isinstance(value, (int, float)) and isinstance(old, (int, float)) and old:
            pct = 100.0 * (value - old) / old
            change = "  (%s: %s, %+.0f%%)" % (before["version"], old, pct)

        rows.append("  %-24s %s%s" % (key, value, change))

    return rows


def main(argv):
    restart = "--restart" in argv
    hosts = [a for a in argv if not a.startswith("--")]
    host = hosts[0] if hosts else ADDRESS
    version = open(os.path.join(ROOT, "VERSION")).read().strip()
    folder = os.path.join(OUT, "%s-%s" % (version, time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(folder, exist_ok=True)
    began = time.monotonic()
    restart_s = None

    if restart:
        print("m700: restarting %s into what is served" % host)
        started = time.monotonic()

        if T.restart(host, 300.0) != 0:
            print("FAIL: the M700 did not come back")
            return 1

        restart_s = round(time.monotonic() - started, 1)

    m = Machine(host)
    m.connect()
    s = Suite(m, folder)
    banner = m.run("neofetch")
    running = re.search(r"Kosmos (\d+\.\d+\.\d+)", banner)

    if restart:
        s.check(running is not None and running.group(1) == version,
                "the M700 runs %s, not %s" % (running and running.group(1), version))

    s.note("restart s", restart_s)

    # The counts the I219 says at 30 s, and the desktop settled.
    uptime = lambda: number(r"^Uptime\s+(\d+) seconds?", m.run("neofetch"), int)
    up = uptime()

    while up is not None and up < 35:
        time.sleep(35 - up)
        up = uptime()

    # A Terminal for Diego to watch, given the keys.
    m.run("open terminal")
    time.sleep(2)
    where = m.windows().get("Terminal")

    if where:
        x, y, w, h = where
        m.terminal = (x + w // 2, y + h // 2)

    m.run("mkdir /Temporary/m700")
    m.show("the M700 suite", "the M700 suite: Kosmos %s on %s, from the Mac\n" % (version, host))

    print("m700: real hardware")
    s.network()
    s.usb_and_disk()
    s.sound()
    print("m700: real performance")
    s.performance()
    print("m700: every application")
    s.applications()

    rows = history(s.numbers, version)
    took = time.monotonic() - began

    with open(os.path.join(folder, "results.json"), "w") as f:
        json.dump({"version": version, "checks": s.checks, "fails": s.fails,
                   "numbers": s.numbers, "seconds": round(took)}, f, indent=2)

    summary = ("%s: %d of %d checks on the M700 in %d:%02d\n" %
               ("FAIL" if s.fails else "PASS", s.checks - len(s.fails), s.checks,
                took // 60, took % 60)
               + "".join("  FAIL: %s\n" % f for f in s.fails)
               + "\n".join(rows) + "\n")
    m.run("open terminal")
    time.sleep(2)
    where = m.windows().get("Terminal")

    if where:
        x, y, w, h = where
        m.terminal = (x + w // 2, y + h // 2)

    m.show("the summary", summary)
    print()
    print(summary)
    print("m700: pictures and outputs in %s" % os.path.relpath(folder, ROOT))
    return 1 if s.fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
