# LuaJIT, studied

`roadmap.md` 6zo. Diego, 27 September 2026: "I want to study the
possibility of using luajit in kosmos to improve performance for Lua code".
LuaJIT was rejected in August for Lua 5.4 (`design.md` §5, the README's
decision log). This page is what was measured the night it was asked, so
that the decision - kept or changed - is made on numbers.

**The short answer: it would be fast, and it would cost the language
Kosmos is written in.** LuaJIT's compiler runs Kosmos's own libraries two
to four times faster on this Mac. It is Lua 5.1, and more than half of
Kosmos's Lua files use something 5.1 does not have - integer arithmetic
above all, which the protocols, the filesystem's dates and the counter all
depend on. And its compiler writes machine code into memory as it runs,
which Kosmos refuses by design.

---

## What was measured

**The Lua.** Kosmos's own, vendored Lua left out: `user/init`, `user/bin`
and `user/lib`, **194 files and 75,285 lines**.

**The speed.** Four of Kosmos's libraries that LuaJIT can run unchanged -
chosen because they use nothing LuaJIT lacks, which is itself a selection:
the IDE's lexer over every line of `wm.lua`, Markdown's parse and wrap of
`roadmap.md`, the editor's buffer taking 20,000 keystrokes and 200 undos,
and the planet simulation stepped 20,000 times. Each run once to warm up,
then timed as the mean of several with `os.clock`.

On this Mac (Apple Silicon, arm64), against Lua 5.4.8 built at `-O2` as
the guest's is - the build tree's host copy is `-O1`, which would have
flattered LuaJIT by a few per cent:

| Workload | Lua 5.4 | LuaJIT, interpreter | LuaJIT, compiled |
|---|---:|---:|---:|
| Lexer, `wm.lua` | 5.04 ms | 4.54 ms (1.11x) | 2.18 ms (2.3x) |
| Markdown, `roadmap.md` | 23.5 ms | 20.2 ms (1.16x) | 20.1 ms (1.17x) |
| Editor buffer, 20,000 keys | 35.6 ms | 23.6 ms (1.51x) | 14.5 ms (2.45x) |
| Planets, 20,000 steps | 130 ms | 95.6 ms (1.36x) | 30.0 ms (4.3x) |

LuaJIT 2.1 from its repository (`c6ffc14`, 8 September 2026), built on the
Mac. Mac numbers, not the Pi 5's - `CLAUDE.md` is clear that neither QEMU
nor this Mac is the target - but the ratios between two interpreters on the
same processor say what the choice is worth.

**Where the gain is**: the compiler, on numeric loops (the planets) and on
table-and-string work done in tight loops (the lexer, the buffer). The
interpreter on its own is 1.1 to 1.5 times - LuaJIT's hand-written
assembly against Lua's C, and not enough to pay for anything below.
Markdown, which is string building and pattern matching, is the same speed
either way: the time is in the string library, which is C in both.

## What LuaJIT does not have

It is Lua 5.1, with some of 5.2. Counted in Kosmos's own code, strings and
comments excluded:

| What | Uses | Files |
|---|---:|---:|
| Integer division `//` | 685 | 84 |
| Bitwise `&`, `\|` | 150 | 24 |
| Shifts `<<`, `>>` | 89 | 19 |
| `string.pack`, `unpack`, `packsize` | 94 | 11 |
| `math.tointeger`, `math.type` | 29 | 11 |
| `utf8` | 12 | 3 |

**96 of the 194 files use at least one.** Every one of those could be
rewritten - LuaJIT has a `bit` library and FFI - and a rewrite would be a
week of mechanical work and a new class of bug in each.

**The one that is not a rewrite is integers.** LuaJIT's numbers are
doubles: whole numbers are exact up to 2^53 and not past it. Kosmos
depends on 64-bit integers in places where losing the low bits is silent:

- **A file's date** (`kfs.stamp`, 27 September) is bit 62 with the seconds
  above bit 16. As a double it would not survive.
- **The counter**: at a PC's 3 GHz, `sys.ticks()` passes 2^53 after about
  34 days of uptime, and from then on every duration computed in Lua loses
  precision.
- **Every protocol** is `string.pack` over 64-bit fields.

LuaJIT's FFI has 64-bit integers as boxed values, which would mean every
place that does arithmetic on one of these changing how it does it.

## What the kernel would have to allow

LuaJIT's compiler writes machine code into memory and then runs it. Kosmos
does not allow that: the ELF loader refuses a segment writable and
executable at once (`elfimage.c`, step 2 of `elf.md`), and no process can
turn memory it wrote into memory it runs - there is no call for it. LuaJIT
can keep to "never both at once" by writing to a page and then flipping it
to executable, so what it would need is **a system call that changes a
region from writable to executable and back** - and a decision about who
may make it, since it is exactly the thing an attacker who has control of
a process wants. Without it, only the interpreter runs, which is the
1.1-1.5x column.

## Where the time goes today

`frames` on a busy desktop: **composing is 76 to 85 per cent of a pass,
and it is C** already; the Lua half - requests, layout, focus - is about a
tenth. A 2.5x faster Lua would make a busy desktop pass perhaps 6 per cent
faster. The garbage collector, which is what `CLAUDE.md` says decides a
server's language, is a separate question LuaJIT does not answer better:
its collector is incremental and not generational.

What LuaJIT would change is **applications that compute in Lua** - a game,
a simulation, the IDE's checking. For those, `CLAUDE.md`'s answer already
stands and has been measured: move the loop to C, in a kit, where the
scanner went from 538 ms to 4.7 ms.

## Recommendation

**Keep Lua 5.4.** The speed is real - up to 4x on numeric Lua - but it is
bought with the language's integers, a rewrite of half the files, and
executable memory the system is designed not to have; and the frame is
mostly C already. **Revisit it if** an application that must stay in Lua is
measured to be bound by the interpreter, and a kit cannot take its loop.

The alternatives worth more per hour, in order:

1. **A profile of any slow application first** (6l): the answer so far has
   always been one loop, and C.
2. **Lua 5.4 built with `-O2` everywhere**, which it is on the machine; the
   host tools' `-O1` only affects the build machine's tests.
3. **Allocation in the Lua on a deadline**, which `CLAUDE.md` found was
   worth four times the worst pause where the language question was worth
   a ninth.

Diego's to decide.
