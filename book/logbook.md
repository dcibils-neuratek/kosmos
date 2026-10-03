# The logbook

Every major decision and development, as it happens. Each entry opens with
**In short** - what happened, for anyone, without the technical words - and
then the technical account: what was done, why, what a person using Kosmos
notices, and how it was measured.

Diego, 2 October 2026: "explain in simple terms then conclusions and
measurements made for every major decision and development", "so we will
use all that for our book", and "technical explanation is good but we need
to have a high level explanation for all major points".

This is the book's source material, not a chapter. `docs/testing.md` has
the full account of every measurement, under the number given here.

---

## 1 October 2026 - the browser, faster

**In short:** web pages now appear sooner, the browser keeps responding
while a page loads, and a load can be stopped.

**Memory copied a word at a time.** Every program copies and clears memory
constantly, and Kosmos's own C library did it one byte at a time. A
profile of the browser laying out a long Wikipedia article found 9% of its
time in those byte loops. Now they move eight bytes at a time. *Measured*
by counting instructions under QEMU: the article's layout took 12% fewer,
its parse 5% fewer. *Noticeable*: a little, on big pages; and every program
gets it, not only the browser. (`testing.md` 18.329)

**A page is read while it downloads.** The browser used to wait for the
last byte, then unzip the page, then read it. Now it unzips and reads each
piece as it arrives. *Measured*: Wikipedia's "Dam" article spent 300 ms
being read; now that happens during the download, and 0.1 ms is left after
the last byte. *Noticeable*: a large page appears sooner after it finishes
arriving. One exception: under QEMU's x86 emulation the load got slightly
slower, because QEMU runs an emulated PC's processors one at a time on this
Mac, so reading and downloading cannot overlap there. (18.330)

**Pictures arrive in the background.** Once a page is shown, the pictures
further down are fetched while you read, so scrolling to them no longer
stops to wait. (18.331)

**A page load you can walk away from.** The window used to freeze until a
page finished. Now it keeps drawing and answering while a page loads, and
Escape, a new address, Back or switching tabs stops the load. Building
this found three old faults, all fixed: closing a page could crash the
browser; a lone Escape did nothing until the next key; and the network on
the x86 machine took nine seconds for a page it should fetch at once.
(18.332)

## 1 October - the browser's smaller pieces

**In short:** drop-down menus work, favorites can be rearranged by
dragging, serif text looks serif, and more page styles load.

- **Drop-down menus** - a page's `<select>` opens the system's own menu,
  and a list taller than the screen is split into submenus.
- **Favorites dragged** along the bar, and the star dragged onto it.
- **Serif text** - pages that ask for a serif face get IBM Plex Serif.
- **Stylesheets that import other stylesheets** are fetched, chains of
  them included, and a circle of imports ends.

## 1 October - the disk server, all in C

**In short:** the part of Kosmos that reads and writes your files was
rewritten from Lua into C on 29 September, because the C version is about
twice as fast. On 1 October the old Lua copy, left behind unused, was
deleted.

**Why C, 29 September.** Disk Benchmark showed 38% of a random file read
going to walking the file's path in Lua, and Diego had said on 14 September
"if you need to take the filesystem from lua to c do it". *Measured* under
QEMU on the same disk, Lua server against C server: random 4 KB reads 1,796
a second to 3,824 (2.1x); looking up a file 350 to 235 µs; reading 4 KB 410
to 276 µs; changing a file's attributes 1,912 to 548 µs (3.5x); sequential
read and write 437 and 202 MB/s to 514 and 301. The same measurement found
the Lua server failing outright on a search with 200 answers, which did not
fit in one message; the C server sends long answers in pages. (18.283)

**Why delete, 1 October.** Once the C server ran, the Lua one never did: it
starts before Lua is even loaded. The 4,910 Lua lines had been kept as a
reference - tests held the new C to the old Lua block for block - and after
a few days in use the duplicate had become a cost: a second copy of the
disk's format to keep in step, and code nobody runs, which is where stale
assumptions hide. The tests now check the C directly. *Noticeable*:
nothing, since the deleted code was not running; the speed came with the
first step. (18.337)

## 1 October - the pointer drawn by the display, under QEMU

**In short:** in the emulator, moving the mouse no longer makes the desktop
redraw; the graphics card draws the pointer itself.

When QEMU's virtual graphics card is in use (`make GPU=virtio qemu`), the
display draws the mouse pointer, so a move costs no frame. *Measured*: with
the pointer moved, 0 of 160 pixels of the composed frame changed, against
77 when the desktop draws it. *Noticeable*: only with that virtual card;
the M700 needs a driver for its own GPU to get the same. (18.338)

## 1 October - every file in /Home read as empty

**In short:** for a while every file looked empty. Nothing was lost - the
system had lost track of the disk when the Mac got very busy. It now waits
patiently for a slow disk and never gets confused that way again.

**What happened.** In the middle of a session Tracker showed every file at
0 bytes and every folder as a file, and a film closed as soon as it opened.

**Why.** The kernel's disk driver waited for each disk request by counting
- about a second's worth - and then gave up. The push's test run was
filling this Mac with emulators, so one disk read took longer than that.
The driver gave up while the disk was still working on the request, and
from then on it was one request behind: every answer it read belonged to
the request before. What the disk server already had in memory still
worked, which is why folders still listed.

**The fix.** All five such drivers (disk, screen, randomness, sound) wait
by the clock now, thirty seconds, and a device that truly never answers is
reset rather than left half-way, so it can never write into memory now
used for something else.

**Impact.** The files were never damaged: all 400 on that disk read back
whole, and 39 matched the originals byte for byte. *Measured* with a disk
deliberately slowed to 16 KB a second: the old kernel failed after 1.9
seconds on ARM and 0.4 on x86; the new one waits and reads everything
correctly. That test runs in every gate now. (18.339)

## 1 October - a thread interrupted by mistake

**In short:** a rare scheduling mix-up was fixed; it showed up only as an
occasional false failure in testing.

A "switch threads soon" signal in the scheduler was cleared in one place
only, so it could linger and interrupt a thread that had just started,
putting it behind others. It made one test fail about 3 times in 100 under
load. *Fixed* by clearing the signal whenever the scheduler chooses; a new
test catches it every time instead of 3% of the time. (18.340)

## 1 October - Japanese, Korean and Chinese text

**In short:** Kosmos can now show Japanese, Korean and Chinese. The fonts
for them are large, so they are kept on the disk and loaded only the first
time a page or a file name needs them.

**What.** Kosmos's font covered Latin, Greek and Cyrillic, so these
languages showed as `????`. The four IBM Plex faces for them are 22 MB -
the whole system is 35 - so, by Diego's choice, they live in `/Home/Fonts`
and a program reads one only when it first needs a character from it.
Chinese characters follow the page's language (Japanese, Korean,
Simplified or Traditional Chinese).

**Measured** in the browser: eight Japanese characters drew 239 pixels
wide and eight Korean 214, against 115 for eight `?`; the faces were read
only when the page needed them, and no Chinese face was read for a page
that said it was Japanese. A program's memory of drawn characters also
stopped at 128 - one paragraph of Japanese - and now grows.

**Not yet.** Each program that draws these characters keeps its own copy of
the face. One shared copy needs a font server, planned for when a person
can install fonts of their own. (18.341)

## 2 October - a test that could start too early

**In short:** a test was fixed that occasionally failed for a reason of its
own, not because anything in Kosmos was wrong.

A kernel test made a helper thread and gave it permission one line later,
assuming it could not run in between. It could, if the clock ticked there.
It now gives permission before the thread can run, as every other test
already did. (18.342)

## 2 October - 3D under QEMU, paused

**In short:** the plan to use this Mac's graphics card for 3D inside the
emulator hit a wall in the tools; the recommendation is to spend the effort
on the M700's real graphics instead.

QEMU will only pass 3D to the Mac's GPU (virgl) through a graphics library
built a particular way that Homebrew does not provide. The ways around it
are a version that draws on the processor instead of the GPU - which
defeats the purpose - or building Google's ANGLE, a very large build. The
M700 needs its own driver anyway. Diego's to decide. (`roadmap.md` 4h d)

## 2 October - the M700's graphics, planned

**In short:** the M700's graphics chip can make the desktop faster mainly
in two ways - drawing the mouse pointer itself, and showing a finished
frame without copying it - so those come first.

The part of that GPU that copies and fills rectangles cannot blend
transparency, and blending is most of what the desktop does. So the first
wins come from the part that shows the picture on the screen:

1. **the pointer drawn by the display**, so moving the mouse redraws
   nothing;
2. **page flipping** - the desktop draws into a second screen buffer and
   the display switches to it - which removes copying 19.8 MB to the screen
   for every frame at the M700's 3440x1440.

Then the copy engine for solid areas, then the 3D engine for blending. The
first step is to measure the desktop on the M700 (`frames 30` in a
Terminal). Nothing of this runs under QEMU, so each step is a stick.
(`docs/m700-2d.md`)

## 2 October - messages between programs, cheaper

**In short:** programs in Kosmos talk to each other constantly, and each
conversation had become slower when Kosmos learned to use four processors.
The slow parts were found and removed: each conversation now does about
13% less work.

**Why.** In Kosmos almost everything is a message between programs -
reading a file, updating a window, asking for a name. A message and its
answer (a "round trip") had become 70% more expensive in September, and
nobody had found exactly why.

**How it was measured.** A small QEMU plugin counted every instruction the
processor ran during 100,000 round trips and added them up by kernel
function - for today's kernel and for the kernel of 5 September. Exact,
not sampled: 603 instructions a round trip then, 1,091 now.

**What was found.** The seven locks a round trip takes are needed for four
processors to be safe. What had grown was around each lock: a function call
to ask "is the machine crashing?" on every lock and unlock, more calls to
find out which processor is running, the lock's slow path prepared even
when the lock was free, and copying messages that were empty.

**The fix.** Those checks are now single instructions, the slow path runs
only when a lock is actually busy, and empty messages copy nothing.
*Measured*: a round trip from 68.1 to 59.4 units (−12.8%), a thread switch
from 13.4 to 12.8 (−5.1%). *Noticeable*: on its own, probably not - a round
trip is about a microsecond on real hardware - but every program pays it
thousands of times a second. (18.343)

## 2 October - should the window manager be written in C?

**In short:** the window manager is already part C (all the drawing) and
part Lua (the decisions). Measured: Lua is most of the window manager's own
work, but that work is small - about a third of a millisecond per frame on
the M700 - so rewriting it all in C would not be visible. What people feel
are occasional long pauses, and those have other causes that can be fixed
directly.

**Starting up.** Lua programs are compiled from source each time they
start. The window manager's files are about 23 million instructions to
compile - 3 to 5 ms on the M700, once. Every application also compiles the
UI kit, about 19 million instructions, once per launch. Never per frame.

**While working.** A new version of the instruction-counting tool charges
every instruction to the thread that ran it. Dragging a window, the window
manager's own extra work was 52% the Lua interpreter, 25% drawing in C, 9%
copying memory. About 1.2 million instructions a frame: 0.3 to 0.4 ms of
the 16.7 ms a frame has at 60 a second.

**What is actually felt.** The worst moments are long single passes - a
garbage collection landing at the same time as a program asking for
something - and the window manager re-checking everything every time it is
woken, which with one animated window was 3,000 times a second. And another
program, not the window manager, did a lot of text matching while a window
was dragged. Those are the things to fix. (`testing.md` 18.344)

## 2 October - the M700 that would not answer

**In short:** on the M700 the Deskbar took 23 seconds to appear, its menu
ignored clicks, and one processor ran flat out doing nothing. Two mistakes
were feeding each other. The network card's driver waited for the card to
confirm every message it sent, and the M700's card never confirms, so
everything that asked about the network waited too - the Deskbar on every
redraw. And the window manager, asked by a window to check back "very
soon", rounded "very soon" down to "now" and looped. Now the driver never
waits for its card, the Deskbar draws what it last heard and asks again in
the background, and the window manager rounds up. The network on the M700
still doesn't work - its card still sends nothing - but the desktop no
longer cares, and the log now says what the card is doing.

**What it looked like.** Diego, on 0.10.203: "non responsive", "the menu
does not work", "like 60 seconds per window", then "something consuming
100% of the cpu" and "the deskbar is the thing that is really slow to load
at first". The same build ran fine under QEMU.

**How it was found.** Two readings off the machine itself, each copied back
on its stick. A sixty-second profile said one processor was at 98%, all of
it the window manager and the console, asking each other "anything happen?"
68,000 times a second. A diagnosis of the next boot gave the timeline: every
application opened within milliseconds, but the Deskbar's first picture came
23 seconds after it started, and the moment it finally drew was the moment
the network stack had answered whether there was a card. `neofetch`, run by
a Terminal, was still waiting on the same question 19 seconds later - and a
Terminal running a program asks the window manager to check back every
tick, which is what set the window manager looping.

**Why the network stalled.** Sending a frame, the driver handed it to the
card and then slept a tick at a time, up to a thousand times, until the
card said it was sent. The M700's card never said. So each frame held the
driver four seconds, more on the M700; the network stack was waiting on the
driver; and every program asking the stack anything waited behind that. The
network stack was asking the router for an address every few seconds, each
time a new frame, so it was stuck almost all the time.

**The fixes.** The driver now gives the card a frame and moves on, and
collects the card's confirmations later; a card that keeps a frame a second
is reported once in the log, with the registers that say why. The Deskbar
asks its questions - time, sound, network, battery, processor, memory -
once a second on its own, and a redraw or a click only paints what it last
heard. The window manager sleeps to the next tick instead of not at all.
Diego's rule from this: "all that needs to be programmed async so it does
not wait or hang waiting for network or anything".

**Measured** under QEMU, whose Intel card behaves the same way when its
transmitter is left off: a question to the network stack took 14 seconds
with the old driver and 1 ms with the new one. A window asking to be
checked every tick cost the window manager and the console 97% of a
processor before and 20% after (an emulated processor; on the M700 it
should be far less). Both are tests in every gate now, and each was shown
to fail on the old code.

**Noticeable:** the Deskbar appears with the desktop and its menu opens
when clicked, on the M700 with or without a working network; no processor
sits at 100% while a Terminal runs something. Not yet: the M700 has no
network address, because its card still sends nothing - the next stick's
log says what the card thinks it is doing. And the USB driver has the same
waiting habit, which is next. (`testing.md` 18.345)

**Confirmed on the machine.** The same evening, on the 0.10.204 stick: "now
it works great".

## 2 October, night - the M700's network card, read and approached again

**In short:** the M700's network card was switched on but had never once
looked at the messages it was given. Linux's driver for the same chip says
why that can happen: this chip needs to be started more carefully than the
one in the emulator - quietened first, left alone for a moment after a
reset, and given some settings the older chip does without. Kosmos now
starts it the way Linux does, first gently without any reset, and checks
that it works by sending a message to itself. Whether this fixes it is for
the M700 to say.

**What the log said.** The previous fix made the driver report a card that
holds its messages, and on the M700 it did: the transmitter switched on,
the cable connected at a gigabit, and the card's own counter showing it had
read nothing it was given.

**Why this chip and not the emulator's.** The emulator's card is an older
Intel model of the same family; the M700's is an I219, built into the
chipset. Linux's driver for both was fetched and read - Intel's own engineers
wrote most of it, and it is the best description of the hardware there is.
It does several things for the I219 that Kosmos did not: it stops the card
using the memory bus before resetting it, waits for anything in flight to
finish, and then does not touch the card at all for 20 milliseconds after
the reset, because - its comment says - doing so "hangs the hardware".
Kosmos's driver checked the card immediately after resetting it. Linux also
sets several transmit settings this chip needs, in a particular order, and
tells the chip's firmware that a driver is now in charge.

**What the driver does now.** For the I219 only: it writes down what the
computer's firmware left in the card, then tries without any reset - the
firmware has already brought the card up for its own network boot - and
checks by sending a message addressed to itself, which the network switch
quietly drops. If that message doesn't go out within 50 milliseconds, it
resets the card exactly as Linux does and checks again. Either way, the log
says what happened.

**Measured** under the emulator, made to take the I219's path: the
message to itself went out in 126 microseconds without a reset and 140
after Linux's reset, and the network worked after both. With the
emulator's transmitter held off, both attempts are reported as failed - and
a version that pretends the test passed is caught by the test. What the
emulator cannot say is whether this is what the I219 needed. (`testing.md`
18.346)
