#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""An SMB server on this Mac for Kosmos to talk to, run as the user.

    python3 tools/smbpeer.py start [--dialect 3.1.1] [--sign] [--seal]
    python3 tools/smbpeer.py stop
    python3 tools/smbpeer.py where        # address, share, user, password
    python3 tools/smbpeer.py pause|resume # SIGSTOP and SIGCONT, for a control

    --instance NAME --port N              # a peer of a suite's own (below)

`docs/sharing.md` step N0. Samba's `smbd` from Homebrew, with every file it
keeps under `build/smbpeer/` and listening on 127.0.0.1:4450 - so it needs
no administrator, changes no setting of this Mac's, and is reached from a
guest as 10.0.2.2:4450 through QEMU's user networking. macOS's own File
Sharing is a system setting and Diego's; this is not it.

**What it serves**, made the first time and kept: `Projects`, holding 2,000
small files named so their order is known, a 64 MB file of known bytes and
a folder inside a folder - the shapes `docs/sharing.md`'s steps list,
read and checked against - and nothing of anybody's. **And `Music`**, a
second share of one small file (step N6), so a server's list of shares
has more than one to choose from and a second share is connected on the
same session.

**The account** is a test one: the user's own name, as Samba run without
privilege can only be the user it runs as, with a password made here and
kept in `build/smbpeer/password` - never a real one, and never in the
repository.

**The dialect** is pinned with `--dialect` (2.0.2, 2.1, 3.0, 3.0.2,
3.1.1), signing made mandatory with `--sign` and encryption required with
`--seal`: step N4's matrix is this script run once for each, nine peers at
once on nine ports (`run_share.py`, its first part). `--dialect 1.0` pins SMB 1
alone (Samba's `NT1`), which nothing here speaks: the control a client is
held to when it says it refuses SMB 1 (step N2).

**A peer of a suite's own**, `--instance NAME --port N` (step N2): the gate
runs suites side by side, and `host`'s check and `arm-share` each starting
and stopping the one peer on 4450 would stop each other's. An instance keeps
its configuration, passdb, locks and pid under `build/smbpeer/instances/
NAME/` and listens on its own port; the share's files and the password are
the one set, read only, so nothing 64 MB is made twice.

`pause` and `resume` stop and continue the peer (SIGSTOP, SIGCONT): a
connection made while it is stopped is taken by the Mac's kernel and
answered by nobody, which is a server that has gone to sleep
mid-negotiation. **Every process of it** (step N3): smbd serves each
connection from a process of its own, forked from the listening one, so
stopping the listener alone leaves a connection already made answering -
and a share whose server has gone to sleep is that connection going
quiet. The whole process group is signalled.
"""

import argparse
import getpass
import hashlib
import os
import secrets
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = os.path.join(ROOT, "build", "smbpeer")
SHARE = os.path.join(BASE, "Projects")
MUSIC = os.path.join(BASE, "Music")
STATE = BASE                    # an instance's own, from `--instance`
PORT = 4450

# Where it listens: this Mac alone, unless `--listen` names its address on the
# network - for a real machine, the M700, to reach it (step N7 by hand).
LISTEN = "127.0.0.1"

SAMBA = "/opt/homebrew/opt/samba"
SMBD = os.path.join(SAMBA, "sbin", "samba-dot-org-smbd")
PDBEDIT = os.path.join(SAMBA, "bin", "pdbedit")

DIALECTS = {"1.0": "NT1", "2.0.2": "SMB2_02", "2.1": "SMB2_10", "3.0": "SMB3_00",
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


def make_music():
    """The second share (N6): one file, made if missing."""
    song = os.path.join(MUSIC, "song.txt")

    if not os.path.exists(song):
        os.makedirs(MUSIC, exist_ok=True)

        with open(song + ".%d" % os.getpid(), "w") as f:
            f.write("a song, as far as a test can tell\n")

        os.replace(song + ".%d" % os.getpid(), song)


def make_share():
    """The share's contents, once: made only if missing, so a run that
    checks them reads what the last one checked."""
    make_music()
    marker = os.path.join(SHARE, ".made")

    if os.path.exists(marker):
        return

    # Made beside it and renamed into place, so two suites starting peers at
    # once on a fresh tree never serve a half-made share: the loser of the
    # rename throws its copy away.
    os.makedirs(BASE, exist_ok=True)
    making = os.path.join(BASE, "Projects.making.%d" % os.getpid())
    shutil.rmtree(making, ignore_errors=True)
    os.makedirs(os.path.join(making, "many"))
    os.makedirs(os.path.join(making, "inside", "deeper"))

    for i in range(FILES):
        with open(os.path.join(making, "many", "f%04d.txt" % i), "w") as f:
            f.write("file %d\n" % i)

    with open(os.path.join(making, "big.bin"), "wb") as f:
        f.write(big_bytes())

    with open(os.path.join(making, "inside", "deeper", "note.txt"), "w") as f:
        f.write("a folder inside a folder\n")

    with open(os.path.join(making, "hello.txt"), "w") as f:
        f.write("Hello from the Mac, over SMB.\n")

    open(os.path.join(making, ".made"), "w").close()

    if os.path.isdir(SHARE) and not os.path.exists(marker):
        shutil.rmtree(SHARE, ignore_errors=True)    # an older, unfinished one

    try:
        os.rename(making, SHARE)
    except OSError:
        shutil.rmtree(making, ignore_errors=True)   # another made it first


def password():
    """The test account's password: made once, kept beside the state."""
    path = os.path.join(BASE, "password")
    os.makedirs(BASE, exist_ok=True)

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
    interfaces = {LISTEN}
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

[Music]
    path = {MUSIC}
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


def signal_peer(sig):
    """The peer and every connection it is serving: smbd's process group,
    which is its own (`start` starts it in a session of its own)."""
    pid = running()

    if pid:
        try:
            group = os.getpgid(pid)
        except OSError:
            group = None

        if group is not None and group != os.getpgrp():
            os.killpg(group, sig)
        else:
            os.kill(pid, sig)

    return pid


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def stop():
    """The peer and every connection it serves, ended - and waited for.

    **Waited for by its process, not its pid file** (`testing.md` 18.411):
    smbd removes the file before it has finished going, so a `start` right
    after a `stop` - which N5's suite does, a server restarted - met the
    old one still running, and refused to start ("smbd is already
    running"). And every process of it, so a connection it was serving
    closes as a machine that went away closes it."""
    pid = running()

    if not pid:
        return None

    try:
        group = os.getpgid(pid)
    except OSError:
        group = None

    def everyone(sig):
        try:
            if group is not None and group != os.getpgrp():
                os.killpg(group, sig)
            else:
                os.kill(pid, sig)
        except OSError:
            pass

    everyone(signal.SIGCONT)                    # a paused one cannot hear TERM
    everyone(signal.SIGTERM)

    for _ in range(100):
        if not alive(pid):
            break
        time.sleep(0.05)
    else:
        everyone(signal.SIGKILL)

        for _ in range(40):
            if not alive(pid):
                break
            time.sleep(0.05)

    for name in ("samba-dot-org-smbd.pid", "smbd.pid"):
        try:
            os.unlink(os.path.join(STATE, "pid", name))
        except OSError:
            pass

    return pid


def listening():
    """Whether something takes a connection on the peer's port."""
    import socket

    try:
        socket.create_connection((LISTEN, PORT), timeout=0.5).close()
        return True
    except OSError:
        return False


def start(dialect, sign, seal, changed=False):
    os.makedirs(STATE, exist_ok=True)
    stop()
    make_share()
    conf = config(dialect, sign, seal)
    user = getpass.getuser()
    secret = password()

    # **Changed** (`keyring.md`, K5): the account's password is no longer the
    # one a client remembered - the usual one with "-changed" after it.
    if changed:
        secret += "-changed"

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

    # Started is listening, not a pid file written: a suite connects the
    # moment this returns.
    for _ in range(200):
        if running() and listening():
            break
        time.sleep(0.05)

    if not (running() and listening()):
        sys.exit("smbpeer: smbd did not start; see %s/log/smbd.log" % STATE)

    print(f"smbpeer: smb://127.0.0.1:{PORT}/Projects (10.0.2.2:{PORT} from a "
          f"guest), {dialect}{', signed' if sign else ''}"
          f"{', sealed' if seal else ''}, as {user}")


def main():
    global STATE, PORT, LISTEN
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("what", choices=["start", "stop", "where", "pause", "resume"])
    ap.add_argument("--dialect", default="3.1.1", choices=sorted(DIALECTS))
    ap.add_argument("--sign", action="store_true")
    ap.add_argument("--seal", action="store_true")
    ap.add_argument("--changed", action="store_true",
                    help="start with the account's password changed")
    ap.add_argument("--instance", default=None)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--listen", default=None,
                    help="this Mac's address on the network, for a real machine")
    args = ap.parse_args()

    PORT = args.port
    LISTEN = args.listen or LISTEN

    if args.instance:
        STATE = os.path.join(BASE, "instances", args.instance)

    if args.what == "start":
        start(args.dialect, args.sign, args.seal, args.changed)
    elif args.what == "stop":
        print("smbpeer: stopped" if stop() else "smbpeer: was not running")
    elif args.what in ("pause", "resume"):
        sig = signal.SIGSTOP if args.what == "pause" else signal.SIGCONT
        print("smbpeer: %s" % (args.what + "d" if signal_peer(sig) else "not running"))
    else:
        print(f"127.0.0.1 {PORT} Projects {getpass.getuser()} {password()}")


if __name__ == "__main__":
    main()
