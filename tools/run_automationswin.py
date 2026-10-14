#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Automations window, as drawn (`docs/automations.md` A4).

Driven by name - through `automate`, the window's own controls published as
any application's are - on a machine with a disk for /Home:

  1. It opens on the automations written, each in the list.
  2. **Run now** on the chosen one: `autod` started, the run lit and done,
     and the row saying so; its **History** sheet with the run.
  3. Its **switch** off: kept in the file, and `autod` says it is off.
  4. **+ New** makes one; **Change** gives it a trigger - every 15 minutes -
     which the list then says; **Add a step**'s sheet keeps an Open of
     Calculator, read back as the step's sentence.
  5. **Its file opened**, as Tracker opens one: the automation chosen and
     run.

Usage: run_automationswin.py IMAGE
"""

import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

_DISK = os.path.join(scratch.directory("automationswin"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "16"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_servers as S                                     # noqa: E402

WRITE = r'''
local A = use("/Kosmos/Libraries/automations.lua")
print("W " .. tostring(A.write{ name = "Answer", when = { kind = "hand" }, steps = {
  { verb = "open", app = "Calculator" }, { verb = "press", app = "Calculator", control = "C" },
  { verb = "press", app = "Calculator", control = "7" }, { verb = "press", app = "Calculator", control = "×" },
  { verb = "press", app = "Calculator", control = "6" }, { verb = "press", app = "Calculator", control = "=" },
  { verb = "read", app = "Calculator", control = "Result", as = "answer" },
  { verb = "notify", title = "The answer is {answer}" } } }))
print("W " .. tostring(A.write{ name = "Good morning", when = { kind = "at", time = "8:00",
  days = { "mon", "tue", "wed", "thu", "fri" } }, steps = { { verb = "notify", title = "Morning" } } }))
'''

# What Tracker does with a double-click: the type's opener, launched with
# the file.
OPENER = ('local ft = use("/Kosmos/Libraries/filetypes.lua")\n'
          'local p = ft.opener(args, fs.getattr(args))\n'
          'print("OPENER " .. tostring(p))\n'
          'if p then fs.send("/Running/wm", { type = "launch", program = p,\n'
          '  args = use("/Kosmos/Libraries/files.lua").quote(args) }) end\n')

FILE = ('local a = fs.read("/Home/Automations/" .. (args:gsub(\'"\', "")) .. ".automation")\n'
        'print("FILE " .. tostring(type(a) == "table" and a.on) .. " " ..\n'
        '      tostring(type(a) == "table" and a.when.kind) .. " " ..\n'
        '      tostring(type(a) == "table" and a.when.minutes) .. " " ..\n'
        '      tostring(type(a) == "table" and #a.steps))\n')


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    checks, fails, said = 0, [], {}

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(complaint)

    def line(text, since, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            guest._read_available()
            m = re.search(re.escape(text) + r"([^\r\n]*)\r?\n", guest.seen[since:])
            if m:
                return m.group(1)
            time.sleep(0.1)
        return None

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        def step(words):
            out = session.run("automate " + words).decode(errors="replace")
            said[words] = out
            return out

        session.put(WRITE.encode(), "/Temporary/write.lua")
        session.put(FILE.encode(), "/Temporary/file.lua")
        session.put(OPENER.encode(), "/Temporary/opener.lua")
        said["write"] = session.run("/Temporary/write.lua").decode(errors="replace")

        # ---- 1: it opens on them ----
        mark = len(guest.seen)
        out = step("open Automations")
        check("open as Automations" in out, "the window did not open: %r" % out)
        opened = line("automations: window at ", mark)
        check(opened is not None and ", 2 listed" in opened, "it did not list the two: %r" % opened)
        out = step("controls Automations")
        for want in ("row/Answer", "row/Good morning", "switch/Answer", "button/Run now",
                     "button/+ New", "button/Change", "button/Add a step", "button/History"):
            check(re.search("^" + re.escape(want) + r"\s", out, re.M) is not None,
                  "the window did not publish %s: %r" % (want, out[-500:]))

        # ---- 2: Run now, lit and done ----
        mark = len(guest.seen)
        step("press Automations row/Answer")
        check(line("automations: chose Answer", mark) is not None, "Answer was not chosen")
        out = step('press Automations "Run now"')
        check("button/Run now" in out, "Run now was not pressed: %r" % out)
        ran = line("automations: Answer ran - ", mark, 60)
        check(ran == "done", "the window did not see the run done: %r" % ran)
        out = step("read Automations row/Answer")
        check(re.search(r"ran \d+:\d\d, done", out) is not None, "the row did not say it ran: %r" % out)
        out = step("read Automations step/7")
        check("Read Result in Calculator" in out, "step 7 did not read as its sentence: %r" % out)
        mark = len(guest.seen)
        step("press Automations button/History")
        hist = line("automations: history of Answer, ", mark)
        check(hist == "1 runs", "the history sheet did not have the run: %r" % hist)
        step("press Automations Done")

        # ---- 3: its switch off ----
        out = step("set Automations switch/Answer off")
        check("switch/Answer" in out, "the switch was not set: %r" % out)
        said["file"] = session.run("/Temporary/file.lua Answer").decode(errors="replace")
        check("FILE false hand" in said["file"], "off was not kept in the file: %r" % said["file"])
        time.sleep(1)
        out = step("automations")
        check(re.search(r"^Answer\s+off", out, re.M) is not None, "autod did not say it is off: %r" % out)

        # ---- 4: a new one, its trigger, a step ----
        mark = len(guest.seen)
        step('press Automations "+ New"')
        check(line("automations: kept New automation", mark) is not None, "+ New made nothing")
        mark = len(guest.seen)
        step("press Automations Change")
        check(line("automations: when, hand", mark) is not None, "Change did not open the trigger's sheet")
        out = step('choose Automations "What starts it" "Every so many minutes"')
        check("dropdown/What starts it" in out, "the trigger's kind was not chosen: %r" % out)
        step("type Automations Minutes 15")
        mark = len(guest.seen)
        step("press Automations Keep")
        check(line("automations: kept New automation", mark) is not None, "the trigger was not kept")
        out = step('read Automations "row/New automation"')
        said["file2"] = session.run('/Temporary/file.lua "New automation"').decode(errors="replace")
        check("FILE true every 15" in said["file2"], "every 15 minutes was not kept: %r" % said["file2"])

        mark = len(guest.seen)
        out = step('press Automations "Add a step"')
        check(line("automations: add a step, 16 verbs", mark) is not None, "Add a step offered no verbs")
        mark = len(guest.seen)
        out = step('press Automations "menu/Add a step/Open an application"')
        check(line("automations: editing step 1, open", mark) is not None,
              "choosing Open did not open its sheet: %r" % out)
        step("type Automations Application Calculator")
        mark = len(guest.seen)
        step("press Automations Keep")
        check(line("automations: kept New automation", mark) is not None, "the step was not kept")
        out = step("read Automations step/1")
        check("Open Calculator" in out, "the new step did not read as Open Calculator: %r" % out)

        # ---- 5: its file opened, as Tracker opens it: chosen and run ----
        mark = len(guest.seen)
        said["open file"] = session.run("/Temporary/opener.lua /Home/Automations/Answer.automation")\
            .decode(errors="replace")
        check("OPENER automations" in said["open file"],
              "Tracker would not open an .automation with Automations: %r" % said["open file"])
        check(line("automations: opened Answer from its file", mark) is not None,
              "opening Answer's file did not choose it: %r" % said["open file"])
        check(line("automations: Answer ran - ", mark, 60) == "done",
              "opening Answer's file did not run it")
    except Exception as e:                  # noqa: BLE001 - said below
        check(False, "the steps stopped: %s: %s" % (type(e).__name__, str(e)[:500]))
    finally:
        transcript = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on the Automations window:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("  %s: %r" % (k, v[-300:]))
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on the Automations window (opened on two, its controls published; "
          "Run now ran Answer, lit and done, the row and the history saying so; its switch "
          "off kept and told to autod; + New, Change to every 15 minutes, Add a step's Open of "
          "Calculator kept, all by name; its file opened as Tracker opens it, chosen and run)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
