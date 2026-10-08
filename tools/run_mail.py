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


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    work = scratch.directory("mail")
    run_tls.pki(work)

    peer = mailpeer.Peer(work, "good.pem", "server.key")
    imap_port, smtp_port = peer.start()

    script = (CHECK.replace("IMAP", str(imap_port)).replace("SMTP", str(smtp_port))
              .replace("USER", mailpeer.USER).replace("PASSWORD", mailpeer.PASSWORD))

    with open(os.path.join(work, "mailcheck.lua"), "w") as f:
        f.write(script)

    disk = os.path.join(work, "disk.img")
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32",
                    os.path.join(work, "ca.der") + ":/Home/ca.der",
                    os.path.join(work, "mailcheck.lua") + ":/Home/mailcheck.lua"],
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

    lines = {}

    for line in said.splitlines():
        if line.startswith("M "):
            name, _, rest = line[2:].partition(" ")
            lines[name] = rest

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

    expect("boxes", "Archive=archive,Café=nil,Drafts=drafts,INBOX=inbox,Sent=sent,Trash=trash",
           "the mailboxes by their use")
    expect("status", "3 3", "the Inbox's messages and unseen")
    expect("select", "3 1000 true", "INBOX selected, with its UIDVALIDITY and a MODSEQ")
    expect("new", "1,2,3 3", "what is new in a mailbox seen for the first time")

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

    checks = 20

    if fails:
        print("FAIL: %d of %d checks on mail's conversations:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on mail's conversations with a server on this Mac "
          "(signed in over TLS and a wrong password refused; mailboxes by their use, "
          "Café's name; what is new; a message fetched and read by the Mail Kit, and "
          "420 KB byte for byte; a flag, a move and an append reaching the server; "
          "what changed by CONDSTORE; IDLE told of new mail; a message sent with "
          "STARTTLS to two people, a dot kept, and a refused recipient said)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
