#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""smbfs connects: a Kosmos machine signs into Samba on this Mac.

`docs/sharing.md` step N2. The far end is `tools/smbpeer.py` - Homebrew's
Samba run as the user, on a port of this suite's own, reached from the guest
as 10.0.2.2 through QEMU's user network - and the near end is the `share`
program at the prompt, asking smbfs through the namespace (`fs.share_*`).
What is checked, each as a person at the prompt would see it:

  - `share status` before anything is asked says nothing has been;
  - `share connect smb://10.0.2.2:PORT/Projects ACCOUNT`, with the peer's
    password typed, answers with the dialect and the server's own name -
    3.1.1, signed (the peer requires signing, so SMB 3's keys, the SHA-512
    preauthentication hash and AES-CMAC all have to be right) - and the
    share and account;
  - `share status` shows it connected;
  - `share probe` of the peer: answered, SMB 3.1.1, signing required;
  - a wrong password is refused, in words that say so;
  - an address with nobody on it (10.0.2.2:4451) is "nothing ... took the
    connection", and inside smbfs's bound;
  - the peer sealing (SMB 3's encryption, required): connected, and sealed -
    the AES-128-CCM that is now BearSSL's under libsmb2.

**The controls the design names for N2**:

  - **a peer of SMB 1 alone** (Samba's `NT1`): refused, as a server that
    "does not speak SMB 2 or 3" - and never connected;
  - **a peer stopped mid-negotiation** (SIGSTOP: the Mac's kernel takes the
    connection, nobody answers it): `share connect --no-wait` begins, and
    `share status` answers at once while it is asking - and again once the
    bound has passed, saying it did not answer.

Without Homebrew's Samba this is a skip that says so: the peer is a tool of
this Mac's, as `tools/test_smbpeer.py` says.

**`--part 2`, a share is a folder** (`docs/sharing.md` step N3), on a peer
and a machine of its own beside the first half: once connected, `ls
/Network` names the server and `ls /Network/MACPEER` its share; the 2,000
names of `many/` are the peer's, each with its facts, and what listing them
cost is measured; `big.bin`, 64 MB, read whole into one region, has the
SHA-256 this Mac computes of it, and a hundred pieces read at offsets drawn
from a seed both sides know are this Mac's bytes at those offsets; `cat`
reads `hello.txt` and `cp` copies a file from two folders down into
`/Temporary`; the dates are the peer's; a write, a new folder and a delete
are refused in words; and Tracker opens the share as a folder. **The
controls**: the peer stopped, every process of it, with the connection
made - a folder already listed is answered from memory and says it is as
last heard, a folder never listed is "not answering", each within the
bound and neither hanging - and after it continues the folder is fresh
again.

Usage: run_share.py IMAGE [--part 1|2]
"""

import hashlib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

PEER = os.path.join(HERE, "smbpeer.py")
NOBODY = 4451                   # a port nothing on this Mac listens on
BOUND = 10                      # smbfs's ANSWER_SECONDS


def peer(instance, port, *args):
    return subprocess.run([sys.executable, PEER, *args, "--instance", instance,
                           "--port", str(port)],
                          capture_output=True, text=True, check=True).stdout.strip()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
        else "build/kosmos.elf"
    part = sys.argv[sys.argv.index("--part") + 1] if "--part" in sys.argv else "1"

    if not os.path.exists("/opt/homebrew/opt/samba/sbin/samba-dot-org-smbd"):
        print("SKIP: no Homebrew Samba on this Mac, so no peer to sign into "
              "(brew install samba; docs/sharing.md N0)")
        return 0

    return folders(image) if part == "2" else connects(image)


def connects(image):
    import run_screenshot as R

    x86 = R.machine(image) == "x86_64"
    instance, port = ("x86-share", 4471) if x86 else ("arm-share", 4461)

    peer(instance, port, "start", "--dialect", "3.1.1", "--sign")
    _, _, share, account, password = peer(instance, port, "where").split()
    at = "10.0.2.2:%d" % port

    board = "X86_ARGS" if x86 else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        guest = R.Guest(image, 90)
    finally:
        setattr(R, board, saved)

    said = {}
    took = {}
    fails = []

    def run(key, command, secret=None, ends=") ended, code"):
        """One command typed; what it printed up to the shell's line saying
        it ended, and how long that took."""
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type(command)

        if secret is not None:
            guest.wait_for_line("password for", "asked for the password: " + command,
                                since=mark)
            guest.type(secret)

        guest.wait_for_line(ends, "answered " + command, since=mark)
        took[key] = time.monotonic() - started
        said[key] = guest.seen[mark:].replace("\r", "")
        return said[key]

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")

        run("nothing", "share status")
        run("probe", "share probe " + at)
        run("connect", "share connect smb://%s/%s %s" % (at, share, account), password)
        run("status", "share status")
        run("let go", "share disconnect " + at)
        run("wrong", "share connect smb://%s/%s %s" % (at, share, account),
            password + "-not")
        run("nobody", "share probe 10.0.2.2:%d" % NOBODY)

        # The control: stopped mid-negotiation. The connection is taken by
        # this Mac's kernel and answered by nobody; `status` is asked at
        # once and must answer at once, while smbfs is still asking.
        peer(instance, port, "pause")
        run("stopped", "share connect --no-wait smb://%s/%s %s" % (at, share, account),
            password)
        run("asking", "share status")
        time.sleep(BOUND + 2)
        run("gave up", "share status")
        peer(instance, port, "resume")

        # Sealed: SMB 3's encryption, required by the peer.
        peer(instance, port, "start", "--dialect", "3.1.1", "--seal")
        run("sealed", "share connect smb://%s/%s %s" % (at, share, account), password)
        run("let go sealed", "share disconnect " + at)

        # The control: SMB 1 alone.
        peer(instance, port, "start", "--dialect", "1.0")
        run("smb1", "share probe " + at)
        run("smb1 connect", "share connect smb://%s/%s %s" % (at, share, account),
            password)
        run("last", "share status")
    except Exception as e:                  # noqa: BLE001 - said below
        said["error"] = "%s: %s" % (type(e).__name__, str(e)[:1500])
    finally:
        guest.close()

        try:
            peer(instance, port, "stop")
        except subprocess.CalledProcessError:
            pass

    transcript = guest.seen.replace("\r", "")

    def check(ok, what):
        if not ok:
            fails.append(what)

    def text(key):
        return said.get(key, "")

    if "error" in said:
        fails.append("the machine stopped: " + said["error"])

    check("nothing has been asked of any server" in text("nothing"),
          "status before anything was asked: %r" % text("nothing")[-300:])

    check(("share: %s answered - MACPEER, SMB 3.1.1, signed; %s connected as %s"
           % (at, share, account)) in text("connect"),
          "connect did not answer with the dialect and the server's name: %r"
          % text("connect")[-400:])

    check(("%s  connected  MACPEER  SMB 3.1.1, signed  %s as %s" % (at, share, account))
          in text("status"),
          "status did not show it connected: %r" % text("status")[-400:])

    check(("share: %s answered - SMB 3.1.1, signing required" % at) in text("probe"),
          "a probe of the peer did not answer: %r" % text("probe")[-300:])

    check(("share: %s let go" % at) in text("let go"),
          "disconnect: %r" % text("let go")[-300:])

    check(("refused the account %s: the name or the password is wrong" % account)
          in text("wrong") and "connected as" not in text("wrong"),
          "a wrong password was not refused in words: %r" % text("wrong")[-300:])

    check("nothing at 10.0.2.2:%d took the connection" % NOBODY in text("nobody")
          and took.get("nobody", 99) < BOUND + 5,
          "an address with nobody on it was not 'not answering' within its bound "
          "(%.1f s): %r" % (took.get("nobody", -1), text("nobody")[-300:]))

    check(("share: asking %s" % at) in text("stopped"),
          "connect --no-wait did not come back at once: %r" % text("stopped")[-300:])

    check(("%s  asking" % at) in text("asking") and took.get("asking", 99) < 5,
          "status did not answer at once while a stopped peer was asked "
          "(%.1f s): %r" % (took.get("asking", -1), text("asking")[-300:]))

    check(("%s  away  %s took the connection and did not answer within %d seconds"
           % (at, at, BOUND)) in text("gave up"),
          "a stopped peer was not given up on within the bound: %r"
          % text("gave up")[-300:])

    check(("share: %s answered - MACPEER, SMB 3.1.1, sealed; %s connected as %s"
           % (at, share, account)) in text("sealed")
          or ("share: %s answered - MACPEER, SMB 3.1.1, signed, sealed; %s connected as %s"
              % (at, share, account)) in text("sealed"),
          "a sealing peer was not connected, sealed: %r" % text("sealed")[-400:])

    refusal = "took the connection and hung up without answering: it does not speak SMB 2 or 3"
    check(refusal in text("smb1") and "answered - SMB" not in text("smb1"),
          "a peer of SMB 1 alone was not refused: %r" % text("smb1")[-300:])
    check(refusal in text("smb1 connect") and "connected as" not in text("smb1 connect"),
          "a connect to a peer of SMB 1 alone was not refused: %r"
          % text("smb1 connect")[-300:])

    check(" died" not in transcript and "smbfs exited" not in transcript,
          "something died: %r" % transcript[transcript.find(" died") - 200:][:400])

    checks = 14

    # The serial line kept, pass or fail: what smbfs said of each server is
    # in it, a line each.
    log = os.path.join(ROOT, "build", "%s-serial.log" % instance)

    with open(log, "w") as f:
        f.write(transcript)

    if fails:
        print("FAIL: %d of %d checks on smbfs connecting (the serial line: %s):"
              % (len(fails), checks, log))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on smbfs connecting to Samba on this Mac (3.1.1 "
          "signed, with the server's name; status; a probe; a wrong password "
          "refused in words; nobody at %d in %.1f s; sealed; controls: SMB 1 "
          "alone refused, and a stopped peer given up on after %d s while "
          "status answered in %.1f s)."
          % (checks, NOBODY, took.get("nobody", -1), BOUND, took.get("asking", -1)))
    return 0




#--------------------------------------------------------------------------
# The second half: a share is a folder (N3).
#--------------------------------------------------------------------------

SHARE_DIR = os.path.join(ROOT, "build", "smbpeer", "Projects")
BIG = 64 * 1024 * 1024
PIECES = 100
PIECE_MOST = 3000
MASK = (1 << 64) - 1

#
# What the machine runs once connected, written into `/Temporary` and run
# as a program: the 2,000 names and what they cost, the big file whole and
# in pieces, the dates, a write refused, and the root as a window for
# people shows it. Each answer on a line of its own, its tag put together
# from two halves so the echo of this text as it is typed never matches.
#
PROBE = r'''
local crypto = use("/Kosmos/Kits/crypto")
local regions = use("/Kosmos/Libraries/regions.lua")
local placelib = use("/Kosmos/Libraries/places.lua")
local hz = fs.read("/Devices/cpu").counter_hz
local S = "/Network/MACPEER/Projects"
local function since(t) return (sys.ticks() - t) * 1000 // hz end
local function dev() return fs.read(S .. "/.device") or {} end
local d0 = dev()
local t = sys.ticks()
local names, why = fs.list(S .. "/many")
if not names then print("N3" .. "NAMES none " .. tostring(why)) return end
local list_ms = since(t)
t = sys.ticks()
local facts = 0
for _, n in ipairs(names) do
  local a = fs.getattr(S .. "/many/" .. n)
  if a and a.kind == "file" and (a.size or 0) > 0 and a.modified then facts = facts + 1 end
end
local attr_ms = since(t)
local d1 = dev()
table.sort(names)
print(("N3" .. "NAMES %d %s %d"):format(#names, crypto.sha256(table.concat(names, "\n")), facts))
print(("N3" .. "LIST %d %d %d %d %d %d %d"):format(list_ms, attr_ms,
  d1.listings - d0.listings, d1.listing_requests - d0.listing_requests,
  (d1.listing_counter_ticks - d0.listing_counter_ticks) * 1000 // hz,
  d1.cache_hits - d0.cache_hits, d1.requests - d0.requests))
local SIZE = @BIG@
local WHOLE = @WHOLE@
local r, oops = regions.make(WHOLE)
if not r then print("N3" .. "BIG none " .. tostring(oops)) return end
t = sys.ticks()
local got, size = fs.read_into(S .. "/big.bin", r.cap, 0, WHOLE)
local read_ms = since(t)
local d2 = dev()
t = sys.ticks()
local sum = got and crypto.sha256(r.at, got) or tostring(size)
print(("N3" .. "BIG %s %s %s %d %d %d %d"):format(tostring(got), tostring(got and size),
  sum, read_ms, since(t), d2.reads - d1.reads, d2.read_bytes - d1.read_bytes))
regions.free(r)
local x = @SEED@
local function nxt() x = x ~ (x << 13) x = x ~ (x >> 7) x = x ~ (x << 17) return x end
local scratch = regions.unmapped(4096)
local acc = regions.make(@N@ * 4096)
local at, bad = 0, "none"
t = sys.ticks()
for i = 1, @N@ do
  local off = (nxt() >> 1) % (SIZE - 4096)
  local len = 1 + (nxt() >> 1) % @MOST@
  local n, e = fs.read_into(S .. "/big.bin", scratch.cap, off, len)
  if n ~= len and bad == "none" then bad = i .. ":" .. tostring(n) .. ":" .. tostring(e) end
  if n then sys.region_copy(acc.cap, at, scratch.cap, 0, n) at = at + n end
end
print(("N3" .. "PIECES %d %s %s %d"):format(at, crypto.sha256(acc.at, at), bad, since(t)))
regions.free(scratch, acc)
local h = fs.getattr(S .. "/hello.txt") or {}
local b = fs.getattr(S .. "/big.bin") or {}
local i = fs.getattr(S .. "/inside") or {}
print(("N3" .. "DATES %s %s %s %s %s"):format(tostring(h.modified), tostring(b.modified),
  tostring(h.size), tostring(b.size), tostring(i.kind)))
local ok, e = fs.write(S .. "/new.txt", "x")
print("N3" .. "WRITE " .. tostring(ok) .. " " .. tostring(e))
local root = {}
for _, n in ipairs(fs.list("/") or {}) do root[#root + 1] = { name = n } end
local shown = {}
for _, n in ipairs(placelib.files_only("/", root)) do shown[#shown + 1] = n.name end
print("N3" .. "ROOT " .. table.concat(shown, ","))
'''


def pieces_expected(seed, data):
    """The bytes the machine's hundred pieces should be, in order: the same
    xorshift as the probe's, on 64-bit words, its shifts logical."""
    x = seed
    out = bytearray()

    def nxt():
        nonlocal x
        x ^= (x << 13) & MASK
        x ^= x >> 7
        x ^= (x << 17) & MASK
        return x

    for _ in range(PIECES):
        off = (nxt() >> 1) % (BIG - 4096)
        n = 1 + (nxt() >> 1) % PIECE_MOST
        out += data[off:off + n]

    return bytes(out)


def folders(image):
    import run_screenshot as R

    x86 = R.machine(image) == "x86_64"
    instance, port = ("x86-share-2", 4472) if x86 else ("arm-share-2", 4462)

    # 3.1.1, the peer asking for no signature: a 64 MB file through CMAC
    # under emulation would measure signing, which is step N4's.
    peer(instance, port, "start", "--dialect", "3.1.1")
    _, _, share, account, password = peer(instance, port, "where").split()
    at = "10.0.2.2:%d" % port
    there = "/Network/MACPEER/%s" % share

    # What the Mac says the share holds, before the machine is asked.
    names = sorted(os.listdir(os.path.join(SHARE_DIR, "many")))
    names_sum = hashlib.sha256("\n".join(names).encode()).hexdigest()

    with open(os.path.join(SHARE_DIR, "big.bin"), "rb") as f:
        big = f.read()

    #
    # **The whole file on ARM, its first 4 MB on x86-64.** The x86-64
    # machine's network receives about a tenth of a megabyte a second under
    # QEMU - `fetch` of 2 MB over plain HTTP takes 13 s there and 1.2 s on
    # ARM, so it is the stack's and the emulated card's, not SMB's
    # (`testing.md` 18.408) - and 64 MB would be seven minutes of a
    # ten-minute gate. The same path, into one region, a sixteenth of it.
    #
    whole = (4 << 20) if x86 else BIG
    big_sum = hashlib.sha256(big[:whole]).hexdigest()
    seed = int.from_bytes(os.urandom(8), "little") & ((1 << 62) - 1) | 1
    pieces = pieces_expected(seed, big)
    pieces_sum = hashlib.sha256(pieces).hexdigest()
    hello_at = int(os.stat(os.path.join(SHARE_DIR, "hello.txt")).st_mtime)
    big_at = int(os.stat(os.path.join(SHARE_DIR, "big.bin")).st_mtime)
    hello_size = os.stat(os.path.join(SHARE_DIR, "hello.txt")).st_size

    with open(os.path.join(SHARE_DIR, "hello.txt")) as f:
        hello = f.read().strip()

    with open(os.path.join(SHARE_DIR, "inside", "deeper", "note.txt")) as f:
        note = f.read().strip()

    # The pages the namespace asks for: the names as smbfs packs them, a
    # zero after each, as many as fit in a kilobyte.
    pages, used = 1, 0

    for n in names:
        if used + len(n) + 1 > 1024:
            pages, used = pages + 1, 0
        used += len(n) + 1

    # One line: the prompt reads a line at a time, so the probe crosses as
    # statements side by side - it has no comment that would end early.
    probe = " ".join(line.strip() for line in PROBE.splitlines() if line.strip())
    probe = (probe.replace("@BIG@", str(BIG)).replace("@WHOLE@", str(whole))
             .replace("@SEED@", str(seed))
             .replace("@N@", str(PIECES)).replace("@MOST@", str(PIECE_MOST)))
    top = len(os.listdir(SHARE_DIR))

    board = "X86_ARGS" if x86 else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        guest = R.Guest(image, 150)
    finally:
        setattr(R, board, saved)

    said, took, fails = {}, {}, []

    def run(key, command, secret=None, ends=") ended, code"):
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type(command)

        if secret is not None:
            guest.wait_for_line("password for", "asked for the password: " + command,
                                since=mark)
            guest.type(secret)

        guest.wait_for_line(ends, "answered " + command, since=mark)
        took[key] = time.monotonic() - started
        said[key] = guest.seen[mark:].replace("\r", "")
        return said[key]

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")

        run("nothing", "ls /Network")
        run("connect", "share connect smb://%s/%s %s" % (at, share, account), password)
        run("network", "ls /Network")
        run("server", "ls /Network/MACPEER")
        run("share", "ls " + there)

        # The probe, typed in pieces a line can carry, then put together.
        parts = [probe[i:i + 600] for i in range(0, len(probe), 600)]

        for i, piece in enumerate(parts):
            guest.type('fs.write("/Temporary/n3_%d.lua", [==[%s]==])' % (i, piece))
            guest.wait_for(R.PROMPT, "the probe written")

        run("written", 'fs.write("/Temporary/n3.lua", '
            + " .. ".join('fs.read("/Temporary/n3_%d.lua")' % i for i in range(len(parts)))
            + ') print("probe " .. "written")', ends="probe written")
        run("probe", "/Temporary/n3.lua")

        run("cat", "cat %s/hello.txt" % there)
        run("cp", "cp %s/inside/deeper/note.txt /Temporary/note.txt" % there)
        run("cat copy", "cat /Temporary/note.txt")
        run("mkdir", "mkdir %s/newdir" % there)
        run("rm", "rm %s/hello.txt" % there)

        # The control: the peer stopped, every process of it, with the
        # connection made. A folder heard more than a moment ago is asked
        # again and answered from memory, marked; one never listed is away.
        time.sleep(2.5)
        peer(instance, port, "pause")
        run("stale", "ls " + there)
        run("never", "ls %s/inside" % there)
        peer(instance, port, "resume")
        time.sleep(3)
        run("back", "ls " + there)

        # Tracker, on the share: listed and opened as a folder. Last, since
        # the window manager keeps the console once it is up.
        mark = len(guest.seen)
        guest.type("wm tracker:" + there)
        said["tracker"] = guest.wait_for_line("tracker: showing " + there,
                                              "Tracker to open the share", mark)
    except Exception as e:                  # noqa: BLE001 - said below
        said["error"] = "%s: %s" % (type(e).__name__, str(e)[:1500])
    finally:
        guest.close()

        try:
            peer(instance, port, "stop")
        except subprocess.CalledProcessError:
            pass

    transcript = guest.seen.replace("\r", "")

    def check(ok, what):
        if not ok:
            fails.append(what)

    def text(key):
        return said.get(key, "")

    def tagged(tag):
        m = re.search(r"^N3%s (.*)$" % tag, text("probe"), re.M)
        return m.group(1).split() if m else []

    if "error" in said:
        fails.append("the machine stopped: " + said["error"])

    check("(empty)" in text("nothing"),
          "/Network before a share was connected is not an empty folder: %r"
          % text("nothing")[-300:])
    check("connected as %s" % account in text("connect"),
          "the share did not connect: %r" % text("connect")[-300:])
    check(re.search(r"^\s+MACPEER\s+--\s+folder", text("network"), re.M) is not None,
          "ls /Network does not name the server as a folder: %r" % text("network")[-300:])
    check(re.search(r"^\s+%s\s+--\s+folder" % share, text("server"), re.M) is not None,
          "ls /Network/MACPEER does not name its share: %r" % text("server")[-300:])

    listing = text("share")
    check(all(re.search(r"^\s+%s\s" % re.escape(n), listing, re.M)
              for n in ("big.bin", "hello.txt", "inside", "many"))
          and re.search(r"^\s+big\.bin\s+%d\s" % BIG, listing, re.M) is not None
          and "as last heard" not in listing,
          "the share's own folder is not the peer's: %r" % listing[-500:])

    got = tagged("NAMES")
    check(got[:3] == [str(len(names)), names_sum, str(len(names))],
          "the 2,000 names are not the peer's, each with its facts: %r (want %d %s)"
          % (got, len(names), names_sum))

    cost = tagged("LIST")
    check(len(cost) == 7, "what listing the 2,000 cost was not said: %r" % cost)

    big_got = tagged("BIG")
    check(big_got[:3] == [str(whole), str(BIG), big_sum],
          "big.bin read whole into a region is not the Mac's: %r (want %d %d %s)"
          % (big_got, whole, BIG, big_sum))

    piece_got = tagged("PIECES")
    check(piece_got[:3] == [str(len(pieces)), pieces_sum, "none"],
          "a hundred pieces of big.bin at random offsets are not the Mac's bytes "
          "there: %r (want %d %s, seed %d)" % (piece_got, len(pieces), pieces_sum, seed))

    dates = tagged("DATES")
    check(dates == [str(hello_at), str(big_at), str(hello_size), str(BIG), "directory"],
          "the dates and sizes are not the peer's: %r (want %d %d %d %d directory)"
          % (dates, hello_at, big_at, hello_size, BIG))

    refusal = "MACPEER's %s is open read only" % share
    check(re.search(r"^N3WRITE (nil|false) " + re.escape(refusal), text("probe"), re.M) is not None,
          "a write to the share was not refused in words: %r"
          % (re.search(r"^N3WRITE.*$", text("probe"), re.M) or [""])[0])
    check(refusal in text("mkdir"),
          "a new folder in the share was not refused in words: %r" % text("mkdir")[-300:])
    check(refusal in text("rm"),
          "a delete in the share was not refused in words: %r" % text("rm")[-300:])

    check("Network" in tagged("ROOT")[0].split(",") if tagged("ROOT") else False,
          "the root as Tracker shows it leaves /Network out: %r" % tagged("ROOT"))

    check(hello in text("cat"), "cat of hello.txt: %r" % text("cat")[-300:])
    check("copied to /Temporary/note.txt" in text("cp") and note in text("cat copy"),
          "cp from the share into /Temporary: %r %r"
          % (text("cp")[-200:], text("cat copy")[-200:]))

    # The controls.
    stale = text("stale")
    check("as last heard" in stale and "hello.txt" in stale
          and took.get("stale", 99) < 5,
          "a folder already listed, its server stopped, was not answered from "
          "memory and marked (%.1f s): %r" % (took.get("stale", -1), stale[-400:]))
    # Five seconds of silence for a folder never listed (`SILENT_UNKNOWN_MS`),
    # since there is nothing in memory to answer with: said within ten, and
    # not before four - a quarter-second answer would be the old bound that
    # called a busy server missing.
    check("MACPEER is not answering" in text("never")
          and 4 <= took.get("never", 99) < 10,
          "a folder never listed, its server stopped, was not 'not answering' "
          "after its five-second bound (%.1f s): %r" % (took.get("never", -1), text("never")[-300:]))
    check("hello.txt" in text("back") and "as last heard" not in text("back"),
          "the server continued, and the folder was not fresh again: %r"
          % text("back")[-300:])

    check(said.get("tracker", "").startswith(", %d items" % top),
          "Tracker did not open the share as a folder of %d: %r"
          % (top, said.get("tracker", "")[-200:]))

    check(" died" not in transcript and "smbfs exited" not in transcript,
          "something died: %r" % transcript[transcript.find(" died") - 200:][:400])

    checks = 19
    log = os.path.join(ROOT, "build", "%s-serial.log" % instance)

    with open(log, "w") as f:
        f.write(transcript)

    if fails:
        print("FAIL: %d of %d checks on a share as a folder (the serial line: %s):"
              % (len(fails), checks, log))

        for f in fails:
            print("  " + f)

        return 1

    list_ms, attr_ms, listings, requests, smb_ms, hits, sent = (int(v) for v in cost)
    big_ms, sum_ms, reads, read_bytes = (int(v) for v in big_got[3:7])

    print("PASS: %d checks on a share as a folder (ls /Network and its server; "
          "the %d names of many/ the peer's; big.bin's SHA-256 read into a region and "
          "%d pieces at random offsets; cat, cp, dates; writes refused in "
          "words; Tracker opens it; controls: a stopped peer answered from "
          "memory as last heard in %.1f s and 'not answering' in %.1f s)."
          % (checks, len(names), PIECES, took.get("stale", -1), took.get("never", -1)))
    print("  the %d names: listed in %d ms - %d listing, %d SMB requests, %d ms "
          "of them in smbfs - and %d namespace pages; %d getattrs in %d ms, "
          "%d from smbfs's memory" % (len(names), list_ms, listings, requests,
                                      smb_ms, pages, len(names), attr_ms, hits))
    print("  big.bin: %d MB in %d ms (%.2f MB/s), %d READs; its SHA-256 in %d ms"
          % (whole >> 20, big_ms, (whole / 1048576) / max(big_ms / 1000, 0.001),
             reads, sum_ms))
    return 0


if __name__ == "__main__":
    sys.exit(main())
