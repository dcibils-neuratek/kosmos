#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Mail's conversations: `imap.lua` and `smtp.lua` against `mailpeer.py`.

`docs/mail.md` M2. A mail server runs on this Mac (`tools/mailpeer.py`),
reached from the guest at 10.0.2.2 over TLS, its certificate signed by an
authority made for the test (`run_tls.pki`) and handed to the guest as
`/Home/ca.der`. Every message and address in it is made up. A script on
the guest's disk speaks to it and says what it found, and this holds both
what it said and what the server now holds:

  - signed in, and what the server can do; a wrong password refused with
    the server's own words;
  - the mailboxes by their use, `Café`'s name out of modified UTF-7, and
    the Inbox's count;
  - what is new, then a message fetched into a file and read by the Mail
    Kit - its Windows-1252 subject - and one of 420 KB, whose literal comes
    in many reads, byte for byte;
  - a flag and a move reaching the server, and what changed since, by
    CONDSTORE; an append to Sent;
  - IDLE told of a message delivered while it waited;
  - a message sent through SMTP with STARTTLS, to two people, a line that
    begins with a dot kept, and a recipient the server refuses said so.

Usage: run_mail.py IMAGE
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import mailpeer                                             # noqa: E402
import run_tls                                              # noqa: E402
import scratch                                              # noqa: E402

CHECK = r'''
local imap = use("/Kosmos/Libraries/imap.lua")
local smtp = use("/Kosmos/Libraries/smtp.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local mail = use("/Kosmos/Kits/mail")

local der = fs.read("/Home/ca.der")
local where = { host = "10.0.2.2", port = IMAP, name = "kosmos-test.local",
                anchors = { der }, user = "USER", password = "PASSWORD" }

local function say(...) print("M " .. table.concat({ ... }, " ")) end

local function run()
  local s, why = imap.open(where)
  if not s then return say("open", "failed", why) end

  local caps, cwhy = s:wait(s.ready)
  say("ready", tostring(caps ~= nil), caps and tostring(caps.MOVE and caps.CONDSTORE) or tostring(cwhy))

  local wrong = {}
  for k, v in pairs(where) do wrong[k] = v end
  wrong.password = "not-it"
  local w = imap.open(wrong)
  local _, wwhy = w:wait(w.ready)
  say("refused", tostring(wwhy))

  local boxes = s:wait(s:mailboxes()) or {}
  local named = {}
  for _, b in ipairs(boxes) do named[#named + 1] = b.title .. "=" .. tostring(b.use) end
  table.sort(named)
  say("boxes", table.concat(named, ","))

  local st = s:wait(s:status("INBOX")) or {}
  say("status", tostring(st.messages), tostring(st.unseen))

  local box = s:wait(s:select("INBOX")) or {}
  say("select", tostring(box.exists), tostring(box.uidvalidity), tostring(box.highestmodseq ~= nil))

  -- A first look at a large mailbox asks for its newest alone, by place.
  local two = s:wait(s:changes(nil, { newest = 2 })) or { new = {} }
  local newest = {}
  for _, m in ipairs(two.new) do newest[#newest + 1] = m.uid end
  table.sort(newest)
  say("newest", table.concat(newest, ","))

  local seen = s:wait(s:changes(nil)) or { new = {} }
  local uids = {}
  for _, m in ipairs(seen.new) do uids[#uids + 1] = m.uid end
  say("new", table.concat(uids, ","), tostring(seen.uid_high))

  local got, fwhy = s:wait(s:fetch(1, "/Home/m1.eml"))
  local r, size = regions.read_whole("/Home/m1.eml")
  local m = r and mail.parse(r.at, size)
  say("fetch1", tostring(got and got.bytes), tostring(m and m:header("subject")) or tostring(fwhy))
  regions.free(r)

  local big, bwhy = s:wait(s:fetch(3, "/Home/m3.eml"), 60)
  local r3, size3 = regions.read_whole("/Home/m3.eml")
  local m3 = r3 and mail.parse(r3.at, size3)
  local part
  for _, p in ipairs(m3 and m3:parts() or {}) do
    if p.name == "photos.bin" then part = p end
  end
  if part then
    local out = regions.make(part.bound)
    local n = m3:part_into(part.id, out.at, out.size)
    local head = sys.region_read(out.cap, 0, 4)
    local tail = sys.region_read(out.cap, n - 4, 4)
    say("fetch3", tostring(big and big.bytes), tostring(size3), tostring(n),
        (head:gsub(".", function(c) return ("%02x"):format(c:byte()) end)),
        (tail:gsub(".", function(c) return ("%02x"):format(c:byte()) end)))
    regions.free(out)
  else
    say("fetch3", "no attachment", tostring(bwhy))
  end
  regions.free(r3)

  local fl = s:flag({ 1 }, "seen", true)
  s:wait(fl)
  say("flag", tostring(fl.ok), tostring(fl.why))

  local since = { uidvalidity = seen.uidvalidity, uid_high = seen.uid_high, modseq = seen.modseq }
  local ch = s:wait(s:changes(since)) or { changed = {}, present = {} }
  local changed = {}
  for _, c in ipairs(ch.changed) do
    changed[#changed + 1] = c.uid .. (c.flags.seen and ":seen" or "")
  end
  say("changed", table.concat(changed, ","), tostring(#ch.new))

  local mv = s:move({ 2 }, "Archive")
  s:wait(mv)
  say("move", tostring(mv.ok), tostring(mv.why))

  local after = s:wait(s:changes(ch)) or { present = {} }
  say("present", table.concat(after.present, ","))

  fs.write("/Home/sent.eml", "From: lena@example.com\r\nTo: bob@example.net\r\n"
           .. "Subject: Kept in Sent\r\n\r\nA copy.\r\n")
  local ap = s:append("Sent", "/Home/sent.eml", { "seen" })
  local apr = s:wait(ap)
  say("append", tostring(ap.ok), tostring(apr and apr.uid), tostring(ap.why))

  local idle = s:idle()
  local hz = (sys.info() or {}).tick_hz or 250
  local until_ = sys.ticks() + 10 * (fs.read("/Devices/cpu") or {}).counter_hz
  while not s.news and sys.ticks() < until_ and not s.broken do
    s:step()
    s.stream.conn:wait(math.max(1, hz // 20))
  end
  local news = s.news
  s:done()
  s:wait(idle)
  say("idle", tostring(news), tostring(idle.ok), tostring(s.mailbox.exists))

  s:wait(s:logout())

  local job, jwhy = smtp.send{ host = "10.0.2.2", port = SMTP, name = "kosmos-test.local",
                               anchors = { der }, user = "USER", password = "PASSWORD",
                               from = "USER", to = { "bob@example.net", "carol@example.org" },
                               message = "From: USER\r\nTo: bob@example.net\r\n"
                                         .. "Subject: Sent from Kosmos\r\n\r\nFirst line.\r\n"
                                         .. ".a line that begins with a dot\r\nLast.\r\n" }
  local sent, swhy = job and job:wait(30)
  say("smtp", tostring(sent), tostring(swhy or jwhy))

  local bad = smtp.send{ host = "10.0.2.2", port = SMTP, name = "kosmos-test.local",
                         anchors = { der }, user = "USER", password = "PASSWORD",
                         from = "USER", to = { "nobody@refused.example.com" },
                         message = "Subject: x\r\n\r\ny\r\n" }
  local _, badwhy = bad:wait(30)
  say("smtp-refused", tostring(badwhy))
end

local ok, err = pcall(run)
if not ok then say("error", tostring(err)) end
print("MAILCHECK" .. " END")
'''


def setup():
    """The authority, the server, and their ports."""
    work = scratch.directory("mail")
    run_tls.pki(work)
    peer = mailpeer.Peer(work, "good.pem", "server.key")
    imap_port, smtp_port = peer.start()
    return work, peer, imap_port, smtp_port


def fill(text, imap_port, smtp_port):
    return (text.replace("IMAP", str(imap_port)).replace("SMTP", str(smtp_port))
            .replace("USER", mailpeer.USER).replace("PASSWORD", mailpeer.PASSWORD))


def boot(image, work, scripts):
    """A disk holding the authority and `scripts`, and the machine on the
    network with it."""
    disk = os.path.join(work, "disk.img")
    put = [os.path.join(work, "ca.der") + ":/Home/ca.der"]

    for name, text in scripts.items():
        with open(os.path.join(work, name), "w") as f:
            f.write(text)
        put.append(os.path.join(work, name) + ":/Home/" + name)

    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32", *put],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    import run_network                                      # noqa: F401 - after the disk
    import run_screenshot as R

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        guest = R.Guest(image, 180)
    finally:
        setattr(R, board, saved)

    return guest, R


def said_lines(said, mark="M "):
    lines = {}

    for line in said.splitlines():
        if line.startswith(mark):
            name, _, rest = line[len(mark):].partition(" ")
            lines[name] = rest

    return lines


def part1(image):
    work, peer, imap_port, smtp_port = setup()
    guest, R = boot(image, work, {"mailcheck.lua": fill(CHECK, imap_port, smtp_port)})
    said, error = "", None

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")
        mark = len(guest.seen)
        guest.type("/Home/mailcheck.lua")
        guest.wait_for_line("MAILCHECK END", "the mail check", since=mark)
        said = guest.seen[mark:].replace("\r", "")
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
        said = guest.seen.replace("\r", "")
    finally:
        guest.close()
        peer.stop()

    lines = said_lines(said)
    fails = []

    def expect(name, want, what):
        got = lines.get(name)

        if got != want:
            fails.append("%s: said %r, not %r" % (what, got, want))

    if error:
        fails.append("the machine stopped: " + error)

    if "error" in lines:
        fails.append("the check raised: " + lines["error"])

    expect("ready", "true true", "signed in, MOVE and CONDSTORE")

    if "AUTHENTICATIONFAILED" not in lines.get("refused", ""):
        fails.append("a wrong password was not refused in the server's words: %r"
                     % lines.get("refused"))

    expect("boxes", "Archive=archive,Café=nil,Drafts=drafts,INBOX=inbox,Projects/Kosmos=nil,Projects=nil,Sent=sent,Trash=trash,[Gmail]=nil",
           "the mailboxes by their use")
    expect("status", "3 3", "the Inbox's messages and unseen")
    expect("select", "3 1000 true", "INBOX selected, with its UIDVALIDITY and a MODSEQ")
    expect("new", "1,2,3 3", "what is new in a mailbox seen for the first time")
    expect("newest", "2,3", "a first look asking for the newest two, by place")

    m1 = len(mailpeer.SEED[0])
    expect("fetch1", "%d Café on Saturday" % m1,
           "a message fetched and its Windows-1252 subject read by the Mail Kit")

    m3 = len(mailpeer.SEED[2])
    big = mailpeer.BIG
    expect("fetch3", "%d %d %d %s %s" % (m3, m3, len(big), big[:4].hex(), big[-4:].hex()),
           "420 KB fetched whole and its attachment undone")

    expect("flag", "true nil", "a flag stored")
    expect("changed", "1:seen 0", "what changed since, by CONDSTORE: the message flagged")
    expect("move", "true nil", "a message moved")
    expect("present", "1,3", "what is still in the Inbox after the move")
    expect("idle", "true true 3", "IDLE told of a message delivered while it waited")

    if not (lines.get("append", "").startswith("true ") and lines["append"].endswith(" nil")):
        fails.append("an append to Sent: said %r" % lines.get("append"))

    expect("smtp", "true nil", "a message sent through SMTP with STARTTLS")

    if "was refused" not in lines.get("smtp-refused", "") or "550" not in lines.get("smtp-refused", ""):
        fails.append("a refused recipient was not said: %r" % lines.get("smtp-refused"))

    # And what the server now holds.
    with peer.lock:
        inbox = peer.boxes["INBOX"].messages
        first = next((m for m in inbox if m["uid"] == 1), None)

        if not first or "\\Seen" not in first["flags"]:
            fails.append("the flag did not reach the server")

        if peer.find("Archive", "This week's route") is None or any(m["uid"] == 2 for m in inbox):
            fails.append("the move did not reach the server")

        if peer.find("Sent", "Kept in Sent") is None:
            fails.append("the append did not reach the server")

        sent = [s for s in peer.sent if "Sent from Kosmos" in s["data"]]

        if not sent or sent[0]["to"] != ["bob@example.net", "carol@example.org"] \
           or "\r\n.a line that begins with a dot\r\n" not in sent[0]["data"]:
            fails.append("the message sent did not arrive as written: %r" % (sent[:1],))

        if any("refused.example.com" in r for s in peer.sent for r in s["to"]):
            fails.append("a refused recipient's message was delivered")

    if " died: " in said:
        fails.append("something died: " + said[said.find(" died: ") - 80:][:300])

    checks = 21

    if fails:
        print("FAIL: %d of %d checks on mail's conversations:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on mail's conversations with a server on this Mac "
          "(signed in over TLS and a wrong password refused; mailboxes by their use, "
          "Café's name; what is new, and only the newest when asked; a message fetched and read by the Mail Kit, and "
          "420 KB byte for byte; a flag, a move and an append reaching the server; "
          "what changed by CONDSTORE; IDLE told of new mail; a message sent with "
          "STARTTLS to two people, a dot kept, and a refused recipient said)." % checks)
    return 0



# `maild`'s part: an account, kept, and what happens to it.
SETUP = r"""
local files = use("/Kosmos/Libraries/files.lua")
files.make_folder("/Home/Mail/USER")
local ok, why = fs.write("/Home/Mail/USER/account", {
  address = "USER", name = "Lena Moreau",
  imap = { host = "10.0.2.2", port = IMAP },
  smtp = { host = "10.0.2.2", port = SMTP },
  certificate = "/Home/ca.der", tls_name = "kosmos-test.local" })
print("SETUP " .. tostring(ok) .. " " .. tostring(why) .. " DONE")
"""

LOOK = r"""
local dir = "/Home/Mail/USER/INBOX"
local names = {}
for _, n in ipairs(fs.list(dir) or {}) do
  if n:match("%.eml$") then names[#names + 1] = n end
end
table.sort(names)
for _, n in ipairs(names) do
  local a = fs.getattr(dir .. "/" .. n) or {}
  print(("L %s|%s|%s|%s|%s|%s|%s"):format(n, tostring(a.from), tostring(a.subject),
        tostring(a.seen), tostring(a.attachments), tostring(a.date), tostring(a.preview)))
end
local boxes = fs.read("/Home/Mail/USER/mailboxes") or {}
local uses = {}
for _, b in ipairs(boxes) do uses[#uses + 1] = b.title .. "=" .. tostring(b.use) end
table.sort(uses)
print("B " .. table.concat(uses, ","))
local st = fs.send("/Running/maild", { type = "status" }) or {}
local a = (st.accounts or {})[1] or {}
print(("S %s %s %s %s"):format(tostring(st.ok), tostring(a.state), tostring(a.messages),
      tostring(a.unseen)))
print("LOOK" .. " END")
"""

SYNC = 'local r = fs.send("/Running/maild", { type = "sync" })\nprint("SYNC " .. tostring(r and r.ok))\n'

# A message's facts as an older reading worked them out.
STALE = ('fs.setattr("/Home/Mail/USER/INBOX/1.eml", { facts = 1, preview = "stale" })\n'
         'local r = fs.send("/Running/maild", { type = "sync" })\nprint("STALE " .. tostring(r and r.ok))\n')


def part2(image):
    work, peer, imap_port, smtp_port = setup()
    scripts = {"setup.lua": fill(SETUP, imap_port, smtp_port),
               "look.lua": fill(LOOK, imap_port, smtp_port), "sync.lua": SYNC,
               "stale.lua": fill(STALE, imap_port, smtp_port)}
    guest, R = boot(image, work, scripts)
    who = mailpeer.USER
    looks, error, said = [], None, ""

    def look():
        mark = len(guest.seen)
        guest.type("/Home/look.lua")
        guest.wait_for_line("LOOK END", "a look at what maild kept", since=mark)
        looks.append(guest.seen[mark:].replace("\r", ""))

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")

        mark = len(guest.seen)
        guest.type("/Home/setup.lua")
        guest.wait_for_line("DONE", "the account written", since=mark)

        mark = len(guest.seen)
        guest.type("mailpass keep imap://10.0.2.2:%d %s %s" % (imap_port, who, mailpeer.PASSWORD))
        guest.wait_for_line("mailpass: kept", "the password kept", since=mark)

        guest.type("maild &")
        guest.wait_for("maild: %s: 3 messages kept, 3 unseen" % who, "the Inbox kept")

        # IDLE: the server delivers while maild waits, and it says so.
        guest.wait_for("maild: %s: INBOX: 1 new, 0 changed, 0 gone" % who, "the arrival fetched")
        guest.wait_for('"Bob" from /Kosmos/Programs/maild.lua', "the arrival said")
        look()

        # Facts from an older reading, worked out again at the next look.
        mark = len(guest.seen)
        guest.type("/Home/stale.lua")
        guest.wait_for_line("maild: %s: INBOX: 1 worked out again" % who,
                            "an older reading's facts redone", since=mark)

        # Changed on the server: one message read, one deleted elsewhere.
        with peer.lock:
            inbox = peer.boxes["INBOX"]
            for m in inbox.messages:
                if m["uid"] == 2:
                    m["flags"].add("\\Seen")
                    peer.modseq += 1
                    m["modseq"] = peer.modseq
            inbox.messages = [m for m in inbox.messages if m["uid"] != 3]

        mark = len(guest.seen)
        guest.type("/Home/sync.lua")
        guest.wait_for_line("maild: %s: INBOX: 0 new, 1 changed, 1 gone" % who,
                            "the changes caught up", since=mark)
        look()
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        said = guest.seen.replace("\r", "")
        guest.close()
        peer.stop()

    fails = []

    if error:
        fails.append("the machine stopped: " + error + " ... " + said[-600:])

    def rows(text):
        return [l[2:].split("|") for l in text.splitlines() if l.startswith("L ")]

    def mark_of(text, m):
        return next((l[2:] for l in text.splitlines() if l.startswith(m + " ")), None)

    first = looks[0] if looks else ""
    r1 = rows(first)

    if [r[0] for r in r1] != ["1.eml", "2.eml", "3.eml", "4.eml"]:
        fails.append("the Inbox kept as files, and the arrival: %r" % [r[0] for r in r1])
    else:
        if r1[0][1] != "Tomás Ferreira" or r1[0][2] != "Café on Saturday":
            fails.append("a message's sender and subject as attributes: %r" % r1[0][:3])
        if r1[0][5] != "1791272467" or not r1[0][6].startswith("Lena, Yes - at ten"):
            fails.append("a message's date and preview as attributes: %r" % r1[0][5:])
        if r1[2][4] != "1":
            fails.append("the attachment counted: %r" % r1[2])
        if r1[3][1] != "Bob" or r1[3][2] != "While you were idling":
            fails.append("the message that arrived during IDLE: %r" % r1[3][:3])
        if any(r[3] != "false" for r in r1):
            fails.append("messages nobody read marked seen: %r" % [r[3] for r in r1])

    if mark_of(first, "B") != "Archive=archive,Café=nil,Drafts=drafts,INBOX=inbox,Projects/Kosmos=nil,Projects=nil,Sent=sent,Trash=trash,[Gmail]=nil":
        fails.append("the mailboxes kept: %r" % mark_of(first, "B"))

    if mark_of(first, "S") != "true idle 4 4":
        fails.append("/Running/maild's status: %r" % mark_of(first, "S"))

    second = looks[1] if len(looks) > 1 else ""
    r2 = rows(second)

    if r2 and not r2[0][6].startswith("Lena, Yes - at ten"):
        fails.append("a preview from an older reading not worked out again: %r" % r2[0][6])

    if [r[0] for r in r2] != ["1.eml", "2.eml", "4.eml"]:
        fails.append("a message deleted on the server not taken away: %r" % [r[0] for r in r2])
    elif r2[1][3] != "true":
        fails.append("a message read elsewhere not marked seen: %r" % r2[1])

    if mark_of(second, "S") != "true idle 3 2":
        fails.append("the status after the changes: %r" % mark_of(second, "S"))

    if any(l.startswith("maild:") and mailpeer.PASSWORD in l for l in said.splitlines()):
        fails.append("maild printed the password")

    if " died: " in said:
        fails.append("something died: " + said[said.find(" died: ") - 80:][:300])

    checks = 13

    if fails:
        print("FAIL: %d of %d checks on maild:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on maild against a server on this Mac (an account "
          "signed in with the keyring's password and never printing it; the Inbox "
          "kept as files with sender, subject, date, preview and attachments as "
          "attributes; the mailboxes; a message delivered during IDLE fetched and "
          "said in a notification; one read and one deleted on the server caught "
          "up by CONDSTORE; /Running/maild's status)." % checks)
    return 0



# The window's part (M4): Mail on the desktop, an account added through it.
KEYS = {".": "dot", "@": "shift-2", "-": "minus", " ": "spc", ":": "shift-semicolon",
        "/": "slash", "_": "shift-minus"}


def keys_for(text):
    out = []
    for ch in text:
        if ch in KEYS:
            out.append(KEYS[ch])
        elif ch.isupper():
            out.append("shift-" + ch.lower())
        else:
            out.append(ch)
    return out


def part3(image):
    import importlib
    import random
    import re
    import time

    work = scratch.directory("mail")
    # The certificate is for the address the guest types: 10.0.2.2.
    run_tls.pki(work, "10.0.2.2")
    peer = mailpeer.Peer(work, "good.pem", "server.key")
    peer.idle_delivers = False

    # An Inbox longer than the window, for the wheel to move.
    for i in range(12):
        peer.deliver("INBOX", ("From: Club <club@example.net>\r\nTo: lena@example.com\r\n"
                               "Subject: Notice %d\r\nDate: Mon, 5 Oct 2026 08:%02d:00 +0000\r\n"
                               "\r\nNotice %d.\r\n" % (i, i, i)).encode())

    # Labels enough that the sidebar is taller than the window, as Gmail's.
    for i in range(1, 21):
        name = "Label %02d" % i
        peer.boxes[name] = mailpeer.Box(name, "", 2000 + i)

    # A picture on the network the route names, served from this Mac -
    # fetched only when Load Pictures is pressed.
    import http.server
    import threading

    asked_far = []

    class Far(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            asked_far.append(self.path)
            body = mailpeer.png(60, 30, (0xc8, 0x40, 0x30))
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    far = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Far)
    threading.Thread(target=far.serve_forever, daemon=True).start()
    route = peer.find("INBOX", "This week's route")
    route["data"] = route["data"].replace(
        b'<img src="cid:map@example.net">',
        b'<img src="cid:map@example.net"> <img src="http://10.0.2.2:%d/far.png">' % far.server_address[1])

    # The three of the drawing newest, so they stay the first rows.
    inbox = peer.boxes["INBOX"]
    inbox.messages = inbox.messages[3:] + inbox.messages[:3]
    for m in inbox.messages:
        m["uid"] = inbox.next_uid
        inbox.next_uid += 1
    imap_port, smtp_port = peer.start()

    # The test's authority trusted as a person trusts one: a file in
    # /Home/Preferences/Authorities, which every TLS connection reads.
    disk = os.path.join(work, "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32",
                    os.path.join(work, "ca.der") + ":/Home/Preferences/Authorities/test.der"],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    import run_screenshot as R
    import run_servers as S
    importlib.reload(R)

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    said, error = {}, None
    who = mailpeer.USER

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        session = S.connect(telnet)

        mark = len(guest.seen)
        session.run("open mail")
        placed = guest.wait_for_line("wm: window Mail at ", "the Mail window", mark)
        sheet = guest.wait_for_line("mail: sheet ", "Add Account, with no account yet", mark)
        wx, wy = (int(v) for v in re.match(r"(\d+),(\d+)", placed).groups())
        width, height, _ = R.parse_ppm(guest.screendump())

        def click(x, y):
            guest.mouse_to(*R._to_tablet(wx + x, wy + y, width, height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.6)

        def keep_window(name):
            import kosmos_vnc as V
            w_, h_, rgb_ = R.parse_ppm(guest.screendump())
            ww, wh = (int(v) for v in re.search(r"(\d+)x(\d+)", placed).groups())
            rows_ = [rgb_[((y * w_) + wx) * 3:((y * w_) + wx + ww) * 3]
                     for y in range(wy, min(h_, wy + wh))]
            os.makedirs(os.path.join(ROOT, "build", "mail"), exist_ok=True)
            V.png(os.path.join(ROOT, "build", "mail", name), ww, len(rows_), b"".join(rows_))
            return b"".join(rows_)

        def at(name, text):
            m = re.search(r"\b" + name + r" (\d+),(\d+)", text)
            return (int(m.group(1)), int(m.group(2))) if m else None

        def typed(text):
            for k in keys_for(text):
                guest.sendkey(k)
                time.sleep(0.12)

        def tab():
            guest.sendkey("tab")
            time.sleep(0.3)

        def cleared(n=24):
            for _ in range(n):
                guest.sendkey("backspace")
                time.sleep(0.05)

        # Other IMAP, its fields in order.
        mark = len(guest.seen)
        click(*at("imap", sheet))
        sheet = guest.wait_for_line("mail: sheet ", "the IMAP fields", mark)
        click(*at("field1", sheet))
        typed("Lena Moreau"); tab()
        typed(who); tab()
        typed(mailpeer.PASSWORD); tab()
        cleared(); typed("10.0.2.2"); tab()
        cleared(6); typed(str(imap_port)); tab()
        cleared(); typed("10.0.2.2"); tab()
        cleared(6); typed(str(smtp_port))

        mark = len(guest.seen)
        guest.sendkey("ret")
        said["signing"] = guest.wait_for_line("mail: signing in ", "Sign In pressed", mark)
        said["signed"] = guest.wait_for_line("mail: signed in ", "the account signed in", mark)
        guest.wait_for("maild: %s: 15 messages kept" % who, "the Inbox kept")
        said["account"] = session.run("cat /Home/Mail/%s/account" % who).decode(errors="replace")
        said["boxes"] = guest.wait_for_line("mail: mailboxes ", "the mailboxes listed", 0)

        # The list, newest first: the photos, the route in HTML, the café.
        places = guest.wait_for_line("mail: places ", "the window's places", 0)
        lx, ly = at("list", places)
        rh = int(re.search(r"rows (\d+)", places).group(1))
        time.sleep(2)

        # The wheel over the sidebar, taller than the window, moves it on.
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(wx + 100, wy + 400, width, height))
        time.sleep(0.4)
        guest.mouse_button(True, "wheel-down")
        time.sleep(0.05)
        guest.mouse_button(False, "wheel-down")
        said["side"] = guest.wait_for_line("mail: sidebar from ", "the wheel over the sidebar", mark)

        # The wheel over the Inbox moves the list a row a notch.
        mark = len(guest.seen)
        guest.mouse_to(*R._to_tablet(wx + lx + 100, wy + ly + rh, width, height))
        time.sleep(0.4)
        guest.mouse_button(True, "wheel-down")
        time.sleep(0.05)
        guest.mouse_button(False, "wheel-down")
        said["wheel"] = guest.wait_for_line("mail: list from ", "the wheel over the list", mark).split()[0]
        guest.mouse_button(True, "wheel-up")
        time.sleep(0.05)
        guest.mouse_button(False, "wheel-up")
        time.sleep(1.5)
        guest._read_available()
        said["turns"] = re.findall(r"mail: list from [^\n]*", guest.seen[mark:])

        mark = len(guest.seen)
        click(lx + 100, ly + rh // 2)
        said["photos"] = guest.wait_for_line("mail: showing ", "the newest shown", mark)

        mark = len(guest.seen)
        click(lx + 100, ly + rh + rh // 2)
        said["route"] = guest.wait_for_line("mail: showing ", "the second shown", mark)
        said["html"] = guest.wait_for_line("mail: html laid out at ", "the route's HTML laid out", mark)
        said["asked_before"] = len(asked_far)
        load = guest.wait_for_line("mail: load pictures at ", "Load Pictures shown", mark)
        lpx, lpy = (int(v) for v in load.split(","))
        click(lpx, lpy)
        guest.wait_for_line(" pictures came", "the pictures fetched", mark)
        said["came"] = re.findall(r"mail: (\d+ of \d+) pictures came", guest.seen[mark:])[-1:]
        said["asked_after"] = len(asked_far)
        time.sleep(1.5)
        keep_window("mail-html.png")

        mark = len(guest.seen)
        click(lx + 100, ly + 2 * rh + rh // 2)
        said["cafe"] = guest.wait_for_line("mail: showing ", "the oldest shown", mark)
        said["set"] = guest.wait_for_line("mail: set ", "its text set", mark)
        time.sleep(2)

        # The window as it looked, kept for whoever reads the run after.
        rows_ = keep_window("mail.png")
        said["paper"] = sum(1 for i in range(0, len(rows_), 3 * 7)
                            if rows_[i:i + 3] == b"\xf7\xf7\xf5")

        # Flag it; then delete the photos and archive the route.
        mark = len(guest.seen)
        click(*[v + 13 for v in at("flag", places)])
        said["flag"] = guest.wait_for_line("mail: flagged ", "the flag pressed", mark)

        mark = len(guest.seen)
        click(lx + 100, ly + rh // 2)
        guest.wait_for_line("mail: showing ", "the photos again", mark)
        click(*[v + 13 for v in at("delete", places)])
        said["delete"] = guest.wait_for_line("mail: delete ", "Delete pressed", mark)

        mark = len(guest.seen)
        click(lx + 100, ly + rh // 2)
        guest.wait_for_line("mail: showing ", "the route at the top", mark)
        click(*[v + 13 for v in at("archive", places)])
        said["archive"] = guest.wait_for_line("mail: archive ", "Archive pressed", mark)
        guest.wait_for("maild: %s: archive " % who, "the archive done on the server")
        time.sleep(2)
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        seen = guest.seen.replace("\r", "")
        guest.close()
        peer.stop()

        # The whole transcript, kept for whoever reads a failure after.
        os.makedirs(os.path.join(ROOT, "build", "mail"), exist_ok=True)
        with open(os.path.join(ROOT, "build", "mail", "guest.log"), "w") as f:
            f.write(seen)

    fails = []

    if error:
        fails.append("the machine stopped: " + error + " ... " + seen[-800:])

    if not said.get("signed", "").startswith(who):
        fails.append("the account was not signed in through Add Account: %r" % said.get("signed"))

    acc = said.get("account", "")
    if "10.0.2.2" not in acc or mailpeer.PASSWORD in acc:
        fails.append("the account file, its server and no password: %r" % acc[:300])

    labels = ", ".join("Label %02d" % i for i in range(1, 21))
    if said.get("boxes") != "Inbox, Drafts, Sent, Archive, Trash, Café, " + labels + ", Projects, -Kosmos":
        fails.append("the mailboxes as a person reads them, [Gmail] left out: %r"
                     % said.get("boxes"))

    if mailpeer.PASSWORD in seen:
        fails.append("the password was printed")

    if said.get("side") != "102":
        fails.append("a notch of the wheel over the sidebar: %r" % said.get("side"))

    if not (len(said.get("turns", [])) == 2 and said["turns"][0].startswith("mail: list from 4 ")
            and said["turns"][1].startswith("mail: list from 1 ")):
        fails.append("a notch of the wheel over the list: %r" % said.get("turns"))
    if not said.get("photos", "").startswith("30, The photos"):
        fails.append("the newest message first: %r" % said.get("photos"))
    if not said.get("route", "").startswith("29, This week's route"):
        fails.append("the second, in HTML: %r" % said.get("route"))
    if not re.search(r"\d+ px tall, \d+ wide, fitted at 100%, 1 sent inside, 1 on the network", said.get("html", "")):
        fails.append("the route's HTML, its own picture and one on the network: %r" % said.get("html"))
    if said.get("asked_before") != 0:
        fails.append("a picture on the network fetched before Load Pictures: %r" % said.get("asked_before"))
    if said.get("came") != ["1 of 1"] or said.get("asked_after") != 1:
        fails.append("Load Pictures fetched the one picture: %r, asked %r"
                     % (said.get("came"), said.get("asked_after")))
    if not said.get("cafe", "").startswith("28, Café on Saturday, from Tomás Ferreira"):
        fails.append("the café message read by the Mail Kit: %r" % said.get("cafe"))
    if not re.match(r"\d+ paragraphs", said.get("set", "")):
        fails.append("its text set by Write's engine: %r" % said.get("set"))
    if said.get("paper", 0) < 2000:
        fails.append("the paper not drawn: %d of its pixels" % said.get("paper", 0))

    with peer.lock:
        def flags(box, subject):
            m = peer.find(box, subject)
            return m["flags"] if m else None

        if "\\Seen" not in (flags("INBOX", "=?windows-1252?Q?Caf=E9_on_Saturday?=") or ()):
            fails.append("reading a message did not mark it seen on the server")
        if "\\Flagged" not in (flags("INBOX", "=?windows-1252?Q?Caf=E9_on_Saturday?=") or ()):
            fails.append("the flag did not reach the server")
        if peer.find("Trash", "The photos") is None or peer.find("INBOX", "The photos"):
            fails.append("Delete did not move the message to the Trash")
        if peer.find("Archive", "This week's route") is None or peer.find("INBOX", "This week's route"):
            fails.append("Archive did not move the message to the Archive")

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 19

    if fails:
        print("FAIL: %d of %d checks on Mail's window:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on Mail's window (an account added through Add Account "
          "against a server on this Mac, kept without its password; the Inbox newest "
          "first; a plain message, an HTML one and one with an attachment read by the "
          "Mail Kit, the text set by Write's engine on its paper; read, flagged, "
          "deleted to the Trash and archived, each on the server)." % checks)
    return 0


# The composer's part (M6): written, completed, answered, sent and kept.
PLANS = ("From: Priya Nair <priya.nair@example.com>\r\n"
         "To: Lena Moreau <lena@example.com>, Tom <tomas@example.org>\r\n"
         "Cc: Ana Ruiz <ana@example.com>\r\n"
         "Subject: Saturday plans\r\n"
         "Date: Fri, 9 Oct 2026 07:00:00 +0000\r\n"
         "Message-ID: <four@example.com>\r\n"
         "References: <one@example.org>\r\n"
         "Content-Type: text/plain; charset=utf-8\r\n"
         "\r\n"
         "Who's in for Saturday?\r\n").encode()



# A newsletter whose table is a fixed 900, wider than the pane: fitted.
WIDE = ("From: Weekly Wide <wide@example.net>\r\n"
        "To: lena@example.com\r\n"
        "Subject: The wide one\r\n"
        "Date: Fri, 9 Oct 2026 06:00:00 +0000\r\n"
        "Message-ID: <wide@example.net>\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "<table width=\"900\" style=\"width:900px\"><tr><td style=\"background:#e8e0d0\">"
        "<p>The left edge of a wide newsletter.</p></td>"
        "<td style=\"text-align:right;background:#d0e0e8\"><p>The right edge, which must be seen.</p>"
        "</td></tr></table>\r\n").encode()


# A picture far wider than its column, not loaded: the column is what the
# page is, so it is not shrunk.
PICTURE_WIDE = ("From: Picture Post <pictures@example.net>\r\n"
                "To: lena@example.com\r\n"
                "Subject: One big picture\r\n"
                "Date: Fri, 9 Oct 2026 05:00:00 +0000\r\n"
                "Message-ID: <picture@example.net>\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "\r\n"
                "<div style=\"width:500px\"><p>A column of words.</p>"
                "<img width=\"1400\" height=\"700\" src=\"http://10.0.2.2:9/big.png\"></div>"
                # As newsletters are made: a table, which grows to hold its
                # picture before a percentage can hold it back (the M700).
                "<table><tr><td><p>And in a table.</p>"
                "<img width=\"1456\" height=\"728\" src=\"http://10.0.2.2:9/bigger.png\"></td></tr></table>"
                # And as Substack makes them (the M700): a table
                # of 100% whose cell says the picture's own 1456, which a
                # browser takes as a wish and NetSurf as a floor.
                "<table width=\"100%\"><tr><td></td><td class=\"content\" width=\"1456\">"
                "<img width=\"1456\" height=\"819\" src=\"http://10.0.2.2:9/biggest.png\"></td></tr></table>\r\n").encode()


def mail_window(image, deliver, kept, said):
    """Mail on the desktop against the peer on this Mac, an account added
    through Add Account as a person adds one - a telnet session is not
    handed Mail's passwords, rightly - and `deliver` in its Inbox first,
    oldest first. What parts 4 and 5 stand on; on a failure the machine and
    the peer are stopped before it is raised."""
    import importlib
    import random
    import re
    import time
    import types

    work = scratch.directory("mail")
    run_tls.pki(work, "10.0.2.2")
    peer = mailpeer.Peer(work, "good.pem", "server.key")
    peer.idle_delivers = False

    for data in deliver:
        peer.deliver("INBOX", data)

    imap_port, smtp_port = peer.start()

    disk = os.path.join(work, "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32",
                    os.path.join(work, "ca.der") + ":/Home/Preferences/Authorities/test.der"],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk

    import run_screenshot as R
    import run_servers as S
    importlib.reload(R)

    telnet, web = random.randint(20000, 40000), random.randint(40001, 60000)
    guest = S.boot(image, telnet, web)
    who = mailpeer.USER
    w = types.SimpleNamespace(guest=guest, peer=peer, who=who, R=R, imap_port=imap_port,
                              smtp_port=smtp_port)

    try:
        guest.wait_for("wm: window Deskbar at ", "the bar")
        guest.wait_for("telnetd: on port ", "telnetd listening")
        w.session = S.connect(telnet)

        mark = len(guest.seen)
        w.session.run("open mail")
        w.placed = guest.wait_for_line("wm: window Mail at ", "the Mail window", mark)
        sheet = guest.wait_for_line("mail: sheet ", "Add Account, with no account yet", mark)
        w.wx, w.wy = (int(v) for v in re.match(r"(\d+),(\d+)", w.placed).groups())
        w.width, w.height, _ = R.parse_ppm(guest.screendump())

        def at(name, text):
            m = re.search(r"\b" + name + r" (\d+),(\d+)", text)
            return (int(m.group(1)), int(m.group(2))) if m else None

        def click(x, y, ox=None, oy=None):
            guest.mouse_to(*R._to_tablet((w.wx if ox is None else ox) + x, (w.wy if oy is None else oy) + y,
                                         w.width, w.height))
            time.sleep(0.3)
            guest.mouse_button(True)
            time.sleep(0.2)
            guest.mouse_button(False)
            time.sleep(0.6)

        def typed(text):
            for k in keys_for(text):
                guest.sendkey(k)
                time.sleep(0.12)

        def key(name):
            guest.sendkey(name)
            time.sleep(0.4)

        def composer(title, mark):
            line = guest.wait_for_line("mail: composer ", "a composer", mark)
            cid = line.split()[0]
            spot = guest.wait_for_line("wm: window %s at " % title, "the composer's window", mark)
            cx, cy = (int(v) for v in re.match(r"(\d+),(\d+)", spot).groups())
            where = guest.wait_for_line("mail: composer %s places " % cid, "its places", mark)
            time.sleep(1)
            return cid, cx, cy, where

        w.at, w.click, w.typed, w.key, w.composer = at, click, typed, key, composer

        mark = len(guest.seen)
        click(*at("imap", sheet))
        sheet = guest.wait_for_line("mail: sheet ", "the IMAP fields", mark)
        click(*at("field1", sheet))
        typed("Lena Moreau"); key("tab")
        typed(who); key("tab")
        typed(mailpeer.PASSWORD); key("tab")
        for _ in range(24): guest.sendkey("backspace")
        typed("10.0.2.2"); key("tab")
        for _ in range(6): guest.sendkey("backspace")
        typed(str(imap_port)); key("tab")
        for _ in range(24): guest.sendkey("backspace")
        typed("10.0.2.2"); key("tab")
        for _ in range(6): guest.sendkey("backspace")
        typed(str(smtp_port))
        mark = len(guest.seen)
        guest.sendkey("ret")
        said["setup"] = guest.wait_for_line("mail: signed in ", "the account signed in", mark)
        guest.wait_for("maild: %s: %d messages kept" % (who, kept), "the Inbox kept")
        w.places = guest.wait_for_line("mail: places ", "the window's places", 0)
        w.lx, w.ly = at("list", w.places)
        w.rh = int(re.search(r"rows (\d+)", w.places).group(1))
        time.sleep(2)
    except BaseException:
        guest.close()
        peer.stop()
        raise

    return w


def window_closed(w, log):
    """The machine and the peer stopped, the transcript kept; what it said."""
    seen = w.guest.seen.replace("\r", "") if w else ""

    if w:
        w.guest.close()
        w.peer.stop()

    os.makedirs(os.path.join(ROOT, "build", "mail"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "mail", log), "w") as f:
        f.write(seen)

    return seen


def part4(image):
    """The composer: Reply All quoting and threading, a new message whose
    address is completed and whose Bcc is in no header, a draft kept on the
    server and written on again, and a recipient the server refuses."""
    import re
    import time

    said, error, w = {}, None, None

    try:
        w = mail_window(image, [PICTURE_WIDE, WIDE, PLANS], 6, said)
        guest, peer, session, who, R = w.guest, w.peer, w.session, w.who, w.R
        at, click, typed, key, composer = w.at, w.click, w.typed, w.key, w.composer
        places, placed, wx, wy, lx, ly, rh = w.places, w.placed, w.wx, w.wy, w.lx, w.ly, w.rh
        width, height = w.width, w.height

        # 0. The wide newsletter, second newest: laid out wider than the
        #    pane and fitted to it, its right edge in the picture.
        mark = len(guest.seen)
        click(lx + 100, ly + rh + rh // 2)
        said["wide"] = guest.wait_for_line("mail: html laid out at ", "the wide one laid out", mark)
        time.sleep(1.5)
        import kosmos_vnc as V
        w_, h_, rgb_ = R.parse_ppm(guest.screendump())
        ww_, wh_ = (int(v) for v in re.search(r"(\d+)x(\d+)", placed).groups())
        rows_ = [rgb_[((y * w_) + wx) * 3:((y * w_) + wx + ww_) * 3] for y in range(wy, min(h_, wy + wh_))]
        V.png(os.path.join(ROOT, "build", "mail", "wide.png"), ww_, len(rows_), b"".join(rows_))

        # 0b. The big picture, third: no wider than its column, not fitted.
        mark = len(guest.seen)
        click(lx + 100, ly + 2 * rh + rh // 2)
        said["picture"] = guest.wait_for_line("mail: html laid out at ", "the big picture laid out", mark)
        time.sleep(1)

        # 1. Reply All to Priya's: everyone but Lena, quoted, threaded.
        mark = len(guest.seen)
        click(lx + 100, ly + rh // 2)
        said["plans"] = guest.wait_for_line("mail: showing ", "the plans shown", mark)
        mark = len(guest.seen)
        click(*[v + 13 for v in at("replyall", places)])
        rid, _, _, _ = composer("Reply All", mark)
        typed("See you there.")
        mark = len(guest.seen)
        key("ctrl-ret")
        said["reply_sent"] = guest.wait_for_line("mail: composer %s sent to " % rid, "Reply All sent", mark)
        guest.wait_for("maild: sent %s" % rid, "maild sent the reply")
        guest.wait_for("maild: %s: kept %s in Sent: done" % (who, rid), "the reply kept in Sent")

        # 2. A new message: Ana completed from three letters, Bob in Bcc.
        mark = len(guest.seen)
        click(*[v + 13 for v in at("compose", places)])
        nid, nx, ny, where = composer("New Message", mark)
        mark = len(guest.seen)
        typed("ana")
        said["suggests"] = guest.wait_for_line("mail: composer suggests ", "Ana suggested", mark)
        key("tab")                                   # taken; on to Cc
        key("tab")                                   # Bcc
        typed("bob@example.net,")
        key("tab")                                   # Subject
        typed("Lunch")
        key("tab")                                   # the body
        typed("Hello Ana.")
        time.sleep(1.5)

        # The composer as it looked, kept for whoever reads the run after.
        import kosmos_vnc as V
        w_, h_, rgb_ = R.parse_ppm(guest.screendump())
        rows_ = [rgb_[((y * w_) + nx) * 3:((y * w_) + nx + 680) * 3] for y in range(ny, min(h_, ny + 560))]
        V.png(os.path.join(ROOT, "build", "mail", "composer.png"), 680, len(rows_), b"".join(rows_))

        mark = len(guest.seen)
        click(*at("send", where), ox=nx, oy=ny)
        said["new_sent"] = guest.wait_for_line("mail: composer %s sent to " % nid, "the new one sent", mark)
        guest.wait_for("maild: sent %s" % nid, "maild sent the new one")

        # 3. A draft: closed half written, kept on the server, opened from
        #    Drafts and sent - its server copy gone after.
        mark = len(guest.seen)
        click(*[v + 13 for v in at("compose", places)])
        did, dx, dy, _ = composer("New Message", mark)
        typed("tomas@example.org")
        key("tab"); key("tab"); key("tab")
        typed("Draft plans")
        key("tab")
        typed("Half written.")
        # Closed by its red light, as a person closes it - which gives the
        # window a second to go - and the window gone before the draft is
        # written (the M700, 9 October: a slow disk past that second ended
        # the whole of Mail).
        mark = len(guest.seen)
        click(658, 22, ox=dx, oy=dy)
        said["kept"] = guest.wait_for_line("mail: composer %s closed" % did, "the draft closed", mark)
        said["close_order"] = (guest.seen.find("wm: closed New Message", mark),
                               guest.seen.find("mail: composer %s closed" % did, mark))
        guest.wait_for("maild: %s: draft %s kept in Drafts: done" % (who, did), "the draft on the server")
        with peer.lock:
            d = peer.find("Drafts", "Draft plans")
            said["draft_flags"] = sorted(d["flags"]) if d else None

        guest.wait_for("drafts ", "the sidebar with its Drafts")
        side = re.findall(r"mail: side ([^\n]*drafts[^\n]*)", guest.seen)[-1]
        mark = len(guest.seen)
        click(*at("drafts", side))
        # The first look at a mailbox is not said in the log: its file, then.
        for _ in range(40):
            if ".eml" in session.run("ls /Home/Mail/%s/Drafts" % who).decode(errors="replace"):
                break
            time.sleep(1)
        time.sleep(2)
        click(lx + 100, ly + rh // 2)
        said["reopened"] = guest.wait_for_line("mail: composer %s open" % did, "the draft opened again", mark)
        time.sleep(1)
        typed("Now whole. ")
        mark = len(guest.seen)
        key("ctrl-ret")
        guest.wait_for_line("mail: composer %s sent to " % did, "the draft sent", mark)
        guest.wait_for("maild: sent %s" % did, "maild sent the draft")
        guest.wait_for("maild: %s: draft %s taken from Drafts" % (who, did), "the draft's copy taken away")

        # 4. Nobody there: the server's refusal said, the message kept.
        mark = len(guest.seen)
        click(*[v + 13 for v in at("compose", places)])
        fid, _, _, _ = composer("New Message", mark)
        typed("nobody@refused.example.com")
        key("tab"); key("tab"); key("tab")
        typed("Nowhere")
        mark = len(guest.seen)
        key("ctrl-ret")
        said["refused"] = guest.wait_for_line("maild: not sent %s: " % fid, "the refusal said", mark)
        said["outbox"] = session.run("ls /Home/Mail/Outbox").decode(errors="replace")
        time.sleep(1)
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        seen = window_closed(w, "guest-4.log")

    if w is None:
        print("FAIL: Mail's composer: the machine never came up: %s" % error)
        return 1

    peer, who = w.peer, w.who

    fails = []

    if error:
        fails.append("the machine stopped: " + error + " ... " + seen[-1200:])

    if not said.get("setup", "").startswith(who):
        fails.append("the account was not signed in: %r" % said.get("setup"))

    with peer.lock:
        def sent(subject):
            got = [m for m in peer.sent if ("Subject: " + subject + "\r\n") in m["data"]]
            return got[0] if got else None

        r = sent("Re: Saturday plans")
        if not r:
            fails.append("Reply All did not reach the server: %r" % [m["data"][:200] for m in peer.sent])
        else:
            d = r["data"]
            if sorted(r["to"]) != ["ana@example.com", "priya.nair@example.com", "tomas@example.org"]:
                fails.append("Reply All went to %r, not everyone but Lena" % r["to"])
            for want in ("To: Priya Nair <priya.nair@example.com>, Tom <tomas@example.org>\r\n",
                         "Cc: Ana Ruiz <ana@example.com>\r\n", "In-Reply-To: <four@example.com>\r\n",
                         "References: <one@example.org> <four@example.com>\r\n",
                         "From: Lena Moreau <lena@example.com>\r\n",
                         "\r\n\r\nSee you there.\r\n",
                         "On 9 October 2026, Priya Nair wrote:\r\n> Who's in for Saturday?\r\n"):
                if want not in d:
                    fails.append("the reply lacked %r: %r" % (want, d[:900]))
        if peer.find("Sent", "Re: Saturday plans") is None:
            fails.append("the reply was not kept in Sent")
        p = peer.find("INBOX", "Saturday plans")
        if not p or "\\Answered" not in p["flags"]:
            fails.append("the answered message was not flagged Answered: %r" % (p and p["flags"]))

        n = sent("Lunch")
        if not n:
            fails.append("the new message did not reach the server")
        else:
            if sorted(n["to"]) != ["ana@example.com", "bob@example.net"]:
                fails.append("the new message went to %r" % n["to"])
            if "To: Ana Ruiz <ana@example.com>\r\n" not in n["data"]:
                fails.append("Ana's completed address not in To: %r" % n["data"][:500])
            if "bob@example.net" in n["data"] or "Bcc" in n["data"]:
                fails.append("the Bcc was in the message: %r" % n["data"][:500])
            if "Hello Ana.\r\n" not in n["data"]:
                fails.append("the new message's text: %r" % n["data"][:600])

        w = sent("Draft plans")
        if not w or "Now whole. Half written.\r\n" not in w["data"]:
            fails.append("the draft written on was not sent whole: %r" % (w and w["data"][:600]))
        if peer.find("Drafts", "Draft plans") is not None:
            fails.append("the sent draft's copy stayed in Drafts")

    m = re.match(r"(\d+) in [\d.]+ ms, \d+ px tall, (\d+) wide, fitted at (\d+)%", said.get("wide", ""))
    if not (m and int(m.group(2)) >= 900 > int(m.group(1)) and int(m.group(3)) < 100
            and abs(int(m.group(3)) - round(100 * int(m.group(1)) / int(m.group(2)))) <= 1):
        fails.append("a page wider than the pane was not fitted to it: %r" % said.get("wide"))

    if not re.search(r"fitted at 100%", said.get("picture", "")):
        fails.append("a picture wider than its column shrank the page: %r" % said.get("picture"))

    if "ana@example.com" not in said.get("suggests", ""):
        fails.append("three letters did not suggest Ana: %r" % said.get("suggests"))
    if said.get("draft_flags") != ["\\Draft", "\\Seen"]:
        fails.append("the draft on the server and its flags: %r" % said.get("draft_flags"))
    order = said.get("close_order", (-1, -1))
    if not (0 <= order[0] < order[1]):
        fails.append("the composer's window did not go before its draft was written: %r" % (order,))
    if not said.get("kept", "").endswith("kept as a draft"):
        fails.append("closing the composer did not keep the draft: %r" % said.get("kept"))
    if "550" not in said.get("refused", "") and "refused" not in said.get("refused", ""):
        fails.append("the server's refusal not said: %r" % said.get("refused"))
    if ".eml" not in said.get("outbox", ""):
        fails.append("the refused message was not kept in the Outbox: %r" % said.get("outbox"))
    if mailpeer.PASSWORD in seen:
        fails.append("the password was printed")
    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 24

    if fails:
        print("FAIL: %d of %d checks on Mail's composer:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on Mail's composer (Reply All to everyone but the account, quoted "
          "and threaded, kept in Sent and the message flagged Answered; a new message with an "
          "address completed from three letters and a Bcc in no header; a draft closed by its "
          "red light, its window gone before it was written, kept on the server, opened from Drafts, sent whole and its copy taken away; "
          "a recipient refused, said, and the message kept in the Outbox; a newsletter "
          "wider than the pane fitted to it, and one big picture not)." % checks)
    return 0


# A message carrying a text file, which opens in the Text Editor.
NOTES = ("From: Tom <tomas@example.org>\r\n"
         "To: lena@example.com\r\n"
         "Subject: The notes\r\n"
         "Date: Fri, 9 Oct 2026 08:00:00 +0000\r\n"
         "Message-ID: <notes@example.org>\r\n"
         "MIME-Version: 1.0\r\n"
         "Content-Type: multipart/mixed; boundary=n\r\n"
         "\r\n"
         "--n\r\nContent-Type: text/plain\r\n\r\nThe notes, attached.\r\n"
         "--n\r\nContent-Type: text/plain; name=\"notes.txt\"\r\n"
         "Content-Disposition: attachment; filename=\"notes.txt\"\r\n\r\n"
         "Bring the map.\r\n--n--\r\n").encode()


def part5(image):
    """Attachments (M7): a message's file saved through the Save window,
    whole; a text file opened in the Text Editor; a file attached with the
    paperclip and the Open window and sent, arriving byte for byte; and
    Forward carrying the message's own file."""
    import email
    import re
    import time
    from email import policy

    said, error, w = {}, None, None

    try:
        w = mail_window(image, [NOTES], 4, said)
        guest, session = w.guest, w.session
        at, click, typed, key, composer = w.at, w.click, w.typed, w.key, w.composer
        places, lx, ly, rh = w.places, w.lx, w.ly, w.rh

        def menu_item(n, mark):
            line = guest.wait_for_line("mail: attachment menu at ", "the attachment's menu", mark)
            m = re.match(r"(\d+),(\d+), (\d+) wide, rows of (\d+)", line)
            mx, my, _, row = (int(v) for v in m.groups())
            time.sleep(0.6)
            click(mx + 24, my + 2 + (n - 1) * row + row // 2, ox=0, oy=0)

        # 1. The photos, second: its file saved into Downloads, whole.
        mark = len(guest.seen)
        click(lx + 100, ly + rh + rh // 2)
        guest.wait_for_line("mail: showing ", "the photos shown", mark)
        chip = guest.wait_for_line("mail: attachments ", "its file's chip", mark)
        time.sleep(1)
        click(*at("at", chip))
        menu_item(2, mark)                       # Save...
        guest.wait_for_line("mail: save panel at ", "the Save window", mark)
        time.sleep(1.5)
        key("ret")                               # its own name, in Downloads
        said["saved"] = guest.wait_for_line("mail: saved photos.bin to ", "the file saved", mark)
        said["downloads"] = session.run("ls /Home/Downloads").decode(errors="replace")

        # 2. The notes, first: its text file opened in the Text Editor.
        mark = len(guest.seen)
        click(lx + 100, ly + rh // 2)
        guest.wait_for_line("mail: showing ", "the notes shown", mark)
        chip = guest.wait_for_line("mail: attachments ", "its file's chip", mark)
        time.sleep(1)
        click(*at("at", chip))
        menu_item(1, mark)                       # Open
        said["opened"] = guest.wait_for_line("mail: opening notes.txt with ", "the notes opened", mark)
        said["editor"] = guest.wait_for_line("wm: window ", "a window for the notes", mark)
        time.sleep(2)

        # 3. A new message with a file from Documents, by the paperclip.
        session.run("cp /Home/Downloads/photos.bin /Home/Documents/photos.bin")
        mark = len(guest.seen)
        click(*[v + 13 for v in at("compose", places)])
        cid, cx, cy, where = composer("New Message", mark)
        typed("ana@example.com,")
        key("tab"); key("tab"); key("tab")
        typed("With the photos")
        key("tab")
        typed("Here.")
        mark = len(guest.seen)
        click(*at("attach", where), ox=cx, oy=cy)
        guest.wait_for_line("mail: composer %s attach panel at " % cid, "the Open window", mark)
        time.sleep(1.5)
        key("ret")                               # the one file there
        said["attached"] = guest.wait_for_line("mail: composer %s attached " % cid, "the file attached", mark)
        time.sleep(1.5)
        import kosmos_vnc as V
        w_, h_, rgb_ = w.R.parse_ppm(guest.screendump())
        rows_ = [rgb_[((y * w_) + cx) * 3:((y * w_) + cx + 680) * 3] for y in range(cy, min(h_, cy + 560))]
        V.png(os.path.join(ROOT, "build", "mail", "composer-files.png"), 680, len(rows_), b"".join(rows_))
        mark = len(guest.seen)
        key("ctrl-ret")
        guest.wait_for("maild: sent %s" % cid, "the photos sent")

        # 4. Forward the photos: its file goes with it.
        mark = len(guest.seen)
        click(lx + 100, ly + rh + rh // 2)
        guest.wait_for_line("mail: showing ", "the photos again", mark)
        mark = len(guest.seen)
        click(*[v + 13 for v in at("forward", places)])
        fid, _, _, _ = composer("Forward", mark)
        said["carried"] = guest.wait_for_line("mail: composer %s attached " % fid, "its file carried", mark)
        typed("bob@example.net,")
        mark = len(guest.seen)
        key("ctrl-ret")
        guest.wait_for("maild: sent %s" % fid, "the forward sent")
        time.sleep(1)
    except Exception as e:                  # noqa: BLE001 - said below
        error = "%s: %s" % (type(e).__name__, str(e).splitlines()[0])
    finally:
        seen = window_closed(w, "guest-5.log")

    if w is None:
        print("FAIL: Mail's attachments: the machine never came up: %s" % error)
        return 1

    fails = []

    if error:
        fails.append("the machine stopped: " + error + " ... " + seen[-1200:])

    if not said.get("saved", "").endswith("/Home/Downloads/photos.bin: %d bytes" % len(mailpeer.BIG)):
        fails.append("the photos' file not saved whole into Downloads: %r" % said.get("saved"))
    if "photos.bin" not in said.get("downloads", ""):
        fails.append("Downloads did not hold the file: %r" % said.get("downloads"))
    if not said.get("opened", "").startswith("texteditor"):
        fails.append("the notes did not open in the Text Editor: %r" % said.get("opened"))
    if not said.get("attached", "").startswith("photos.bin, %d bytes" % len(mailpeer.BIG)):
        fails.append("the paperclip did not attach the file: %r" % said.get("attached"))
    if not said.get("carried", "").startswith("photos.bin, %d bytes" % len(mailpeer.BIG)):
        fails.append("Forward did not carry the message's file: %r" % said.get("carried"))

    def carried(subject):
        with w.peer.lock:
            got = [m for m in w.peer.sent if ("Subject: " + subject + "\r\n") in m["data"]]
        if not got:
            return None, None
        msg = email.message_from_string(got[0]["data"], policy=policy.default)
        files = [(p.get_filename(), p.get_content()) for p in msg.iter_attachments()]
        return msg, files

    msg, files = carried("With the photos")
    if not msg:
        fails.append("the message with the photos did not reach the server")
    elif files != [("photos.bin", mailpeer.BIG)]:
        fails.append("the photos did not arrive byte for byte: %r" % [(n, len(b)) for n, b in (files or [])])
    elif "Here." not in msg.get_body(("plain",)).get_content():
        fails.append("the text beside the file was lost")

    msg, files = carried("Fwd: The photos")
    if not msg or files != [("photos.bin", mailpeer.BIG)]:
        fails.append("the forward did not carry the photos whole: %r"
                     % [(n, len(b)) for n, b in (files or [])])

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    checks = 9

    if fails:
        print("FAIL: %d of %d checks on Mail's attachments:" % (len(fails), checks))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d checks on Mail's attachments (a message's file saved through the Save "
          "window into Downloads, whole; a text file opened in the Text Editor; a file attached "
          "with the paperclip and the Open window, sent and arriving byte for byte beside its "
          "text; Forward carrying the message's own file)." % checks)
    return 0


if __name__ == "__main__":
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    part = sys.argv[sys.argv.index("--part") + 1] if "--part" in sys.argv else "1"
    sys.exit(part5(image) if part == "5" else part4(image) if part == "4" else part3(image) if part == "3"
             else part2(image) if part == "2" else part1(image))
