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

Usage: run_share.py IMAGE
"""

import os
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
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"

    if not os.path.exists("/opt/homebrew/opt/samba/sbin/samba-dot-org-smbd"):
        print("SKIP: no Homebrew Samba on this Mac, so no peer to sign into "
              "(brew install samba; docs/sharing.md N0)")
        return 0

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


if __name__ == "__main__":
    sys.exit(main())
