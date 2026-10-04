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

## 2 October, late - the M700 boots from the Mac, over the network

**In short:** every try on the M700 used to mean building a USB stick,
writing it, carrying it over and booting it. Now the M700 can load Kosmos
straight from the Mac over the network cable. A new build is a command on
the Mac and a restart of the M700. The stick stays plugged in only for the
person's files.

**Why.** Diego, after three sticks spent chasing the network card: "is
there any way we can simulate the e1000 driver in qemu so i dont have to
build sticks all the time?" The emulator has no model of that card, so its
fault can't be reproduced there. But the M700's own firmware - the
software that runs before any operating system - already knows how to use
that card, and knows how to start an operating system from the network.

**How it works.** At power-on, the M700 asks the network for something to
boot. A small server on the Mac (dnsmasq, set to answer only that question
and to leave everything else to the router) replies with Kosmos's loader.
The loader then fetches the rest of Kosmos from the same server, checks it
page by page against what the build wrote, and starts it. The person's
files stay on the stick, which is never rewritten.

**Measured** under the emulator, set up the same way - network first, the
files' stick on USB: 12 seconds from power-on to a working Kosmos, the
33 MB system fetched and checked. Two surprises on the way: the emulator's
firmware would not start its network at all until it was given a source
of random numbers, which modern firmware insists on; and a test of the
loader made to lose track of its server proves the test notices.
(`testing.md` 18.348)

**Confirmed on the machine.** The same night, the M700 started 0.10.206
from the Mac: "it booted over network!"

## 3 October - Restart that restarts, and the M700 on the network

**In short:** choosing Restart on the M700 did nothing; now it restarts
the machine, from the menu or by typing `restart`. And the M700's network
card, which last night could only send, now receives too: the M700 gets an
address from the router and answers from across the network - slowly at
first, because the card was holding incoming messages back in batches,
which the next build stops.

**Restart.** Kosmos restarted a PC the way PCs have been restarted since
1984: through the keyboard chip. The M700 has no such chip - its firmware
pretends to have one, and stops pretending once Kosmos takes over the USB
ports. Modern PCs describe their own restart switch in their firmware
tables, and Kosmos now uses that first, then three older ways in turn,
saying each one as it tries it. In the emulator, the first way restarts the
machine; a test checks that it is the first way that does it.

**The network, step by step.** After the power cut, 0.10.207 got an
address - so the card receives - but nothing could reach the internet.
Pinging the M700 from the Mac showed why: it heard messages sent to
everyone, but not ones sent to it alone. 0.10.208 counted what the card
received and, finding nothing addressed to itself, switched off the card's
address filter; from then on the M700 answered the Mac - but each answer
came 7 to 19 seconds late. The card was keeping received messages until
several had piled up, a setting the M700's firmware leaves behind. The next
build has the card hand over each message as it arrives.

**And the remote program** (`telnetd`) used to give up if it started before
the M700 had an address, which on every network boot it did. It now waits
for one.

## 3 October, evening - the M700 on the network for real, and driven from the Mac

**In short:** the M700's network now works properly - an address from the
router, answers in half a millisecond, Wikipedia in the browser. The fault
was not any of the settings blamed for it over six builds, this morning's
entry included: the card was still following the list of memory slots the
M700's firmware had given it for its own network boot. Resetting the card
before Kosmos uses it, as Linux always does, cleared it. And the M700 can
now be rebuilt, restarted, looked at and used entirely from the Mac: a new
build reaches it and answers in about a minute, and a window on the Mac
shows its screen and passes on the keyboard and mouse.

**What was wrong.** A network card is handed a ring of empty slots in
memory and puts each arriving message into the next one. When the M700
boots from the network, its firmware drives the card first and gives it a
ring of its own. Kosmos then gave the card a new ring - but without a
reset, the card kept working from slots it had already taken from the
firmware's. Messages arrived, the card counted them as received, and they
went into memory Kosmos no longer expected anything in; now and then one
landed in Kosmos's ring, seconds late. That is why pings came back after 5
to 29 seconds, why the router's answer to "who has this address" got lost,
and why the morning's explanation - the card holding messages back in
batches - looked right for a day: the delays fitted it, and the fix for it
changed nothing.

**How it was found.** Each build added numbers to the log: how many
messages the card counted, how many it dropped, how many reached Kosmos,
and where in the ring the card was. The telling build had the card count
2,460 messages, drop none, lack no slot - and put none in Kosmos's ring,
its position never leaving the first slot. A card that receives everything
and delivers nothing somewhere else is a card using somebody else's ring.
The reset was tried by changing one line the Mac serves to the M700 at
boot, with nothing rebuilt: 686 messages counted, 686 delivered.

**Visible impact.** The M700 answers the Mac in 0.5 ms instead of not at
all, reaches the internet in 15 ms, looks names up, and loads Wikipedia.
Nothing in the emulator could ever have shown this: its cards keep no
leftovers from a firmware.

**The loop.** Diego asked for a cycle where the M700 is rebuilt, restarted,
tested and looked at without anyone touching it. It now runs: the Mac
builds and serves the new Kosmos, one command restarts the M700 over the
network and waits - "0.10.213, answering again after 59 s" - and others
open an application on it, click and type on its screen, and take a
picture of it. A click sent from the Mac opened the Kosmos menu on the
M700. The keyboard and mouse are lent to a remote viewer only because the
boot line the Mac serves says so; a machine started from its own stick
does not lend them.

**And a window for Diego.** `tools/kosmos_view.py` shows the M700's screen
on the Mac and sends the keyboard and mouse back, finding the machine by
itself and reconnecting after each restart. It is built on the same code
the tests use. The first picture takes about 7 seconds, because the M700
sends at only 1.4 MB a second - the next thing to measure. A viewer the
other way round, inside Kosmos, is agreed and will be drawn first.

## 3 October, night - a dock at the bottom, in a look called Night

**In short:** Kosmos can now put its taskbar where Googlebook puts it - a
floating dock at the bottom centre, with the time and the indicators in a
thin strip across the top - in a new dark-blue look called Night, as Diego
asked, drew and agreed the same day. The BeOS-style bar at the top is still
the default and one setting away. Trying it under the emulator found six
faults before any test was written; each is fixed, and a new 31-second
test checks the dock every time the system is tested.

**What.** Appearance in Preferences has two new choices: *The bar* (Top,
or Bottom, centred) and *The dock* (Floating, or Whole width). Chosen, the
Deskbar restarts itself in its new place. As a dock it shows the Kosmos
button, eight pinned applications, a separator and whatever else is
running - each application once, however many windows it has, with a small
mark under it while it runs. A press opens an application, brings it to the
front, or puts it away if it is already in front. The Kosmos menu opens
upwards. A window maximised fills the room between the strip and the dock.

**Why this shape.** Diego wanted the design language of Googlebook beside
BeOS's, not instead of it, and drew both into one page before any code
(`docs/dock.html`). The Deskbar stays one program that draws either way, so
everything it knows - what runs, what is starting, the menu - is shared
rather than written twice.

**What the first look found.** A maximised window slid under the dock,
because the window manager counted the dock's space before the dock was on
its list. The strip at the top stayed blank, because a second piece of the
Deskbar replaced the hook that redraws it. Moving the bar reopened the four
login windows. The command that moved it got an error back, because the old
Deskbar left before answering. The Kosmos button stayed lit after its menu
was dismissed - the window manager closed the menu without telling its
owner, and the bar at the top reads the same list. And the version line in
the corner sat under the dock. All six are fixed, and each of the four
that a test could undo was undone once to prove its check notices.

**Visible impact.** On a 1440 by 900 screen the dock sits twelve pixels
above the edge with a faint light outline that separates it from a dark
window behind it; Groove opened maximised now ends above it. Switching
between the dock, the whole-width dock and the bar at the top takes about a
second each way. Still to come, in the agreed order: the launcher as a grid
of applications above the Kosmos button, quick settings under the strip,
and Night's own title bars and desktop icons.

**And an older fault the gate turned up.** Cafesa3D's test drags across
a rotation field and expects 20 degrees; now and then it got 16. The window
manager tells a window about a drag move by move, but only while the button
is held - so when the last bit of movement and the letting go reached it at
the same moment, it reported only the letting go, and the last movement was
lost to any program that counts moves. It now reports the movement first,
then the release, and a new 3-second test sends both at once to prove it,
every time.

## 3 October, late - the dock lived with, on the M700

**In short:** Diego used the new dock on the M700 all evening and every
thing he noticed was fixed the same night: the wallpaper now fills the
screen, the launcher opens as a grid of applications over the Kosmos
button with its five folders as pills, applications go by their real
names, a held key repeats, the dock names an icon when the pointer rests
on it, a screenshot is one key away, the Windows key opens the launcher,
and the dock can be arranged by hand. Ten revisions, 0.10.217 to 0.10.226,
each with its own test.

**What.** In the order he asked:

- *The wallpaper* was centred and never scaled, so a 5120-pixel picture
  showed its middle. It now fills the screen by default, resampled once
  when it is chosen, and Appearance's *Wallpaper size* offers *Centred*.
- *The menu* opened far from the Kosmos button; the launcher now opens as
  a grid above it, like Googlebook's, with a search line, six columns, and
  pills for All, Applications, System, Development, Demos and Preferences -
  Diego's five folders, in his order. Tab goes from pill to pill.
- *Names*: `procs` is Process Viewer, `ide` Kosmos IDE, `machine` About
  This Machine. Each application declares its name beside its icon, and the
  file's name stays its key.
- *A held Backspace* deleted one letter. Key repeat had never existed on a
  USB keyboard - the M700's only kind - and it does now: half a second, then
  thirty a second.
- *A name over each dock icon*, as macOS draws it: a dark pill with a
  small arrow down to the icon.
- *Screenshots*: Print Screen, Control Alt 1, or Super Shift 3 writes a PNG
  of the whole screen into Captures, so Diego can send pictures without
  photographing the monitor.
- *The Windows key alone* opens the launcher; the numeric keypad types.
- *The dock arranged by hand*: drag an icon along it to move it, drag it up
  off the dock to take it out, right-click it for Keep in Dock, Remove from
  Dock and Quit, and right-click a tile in the launcher for Add to Dock.

**Why these are small and one at a time.** Each was something a person
using the machine ran into within a minute, and each is the kind of thing
that decides whether a desktop feels finished. Done one per revision, a
build that misbehaves on the M700 points at one change.

**What it found underneath.** Switching between the floating and the
whole-width dock worked once and never again: the program that looks up
names kept a handle to the Deskbar it had first found, and after the
Deskbar restarted that handle pointed at nothing. A name looked up on its
own now drops a dead handle and tries again. And a click in the dock now
acts when the button is let go rather than pressed, because only then is
it known whether the press was a click or the start of a drag.

**Visible impact.** The dock test now checks 44 things on a 1720 by 1440
screen, the M700's, with a USB keyboard so every key goes the M700's way.
Each new check was proven by breaking what it checks and watching it fail.
Still to come: the dock's numbers in a file Diego can edit, a Spotlight
look for Super Space, quick settings, notifications, and every older
application moved onto the new window chrome.
