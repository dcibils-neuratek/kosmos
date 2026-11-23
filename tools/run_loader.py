#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Programs from a file (`docs/elf.md` step 4).

Booted with a /Home of its own holding `/Home/apps/apptest/`:

  apptest.lua    `-- kosmos: image apptest.elf`, and prints what the
                 apptest kit answers - 42, which only its own image can
                 say: the kit is in no other (`make apps` checks)
  apptest.elf    that image, as the build linked it, stripped
  plain.lua      the same program with no image line: run in the system's
                 image, where there is no kit called apptest
  broken.lua     names broken.elf, the image's first 200 KB: refused with
                 the reader's sentence, and nothing started
  stranger.lua   names stranger.elf, the image's first page with the other
                 processor's number in it: refused as built for that one
  stale.lua      names stale.elf, the image whole with another protocol
                 stamp in it (`tools/protostamp.py`): refused as built for
                 another Kosmos, with what to do, and nothing run

and `run` at the prompt for each, then apptest.lua again - from the image
made the first time, which the launcher says only when it makes one. How
long each took is said.

Usage: run_loader.py IMAGE [APPS]

`APPS` is the directory `make apps` linked the images into: the lean
userland's by default, `build/user-test/apps` from the gate, which links them
against the test userland it builds anyway (`roadmap.md` 6zp).
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else None
WORK = scratch.directory("loader")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")


def this_boards_image(name="apptest.elf"):
    """The `name` `make apps` linked for this board, or None: from `APPS`,
    or the lean userland's `apps/` - one place, never the newest of any, since
    a stale image from a directory nothing builds any more is newer than
    nothing."""
    folder = APPS or os.path.join(ROOT, "build", "user-x86_64" if X86 else "user", "apps")
    path = os.path.join(folder, name)
    return path if os.path.exists(path) else None


PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The loader's test program (`tools/run_loader.py`).
{line}
local kit = use("apptest.elf")
print("apptest: " .. tostring(kit.answer()))
if kit.tls then print("apptest-tls: " .. kit.tls()) end
"""

app = this_boards_image()

if app is None:
    print("FAIL: no apptest.elf for this board - `make apps` (and ARCH=x86_64) builds it")
    sys.exit(1)

stripped = os.path.join(WORK, "apptest.elf")
subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                "--strip-debug", app, stripped], check=True)

with open(stripped, "rb") as f:
    whole = f.read()

with open(os.path.join(WORK, "broken.elf"), "wb") as f:
    f.write(whole[:200 * 1024])

stranger = bytearray(whole[:4096])
stranger[18:20] = (183 if X86 else 62).to_bytes(2, "little")

with open(os.path.join(WORK, "stranger.elf"), "wb") as f:
    f.write(bytes(stranger))

# The image whole, its protocol stamp another: what Doom linked for 0.10.205
# was on 4 October, as the system sees one.
MARK = b"KOSMOS-PROTOSTAMP:"
at = whole.find(MARK)

if at < 0:
    print("FAIL: apptest.elf carries no protocol stamp (`tools/protostamp.py`)")
    sys.exit(1)

STAMP = whole[at + len(MARK):at + len(MARK) + 16].decode()

with open(os.path.join(WORK, "stale.elf"), "wb") as f:
    f.write(whole[:at + len(MARK)] + b"0000000000000000" + whole[at + len(MARK) + 16:])

for name, line in (("apptest", "-- kosmos: image apptest.elf"), ("plain", ""),
                   ("broken", "-- kosmos: image broken.elf"),
                   ("stranger", "-- kosmos: image stranger.elf"),
                   ("stale", "-- kosmos: image stale.elf")):
    with open(os.path.join(WORK, name + ".lua"), "w") as f:
        f.write(PROGRAM.format(line=line))

#
# **C++** (`docs/webkit.md` W1d): `cxxtest.elf`, the first C++ in the tree,
# and `cxxtest.lua` beside it - what the C++ says, and its `thread_local`
# counted on two threads. GCC's images only: TinyCC's (`apps-tcc/`) have no
# C++. **The control is an image**: the same one with its unwinding tables
# zeroed, `.eh_frame` and its index, so the throw has nothing to walk and
# must not arrive at its `catch`.
#
CXX = not (APPS or "").rstrip("/").endswith("apps-tcc")
CXX_PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- C++'s test program (`tools/run_loader.py`).
-- kosmos: image {image}
local kit = use("{image}")
print("cxxtest: " .. kit.run())
print("cxxtest-threads: " .. kit.threads())
print("cxxtest-std: " .. kit.std())
print("cxxtest-streams: " .. kit.streams())
"""

cxx_files = []
cxx_elsewhere = []

if CXX:
    built = this_boards_image("cxxtest.elf")

    if built is None:
        print("FAIL: no cxxtest.elf for this board - `make apps` (and ARCH=x86_64) builds it")
        sys.exit(1)

    cxx_image = os.path.join(WORK, "cxxtest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, cxx_image], check=True)

    sections = subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "readelf",
                               "-S", "-W", cxx_image],
                              check=True, capture_output=True, text=True).stdout
    unwound = bytearray(open(cxx_image, "rb").read())
    zeroed = 0

    for line in sections.splitlines():
        fields = line.replace("[ ", "[").split()
        if len(fields) > 5 and fields[1] in (".eh_frame", ".eh_frame_hdr"):
            offset, size = int(fields[4], 16), int(fields[5], 16)
            unwound[offset:offset + size] = bytes(size)
            zeroed += 1

    if zeroed != 2:
        print("FAIL: cxxtest.elf has not got both of .eh_frame and .eh_frame_hdr")
        sys.exit(1)

    # Named `cxxtest.elf` as well, in a folder of its own: a program reaches
    # an image's kit by the image's name, so under any other name it would
    # be refused before anything was thrown.
    os.makedirs(os.path.join(WORK, "unwound"), exist_ok=True)

    with open(os.path.join(WORK, "unwound", "cxxtest.elf"), "wb") as f:
        f.write(bytes(unwound))

    with open(os.path.join(WORK, "cxxtest.lua"), "w") as f:
        f.write(CXX_PROGRAM.format(image="cxxtest.elf"))

    with open(os.path.join(WORK, "unwound", "unwound.lua"), "w") as f:
        f.write(CXX_PROGRAM.format(image="cxxtest.elf"))

    cxx_files = ["cxxtest.lua", "cxxtest.elf"]
    cxx_elsewhere = ["%s:/Home/apps/unwound/%s" % (os.path.join(WORK, "unwound", n), n)
                     for n in ("unwound.lua", "cxxtest.elf")]

#
# **The POSIX layer** (`docs/w3.md` W3a): `posixtest.elf`, its checks written
# as a port writes them, against `<pthread.h>` and nothing of Kosmos's.
#
POSIX_PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The POSIX layer's test program (`tools/run_loader.py`).
-- kosmos: image posixtest.elf
local kit = use("posixtest.elf")
print("posixtest: " .. kit.run())
"""

# **The Storage Kit** (`docs/w3.md` W3d1), from the same image: `/Home`,
# `/Temporary` and `/Kosmos` by path, from C, and from four threads at once.
STORAGE_PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Storage Kit's test program (`tools/run_loader.py`).
-- kosmos: image posixtest.elf
local kit = use("posixtest.elf")
print("storagetest: " .. kit.storage())
"""

# **Sockets** (`docs/w3.md` W3e), from the same image: a web server here on
# the host, reached as 10.0.2.2 through QEMU's network, a port nobody
# answers, and a listener in the guest that the host calls through QEMU's
# port forward. `needs network` is what gives the program `/Network`.
SOCKET_PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The sockets' test program (`tools/run_loader.py`).
-- kosmos: image posixtest.elf
-- kosmos: needs network
local kit = use("posixtest.elf")
print("sockettest: " .. kit.sockets("10.0.2.2", %d, %d, %d))
"""

GUEST_LISTENS = 7070                # and 7071, which nobody calls


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


WEB_PORT, REFUSED_PORT, CALLER_PORT = free_port(), free_port(), free_port()
caller_heard = [None]


def serve_web():
    """The guest's /big and /small/N, HTTP/1.0, one thread a connection."""
    import socket
    import threading

    big = bytes(i % 251 for i in range(200000))
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", WEB_PORT))
    server.listen(16)

    def one(c):
        try:
            c.settimeout(60)
            request = b""
            while b"\r\n\r\n" not in request:
                part = c.recv(4096)
                if not part:
                    return
                request += part
            path = request.split(b" ")[1].decode()
            if path == "/big":
                body = big
            elif path.startswith("/small/"):
                body = bytes([65 + int(path[7:])]) * 30000
            else:
                body = b""
            c.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        except (OSError, ValueError, IndexError):
            pass
        finally:
            c.close()

    def accept():
        while True:
            c, _ = server.accept()
            threading.Thread(target=one, args=(c,), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()


def call_guest():
    """Calls the guest's listener until it answers: QEMU takes the call at
    once and hangs it up while nobody listens there yet. A call that is
    not hung up waits as long as the guest may take to reach its `accept`,
    which is after everything else it checks - a caller that gave up sooner
    left the guest answering a call nobody was on any more."""
    import socket

    until = time.time() + 600
    while time.time() < until and caller_heard[0] is None:
        try:
            c = socket.create_connection(("127.0.0.1", CALLER_PORT), timeout=300)
            c.sendall(b"ping\n")
            heard = b""
            while len(heard) < 5:
                part = c.recv(16)
                if not part:
                    break
                heard += part
            c.close()
            if heard == b"pong\n":
                caller_heard[0] = heard
                return
        except OSError:
            pass
        time.sleep(1)


# GCC's images only, as C++'s: TinyCC's have no `_Thread_local`, which
# `pthread.c` is built on.
posix_files = []

if CXX:
    built = this_boards_image("posixtest.elf")

    if built is None:
        print("FAIL: no posixtest.elf for this board - `make apps` (and ARCH=x86_64) builds it")
        sys.exit(1)

    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, os.path.join(WORK, "posixtest.elf")], check=True)

    with open(os.path.join(WORK, "posixtest.lua"), "w") as f:
        f.write(POSIX_PROGRAM)

    with open(os.path.join(WORK, "storagetest.lua"), "w") as f:
        f.write(STORAGE_PROGRAM)

    with open(os.path.join(WORK, "sockettest.lua"), "w") as f:
        f.write(SOCKET_PROGRAM % (WEB_PORT, REFUSED_PORT, GUEST_LISTENS))

    posix_files = ["posixtest.lua", "storagetest.lua", "sockettest.lua", "posixtest.elf"]

#
# **Doom, installed** (`docs/elf.md` step 5): `doom.lua` and its image in one
# folder, `/Home/Apps/Doom`, as a stick carries them - and no WAD, which is
# not the repository's to have. Typed at the prompt, `doom` has to be found
# there, run in `doom.elf`, reach its engine as `use("doom.elf")`, and then
# say it has no WAD beside it: the sentence only the program itself says,
# once all of that has worked.
#
#
# **And Quake and the Super Nintendo**, installed the same way on 28
# September: each folder's Lua and its image, stripped.
#
installed = []

for folder, image in (("Doom", "doom.elf"), ("Quake", "quake.elf"), ("SNES", "snes.elf")):
    built = this_boards_image(image)

    if built is None:
        print("FAIL: no %s for this board - `make apps` (and ARCH=x86_64) builds it" % image)
        sys.exit(1)

    stripped_image = os.path.join(WORK, image)
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, stripped_image], check=True)
    installed.append("%s:/Home/Apps/%s/%s" % (stripped_image, folder, image))

    source = os.path.join(ROOT, "user", "installed", folder)

    for name in sorted(os.listdir(source)):
        if name.endswith(".lua"):
            installed.append("%s:/Home/Apps/%s/%s" % (os.path.join(source, name), folder, name))

files = ["apptest.lua", "plain.lua", "broken.lua", "stranger.lua", "stale.lua",
         "apptest.elf", "broken.elf", "stranger.elf", "stale.elf"] + cxx_files + posix_files
#
# **The disk as large as what goes on it, and room for the tests' writes**:
# it was 224 MB, fixed, and the images it carries grow with the runtime - on
# 22 November, in 0.12.80, they filled it, and the Storage Kit's write of
# 2.5 MB stopped at 2 with the disk full and nothing saying so. The writes
# the programs make are a few megabytes; 64 is room to spare.
#
pairs = (["%s:/Home/apps/apptest/%s" % (os.path.join(WORK, n), n) for n in files]
         + installed + cxx_elsewhere)
carried = sum(os.path.getsize(pair.split(":/Home/", 1)[0]) for pair in pairs)
disk_mb = (carried + (1 << 20) - 1) // (1 << 20) + 64
subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK, str(disk_mb)]
               + pairs,
               check=True, capture_output=True, cwd=ROOT)
os.environ["KOSMOS_DISK"] = HOME_DISK

import run_screenshot as R                                   # noqa: E402

# A network card, for the sockets: QEMU's own, with its router - the host -
# at 10.0.2.2, and the guest's listener forwarded to a port here.
if CXX:
    NIC = ["-netdev", "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:%d" % (CALLER_PORT, GUEST_LISTENS),
           "-device", R.device(IMAGE, "net") + ",netdev=net0"]
    if X86:
        R.X86_ARGS = R.X86_ARGS + NIC
    else:
        R.QEMU_ARGS = R.QEMU_ARGS + NIC


def main():
    if CXX:
        import threading
        serve_web()
        threading.Thread(target=call_guest, daemon=True).start()

    guest = R.Guest(IMAGE, 120)
    failed = []
    checks = 0
    times = []
    posix_said = [None]
    storage_said = [None]
    socket_said = [None]

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def run(name, want, seconds):
        return typed("run /Home/apps/apptest/%s.lua" % name, want, seconds)

    def typed(command, want, seconds):
        """`command` typed; the first line after it holding `want`.

        **And then the prompt, before the next one is typed.** A line a
        program says can arrive after the shell has printed its prompt, and
        this typed the next `run` at once - so on 27 September `plain`'s
        check read its own echoed command, which names `apptest`, and the
        `broken` check read `plain`'s late error. The echo of what was typed
        is never an answer either.
        """
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type(command)
        deadline = started + seconds
        found = None

        while time.monotonic() < deadline and found is None:
            # Not the first line and not a line ending in the command: the
            # echo is the first line only when nothing else arrived between
            # the prompt and it, and in the whole gate once, on 28
            # September, a line break did - so the echo of `plain`, which
            # names `apptest`, was read as its answer again. *Ending in* it,
            # not holding it: `doom` and `quake` answer in lines that name
            # themselves.
            # **Whole lines only**: the last piece is a line still arriving
            # until its newline does, and on 4 October, under the whole
            # gate's load, `stale`'s sentence was read as "run: st".
            # **And no piece of it**: on 7 October, under the gate, the
            # echo of `plain` arrived broken in two and its second half,
            # "/apptest/plain.lua", was read as the answer. A line that is
            # any part of what was typed is the echo, whole or in pieces.
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if (want in line and not line.rstrip().endswith(command)
                        and line.strip() not in command
                        and not line.lstrip().startswith("kosmos>")):
                    found = line.strip()
                    break
            if found is None:
                time.sleep(0.1)
                guest._read_available()

        took = time.monotonic() - started

        if found is not None:
            at = guest.seen.find(found, mark) + len(found)

            while time.monotonic() < deadline and "kosmos> " not in guest.seen[at:]:
                time.sleep(0.1)
                guest._read_available()

        return found, took

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        first = len(guest.seen)
        said, took = run("apptest", "apptest:", 180)
        times.append(took)
        check(said == "apptest: 42",
              "apptest.lua, in its own image, did not say 42: %r" % said)
        check("image: made /Home/apps/apptest/apptest.elf, " in guest.seen[first:],
              "the first start did not make the image from the file")
        # **Constructors and thread variables** (`docs/webkit.md` W1b): the
        # image's constructor ran before the kit was asked, and a second
        # thread's copy of a `_Thread_local` began as the template and stayed
        # its own.
        # GCC's image only: TinyCC's (`apps-tcc/`, `x86-tcc`) has no
        # `_Thread_local`, and its kit offers no `tls`.
        tls_line = "constructed 7, main 6, thread 500099, main again 6"
        check((APPS or "").rstrip("/").endswith("apps-tcc")
              or ("apptest-tls: " + tls_line) in guest.seen[first:],
              "the image's constructor or its thread variables were wrong: %r"
              % [l for l in guest.seen[first:].split("\n") if "apptest-tls" in l][:1])

        if CXX:
            said, _ = run("cxxtest", "cxxtest:", 180)
            check(said == "cxxtest: constructed 7, caught 'three frames down', unique 55",
                  "C++ in cxxtest.elf: its constructor, its throw or its containers "
                  "were wrong: %r" % said)
            line = [l.strip() for l in guest.seen.split("\n") if "cxxtest-threads:" in l
                    and "print(" not in l]
            check(line[-1:] == ["cxxtest-threads: main 8, thread 105, main again 8"],
                  "C++'s thread_local was not each thread's own: %r" % line[-1:])
            line = [l.strip() for l in guest.seen.split("\n") if "cxxtest-std:" in l
                    and "print(" not in l]
            check(line[-1:] == ["cxxtest-std: 40000 under a mutex, 100 handed over, made once, "
                                "slept 20 ms, own 4"],
                  "C++'s standard threads on the POSIX layer (W4a) did not all hold: %r"
                  % line[-1:])
            line = [l.strip() for l in guest.seen.split("\n") if "cxxtest-streams:" in l
                    and "print(" not in l]
            check(line[-1:] == ["cxxtest-streams: wrote '42 ff 3.25 00007 true', read 12 0.5 "
                                "word, file 'line one' 99"],
                  "C++'s streams over Kosmos's locale (G2) did not all hold: %r" % line[-1:])

            unwound_at = len(guest.seen)
            said, _ = typed("run /Home/apps/unwound/unwound.lua", "unwound", 180)
            heard = guest.seen[unwound_at:]
            check("image: made /Home/apps/unwound/cxxtest.elf, " in heard
                  and "caught 'three frames down'" not in heard,
                  "cxxtest.elf with its unwinding tables zeroed did not run, or "
                  "still caught its exception - the throw is not walking them: %r"
                  % heard[:400])

        said = None
        posix_at = len(guest.seen)      # the message below is made even without CXX

        if CXX:
            said, _ = run("posixtest", "posixtest:", 180)

        posix_said[0] = said if CXX else "not in TinyCC's images"
        check(not CXX or said is not None and said.startswith(
                  "posixtest: posix: counted 160000, ring 1000, timed out, once 1, keys 4, "
                  "thread-locals in turn, recursive, stacks, a whole stack, reaped, rwlock ")
              and ", joined 42; memory: 4 GB reserved, protected, discarded, unmapped "
                  "in the middle, fixed, refused, aligned, 4.8 GB of 4 MB blocks freed and "
                  "asked for again at one address; process: slept, timed, pid in the table, " in said
              and (" processors, Kosmos %s, random, no environment; "
                   % ("x86_64" if X86 else "aarch64")) in said
              and "; files: made, excl, read and written, positioned, appended, cut, dup "
                  "shared, flags, closed, folders listed and removed, read only, cwd, "
                  "realpath, console, room, 200 open, 4 threads; " in said
              and said.endswith("; stdio: written and read by line, a thousand of them, "
                                "positioned, scanned, given back, getline, r+, appended, wx "
                                "refused, ten thousand bytes, fdopen, tmpfile gone, a "
                                "provided file, the console"),
              "the POSIX layer's checks in posixtest.elf did not all hold: %r%s" %
              (said, "" if said else "; heard after it:\n" + guest.seen[posix_at:][-1500:]))

        said = None

        if CXX:
            said, _ = run("storagetest", "storagetest:", 120)

        storage_said[0] = said if CXX else "not in TinyCC's images"
        check(not CXX or said is not None and said.startswith(
                  "storagetest: storage: refused a second table, paths, no mount, own source ")
              and "/Home and /Temporary written, cut, grown, folders, renamed, deleted; "
                  "/Kosmos " in said
              and said.endswith(" programs, read only; crosses refused; 4 threads"),
              "the Storage Kit's checks in posixtest.elf did not all hold: %r" % said)

        said = None

        if CXX:
            said, _ = run("sockettest", "sockettest:", 180)

        socket_said[0] = said if CXX else "not in TinyCC's images"
        check(not CXX or said == "sockettest: sockets: names, rings past 64 KB and as asked, fetched 200000, two under "
                                 "poll, four threads, refused, timed out, pipes woken alone and "
                                 "beside a listener, accepted, descriptors",
              "the sockets' checks in posixtest.elf did not all hold: %r" % said)
        # The guest has answered by now, and the caller's thread may not
        # have read the answer yet: a few seconds for it to.
        heard_by = time.monotonic() + 10
        while CXX and caller_heard[0] is None and time.monotonic() < heard_by:
            time.sleep(0.2)
        check(not CXX or caller_heard[0] == b"pong\n",
              "the harness's call to the guest's listener was never answered")

        said, _ = run("plain", "apptest", 60)
        check(said is not None and "not running in apptest.elf" in said,
              "plain.lua, in the system's image, found the apptest kit: %r" % said)

        said, _ = run("broken", "run:", 60)
        check(said is not None and "broken.elf: a segment runs past the end of the file" in said,
              "broken.elf was not refused as the reader refuses it: %r" % said)

        said, _ = run("stranger", "run:", 60)
        other = "an AArch64 processor" if X86 else "an x86-64 processor"
        check(said is not None and ("stranger.elf: built for %s, not this one" % other) in said,
              "stranger.elf was not refused as built for another processor: %r" % said)

        said, _ = run("stale", "run:", 180)
        check(said is not None
              and ("stale was built for another Kosmos (protocols 0000000000000000, and "
                   "this system's %s): build it again - make install-apps" % STAMP) in said,
              "stale.elf, built for other protocols, was not refused with the sentence "
              "that says so: %r" % said)

        again = len(guest.seen)
        said, took = run("apptest", "apptest:", 180)
        times.append(took)
        check(said == "apptest: 42", "apptest.lua did not say 42 the second time: %r" % said)
        check("image: made" not in guest.seen[again:],
              "the second start made the image again rather than using the one kept")

        doom_at = len(guest.seen)
        said, took = typed("doom", "doom:", 240)
        times.append(took)
        check(said is not None
              and said.startswith("doom: no /Home/Apps/doom/doom1.wad"),
              "doom, typed, was not found in /Home/Apps/Doom, run in doom.elf "
              "and its engine reached - or did not say it has no WAD beside "
              "it: %r" % said)
        check("image: made /Home/Apps/doom/doom.elf, " in guest.seen[doom_at:],
              "doom did not run in the image beside it")

        quake_at = len(guest.seen)
        said, took = typed("quake", "quake:", 240)
        times.append(took)
        check(said is not None
              and said.startswith("quake: no /Home/Apps/quake/id1/pak0.pak"),
              "quake, typed, was not found in /Home/Apps/Quake, run in quake.elf "
              "and its engine reached - or did not say it has no pak beside it: %r"
              % said)
        check("image: made /Home/Apps/quake/quake.elf, " in guest.seen[quake_at:],
              "quake did not run in the image beside it")

        #
        # **The Super Nintendo's `--scale`**, from the display harness, which
        # carries no disk and so no installed application: through the window
        # manager, as the Deskbar and a launcher start it. A scale it cannot
        # draw is refused by name, and the option comes off the front of the
        # line rather than becoming part of the ROM's name. Neither needs a
        # ROM, which this suite does not carry.
        #
        def ask(line, want):
            mark = len(guest.seen)
            guest.type(line)
            deadline = time.monotonic() + 120

            while want not in guest.seen[mark:] and time.monotonic() < deadline:
                time.sleep(0.2)
                guest._read_available()

            heard = want in guest.seen[mark:]
            answer = guest.seen[mark:]
            stop = len(guest.seen)
            guest.proc.stdin.write(R.STOP_DESKTOP)
            guest.proc.stdin.flush()
            end = time.monotonic() + 20

            while time.monotonic() < end and R.PROMPT not in guest.seen[stop:]:
                time.sleep(0.2)
                guest._read_available()

            return heard, answer

        #
        # The refusal is also the proof it ran in `snes.elf`: `snes.lua`
        # reaches its options only after `use("snes.elf")`, which fails in
        # any other image. (The launcher's "image: made" is said by the
        # process that spawns for the window manager, whose `print` reaches
        # nothing - so it is not looked for here.)
        #
        heard, answer = ask("wm snes:--scale 3", "snes: --scale is 1 or 2, and 3 is neither")
        check(heard, "`wm snes:--scale 3` was not refused by name - so not "
              "found in /Home/Apps/SNES, or not run in snes.elf: %r" % answer[-300:])

        heard, answer = ask("wm snes:--scale 2 nosuch.sfc",
                            "snes: no /Home/roms/snes/nosuch.sfc")
        check(heard, "`wm snes:--scale 2 nosuch.sfc` did not look for exactly nosuch.sfc "
              "- the option has to come off the front of the ROM's name: %r" % answer[-300:])
    finally:
        guest.close()

    # What the guest said, kept when somebody asks for it.
    if os.environ.get("KOSMOS_LOADER_SERIAL"):
        with open(os.environ["KOSMOS_LOADER_SERIAL"], "w") as f:
            f.write(guest.seen)

    if failed:
        print("FAIL: %d of %d checks on programs from a file:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on programs from a file (a program run in the %.1f MB image "
          "beside it, where its kit is and the system's has none; a truncated image and "
          "one for another processor refused with the reader's sentences; started again "
          "from the image made the first time - %.1f s, then %.1f s; Doom and "
          "Quake, installed in /Home/Apps, each found by its name and run in its "
          "own image, %.1f s and %.1f s; and the Super Nintendo's --scale, from "
          "its image, through the window manager; the POSIX layer's checks: %s; "
          "the Storage Kit's: %s; and the sockets': %s)"
          % (checks, len(whole) / 1e6, times[0], times[1], times[2], times[3],
             (posix_said[0] or "").replace("posixtest: posix: ", ""),
             (storage_said[0] or "").replace("storagetest: storage: ", ""),
             (socket_said[0] or "").replace("sockettest: sockets: ", "")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
