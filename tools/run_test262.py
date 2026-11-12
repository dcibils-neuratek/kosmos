#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""test262's language tests, run by JavaScriptCore on Kosmos (`docs/w4.md` W4e).

    python3 tools/run_test262.py IMAGE APPS [--shards N] [--only PATTERN]
                                 [--update]

**Not part of `make test`** (Diego, 12 November 2026): 23,832 tests, most
of them run twice - once in strict mode, once without - is far past the
gate's ten minutes under QEMU. `make test262` runs it, and it is held to
what it was before: `bench/test262.json` keeps the commit of test262 it
was counted against, how many passed, and which failed. A run fails when a
test that passed before fails now - fewer passes is the same failure
counted - and says which tests newly pass; `--update` writes what this run
counted as the new baseline, which is how a step forward is kept.

What is run, and what is not:

- every test under `test/language/` except the fixtures other tests load;
- **not modules** (`flags: [module]`), counted as not run: JavaScriptCore's
  C API evaluates scripts, and a module needs a loader the driver does not
  have yet;
- the harness's files once per bundle, composed with each test in the guest
  (`user/installed/JSC/jsc_kosmos.cpp`), as test262's own runners do:
  `assert.js`, `sta.js`, `doneprintHandle.js` for an `async` test, then the
  test's `includes`, then the test, with `"use strict";` first in strict
  mode.

The tests are split into shards, each a guest of its own with a /Home of
its own, run side by side: the Nth test of every `--shards` goes to the
same guest, so slow directories spread. A guest whose test brings the
program down is told to carry on from the next, and one that says nothing
for `STALL` seconds is booted again past the test it was on; either way the
test is counted as failed, with what happened as its reason.
"""

import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import upstream                                              # noqa: E402

COMMIT = upstream.TEST262["commit"]
BASELINE = os.path.join(ROOT, "bench", "test262.json")
LUA = os.path.join(ROOT, "build", "host", "lua")
STALL = 150          # seconds a guest may print nothing, mid-test
RESULTS = os.path.join(ROOT, "build", "test262")
# The guests' memory: a gigabyte, not the 512 MB every other suite boots.
# Three tests compile 65,536 regular expressions each through `eval`, and in
# 512 MB the engine ran out and the program ended (`testing.md` 18.780).
MEMORY = os.environ.get("KOSMOS_T262_MEMORY", "1G")

ONLY_STRICT, NO_STRICT, RAW, ASYNC = 1, 2, 4, 8

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- test262's runner in the guest (`tools/run_test262.py`): each case of the
-- bundle from the one given, and a line for each.
-- kosmos: image jsc.elf
local jsc = use("jsc.elf")
local first = tonumber(args) or 1
local count, why = jsc.t262("/Home/t262/bundle")

if not count then
  print("t262: " .. tostring(why))
  return
end

for i = first, count do
  local ok, reason = jsc.case(i)
  print(ok and ("@" .. i .. " P") or ("@" .. i .. " F " .. reason))
end
print("t262: done " .. count)
"""


def front_matter(source):
    """What a test's `/*--- ... ---*/` says, as much of YAML as test262 uses:
    `key: value`, `key: [a, b]`, a list of `- item` lines, and `negative`'s
    two keys beneath it."""
    start = source.find("/*---")
    end = source.find("---*/", start)
    meta = {}

    if start < 0 or end < 0:
        return meta

    key = None
    for line in source[start + 5:end].split("\n"):
        stripped = line.strip()
        if not stripped:
            continue
        indented = line[:1] in (" ", "\t")
        if indented and key and stripped.startswith("- "):
            meta.setdefault(key, [])
            if isinstance(meta[key], list):
                meta[key].append(stripped[2:].strip())
            continue
        if indented and key == "negative" and ":" in stripped:
            k, v = stripped.split(":", 1)
            meta["negative"][k.strip()] = v.strip()
            continue
        if indented:
            continue
        m = re.match(r"([A-Za-z_]+):\s*(.*)$", line)
        if not m:
            continue
        key, value = m.group(1), m.group(2).strip()
        if key == "negative":
            meta["negative"] = {}
        elif value.startswith("["):
            meta[key] = [v.strip() for v in value.strip("[]").split(",") if v.strip()]
        elif value in ("", "|", ">"):
            meta[key] = []
        else:
            meta[key] = value
    return meta


def tests(root, only):
    """Every language test, in a fixed order: (name, source, meta)."""
    found = []
    base = os.path.join(root, "test", "language")

    for top, dirs, names in os.walk(base):
        dirs.sort()
        for n in sorted(names):
            if not n.endswith(".js") or "_FIXTURE" in n:
                continue
            path = os.path.join(top, n)
            name = os.path.relpath(path, os.path.join(root, "test"))
            if only and not re.search(only, name):
                continue
            with open(path, encoding="utf-8", errors="surrogateescape") as f:
                source = f.read()
            found.append((name, source, front_matter(source)))
    return found


def record(head, body):
    data = body.encode("utf-8", errors="surrogateescape")
    return ("\t".join(head + [str(len(data))]) + "\n").encode() + data + b"\n"


def bundle(root, cases, path):
    """The harness's files and these cases, in the format the driver reads."""
    used = {"assert.js", "sta.js", "doneprintHandle.js"}
    for _, _, meta in cases:
        used.update(meta.get("includes", []))

    with open(path, "wb") as f:
        for name in sorted(used):
            with open(os.path.join(root, "harness", name), encoding="utf-8") as h:
                f.write(record(["H", name], h.read()))
        for name, source, meta in cases:
            flags = meta.get("flags", [])
            bits = ((ONLY_STRICT if "onlyStrict" in flags else 0)
                    | (NO_STRICT if "noStrict" in flags else 0)
                    | (RAW if "raw" in flags else 0)
                    | (ASYNC if "async" in flags else 0))
            negative = meta.get("negative", {})
            f.write(record(["T", name, str(bits),
                            negative.get("phase") or "-", negative.get("type") or "-",
                            ",".join(meta.get("includes", [])) or "-"], source))


class Shard(threading.Thread):
    """One guest, its share of the cases, and what each did."""

    def __init__(self, number, image, elf, cases):
        super().__init__(daemon=True)
        self.number = number
        self.image = image
        self.cases = cases
        self.verdicts = {}               # index from 1 -> None (passed) or why
        self.work = scratch.directory("t262-%d" % number)
        self.disk = os.path.join(self.work, "home.img")
        self.log = os.path.join(RESULTS, "shard-%d.log" % number)   # kept, for a failure
        self.error = None

        program = os.path.join(self.work, "run.lua")
        with open(program, "w") as f:
            f.write(PROGRAM)
        data = os.path.join(self.work, "bundle")
        bundle(upstream.test262(), cases, data)

        megabytes = (os.path.getsize(elf) + os.path.getsize(data)) // (1 << 20)
        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", self.disk,
                        str(megabytes + 32),
                        "%s:/Home/t262/jsc.elf" % elf,
                        "%s:/Home/t262/bundle" % data,
                        "%s:/Home/t262/run.lua" % program],
                       check=True, capture_output=True, cwd=ROOT)

    def boot(self):
        """A guest of this shard's own, in a process of its own: the harness
        reads its disk from the environment when it is imported."""
        child = subprocess.Popen(
            [sys.executable, "-c",
             "import sys; sys.path.insert(0, %r)\n"
             "import run_screenshot as R\n"
             "R.X86_ARGS = [%r if a == '512M' else a for a in R.X86_ARGS]\n"
             "R.QEMU_ARGS = [%r if a == '512M' else a for a in R.QEMU_ARGS]\n"
             "g = R.Guest(%r, 240)\n"
             "g.wait_for('kosmos> ', 'a prompt')\n"
             "sys.stdout.write('\\n<<booted>>\\n'); sys.stdout.flush()\n"
             "import select, os, time\n"
             "mark = len(g.seen)\n"
             "def pump():\n"
             "    global mark\n"
             "    if len(g.seen) > mark:\n"
             "        sys.stdout.write(g.seen[mark:]); sys.stdout.flush(); mark = len(g.seen)\n"
             "try:\n"
             "    while True:\n"
             "        r, _, _ = select.select([sys.stdin], [], [], 0.2)\n"
             "        if r:\n"
             "            line = sys.stdin.readline()\n"
             "            if not line: break\n"
             "            g.type(line.rstrip('\\n'))\n"
             "        pump()\n"
             "finally:\n"
             "    g.close()\n" % (HERE, MEMORY, MEMORY, self.image)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            env=dict(os.environ, KOSMOS_DISK=self.disk), bufsize=0)
        os.set_blocking(child.stdout.fileno(), False)
        return child

    def run(self):
        try:
            self._run()
        except Exception as e:                                # noqa: BLE001
            self.error = repr(e)

    def _run(self):
        count = len(self.cases)
        nxt = 1
        log = open(self.log, "w")

        while nxt <= count:
            child = self.boot()
            seen = ""
            booted = False
            last_heard = time.monotonic()
            ended = False

            while True:
                chunk = b""
                try:
                    chunk = child.stdout.read() or b""
                except BlockingIOError:
                    pass
                if chunk:
                    text = chunk.decode("utf-8", errors="replace")
                    log.write(text)
                    seen += text
                    last_heard = time.monotonic()

                if not booted and "<<booted>>" in seen:
                    booted = True
                    seen = seen.split("<<booted>>", 1)[1]
                    child.stdin.write(b"/Home/t262/run.lua %d\n" % nxt)
                    child.stdin.flush()

                # Whole lines only; the last may still be arriving.
                if booted and "\n" in seen:
                    lines, seen = seen.rsplit("\n", 1)
                    lines = lines.split("\n")
                    for line in lines:
                        for m in re.finditer(r"@(\d+) ([PF])(?: (.*))?", line):
                            i = int(m.group(1))
                            if 1 <= i <= count:
                                self.verdicts[i] = None if m.group(2) == "P" else (m.group(3) or "")
                                nxt = max(nxt, i + 1)
                        if "t262: done" in line:
                            nxt = count + 1
                            ended = True
                        elif line.startswith("t262: "):
                            raise RuntimeError("shard %d: %s" % (self.number, line))
                # The prompt again, with no newline after it, before the last
                # case: the program died on the one after the last it said.
                if booted and not ended and "kosmos> " in seen and nxt <= count \
                        and time.monotonic() - last_heard > 2:
                    self.verdicts[nxt] = "the program ended while it ran"
                    nxt += 1
                    ended = True

                if ended:
                    if nxt <= count:
                        child.stdin.write(b"/Home/t262/run.lua %d\n" % nxt)
                        child.stdin.flush()
                        ended = False
                        seen = ""
                        continue
                    break

                if time.monotonic() - last_heard > (STALL if booted else 300):
                    if booted:
                        self.verdicts[nxt] = "nothing for %d s; the guest was booted again" % STALL
                        nxt += 1
                    else:
                        raise RuntimeError("shard %d did not boot" % self.number)
                    break

                time.sleep(0.1)

            try:
                child.stdin.close()
                child.wait(30)
            except Exception:                                 # noqa: BLE001
                child.kill()
        log.close()


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    image, apps = argv[0], argv[1]
    shards = int(argv[argv.index("--shards") + 1]) if "--shards" in argv else 8
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    update = "--update" in argv

    root = upstream.test262()
    elf_built = os.path.join(apps, "jsc.elf")
    if not os.path.exists(elf_built):
        print("FAIL: no %s - `make test262` builds it" % elf_built)
        return 1

    os.makedirs(RESULTS, exist_ok=True)
    elf = os.path.join(RESULTS, "jsc.elf")
    subprocess.run([("x86_64-elf-" if "x86_64" in image else "aarch64-elf-") + "objcopy",
                    "--strip-debug", elf_built, elf], check=True)

    everything = tests(root, only)
    modules = [t for t in everything if "module" in t[2].get("flags", [])]
    run = [t for t in everything if "module" not in t[2].get("flags", [])]
    shards = max(1, min(shards, len(run)))
    print("test262 %s: %d language tests, %d of them modules and not run; "
          "%d in %d guests" % (COMMIT[:10], len(everything), len(modules), len(run), shards))

    started = time.monotonic()
    parts = [Shard(n, image, elf, run[n::shards]) for n in range(shards)]
    for p in parts:
        p.start()

    while any(p.is_alive() for p in parts):
        time.sleep(30)
        done = sum(len(p.verdicts) for p in parts)
        print("  %d of %d, %d min" % (done, len(run), (time.monotonic() - started) / 60),
              flush=True)

    for p in parts:
        if p.error:
            print("FAIL: %s (its serial: %s)" % (p.error, p.log))
            return 1

    failed = {}
    for n, p in enumerate(parts):
        for i, (name, _, _) in enumerate(p.cases, 1):
            why = p.verdicts.get(i, "no verdict")
            if why is not None:
                failed[name] = why
    passed = len(run) - len(failed)
    took = (time.monotonic() - started) / 60

    with open(os.path.join(RESULTS, "failures.txt"), "w") as f:
        for name in sorted(failed):
            f.write("%s\t%s\n" % (name, failed[name]))

    print("test262 %s: %d passed, %d failed, %d modules not run, in %.1f min; "
          "the failures and why: %s"
          % (COMMIT[:10], passed, len(failed), len(modules), took,
             os.path.relpath(os.path.join(RESULTS, "failures.txt"), ROOT)))

    counted = {"commit": COMMIT, "tests": len(everything), "modules_not_run": len(modules),
               "passed": passed, "failed": sorted(failed)}

    if update:
        if only:
            print("FAIL: the baseline is the whole suite's, and this ran %d tests of it" % len(run))
            return 1
        with open(BASELINE, "w") as f:
            json.dump(counted, f, indent=1)
            f.write("\n")
        print("PASS: the baseline is now this run's, in %s" % os.path.relpath(BASELINE, ROOT))
        return 0

    if not os.path.exists(BASELINE):
        print("FAIL: no baseline yet - `make test262 UPDATE=1` writes one")
        return 1

    with open(BASELINE) as f:
        base = json.load(f)
    if base["commit"] != COMMIT:
        print("FAIL: the baseline was counted against test262 %s, this is %s - "
              "`make test262 UPDATE=1`" % (base["commit"][:10], COMMIT[:10]))
        return 1

    # With `--only`, held to the baseline for the tests it ran, and no more.
    ran = set(name for name, _, _ in run)
    if only:
        base["failed"] = [n for n in base["failed"] if n in ran]
        base["passed"] = len(ran) - len(base["failed"])

    before = set(base["failed"])
    worse = sorted(n for n in failed if n not in before)
    better = sorted(n for n in before if n not in failed)

    for name in better:
        print("  now passes: %s" % name)
    if worse:
        for name in worse:
            print("  passed before, fails now: %s - %s" % (name, failed[name]))
        print("FAIL: %d tests that passed before fail now (%d passed, %d before)"
              % (len(worse), passed, base["passed"]))
        return 1

    print("PASS: test262, %d passed (%d before)%s"
          % (passed, base["passed"],
             ", %d newly - `make test262 UPDATE=1` keeps them" % len(better) if better else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
