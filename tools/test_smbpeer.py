#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""libsmb2 against the peer, on this Mac - `docs/sharing.md` step N0.

    python3 tools/test_smbpeer.py build/host/libsmb2

Before either is trusted inside the machine, the library as vendored
(`runtime/upstream/libsmb2/`, built for the Mac by the Makefile) and the
peer (`tools/smbpeer.py`, Samba run as the user on 127.0.0.1:4450) are held
to each other at every dialect the design names: 2.0.2, 3.0, 3.1.1 signed
and 3.1.1 sealed. At each, libsmb2's own `smb2-ls-async` lists the folder
of 2,000 files - every name, in any order - and `smb2-cat-async` reads the
64 MB file, whose SHA-256 must be the Mac's own of the same file.

**The control**: the peer pinned to 3.1.1 with encryption required and a
library told it may not encrypt would be one; libsmb2 has no such switch, so
the control is the peer's: a wrong password is refused, and the listing
then holds nothing.

Without Homebrew's Samba this is a skip that says so, not a failure: the
peer is a tool of this Mac's, and a machine without it has nothing to hold
the library to.
"""

import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PEER = os.path.join(HERE, "smbpeer.py")
STATE = os.path.join(ROOT, "build", "smbpeer")

CASES = [("2.0.2", False, False), ("3.0", False, False),
         ("3.1.1", True, False), ("3.1.1", False, True)]


def peer(*args):
    return subprocess.run([sys.executable, PEER, *args], capture_output=True,
                          text=True, check=True).stdout.strip()


def main():
    tools = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "host", "libsmb2")

    if not os.path.exists("/opt/homebrew/opt/samba/sbin/samba-dot-org-smbd"):
        print("SKIP: no Homebrew Samba on this Mac, so nothing to hold libsmb2 to "
              "(brew install samba; docs/sharing.md N0)")
        return 0

    checks, failed = 0, []

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(what)

    try:
        for dialect, sign, seal in CASES:
            args = ["start", "--dialect", dialect] + (["--sign"] if sign else []) + (["--seal"] if seal else [])
            peer(*args)
            _, port, share, user, password = peer("where").split()
            users = os.path.join(STATE, "ntlm_user")

            with open(users, "w") as f:
                f.write(f":{user}:{password}\n")

            env = dict(os.environ, NTLM_USER_FILE=users)
            url = f"smb://{user}@127.0.0.1:{port}/{share}"
            name = dialect + (" signed" if sign else "") + (" sealed" if seal else "")

            listed = subprocess.run([os.path.join(tools, "smb2-ls-async"), url + "/many"],
                                    capture_output=True, text=True, env=env, timeout=120)
            names = {line.split()[0] for line in listed.stdout.splitlines()
                     if line.strip() and line.split()[0].endswith(".txt")}
            check(names == {"f%04d.txt" % i for i in range(2000)},
                  f"{name}: the listing held {len(names)} of the 2,000 names")

            read = subprocess.run([os.path.join(tools, "smb2-cat-async"), url + "/big.bin"],
                                  capture_output=True, env=env, timeout=300)

            with open(os.path.join(STATE, "Projects", "big.bin"), "rb") as f:
                want = hashlib.sha256(f.read()).hexdigest()

            check(hashlib.sha256(read.stdout).hexdigest() == want,
                  f"{name}: the 64 MB file read back {len(read.stdout)} bytes, not the Mac's")

            print(f"  {name}: 2,000 names and 64 MB, byte for byte")

        # The control: the right server, the wrong password.
        with open(users, "w") as f:
            f.write(f":{user}:not-the-password\n")

        refused = subprocess.run([os.path.join(tools, "smb2-ls-async"), url + "/many"],
                                 capture_output=True, text=True,
                                 env=dict(os.environ, NTLM_USER_FILE=users), timeout=60)
        check(".txt" not in refused.stdout, "a wrong password was let in")
    finally:
        peer("stop")

    if failed:
        print("FAIL: %d of %d checks on libsmb2 against the peer:\n  %s"
              % (len(failed), checks, "\n  ".join(failed)))
        return 1

    print("PASS: %d checks on libsmb2 against Samba on this Mac (2.0.2, 3.0, "
          "3.1.1 signed and sealed: 2,000 names and 64 MB each; a wrong "
          "password refused)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
