#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""JavaScriptCore on Kosmos (`docs/w4.md` W4d).

`jsc.elf` - WebKit's engine with its interpreter, WTF, bmalloc, ICU and the
C++ runtimes, driven through its C API by `user/installed/JSC/jsc_kosmos.cpp`
- run from a /Home of its own, given a script that uses the language, and
what the script printed held to what it must print: a closure, a class
that extends another, a generator, a Proxy, a regular expression, numbers
formatted by Intl in two locales, a BigInt, the maths library, a hundred
thousand objects through the collector, and an `async` function's result,
printed by a promise's reaction after the script itself has finished. And
a script that throws, whose exception must come back as a sentence with
its line. Then `jsc.lua` itself, typed at the prompt as a person would,
with a script given by `-e`.

And the JIT (`docs/jit.md`): a hot function called three thousand times
must give the interpreter's answer, and the bytes written into the code
pool through its writable mapping must grow while it runs - the proof that
Baseline and DFG compiled it, into a pool run from a mapping that cannot
be written (`testing.md` 18.846).

Usage: run_jsc.py IMAGE [APPS]

`APPS` is where `make apps` linked `jsc.elf` - the gate's test userland,
or the lean one's by default.
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
WORK = scratch.directory("jsc")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- JavaScriptCore's test program (`tools/run_jsc.py`).
-- kosmos: image jsc.elf
local jsc = use("jsc.elf")
local HOT = [[
function f(n) { let s = 0; for (let i = 0; i < n; i++) s = (s + i * i) % 1000003; return s; }
let r = 0;
for (let k = 0; k < 3000; k++) r = f(1000);
print("hot=" + r);
]]
local said, ok = jsc.run("/Home/apps/jsc/language.js")
local threw, clean = jsc.eval("let a = 1;\\nnull.property;")
local lines = {}
for line in said:gmatch("([^\\n]*)\\n") do lines[#lines + 1] = line end
print("jscsays: " .. table.concat(lines, " ") .. (ok and " finished" or " threw")
      .. " | " .. threw:gsub("\\n$", "") .. (clean and " finished" or " threw"))
-- The JIT (`docs/jit.md`): a function run until it is compiled, and the
-- pool's pages written before and after, as Nebula counts them.
local pages, before = jsc.jit_pages()
local hot = jsc.eval(HOT)
local pages_after, after = jsc.jit_pages()
print(("jscjit: pages=%d,%d before=%d after=%d %s"):format(pages, pages_after, before, after,
      (hot:gsub("\\n", " "))))
"""

# The language, a piece a line, each printed so its answer can be held.
SCRIPT = """// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The language, for JavaScriptCore on Kosmos (`tools/run_jsc.py`).
function counter() { let n = 0; return () => ++n; }
const count = counter(); count(); count();
print("closure=" + count());

class Shape {
  constructor(name) { this.name = name; }
  area() { return 0; }
  toString() { return `${this.name}:${this.area()}`; }
}
class Square extends Shape {
  constructor(side) { super("square"); this.side = side; }
  area() { return this.side * this.side; }
}
print("class=" + new Square(4));

function* fibonacci() { let [a, b] = [0, 1]; for (;;) { yield a; [a, b] = [b, a + b]; } }
const fib = [];
for (const x of fibonacci()) { if (fib.length === 10) break; fib.push(x); }
print("generator=" + fib.join(","));

const lengths = new Proxy({}, { get: (_, key) => typeof key === "string" ? key.length : undefined });
print("proxy=" + lengths.kosmos);

const date = "2026-11-12".match(/(?<year>\\d{4})-(\\d+)-(\\d+)/u);
print("regex=" + date.groups.year + "/" + date.slice(2).join("/"));

print("intl=" + new Intl.NumberFormat("de-DE").format(1234567.891) + "/"
      + new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" }).format(1234.5));

print("bigint=" + 2n ** 64n);
print("math=" + Math.hypot(3, 4) + "/" + Math.cbrt(27) + "/" + Math.sin(Math.PI / 6).toFixed(6));

const kept = [];
let sum = 0;
for (let i = 0; i < 100000; i++) kept.push({ i, text: "n" + i });
for (const o of kept) sum += o.i + o.text.length;
print("objects=" + kept.length + "/" + sum);

async function twice(x) { await null; return x * 2; }
twice(21).then(v => print("async=" + v));
print("script=done");
"""

# What it must print, in order: the reaction's line comes after the
# script's last, because reactions run when the script has returned.
WANT = " ".join([
    "closure=3",
    "class=square:16",
    "generator=0,1,1,2,3,5,8,13,21,34",
    "proxy=6",
    "regex=2026/11/12",
    "intl=1.234.567,891/$1,234.50",
    "bigint=18446744073709551616",
    "math=5/3/0.500000",
    "objects=100000/5000538890",        # the sum of i, and of the lengths of "n" + i
    "script=done",
    "async=42",
    "finished",
    "| exception: TypeError: null is not an object (evaluating 'null.property') (line 2) threw",
])


# The hot function's answer: the sum of i * i for i below 1000, modulo
# 1000003, as Python works it out.
HOT_WANT = "832504"


def main():
    built = os.path.join(APPS, "jsc.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    want = WANT
    image = os.path.join(WORK, "jsc.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "jsctest.lua"), "w") as f:
        f.write(PROGRAM)

    with open(os.path.join(WORK, "language.js"), "w") as f:
        f.write(SCRIPT)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/jsc/%s" % (os.path.join(WORK, n), n)
                      for n in ("jsctest.lua", "jsc.elf", "language.js")]
                   + ["%s:/Home/apps/jsc/jsc.lua" % os.path.join(ROOT, "user", "installed",
                                                                "JSC", "jsc.lua")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = None
    jit = None
    program = False

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/jsc/jsctest.lua")

        while time.monotonic() < started + 240 and (said is None or jit is None):
            for line in guest.seen[mark:].split("\n")[1:-1]:
                if line.strip().startswith("jscsays: "):
                    said = line.strip()
                if line.strip().startswith("jscjit: "):
                    jit = line.strip()
            if said is None or jit is None:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started

        # And the program a person runs, `jsc.lua`, with its script given.
        if said is not None:
            mark = len(guest.seen)
            guest.type('/Home/apps/jsc/jsc.lua -e "print(6 * 7, typeof Intl)"')
            stop = time.monotonic() + 120
            while time.monotonic() < stop and "42 object" not in guest.seen[mark:]:
                time.sleep(0.2)
                guest._read_available()
            program = "42 object" in guest.seen[mark:]

            # And `--no-jit` (`docs/jit.md`): a loop hot enough to be
            # compiled, run on the interpreter alone - its answer, and not
            # a byte of code written. The hot function above, with the JIT
            # on, is the control: the same engine writes code there.
            if program:
                interp = len(guest.seen)
                guest.type('/Home/apps/jsc/jsc.lua --no-jit -e '
                           '"var s = 0; for (var i = 0; i < 300000; i++) s = (s + i) % 997; print(s)"')
                stop = time.monotonic() + 120
                said_interp = None
                while time.monotonic() < stop and said_interp is None:
                    said_interp = re.search(r"(\d+)\r?\njsc: on the interpreter alone, (\d+) bytes",
                                            guest.seen[interp:])
                    time.sleep(0.2)
                    guest._read_available()
    finally:
        guest.close()

    if os.environ.get("KOSMOS_JSC_SERIAL"):
        with open(os.environ["KOSMOS_JSC_SERIAL"], "w") as f:
            f.write(guest.seen)

    if said is None:
        said_interp = None
    if said == "jscsays: " + want and not program:
        print("FAIL: `jsc.lua -e` did not print 42 object")
        print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    if said != "jscsays: " + want:
        print("FAIL: JavaScriptCore on Kosmos did not print what it must")
        print("  wanted: %s" % want)
        print("  said:   %s" % said)
        if said is None:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    # The JIT: the hot function's answer, the pool's pages written as Nebula
    # counts them, and code written into the pool while it ran - the bytes
    # through its one writer grew. Pages alone need not grow: a new
    # function's code can land in a page already touched.
    m = re.match(r"jscjit: pages=(-?\d+),(-?\d+) before=(\d+) after=(\d+) hot=(\d+)", jit or "")
    if not m or m.group(5) != HOT_WANT or int(m.group(2)) <= 0 \
       or int(m.group(4)) <= int(m.group(3)):
        print("FAIL: the JIT did not compile the hot function into its pool: %s" % jit)
        return 1

    if not said_interp or said_interp.group(1) != "765" or said_interp.group(2) != "0":
        print("FAIL: `jsc --no-jit` did not run its loop on the interpreter alone: %s"
              % (said_interp.group(0) if said_interp else guest.seen[interp:][-800:]))
        return 1

    print("PASS: JavaScriptCore on Kosmos, from a %.1f MB image, in %.1f s: %s; "
          "and jsc.lua -e printed 42 object; and the JIT compiled a hot function "
          "into its pool, %s bytes written by it (%s pages of the pool touched), hot=%s; "
          "and with --no-jit a hot loop ran with no code written"
          % (os.path.getsize(image) / 1e6, took, want, int(m.group(4)) - int(m.group(3)),
             m.group(2), HOT_WANT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
