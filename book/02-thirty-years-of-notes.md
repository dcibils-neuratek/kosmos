# 2. Thirty years of taking notes

## It started with a question I could not answer

In 1990 I was writing BASIC, and at some point I asked what I think
everybody asks eventually: what is *underneath* this? Something is deciding
when my program runs. Something is deciding what happens when I ask for a
file. Something turned my keypress into a character. What is that thing,
and who wrote it?

The honest answer took me about thirty years, and it turned out not to be
one answer. It is a long argument that has been running since the 1960s,
where every side is partly right, and where most of the famous decisions
were made for reasons that had nothing to do with computer science.

This chapter is the notes. Which systems I used, what each one was right
about, and - the part that matters more - what each one paid for being
right about it.

Because that is the shape of the whole subject. **There is no operating
system design that only wins.** Every one of these bought something with
something else, and the interesting question is never "was this good" but
"what did it cost, and did they know".

---

## 2.1 The argument, in one picture

Almost everything in this chapter is a position on one question: **how much
of the system runs with complete power over the machine?**

```
   MONOLITHIC                    MICROKERNEL              HYBRID
   Linux, BSD, Solaris,          QNX, L4, MINIX 3         Windows NT,
   IRIX, OS/2, BeOS                                       macOS (XNU)

   +--------------+              +--------------+         +--------------+
   | applications |              | applications |         | applications |
   +--------------+              +--+--+--+--+--+         +--------------+
   |              |              |fs|net|drv|..|          |  some servers|
   |    KERNEL    |              +--+--+--+--+--+         +--------------+
   | fs net drv   |              |   kernel     |         |    KERNEL    |
   |  sched mm    |              | sched mm ipc |         | + fs, net,   |
   +--------------+              +--------------+         |   graphics   |
   |   hardware   |              |   hardware   |         +--------------+

   one address space,            each box is its own      started as the
   one crash                     process; a crash is      middle one and
   takes everything              one box                  moved things in
                                                          for speed
```

The monolithic side says: crossing between address spaces is expensive, so
put everything on one side of the boundary and make the calls cheap.

The microkernel side says: a bug in the graphics driver should not be able
to overwrite the filesystem's memory, so put a hardware boundary between
them and pay for the crossings.

Both of those are true statements. That is why the argument lasted.

---

## 2.2 The Unixes: Linux, the BSDs, Solaris, IRIX

I spent most of my working life around these, and it is worth being clear
about what they got right, because it is easy to spend a book describing an
alternative and forget to say why the mainstream won.

**What Unix got right is the idea that a small number of general mechanisms
beats a large number of specific ones.** A file descriptor is the same
thing whether it is a file, a pipe or a socket. A process is a process. You
can compose programs you did not write into a pipeline nobody planned. That
is a genuinely deep idea, and every system in this book is downstream of
it, including this one.

Kosmos's "one protocol for every resource" is that idea taken further than
Unix took it - which I will come back to, because Unix stopped halfway and
the place where it stopped is instructive.

### Solaris: what it looks like when a company can afford to be serious

Solaris is where I learned what a mature system looks like when there is
money and twenty years behind it. Symmetric multiprocessing done properly
rather than bolted on. DTrace, which let you ask a running production
machine questions nobody had planned for you to ask. ZFS, which took the
position that the filesystem should not trust the disk - checksums on
everything, because disks lie and RAID controllers lie differently. Zones,
which were containers before the word.

**What it cost:** all of that lives in one kernel. DTrace is powerful
precisely because it can instrument anything in that one address space, and
that is also the reason it needs total privilege to exist. The design and
the risk are the same property seen from two sides.

### IRIX: a Unix bent for one job

IRIX I remember as feeling *different* in a way that took me a long time to
be able to name. It was Unix with SGI's real-time work in it, and it was
built around getting graphics and audio out on time rather than getting the
most total work done. Those are not the same goal, and the second one is
what a general-purpose scheduler optimises for.

XFS came from there too, and it was ahead of its time - extents, allocation
groups, built for large files when large meant something different.

**What it taught me** is the thing this project cares most about:
**throughput and latency are different, and a system usually has to choose.**
A machine that finishes the most work per hour and a machine that always
responds within sixteen milliseconds are tuned differently, and if you do
not decide which one you are building, you get the first one by default -
because the first one is what benchmarks measure.

BeOS made the same choice as IRIX, from a completely different direction.

### Linux and the BSDs

Linux is the monolithic argument's strongest case: it won, comprehensively,
and it won partly *because* of its structure. Loadable modules gave it
enough of the modularity benefit to take the pressure off, everything in
one address space made it fast, and the licence and the community did the
rest.

The BSDs are the same shape with a different temperament - a whole system
developed together rather than a kernel plus a distribution, which shows in
how coherent they feel.

**What the monolithic design costs is not a secret and not a scandal:** the
attack surface and the crash surface are the same size as the kernel, and
the kernel is enormous. A driver bug is a system bug. That has been managed
extremely well for thirty years with review, testing, and eventually with
things like eBPF's verifier - but it is managed, not eliminated.

---

## 2.3 OS/2 Warp: the most important lesson in this book

OS/2 is where I learned the thing that decided the single most restrictive
rule in Kosmos, so this section is longer than its size in my life
justifies.

The Workplace Shell was genuinely ahead of everything. Not "icons on a
desktop" - *objects*, with real inheritance, that you could subclass; a
folder was an object, a printer was an object, and dragging one onto
another was a message between them. People who used it seriously still miss
it, and I understand why.

And IBM made a decision that seemed obviously correct: **OS/2 would run
Windows applications.** There was a whole Windows environment inside it. It
was marketed, fairly, as a better way to run Windows software than Windows
was.

Here is what happened.

```
   A developer in 1994, deciding what to write for:

        write for OS/2                      write for Windows
        ---------------                     -----------------
        runs on OS/2                        runs on Windows
                                            AND runs on OS/2

   The second column is strictly better. Every rational developer
   picks it. The Workplace Shell's API gets used by almost nobody,
   including by people whose software runs on OS/2 every day.
```

The compatibility layer did not fail. It worked, and that was the problem.
It removed the only reason anybody had to learn what was distinctive about
the system. OS/2 ended up as a very good place to run other people's
software, and the ideas that made it worth having became a thing enthusiasts
described to each other.

**The lesson is not "compatibility is bad".** It is sharper than that:

> A compatibility layer competes with the native design for the same
> developers. If the layer is good, it wins, and then the native design is
> decoration.

That is why `fork`, signals, sockets and a global `/` are refused in
Kosmos, and it is the hardest cost in the whole project - it means refusing
almost every program that already exists. Chapter 1 put the trade honestly
and I will not soften it here. But this is where the conviction came from.
It is not theory. I watched it happen to a better system than mine.

Haiku, later, is the same story with a happier tone and the same ending.

---

## 2.4 QNX: the microkernel that was not slow

The standard story about microkernels goes like this: they were tried, in
Mach, and they were slow, and the argument was settled in the 1990s.

That story is wrong, and QNX is the reason I know it is wrong. QNX shipped
a microkernel commercially for decades, in cars and medical equipment and
industrial control - places where "the system paused for 40 milliseconds"
is not a complaint, it is an incident. It was fast, and it was fast in the
way that mattered: predictably.

The whole system is built on three operations:

```
    a client                              a server
    --------                              --------

    MsgSend  ------------------------->   MsgReceive
       (blocks; the kernel switches
        directly to the server)
                                          ... does the work ...
    returns  <-------------------------   MsgReply
```

Send, receive, reply. A driver is a process. The filesystem is a process.
The network stack is a process. You can kill the network stack and start it
again, on a running machine, and the machine keeps running.

**Why it was fast where Mach was slow** is worth understanding, because it
is the difference between "microkernels are slow" and "that microkernel was
slow". Mach's IPC was asynchronous and buffered: messages were queued,
copied, managed. QNX's is a synchronous rendezvous - the sender blocks, the
kernel switches straight to the receiver, and nothing is queued because
nothing needs to be. Later, L4 showed the same thing in a research setting
by cutting IPC cost by more than an order of magnitude, and seL4 went
further and *proved* its kernel correct.

Kosmos's IPC is QNX's shape, for QNX's reasons. Nebula's messages are a
synchronous rendezvous with nothing buffered in the kernel.

**What it costs**, and chapter 1 already named the number: every one of
those arrows is a boundary crossing. On this system a Lua-to-C crossing
costs about nine microseconds, which is roughly the time to write two
thousand pixels. That is not a rounding error, and there is a whole chapter
about the rule that follows from it. Asynchrony, when it is genuinely
needed, gets built on top in userspace - which is also QNX's answer.

---

## 2.5 BeOS: the one I still miss

BeOS is the system this one owes its personality to.

It was built, from the start, around the machine feeling *immediate*.
Everything was threaded - every window had its own thread, so a program
busy doing something could not stop its own window from redrawing, let
alone anybody else's. Input ran at high priority because input is what the
person is doing. The whole system was tuned for latency in the way IRIX
was, but for a desktop rather than a graphics workstation.

And then there was the filesystem, which is the part I have never really
got over.

```
   The ordinary way                      The BeOS way
   ----------------                      ------------

   filesystem: names and bytes           filesystem: names, bytes,
                                         and typed attributes
        +                                
   an indexing service that              queries are answered from
   walks it all and keeps its            indices the filesystem
   own database somewhere                maintains as part of writing

   two things that can disagree          one thing
```

A file in BFS had typed attributes - not a magic number guessed from the
contents, actual typed fields. Attributes could be indexed. Queries against
those indices were a filesystem operation, live, and fast regardless of how
many files there were.

The consequence people remember is the email client: every message was a
*file*, with `From` and `Subject` and `When` as attributes, and your inbox
was a query. There was no mail database. The filesystem was the database,
and any program could participate without asking anyone's permission.

**What it cost.** Two things, and both are honest.

First, BeOS was monolithic. Drivers and the filesystem lived in the kernel,
and nothing could be replaced while the system ran. The responsiveness came
from good engineering inside one address space, not from isolation.

Second - and this is the more interesting one - **they tried the ambitious
version first and pulled it.** Early BeOS had a real relational database
underneath the filesystem. It was pulled before release because it was
hideously complex to maintain and cost too much performance, and it was
replaced with a filesystem *shaped like* a database: attributes and
indices, no general query engine. They lost very little that anybody
missed.

That is the most useful warning in this book, and Kosmos takes it directly:
build the specific thing that solves the actual case, not the general
machine that could solve cases nobody has.

---

## 2.6 Haiku: what happens when you keep the ideas and add POSIX

Haiku is a remarkable piece of work - an open-source reimplementation of
BeOS that actually runs, with the queries and the attributes and the
message-passing API intact.

It also has a POSIX layer, because otherwise there would be almost no
software for it.

And so most software on Haiku is POSIX software, and it ignores the
attributes, ignores the queries, and ignores the message system. The good
ideas are still in there. They are just not what anything is built on.

It is OS/2's lesson again, on a system I like a great deal, and it is the
second reason chapter 1's refusal is written the way it is.

The other thing Haiku taught me is about scope. Twenty years, dozens of
contributors, and the hardest single problem is the web browser - because a
modern browser is thirty million lines that assume a POSIX system, threads,
a JIT, a GPU and a full network stack. **Any project that wants to be
somebody's daily machine has signed up to that**, and this one has not.

---

## 2.7 Windows NT and macOS: two hybrids, and what "hybrid" really means

Both of these are microkernel-influenced designs that moved things back
into the kernel for speed. That sounds like an accusation. It is not - it
is the trade-off being made deliberately, and it is worth studying because
of how it turned out.

**Windows NT** was designed with a small executive and subsystems around
it, and the graphics and windowing system lived in userspace. In NT 4, it
was moved into the kernel, because the crossings cost too much and the UI
felt slow.

It worked. It was also, for years afterwards, one of the largest sources of
crashes and of privilege-escalation bugs in the system, because a font
parser and a window manager were now running with full power over the
machine.

**That is the single most relevant precedent for this project**, because
Kosmos has put its window manager in userspace, and written it in Lua, and
declared that it will stay there. NT tried that and retreated, on hardware
where the crossing was more expensive than it is now and with a workload
that had no other choice. If Kosmos's compositor is too slow, that is not a
surprising outcome. It is the outcome the most heavily-resourced attempt at
this got.

**macOS's XNU** is Mach and BSD in one address space. The Mach parts give
it the structure; the BSD parts give it the Unix system; putting them in
one address space avoids paying for the boundary between them. Apple made
the pragmatic choice and shipped the most successful desktop Unix there has
ever been.

Neither of these is a failure of nerve. They are engineers looking at a
measured cost and deciding it was too high. The right response to that is
not to ignore them; it is to measure, on your own hardware, and to be
willing to find out they were right.

---

## 2.8 The ones I read about rather than ran

Three systems shaped Kosmos that I never used in anger, and honesty demands
that distinction.

**Plan 9** asked what Unix would look like if it actually finished the job.
Unix says everything is a file and then makes exceptions - `ioctl` is the
admission that some things are not. Plan 9 removed the exceptions, and
added the idea Kosmos leans on hardest: **the namespace is per-process.**
Not one tree with permissions on the branches; a private view assembled for
each process out of what it was given. That is where chapter 1's "what you
did not mount does not exist" comes from.

**The Lisp Machines** were systems where the running image was the system,
inspectable and changeable while it ran, with no distinction between the
thing you were building and the thing you were using. Kosmos's whole
userland is a dynamic language for that one reason. They also died, and
they died because they needed custom hardware to be fast - which is why
this runs on boards you can buy.

**Oberon** treated the *total complexity of the system* as a hard design
constraint, on the grounds that a system should fit in one person's head.
That constraint is the reason this project keeps an eye on its own size,
and the reason the kernel refuses to learn what a file is.

---

## 2.9 The notes, collected

Everything above, as a table. Left column is what I took; right column is
what taking it costs, because a list of features without its bill is a
brochure.

| From | What Kosmos takes | What it costs |
|---|---|---|
| QNX, L4 | Microkernel; synchronous rendezvous IPC as the only primitive | Every service call is a boundary crossing |
| seL4 | Capabilities: you reach what you were handed, by index | Somebody must decide what each program gets |
| Plan 9 | Per-process namespaces; one protocol for every resource | No global tree means no familiar paths |
| BeOS | Typed attributes, indexed queries, latency as the goal | Indices must be maintained on every write |
| BeOS (DR8) | *Not* building the general database first | Some queries stay impossible for now |
| IRIX | Latency and throughput are different goals | Choosing latency costs total throughput |
| Lisp Machines | A live image, changed while it runs | A dynamic language in the system's hot paths |
| Oberon | Complexity as a constraint | Features get refused for being features |
| OS/2, Haiku | Refuse a system-level compatibility layer | Almost no existing software will run |
| Solaris | Take observability seriously | Everything must be inspectable by design |

---

## 2.10 What I looked at and left out

Four, with reasons, because the rejections say as much as the choices.

**A single-level store, as the AS/400 had it** - one address space for
everything, with persistence as a property of memory rather than a thing
you do to files. It is beautiful and it needs tagged memory in hardware to
be safe. The AS/400 had that. A Raspberry Pi does not.

**SOM, OS/2's object model.** Interface definitions, generated stubs, two
descriptions of every interface to keep in agreement. The work it creates
is never in the interesting part of the problem. Kosmos's answer is that
both ends of every conversation speak the same language, so there is
nothing to describe twice.

**Custom hardware.** It is what killed the Lisp Machines and it is what
killed most of the interesting systems of that era. Whatever this is, it
runs on boards anybody can buy.

**Plan 9's purity about starting over.** Plan 9 was right about nearly
everything and asked you to throw away everything you had. Kosmos uses the
firmware that is already on the board, boots the way the board boots, and
takes the hardware as it finds it.

---

## 2.11 Why this is one system and not a list

Reading back over that table, the obvious objection is that it is a
shopping list - the good bits of eight systems, which is what everybody
thinks they are building right up until the parts refuse to fit.

The thing that makes it one system rather than a collage is the sentence
from chapter 1:

> The protocol between servers is the data model of the userland language.

Look at what that single decision does to the list. QNX's message passing
needs a message format, and the format is a Lua table, so there is nothing
to marshal. Plan 9's one-protocol-for-everything needs a universal
representation of a resource, and a Lua table is one. BeOS's typed
attributes need somewhere to put types, and a table has them. The Lisp
Machine's live image needs code that can be replaced at runtime, which is
what a dynamic language does.

They fit because they are all the same decision seen from different sides.
That is the bet the project is making, and whether it comes off is what the
rest of this book is about.

The next chapter is about how the work is actually done - the method, the
tests, and why the writing follows the building instead of leading it.
