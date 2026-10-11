#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""smbd, the server side of sharing (`docs/sharing.md` N12a): signing in.

A machine told to share (`opt/kosmos/smbd=445 kosmos HASH`, the NT hash of
the fixture word), its port 445 forwarded to this Mac, and Samba's own
`smbclient` - a client that is not Kosmos - asking:

  - the account and the right password: signed in, then told there is no
    such share, since N12a offers none - which is what a sign-in that
    worked looks like;
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
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402

CHECKS = 4
PASSWORD = "test-only-word"
SMBCLIENT = "/opt/homebrew/bin/smbclient"


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


def boot(image, port, share):
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    extra = ["-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:445" % port,
             "-device", R.device(image, "net") + ",netdev=net0"]
    if share:
        nt = md4(PASSWORD.encode("utf-16-le")).hex()
        extra += ["-fw_cfg", "name=opt/kosmos/smbd,string=445 kosmos " + nt]
    setattr(R, board, saved + extra)
    try:
        return R.Guest(image, 120)
    finally:
        setattr(R, board, saved)


def ask(port, user, password):
    run = subprocess.run([SMBCLIENT, "//127.0.0.1/Projects", "-p", str(port), "-m", "SMB3",
                          "-U", "%s%%%s" % (user, password), "-c", "ls"],
                         capture_output=True, text=True, timeout=60)
    return (run.stdout + run.stderr).strip()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    fails, said = [], {}

    port = random.randint(20000, 40000)
    guest = boot(image, port, True)
    try:
        guest.wait_for("smbd: sharing on port 445", "smbd listening")
        time.sleep(1)

        right = ask(port, "kosmos", PASSWORD)
        guest._read_available()
        if "NT_STATUS_BAD_NETWORK_NAME" not in right or "smbd: signed in" not in guest.seen:
            fails.append("the right password did not sign in: %r" % right[-200:])
        else:
            said["dialect"] = re.search(r"smbd: signed in, SMB dialect (\w+)", guest.seen).group(1)

        wrong = ask(port, "kosmos", "not-the-word")
        if "NT_STATUS_LOGON_FAILURE" not in wrong:
            fails.append("a wrong password was not a logon failure: %r" % wrong[-200:])

        mark = len(guest.seen)
        other = ask(port, "stranger", PASSWORD)
        guest._read_available()
        if "NT_STATUS_LOGON_FAILURE" not in other or "smbd: refused stranger" not in guest.seen[mark:]:
            fails.append("another account was not refused: %r" % other[-200:])
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    # Not asked to share: nothing on the port.
    port = random.randint(40001, 60000)
    guest = boot(image, port, False)
    try:
        guest.wait_for("kosmos> ", "a prompt")
        time.sleep(1)
        none = ask(port, "kosmos", PASSWORD)
        guest._read_available()
        # QEMU's forward takes the connection and nothing behind it answers:
        # a timeout, a reset or a refusal - never a sign-in.
        if "signed in" in guest.seen or "smbd: sharing" in guest.seen or "Anonymous" in none:
            fails.append("a machine not told to share answered: %r" % none[-200:])
    except Exception as e:                  # noqa: BLE001
        fails.append("the second machine stopped: %s" % str(e).splitlines()[0])
    finally:
        guest.close()

    if fails:
        print("FAIL: %d of %d checks on smbd signing in:" % (len(fails), CHECKS))
        for f in fails:
            print("  " + f)
        for l in seen.splitlines():
            if l.startswith("smbd") or "net:" in l and "445" in l:
                print("  | " + l.strip()[:160])
        return 1

    print("PASS: %d checks on smbd signing in (Samba's smbclient through a forwarded port: "
          "the account and the fixture word signed in, SMB dialect %s, and told no share is "
          "offered yet; a wrong password a logon failure; another account refused; a "
          "machine not told to share answering nothing)" % (CHECKS, said.get("dialect")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
