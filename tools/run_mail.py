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


def part2(image):
    work, peer, imap_port, smtp_port = setup()
    scripts = {"setup.lua": fill(SETUP, imap_port, smtp_port),
               "look.lua": fill(LOOK, imap_port, smtp_port), "sync.lua": SYNC}
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

    checks = 12

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
        guest.wait_for("maild: %s: 3 messages kept" % who, "the Inbox kept")
        said["account"] = session.run("cat /Home/Mail/%s/account" % who).decode(errors="replace")
        said["boxes"] = guest.wait_for_line("mail: mailboxes ", "the mailboxes listed", 0)

        # The list, newest first: the photos, the route in HTML, the café.
        places = guest.wait_for_line("mail: places ", "the window's places", 0)
        lx, ly = at("list", places)
        rh = int(re.search(r"rows (\d+)", places).group(1))
        time.sleep(2)

        mark = len(guest.seen)
        click(lx + 100, ly + rh // 2)
        said["photos"] = guest.wait_for_line("mail: showing ", "the newest shown", mark)

        mark = len(guest.seen)
        click(lx + 100, ly + rh + rh // 2)
        said["route"] = guest.wait_for_line("mail: showing ", "the second shown", mark)

        mark = len(guest.seen)
        click(lx + 100, ly + 2 * rh + rh // 2)
        said["cafe"] = guest.wait_for_line("mail: showing ", "the oldest shown", mark)
        said["set"] = guest.wait_for_line("mail: set ", "its text set", mark)
        time.sleep(2)

        # The window as it looked, kept for whoever reads the run after.
        import kosmos_vnc as V
        w_, h_, rgb_ = R.parse_ppm(guest.screendump())
        ww, wh = (int(v) for v in re.search(r"(\d+)x(\d+)", placed).groups())
        rows_ = [rgb_[((y * w_) + wx) * 3:((y * w_) + wx + ww) * 3]
                 for y in range(wy, min(h_, wy + wh))]
        os.makedirs(os.path.join(ROOT, "build", "mail"), exist_ok=True)
        V.png(os.path.join(ROOT, "build", "mail", "mail.png"), ww, len(rows_), b"".join(rows_))
        said["paper"] = sum(1 for i in range(0, len(b"".join(rows_)), 3 * 7)
                            if b"".join(rows_)[i:i + 3] == b"\xf7\xf7\xf5")

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

    fails = []

    if error:
        fails.append("the machine stopped: " + error + " ... " + seen[-800:])

    if not said.get("signed", "").startswith(who):
        fails.append("the account was not signed in through Add Account: %r" % said.get("signed"))

    acc = said.get("account", "")
    if "10.0.2.2" not in acc or mailpeer.PASSWORD in acc:
        fails.append("the account file, its server and no password: %r" % acc[:300])

    if said.get("boxes") != "Inbox, Drafts, Sent, Archive, Trash, Café, Projects, -Kosmos":
        fails.append("the mailboxes as a person reads them, [Gmail] left out: %r"
                     % said.get("boxes"))

    if mailpeer.PASSWORD in seen:
        fails.append("the password was printed")

    if not said.get("photos", "").startswith("3, The photos"):
        fails.append("the newest message first: %r" % said.get("photos"))
    if not said.get("route", "").startswith("2, This week's route"):
        fails.append("the second, in HTML: %r" % said.get("route"))
    if not said.get("cafe", "").startswith("1, Café on Saturday, from Tomás Ferreira"):
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

    checks = 14

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


if __name__ == "__main__":
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    part = sys.argv[sys.argv.index("--part") + 1] if "--part" in sys.argv else "1"
    sys.exit(part3(image) if part == "3" else part2(image) if part == "2" else part1(image))
