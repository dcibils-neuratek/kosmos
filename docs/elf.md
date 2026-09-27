# Programs from a file

How a program that is not in the image is started: an ELF file, read from
`/Home`, made into a process. Written before it is built, as
`threads.md` was, and agreed with Diego on 27 September 2026:
"i want to go ahead and make the elf loader so we can start shipping a
really usable system with games on /home" (`roadmap.md` 6t). The layout it
installs into is `layout.html`.

---

## Why, in one paragraph

Every process in Kosmos is the same image entered at a different role, so a
program with C in it has to be compiled into that image. That is why Doom,
Quake and the Super Nintendo are in the MEGA build, why the system image is
a GPLv2 work whenever they are in it, and why nobody else's application
could be installed at all - Kosmos is rebuilt, not installed on. A program
loaded from a file changes that: a game is a folder in `/Home/Apps`, the
image that ships is the system and nothing else, and a person can take
their games to another machine with their home. It is the step
`roadmap.md` called the difference "between a system that is rebuilt and a
system that is used".

---

## The line this has to respect

**The kernel does not know what a file is** (CLAUDE.md). An ELF is a file
format, so reading one is not the kernel's business. What the kernel knows
is pages, address spaces, threads and capabilities - and that is exactly
what starting a program needs from it: pages put at addresses with
permissions, an entry point, and a thread. So the work divides the way the
rest of the system does:

- **A program in userland reads the file**, checks it, and lays its
  segments out in memory it owns. A malformed file is that program's
  problem and is refused there, with a sentence.
- **The kernel is handed pages and a map** - these bytes, at this address,
  readable and executable or readable and writable, never both - and an
  entry point, and makes a process of them. What it checks is what it has
  always checked about memory: that the addresses are the user's, that
  nothing overlaps, that nothing is writable and executable at once.

QNX puts its loader in the process manager beside the kernel; seL4 leaves
it entirely to userland. This is seL4's answer, for the same reason: the
kernel stays a thing that knows about memory, and the part that parses
bytes somebody else wrote is a process that can fail alone.
