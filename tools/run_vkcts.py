#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Khronos's Vulkan conformance suite on Kosmos (`docs/gpu3d.md` G2).

`vkcts.elf` - dEQP-VK's `info` and `api` groups from VK-GL-CTS 1.4.7.1
(`tools/cts_build.py`), with lavapipe on llvmpipe and LLVM's JIT linked in -
run from a /Home of its own over the cases `CASES` names, chosen to fit a
minute. The suite judges every case itself, as it does on any platform;
this script holds the run to having come to its end, to having executed
the cases it was given, and to no case failing or losing the device. The
log the suite writes is read back from the disk, and a failing case is
named from it.

Usage: run_vkcts.py IMAGE [APPS]

`KOSMOS_VKCTS_CASES` runs other cases than `CASES` - a pattern of the
suite's own, `dEQP-VK.api.info.*` - `KOSMOS_VKCTS_SERIAL` keeps what the
guest said, and `KOSMOS_VKCTS_LOG` the suite's log.
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

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("vkcts")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

# **The minute**, measured group by group on this Mac (`testing.md`
# 18.926): about 4200 cases in about 47 s of the suite's. What is not here
# and why: `api.descriptor_pool`, six cases and 23 s; `api.info`, past two
# minutes, its formats' properties one by one; and `api.object_management`
# past `single`, whose `max_concurrent` cases allocate until the driver
# says out of memory - which on Kosmos it cannot say, the heap's pages
# being committed on first touch (`roadmap.md`). A pattern of the suite's
# own, several with commas, as `--deqp-case` takes them.
CASES = os.environ.get("KOSMOS_VKCTS_CASES", ",".join([
    "dEQP-VK.info.*",
    "dEQP-VK.api.smoke.*",
    "dEQP-VK.api.null_handle.*",
    "dEQP-VK.api.version_check.*",
    "dEQP-VK.api.get_device_proc_addr.*",
    "dEQP-VK.api.device_init.*",
    "dEQP-VK.api.object_management.single.*",
    "dEQP-VK.api.command_buffers.*",
    "dEQP-VK.api.buffer.*",
]))
NAMED = "the minute's %d groups" % len(CASES.split(",")) if "KOSMOS_VKCTS_CASES" not in os.environ else CASES
LOG = "/Home/apps/vkcts/vkcts.qpa"
WAIT = int(os.environ.get("KOSMOS_VKCTS_WAIT", "300"))

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: image vkcts.elf
local kit = use("vkcts.elf")
local started = sys.ticks()
local counted, why = kit.run({
    "--deqp-case=%s",
    "--deqp-log-filename=%s",
    "--deqp-archive-dir=/Home/apps/vkcts",
    "--deqp-log-images=disable",
    "--deqp-log-shader-sources=disable",
    "--deqp-shadercache=disable",
})
local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 0
local ms = hz > 0 and (sys.ticks() - started) * 1000 // hz or -1
if not counted then
    print("vkcts: stopped: " .. tostring(why))
else
    print(string.format("vkcts: executed %%d passed %%d failed %%d not_supported %%d"
        .. " warnings %%d waived %%d device_lost %%d complete %%s ms %%d",
        counted.executed, counted.passed, counted.failed, counted.not_supported,
        counted.warnings, counted.waived, counted.device_lost,
        tostring(counted.complete), ms))
end
""" % (CASES, LOG)

SAID = re.compile(r"vkcts: executed (\d+) passed (\d+) failed (\d+) not_supported (\d+)"
                  r" warnings (\d+) waived (\d+) device_lost (\d+) complete (\w+) ms (-?\d+)")


def failures(qpa):
    """Each case the log says did not pass or was not supported, by name."""
    named = []
    case = None
    for line in qpa.splitlines():
        if line.startswith("#beginTestCaseResult "):
            case = line.split(" ", 1)[1].strip()
        m = re.search(r'<Result StatusCode="([^"]+)"', line)
        if m and case and m.group(1) not in ("Pass", "NotSupported", "QualityWarning",
                                             "CompatibilityWarning", "Waiver"):
            named.append("%s (%s)" % (case, m.group(1)))
            case = None
    return named


def main():
    built = os.path.join(APPS, "vkcts.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` builds it" % built)
        return 1

    image = os.path.join(WORK, "vkcts.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "vkcts.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 48)]
                   + ["%s:/Home/apps/vkcts/%s" % (os.path.join(WORK, n), n)
                      for n in ("vkcts.lua", "vkcts.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    # **A processor with SSE4.1, and two gigabytes** (`testing.md` 18.926).
    # llvmpipe compiles for the processor it finds, and on QEMU's default
    # `qemu64` - SSE2 and nothing after - its shaders wrote a colour of a
    # thousand million where they meant one, which a suite of real cases
    # reads as a failure of lavapipe's rather than of an old processor no
    # machine of Kosmos's has. And the command buffers' cases record
    # thousands of draws into one buffer: on the harness's 512 MB the heap
    # met the kernel's reserve, and a page refused on first touch ends the
    # process - a malloc that cannot say NULL (`roadmap.md`). QEMU takes the
    # last `-m` it is given.
    R.extra_args(IMAGE, ["-cpu", os.environ.get("KOSMOS_VKCTS_CPU", "max"),
                         "-m", os.environ.get("KOSMOS_VKCTS_MEMORY", "2G")])
    guest = R.Guest(IMAGE, 180)
    said = None

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/vkcts/vkcts.lua")

        died = False
        while time.monotonic() < started + WAIT and said is None and not died:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("vkcts: "):
                    said = line.strip()
                # A fault ends the process, and the kernel says where:
                # nothing more is coming, so the wait ends with it.
                died = died or ("(vkcts) ended" in line)
            if said is None and not died:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_VKCTS_SERIAL"):
        with open(os.environ["KOSMOS_VKCTS_SERIAL"], "w") as f:
            f.write(guest.seen)

    qpa_file = os.path.join(WORK, "vkcts.qpa")
    if os.path.exists(qpa_file):
        os.remove(qpa_file)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "get", HOME_DISK, LOG, qpa_file],
                   capture_output=True, cwd=ROOT)
    qpa = open(qpa_file, errors="replace").read() if os.path.exists(qpa_file) else ""
    if os.environ.get("KOSMOS_VKCTS_LOG"):
        with open(os.environ["KOSMOS_VKCTS_LOG"], "w") as f:
            f.write(qpa)

    m = SAID.match(said or "")
    if not m:
        print("FAIL: the conformance suite on Kosmos did not finish: %s" % (said or "nothing said"))
        print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    executed, passed, failed, unsupported, warnings, waived, lost = map(int, m.groups()[:7])
    complete, ms = m.group(8) == "true", int(m.group(9))
    wrong = []
    if not complete:
        wrong.append("the run did not come to its end")
    if executed == 0:
        wrong.append("no case was executed")
    if failed or lost:
        wrong.append("%d failed and %d lost the device: %s"
                     % (failed, lost, ", ".join(failures(qpa)[:12]) or "the log names none"))
    if passed + unsupported + warnings + waived + failed + lost != executed:
        wrong.append("the counts do not add up to the %d executed" % executed)
    if wrong:
        print("FAIL: dEQP-VK on Kosmos, %s: %s" % (NAMED, "; ".join(wrong)))
        print("  said: %s" % said)
        return 1

    print("PASS: dEQP-VK on Kosmos, %s, from a %.1f MB image, in %.1f s (%.1f s in the suite):"
          " %d cases executed, %d passed, %d not supported, %d with a warning, none failed"
          % (NAMED, os.path.getsize(image) / 1e6, took, ms / 1000.0, executed, passed,
             unsupported, warnings))
    return 0


if __name__ == "__main__":
    sys.exit(main())
