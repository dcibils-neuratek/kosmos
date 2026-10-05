#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""An SMB server on this Mac for Kosmos to talk to, run as the user.

    python3 tools/smbpeer.py start [--dialect 3.1.1] [--sign] [--seal]
    python3 tools/smbpeer.py stop
    python3 tools/smbpeer.py where        # address, share, user, password

`docs/sharing.md` step N0. Samba's `smbd` from Homebrew, with every file it
keeps under `build/smbpeer/` and listening on 127.0.0.1:4450 - so it needs
no administrator, changes no setting of this Mac's, and is reached from a
guest as 10.0.2.2:4450 through QEMU's user networking. macOS's own File
Sharing is a system setting and Diego's; this is not it.

**What it serves**, made the first time and kept: `Projects`, holding 2,000
small files named so their order is known, a 64 MB file of known bytes and
a folder inside a folder - the shapes `docs/sharing.md`'s steps list,
read and checked against - and nothing of anybody's.

**The account** is a test one: the user's own name, as Samba run without
privilege can only be the user it runs as, with a password made here and
kept in `build/smbpeer/password` - never a real one, and never in the
repository.

**The dialect** is pinned with `--dialect` (2.0.2, 3.0, 3.1.1), signing
made mandatory with `--sign` and encryption required with `--seal`: step
N4's matrix is this script run once for each.
"""

import argparse
import getpass
import hashlib
import os
import secrets
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STATE = os.path.join(ROOT, "build", "smbpeer")
SHARE = os.path.join(STATE, "Projects")
PORT = 4450

SAMBA = "/opt/homebrew/opt/samba"
SMBD = os.path.join(SAMBA, "sbin", "samba-dot-org-smbd")
PDBEDIT = os.path.join(SAMBA, "bin", "pdbedit")

DIALECTS = {"2.0.2": "SMB2_02", "2.1": "SMB2_10", "3.0": "SMB3_00",
            "3.0.2": "SMB3_02", "3.1.1": "SMB3_11"}

FILES = 2000
BIG = 64 * 1024 * 1024


def big_bytes():
    """The 64 MB file's bytes: SHA-256 of a counter, repeated - known, and
    nothing a run of zeros could pass for."""
    out = bytearray()
    n = 0

    while len(out) < BIG:
        out += hashlib.sha256(n.to_bytes(8, "little")).digest() * 2048
        n += 1

    return bytes(out[:BIG])


def make_share():
    """The share's contents, once: made only if missing, so a run that
    checks them reads what the last one checked."""
    marker = os.path.join(SHARE, ".made")

    if os.path.exists(marker):
        return

    os.makedirs(os.path.join(SHARE, "many"), exist_ok=True)
    os.makedirs(os.path.join(SHARE, "inside", "deeper"), exist_ok=True)

    for i in range(FILES):
        with open(os.path.join(SHARE, "many", "f%04d.txt" % i), "w") as f:
            f.write("file %d\n" % i)

    with open(os.path.join(SHARE, "big.bin"), "wb") as f:
        f.write(big_bytes())

    with open(os.path.join(SHARE, "inside", "deeper", "note.txt"), "w") as f:
        f.write("a folder inside a folder\n")

    with open(os.path.join(SHARE, "hello.txt"), "w") as f:
        f.write("Hello from the Mac, over SMB.\n")

    open(marker, "w").close()


def password():
    """The test account's password: made once, kept beside the state."""
    path = os.path.join(STATE, "password")

    if not os.path.exists(path):
        with open(path, "w") as f:
            f.write(secrets.token_urlsafe(18))

        os.chmod(path, 0o600)

    with open(path) as f:
        return f.read().strip()


def config(dialect, sign, seal):
    pinned = DIALECTS[dialect]
    dirs = {name: os.path.join(STATE, name)
            for name in ("private", "lock", "state", "cache", "pid", "log")}

    for d in dirs.values():
        os.makedirs(d, exist_ok=True)

    text = f"""[global]
    server role = standalone server
    workgroup = KOSMOS
    netbios name = MACPEER
    interfaces = 127.0.0.1
    bind interfaces only = yes
    smb ports = {PORT}
    disable netbios = yes
    server min protocol = {pinned}
    server max protocol = {pinned}
    server signing = {"mandatory" if sign else "auto"}
    smb encrypt = {"required" if seal else "if_required"}
    map to guest = never
    passdb backend = tdbsam:{dirs["private"]}/passdb.tdb
    private dir = {dirs["private"]}
    lock directory = {dirs["lock"]}
    state directory = {dirs["state"]}
    cache directory = {dirs["cache"]}
    pid directory = {dirs["pid"]}
    log file = {dirs["log"]}/smbd.log
    log level = 1
    load printers = no
    printing = bsd
    printcap name = /dev/null
    disable spoolss = yes

[Projects]
    path = {SHARE}
    read only = yes
    guest ok = no
"""
    path = os.path.join(STATE, "smb.conf")

    with open(path, "w") as f:
        f.write(text)

    return path


def running():
    pid_file = os.path.join(STATE, "pid", "samba-dot-org-smbd.pid")

    if not os.path.exists(pid_file):
        pid_file = os.path.join(STATE, "pid", "smbd.pid")

    try:
        with open(pid_file) as f:
            pid = int(f.read().strip())
        os.kill(pid, 0)
        return pid
    except (OSError, ValueError):
        return None


def stop():
    pid = running()

    if pid:
        os.kill(pid, signal.SIGTERM)

        for _ in range(50):
            if not running():
                break
            time.sleep(0.1)

    return pid


def start(dialect, sign, seal):
    os.makedirs(STATE, exist_ok=True)
    stop()
    make_share()
    conf = config(dialect, sign, seal)
    user = getpass.getuser()
    secret = password()

    # The account, in this state's own passdb: Samba run as the user can only
    # serve as the user, so the test account is the user's name with a
    # password that belongs to this peer and nothing else.
    subprocess.run([PDBEDIT, "-s", conf, "-a", "-t", "-u", user],
                   input=f"{secret}\n{secret}\n", text=True,
                   capture_output=True, check=True)

    # In a session of its own: smbd signals its process group when it stops,
    # and in the caller's group that took the caller - the shell, the gate -
    # down with it.
    subprocess.Popen([SMBD, "-D", "-s", conf],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)

    for _ in range(100):
        if running():
            break
        time.sleep(0.1)

    if not running():
        sys.exit("smbpeer: smbd did not start; see build/smbpeer/log/smbd.log")

    print(f"smbpeer: smb://127.0.0.1:{PORT}/Projects (10.0.2.2:{PORT} from a "
          f"guest), {dialect}{', signed' if sign else ''}"
          f"{', sealed' if seal else ''}, as {user}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("what", choices=["start", "stop", "where"])
    ap.add_argument("--dialect", default="3.1.1", choices=sorted(DIALECTS))
    ap.add_argument("--sign", action="store_true")
    ap.add_argument("--seal", action="store_true")
    args = ap.parse_args()

    if args.what == "start":
        start(args.dialect, args.sign, args.seal)
    elif args.what == "stop":
        print("smbpeer: stopped" if stop() else "smbpeer: was not running")
    else:
        print(f"127.0.0.1 {PORT} Projects {getpass.getuser()} {password()}")


if __name__ == "__main__":
    main()
