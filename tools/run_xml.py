#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5c's libxml2 and libpsl on Kosmos (`docs/w5.md`).

`xmltest.elf` - libxml2 2.15.4 with its own `runtest.c`, and libpsl
0.23.3 with its own five tests - booted once, on a `/Home` holding
libxml2's `test/` and `result/` trees and libpsl's list and tests:

1. `runtest -v`, in the folder with the trees: every one of its sections,
   each one's count of tests and its errors and leaks, held to what the same
   program says on this Mac, and its total with no errors.
2. libpsl's five tests, each one's count held to this Mac's, all passed.

This Mac's programs are built here from the same releases, libxml2 with
the same options (`tools/w5_build.py host libxml2`), so a difference is
Kosmos's.

Usage: run_xml.py IMAGE [APPS]

`KOSMOS_XML_BREAK` is the control: `result` changes a byte in one of
libxml2's expected results on the guest's disk, `psl` takes half of
libpsl's tests away from the guest's copy; the guest must disagree either
way.
"""

import os
import re
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
WORK = scratch.directory("xml")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")
GUEST = "/Home/apps/xml"
PSL_TESTS = [("is-public", "test-is-public"), ("is-public-all", "test-is-public-all"),
             ("cookie-domain", "test-is-cookie-domain-acceptable"),
             ("is-public-builtin", "test-is-public-builtin"),
             ("registrable-domain", "test-registrable-domain")]

FIRST = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5c's libxml2 and libpsl test (`tools/run_xml.py`), run in the folder
-- holding libxml2's `test/` and `result/`.
-- kosmos: image xmltest.elf
local kit = use("xmltest.elf")
local code, said = kit.runtest({"-v", "--out", "/Temporary/"})
write(said)
print("xml-runtest: exit " .. code)
for _, name in ipairs({%s}) do
  code, said = kit.psl(name, {})
  write(said)
  print("xml-psl " .. name .. ": exit " .. code)
end
print("xml-done")
""" % ", ".join('"%s"' % n for n, _ in PSL_TESTS)


def sections(text):
    """`runtest -v`'s sections, each with what it ran and found."""
    return re.findall(r"^## (.+)\n(Ran \d+ tests, .+)$", text, re.M)


def psl_counts(text):
    return re.findall(r"^Summary: (.+)$", text, re.M)


def mac():
    """libxml2 and libpsl built for this Mac, and what their tests say."""
    xml = upstream.where("libxml2")
    psl = upstream.where("libpsl")
    subprocess.run(["python3", os.path.join(HERE, "w5_build.py"), "host", "libxml2"],
                   check=True, capture_output=True)
    host = os.path.join(ROOT, "build", "w5", "host")
    made = os.path.join(WORK, "mac")
    os.makedirs(os.path.join(made, "out"), exist_ok=True)

    runtest = os.path.join(made, "runtest")
    subprocess.run(["/usr/bin/clang", "-O2", "-w", "-I" + os.path.join(host, "libxml2"),
                    "-I" + os.path.join(xml, "include"), "-I" + xml,
                    os.path.join(xml, "runtest.c"),
                    os.path.join(host, "install", "lib", "libxml2.a"), "-lm", "-o", runtest],
                   check=True)
    said = subprocess.run([runtest, "-v", "--out", os.path.join(made, "out") + "/"], cwd=xml,
                          capture_output=True, text=True)

    counts = []
    for _, source in PSL_TESTS:
        program = os.path.join(made, source)
        subprocess.run(["/usr/bin/clang", "-O2", "-w", "-DENABLE_BUILTIN",
                        '-DPACKAGE_VERSION="0.23.3"', '-DPSL_DISTFILE=""',
                        '-DPSL_FILE="%s/list/public_suffix_list.dat"' % psl,
                        '-DPSL_TESTFILE="%s/list/tests/tests.txt"' % psl,
                        '-DPSL_DAFSA="%s/tests/psl.dafsa"' % psl,
                        '-DPSL_ASCII_DAFSA="%s/tests/psl_ascii.dafsa"' % psl,
                        '-DSRCDIR="%s/tests"' % psl,
                        "-I" + os.path.join(psl, "include"), "-I" + os.path.join(psl, "src"),
                        os.path.join(psl, "src", "psl.c"),
                        os.path.join(psl, "src", "lookup_string_in_fixed_set.c"),
                        os.path.join(psl, "tests", "common.c"),
                        os.path.join(psl, "tests", source + ".c"), "-o", program], check=True)
        r = subprocess.run([program], capture_output=True, text=True)
        counts.append(psl_counts(r.stdout + r.stderr))
    return said.stdout + said.stderr, counts


def boot(R, seconds):
    guest = R.Guest(IMAGE, seconds)
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        # Into the folder with the trees, which `runtest` reads as `./test`
        # and `./result`: a program starts where the shell is. The next
        # prompt says the shell is there.
        prompts = guest.seen.count("kosmos> ")
        guest.type("cd " + GUEST)
        deadline = time.monotonic() + 30
        while guest.seen.count("kosmos> ") == prompts and time.monotonic() < deadline:
            time.sleep(0.1)
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run first.lua")
        ended = re.compile(r"process \d+ \(first\) ended")
        while time.monotonic() < started + seconds and "xml-done" not in guest.seen[mark:] \
                and not ended.search(guest.seen, mark):
            time.sleep(0.2)
            guest._read_available()
        time.sleep(0.5)
        guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()
    if os.environ.get("KOSMOS_XML_SERIAL"):
        with open(os.environ["KOSMOS_XML_SERIAL"], "w") as f:
            f.write(guest.seen)
    return guest.seen[mark:].replace("\r", ""), took


def main():
    built = os.path.join(APPS, "xmltest.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1
    broken = os.environ.get("KOSMOS_XML_BREAK")
    if broken not in (None, "result", "psl"):
        print("FAIL: KOSMOS_XML_BREAK is result or psl, not %s" % broken)
        return 1

    upstream.unpack("libxml2")
    upstream.unpack("libpsl")
    mac_said, mac_psl = mac()
    mac_sections = sections(mac_said)
    if not mac_sections or "tests, no errors" not in (re.findall(r"^Total .+$", mac_said, re.M)
                                                       or [""])[-1] \
            or any(len(c) != 1 or not c[0].startswith("All ") for c in mac_psl):
        print("FAIL: the libraries' own tests did not pass on this Mac:\n%s\n%s"
              % (mac_said[-800:], mac_psl))
        return 1

    xml = upstream.where("libxml2")
    psl = upstream.where("libpsl")
    image = os.path.join(WORK, "xmltest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "first.lua"), "w") as f:
        f.write(FIRST)

    files = [(os.path.join(WORK, n), GUEST + "/" + n) for n in ("first.lua", "xmltest.elf")]
    for top in ("test", "result"):
        for folder, _, names in os.walk(os.path.join(xml, top)):
            for n in sorted(names):
                path = os.path.join(folder, n)
                files.append((path, GUEST + "/" + os.path.relpath(path, xml)))
    for source, name in ((os.path.join(psl, "list", "public_suffix_list.dat"), None),
                         (os.path.join(psl, "list", "tests", "tests.txt"), None),
                         (os.path.join(psl, "tests", "psl.dafsa"), None),
                         (os.path.join(psl, "tests", "psl_ascii.dafsa"), None)):
        files.append((source, GUEST + "/psl/" + (name or os.path.basename(source))))

    if broken is not None:
        # The control: one input the guest reads changed, the Mac's not.
        changed = os.path.join(WORK, "changed")
        if broken == "result":
            want = os.path.join(xml, "result", "att1")
            data = bytearray(open(want, "rb").read())
            data[len(data) // 2] ^= 0x01
        else:
            want = os.path.join(psl, "list", "tests", "tests.txt")
            lines = open(want, "rb").read().splitlines(True)
            data = b"".join(ln for i, ln in enumerate(lines) if i % 2 == 0)
        open(changed, "wb").write(bytes(data))
        files = [(changed if s == want else s, g) for s, g in files]

    # kfs gives a disk one inode for every 64 KB (`kfs_mkfs`), and libxml2's
    # trees are 3,500 small files: the disk is sized by how many there are
    # and their folders, sixteen to a megabyte, as well as by their bytes.
    folders = {os.path.dirname(g) for _, g in files}
    megabytes = max(sum(os.path.getsize(s) for s, _ in files) // (1 << 20) * 2 + 64,
                    (len(files) + len(folders) + 512) // 16)
    subprocess.run([LUA, KFS, "create", HOME_DISK, str(megabytes)]
                   + ["%s:%s" % f for f in files], check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    said, took = boot(R, 420)
    fails = []
    guest_sections = sections(said)
    if guest_sections != mac_sections:
        differ = [(m, g) for m, g in zip(mac_sections, guest_sections) if m != g]
        fails.append("runtest's sections differ from this Mac's (%d against %d), first:\n"
                     "  Mac:    %s\n  Kosmos: %s"
                     % (len(mac_sections), len(guest_sections),
                        differ[0][0] if differ else "-", differ[0][1] if differ else "-"))
    total = re.findall(r"^Total .+$", said, re.M)
    if not total or not total[-1].endswith("tests, no errors") \
            or "xml-runtest: exit 0" not in said:
        fails.append("runtest did not finish without errors: %s"
                     % (total[-1] if total else "no total"))
    for (name, _), want in zip(PSL_TESTS, mac_psl):
        # Each test's summary is the last one before its exit line.
        before = said[:said.find("xml-psl %s: exit" % name)]
        summary = psl_counts(before)[-1:] if "xml-psl %s: exit" % name in said else []
        if summary != want or "xml-psl %s: exit 0" % name not in said:
            fails.append("libpsl's %s: %s, on this Mac %s"
                         % (name, summary or "nothing", want))
    if fails:
        print("FAIL: " + "\nFAIL: ".join(fails))
        print("--- the guest's last words ---\n" + said[-1500:])
        return 1

    print("PASS: libxml2 2.15.4 on Kosmos - runtest's %d sections as on this Mac, %s; "
          "and libpsl 0.23.3's five tests as on this Mac, %s tests (%.0f s)"
          % (len(guest_sections), total[-1],
             sum(int(re.search(r"(\d+)", c[0]).group(1)) for c in mac_psl), took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
