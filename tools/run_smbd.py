#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""smbd, the server side of sharing (`docs/sharing.md` N12a and N12b):
signing in, and folders of /Home shared read only.

A machine told to share (`opt/kosmos/smbd=445,kosmos,HASH`, the NT hash of
the fixture word, and `opt/kosmos/smbd_shares=Projects:/Home/Projects,
Notes:/Home/Notes`), a disk made here with made-up files on it, its port
445 forwarded to this Mac, and Samba's own `smbclient` - a client that is
not Kosmos - asking:

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
  - and a machine not told to share: nothing answers on the port at all.

Usage: run_smbd.py IMAGE
"""

import os
import random
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

CHECKS = 13
PASSWORD = "test-only-word"
SMBCLIENT = "/opt/homebrew/bin/smbclient"
LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")

# Made up for the test, and nobody's.
HELLO = b"Hello from a Kosmos share.\n"
INNER = b"A note inside a folder of the share.\n"
NOTE = b"The second share's only file.\n"
SECRET = b"Not shared: this must never leave the machine.\n"


def md4(data):
    """RFC 1320, for the NT hash; this Python's hashlib has none."""
    def f(x, y, z): return (x & y) | (~x & z)
    def g(x, y, z): return (x & y) | (x & z) | (y & z)
    def h(x, y, z): return x ^ y ^ z
    def rol(v, s): v &= 0xffffffff; return ((v << s) | (v >> (32 - s))) & 0xffffffff
    msg = data + b"\x80" + b"\0" * ((55 - len(data)) % 64) + struct.pack("<Q", len(data) * 8)
    a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    for i in range(0, len(msg), 64):
        x = list(struct.unpack("<16I", msg[i:i + 64]))
        aa, bb, cc, dd = a, b, c, d
        for k in range(16):
            s = (3, 7, 11, 19)[k % 4]
            a, b, c, d = d, rol(a + f(b, c, d) + x[k], s), b, c
        for k in range(16):
            s = (3, 5, 9, 13)[k % 4]
            idx = (k % 4) * 4 + k // 4
            a, b, c, d = d, rol(a + g(b, c, d) + x[idx] + 0x5a827999, s), b, c
        for k in range(16):
            s = (3, 9, 11, 15)[k % 4]
            idx = (0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15)[k]
            a, b, c, d = d, rol(a + h(b, c, d) + x[idx] + 0x6ed9eba1, s), b, c
        a, b, c, d = (a + aa) & 0xffffffff, (b + bb) & 0xffffffff, (c + cc) & 0xffffffff, (d + dd) & 0xffffffff
    return struct.pack("<4I", a, b, c, d)


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
        w("secret.txt") + ":/Home/Private/secret.txt")
    return disk, big


def boot(R, image, port, share):
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    extra = ["-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:445" % port,
             "-device", R.device(image, "net") + ",netdev=net0"]
    if share:
        nt = md4(PASSWORD.encode("utf-16-le")).hex()
        # Commas, as a real machine's command line must have them; QEMU's
        # option parser takes ",," for a comma.
        extra += ["-fw_cfg", "name=opt/kosmos/smbd,string=445,,kosmos,," + nt,
                  "-fw_cfg", "name=opt/kosmos/smbd_shares,"
                             "string=Projects:/Home/Projects,,Notes:/Home/Notes"]
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
    guest = boot(R, image, port, True)
    seen = ""
    try:
        guest.wait_for("smbd: sharing on port 445", "smbd listening")
        time.sleep(1)

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
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    # 13. Not asked to share: nothing on the port.
    port = random.randint(40001, 60000)
    guest = boot(R, image, port, False)
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

    print("PASS: %d checks on smbd (Samba's smbclient through a forwarded port: the "
          "account and the fixture word signed in, SMB dialect %s; Projects listed with "
          "its sizes and folder; hello.txt, a 1.5 MB file over more than one read, and a "
          "file in a folder fetched byte for byte; the second share listed; a put refused "
          "as read only; a climb to a folder not shared never served; a share not offered "
          "'bad network name'; a wrong password a logon failure; another account refused; "
          "the server answering after all of it; a machine not told to share answering "
          "nothing)" % (CHECKS, said.get("dialect")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
