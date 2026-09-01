# 1. What Kosmos is, and who it is for

## Why I am writing this

I have wanted to build an operating system since 1990.

That is a long time to want something. In between, I built a career in IT,
and I spent most of it around other people's operating systems - reading
them, running them, arguing about them, and taking notes. Monolithic
kernels and microkernels. Hybrids. Real-time systems. QNX, Solaris, IRIX,
Windows, Linux, the BSDs, macOS, OS/2 Warp, BeOS, Haiku.

Every one of them got something right. Not one of them got everything
right, and the reasons why are usually not technical - a company ran out of
money, or a decision made in 1988 could not be undone in 1998 without
breaking the customers who were paying for it.

So this book is the thing I have wanted to do for ages: take the parts I
love from all of those systems, put them into one system, build it, and
write down honestly what happened. Which parts fit together. Which parts
fought each other. What each decision cost.

Because that is the real subject here, and I want to say it on the first
page: **an operating system is a trade-off game.** There is no design that
only wins. Every choice buys you something and charges you something else,
and most of the interesting engineering is in knowing what you are buying
and what the bill is. A book that only lists the wins is a brochure. This
one tries to show both sides of every decision, including the ones where I
got the trade wrong and had to go back.

---

## 1.1 Start with what you see

Turn the machine on. Twelve lines go past on the serial cable, each one
naming a thing that just started working - the MMU, the timer, the
interrupt controller - and then a desktop appears. There is a bar along the
bottom with the programs you can run. You open a Terminal, type `ls`, and a
list comes back. You drag a window and it moves.

None of that is unusual. That is the point. What is unusual is underneath.

Almost nothing you just used is part of the kernel. The thing that listed
the files is a program. The thing that drew the window is a program. The
thing that read the keyboard is a program. If any of them crashed, the
others would keep running, and the one that crashed could be started again
without rebooting the machine.

That arrangement has a name, and the name is the first thing to explain.

---

## 1.2 Two names, and they are not interchangeable

**Kosmos** is the operating system. The desktop, the servers, the shell,
the applications, the filesystem, the window manager. Everything you can
see.

**Nebula** is the microkernel underneath it. Threads, address spaces,
message passing, capabilities. That is the complete list. It does not know
what a file is. It has never heard of a window.

Almost every time somebody says "the kernel" they mean Nebula, and almost
every time somebody says "the system" they mean Kosmos. Keeping the two
apart is not pedantry - most of this book is about which of the two a given
job belongs to, and the answer is nearly always Kosmos.

---

## 1.3 What a microkernel actually means

Here is how an operating system is usually drawn.

```
    A conventional system

    +-------------------------------------------------------+
    |  your programs                                        |
    +-------------------------------------------------------+
    |                                                       |
    |                      THE KERNEL                       |
    |                                                       |
    |    scheduler    memory     files      network         |
    |    drivers      terminal   graphics   sockets         |
    |                                                       |
    +-------------------------------------------------------+
    |  hardware                                             |
    +-------------------------------------------------------+
```

The kernel is the widest box. Everything that matters lives inside it, and
your programs sit on top because there is nowhere else to sit. The
filesystem is kernel code. The terminal is kernel code. Drawing on the
screen is kernel code.

That has a real consequence: all of that code runs with complete authority
over the machine. A bug in the graphics driver is not a graphics problem.
It is a machine problem, because that code can write to any address it
likes.

Kosmos is drawn the other way round.

```
    Kosmos

    +---------+  +---------+  +---------+  +---------+
    |  file   |  | window  |  |  disk   |  |  your   |
    | server  |  | manager |  | server  |  | program |
    +---------+  +---------+  +---------+  +---------+

       every one of these is an ordinary process, with its own
       memory, unable to touch the others

    +-------------------------------------------------------+
    |  NEBULA                                               |
    |  threads . address spaces . messages . capabilities   |
    +-------------------------------------------------------+
    |  hardware                                             |
    +-------------------------------------------------------+
```

Now the kernel is the *narrowest* box. Almost everything the first picture
put inside it has moved out and become a process standing beside your
program rather than underneath it.

The filesystem is a process. The window manager is a process. The thing
that talks to the disk is a process. They are not special. They get started
the same way your program does, they can be stopped, and if one of them has
a bug it takes itself down and nothing else.

**That is the whole idea of a microkernel.** Push everything out of the
privileged core that does not absolutely have to be there, so that a
mistake is contained by the hardware instead of trusted not to happen.

### And here is the bill

I said this book shows both sides, so here is the other side of that
picture, immediately.

Those processes now have to talk to each other, and every conversation
crosses a boundary that used to be a plain function call. In the first
diagram, the filesystem calling the disk driver is a `call` instruction. In
the second, it is a message: a context switch out, a context switch back,
and the kernel in the middle.

That is not free, and it is the reason microkernels got a bad reputation in
the 1990s. Mach was slow enough that the argument looked settled to a lot
of people. It was not settled - QNX was shipping fast microkernels the
entire time, and L4 later showed the cost could be cut by more than an
order of magnitude - but the cost is real and it never goes to zero.

I measured it on this system. One crossing between Lua and C costs about
nine microseconds, which is roughly the time it takes to write two thousand
pixels. That single number ended up deciding the design of the graphics
layer, and there is a whole chapter about how. For now the point is only
this: **isolation is bought with communication cost.** That is the trade.
Much of the rest of the book is about whether it was worth it, and where it
was not.

---

## 1.4 The part that is not standard

Microkernels are not new. QNX shipped one commercially for decades, and L4
and seL4 are the serious research lineage. If Kosmos stopped here it would
be a small QNX with fewer features.

The unusual decision is the userland. **Everything above the kernel is
written in Lua.**

Not scripting on top of a system written in C. The system itself. The
filesystem server is Lua. The window manager is Lua. The shell, the
compositor, the process viewer, the text editor, the games. Lua.

The C that remains does exactly two jobs: it touches hardware, or it defines
the boundary that keeps processes apart. The test I use throughout the
project is one sentence:

> If a bug there can corrupt another process, it is C.
> If it can only kill its own process, it is Lua.

That leaves the kernel in C, the Lua interpreter itself in C, and the
handful of primitives that write pixels in a tight loop. Everything else -
which is most of the system by line count - is Lua.

**What that buys:** the system can be changed while it is running. A server
can be given new code without losing its state or its clients. That is the
Lisp Machine idea, and it is the reason for nearly every other decision in
the design.

**What it costs:** speed, in the places where speed comes from being close
to the metal, and a garbage collector living inside the system rather than
outside it. Both of those show up later as real problems with real numbers,
and both of them shaped the code.

---

## 1.5 The thesis

Here is the sentence the whole project is built on, and it is worth reading
twice.

> **The protocol between servers is the data model of the userland
> language.**

In plain terms: when one process talks to another, the message it sends is
a Lua table. Not a byte buffer that both sides agree to interpret the same
way. Not a struct defined in a header both sides include. Not something
described in an interface definition language and code-generated into two
stubs. A table - the ordinary thing you write in Lua every day.

```
   your program                          a server
   ------------                          --------

   { type = "read",         ------->     receives that table,
     path = "/dev/cpu" }                 looks at type and path

                                         builds a reply table
   { implementer = "ARM",   <-------     and returns it
     part = "Cortex-A72",
     cores = 1 }
```

Nothing is marshalled. There is no schema. A server is a function that
receives a table and returns a table, and both processes are speaking their
native tongue the entire time.

I have written code against IDLs and generated stubs in more than one of
the systems in my list, and the work they create is never in the
interesting part of the problem. It is in keeping two descriptions of the
same thing in agreement. Removing that entirely is worth a great deal - and
it is only possible because both ends of every conversation speak the same
language, which is a constraint and not a free lunch. It is the price of
admission for everything in this section.

---

## 1.6 One protocol, and what it buys

On a conventional system, doing five ordinary things means learning five
different mechanisms:

| What you want | How you get it on Linux |
|---|---|
| read a file | `open` / `read` |
| read a sensor | ioctl, or a sysfs file with a made-up text format |
| list the processes | read and parse `/proc` |
| watch for changes | inotify |
| read metadata | `stat`, or xattr calls |

Five APIs. Five sets of error conventions. Five things to learn.

On Kosmos it is one. Each of those is a path you read, and each answers
with a table. These are real, and they work today:

```lua
    fs.read("/data/notes.txt")   -- a file, on the actual disk
    fs.read("/dev/cpu")          -- which processor this turned out to be
    fs.read("/dev/memory")       -- how much there is, and how much is left
    fs.read("/dev/screen")       -- the display's size and layout
```

The client code is identical. It does not know or care that the first one
came off the disk through a driver and three servers, and the second was
assembled on the spot out of a register the processor was asked to describe
itself with.

Being straight about the current state: the list above is not yet the
*whole* machine. Reading the process table is still a system call rather
than a path, and there is no networking at all, so `/proc` and `/net` are
in the design and not in the code. They are named here because the shape is
decided, not because you can type them today. A book written alongside a
system has to be careful about that difference, and this one tries to be:
where something is planned rather than built, it says so.

**The question this project exists to answer is exactly that:** how much
does a system actually simplify if every resource speaks one protocol, and
every process sees only what you handed it? Not "would it be nicer" - it
obviously would - but how much, measured by building the thing.

---

## 1.7 What a process is allowed to see

There is a second idea doing as much work as the first, and it changes what
security means here.

A process on Kosmos has a **namespace**: a small list of what it can reach.
Not a view of a big global tree with permissions on the branches. There is
no big global tree. If `/data` was not put into a process's namespace, then
for that process `/data` does not exist - not "permission denied", but *no
such path*.

This is not a description of how it ought to work. There is a program in
`/bin` called `hello` whose entire job is to print its own namespace, and
this is what it says:

```
    Hello from a process of my own.

      what I was given:  /app  /bin  /data  /dev  /dev/console  /home  /lib

    I cannot reach anything that is not on that list. There is no
    global filesystem to walk and no name to guess: a capability is an
    index into my own table, and I only have the ones I was handed.
```

Seven entries. That is the entire world that program can see. A different
program started with a shorter list has a smaller world, and there is no
call it can make to widen it - not one that fails, one that does not exist.

If a game is handed `/dev/wm` so it can open a window, and nothing else, it
cannot open your files. Not because it is forbidden. Because it has no way
to name them.

The mechanism underneath is **capabilities**. A process holds a small table
of handles it was given, and a system call takes an *index into its own
table*, never a global name. There is no string a process can guess its way
to something with. If it was not handed to you, you cannot ask for it.

This is why Lua does not need to be sandboxed here, which surprises people.
Lua is not a secure language and this system does not pretend it is. The
isolation is not in the language - it is in the hardware. Each process runs
at the unprivileged level with its own page tables. If a program breaks out
of Lua completely, it has broken out into its own address space, where
there is nothing to steal and nothing to break but itself.

**The trade:** this is genuinely stronger than file permissions, and it is
genuinely more work to set up. Somebody has to decide what every program
gets, and there is no "just give it everything" fallback - because that
fallback is exactly what would make the whole mechanism decorative. Whether
the inconvenience is worth the guarantee is a fair question, and I will
come back to it once there are enough programs here to have an opinion
worth having.

---

## 1.8 Where the ideas came from

Nothing here is invented. Nearly every piece of Kosmos worked somewhere
before, usually in a system that had commercial reasons not to take it
further. What is new, if anything is, is the combination.

| Idea | Where from | What it solves |
|---|---|---|
| Microkernel, messages as the only primitive | QNX, L4 | A driver failing does not take the machine down |
| Per-process namespaces, one protocol | Plan 9 | Isolation and uniformity from one mechanism |
| Typed attributes and live queries in the filesystem | BeOS | Real search without an indexer bolted on |
| One thread per window, input at top priority | BeOS | The feeling of a machine that responds |
| A live image you can inspect and change while it runs | Lisp Machines | Iterating without recompiling |
| A complexity budget as a design constraint | Oberon | Fitting the whole system in one head |
| Capabilities instead of permissions | seL4 | You reach exactly what you were handed |

That table is the short version, and it does the sources an injustice. What
each of those systems taught me is not really a row in a table - it is
usually a specific moment of using the thing and noticing something. IRIX
felt different from Solaris in a way it took me years to be able to name.
QNX taught me that the performance argument against microkernels was an
argument about *one* microkernel, not about microkernels. BeOS is the one I
still miss.

That is the next chapter, along with the things I looked at just as
carefully and deliberately left out - which are as interesting as the
inclusions, and sometimes more.

---

## 1.9 What Kosmos is not

**It is not going to be your daily machine, and chasing that would kill
it.** A daily machine needs a browser. A modern browser is around thirty
million lines of code that assume POSIX, threads, a JIT compiler, a GPU and
a full network stack. Porting one is more work than the entire operating
system it would run on. Haiku hit that wall with twenty years and dozens of
people, and Haiku had a far easier target than this one.

**It is not a BeOS clone,** although the debt is obvious and there is a
whole chapter about it. BeOS was monolithic - drivers and the filesystem
lived inside the kernel, and nothing could be reloaded while running.
Kosmos takes its concurrency model, its live queries and its sense of
taste. The architecture is QNX's.

**It does not chase compatibility at the system level.** A library with
little system surface - SQLite, zlib, a font rasterizer, Doom - can be
ported with a small libc that lives inside the process and resolves its
file calls against that process's own namespace. That is fine, and it is
planned. What is refused is a POSIX personality: `fork`, signals, sockets,
a global `/`.

That is the hardest trade in the whole project, so let me be honest about
what it costs. Refusing POSIX means refusing almost every program that
already exists, and there is no shortcut back. In exchange, the namespace
design stays load-bearing instead of becoming decoration - which is exactly
what happened to Haiku, a system with queries, attributes and a beautiful
message system, where most software ended up running on POSIX and ignoring
all of it. I would rather have a small system where the ideas actually hold
than a larger one where they are a museum exhibit. That is a preference,
not a proof, and somebody could reasonably choose the other way.

---

## 1.10 The goal you can check

Vague goals produce vague systems, so there is a specific one.

> **Doom, at 35 frames a second, in a window on the desktop, on a Raspberry
> Pi, next to a prompt where you can redefine the window manager while the
> game is running.**

Doom is a better operating system benchmark than it looks. It needs no GPU
and no floating point on the hot path, but it does need a framebuffer with
a fast blit, input that arrives with low latency, timing that is actually
accurate, real file reads, and memory managed by the application. If it
runs smoothly, the foundations are sound. If it stutters, something is
genuinely wrong and can be found.

The second half of that sentence is the harder half. Redefining a running
window manager, without restarting it, without the windows it is managing
noticing - that is the Lisp Machine idea, and it is the reason the userland
is a dynamic language in the first place.

---

## 1.11 Who this is for

**This book is for somebody who wants to understand how an operating system
works,** and who has found that reading about them is not the same as
watching one being built and being told why each decision went the way it
did.

You should be comfortable reading code. C and Lua both turn up, and neither
is used cleverly - if a piece of code here needs a trick to understand,
that is a bug in the code. You do not need to have written a kernel. You do
not need to know AArch64 assembly; the parts that matter get explained
where they come up.

**What you will not find is a tutorial.** This is not "build your own OS in
twelve steps". The chapters are areas, in the manner of the Be Book: each
one readable on its own, and ordered so that front to back also works.

**What you will find is the reasoning, with both sides of it.** Every real
decision here is written down with the argument that produced it, what it
bought, what it cost, and what the alternative would have been. Including
the decisions that turned out wrong. This project has changed its mind in
public several times - a rule that got demoted when it stopped being true,
a test that passed while the bug was still sitting there, a fix that broke
something else on its way past. Those are the most useful pages in the
book, and they only exist because the writing follows the building instead
of leading it.

---

## 1.12 One more thing, about why this is possible

There are no users. There is no compatibility to maintain. There is no
deadline, and nobody is waiting for a release.

That sounds like a limitation and it is the opposite. Every system in my
list made compromises that were not engineering decisions. Solaris carried
weight for its customers. OS/2 carried weight for IBM. BeOS ran out of
money before it ran out of ideas. Those compromises were rational, and they
were still compromises.

Here, when something turns out to be wrong, I can simply change it, because
there is nobody to break. When something is slow I can measure it properly
instead of patching around it, because nothing is shipping on Friday. That
freedom is the one real advantage this project has over every system it
borrows from, and most of what is interesting in this book lives in the
space it opens up.

The next chapter is the long version of where the ideas came from: the
systems I actually used, roughly in the order I met them, and what each one
turned out to be right about.
