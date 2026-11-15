#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5c's SQLite on Kosmos (`docs/w5.md`).

`sqltest.elf` - SQLite 3.54.1 with its own `fuzzcheck` and `speedtest1`,
and `user/kits/sqltest/sqltest.c` - booted twice on one `/Home`:

1. `fuzzcheck` over SQLite's eight fuzz databases, each run's count of
   tests held to what the same program says on this Mac, and no errors;
   `speedtest1 --verify` on a database in `/Home`, its hash held to this
   Mac's; the dotfile lock, two connections, the second refused while the
   first writes and let in after; and five thousand rows written.
2. The same rows read back, after the machine was turned off and on, with
   the fingerprint the first boot said - and this Mac's.

This Mac's programs are built here from the same sources with the same
options (`SQLITE_OPTS` in the Makefile), so a difference is Kosmos's.

Usage: run_sql.py IMAGE [APPS]

`KOSMOS_SQL_BREAK` is the control: `fuzz` gives the reference one fuzz
database with a byte changed in every test, `keep` turns over one byte of
the database between the boots; the guest must disagree either way.
"""

import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import upstream                                              # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("sql")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")
TEST = os.path.join(ROOT, "user", "kits", "sqltest")
FUZZ = ["fuzzdata%d.db" % i for i in range(1, 9)]
KEPT = "/Home/apps/sql/kept.db"

FIRST = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5c's SQLite test, the first boot (`tools/run_sql.py`).
-- kosmos: image sqltest.elf
local kit = use("sqltest.elf")
local fuzz = {}
for i = 1, 8 do fuzz[i] = "/Home/apps/sql/fuzzdata" .. i .. ".db" end
local code, said = kit.fuzzcheck(fuzz)
write(said)
print("sql-fuzzcheck: exit " .. code)
code, said = kit.speedtest1({"--verify", "--size", "10", "/Home/apps/sql/speed.db"})
write(said)
print("sql-speedtest1: exit " .. code)
print(kit.locks("/Home/apps/sql/locks.db"))
print(kit.keep("%s", "write"))
print("sql-done: first")
""" % KEPT

SECOND = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5c's SQLite test, the second boot (`tools/run_sql.py`).
-- kosmos: image sqltest.elf
local kit = use("sqltest.elf")
print(kit.keep("%s", "read"))
print("sql-done: second")
""" % KEPT


def options():
    """`SQLITE_OPTS`, as the Makefile has it, so this Mac's programs are
    the guest's."""
    import shlex
    text = open(os.path.join(ROOT, "Makefile")).read().replace("\\\n", " ")
    line = next(ln for ln in text.splitlines() if ln.startswith("SQLITE_OPTS :="))
    return shlex.split(line.split(":=", 1)[1])


def build_mac():
    upstream.unpack("sqlite")
    upstream.unpack("sqlite-src")
    amal = upstream.where("sqlite")
    src = upstream.where("sqlite-src")
    opts = options()
    made = os.path.join(WORK, "mac")
    os.makedirs(made, exist_ok=True)
    cc = ["/usr/bin/clang", "-O2", "-w", "-I" + amal, "-I" + os.path.join(src, "test"),
          "-I" + os.path.join(src, "ext", "recover")] + opts
    lib = os.path.join(made, "sqlite3.o")
    if not os.path.exists(lib):
        subprocess.run(cc + ["-c", os.path.join(amal, "sqlite3.c"), "-o", lib], check=True)
    misc = [os.path.join(src, "ext", "misc", n) for n in (
        "base64.c", "base85.c", "completion.c", "decimal.c", "diskused.c", "ieee754.c",
        "randomjson.c", "regexp.c", "series.c", "shathree.c", "sha1.c", "stmtrand.c",
        "strdup.c")]
    fuzzcheck = os.path.join(made, "fuzzcheck")
    subprocess.run(cc + [os.path.join(src, "test", n) for n in (
        "fuzzcheck.c", "ossfuzz.c", "fuzzinvariants.c", "vt02.c")]
        + [os.path.join(src, "ext", "recover", n) for n in ("dbdata.c", "sqlite3recover.c")]
        + misc + [lib, "-o", fuzzcheck], check=True)
    speedtest1 = os.path.join(made, "speedtest1")
    subprocess.run(cc + [os.path.join(src, "test", "speedtest1.c"), lib, "-o", speedtest1],
                   check=True)
    sqltest = os.path.join(made, "sqltest")
    subprocess.run(cc + ["-DSQLTEST_HOST", os.path.join(TEST, "sqltest.c"), lib,
                         "-o", sqltest], check=True)
    return src, fuzzcheck, speedtest1, sqltest


def fuzz_counts(text):
    """Each database's count of tests, as `fuzzcheck` says it."""
    return re.findall(r"(fuzzdata\d\.db): [0-9% ]*- (\d+) tests", text)


def reference(src, fuzzcheck, speedtest1, sqltest, broken):
    mac = os.path.join(WORK, "mac-run")
    shutil.rmtree(mac, ignore_errors=True)
    os.makedirs(mac)
    dbs = []
    for n in FUZZ:
        into = os.path.join(mac, n)
        shutil.copyfile(os.path.join(src, "test", n), into)
        dbs.append(into)
    if broken == "fuzz":
        # The control: one database's tests taken away from the reference.
        subprocess.run(["sqlite3", dbs[0], "DELETE FROM xsql WHERE rowid % 2 = 0"],
                       check=True)
    fz = subprocess.run([fuzzcheck] + dbs, capture_output=True, text=True, cwd=mac)
    st = subprocess.run([speedtest1, "--verify", "--size", "10",
                         os.path.join(mac, "speed.db")], capture_output=True, text=True)
    locks = subprocess.run([sqltest, "locks", os.path.join(mac, "locks.db")],
                           capture_output=True, text=True, check=True).stdout.strip()
    keep = subprocess.run([sqltest, "write", os.path.join(mac, "kept.db")],
                          capture_output=True, text=True, check=True).stdout.strip()
    return fz.stdout + fz.stderr, st.stdout, locks, keep


def boot(R, program, last, seconds):
    guest = R.Guest(IMAGE, seconds)
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run " + program)
        name = os.path.basename(program).rsplit(".", 1)[0]
        ended = re.compile(r"process \d+ \(%s\) ended" % re.escape(name))
        while time.monotonic() < started + seconds and last not in guest.seen[mark:] \
                and not ended.search(guest.seen, mark):
            time.sleep(0.2)
            guest._read_available()
        time.sleep(0.5)
        guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()
    if os.environ.get("KOSMOS_SQL_SERIAL"):
        with open(os.environ["KOSMOS_SQL_SERIAL"] + "." + name, "w") as f:
            f.write(guest.seen)
    return guest.seen[mark:].replace("\r", ""), took


def line(text, prefix):
    for ln in text.splitlines():
        if ln.startswith(prefix):
            return ln.strip()
    return None


def main():
    built = os.path.join(APPS, "sqltest.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1
    broken = os.environ.get("KOSMOS_SQL_BREAK")
    if broken not in (None, "fuzz", "keep"):
        print("FAIL: KOSMOS_SQL_BREAK is fuzz or keep, not %s" % broken)
        return 1

    src, fuzzcheck, speedtest1, sqltest = build_mac()
    mac_fuzz, mac_speed, mac_locks, mac_keep = reference(src, fuzzcheck, speedtest1,
                                                         sqltest, broken)
    mac_counts = fuzz_counts(mac_fuzz)
    mac_hash = line(mac_speed, "Verification Hash:")
    if len(mac_counts) != len(FUZZ) or not mac_hash or "0 errors" not in mac_fuzz:
        print("FAIL: SQLite's own programs did not pass on this Mac:\n%s\n%s"
              % (mac_fuzz[-800:], mac_speed[-400:]))
        return 1

    image = os.path.join(WORK, "sqltest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    for name, text in (("first.lua", FIRST), ("second.lua", SECOND)):
        with open(os.path.join(WORK, name), "w") as f:
            f.write(text)
    files = [(os.path.join(WORK, n), "/Home/apps/sql/" + n)
             for n in ("first.lua", "second.lua", "sqltest.elf")]
    files += [(os.path.join(src, "test", n), "/Home/apps/sql/" + n) for n in FUZZ]
    megabytes = sum(os.path.getsize(s) for s, _ in files) // (1 << 20)
    subprocess.run([LUA, KFS, "create", HOME_DISK, str(megabytes + 64)]
                   + ["%s:%s" % f for f in files], check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    fails = []
    said, took1 = boot(R, "/Home/apps/sql/first.lua", "sql-done: first", 420)
    counts = fuzz_counts(said)
    if counts != mac_counts:
        fails.append("fuzzcheck's tests differ from this Mac's:\n  Mac:    %s\n  Kosmos: %s"
                     % (mac_counts, counts))
    total = re.search(r"fuzzcheck: (\d+) errors out of (\d+) tests", said)
    if not total or total.group(1) != "0" or line(said, "sql-fuzzcheck:") != \
            "sql-fuzzcheck: exit 0":
        fails.append("fuzzcheck did not finish without errors: %s / %s"
                     % (total and total.group(0), line(said, "sql-fuzzcheck:")))
    guest_hash = line(said, "Verification Hash:")
    if guest_hash != mac_hash:
        fails.append("speedtest1's hash differs:\n  Mac:    %s\n  Kosmos: %s"
                     % (mac_hash, guest_hash))
    guest_locks = line(said, "sql-locks:")
    want_locks = mac_locks
    if guest_locks != want_locks or "second busy, then in, rows 3" not in (guest_locks or ""):
        fails.append("the lock is not as on this Mac:\n  Mac:    %s\n  Kosmos: %s"
                     % (want_locks, guest_locks))
    guest_keep = line(said, "sql-keep:")
    if guest_keep != mac_keep:
        fails.append("the rows written differ:\n  Mac:    %s\n  Kosmos: %s"
                     % (mac_keep, guest_keep))
    if fails:
        print("FAIL: " + "\nFAIL: ".join(fails))
        print("--- the guest's last words ---\n" + said[-1500:])
        return 1

    if broken == "keep":
        # The control: the database changed on the disk while the machine
        # was off; the second boot must not read what the first wrote.
        out = os.path.join(WORK, "kept.out")
        subprocess.run([LUA, KFS, "get", HOME_DISK, KEPT, out], check=True,
                       capture_output=True, cwd=ROOT)
        b = bytearray(open(out, "rb").read())
        b[len(b) - 100] ^= 0xff
        open(out, "wb").write(bytes(b))
        subprocess.run([LUA, KFS, "put", HOME_DISK, out, KEPT], check=True,
                       capture_output=True, cwd=ROOT)

    # The disk as the first boot left it, kept until this run passes - or
    # always, with KOSMOS_SQL_KEEP set: what a failed read after the reboot
    # is looked into with (`kfs.lua ls`), and how its files lie.
    kept_disk = os.path.join(ROOT, "build", "gate", "sql-first-boot-%s.img"
                             % os.environ.get("KOSMOS_SUITE", "sql"))
    shutil.copyfile(HOME_DISK, kept_disk)

    again, took2 = boot(R, "/Home/apps/sql/second.lua", "sql-done: second", 120)
    read = line(again, "sql-keep:")
    if read != mac_keep:
        listed = subprocess.run([LUA, KFS, "ls", kept_disk, "/Home/apps/sql"],
                                capture_output=True, text=True, cwd=ROOT)
        print("FAIL: the rows read after a reboot are not the rows written:\n"
              "  written: %s\n  read:    %s" % (mac_keep, read))
        print("  the disk as the first boot left it is %s; /Home/apps/sql on it:\n    %s"
              % (kept_disk, (listed.stdout + listed.stderr).strip().replace("\n", "\n    ")))
        print("--- the guest's last words ---\n" + again[-1500:])
        return 1

    if not os.environ.get("KOSMOS_SQL_KEEP"):
        os.remove(kept_disk)
    print("PASS: SQLite %s on Kosmos - fuzzcheck's %s tests over %d databases without an "
          "error, speedtest1's hash as on this Mac, the dotfile lock refusing a second "
          "writer and letting it in after, and %s across a reboot (%.0f s + %.0f s)"
          % (re.search(r"SQLite (\S+)", said).group(1) if "SQLite " in said else "?",
             total.group(2), len(FUZZ), mac_keep.split(", integrity")[0][len("sql-keep: "):],
             took1, took2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
