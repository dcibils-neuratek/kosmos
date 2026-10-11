#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""smbd, the server side of sharing (`docs/sharing.md` N12a-N12c):
signing in, folders of /Home shared read only, and smbd told what to share
through its own door.

A disk made here with made-up files on it, port 445 forwarded to this Mac,
and at the prompt the `sharing` program, which holds smbd's door (N12c):

  - a copy of `sharing` run from /Home: not handed the door;
  - `sharing folders Projects:/Home/Projects Notes:/Home/Notes`, then
    `sharing start kosmos` before any password is kept: refused;
  - `sharing account kosmos <the fixture word>`, `sharing start kosmos`:
    serving on 445, and the keyring keeping the account under its own kind.

Then Samba's own `smbclient` - a client that is not Kosmos - asking:

  - the account and the right password: signed in, and `Projects` listed -
    a file, a larger one, a folder, with their sizes;
  - a file fetched, byte for byte; one of 1.5 MB, which takes more than one
    read, byte for byte; and one inside a folder;
  - the second share listed, so the shares' list is read past its first;
  - a file put: refused, "access denied", since N12b shares read only;
  - a climb out of the share (`..\\Private\\secret.txt`): never served -
    which Samba's client never sends as it is, since it cleans ".." out of a
    name first; smbd's own refusal of one is `tools/test_smbpath.c`'s;
  - a share not offered: "bad network name";
  - the right account and a wrong password: a logon failure;
  - another account: refused before any answer is checked;
  - `sharing stop`: nothing answers; `sharing start kosmos` again: it does;
  - and a machine whose `sharing` was never started: nothing answers on the
    port at all.

Usage: run_smbd.py IMAGE
"""

import os
import random
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

CHECKS = 18
PASSWORD = "test-only-word"
SMBCLIENT = "/opt/homebrew/bin/smbclient"
LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")

# Made up for the test, and nobody's.
HELLO = b"Hello from a Kosmos share.\n"
INNER = b"A note inside a folder of the share.\n"
NOTE = b"The second share's only file.\n"
SECRET = b"Not shared: this must never leave the machine.\n"


def kfs(*args):
    return subprocess.run([LUA, KFS, *args], check=True, capture_output=True,
                          text=True, cwd=ROOT).stdout


def make_disk(work):
    """A /Home with two shared folders and one that is not, from bytes made here."""
    # Bytes that differ at every place, so a read from the wrong offset
    # cannot pass for the right one.
    big = random.Random(1975).randbytes(1536 * 1024)
    files = {"hello.txt": HELLO, "big.bin": big, "inner.txt": INNER,
             "note.txt": NOTE, "secret.txt": SECRET}
    for name, data in files.items():
        with open(os.path.join(work, name), "wb") as f:
            f.write(data)

    disk = os.path.join(work, "disk.img")
    if os.path.exists(disk):
        os.remove(disk)
    w = lambda n: os.path.join(work, n)                     # noqa: E731
    kfs("create", disk, "32",
        w("hello.txt") + ":/Home/Projects/hello.txt",
        w("big.bin") + ":/Home/Projects/big.bin",
        w("inner.txt") + ":/Home/Projects/Sub/inner.txt",
        w("note.txt") + ":/Home/Notes/note.txt",
        w("secret.txt") + ":/Home/Private/secret.txt",
        os.path.join(ROOT, "user/bin/programs/sharing.lua") + ":/Home/sharing.lua")
    return disk, big


def boot(R, image, port):
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    extra = ["-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:445" % port,
             "-device", R.device(image, "net") + ",netdev=net0"]
    setattr(R, board, saved + extra)
    try:
        return R.Guest(image, 120)
    finally:
        setattr(R, board, saved)


def ask(port, share, user, password, commands, cwd=None):
    run = subprocess.run([SMBCLIENT, "//127.0.0.1/" + share, "-p", str(port), "-m", "SMB3",
                          "-U", "%s%%%s" % (user, password), "-c", commands],
                         capture_output=True, text=True, timeout=90, cwd=cwd)
    return (run.stdout + run.stderr).strip()


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    work = scratch.directory("smbd")
    fails, said = [], {}
    disk, big = make_disk(work)
    got = os.path.join(work, "got")
    os.makedirs(got, exist_ok=True)

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                              # noqa: E402

    def check(ok, what):
        if not ok:
            fails.append(what)

    port = random.randint(20000, 40000)
    guest = boot(R, image, port)
    seen = ""
    try:
        guest.wait_for("kosmos> ", "a prompt")

        # N12c: the door, only to the image's own `sharing`.
        mark = len(guest.seen)
        guest.type("run /Home/sharing.lua")
        guest.wait_for_line("sharing:", "the copy in /Home answering", since=mark)
        check("not handed smbd's door" in guest.seen[mark:],
              "a copy of sharing in /Home was handed smbd's door:\n"
              + guest.seen[mark:][-300:])

        guest.type("sharing folders Projects:/Home/Projects Notes:/Home/Notes")
        guest.wait_for("2 folders, 0 connected", "two folders set")

        mark = len(guest.seen)
        guest.type("sharing start kosmos")
        guest.wait_for_line("sharing:", "a start with no account kept", since=mark)
        check("no account by that name is kept" in guest.seen[mark:],
              "a start before any password was kept was not refused:\n"
              + guest.seen[mark:][-300:])

        guest.type("sharing account kosmos " + PASSWORD)
        guest.wait_for("password kept, as its NT hash", "the account kept")
        guest.type("sharing start kosmos")
        guest.wait_for("smbd: sharing on port 445", "smbd listening")
        time.sleep(1)

        mark = len(guest.seen)
        guest.type("keyring")
        guest.wait_for_line(", the file ", "the keyring listed", since=mark)
        time.sleep(1)
        guest._read_available()
        check(re.search(r"^\s*\d+\s+sharing\s+this machine\s+kosmos", guest.seen[mark:], re.M)
              and PASSWORD not in guest.seen[mark:],
              "the keyring does not keep the account under its own kind:\n"
              + guest.seen[mark:][-500:])

        # 1-2. Signed in, and the share listed.
        listing = ask(port, "Projects", "kosmos", PASSWORD, "ls")
        guest._read_available()
        m = re.search(r"smbd: signed in, SMB dialect (\w+)", guest.seen)
        check(m is not None, "the right password did not sign in: %r" % listing[-300:])
        said["dialect"] = m.group(1) if m else "?"
        check(re.search(r"^\s+hello\.txt\s+\w*\s+%d\s" % len(HELLO), listing, re.M)
              and re.search(r"^\s+big\.bin\s+\w*\s+%d\s" % len(big), listing, re.M)
              and re.search(r"^\s+Sub\s+D\s+0\s", listing, re.M),
              "Projects was not listed with its files, sizes and folder:\n" + listing[-600:])

        # 3-5. Files fetched, byte for byte.
        for n in ("hello.txt", "big.bin", "inner.txt"):
            if os.path.exists(os.path.join(got, n)):
                os.remove(os.path.join(got, n))
        fetched = ask(port, "Projects", "kosmos", PASSWORD,
                      "get hello.txt hello.txt; get big.bin big.bin; "
                      "cd Sub; get inner.txt inner.txt", cwd=got)
        check(read(os.path.join(got, "hello.txt")) == HELLO,
              "hello.txt did not arrive as it is: %r" % fetched[-300:])
        b = read(os.path.join(got, "big.bin"))
        check(b == big, "big.bin did not arrive as it is (%s bytes of %d): %r"
              % (len(b) if b is not None else "no", len(big), fetched[-300:]))
        check(read(os.path.join(got, "inner.txt")) == INNER,
              "Sub\\inner.txt did not arrive as it is: %r" % fetched[-300:])

        # 6. The second share, so the list was read past its first.
        notes = ask(port, "Notes", "kosmos", PASSWORD, "ls")
        check(re.search(r"^\s+note\.txt\s+\w*\s+%d\s" % len(NOTE), notes, re.M) is not None,
              "the second share was not listed:\n" + notes[-400:])

        # 7. Read only.
        put = ask(port, "Projects", "kosmos", PASSWORD, "put hello.txt written.txt", cwd=got)
        check("NT_STATUS_ACCESS_DENIED" in put, "a file was not refused: %r" % put[-300:])

        # 8. Out of the share, never.
        climbed = os.path.join(got, "secret.txt")
        if os.path.exists(climbed):
            os.remove(climbed)
        climb = ask(port, "Projects", "kosmos", PASSWORD,
                    "get ..\\Private\\secret.txt secret.txt", cwd=got)
        check(read(climbed) != SECRET and SECRET.decode() not in climb,
              "a climb out of the share was served: %r" % climb[-300:])

        # 9. A share not offered.
        private = ask(port, "Private", "kosmos", PASSWORD, "ls")
        check("NT_STATUS_BAD_NETWORK_NAME" in private,
              "a folder not shared was not 'bad network name': %r" % private[-300:])

        # 10. A wrong password.
        wrong = ask(port, "Projects", "kosmos", "not-the-word", "ls")
        check("NT_STATUS_LOGON_FAILURE" in wrong,
              "a wrong password was not a logon failure: %r" % wrong[-200:])

        # 11. Another account.
        mark = len(guest.seen)
        other = ask(port, "Projects", "stranger", PASSWORD, "ls")
        guest._read_available()
        check("NT_STATUS_LOGON_FAILURE" in other and "smbd: refused stranger" in guest.seen[mark:],
              "another account was not refused: %r" % other[-200:])

        # 12. The machine still answering, after all of that.
        again = ask(port, "Projects", "kosmos", PASSWORD, "ls")
        check("hello.txt" in again, "the server did not answer after the others: %r"
              % again[-200:])

        # N12c: stopped, nothing; started again, it serves.
        guest.type("sharing stop")
        guest.wait_for("smbd: stopped sharing", "smbd stopped")
        stopped = ask(port, "Projects", "kosmos", PASSWORD, "ls")
        check("hello.txt" not in stopped and "Anonymous" not in stopped,
              "smbd answered after it was stopped: %r" % stopped[-200:])

        mark = len(guest.seen)
        guest.type("sharing start kosmos")
        guest.wait_for_line("smbd: sharing on port 445", "smbd serving again", since=mark)
        time.sleep(1)
        restarted = ask(port, "Projects", "kosmos", PASSWORD, "ls")
        check("hello.txt" in restarted, "smbd did not serve when started again: %r"
              % restarted[-200:])
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    # 13. Not asked to share: nothing on the port.
    port = random.randint(40001, 60000)
    guest = boot(R, image, port)
    try:
        guest.wait_for("kosmos> ", "a prompt")
        time.sleep(1)
        none = ask(port, "Projects", "kosmos", PASSWORD, "ls")
        guest._read_available()
        # QEMU's forward takes the connection and nothing behind it answers:
        # a timeout, a reset or a refusal - never a sign-in.
        check(not ("signed in" in guest.seen or "smbd: sharing" in guest.seen
                   or "hello.txt" in none),
              "a machine not told to share answered: %r" % none[-200:])
    except Exception as e:                  # noqa: BLE001
        fails.append("the second machine stopped: %s" % str(e).splitlines()[0])
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on smbd:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if l.startswith("smbd") or "net:" in l and "445" in l:
                print("  | " + l.strip()[:160])
        return 1

    print("PASS: %d checks on smbd (told through its door by `sharing`, which a copy "
          "in /Home is not handed; a start before any password refused; the account "
          "kept in the keyring under its own kind; Samba's smbclient through a forwarded "
          "port: the account and the fixture word signed in, SMB dialect %s; Projects listed with "
          "its sizes and folder; hello.txt, a 1.5 MB file over more than one read, and a "
          "file in a folder fetched byte for byte; the second share listed; a put refused "
          "as read only; a climb to a folder not shared never served; a share not offered "
          "'bad network name'; a wrong password a logon failure; another account refused; "
          "the server answering after all of it; stopped, nothing answering, and started "
          "again, serving; a machine whose sharing was never started answering nothing)" % (CHECKS, said.get("dialect")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
