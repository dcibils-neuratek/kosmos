#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""smbd, the server side of sharing (`docs/sharing.md` N12a-N12c):
signing in, folders of /Home shared read only, and smbd told what to share
through its own door.

A disk made here with made-up files on it, port 445 forwarded to this Mac,
and at the prompt the `sharing` program, which holds smbd's door (N12c):

  - a copy of `sharing` run from /Home: not handed the door;
  - `sharing folders Projects:/Home/Projects Notes:/Home/Notes:write`, then
    `sharing start kosmos` before any password is kept: refused;
  - `sharing start kosmos patient`, as Start with the machine is at boot:
    waiting for the keyring; then `sharing account kosmos <the fixture
    word>`, and it serves - the wait over by itself:
    serving on 445, and the keyring keeping the account under its own kind.

Then Samba's own `smbclient` - a client that is not Kosmos - asking:

  - the share list (N12d), over IPC$'s srvsvc pipe as the Mac's Finder
    asks it before it mounts anything: Projects, Notes and IPC$;
  - a compound, as Finder sends them: libsmb2's own stat, CREATE,
    QUERY_INFO and CLOSE in one, the last two naming the file the first
    opened;

  - the account and the right password: signed in, and `Projects` listed -
    a file, a larger one, a folder, with their sizes;
  - a file fetched, byte for byte; one of 1.5 MB, which takes more than one
    read, byte for byte; and one inside a folder;
  - the second share listed, so the shares' list is read past its first;
  - a file put into Projects, shared read only: refused, "access denied";
  - and Notes, shared read and write (N14): a 5 MB file put, a megabyte a
    write, and fetched back byte for byte; a folder made and a file put in
    it; a file renamed; a shorter file put over a longer one, and fetched
    back as the shorter - cut where it ends; a file deleted; a folder with
    something in it refused deletion;
  - a climb out of the share (`..\\Private\\secret.txt`): never served -
    which Samba's client never sends as it is, since it cleans ".." out of a
    name first; smbd's own refusal of one is `tools/test_smbpath.c`'s;
  - a share not offered: "bad network name";
  - the right account and a wrong password: a logon failure;
  - another account: refused before any answer is checked;
  - `sharing stop`: nothing answers; `sharing start kosmos` again: it does;
  - who is connected (N12c-3): an smbclient that stays, listed by `sharing
    who` as kosmos in Projects, and gone at `sharing disconnect`; This
    network only, with a netmask of one address the Mac refused, and with
    its network's back, let in;
  - the Servers window's File sharing page (N12c-2): the account's name
    typed, a new password set through its sheet, Projects chosen in the Open
    window's folder mode, the switch on - the new password signs in and the
    old one does not - a visitor under Connected now, ended by its
    Disconnect, and off again;
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

CHECKS = 38
PASSWORD = "test-only-word"
NEW_PASSWORD = "second-test-word"
SMBCLIENT = "/opt/homebrew/bin/smbclient"
STAT = os.path.join(ROOT, "build", "host", "libsmb2", "smb2-stat-sync")
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


def visitor(port, password):
    """An smbclient that stays connected, for Connected now and Disconnect -
    on a terminal of its own, since on a pipe it holds its output back until
    a buffer fills."""
    import pty

    master, slave = pty.openpty()
    p = subprocess.Popen([SMBCLIENT, "//127.0.0.1/Projects", "-p", str(port), "-m", "SMB3",
                          "-U", "kosmos%" + password],
                         stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    os.set_blocking(master, False)
    p.terminal = master
    p.heard = b""
    return p


def visitor_says(p, command, until, seconds=20):
    """`command` sent, and what it said up to `until`, or all it said if it
    stopped or the time ran out."""
    mark = len(p.heard)

    try:
        os.write(p.terminal, (command + "\n").encode())
    except OSError:
        pass

    deadline = time.monotonic() + seconds

    while time.monotonic() < deadline:
        try:
            chunk = os.read(p.terminal, 65536)
        except (BlockingIOError, OSError):
            chunk = None
        if chunk:
            p.heard += chunk
        if until.encode() in p.heard[mark:] or p.poll() is not None:
            break
        time.sleep(0.1)

    return p.heard[mark:].decode("utf-8", "replace")


def connections(R, guest, port, check):
    """N12c-3 at the prompt: who is connected, Disconnect, This network only."""
    v = visitor(port, PASSWORD)

    try:
        first = visitor_says(v, "ls", "blocks available")
        mark = len(guest.seen)
        guest.type("sharing who")
        guest.wait_for_line("10.0.2.2", "the visitor listed", mark)
        time.sleep(0.5)
        guest._read_available()
        m = re.search(r"^\s*(\d+)\s+10\.0\.2\.2\s+as kosmos in Projects, for \d+ s",
                      guest.seen[mark:], re.M)
        check("hello.txt" in first, "the staying visitor did not list Projects: %r" % first[-300:])
        check(m is not None, "`sharing who` did not list the visitor as kosmos in Projects:\n"
              + guest.seen[mark:][-300:])

        mark = len(guest.seen)
        guest.type("sharing disconnect %s" % (m.group(1) if m else "0"))
        guest.wait_for_line("smbd: disconnected 10.0.2.2", "the visitor disconnected", mark)
        # smbclient says so, then shows what it last listed with the error
        # after it - its cache, not an answer.
        after = visitor_says(v, "ls", "listing", 15)
        check("NT_STATUS_CONNECTION_DISCONNECTED" in after,
              "a disconnected visitor was not told: %r" % after[-300:])
    finally:
        v.kill()
        v.wait()
        os.close(v.terminal)

    # This network only: with a netmask of one address, the Mac at 10.0.2.2
    # is somewhere else - refused; with its own network's back, let in, since
    # the network is asked at each sign-in.
    guest.type("sharing stop")
    guest.wait_for("smbd: stopped sharing", "smbd stopped for This network only")

    def netmask(mask):
        mark = len(guest.seen)
        four = lambda text: "string.char(%s)" % text.replace(".", ",")
        guest.type('print("net" .. "mask", fs.net_configure("/Network", %s, %s, %s, %s))'
                   % (four("10.0.2.15"), four(mask), four("10.0.2.2"), four("10.0.2.3")))
        # Its answer, not the echo of the line typed, which says `"net" .. "mask"`.
        said = guest.wait_for_line("netmask\t", "the netmask set", mark)
        check(said.startswith("true"), "the netmask %s was not set: %r" % (mask, said))

    netmask("255.255.255.255")
    mark = len(guest.seen)
    guest.type("sharing start kosmos local")
    guest.wait_for_line("smbd: sharing on port 445", "smbd serving this network only", mark)
    time.sleep(1)
    far = ask(port, "Projects", "kosmos", PASSWORD, "ls")
    guest._read_available()
    check("NT_STATUS_LOGON_FAILURE" in far
          and "smbd: refused 10.0.2.2, which is not on this network" in guest.seen[mark:],
          "a visitor from outside this network was not refused: %r" % far[-200:])

    netmask("255.255.255.0")
    near = ask(port, "Projects", "kosmos", PASSWORD, "ls")
    check("hello.txt" in near,
          "with its network back, the same visitor was not let in: %r" % near[-200:])


def window_page(R, guest, port, check):
    from run_share import keys_for

    def line(text, what, since):
        return guest.wait_for_line(text, what, since)

    def window_at(title, since):
        found = line("wm: window %s at " % title, "the %s window" % title, since)
        return tuple(int(v) for v in re.match(r"(\d+),(\d+)", found).groups())

    def page(since, ok=lambda t: True, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for f in re.finditer(r"servers: file sharing - (.*)\n", guest.seen[since:]):
                if ok(f.group(1)):
                    return f.group(1)
            time.sleep(0.2)
        raise R.Failure("the page never said that within %d s:\n%s"
                        % (seconds, guest.seen[since:][-600:]))

    def places(text):
        m = re.search(r"its switch at (\d+),(\d+), \w+; share at (\d+),(\d+); "
                      r"password at (\d+),(\d+); name at (\d+),(\d+)", text)
        v = [int(x) for x in m.groups()]
        return (v[0], v[1]), (v[2], v[3]), (v[4], v[5]), (v[6], v[7])

    guest.type("sharing stop")
    guest.wait_for("smbd: stopped sharing", "smbd stopped before the window")

    mark = len(guest.seen)
    guest.type("wm servers:sharing")
    sx, sy = window_at("Servers", mark)
    told = page(mark, lambda t: t.startswith("stopped"))
    switch, share, password, name = places(told)
    time.sleep(2.0)
    width, height, _ = R.parse_ppm(guest.screendump())

    def click(x, y):
        guest.mouse_to(*R._to_tablet(x, y, width, height))
        time.sleep(0.25)
        guest.mouse_button(True, "left")
        time.sleep(0.1)
        guest.mouse_button(False, "left")
        time.sleep(0.3)

    def type_keys(text):
        for k in keys_for(text):
            guest.sendkey(k)

    # The account's name, and a password of its own through the sheet.
    click(sx + name[0], sy + name[1])
    type_keys("kosmos")
    time.sleep(0.5)
    mark = len(guest.seen)
    click(sx + password[0], sy + password[1])
    guest.wait_for_line("servers: the password sheet open", "the sheet", mark)
    type_keys(NEW_PASSWORD)
    guest.sendkey("ret")
    kept = guest.wait_for_line("servers: kosmos's password kept", "the new password", mark)

    # A folder, chosen in the folder window: down to Projects, then Share.
    mark = len(guest.seen)
    click(sx + share[0], sy + share[1])
    px, py = window_at("Share a folder of /Home", mark)
    said = guest.wait_for_line("panel: Share a folder of /Home at ", "the folder window", mark)
    # Its list has the keyboard from the moment it opens.
    for _ in range(12):
        if ", Projects selected" in said:
            break
        mark = len(guest.seen)
        guest.sendkey("down")
        said = guest.wait_for_line("panel: Share a folder of /Home at ", "the next folder", mark)
    bx, by = (int(v) for v in re.search(r"button at (\d+),(\d+)", said).groups())
    mark = len(guest.seen)
    click(px + bx, py + by)
    chosen = guest.wait_for_line("servers: sharing ", "the folder shared", mark)
    check(chosen.startswith("/Home/Projects as Projects"),
          "the folder window did not share Projects: %r" % chosen)

    # On, by its switch: serving, the window's password signing in and the
    # old one not.
    mark = len(guest.seen)
    click(sx + switch[0], sy + switch[1])
    on = page(mark, lambda t: t.startswith("sharing 1 folder"))
    time.sleep(1)
    listing = ask(port, "Projects", "kosmos", NEW_PASSWORD, "ls")
    old = ask(port, "Projects", "kosmos", PASSWORD, "ls")
    check("hello.txt" in listing,
          "the switch on, Projects was not listed with the window's password: %r"
          % listing[-300:])
    check("NT_STATUS_LOGON_FAILURE" in old,
          "the old password still signed in after the window changed it: %r" % old[-200:])
    check(on.startswith("sharing 1 folder") and kept is not None,
          "the page did not say it was sharing one folder: %r" % on)

    # Connected now: a visitor listed with its Disconnect, which ends it.
    v = visitor(port, NEW_PASSWORD)

    try:
        mark = len(guest.seen)
        visitor_says(v, "ls", "blocks available")
        row = guest.wait_for_line("servers: connected 10.0.2.2 as kosmos in Projects; "
                                  "disconnect at ", "the visitor on the page", mark)
        dx, dy = (int(n) for n in row.split(","))
        mark = len(guest.seen)
        click(sx + dx, sy + dy)
        guest.wait_for_line("smbd: disconnected 10.0.2.2", "the page's Disconnect", mark)
        after = visitor_says(v, "ls", "listing", 15)
        check("NT_STATUS_CONNECTION_DISCONNECTED" in after,
              "the page's Disconnect left the visitor connected: %r" % after[-300:])
    finally:
        v.kill()
        v.wait()
        os.close(v.terminal)

    # Off, by its switch.
    mark = len(guest.seen)
    click(sx + switch[0], sy + switch[1])
    page(mark, lambda t: t.startswith("stopped"))
    time.sleep(1)
    off = ask(port, "Projects", "kosmos", NEW_PASSWORD, "ls")
    check("hello.txt" not in off, "the switch off, smbd still answered: %r" % off[-200:])


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

        guest.type("sharing folders Projects:/Home/Projects Notes:/Home/Notes:write")
        guest.wait_for("2 folders, 0 connected", "two folders set")

        mark = len(guest.seen)
        guest.type("sharing start kosmos")
        guest.wait_for_line("sharing:", "a start with no account kept", since=mark)
        check("no account by that name is kept" in guest.seen[mark:],
              "a start before any password was kept was not refused:\n"
              + guest.seen[mark:][-300:])

        # Patient, as Start with the machine is at boot: before /Home - and
        # the keyring's file on it - has the password, it waits; then serves.
        mark = len(guest.seen)
        guest.type("sharing start kosmos patient")
        guest.wait_for_line("smbd: waiting for the keyring to hold kosmos", "a patient start", mark)
        guest.type("sharing account kosmos " + PASSWORD)
        guest.wait_for("password kept, as its NT hash", "the account kept")
        guest.wait_for_line("smbd: the keyring holds kosmos", "the wait over", mark)
        guest.wait_for_line("smbd: sharing on port 445", "smbd listening", mark)
        check(True, "")
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

        # N12d: the share list, as Finder asks it - IPC$, the srvsvc pipe,
        # NetShareEnumAll - which `smbclient -L` asks the same way.
        run = subprocess.run([SMBCLIENT, "-L", "//127.0.0.1", "-p", str(port), "-m", "SMB3",
                              "-U", "kosmos%" + PASSWORD], capture_output=True, text=True,
                             timeout=60)
        listed = run.stdout + run.stderr
        check(re.search(r"^\s*Projects\s+Disk", listed, re.M)
              and re.search(r"^\s*Notes\s+Disk", listed, re.M)
              and re.search(r"^\s*IPC\$\s+IPC", listed, re.M),
              "the share list over IPC$ was not Projects, Notes and IPC$:\n" + listed[-600:])

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

        # A compound - CREATE, QUERY_INFO and CLOSE in one, the last two
        # naming "the file just opened" - as Finder sends them: libsmb2's own
        # stat sends exactly that (`smb2-stat-sync`, built for this Mac).
        users = os.path.join(work, "ntlm_user")
        with open(users, "w") as f:
            f.write(":kosmos:%s\n" % PASSWORD)
        stat = subprocess.run([STAT, "smb://kosmos@127.0.0.1:%d/Projects/hello.txt" % port],
                              capture_output=True, text=True, timeout=60,
                              env=dict(os.environ, NTLM_USER_FILE=users))
        info = stat.stdout + stat.stderr
        check(stat.returncode == 0 and re.search(r"Size:\s*%d\b" % len(HELLO), info) is not None,
              "a compound's related CREATE, QUERY_INFO and CLOSE were not answered: %r"
              % info[-300:])

        # 6. The second share, so the list was read past its first.
        notes = ask(port, "Notes", "kosmos", PASSWORD, "ls")
        check(re.search(r"^\s+note\.txt\s+\w*\s+%d\s" % len(NOTE), notes, re.M) is not None,
              "the second share was not listed:\n" + notes[-400:])

        # 7. Read only.
        put = ask(port, "Projects", "kosmos", PASSWORD, "put hello.txt written.txt", cwd=got)
        check("NT_STATUS_ACCESS_DENIED" in put, "a file was not refused: %r" % put[-300:])

        # 7b. Read and write (N14): Notes.
        payload = os.urandom(5 * 1024 * 1024 + 4321)
        with open(os.path.join(got, "five.bin"), "wb") as f:
            f.write(payload)
        with open(os.path.join(got, "short.txt"), "wb") as f:
            f.write(b"short\n")
        for name in ("back.bin", "short-back.txt"):
            if os.path.exists(os.path.join(got, name)):
                os.remove(os.path.join(got, name))

        wrote = ask(port, "Notes", "kosmos", PASSWORD,
                    "put five.bin five.bin; get five.bin back.bin", cwd=got)
        said["put"] = wrote[-400:]
        check(read(os.path.join(got, "back.bin")) == payload,
              "a 5 MB file put into a share read and write did not come back byte for byte: %r"
              % wrote[-400:])

        made = ask(port, "Notes", "kosmos", PASSWORD,
                   "mkdir Inbox; put short.txt Inbox\\a.txt; rename five.bin renamed.bin; ls; "
                   "cd Inbox; ls", cwd=got)
        check(re.search(r"^\s+renamed\.bin\s+\w*\s+%d\s" % len(payload), made, re.M) is not None
              and re.search(r"^\s+five\.bin\s", made, re.M) is None
              and re.search(r"^\s+Inbox\s+D", made, re.M) is not None
              and re.search(r"^\s+a\.txt\s+\w*\s+6\s", made, re.M) is not None,
              "a folder made, a file put in it and a file renamed were not all so: %r" % made[-600:])

        cut = ask(port, "Notes", "kosmos", PASSWORD,
                  "put short.txt renamed.bin; get renamed.bin short-back.txt", cwd=got)
        check(read(os.path.join(got, "short-back.txt")) == b"short\n",
              "a shorter file put over a longer one did not come back as the shorter: %r" % cut[-300:])

        gone = ask(port, "Notes", "kosmos", PASSWORD, "rmdir Inbox; del renamed.bin; ls", cwd=got)
        check(re.search(r"^\s+renamed\.bin\s", gone, re.M) is None
              and re.search(r"^\s+Inbox\s+D", gone, re.M) is not None
              and ("NOT_EMPTY" in gone or "DIRECTORY_NOT_EMPTY" in gone),
              "a file deleted, and a folder with something in it kept: %r" % gone[-600:])

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

        # N12c-3 at the prompt: who is connected, Disconnect, This network
        # only.
        connections(R, guest, port, check)

        # N12c-2: the Servers window's File sharing page, driven as a person
        # would - the account's name typed, a new password in the sheet, a
        # folder chosen in the Open window's folder mode, the switch.
        window_page(R, guest, port, check)
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
            if l.startswith(("smbd", "servers:", "panel:", "wm: window")) or "net:" in l and "445" in l:
                print("  | " + l.strip()[:160])
        return 1

    print("PASS: %d checks on smbd (told through its door by `sharing`, which a copy "
          "in /Home is not handed; a start before any password refused; the account "
          "kept in the keyring under its own kind; Samba's smbclient through a forwarded "
          "port: the account and the fixture word signed in, SMB dialect %s; Projects listed with "
          "its sizes and folder; hello.txt, a 1.5 MB file over more than one read, and a "
          "file in a folder fetched byte for byte; the second share listed; a put refused "
          "in a share read only; in one read and write a 5 MB file put and fetched back, a folder "
          "made, a rename, a file cut shorter, a delete, and a full folder kept; "
          "as read only; a climb to a folder not shared never served; a share not offered "
          "'bad network name'; a wrong password a logon failure; another account refused; "
          "the server answering after all of it; stopped, nothing answering, and started "
          "again, serving; the Servers window's page - the account typed, a new password "
          "through its sheet that signs in where the old one no longer does, Projects "
          "chosen in the folder window, its switch on serving and off stopping, a visitor "
          "under Connected now ended by its Disconnect; who is connected at the prompt "
          "and disconnected; This network only refusing the Mac outside it and letting "
          "it in with the network back; a machine "
          "whose sharing was never started answering nothing)" % (CHECKS, said.get("dialect")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
