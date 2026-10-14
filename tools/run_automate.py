#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The verbs of an automation, one step at a time (`docs/automations.md` A2).

One boot, driven over Telnet by the `automate` program - each line a step as
an automation will run it, through `automate.lua`:

  1. **open** Calculator by the name it declares (its program is `calc`), and
     open again finds it rather than starting a second; **press** 7 × 6 =
     by label and **read** its Result: 42. A label nothing has is said so.
  2. **open** the Text Editor with a file; **menu** more / Find and replace;
     **type** into its Find field by the field's hint; **read** it back.
  3. **mail** through `mailout.lua`, the Outbox's one door: the message and
     its envelope written, from the account that sends, with its subject.
  4. **check** a page this Mac serves: its title, and a page without the
     words asked for refused in words.
  5. **copy** into a folder, **notify**, **wait**, **run** a program to its
     end.

Usage: run_automate.py IMAGE
"""

import http.server
import os
import random
import re
import socketserver
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

# A disk for /Home, where Mail's account and its Outbox are.
_DISK = os.path.join(scratch.directory("automate"), "disk.img")
subprocess.run([os.path.join(os.path.dirname(HERE), "build", "host", "lua"),
                os.path.join(HERE, "kfs.lua"), "create", _DISK, "16"],
               check=True, capture_output=True, cwd=os.path.dirname(HERE))
os.environ["KOSMOS_DISK"] = _DISK

import run_servers as S                                     # noqa: E402

PAGE = (b"<html><head><title>Kosmos builds</title></head>"
        b"<body><p>kosmos-usb-0.11.219-stable.img</p></body></html>")

# Mail's account, as Mail writes one: it sends by a server nobody answers
# on, so what is checked is what was written, never what was sent.
ACCOUNT = ('files = use("/Kosmos/Libraries/files.lua")\n'
           'files.make_folder("/Home/Mail/auto@example.test")\n'
           'print("ACCOUNT " .. tostring(fs.write("/Home/Mail/auto@example.test/account",\n'
           '  { address = "auto@example.test", name = "Auto",\n'
           '    imap = { host = "10.0.2.2", port = 9, security = "none" },\n'
           '    smtp = { host = "10.0.2.2", port = 9, security = "none" } })))\n')

# The Outbox, as it was left: each message's envelope and its head.
OUTBOX = ('for _, n in ipairs(fs.list("/Home/Mail/Outbox") or {}) do\n'
          '  local id = tostring(n):match("^(%x+)%.send$")\n'
          '  if id then\n'
          '    local env = fs.read("/Home/Mail/Outbox/" .. id .. ".send") or {}\n'
          '    local eml = fs.read("/Home/Mail/Outbox/" .. id .. ".eml") or ""\n'
          '    print(("OUT %s from %s to %s subject %s eml %d %s"):format(id, tostring(env.account),\n'
          '      table.concat(env.rcpt or {}, ","), tostring(env.subject), #tostring(eml),\n'
          '      tostring(tostring(eml):match("Subject: ([^\\r\\n]*)"))))\n'
          '  end\n'
          'end\n')

COPIED = 'print("COPIED " .. tostring(fs.read("/Temporary/kept/notes.md")))\n'

# A window with more controls than one message can name: 120 of the kit's
# buttons, about three kilobytes of addresses - so they come a page at a
# time or not at all. Each says when it is pressed.
MANY = ('-- kosmos: application\n'
        'local ui = use("/Kosmos/Libraries/ui.lua")\n'
        'local win = ui.window{ title = "Many", w = 900, h = 520, x = 120, y = 120 }\n'
        'for i = 1, 120 do\n'
        '  local x, y = 10 + ((i - 1) % 10) * 88, 10 + ((i - 1) // 10) * 40\n'
        '  win:add(ui.button{ x = x, y = y, text = "Key " .. i,\n'
        '                     on_click = function() print("many: Key " .. i .. " pressed") end })\n'
        'end\n'
        'win:run()\n')


class Quiet(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        body = PAGE if self.path == "/builds" else b"<html><body>nothing</body></html>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"

    server = socketserver.TCPServer(("127.0.0.1", 0), Quiet)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    checks, fails, said = 0, [], {}

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(complaint)

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        def step(words):
            out = session.run("automate " + words).decode(errors="replace")
            said[words] = out
            return out

        for name, text in (("account", ACCOUNT), ("outbox", OUTBOX), ("copied", COPIED),
                           ("many", MANY)):
            session.put(text.encode(), "/Temporary/%s.lua" % name)
        session.put(b"# Notes\n\nA line to find.\n", "/Temporary/notes.md")

        # ---- 1: Calculator, opened by its name ----
        out = step("open Calculator")
        check("open as Calculator" in out, "Calculator was not opened by its name: %r" % out)
        out = step("open Calculator")
        check("open as Calculator" in out and "Calculator2" not in out,
              "opening it again did not find the one open: %r" % out)

        for key in ("C", "7", "×", "6", "="):
            out = step("press Calculator %s" % key)
            check("button/" + key in out, "%s was not pressed by its label: %r" % (key, out))

        out = step("read Calculator Result")
        check(re.search(r"^\s*42\s*$", out, re.M) is not None, "7 × 6 = did not read 42: %r" % out)

        out = step("press Calculator Launch")
        check("no control called Launch" in out, "a label nothing has was not said so: %r" % out)

        # ---- 2: the Text Editor, a menu and a field ----
        out = step('open "Text Editor" /Temporary/notes.md')
        check("open as notes.md - Text Editor" in out,
              "the Text Editor did not open with the file: %r" % out)
        out = step('menu "Text Editor" more "Find and replace"')
        check("menu/more/Find and replace" in out, "Find and replace was not chosen: %r" % out)
        out = step('type "Text Editor" Find "a line"')
        check("field/Find" in out, "nothing was typed into the Find field: %r" % out)
        out = step('read "Text Editor" field/Find')
        check("a line" in out, "the Find field did not read back what was typed: %r" % out)

        # ---- 2b: more controls than a message holds, a page at a time ----
        out = step("open /Temporary/many.lua")
        check("open as Many" in out, "the window of 120 buttons did not open: %r" % out)
        out = step("controls Many")
        listed = re.findall(r"^button/Key \d+", out, re.M)
        check(len(listed) == 120, "not every one of 120 buttons was listed: %d, %r"
              % (len(listed), out[-300:]))
        mark = len(guest.seen)
        out = step('press Many "Key 117"')
        check("button/Key 117" in out, "Key 117 was not pressed by name: %r" % out)
        try:
            pressed = guest.wait_for_line("many: Key 117 pressed", "the press", mark)
        except Exception:                  # noqa: BLE001 - its check says
            pressed = None
        check(pressed is not None, "Key 117's own handler did not run")

        # ---- 2c: a window that draws itself, by the names it gives ----
        # Its account first, so Mail opens on its mailboxes and not on the
        # sheet that asks for one.
        said["account"] = session.run("/Temporary/account.lua").decode(errors="replace")
        out = step("open Mail")
        check("open as Mail" in out, "Mail did not open: %r" % out)
        out = step("controls Mail")
        for want in ("button/New Message", "button/Get Mail", "button/Reply", "button/more",
                     "menu/more/Get Mail", "field/Search"):
            check(re.search("^" + re.escape(want) + r"\s", out, re.M) is not None,
                  "Mail did not publish %s: %r" % (want, out[-600:]))
        check(re.search(r"^button/Reply\s.*\(disabled\)", out, re.M) is not None,
              "Reply was not disabled with no message chosen: %r" % out[-600:])
        out = step("press Mail Reply")
        check("disabled" in out, "Reply with no message was pressed: %r" % out)
        mark = len(guest.seen)
        out = step('press Mail "New Message"')
        check("button/New Message" in out, "New Message was not pressed by name: %r" % out)
        try:
            composer = guest.wait_for_line("wm: window New Message at ", "the composer", mark)
        except Exception:                  # noqa: BLE001 - its check says
            composer = None
        check(composer is not None, "pressing New Message opened no composer")
        out = step("type Mail Search Invoice")
        check("field/Search" in out, "Mail's search was not typed into: %r" % out)
        out = step("read Mail Search")
        check("Invoice" in out, "Mail's search did not read back: %r" % out)

        # ---- 3: a mail, into the Outbox ----
        out = step('mail ana@example.test "A stable build" "0.11.219 is stable."')
        check("in the Outbox as " in out, "the mail was not written: %r" % out)
        said["outbox"] = session.run("/Temporary/outbox.lua").decode(errors="replace")
        check(re.search(r"OUT \w+ from auto@example.test to ana@example.test subject A stable build "
                        r"eml [1-9]\d* A stable build", said["outbox"]) is not None,
              "the Outbox did not hold the message and its envelope: %r" % said["outbox"])

        # ---- 4: a page checked ----
        out = step("check http://10.0.2.2:%d/builds -stable" % port)
        check("200 Kosmos builds" in out, "the page was not checked with its title: %r" % out)
        out = step("check http://10.0.2.2:%d/other -stable" % port)
        check("does not say" in out and "-stable" in out,
              "a page without the words was not refused in words: %r" % out)

        # ---- 5: copy, notify, wait, run ----
        session.run("mkdir /Temporary/kept")
        out = step("copy /Temporary/notes.md /Temporary/kept")
        check("/Temporary/kept/notes.md" in out, "the copy did not go into the folder: %r" % out)
        said["copied"] = session.run("/Temporary/copied.lua").decode(errors="replace")
        check("A line to find." in said["copied"], "the copy did not hold the file: %r" % said["copied"])

        mark = len(guest.seen)
        out = step('notify "Automated" "from a step"')
        check(re.search(r"^\s*\d+\s*$", out, re.M) is not None, "the banner was not posted: %r" % out)
        try:
            shown = guest.wait_for_line("notifications: banner", "the banner shown", mark)
        except Exception:                  # noqa: BLE001 - its check says
            shown = None
        check(shown is not None, "the banner was not shown")

        t0 = time.monotonic()
        out = step("wait 2")
        check("waited" in out and time.monotonic() - t0 >= 1.8, "wait 2 did not wait: %r" % out)

        out = step("run hello")
        check("ran hello" in out, "a program was not run to its end: %r" % out)
    except Exception as e:                  # noqa: BLE001 - said below
        check(False, "the steps stopped: %s: %s" % (type(e).__name__, str(e)[:500]))
    finally:
        transcript = guest.seen
        guest.close()
        server.shutdown()

    if fails:
        print("FAIL: %d of %d checks on the automate verbs:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        for k, v in said.items():
            print("  %s: %r" % (k, v[-300:]))
        print("--- the guest said ---\n" + transcript[-3000:])
        return 1

    print("PASS: %d checks on the automate verbs (Calculator opened by its name and found "
          "again, 7 × 6 = pressed by label and read 42, a missing label said; the Text "
          "Editor's menu chosen and its Find field typed into and read; a mail written to "
          "the Outbox with its envelope; a page checked and one refused in words; copy into "
          "a folder, a banner, a wait, a program run)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
