#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5d's OpenSSL on Kosmos (`docs/w5.md`).

`ssltest.elf` - OpenSSL 4.1.0's libraries with three of its own test
programs - booted once, on a `/Home` holding the files those programs read,
in the layout of OpenSSL's tree, and each program run from the prompt as a
process of its own (`user/kits/ssltest/ssltest_kit.c` says why):

- `evp`: `evp_test` over OpenSSL's vector files, every cipher, digest, MAC,
  KDF and public-key algorithm against the answers its file gives; in runs
  of at most 90 files, the most one run takes (`test/testutil/options.c`).
- `bn`: `bntest` over its six files, and again with none, for its own
  cases.
- `ssl`: `sslapitest`, a client and a server speaking TLS to each other in
  memory, over OpenSSL's test certificates.

Each run's TAP - every `ok` and `not ok`, and each file's `Completed` count
- is held to what the same program says on this Mac, built here from the
same release with the same options (`tools/w5_build.py host openssl`), and
every run has to end in 0.

Usage: run_ssl.py IMAGE [APPS]

`KOSMOS_SSL_PART` chooses what runs, so the gate can run it in parts side
by side: `evp` (the vectors but the two slowest), `slow` (SLH-DSA's signing
and key generation, a few seconds here and minutes under TCG), `ssl`
(`bntest` and `sslapitest`), or `all`, the default.

`KOSMOS_SSL_BREAK` is the control: `vector` changes one expected answer in
a vector file on the guest's disk, `cert` changes a byte in the test server's
certificate; the guest must disagree either way.
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
PART = os.environ.get("KOSMOS_SSL_PART", "all")
WORK = scratch.directory("ssl-" + PART)
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")
GUEST = "/Home/apps/ssl"
EVP = "test/recipes/30-test_evp_data"
BN = "test/recipes/10-test_bn_data"
SSLAPI = "test/recipes/90-test_sslapi_data"
CONFIG = "test/default-and-legacy.cnf"
SLOW = ("evppkey_slh_dsa_siggen.txt", "evppkey_slh_dsa_keygen.txt")
# **Files whose ciphers exist per processor**: the stitched AES-CBC-HMAC
# ones are OpenSSL's x86-64 assembly beside AES-NI, and their `-ETM`
# variants its AArch64 assembly alone, so this Mac and an x86-64 guest each
# run the half the other skips. Here a file's tests and skips may be split
# differently, but never their sum, and never an error.
PER_PROCESSOR = ("evpciph_aes_stitched.txt",)
RUN_FILES = 90

TESTUTIL = ("basic_output output driver tests cb stanza format_output load fake_random "
            "test_cleanup main testutil_init options test_options provider apps_shims "
            "random helper compare").split()
PROGRAMS = {
    "evp": ("evp_test", ["evp_test.c"]),
    "bn": ("bntest", ["bntest.c"]),
    "ssl": ("sslapitest", ["sslapitest.c", "helpers/ssltestlib.c", "filterprov.c",
                           "tls-provider.c"]),
}

SCRIPT = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- One of W5d's OpenSSL test programs (`tools/run_ssl.py`), run in the
-- folder holding OpenSSL's `test/`.
-- kosmos: image ssltest.elf
local kit = use("ssltest.elf")
local code, said, spilled = kit.run(%s, {%s}, %s)
write(said)
if spilled ~= "" then write(spilled) end
print("ssl-run %s: exit " .. code)
"""


def results(text):
    """The lines that say how a run went (`ssltest_kit.c`'s `result_line`,
    less its error lines, which carry the paths of this Mac or the guest)."""
    kept = []
    for line in text.splitlines():
        m = re.match(r"^( *)(not ok |ok |# Completed|1\.\.)", line)
        if m and (len(m.group(1)) <= 4 or m.group(2) == "not ok "):
            kept.append(line.rstrip())
    return kept


def same(mac_lines, guest_lines, files):
    """Whether a run's lines are this Mac's, allowing `PER_PROCESSOR`'s.

    The `Completed` lines come one a file, in the order the run names them.
    """
    if len(mac_lines) != len(guest_lines):
        return False
    completed = 0
    for m, g in zip(mac_lines, guest_lines):
        if m == g:
            completed += "# Completed" in m
            continue
        mm = re.search(r"# Completed (\d+) tests with 0 errors and (\d+) skipped", m)
        gm = re.search(r"# Completed (\d+) tests with 0 errors and (\d+) skipped", g)
        if not (mm and gm and completed < len(files)
                and os.path.basename(files[completed]) in PER_PROCESSOR
                and int(mm.group(1)) + int(mm.group(2)) == int(gm.group(1)) + int(gm.group(2))):
            return False
        completed += 1
    return True


def runs():
    """What runs: (name, program, argument list), the paths the guest's."""
    src = upstream.where("openssl")
    vectors = sorted(os.listdir(os.path.join(src, EVP)))
    planned = []
    if PART in ("all", "evp"):
        chosen = [v for v in vectors if v not in SLOW]
        for i in range(0, len(chosen), RUN_FILES):
            planned.append(("evp%d" % (i // RUN_FILES + 1), "evp",
                            ["-config", CONFIG] + ["%s/%s" % (EVP, v)
                                                   for v in chosen[i:i + RUN_FILES]]))
    if PART in ("all", "slow"):
        planned.append(("slow", "evp", ["-config", CONFIG] + ["%s/%s" % (EVP, v) for v in SLOW]))
    if PART in ("all", "ssl"):
        planned.append(("bnfiles", "bn", ["%s/%s" % (BN, b)
                                          for b in sorted(os.listdir(os.path.join(src, BN)))]))
        planned.append(("bn", "bn", []))
        planned.append(("ssl", "ssl", ["test/certs", SSLAPI + "/passwd.txt",
                                       "/Temporary/sslapi-state.txt", "default",
                                       "test/default.cnf", SSLAPI + "/dhparams.pem", SSLAPI]))
    return planned


def mac(planned):
    """OpenSSL's tests built for this Mac, and what each planned run says."""
    subprocess.run(["python3", os.path.join(HERE, "w5_build.py"), "host", "zlib", "openssl"],
                   check=True, capture_output=True)
    src = upstream.where("openssl")
    build = os.path.join(ROOT, "build", "w5", "host", "openssl")
    lib = os.path.join(ROOT, "build", "w5", "host", "install", "lib")
    made = os.path.join(WORK, "mac")
    os.makedirs(made, exist_ok=True)
    flags = ["-O1", "-w"] + ["-I" + p for p in (
        os.path.join(build, "include"), os.path.join(build, "apps", "include"),
        os.path.join(src, "include"), os.path.join(src, "apps", "include"),
        os.path.join(src, "providers", "common", "include"), src,
        os.path.join(src, "test"), os.path.join(src, "test", "mfail"))]

    # The test library as OpenSSL builds it, an archive: a program's own
    # `test_get_options` and `cleanup_tests` win over its defaults.
    objects = []
    for name in TESTUTIL:
        o = os.path.join(made, "testutil-%s.o" % name)
        subprocess.run(["/usr/bin/clang", "-c"] + flags +
                       [os.path.join(src, "test", "testutil", name + ".c"), "-o", o], check=True)
        objects.append(o)
    for name, path in (("mfail", "test/mfail/mfail.c"), ("opt", "apps/lib/opt.c")):
        o = os.path.join(made, name + ".o")
        subprocess.run(["/usr/bin/clang", "-c"] + flags + [os.path.join(src, path), "-o", o],
                       check=True)
        objects.append(o)
    testutil = os.path.join(made, "libtestutil.a")
    if os.path.exists(testutil):
        os.remove(testutil)
    subprocess.run(["ar", "rcs", testutil] + objects, check=True)

    wanted = {program for _, program, _ in planned}
    for program in wanted:
        binary, sources = PROGRAMS[program]
        subprocess.run(["/usr/bin/clang"] + flags + [os.path.join(src, "test", s) for s in sources]
                       + [testutil] + [os.path.join(lib, n) for n in
                                       ("libssl.a", "libcrypto.a", "libz.a")]
                       + ["-o", os.path.join(made, binary)], check=True)

    said = {}
    for name, program, args in planned:
        args = [a.replace("/Temporary/", made + "/") for a in args]
        # One stream, as the guest's file is (`ssltest_kit.c`): the counts
        # go to stderr and the results to stdout, and their order is the
        # order they were written.
        r = subprocess.run([os.path.join(made, PROGRAMS[program][0])] + args, cwd=src,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        said[name] = (r.returncode, results(r.stdout))
    return said


def boot(R, planned, seconds):
    guest = R.Guest(IMAGE, seconds)
    times = {}
    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        prompts = guest.seen.count("kosmos> ")
        guest.type("cd " + GUEST)
        deadline = time.monotonic() + 30
        while guest.seen.count("kosmos> ") == prompts and time.monotonic() < deadline:
            time.sleep(0.1)
        mark = len(guest.seen)
        started = time.monotonic()
        ended = re.compile(r"process \d+ \(\w+\) ended")
        for name, _, _ in planned:
            began = time.monotonic()
            at = len(guest.seen)
            guest.type("run %s.lua" % name)
            while time.monotonic() < started + seconds \
                    and "ssl-run %s: exit" % name not in guest.seen[at:] \
                    and not ended.search(guest.seen, at):
                time.sleep(0.2)
                guest._read_available()
            times[name] = time.monotonic() - began
            if "ssl-run %s: exit" % name not in guest.seen[at:]:
                break
            # The next prompt, before the next command is typed.
            prompts = guest.seen.count("kosmos> ")
            deadline = time.monotonic() + 10
            while guest.seen[at:].count("kosmos> ") == 0 and time.monotonic() < deadline:
                time.sleep(0.1)
                guest._read_available()
        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()
    if os.environ.get("KOSMOS_SSL_SERIAL"):
        with open(os.environ["KOSMOS_SSL_SERIAL"], "w") as f:
            f.write(guest.seen)
    return guest.seen[mark:].replace("\r", ""), times


def main():
    built = os.path.join(APPS, "ssltest.elf")
    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1
    broken = os.environ.get("KOSMOS_SSL_BREAK")
    if broken not in (None, "vector", "cert"):
        print("FAIL: KOSMOS_SSL_BREAK is vector or cert, not %s" % broken)
        return 1
    if PART not in ("all", "evp", "slow", "ssl"):
        print("FAIL: KOSMOS_SSL_PART is all, evp, slow or ssl, not %s" % PART)
        return 1

    upstream.unpack("openssl")
    src = upstream.where("openssl")
    planned = runs()
    mac_said = mac(planned)
    for name, (code, lines) in mac_said.items():
        if code != 0 or not lines or any(ln.lstrip().startswith("not ok") for ln in lines):
            print("FAIL: OpenSSL's own %s run did not pass on this Mac (exit %d):\n%s"
                  % (name, code, "\n".join(lines[-20:])))
            return 1

    image = os.path.join(WORK, "ssltest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    files = [(image, GUEST + "/ssltest.elf")]
    for name, program, args in planned:
        script = os.path.join(WORK, name + ".lua")
        with open(script, "w") as f:
            f.write(SCRIPT % ('"%s"' % program, ", ".join('"%s"' % a for a in args),
                              '"/Temporary/%s.log"' % name, name))
        files.append((script, GUEST + "/" + name + ".lua"))

    # What the runs read, where OpenSSL's tree has it: the vector files the
    # runs name, the big numbers', the certificates and the configurations.
    named = {a for _, _, args in planned for a in args if a.startswith("test/")}
    for path in sorted(named):
        whole = os.path.join(src, path)
        if os.path.isfile(whole):
            files.append((whole, GUEST + "/" + path))
    if PART in ("all", "ssl"):
        for top in ("test/certs", SSLAPI):
            for folder, _, names in os.walk(os.path.join(src, top)):
                for n in sorted(names):
                    whole = os.path.join(folder, n)
                    files.append((whole, GUEST + "/" + os.path.relpath(whole, src)))
    files = list(dict.fromkeys(files))

    if broken is not None:
        # The control: one input the guest reads changed, the Mac's not.
        changed = os.path.join(WORK, "changed")
        if broken == "vector":
            want = os.path.join(src, EVP, "evpmd_sha.txt")
            text = open(want).read()
            at = text.index("Output = ") + len("Output = ")
            data = (text[:at] + ("0" if text[at] != "0" else "1") + text[at + 1:]).encode()
            if PART not in ("all", "evp"):
                print("FAIL: KOSMOS_SSL_BREAK=vector needs KOSMOS_SSL_PART evp or all")
                return 1
        else:
            # The server's certificate, which every handshake sends. Not the
            # test root, which `sslapitest` never opens, and not the server's
            # key: a byte changed halfway down it is in a part signing never
            # reads, and this Mac's own run passed with it.
            want = os.path.join(src, "test", "certs", "servercert.pem")
            lines = open(want, "rb").read().splitlines(True)
            middle = len(lines) // 2
            lines[middle] = bytes([lines[middle][0] ^ 0x01]) + lines[middle][1:]
            data = b"".join(lines)
            if PART not in ("all", "ssl"):
                print("FAIL: KOSMOS_SSL_BREAK=cert needs KOSMOS_SSL_PART ssl or all")
                return 1
        open(changed, "wb").write(data)
        files = [(changed if s == want else s, g) for s, g in files]

    # kfs gives a disk one inode for every 64 KB (`kfs_mkfs`): sized by the
    # files and their folders as well as by their bytes, as run_xml.py's is.
    folders = {os.path.dirname(g) for _, g in files}
    megabytes = max(sum(os.path.getsize(s) for s, _ in files) // (1 << 20) * 2 + 64,
                    (len(files) + len(folders) + 512) // 16)
    subprocess.run([LUA, KFS, "create", HOME_DISK, str(megabytes)]
                   + ["%s:%s" % f for f in files], check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    # **The processor the machines have**, not QEMU's default: `qemu64` has
    # no AES-NI, no SHA, no AVX, so OpenSSL chose its plain C for every
    # cipher and the assembly the M700, the T14 and the Mac Pro run was
    # never held to a vector - and the stitched AES-CBC-HMAC ciphers, which
    # exist only beside AES-NI, skipped every test. `max` is everything
    # TCG emulates, and OpenSSL picks its paths from CPUID as it would there.
    if X86:
        R.extra_args(IMAGE, ["-cpu", "max"])

    said, times = boot(R, planned, 540)
    fails = []
    for name, _, _ in planned:
        marker = "ssl-run %s: exit" % name
        if marker not in said:
            fails.append("%s: did not finish (%.0f s)" % (name, times.get(name, 0)))
            continue
        end = said.index(marker)
        begin = said.rfind("run %s.lua" % name, 0, end)
        guest_lines = results(said[begin:end])
        code = re.search(re.escape(marker) + r" (-?\d+)", said).group(1)
        mac_code, mac_lines = mac_said[name]
        files = [a for a in dict((n, a) for n, _, a in planned)[name] if a.endswith(".txt")]
        if not same(mac_lines, guest_lines, files) or code != "0":
            differ = [(m, g) for m, g in zip(mac_lines, guest_lines) if m != g]
            fails.append("%s: exit %s, %d result lines against this Mac's %d, first difference:"
                         "\n  Mac:    %s\n  Kosmos: %s"
                         % (name, code, len(guest_lines), len(mac_lines),
                            differ[0][0] if differ else "-", differ[0][1] if differ else "-"))
    if fails:
        print("FAIL: " + "\nFAIL: ".join(fails))
        print("--- the guest's last words ---\n" + said[-2500:])
        return 1

    completed = sum(int(m.group(1)) for _, (_, lines) in mac_said.items() for ln in lines
                    for m in [re.search(r"# Completed (\d+) tests", ln)] if m)
    print("PASS: OpenSSL 4.1.0 on Kosmos, part %s - %s, each as on this Mac; %d vectors (%s)"
          % (PART, ", ".join("%s %.0f s" % (n, times[n]) for n, _, _ in planned), completed,
             "all runs ended 0"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
