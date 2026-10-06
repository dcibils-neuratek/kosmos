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

**Part 1 is N2 and N4 in one machine** (`testing.md` 18.410): the first
half's checks are asked of the N4 matrix's own peers - its 3.1.1 signed and
3.1.1 sealed ones, and a tenth, the SMB 1 peer - so one machine boots and
ten peers start, once; and the stopped peer's ten seconds pass while the
matrix is read rather than in a sleep.

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

**And gone away, and back** (`docs/sharing.md` step N5), on the same peer:
stopped (SIGSTOP) with the folder open, a read in progress ends in words
once the server has said nothing for ten seconds, a folder is answered from
memory marked as last heard, a read meanwhile is refused at once, and
`share status` says it is away and when it next tries; continued, smbfs
signs in again by itself from the NT hash it kept, and the next read is the
Mac's bytes. Stopped outright - every process of it, so the connection is
closed - it is away at once, its tries come at two seconds, then four,
then eight; restarted, `share retry` (Try now) signs in at once, a new
session and a new tree. And `share disconnect` of a server that is away
forgets it, and nothing tries it again.

**Signed and sealed** (step N4, the second half of `--part 1`) (`docs/sharing.md` step N4), on peers of
its own: every dialect pinned in turn - 2.0.2, 2.1, 3.0, 3.0.2, 3.1.1 - with
signing mandatory, and 3.0, 3.0.2 and 3.1.1 with encryption required, and
3.1.1 asking for neither; each connected, a file read and its bytes this
Mac's, and `share status` saying the dialect, signed and sealed - held to
what a relay in between (`tools/smbrelay.py`) saw on the wire, so the words
are checked against the bytes rather than against an expectation. **The
control that bites**: the relay, on cue, changes one byte of a signed READ's
answer (SMB 2.1's HMAC-SHA256 and 3.1.1's AES-CMAC) and of a sealed one (3.1.1,
AES-128-CCM), and the read fails in words, the region it was reading into
holding none of it; the same relay unarmed passed the same read a moment
before. And what signing and sealing cost a megabyte, measured.

**`--measure`**, run alone rather than in the gate: the reads at the sizes
the tables in `testing.md` 18.408 and 18.409 were measured at - 64 MB
whole, and eight megabytes a conversation read twice. In the gate the same
paths read what they need to prove: 16 MB whole - sixteen READs, two in
flight, the pair refilled fourteen times - and a megabyte a conversation,
its bytes held to this Mac's. A measure beside ninety other suites says
nothing (18.409), and its eighty megabytes were the larger part of a minute
and a half (18.410). **Both boards read the same since 6 October**: x86-64's
network had read a seventh of a megabyte a second, its card never
interrupting (`hal/pc/virtio.c`, 18.410), and its suites had read a
sixteenth of ARM's.

Usage: run_share.py IMAGE [--part 1|2] [--measure]
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
    done = subprocess.run([sys.executable, PEER, *args, "--instance", instance,
                           "--port", str(port)], capture_output=True, text=True)

    if done.returncode != 0:
        print("smbpeer %s: %s" % (" ".join(args), (done.stdout + done.stderr).strip()),
              file=sys.stderr)
        raise subprocess.CalledProcessError(done.returncode, done.args,
                                            done.stdout, done.stderr)

    return done.stdout.strip()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
        else "build/kosmos.elf"
    part = sys.argv[sys.argv.index("--part") + 1] if "--part" in sys.argv else "1"

    if not os.path.exists("/opt/homebrew/opt/samba/sbin/samba-dot-org-smbd"):
        print("SKIP: no Homebrew Samba on this Mac, so no peer to sign into "
              "(brew install samba; docs/sharing.md N0)")
        return 0

    measure = "--measure" in sys.argv

    if part == "2":
        return folders(image, measure)

    return connects(image, measure)


def guest_on_network(R, image, timeout):
    """A machine booted with a network card on QEMU's user network."""
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + ["-netdev", "user,id=net0",
                               "-device", R.device(image, "net") + ",netdev=net0"])

    try:
        return R.Guest(image, timeout)
    finally:
        setattr(R, board, saved)


def runner(guest, said, took):
    """`run(key, command)`: one command typed, and what it printed up to the
    kernel's line saying *that* program ended - its own name, so a program
    left running with `&` ending meanwhile is never taken for it."""

    def run(key, command, secret=None, ends=None):
        if ends is None:
            name = os.path.basename(command.split()[0])
            ends = "(%s) ended, code" % (name[:-4] if name.endswith(".lua") else name)

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

    return run


def connects(image, measuring):
    """N2 and N4 in one machine: the first half's checks asked of the
    matrix's own peers, then the matrix, the control that bites and the
    measure - and the stopped peer's bound passing while they run."""
    import concurrent.futures
    import run_screenshot as R
    import smbrelay

    x86 = R.machine(image) == "x86_64"
    board = "x86-share" if x86 else "arm-share"
    base = 4500 if x86 else 4480

    #
    # **How much each read is**: a megabyte a conversation, and the measure
    # the same in the gate, once - its bytes held to this Mac's - and eight
    # megabytes, twice, with `--measure`.
    #
    each = 1 << 20
    changed_bytes = 256 << 10
    measure = (8 << 20) if measuring else each

    names = [m[0] for m in MATRIX]
    peers = {m[0]: (base + i) for i, m in enumerate(MATRIX)}
    relays = {m[0]: (base + 10 + i) for i, m in enumerate(MATRIX)}
    smb1_port = base + len(MATRIX)          # 4489, 4509: between the two

    # Named apart from part 2's peer (`arm-share-2`), which index 2 of the
    # matrix was called once the two parts shared a board's name.
    def instance_of(name):
        return "%s-m%d" % (board, names.index(name)) if name in names \
            else "%s-smb1" % board

    starts = [(m[0], m[1], m[2], m[3]) for m in MATRIX] + [("smb1", "1.0", False, False)]

    def start(m):
        name, dialect, sign, seal = m
        flags = ["--dialect", dialect] + (["--sign"] if sign else []) + \
                (["--seal"] if seal else [])
        peer(instance_of(name), peers.get(name, smb1_port), "start", *flags)

    # Ten peers at once, each its own smbd on its own port.
    with concurrent.futures.ThreadPoolExecutor(len(starts)) as pool:
        list(pool.map(start, starts))

    _, _, share, account, password = peer(instance_of(names[0]), peers[names[0]],
                                          "where").split()
    watch = {name: smbrelay.Relay(relays[name], peers[name], least=4096)
             for name in names}

    with open(os.path.join(SHARE_DIR, "big.bin"), "rb") as f:
        big = f.read()

    with open(os.path.join(SHARE_DIR, "hello.txt"), "rb") as f:
        hello_sum = hashlib.sha256(f.read()).hexdigest()

    def at_port(port):
        return "10.0.2.2:%d" % port

    # The first half's peers: the matrix's 3.1.1 signed and sealed, reached
    # directly rather than through their relays, and the SMB 1 one.
    at = at_port(peers["3.1.1"])
    sealed_at = at_port(peers["3.1.1 sealed"])
    smb1_at = at_port(smb1_port)

    def probe(text, targets, most):
        return (text.replace("@TARGETS@", targets).replace("@MOST@", str(most))
                .replace("@ACCOUNT@", account).replace("@PASSWORD@", password))

    matrix_targets = ",".join("%s=%d=k" % (at_port(relays[n]), each) for n in names)
    # When measuring, twice, the second time in the other order, and the
    # quicker of the two kept: under emulation one read of a few megabytes
    # moves by a few tenths of a second from one run to the next, which is
    # the size of what is measured.
    rounds = MEASURED + MEASURED[::-1] if measuring else MEASURED
    measure_targets = ",".join("%s=%d=d" % (at_port(peers[n]), measure) for n in rounds)

    guest = guest_on_network(R, image, 240)
    said, took, fails = {}, {}, []
    changed = {}
    run = runner(guest, said, took)
    stopped_at = None

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

        # Sealed: SMB 3's encryption, required by the peer.
        run("sealed", "share connect smb://%s/%s %s" % (sealed_at, share, account),
            password)
        run("let go sealed", "share disconnect " + sealed_at)

        # The control: SMB 1 alone.
        run("smb1", "share probe " + smb1_at)
        run("smb1 connect", "share connect smb://%s/%s %s" % (smb1_at, share, account),
            password)

        # The control: stopped mid-negotiation - the SMB 1 peer, which is as
        # silent stopped as any. The connection is taken by this Mac's kernel
        # and answered by nobody; `status` is asked at once and must answer
        # at once, while smbfs is still asking; and its bound passes while
        # the matrix below is read, rather than in a sleep.
        peer(instance_of("smb1"), smb1_port, "pause")
        run("stopped", "share connect --no-wait smb://%s/%s %s"
            % (smb1_at, share, account), password)
        stopped_at = time.monotonic()
        run("asking", "share status")

        written(guest, R, "n4m", probe(SIGNED_PROBE, matrix_targets, each), run)
        written(guest, R, "n4c", CHANGED_PROBE, run)
        run("matrix", "/Temporary/n4m.lua")
        run("matrix status", "share status")

        # The control that bites: one relay at a time told to change the
        # second qualifying answer it carries - the first read passes, the
        # second is changed - and then the record of it.
        for n in CHANGED:
            watch[n].arm(count=1, skip=1)
            run("changed " + n, "/Temporary/n4c.lua %s %d"
                % (at_port(relays[n]), changed_bytes))
            changed[n] = list(watch[n].altered)
            watch[n].off()

        run("after", "share status")

        written(guest, R, "n4x", probe(SIGNED_PROBE, measure_targets, measure), run)
        run("measure", "/Temporary/n4x.lua")

        # The stopped peer's bound, if the rest was quicker than it.
        left = stopped_at + BOUND + 1.5 - time.monotonic()

        if left > 0:
            time.sleep(left)

        run("gave up", "share status")
    except Exception as e:                  # noqa: BLE001 - said below
        said["error"] = "%s: %s" % (type(e).__name__, str(e)[:1500])
    finally:
        guest.close()

        for relay in watch.values():
            relay.close()

        def stop(m):
            try:
                peer(instance_of(m[0]), peers.get(m[0], smb1_port), "stop")
            except subprocess.CalledProcessError:
                pass

        with concurrent.futures.ThreadPoolExecutor(len(starts)) as pool:
            list(pool.map(stop, starts))

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

    check(("share: asking %s" % smb1_at) in text("stopped"),
          "connect --no-wait did not come back at once: %r" % text("stopped")[-300:])

    check(("%s  asking" % smb1_at) in text("asking") and took.get("asking", 99) < 5,
          "status did not answer at once while a stopped peer was asked "
          "(%.1f s): %r" % (took.get("asking", -1), text("asking")[-300:]))

    check(("%s  away  %s took the connection and did not answer within %d seconds"
           % (smb1_at, smb1_at, BOUND)) in text("gave up"),
          "a stopped peer was not given up on within the bound: %r"
          % text("gave up")[-300:])

    check(("share: %s answered - MACPEER, SMB 3.1.1, sealed; %s connected as %s"
           % (sealed_at, share, account)) in text("sealed")
          or ("share: %s answered - MACPEER, SMB 3.1.1, signed, sealed; %s connected as %s"
              % (sealed_at, share, account)) in text("sealed"),
          "a sealing peer was not connected, sealed: %r" % text("sealed")[-400:])

    refusal = "took the connection and hung up without answering: it does not speak SMB 2 or 3"
    check(refusal in text("smb1") and "answered - SMB" not in text("smb1"),
          "a peer of SMB 1 alone was not refused: %r" % text("smb1")[-300:])
    check(refusal in text("smb1 connect") and "connected as" not in text("smb1 connect"),
          "a connect to a peer of SMB 1 alone was not refused: %r"
          % text("smb1 connect")[-300:])

    def tagged(key, tag):
        return [m.group(1).split() for m in
                re.finditer(r"^N4%s (.*)$" % tag, text(key), re.M)]

    # The matrix: each conversation connected, its bytes this Mac's, and its
    # record what the wire showed.
    rows = {row[0]: row for row in tagged("matrix", "M")}
    status = text("matrix status")
    each_sum = hashlib.sha256(big[:each]).hexdigest()
    results = {}

    for name, dialect, sign, seal in MATRIX:
        a = at_port(relays[name])
        row = rows.get(a)
        wire = watch[name].seen()
        results[name] = (row, wire)

        if row is None or row[1] != "connected":
            fails.append("%s: did not connect: %r" % (name, row))
            continue

        _, _, said_dialect, said_signed, said_sealed, h_sum, b_sum, ms, got = row
        check(said_dialect == dialect and wire["dialect"] == dialect,
              "%s: the record says SMB %s and the wire %s" % (name, said_dialect,
                                                             wire["dialect"]))
        check(h_sum == hello_sum and b_sum == each_sum and got == str(each),
              "%s: what was read is not this Mac's bytes: %r" % (name, row))

        # Sealed: every answer after signing in came in a transform; signed:
        # every one carried a signature; neither: none did. And the record
        # says which, as `share status` prints it.
        if seal:
            truth = wire["sealed"] > 0 and wire["plain"] == 0 and wire["signed"] == 0
            want_words = "SMB %s, sealed" % dialect
        elif sign:
            truth = wire["signed"] > 0 and wire["plain"] == 0 and wire["sealed"] == 0
            want_words = "SMB %s, signed" % dialect
        else:
            truth = wire["plain"] > 0 and wire["signed"] == 0 and wire["sealed"] == 0
            want_words = "SMB %s" % dialect

        check(truth and said_signed == str(sign and not seal).lower()
              and said_sealed == str(seal).lower(),
              "%s: the record says signed %s, sealed %s; the wire carried %r"
              % (name, said_signed, said_sealed, wire))

        line = re.search(r"^%s  connected  MACPEER  (.*?)  Projects as %s"
                         % (re.escape(a), re.escape(account)), status, re.M)
        check(line is not None and line.group(1) == want_words,
              "%s: share status does not say %r: %r" % (name, want_words,
                                                        line and line.group(0)))

    # The control that bites.
    zeros = {}

    for n in CHANGED:
        a = at_port(relays[n])
        key = "changed " + n
        first = [r for r in tagged(key, "FIRST") if r[0] == a]
        second = [r for r in tagged(key, "SECOND") if r[0] == a]
        after = [r for r in tagged(key, "AFTER") if r[0] == a]
        words = re.search(r"^N4WORDS %s (.*)$" % re.escape(a), text(key), re.M)
        want_sum = hashlib.sha256(big[:changed_bytes]).hexdigest()
        zero_sum = zeros.setdefault(changed_bytes,
                                    hashlib.sha256(bytes(changed_bytes)).hexdigest())
        how = "its seal did not open" if "sealed" in n else "its signature did not match"

        check(first and first[0][1:] == [str(changed_bytes), want_sum],
              "%s: the read before the relay changed anything is not this "
              "Mac's bytes: %r" % (n, first))
        check(len(changed.get(n, [])) == 1,
              "%s: the relay did not change exactly one answer: %r"
              % (n, changed.get(n)))
        check(second and second[0][1:] == ["nil", "refused"],
              "%s: an answer changed on the way was handed over: %r" % (n, second))
        check(words is not None and ("answer was changed on the way - %s" % how)
              in words.group(1),
              "%s: the refusal is not in words: %r" % (n, words and words.group(1)))
        check(after and after[0][1] == zero_sum,
              "%s: the region still holds what the refused read put there: %r"
              % (n, after))
        check(re.search(r"^%s  away  MACPEER's answer was changed on the way - %s"
                        % (re.escape(a), re.escape(how)), text("after"), re.M)
              is not None,
              "%s: share status does not say why it ended: %r"
              % (n, text("after")[-600:]))

    # What a megabyte cost, directly to the peer.
    measured = {}
    measure_sum = hashlib.sha256(big[:measure]).hexdigest()

    for row in tagged("measure", "M"):
        name = next((n for n in MEASURED if at_port(peers[n]) == row[0]), None)

        if name and row[1] == "connected" and row[6] == measure_sum:
            measured.setdefault(name, []).append(int(row[7]))

    measured = {n: min(v) for n, v in measured.items()
                if len(v) == len(rounds) // len(MEASURED)}

    check(len(measured) == len(MEASURED),
          "the measure did not read every conversation's bytes: %r"
          % tagged("measure", "M"))

    check(" died" not in transcript and "smbfs exited" not in transcript,
          "something died: %r" % transcript[transcript.find(" died") - 200:][:400])

    # N2's 13 and N4's 4 a conversation, 6 a changed answer and the measure,
    # and one that nothing died.
    checks = 13 + 4 * len(MATRIX) + 6 * len(CHANGED) + 1 + 1
    log = os.path.join(ROOT, "build", "%s-serial.log" % board)

    with open(log, "w") as f:
        f.write(transcript)

    if fails:
        print("FAIL: %d problems in %d checks on smbfs connecting, signed and "
              "sealed (the serial line: %s):" % (len(fails), checks, log))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on smbfs connecting to Samba on this Mac, signed and "
          "sealed (3.1.1 signed, with the server's name; status; a probe; a "
          "wrong password refused in words; nobody at %d in %.1f s; sealed; "
          "controls: SMB 1 alone refused, and a stopped peer given up on after "
          "%d s while status answered in %.1f s; every dialect from 2.0.2 to "
          "3.1.1 signed, 3.0 to 3.1.1 sealed, 3.1.1 plain - each read and its "
          "record held to the wire; the control: one byte changed of a signed "
          "answer at 2.1 and 3.1.1 and a sealed one at 3.1.1, each refused in "
          "words with nothing handed over, the same relay passing the read "
          "before)." % (checks, NOBODY, took.get("nobody", -1), BOUND,
                        took.get("asking", -1)))
    print("  times: %s" % ", ".join("%s %.1f s" % (k, v) for k, v in took.items()
                                    if v >= 1.0))

    for name in names:
        row, wire = results[name]
        print("  %-13s %-9s %4d ms for %d KB  (wire: dialect %s, %d signed, "
              "%d sealed, %d neither)" % (name, row[1], int(row[7]), each >> 10,
                                          wire["dialect"], wire["signed"],
                                          wire["sealed"], wire["plain"]))

    base_ms = measured.get("3.1.1 plain", 0)
    mb = measure / 1048576

    for name in MEASURED:
        ms = measured[name]
        print("  measured %-13s %d MB in %5d ms, %.2f MB/s; %+.0f ms a MB "
              "against plain" % (name, mb, ms, mb / max(ms / 1000, 0.001),
                                 (ms - base_ms) / mb))

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


#
# Gone away, and back (N5): three small programs, each answer a tagged line.
#
# A read into a region: how long it took, and the bytes' SHA-256 or the
# words it ended in.
N5_READ = r"""
local crypto = use("/Kosmos/Kits/crypto")
local regions = use("/Kosmos/Libraries/regions.lua")
local hz = fs.read("/Devices/cpu").counter_hz
local tag, path, bytes = tostring(args):match("^%s*(%S+)%s+(%S+)%s+(%d+)")
bytes = tonumber(bytes)
local r = regions.make(bytes)
local t = sys.ticks()
local got, e = fs.read_into(path, r.cap, 0, bytes)
local ms = (sys.ticks() - t) * 1000 // hz
print(("N5" .. "R %s %d %s %s"):format(tag, ms, tostring(got), got and crypto.sha256(r.at, got) or tostring(e)))
regions.free(r)
"""

# A server watched until it is in a state - or, `plans=N`, until N tries
# have been planned - and what `share status` said of it: its state, how
# long that took, its sign-ins, and each wait it planned, in seconds.
N5_WAIT = r"""
local hz = fs.read("/Devices/cpu").counter_hz
local tag, a, want, most = tostring(args):match("^%s*(%S+)%s+(%S+)%s+(%S+)%s+(%d+)")
most = tonumber(most)
local need = tonumber(want:match("^plans=(%d+)$") or "")
local t = sys.ticks()
local plans, last, s, ms = {}, 0, nil, 0
while true do
  s = nil
  for _, x in ipairs(fs.share_status() or {}) do if x.address == a then s = x end end
  local nt = s and s.next_try_ms or 0
  if nt > last + 500 then plans[#plans + 1] = (nt + 999) // 1000 end
  last = nt
  ms = (sys.ticks() - t) * 1000 // hz
  if need and #plans >= need then break end
  if not need and s and s.state == want and not s.trying then break end
  if not need and want == "gone" and not s then break end
  if ms >= most then break end
  sys.sleep(10)
end
print(("N5" .. "W %s %s %d %d %d %s %s"):format(tag, s and s.state or "gone", ms, s and s.sign_ins or 0, s and s.next_try_ms or 0, #plans > 0 and table.concat(plans, ",") or "-", (s and s.why or "-"):gsub("%s", "_")))
"""

# What a server that is away answers, at once: a folder listed before, from
# memory and marked; a read, refused in words.
N5_AWAY = r"""
local regions = use("/Kosmos/Libraries/regions.lua")
local hz = fs.read("/Devices/cpu").counter_hz
local dir, file = tostring(args):match("^%s*(%S+)%s+(%S+)")
local t = sys.ticks()
local names = fs.list(dir)
local list_ms = (sys.ticks() - t) * 1000 // hz
local r = regions.make(4096)
t = sys.ticks()
local got, e = fs.read_into(file, r.cap, 0, 100)
local read_ms = (sys.ticks() - t) * 1000 // hz
regions.free(r)
print(("N5" .. "A %d %d %s %d %s %s"):format(names and #names or -1, list_ms, tostring(names and names.last_heard_ms), read_ms, tostring(got), tostring(e):gsub("%s", "_")))
"""


def folders(image, measuring):
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
    # **How much of the file is read whole** (`testing.md` 18.410). Into
    # one region, a megabyte a READ and two in flight: 16 MB in the gate -
    # sixteen READs, the pair refilled fourteen times - and the whole 64 MB
    # with `--measure`, the size 18.408's figures are of.
    #
    whole = BIG if measuring else (16 << 20)

    # What N5 reads after each return.
    after = 256 << 10
    after_sum = hashlib.sha256(big[:after]).hexdigest()

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

    probe = (PROBE.replace("@BIG@", str(BIG)).replace("@WHOLE@", str(whole))
             .replace("@SEED@", str(seed))
             .replace("@N@", str(PIECES)).replace("@MOST@", str(PIECE_MOST)))
    top = len(os.listdir(SHARE_DIR))

    guest = guest_on_network(R, image, 150)
    said, took, fails = {}, {}, []
    run = runner(guest, said, took)
    file = there + "/big.bin"

    marks = {}

    try:
        guest.wait_for(R.PROMPT, "the prompt")
        guest.wait_for("net: an address from DHCP", "a lease")

        run("nothing", "ls /Network")
        run("connect", "share connect smb://%s/%s %s" % (at, share, account), password)
        run("network", "ls /Network")
        run("server", "ls /Network/MACPEER")
        run("share", "ls " + there)

        written(guest, R, "n3", probe, run)
        run("probe", "/Temporary/n3.lua")

        run("cat", "cat %s/hello.txt" % there)
        run("cp", "cp %s/inside/deeper/note.txt /Temporary/note.txt" % there)
        run("cat copy", "cat /Temporary/note.txt")
        run("mkdir", "mkdir %s/newdir" % there)
        run("rm", "rm %s/hello.txt" % there)
        touched = time.monotonic()

        written(guest, R, "n5r", N5_READ, run)
        written(guest, R, "n5w", N5_WAIT, run)
        written(guest, R, "n5a", N5_AWAY, run)

        #
        # The control, and N5's first half: the peer stopped, every process
        # of it, with the connection made - the Mac asleep. The share's own
        # folder, heard more than two seconds ago, is asked again and
        # answered from memory, marked; one never listed is "not
        # answering"; and a read started as the peer stopped, left running
        # (`&`), ends in words once the server has said nothing for ten
        # seconds. The two `ls` take their five seconds inside its ten.
        #
        left = touched + 2.3 - time.monotonic()

        if left > 0:
            time.sleep(left)

        peer(instance, port, "pause")
        marks["inflight"] = len(guest.seen)
        guest.type("/Temporary/n5r.lua inflight %s %d &" % (file, after))
        run("stale", "ls " + there)
        run("never", "ls %s/inside" % there)
        said["inflight"] = guest.wait_for_line("N5R inflight ", "the read in flight to end",
                                               since=marks["inflight"])

        # Away: said so, and when it tries again; a folder from memory and
        # a read refused, both at once. Then the peer continues - before
        # the first try, two seconds after it went away - and smbfs signs
        # in again by itself.
        run("away", "share status")
        run("away answers", "/Temporary/n5a.lua %s %s/hello.txt" % (there, there))
        peer(instance, port, "resume")
        resumed = time.monotonic()
        run("back by itself", "/Temporary/n5w.lua back %s connected 30000" % at)
        took["back after resume"] = time.monotonic() - resumed
        run("back status", "share status")
        run("back", "ls " + there)
        run("read back", "/Temporary/n5r.lua back %s %d" % (file, after))

        #
        # N5's second half: the peer stopped outright - every process of it
        # ended, so the connection closes - is away at once; its tries are
        # two seconds apart, then four, then eight; restarted, `share retry`
        # signs in at once, a new session and a new tree.
        #
        peer(instance, port, "stop")
        run("closed", "/Temporary/n5w.lua closed %s away 10000" % at)
        run("backoff", "/Temporary/n5w.lua backoff %s plans=3 20000" % at)
        peer(instance, port, "start", "--dialect", "3.1.1")
        run("retry", "share retry " + at)
        run("retry status", "share status")
        run("read retried", "/Temporary/n5r.lua retried %s %d" % (file, after))

        # And let go of while away: forgotten, and never tried again - its
        # peer back within the two seconds a try would have come at.
        peer(instance, port, "stop")
        run("away again", "/Temporary/n5w.lua again %s away 10000" % at)
        run("forget", "share disconnect " + at)
        forgot = time.monotonic()
        run("forgotten", "share status")
        peer(instance, port, "start", "--dialect", "3.1.1")
        left = forgot + 3.0 - time.monotonic()

        if left > 0:
            time.sleep(left)

        run("still forgotten", "share status")
        run("network forgotten", "ls /Network")
        run("again", "share connect smb://%s/%s %s" % (at, share, account), password)

        # Tracker, on the share: listed and opened as a folder. Last, since
        # the window manager keeps the console once it is up.
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("wm tracker:" + there)
        said["tracker"] = guest.wait_for_line("tracker: showing " + there,
                                              "Tracker to open the share", mark)
        took["tracker"] = time.monotonic() - started
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

    # Gone away, and back (N5).
    def n5(key, tag):
        m = re.search(r"^N5%s (.*)$" % tag, text(key), re.M)
        return m.group(1).split(" ") if m else []

    inflight = said.get("inflight", "").strip().split(" ", 2)
    check(len(inflight) == 3 and inflight[1] == "nil"
          and "MACPEER is not answering" in inflight[2]
          and 9000 <= int(inflight[0]) < 20000,
          "a read in flight as the server stopped did not end in words after "
          "ten seconds of silence: %r" % said.get("inflight"))

    away_line = r"^%s  away  MACPEER is not answering - %s  since [\d.]+ s, " \
                r"(next try in \d+ s|trying again now)"
    check(re.search(away_line % (re.escape(at), "nothing heard from it for 10 seconds"),
                    text("away"), re.M) is not None,
          "share status does not say it is away and when it tries next: %r"
          % text("away")[-400:])

    answers = n5("away answers", "A")
    check(len(answers) == 6 and answers[0] == str(top) and answers[2] != "nil"
          and int(answers[1]) < 2000 and answers[4] == "nil"
          and "MACPEER_is_not_answering" in answers[5] and int(answers[3]) < 2000,
          "while away, a folder was not answered from memory marked, or a read "
          "not refused in words, at once: %r" % answers)

    back = n5("back by itself", "W")
    check(back[:2] == ["back", "connected"] and back[3] == "2",
          "the server continued, and smbfs did not sign in again by itself: %r" % back)
    check(re.search(r"^%s  connected  MACPEER  SMB 3\.1\.1  %s as %s  signed in 2 times"
                    % (re.escape(at), share, account), text("back status"), re.M)
          is not None,
          "share status does not say it is connected again: %r"
          % text("back status")[-300:])
    read_back = n5("read back", "R")
    check(read_back[:1] == ["back"] and read_back[2:4] == [str(after), after_sum],
          "a read after the server came back is not the Mac's bytes: %r" % read_back)

    closed = n5("closed", "W")
    check(closed[:2] == ["closed", "away"] and int(closed[2]) < 5000
          and "it_closed_the_connection" in closed[6],
          "a server stopped outright was not away at once, its connection closed: %r"
          % closed)

    backoff = n5("backoff", "W")
    check(backoff[:2] == ["backoff", "away"] and backoff[5] == "2,4,8",
          "the tries of a server away were not two, four and eight seconds "
          "apart: %r" % backoff)

    check(("share: %s answered - MACPEER, SMB 3.1.1; %s connected as %s"
           % (at, share, account)) in text("retry") and took.get("retry", 99) < 10,
          "share retry of a restarted server did not sign in at once (%.1f s): %r"
          % (took.get("retry", -1), text("retry")[-300:]))
    check(re.search(r"^%s  connected  MACPEER  SMB 3\.1\.1  %s as %s  signed in 3 times"
                    % (re.escape(at), share, account), text("retry status"), re.M)
          is not None,
          "share status after the retry: %r" % text("retry status")[-300:])
    retried = n5("read retried", "R")
    check(retried[:1] == ["retried"] and retried[2:4] == [str(after), after_sum],
          "a read after signing in to a restarted server is not the Mac's "
          "bytes - a new session and a new tree: %r" % retried)

    check(n5("away again", "W")[:2] == ["again", "away"]
          and ("share: %s let go" % at) in text("forget"),
          "a server away was not let go of: %r %r"
          % (n5("away again", "W"), text("forget")[-200:]))
    check("nothing has been asked of any server" in text("forgotten")
          and "nothing has been asked of any server" in text("still forgotten")
          and "(empty)" in text("network forgotten"),
          "a server let go of while away was tried again or kept: %r %r %r"
          % (text("forgotten")[-200:], text("still forgotten")[-200:],
             text("network forgotten")[-200:]))
    check("connected as %s" % account in text("again"),
          "connected again after letting go: %r" % text("again")[-300:])

    check(said.get("tracker", "").startswith(", %d items" % top),
          "Tracker did not open the share as a folder of %d: %r"
          % (top, said.get("tracker", "")[-200:]))

    check(" died" not in transcript and "smbfs exited" not in transcript,
          "something died: %r" % transcript[transcript.find(" died") - 200:][:400])

    checks = 19 + 14
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

    print("PASS: %d checks on a share as a folder, gone away and back (ls "
          "/Network and its server; the %d names of many/ the peer's; big.bin's "
          "SHA-256 read into a region and %d pieces at random offsets; cat, cp, "
          "dates; writes refused in words; Tracker opens it; a stopped peer "
          "answered from memory as last heard in %.1f s and 'not answering' in "
          "%.1f s; a read in flight ended in words after %.1f s; signed in "
          "again by itself %.1f s after the peer continued; a peer restarted "
          "tried at 2, 4 and 8 s and signed into by share retry; let go of "
          "while away, and not tried again)."
          % (checks, len(names), PIECES, took.get("stale", -1), took.get("never", -1),
             int(inflight[0]) / 1000 if len(inflight) == 3 else -1,
             took.get("back after resume", -1)))
    print("  times: %s" % ", ".join("%s %.1f s" % (k, v) for k, v in took.items()
                                    if v >= 1.0))
    print("  the %d names: listed in %d ms - %d listing, %d SMB requests, %d ms "
          "of them in smbfs - and %d namespace pages; %d getattrs in %d ms, "
          "%d from smbfs's memory" % (len(names), list_ms, listings, requests,
                                      smb_ms, pages, len(names), attr_ms, hits))
    print("  big.bin: %d MB in %d ms (%.2f MB/s), %d READs; its SHA-256 in %d ms"
          % (whole >> 20, big_ms, (whole / 1048576) / max(big_ms / 1000, 0.001),
             reads, sum_ms))
    return 0



#--------------------------------------------------------------------------
# The third part: signed and sealed (N4).
#--------------------------------------------------------------------------

#
# Each conversation pinned on a peer of its own: what is pinned, what is
# required, and what `share status` should then say. Every one is reached
# through a relay, unarmed, which watches what crosses - the dialect the
# server chose, and whether its answers were signed, sealed or neither.
#
MATRIX = [
    # name           dialect   sign   seal
    ("2.0.2",        "2.0.2",  True,  False),
    ("2.1",          "2.1",    True,  False),
    ("3.0",          "3.0",    True,  False),
    ("3.0.2",        "3.0.2",  True,  False),
    ("3.1.1",        "3.1.1",  True,  False),
    ("3.0 sealed",   "3.0",    True,  True),
    ("3.0.2 sealed", "3.0.2",  True,  True),
    ("3.1.1 sealed", "3.1.1",  True,  True),
    ("3.1.1 plain",  "3.1.1",  False, False),
]

# The relays that are armed after their read has passed: one of each way an
# answer is held - HMAC-SHA256 (2.x), AES-CMAC (3.x) and AES-128-CCM.
CHANGED = ["2.1", "3.1.1", "3.1.1 sealed"]

# What the cost of a megabyte is measured on, directly to the peer.
MEASURED = ["3.1.1 plain", "2.1", "3.0", "3.1.1", "3.1.1 sealed"]

#
# One program, written into `/Temporary` and run: each address connected, a
# file read from it and hashed, how long that took, and what the record
# says. Its tags are put together from halves, so the echo of this text as
# it is typed never matches.
#
SIGNED_PROBE = r"""
local crypto = use("/Kosmos/Kits/crypto")
local regions = use("/Kosmos/Libraries/regions.lua")
local hz = fs.read("/Devices/cpu").counter_hz
local function since(t) return (sys.ticks() - t) * 1000 // hz end
local function record(a) for _, s in ipairs(fs.share_status() or {}) do if s.address == a then return s end end end
local r = regions.make(@MOST@)
for item in ("@TARGETS@"):gmatch("[^,]+") do
  local a, bytes, keep = item:match("^(.-)=(%d+)=(%a)$")
  bytes = tonumber(bytes)
  local ok, why = fs.share_connect(a, "Projects", "@ACCOUNT@", "@PASSWORD@")
  local s = record(a)
  for _ = 1, 150 do s = record(a) if not s or s.state ~= "asking" then break end sys.sleep(25) end
  if not s or s.state ~= "connected" then
    print(("N4" .. "M %s %s none none none none none 0 %s"):format(a, s and s.state or "gone", tostring(s and s.why or why):gsub("%s", "_")))
  else
    local root = "/Network/" .. a .. "/Projects"
    local hello = fs.read(root .. "/hello.txt")
    local t = sys.ticks()
    local got, e = fs.read_into(root .. "/big.bin", r.cap, 0, bytes)
    local ms = since(t)
    print(("N4" .. "M %s %s %s %s %s %s %s %d %s"):format(a, s.state, tostring(s.dialect),
      tostring(s.signing), tostring(s.sealing),
      type(hello) == "string" and crypto.sha256(hello) or "none",
      got and crypto.sha256(r.at, got) or "none", ms, tostring(got or e):gsub("%s", "_")))
  end
  if keep ~= "k" then fs.share_disconnect(a) end
end
regions.free(r)
print("N4" .. "DONE")
"""

#
# And the read the relay changes: twice into one region, the relay told to
# let the first pass - so the region holds the file's bytes when the second
# is refused, and what is in it after shows whether any were handed over.
#
CHANGED_PROBE = r"""
local crypto = use("/Kosmos/Kits/crypto")
local regions = use("/Kosmos/Libraries/regions.lua")
local a, bytes = tostring(args):match("^%s*(%S+)%s+(%d+)")
bytes = tonumber(bytes)
local root = "/Network/" .. a .. "/Projects"
local r = regions.make(bytes)
local got, e = fs.read_into(root .. "/big.bin", r.cap, 0, bytes)
print(("N4" .. "FIRST %s %s %s"):format(a, tostring(got), got and crypto.sha256(r.at, got) or tostring(e):gsub("%s", "_")))
local got2, e2 = fs.read_into(root .. "/big.bin", r.cap, 0, bytes)
print(("N4" .. "SECOND %s %s %s"):format(a, tostring(got2), got2 and crypto.sha256(r.at, got2) or "refused"))
print("N4" .. "WORDS " .. a .. " " .. tostring(got2 and "" or e2))
print(("N4" .. "AFTER %s %s"):format(a, crypto.sha256(r.at, bytes)))
regions.free(r)
"""


def written(guest, R, name, text, run):
    """A program typed into `/Temporary` in pieces a line can carry."""
    one = " ".join(line.strip() for line in text.splitlines() if line.strip())
    parts = [one[i:i + 600] for i in range(0, len(one), 600)]

    for i, piece in enumerate(parts):
        guest.type('fs.write("/Temporary/%s_%d.lua", [==[%s]==])' % (name, i, piece))
        guest.wait_for(R.PROMPT, "the program written")

    run("written " + name, 'fs.write("/Temporary/%s.lua", ' % name
        + " .. ".join('fs.read("/Temporary/%s_%d.lua")' % (name, i)
                      for i in range(len(parts)))
        + ') print("program " .. "written")', ends="program written")


if __name__ == "__main__":
    sys.exit(main())
