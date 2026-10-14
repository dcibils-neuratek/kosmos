#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Every control published, by what a person sees (`docs/automations.md` A1).

One boot, driven over Telnet with nothing but names - no pointer, no
coordinates:

  1. **Calculator**: `/Running/Calculator/controls` lists its kinds, and
     `controls/button` its keys by their faces; `display/Result` reads 0;
     C, 7, ×, 6 and = pressed by name, and the display reads 42.
  2. **The refusals**: a key that is not there says so, and a display
     cannot be pressed.
  3. **The Text Editor**: the dots' menu is published as `menu/more/...`;
     its Replace button is not a control while the find bar is hidden, and
     choosing `menu/more/Find and replace` by name - the same handler the
     click calls - makes it one.

Usage: run_controls.py IMAGE
"""

import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_servers as S                                     # noqa: E402

LAUNCH = ('local program, rest = args:match("^(%S+)%s*(.*)$")\n'
          'local r = fs.send("/Running/wm", { type = "launch", program = program,\n'
          '                                   args = rest ~= "" and rest or nil })\n'
          'print("LAUNCH " .. tostring(r and r.ok))\n')

# The name under /Running a window registered with whose title holds `args`.
FIND = ('local names = fs.list("/Running") or {}\n'
        'for _, n in ipairs(names) do\n'
        '  local s = type(n) == "table" and n.name or n\n'
        '  if tostring(s):find(args, 1, true) then print("NAME " .. tostring(s)) end\n'
        'end\n')

# `args` is "<app>|<path under controls>": its listing.
LIST = ('local app, under = args:match("^([^|]*)|(.*)$")\n'
        'local r, why = fs.send("/Running/" .. app .. "/controls/" .. under, { type = "list" })\n'
        'if not r then print("LIST refused " .. tostring(why)) return end\n'
        'local out = {}\n'
        'for _, e in ipairs(r.entries or {}) do out[#out + 1] = tostring(type(e) == "table" and e.name or e) end\n'
        'print("LIST [" .. table.concat(out, "|") .. "]")\n')

# `args` is "<app>|<verb>|<address>[|<address>...]": each asked in turn.
ASK = ('local app, verb, rest = args:match("^([^|]*)|([^|]*)|(.*)$")\n'
       'for address in rest:gmatch("[^|]+") do\n'
       '  local r, why = fs.send("/Running/" .. app .. "/controls/" .. address, { type = verb })\n'
       '  if r then\n'
       '    print(("ASK %s %s ok %s"):format(verb, address, tostring(r.value)))\n'
       '  else\n'
       '    print(("ASK %s %s refused %s"):format(verb, address, tostring(why)))\n'
       '  end\n'
       'end\n')

NOTE = "# Notes\n\nA line to find.\n"


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

    def run(session, script, args=""):
        return session.run(("/Temporary/%s.lua %s" % (script, args)).strip()).decode(errors="replace")

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        for name, text in (("launch", LAUNCH), ("find", FIND), ("list", LIST), ("ask", ASK)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)
        session.put(NOTE.encode(), "/Temporary/notes.md")

        # ---- 1: Calculator, by its keys' faces ----
        mark = len(guest.seen)
        run(session, "launch", "/Kosmos/Apps/calc.lua")
        guest.wait_for_line("wm: window Calculator at ", "Calculator's window", mark)
        time.sleep(1)

        said["kinds"] = run(session, "list", "Calculator|")
        check(re.search(r"LIST \[[^\]]*button[^\]]*\]", said["kinds"])
              and "display" in said["kinds"],
              "Calculator's controls did not list button and display: %r" % said["kinds"])

        said["keys"] = run(session, "list", "Calculator|button")
        keys = (re.search(r"LIST \[([^\]]*)\]", said["keys"]) or [None, ""])[1].split("|")
        check(all(k in keys for k in ("7", "6", "×", "=", "C")),
              "Calculator's keys were not listed by their faces: %r" % said["keys"])

        said["before"] = run(session, "ask", "Calculator|read|display/Result")
        check("ASK read display/Result ok 0" in said["before"],
              "the display did not read 0 before a press: %r" % said["before"])

        said["press"] = run(session, "ask", "Calculator|press|button/C|button/7|button/×|button/6|button/=")
        check(said["press"].count(" ok ") == 5,
              "not every key was pressed by name: %r" % said["press"])

        said["after"] = run(session, "ask", "Calculator|read|display/Result")
        check("ASK read display/Result ok 42" in said["after"],
              "7 × 6 = did not read 42: %r" % said["after"])

        # ---- 2: the refusals ----
        said["none"] = run(session, "ask", "Calculator|press|button/Launch")
        check("refused no control at button/Launch" in said["none"],
              "a key that is not there was not refused: %r" % said["none"])

        said["unpressable"] = run(session, "ask", "Calculator|press|display/Result")
        check("refused display/Result cannot be pressed" in said["unpressable"],
              "the display was pressed: %r" % said["unpressable"])

        # ---- 3: the Text Editor, a menu item chosen by name ----
        mark = len(guest.seen)
        run(session, "launch", "/Kosmos/Apps/texteditor.lua /Temporary/notes.md")
        guest.wait_for_line("Text Editor at ", "the Text Editor's window", mark)
        time.sleep(1)

        found = re.search(r"NAME ([^\r\n]+)", run(session, "find", "Text Editor"))
        editor = found.group(1).strip() if found else "Text Editor"
        said["editor"] = editor

        said["menu"] = run(session, "list", editor + "|menu/more")
        check("Find and replace" in said["menu"] and "Wrap long lines" in said["menu"],
              "the dots' menu was not published as menu/more: %r" % said["menu"])

        said["hidden"] = run(session, "list", editor + "|button")
        check("Replace" not in said["hidden"].split("[", 1)[-1].split("|")
              and "LIST [" in said["hidden"],
              "Replace was a control while the find bar was hidden: %r" % said["hidden"])

        said["choose"] = run(session, "ask", editor + "|press|menu/more/Find and replace")
        check(" ok " in said["choose"],
              "Find and replace was not chosen by name: %r" % said["choose"])
        time.sleep(0.5)

        said["shown"] = run(session, "list", editor + "|button")
        shown = (re.search(r"LIST \[([^\]]*)\]", said["shown"]) or [None, ""])[1].split("|")
        check("Replace" in shown,
              "choosing Find and replace did not bring its Replace button: %r" % said["shown"])
    finally:
        transcript = guest.seen
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on the published controls:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("  %s: %r" % (k, v))
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on the published controls (Calculator's kinds and keys listed, "
          "7 × 6 = pressed by name and its display read 42, a missing key and a display "
          "refused; the Text Editor's dots' menu published and Find and replace chosen by "
          "name, its Replace button then a control)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
