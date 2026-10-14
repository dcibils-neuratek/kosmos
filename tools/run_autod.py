#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Automations run on their triggers by `autod` (`docs/automations.md` A3).

One boot with a disk for /Home. Automations are written as files in
/Home/Automations, `autod` is started as the Deskbar starts it, and:

  1. **By hand**: `automate automation Answer` - Calculator opened, C 7 × 6 =
     pressed, its Result read as {answer}, and a banner "The answer is 42";
     its history kept, every step ok.
  2. **A file appearing**: one put in /Temporary/inbox is copied to
     /Temporary/out by the automation watching the folder, {file} its path.
  3. **A failure, said once**: an automation pressing a button Calculator
     does not have fails with the reason in words and a banner - and run
     again, fails without a second banner.
  4. **Stop unless**: a run that stops where its text lacks the words, as a
     success, not a failure.
  5. **A file that is not an automation** is said so, and not run.
  6. **A mail arriving**, as `maild` writes it down: the automation for its
     sender runs with {sender} and {subject}, the one for another sender
     does not, and what arrived before `autod` started is not news.

Usage: run_autod.py IMAGE
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

_DISK = os.path.join(scratch.directory("autod"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "16"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_servers as S                                     # noqa: E402

# The automations, as the window will write them: tables, nothing executable.
WRITE = r'''
local automations = use("/Kosmos/Libraries/automations.lua")
local function keep(a) print("WROTE " .. a.name .. " " .. tostring(automations.write(a))) end
keep{ name = "Answer", when = { kind = "hand" }, steps = {
  { verb = "open", app = "Calculator" },
  { verb = "press", app = "Calculator", control = "C" },
  { verb = "press", app = "Calculator", control = "7" },
  { verb = "press", app = "Calculator", control = "×" },
  { verb = "press", app = "Calculator", control = "6" },
  { verb = "press", app = "Calculator", control = "=" },
  { verb = "read", app = "Calculator", control = "Result", as = "answer" },
  { verb = "notify", title = "The answer is {answer}" } } }
keep{ name = "Inbox", when = { kind = "folder", path = "/Temporary/inbox" }, steps = {
  { verb = "copy", from = "{file}", to = "/Temporary/out" } } }
keep{ name = "Broken", when = { kind = "hand" }, steps = {
  { verb = "press", app = "Calculator", control = "Launch" } } }
keep{ name = "Stops", when = { kind = "hand" }, steps = {
  { verb = "stop_unless", text = "nothing stable here", has = "-stable" },
  { verb = "notify", title = "Stops should have stopped" } } }
keep{ name = "Mail in", when = { kind = "mail", from = "ana@" }, steps = {
  { verb = "notify", title = "Mail from {sender}: {subject}" } } }
keep{ name = "Mail other", when = { kind = "mail", from = "zed@" }, steps = {
  { verb = "notify", title = "Zed wrote" } } }
-- What arrived before autod started is not news to it.
use("/Kosmos/Libraries/mailarrived.lua").add{ { from = "ana@example.test", subject = "Before" } }
-- Not an automation: a verb nobody knows, kept anyway to see it refused.
files = use("/Kosmos/Libraries/files.lua")
print("BAD " .. tostring(fs.write("/Home/Automations/Bad.automation",
  { name = "Bad", when = { kind = "hand" }, steps = { { verb = "format_disk" } } })))
'''

LAUNCH = ('local r = fs.send("/Running/wm", { type = "launch", program = "autod" })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')

HISTORY = ('local h = use("/Kosmos/Libraries/automations.lua").history(args)\n'
           'local r = h[#h]\n'
           'if not r then print("HISTORY none") return end\n'
           'local ok = 0\n'
           'for _, s in ipairs(r.steps) do if s.ok then ok = ok + 1 end end\n'
           'print(("HISTORY %d runs, last %s, %d of %d steps ok%s"):format(#h, r.ok and "ok" or "failed",\n'
           '  ok, #r.steps, r.stopped and ", stopped" or ""))\n')

OUT = 'print("OUT " .. tostring(fs.read("/Temporary/out/note.txt")))\n'

ARRIVE = ('print("ARRIVED " .. tostring(use("/Kosmos/Libraries/mailarrived.lua").add{\n'
          '  { account = "me@example.test", from = "ana@example.test", subject = "Lunch" } }))\n')


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

    def line(text, what, since, seconds=40):
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

        for name, text in (("write", WRITE), ("launch", LAUNCH), ("history", HISTORY), ("out", OUT),
                           ("arrive", ARRIVE)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)

        session.run("mkdir /Temporary/inbox")
        session.run("mkdir /Temporary/out")
        said["write"] = session.run("/Temporary/write.lua").decode(errors="replace")
        check(len(re.findall(r"WROTE [\w ]+ true", said["write"])) == 6,
              "the six automations were not written: %r" % said["write"])

        mark = len(guest.seen)
        session.run("/Temporary/launch.lua")
        listed = line("autod: 7 automations - ", "autod's first reading", mark)
        check(listed is not None and "Answer" in listed and "Inbox" in listed,
              "autod did not read the automations: %r" % listed)
        bad = line("autod: Bad: step 1 does nothing Automations knows", "the bad file", mark, 5)
        check(bad is not None, "the file that is not an automation was not said so")

        # ---- 1: by hand ----
        mark = len(guest.seen)
        said["run"] = session.run("automate automation Answer").decode(errors="replace")
        check("Answer runs" in said["run"], "autod did not take the run: %r" % said["run"])
        done = line("autod: Answer done in ", "the run", mark, 60)
        check(done is not None, "Answer did not finish")
        banner = line("notifications: banner ", "the banner", mark, 20)
        check(banner is not None and "The answer is 42" in banner,
              "the banner did not say the answer: %r" % banner)
        said["history"] = session.run("/Temporary/history.lua Answer").decode(errors="replace")
        check("HISTORY 1 runs, last ok, 8 of 8 steps ok" in said["history"],
              "the history did not keep the run: %r" % said["history"])

        # ---- 2: a file appearing ----
        mark = len(guest.seen)
        session.put(b"a note\n", "/Temporary/inbox/note.txt")
        done = line("autod: Inbox done in ", "the folder's automation", mark, 30)
        check(done is not None, "a file appearing did not run the folder's automation")
        said["out"] = session.run("/Temporary/out.lua").decode(errors="replace")
        check("OUT a note" in said["out"], "the file was not copied: %r" % said["out"])

        # ---- 3: a failure, said once ----
        mark = len(guest.seen)
        session.run("automate automation Broken")
        failed = line("autod: Broken failed - ", "the failure", mark, 30)
        check(failed is not None and "no control called Launch" in failed,
              "the failure was not said with its reason: %r" % failed)
        first = line("notifications: banner ", "the failure's banner", mark, 20)
        check(first is not None and "Broken failed" in first,
              "the failure had no banner: %r" % first)
        mark = len(guest.seen)
        session.run("automate automation Broken")
        again = line("autod: Broken failed - ", "the second failure", mark, 30)
        check(again is not None, "the second run did not fail")
        time.sleep(2)
        guest._read_available()
        check("notifications: banner" not in guest.seen[mark:],
              "the second failure gave a second banner")
        said["status"] = session.run("automate automations").decode(errors="replace")
        check(re.search(r"Broken\s+hand\s+last run failed", said["status"]) is not None
              and re.search(r"Bad\s+bad", said["status"]) is not None,
              "the status did not say Broken failed and Bad is bad: %r" % said["status"])

        # ---- 4: stop unless ----
        mark = len(guest.seen)
        session.run("automate automation Stops")
        stopped = line("autod: Stops stopped in ", "the stop", mark, 30)
        check(stopped is not None and "does not have -stable" in stopped,
              "the run did not stop where its text lacked the words: %r" % stopped)
        said["stops"] = session.run("/Temporary/history.lua Stops").decode(errors="replace")
        check("last ok, 0 of 1 steps ok, stopped" in said["stops"],
              "a stop was kept as a failure: %r" % said["stops"])
        # ---- 5: a mail arriving, as maild writes it down ----
        guest._read_available()
        check("Mail in queued" not in guest.seen, "mail from before autod started ran an automation")
        mark = len(guest.seen)
        said["arrive"] = session.run("/Temporary/arrive.lua").decode(errors="replace")
        done = line("autod: Mail in done in ", "the mail's automation", mark, 30)
        check(done is not None, "a mail from ana@ did not run Mail in")
        banner = line("notifications: banner ", "the mail's banner", mark, 20)
        check(banner is not None and "Mail from ana@example.test: Lunch" in banner,
              "the banner did not carry {sender} and {subject}: %r" % banner)
        guest._read_available()
        check("Mail other queued" not in guest.seen[mark:], "mail from ana@ ran the automation for zed@")
    except Exception as e:                  # noqa: BLE001 - said below
        check(False, "the steps stopped: %s: %s" % (type(e).__name__, str(e)[:500]))
    finally:
        transcript = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on autod:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("  %s: %r" % (k, v[-400:]))
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on autod (the automations read and a bad file said so; by hand, "
          "Calculator pressed and its answer in a banner, the run in the history; a file "
          "appearing copied by the folder's automation; a failure said once with its "
          "reason; a stop kept as a stop)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
