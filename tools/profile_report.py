#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Where a profile's processors went, named from the build's own symbols.

**The Mac half of the App Inspector's first step** (`roadmap.md`; Diego,
29 September: "lets profile in the m700 the Lua VS C"). `profile` on the
machine writes a `.kprof`: a header naming every process and the image it
ran, and sixteen bytes a sample - an address, a process, a processor, and
whether that was a program's own code, the kernel or idle
(`kernel/profile.c`). The kernel does not know what an address means, and
neither does the machine that took it; this does, because the build's ELFs
are here with their debug information.

Each address is named by `addr2line` against the image it ran in - the
system's `init.elf`, an installed application's own, or the kernel - and
put in a **class by the file it was compiled from**: Lua's interpreter,
its collector, its compiler, its libraries; Kosmos's bindings; the
allocator; the libc; a kit; vendored C; a server or driver; an
application's own C; the kernel. The classes add up to layers - Lua, C, the
kernel, idle - which is the question this was built to answer.

**The symbols have to be the ones that ran**, or every name is a
plausible lie. The header carries `str_format`'s address as the machine
had it, and an `init.elf` is used only if its own `str_format` agrees. A
stick's are kept beside its image (`make MEGA=1 x86-usb-image`), and a
QEMU run's are the build's.

Usage:
  profile_report.py [FILE.kprof] [--symbols DIR] [--out PAGE.html]
                    [--json SUMMARY.json]

Without a file, the newest in `build/stick-profiles/` - where
`make stick-log FILE=/Home/profiles/` puts them.
"""

import glob
import html
import json
import os
import re
import struct
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SAMPLE = struct.Struct("<QIHBB")        # pc, pid, thread, cpu, where
USER, KERNEL, IDLE = 1, 2, 3

EM_X86_64, EM_AARCH64 = 62, 183
ADDR2LINE = {EM_X86_64: "x86_64-elf-addr2line", EM_AARCH64: "aarch64-none-elf-addr2line"}
NM = {EM_X86_64: "x86_64-elf-nm", EM_AARCH64: "aarch64-none-elf-nm"}

# Lua's own files, by what they are. `lmem.c` is the interpreter asking for
# memory; the asking is Lua's, what answers it is the allocator's.
LUA_VM = {"lapi", "lctype", "ldebug", "ldo", "ldump", "lfunc", "lmem",
          "lobject", "lopcodes", "lstate", "lstring", "ltable", "ltm",
          "lundump", "lvm", "lzio"}
LUA_GC = {"lgc"}
LUA_COMPILER = {"lcode", "llex", "lparser"}
LUA_LIBS = {"lauxlib", "lbaselib", "lcorolib", "ldblib", "linit", "liolib",
            "lmathlib", "loadlib", "loslib", "lstrlib", "ltablib", "lutf8lib"}

# Every class, its layer, and its colour: Lua's in blues, with the
# collector in red because it is the part a deadline feels; C's in greens;
# the kernel violet; idle and the unknown in greys.
CLASSES = [
    ("Lua interpreter",   "Lua",    "#2f5fd0"),
    ("Lua collector",     "Lua",    "#d2473a"),
    ("Lua compiler",      "Lua",    "#7aa0ea"),
    ("Lua libraries",     "Lua",    "#4c7fe0"),
    ("Kosmos bindings",   "C",      "#1f8a70"),
    ("allocator",         "C",      "#6bbf8a"),
    ("libc and runtime",  "C",      "#3da37a"),
    ("kits",              "C",      "#0f6e5a"),
    ("vendored C",        "C",      "#8fcf9f"),
    ("servers and drivers", "C",    "#2b7f4f"),
    ("an app's own C",    "C",      "#a7d88a"),
    ("kernel",            "kernel", "#7a55b8"),
    ("kernel, on a syscall", "kernel", "#a58ad6"),
    ("unknown",           "unknown", "#9a9a9a"),
    ("idle",              "idle",   "#d9d9d4"),
]
LAYER_OF = {name: layer for name, layer, _ in CLASSES}
COLOUR_OF = {name: colour for name, _, colour in CLASSES}
LAYERS = [("Lua", "#2f5fd0"), ("C", "#1f8a70"), ("kernel", "#7a55b8"),
          ("unknown", "#9a9a9a"), ("idle", "#d9d9d4")]


def classify(path):
    """The class of code compiled from `path`, a source file's name."""
    if not path or path.startswith("??"):
        return "unknown"

    rel = os.path.relpath(path, ROOT) if os.path.isabs(path) else path
    base = os.path.splitext(os.path.basename(rel))[0]

    if rel.startswith("lua/upstream/"):
        if base in LUA_GC:
            return "Lua collector"
        if base in LUA_COMPILER:
            return "Lua compiler"
        if base in LUA_LIBS:
            return "Lua libraries"
        if base in LUA_VM:
            return "Lua interpreter"
        return "unknown"

    if rel.startswith("lua/kosmos/") or rel in (
            "user/init/sys_user.c", "user/init/lua_glue.c", "user/init/misc_user.c"):
        return "Kosmos bindings"

    if rel == "runtime/libc/malloc.c":
        return "allocator"

    if rel.startswith("runtime/libc/") or rel.startswith("user/init/"):
        return "libc and runtime"

    if rel.startswith("runtime/upstream/"):
        return "vendored C"

    if rel.startswith("user/kits/"):
        return "kits"

    if rel.startswith("user/servers/") or rel.startswith("user/drivers/"):
        return "servers and drivers"

    if rel.startswith("user/bin/") or rel.startswith("user/installed/"):
        return "an app's own C"

    if rel.split("/")[0] in ("kernel", "arch", "hal", "boot"):
        return "kernel"

    # A header's inline function lands wherever it was defined; a Kosmos
    # header in `user/include` is a binding or a protocol's, both C.
    if rel.startswith("user/include/") or rel.startswith("runtime/include/"):
        return "libc and runtime"

    return "unknown"


# ---------------------------------------------------------------------------
# The file.

def read_profile(path):
    with open(path, "rb") as f:
        data = f.read()

    end = data.find(b"\nend\n")

    if not data.startswith(b"kprof\t") or end < 0:
        raise SystemExit(f"{path}: not a profile - `profile` on the machine writes one")

    head = {"processes": {}}

    for line in data[:end].decode("utf-8", "replace").split("\n"):
        f = line.split("\t")

        if f[0] == "syscall" and len(f) >= 4:
            head.setdefault("syscalls", []).append((int(f[1]), int(f[2]), int(f[3])))
        elif f[0] == "cpu" and len(f) >= 5:
            head.setdefault("cpus", []).append(tuple(int(x) for x in f[1:5]))
        elif f[0] == "process" and len(f) >= 6:
            head["processes"][int(f[1])] = {
                "id": int(f[1]), "parent": int(f[2]), "name": f[3],
                "from": f[4], "image": f[5]}
        elif f[0] == "anchor" and len(f) >= 3:
            head["anchor"] = (f[1], int(f[2], 16) if f[2] else None)
        elif len(f) >= 2:
            head[f[0]] = f[1]

    body = data[end + len(b"\nend\n"):]
    body = body[:len(body) - len(body) % SAMPLE.size]

    return head, [SAMPLE.unpack_from(body, i) for i in range(0, len(body), SAMPLE.size)]


# ---------------------------------------------------------------------------
# The symbols.

def machine_of(elf):
    with open(elf, "rb") as f:
        head = f.read(20)

    if head[:4] != b"\x7fELF":
        return None

    return struct.unpack_from("<H", head, 18)[0]


def symbol_address(elf, name):
    out = subprocess.run([NM[machine_of(elf)], elf], capture_output=True,
                         text=True).stdout

    for line in out.splitlines():
        f = line.split()

        if len(f) == 3 and f[2] == name:
            return int(f[0], 16)

    return None


def find_symbols(head, symbols_dir):
    """The system image, the kernel and the directory apps come from.

    An `init.elf` only if its anchor agrees with the machine's; a kernel
    from the same directory when it came from a stick's symbols, and
    otherwise this build's for the same architecture, said as a guess.
    """
    name, anchor = head.get("anchor", (None, None))
    notes = []

    if symbols_dir:
        dirs = [symbols_dir]
    else:
        dirs = sorted(glob.glob(os.path.join(ROOT, "build", "x86_64", "*.symbols")),
                      key=os.path.getmtime, reverse=True)
        dirs += sorted(glob.glob(os.path.join(ROOT, "build", "user*")),
                       key=os.path.getmtime, reverse=True)

    init = None

    for d in dirs:
        candidate = os.path.join(d, "init.elf")

        if not os.path.exists(candidate) or machine_of(candidate) not in NM:
            continue

        if anchor is not None and symbol_address(candidate, name) == anchor:
            init = candidate
            break

    if init is None:
        raise SystemExit(
            f"no init.elf here agrees with the machine: its {name} was at "
            f"{anchor:#x} - the profile is from a build whose symbols are not "
            "kept (a stick's are beside its image, as <image>.symbols)"
            if anchor is not None else "the profile names no anchor")

    notes.append(f"{os.path.relpath(init, ROOT)} ({name} agrees)")
    arch = machine_of(init)
    here = os.path.dirname(init)
    kernel = os.path.join(here, "kernel.elf")

    if not os.path.exists(kernel):
        options = [os.path.join(ROOT, "build", "kosmos.elf"),
                   os.path.join(ROOT, "build", "x86_64", "kosmos.elf")]
        options = [k for k in options if os.path.exists(k) and machine_of(k) == arch]
        kernel = options[0] if options else None

        if kernel:
            notes.append(f"{os.path.relpath(kernel, ROOT)} (this build's kernel: "
                         "its names may be a rebuild's)")
    else:
        notes.append(os.path.relpath(kernel, ROOT))

    # An installed application's image: beside the system's when it came
    # from a stick, and otherwise `make apps`'s, which keep their symbols.
    apps = [here, os.path.join(ROOT, "build",
                               "user-x86_64" if arch == EM_X86_64 else "user", "apps")]

    return arch, init, kernel, apps, notes


class Image:
    """An ELF's loaded bytes, by address: what instruction was where."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()

        phoff, = struct.unpack_from("<Q", self.data, 0x20)
        size, count = struct.unpack_from("<HH", self.data, 0x36)
        self.loads = []

        for i in range(count):
            kind, _, offset, vaddr, _, filesz = struct.unpack_from(
                "<IIQQQQ", self.data, phoff + i * size)

            if kind == 1:                           # PT_LOAD
                self.loads.append((vaddr, filesz, offset))

    def bytes_at(self, address, n):
        for vaddr, filesz, offset in self.loads:
            if vaddr <= address and address + n <= vaddr + filesz:
                at = offset + address - vaddr
                return self.data[at:at + n]

        return b""


# What a program's return from a syscall looks like, the instruction before
# the address it comes back to: `svc #0`, and `syscall`.
SVC = {EM_AARCH64: (4, struct.pack("<I", 0xD4000001)), EM_X86_64: (2, b"\x0f\x05")}


class Names:
    """Addresses to what they are, an image at a time, asked once each.

    **A name is the innermost function whose file is a `.c`**, and so is
    its class: `sys0`, inline in a header, is the syscall of whichever
    binding it was inlined into, and a binding is what it is. `addr2line
    -i` gives the chain an inlined address came through.

    **And a sample just after a syscall is the kernel's.** A syscall runs
    with interrupts masked on both boards, so a tick that falls due during
    one is taken as the program resumes - at the instruction after `svc` or
    `syscall`, in the program's own code. Counted there it made `spin`,
    whose loop is `sys.ticks()`, read as 99.7% "its own code" in a stub
    three instructions long. So such an address is time in the kernel on a
    call, and named by what made the call.
    """

    def __init__(self, arch):
        self.arch = arch
        self.cache = {}
        self.images = {}

    def after_syscall(self, elf, address):
        if elf not in self.images:
            self.images[elf] = Image(elf)

        n, pattern = SVC[self.arch]

        return self.images[elf].bytes_at(address - n, n) == pattern

    def resolve(self, elf, addresses):
        want = sorted({a for a in addresses if (elf, a) not in self.cache})

        if not want:
            return

        if elf is None:
            for a in want:
                self.cache[(elf, a)] = ("??", "??", False)
            return

        out = subprocess.run([ADDR2LINE[self.arch], "-a", "-f", "-i", "-e", elf],
                             input="\n".join(f"{a:#x}" for a in want) + "\n",
                             capture_output=True, text=True).stdout.splitlines()
        chains, chain = [], None

        for i in range(0, len(out)):
            line = out[i]

            if line.startswith("0x") and (chain is None or len(chain) % 2 == 0):
                chain = []
                chains.append(chain)
            elif chain is not None:
                chain.append(line)

        for a, lines in zip(want, chains):
            pairs = [(lines[j], lines[j + 1].split(":")[0])
                     for j in range(0, len(lines) - 1, 2)]
            func, path = pairs[0] if pairs else ("??", "??")

            for f, p in pairs:
                if p.endswith(".c") or p.endswith(".S"):
                    func, path = f, p
                    break

            self.cache[(elf, a)] = (func, path, self.after_syscall(elf, a))

    def name(self, elf, address):
        return self.cache.get((elf, address), ("??", "??", False))


# ---------------------------------------------------------------------------
# The sums.

def summarise(head, samples, symbols):
    arch, init, kernel, app_dirs, notes = symbols
    procs = head["processes"]

    def image_for(pid, where):
        if where == KERNEL:
            return kernel

        p = procs.get(pid)
        image = p["image"] if p else "init.elf"

        if image == "init.elf":
            return init

        for d in app_dirs:
            candidate = os.path.join(d, image)

            if os.path.exists(candidate):
                return candidate

        return None

    by_image = defaultdict(set)

    for pc, pid, _, _, where in samples:
        if where != IDLE:
            by_image[image_for(pid, where)].add(pc)

    names = Names(arch)

    for elf, pcs in by_image.items():
        names.resolve(elf, pcs)

    for image in sorted({p["image"] for p in procs.values()} - {"init.elf"}):
        if all(not os.path.exists(os.path.join(d, image)) for d in app_dirs):
            notes.append(f"{image}: no symbols here, so its code is unknown")

    total = len(samples)
    classes = defaultdict(int)
    per_proc = defaultdict(lambda: defaultdict(int))
    funcs = defaultdict(int)
    proc_funcs = defaultdict(lambda: defaultdict(int))

    for pc, pid, _, _, where in samples:
        if where == IDLE:
            classes["idle"] += 1
            continue

        elf = image_for(pid, where)
        func, path, syscall = names.name(elf, pc)
        rel = os.path.relpath(path, ROOT) if os.path.isabs(path) else path

        if where == KERNEL:
            cls = "kernel"
        elif syscall:
            cls, func = "kernel, on a syscall", func + " (a syscall)"
        else:
            cls = classify(path)

        classes[cls] += 1
        per_proc[pid][cls] += 1
        funcs[(func, rel, cls)] += 1
        proc_funcs[pid][(func, rel, cls)] += 1

    layers = defaultdict(int)

    for cls, n in classes.items():
        layers[LAYER_OF[cls]] += n

    return {
        "total": total, "classes": dict(classes), "layers": dict(layers),
        "per_proc": {pid: dict(c) for pid, c in per_proc.items()},
        "funcs": funcs, "proc_funcs": proc_funcs, "notes": notes,
    }


def syscall_names():
    """Each syscall's number to its name, from the header the kernel uses."""
    names = {}

    with open(os.path.join(ROOT, "kernel", "syscall.h")) as f:
        for m in re.finditer(r"#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)\b", f.read()):
            if m.group(1) != "SYS_MAX":
                names.setdefault(int(m.group(2)), m.group(1))

    return names


def syscall_rows(head):
    """Each syscall made, dearest first: name, calls a second, microseconds a
    call, and the share of one processor it took."""
    hz = max(1, int(head.get("counter_hz", 1)))
    seconds = int(head.get("counter_ticks", 0)) / hz or 1
    names = syscall_names()
    rows = []

    for number, calls, ticks in head.get("syscalls", []):
        name = names.get(number, "syscall %d" % number)

        rows.append({"name": name,
                     # Blocks until something happens, so its time is mostly
                     # waiting: listed apart, after the ones that work.
                     "waits": any(w in name for w in ("RECEIVE", "CALL", "SLEEP",
                                                      "WAIT")),
                     "per_second": calls / seconds,
                     "us": (ticks / calls) * 1e6 / hz if calls else 0.0,
                     "core": 100.0 * ticks / hz / seconds})

    return sorted(rows, key=lambda r: (r["waits"], -r["core"]))


def cpu_rows(head):
    """Each processor's time in programs, in the kernel and held off, as a
    share of the profile's length - from the counter, not from samples."""
    hz = max(1, int(head.get("counter_hz", 1)))
    total = max(1, int(head.get("counter_ticks", 1)))

    return [{"cpu": c, "user": 100.0 * u / total, "kernel": 100.0 * k / total,
             "held_off_ms": 1000.0 * h / hz}
            for c, u, k, h in head.get("cpus", [])]


def pct(n, of):
    return 100.0 * n / of if of else 0.0


def proc_name(head, pid):
    if pid == 0:
        return "the kernel's own threads"

    p = head["processes"].get(pid)

    return p["name"] if p else f"process {pid}"


def text_report(head, s):
    total = s["total"]
    busy = total - s["layers"].get("idle", 0)
    lines = [
        f"profile: {head.get('version', '?')} on {head.get('platform', '?')}, "
        f"{head.get('cores', '?')} processors, "
        f"{int(head.get('counter_ticks', 0)) / max(1, int(head.get('counter_hz', 1))):.1f} s, "
        f"{total} samples, {head.get('lost', '?')} lost",
        "symbols: " + "; ".join(s["notes"]),
        "",
        f"busy {pct(busy, total):.1f}% of the processors' time, of which:",
    ]

    for layer, _ in LAYERS[:-1]:
        n = s["layers"].get(layer, 0)

        if n:
            lines.append(f"  {pct(n, busy):5.1f}%  {layer}")

    lines += ["", "by class, of the busy time:"]

    for cls, _, _ in CLASSES[:-1]:
        n = s["classes"].get(cls, 0)

        if n:
            lines.append(f"  {pct(n, busy):5.1f}%  {cls}")

    lines += ["", "by process, of all the processors' time (Lua / C / kernel):"]

    for pid, c in sorted(s["per_proc"].items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(c.values())

        if pct(n, total) < 0.1:
            continue

        by = defaultdict(int)

        for cls, k in c.items():
            by[LAYER_OF[cls]] += k

        lines.append(f"  {pct(n, total):5.1f}%  {proc_name(head, pid):<24} "
                     f"Lua {pct(by['Lua'], n):3.0f}%  C {pct(by['C'], n):3.0f}%  "
                     f"kernel {pct(by['kernel'], n):3.0f}%")

    calls = syscall_rows(head)

    if calls:
        lines += ["", "syscalls, timed from entry to return:"]
        waiting = False

        for r in calls:
            if r["waits"] and not waiting:
                waiting = True
                lines.append("  and those that wait, whose time is mostly waiting:")

            lines.append("  %6.2f%% of a processor  %-20s %9.0f a second, %8.1f us each"
                         % (r["core"], r["name"], r["per_second"], r["us"]))

    cpus = cpu_rows(head)

    if cpus:
        lines += ["", "each processor, from the counter (programs / kernel / held off):"]

        for r in cpus:
            lines.append("  cpu %d  %5.1f%% / %5.1f%% / %7.1f ms"
                         % (r["cpu"], r["user"], r["kernel"], r["held_off_ms"]))

    lines += ["", "the busiest functions, of the busy time:"]

    for (func, path, cls), n in sorted(s["funcs"].items(), key=lambda kv: -kv[1])[:20]:
        lines.append(f"  {pct(n, busy):5.1f}%  {func:<28} {cls:<20} {path}")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# The page.

def bar(parts, of, height=14):
    """A stacked bar: `parts` is [(label, count, colour)]."""
    cells = []

    for label, n, colour in parts:
        if n <= 0 or of <= 0:
            continue

        w = 100.0 * n / of
        cells.append(f'<span style="width:{w:.3f}%;background:{colour}" '
                     f'title="{html.escape(label)}: {w:.1f}%"></span>')

    return f'<div class="bar" style="height:{height}px">{"".join(cells)}</div>'


def syscall_section(head):
    """The timed syscalls and the processors' own counts, when the profile
    carries them."""
    esc = html.escape
    calls = syscall_rows(head)
    cpus = cpu_rows(head)
    out = ""

    if calls:
        rows = "".join(
            f'<tr><td class="n">{r["core"]:.2f}%</td><td><code>{esc(r["name"])}</code>'
            f'{" <span class=split>waits</span>" if r["waits"] else ""}</td>'
            f'<td class="n">{r["per_second"]:.0f}</td><td class="n">{r["us"]:.1f}</td></tr>'
            for r in calls)
        out += ('<h2>Syscalls, timed</h2><p class="meta">Each call from entry to '
                'return. Those marked <em>waits</em> block until something happens, '
                'so their time is mostly waiting.</p><div class="tablewrap '
                'scroll"><table><tr><td class="n">of a processor</td><td>syscall</td>'
                '<td class="n">a second</td><td class="n">us each</td></tr>'
                + rows + '</table></div>')

    if cpus:
        rows = "".join(
            f'<tr><td>cpu {r["cpu"]}</td><td class="n">{r["user"]:.1f}%</td>'
            f'<td class="n">{r["kernel"]:.1f}%</td><td class="n">{r["held_off_ms"]:.1f}</td></tr>'
            for r in cpus)
        out += ('<h2>Each processor, from the counter</h2><div class="tablewrap '
                'scroll"><table><tr><td></td><td class="n">programs</td><td class="n">'
                'kernel</td><td class="n">held off, ms</td></tr>' + rows + '</table></div>')

    return out


def page(head, s, title):
    total = s["total"]
    busy = total - s["layers"].get("idle", 0)
    seconds = int(head.get("counter_ticks", 0)) / max(1, int(head.get("counter_hz", 1)))
    esc = html.escape

    layer_rows = "".join(
        f'<li><i style="background:{c}"></i>{esc(l)}<b>{pct(s["layers"].get(l, 0), busy):.1f}%</b></li>'
        for l, c in LAYERS[:-1] if s["layers"].get(l, 0))
    class_rows = "".join(
        f'<li><i style="background:{c}"></i>{esc(n)}<b>{pct(s["classes"].get(n, 0), busy):.1f}%</b></li>'
        for n, _, c in CLASSES[:-1] if s["classes"].get(n, 0))

    procs = []

    for pid, c in sorted(s["per_proc"].items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(c.values())

        if pct(n, total) < 0.05:
            continue

        by = defaultdict(int)

        for cls, k in c.items():
            by[LAYER_OF[cls]] += k

        parts = [(cls, c.get(cls, 0), COLOUR_OF[cls]) for cls, _, _ in CLASSES]
        top = sorted(s["proc_funcs"][pid].items(), key=lambda kv: -kv[1])[:12]
        top_rows = "".join(
            f'<tr><td class="n">{pct(k, n):.1f}%</td><td><code>{esc(f)}</code></td>'
            f'<td><i class="dot" style="background:{COLOUR_OF[cl]}"></i>{esc(cl)}</td>'
            f'<td class="path">{esc(p)}</td></tr>'
            for (f, p, cl), k in top)
        p = head["processes"].get(pid, {})
        procs.append(
            f'<details><summary><span class="pname">{esc(proc_name(head, pid))}</span>'
            f'<span class="n">{pct(n, total):.1f}%</span>{bar(parts, n)}'
            f'<span class="split">Lua {pct(by["Lua"], n):.0f}% · C {pct(by["C"], n):.0f}% · '
            f'kernel {pct(by["kernel"], n):.0f}%</span></summary>'
            f'<p class="from">{esc(p.get("from") or "built into the image")} · '
            f'{esc(p.get("image", ""))} · process {pid}</p>'
            f'<table>{top_rows}</table></details>')

    top = sorted(s["funcs"].items(), key=lambda kv: -kv[1])[:40]
    func_rows = "".join(
        f'<tr><td class="n">{pct(k, busy):.1f}%</td><td><code>{esc(f)}</code></td>'
        f'<td><i class="dot" style="background:{COLOUR_OF[cl]}"></i>{esc(cl)}</td>'
        f'<td class="path">{esc(p)}</td></tr>'
        for (f, p, cl), k in top)

    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}</title>
<style>
:root {{
  --ground:#f6f6f2; --paper:#ffffff; --ink:#1d1f24; --dim:#62666f;
  --rule:#dcdcd4; --track:#ecece6;
}}
@media (prefers-color-scheme: dark) {{
  :root:not([data-theme="light"]) {{
    --ground:#15171b; --paper:#1d2026; --ink:#e7e8ea; --dim:#9aa0aa;
    --rule:#2e323a; --track:#262a31;
  }}
}}
:root[data-theme="dark"] {{
  --ground:#15171b; --paper:#1d2026; --ink:#e7e8ea; --dim:#9aa0aa;
  --rule:#2e323a; --track:#262a31;
}}
* {{ box-sizing:border-box }}
body {{ margin:0; background:var(--ground); color:var(--ink);
  font:15px/1.5 -apple-system, "Helvetica Neue", Arial, sans-serif; }}
main {{ max-width:1040px; margin:0 auto; padding:28px 16px 64px; }}
h1 {{ font-size:26px; margin:0 0 4px; text-wrap:balance }}
h2 {{ font-size:13px; letter-spacing:.08em; text-transform:uppercase;
  color:var(--dim); margin:36px 0 12px; font-weight:600 }}
.meta {{ color:var(--dim); margin:0 0 20px }}
.meta code, code {{ font:13px ui-monospace, Menlo, monospace }}
.bar {{ display:flex; width:100%; background:var(--track); border-radius:3px;
  overflow:hidden; }}
.bar span {{ display:block; height:100% }}
.big .bar {{ height:34px !important }}
.keys {{ list-style:none; padding:0; margin:12px 0 0; display:grid;
  grid-template-columns:repeat(auto-fill,minmax(210px,1fr)); gap:6px 18px }}
.keys li {{ display:flex; align-items:center; gap:8px }}
.keys b {{ margin-left:auto; font-variant-numeric:tabular-nums; font-weight:600 }}
.keys i, .dot {{ display:inline-block; width:10px; height:10px; border-radius:2px }}
.dot {{ margin-right:6px }}
details {{ background:var(--paper); border:1px solid var(--rule);
  border-radius:6px; margin:6px 0; }}
summary {{ display:grid; grid-template-columns:200px 60px 1fr 210px; gap:12px;
  align-items:center; padding:9px 12px; cursor:pointer; list-style:none }}
summary::-webkit-details-marker {{ display:none }}
summary:focus-visible {{ outline:2px solid #2f5fd0; outline-offset:2px }}
.pname {{ font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap }}
.split {{ color:var(--dim); font-size:13px; font-variant-numeric:tabular-nums }}
.n {{ font-variant-numeric:tabular-nums; text-align:right }}
.from {{ color:var(--dim); margin:0 12px 8px; font-size:13px }}
.scroll {{ overflow-x:auto }}
table {{ border-collapse:collapse; width:100%; font-size:13px }}
details table {{ margin:0 0 10px }}
td {{ padding:4px 12px; border-top:1px solid var(--rule); vertical-align:top }}
td.path {{ color:var(--dim); font:12px ui-monospace, Menlo, monospace;
  word-break:break-all }}
.tablewrap {{ background:var(--paper); border:1px solid var(--rule); border-radius:6px }}
@media (max-width:700px) {{
  summary {{ grid-template-columns:1fr 60px; }}
  summary .bar {{ grid-column:1 / -1 }}
  summary .split {{ grid-column:1 / -1 }}
}}
</style></head>
<body><main>
<h1>{esc(title)}</h1>
<p class="meta">Kosmos {esc(head.get("version", "?"))} on {esc(head.get("platform", "?"))},
{esc(head.get("cores", "?"))} processors, {seconds:.1f} s at {esc(head.get("tick_hz", "?"))} Hz:
{total} samples, {esc(head.get("lost", "?"))} lost. Busy {pct(busy, total):.1f}% of the time.<br>
Symbols: {esc("; ".join(s["notes"]))}</p>

<h2>Of the time the processors were busy</h2>
<div class="big">{bar([(l, s["layers"].get(l, 0), c) for l, c in LAYERS[:-1]], busy)}</div>
<ul class="keys">{layer_rows}</ul>

<h2>By what the code is</h2>
{bar([(n, s["classes"].get(n, 0), c) for n, _, c in CLASSES[:-1]], busy)}
<ul class="keys">{class_rows}</ul>

<h2>By process, of all the processors' time</h2>
{"".join(procs)}

{syscall_section(head)}

<h2>The busiest functions, of the busy time</h2>
<div class="tablewrap scroll"><table>{func_rows}</table></div>
</main></body></html>
"""


def newest_profile():
    found = glob.glob(os.path.join(ROOT, "build", "stick-profiles", "*.kprof"))

    if not found:
        raise SystemExit("no profile named, and none in build/stick-profiles/ - "
                         "`make stick-log FILE=/Home/profiles/` brings them off a stick")

    return max(found, key=os.path.getmtime)


def main(argv):
    path = symbols_dir = out = summary = None
    functions = 12
    rest = list(argv)

    while rest:
        a = rest.pop(0)

        if a == "--symbols":
            symbols_dir = rest.pop(0)
        elif a == "--out":
            out = rest.pop(0)
        elif a == "--json":
            summary = rest.pop(0)
        elif a == "--functions":
            # How many of each process's functions the JSON keeps, busiest
            # first: twelve, as the page shows, unless a reader of the JSON
            # wants the whole of one - the browser's spread over a hundred.
            functions = int(rest.pop(0))
        else:
            path = a

    path = path or newest_profile()
    head, samples = read_profile(path)
    symbols = find_symbols(head, symbols_dir)
    s = summarise(head, samples, symbols)

    print(text_report(head, s))

    name = os.path.splitext(os.path.basename(path))[0]
    out = out or os.path.join(ROOT, "build", "profiles", name + ".html")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    with open(out, "w") as f:
        f.write(page(head, s, f"Profile {name}"))

    print(f"\nthe page: {os.path.relpath(out, ROOT)}")

    if summary:
        busy = s["total"] - s["layers"].get("idle", 0)
        per = {}

        for pid, c in s["per_proc"].items():
            n = sum(c.values())
            by = defaultdict(int)

            for cls, k in c.items():
                by[LAYER_OF[cls]] += k

            top = sorted(s["proc_funcs"][pid].items(),
                         key=lambda kv: -kv[1])[:functions]
            per[proc_name(head, pid)] = {
                "samples": n, "layers": dict(by), "classes": dict(c),
                "functions": [{"name": f, "file": where, "class": cl, "samples": k}
                              for (f, where, cl), k in top]}

        with open(summary, "w") as f:
            json.dump({"samples": s["total"], "busy": busy, "lost": int(head.get("lost", 0)),
                       "cores": int(head.get("cores", 0)),
                       "tick_hz": int(head.get("tick_hz", 0)),
                       "seconds": int(head.get("counter_ticks", 0))
                                  / max(1, int(head.get("counter_hz", 1))),
                       "layers": s["layers"], "classes": s["classes"],
                       "processes": per, "notes": s["notes"],
                       "syscalls": syscall_rows(head), "cpus": cpu_rows(head)},
                      f, indent=1)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
