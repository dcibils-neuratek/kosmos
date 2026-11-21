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

## 4 October - notifications, and the kernel saying who asked

**In short:** Kosmos can tell you things now. An application posts - a
render finished, a stick arrived - and a card slides in at the top right
for five seconds, or stays until closed when it is an alert. A press on the
clock opens the history of everything said, with Do Not Disturb and Clear
all, as Diego drew and agreed it the night before. To make it honest, the
kernel learned something it never knew: who sent a message.

**What.** A server, `/Notifications`, keeps what is said, numbered. A
program, `notifications`, starts with the desktop, asks the server four
times a second what is new, and shows it in a new kind of window - a
*banner*, which can be pressed but never takes the keyboard, so a
notification arriving while you type takes nothing from you. `notify` at
the prompt posts one, for testing and for scripts.

**Why the kernel had to change.** The history groups notifications by the
application that sent them, and Preferences will let a person turn one
application's off by its name. That only means something if an application
cannot claim to be another. Until now no server knew who called it, and any
program could rename itself. So a server can now ask the kernel which
process sent the message it just received, and a program's file is fixed
before its first line runs. Under the emulator, a program that tried to
call itself Cafesa3D was refused, and its post was filed under its own file.

**What it fixed on the way.** The window manager tied each new window to
whichever program it had started last - its own comment called that "good
enough, and honestly not more". A banner opening at the wrong moment would
have taken another application's identity, so closing one could have ended
the other. It now asks the kernel too.

**Visible impact.** Banners in the Night look match the drawing: a rounded
card, the application's picture, its name and "now", the title and a line,
a cross at the corner, an alert edged in red. The history is a tall panel
under the strip, grouped by application. Next: Preferences' Notifications
page - Do Not Disturb, how long a banner stays, how many are kept, and a
switch for each application - and then the first real senders: Cafesa3D's
renders, downloads, sticks arriving, a program that stopped, Clock's timers.

**Later the same day: the settings, and the first real senders.**
Preferences has a Notifications page now - Do Not Disturb, how long a banner
stays, how many are kept, and a switch for each application that has said
something. And four things say something on their own: Cafesa3D when a
render finishes, any application that stops on an error, one the window
manager had to end because it would not close, and any USB device plugged
in or pulled out. The very first of those caught a fault at once: pressing
a banner that named a folder had been failing quietly, and the error
became a notification of its own, "Open stopped". A press now opens a
folder in Tracker and a file with whatever opens it. Still to come, with
the features they belong to: downloads (the browser has none yet) and
Clock's timers.

## 4 October, later - a faster stick, settings you can read, and a mouse that comes back

**In short:** six revisions in one afternoon, each small and each something
Diego had run into. The USB stick on the M700 reads twice as fast. The
dock's background has a transparency slider. Every setting in the system is
now a text file a person can open and read. Applications all keep their
settings through one kit instead of each inventing its own way. An installed
game built for an older Kosmos now says so instead of failing strangely. And
a mouse that hiccups on its cable is no longer abandoned.

**The stick (0.10.231).** Measured on the M700, a stick read in a straight
line went from 31.9 MB/s to 64.5, and small reads in random places from
about 330 a second to about 1,330. Two changes did it. The USB controller
had been told to wait up to a millisecond before reporting that work was
finished; now it waits 40 microseconds. And a read could ask for at most
64 KB at a time; now it asks for up to 1 MB, chained in the pieces the
controller needs. Writes stayed at 4 to 17 MB/s, which is the flash itself.

**The dock (0.10.232).** Appearance has a slider from 0% (opaque) to 100%
(fully see-through) for the dock's background, starting at 25%. The icons
on it are never faded.

**Settings as text (0.10.233 and 0.10.234).** Diego: "I don't like binary
files for settings for anything in the system". A table saved to `/Home`
used to be the serialiser's binary bytes. Now it is Lua's own table syntax
under a first line that says `-- kosmos: table`. It is read as values only,
never run, so a settings file cannot carry a program, and one broken by hand
is refused with the line it broke on. Then the settings kit: an application
opens its settings by name, reads a setting (its default until someone
chooses otherwise), and sets one, which is written at once. Every
application was moved onto it. The build now fails if anything but the kit
names the preferences folder, so the next application cannot drift back to
doing it by hand.

**A stale game says so (0.10.235).** Doom, Quake and the Super Nintendo each
carry their own copy of the system's runtime, so after the system's
protocols change they must be built again. On the M700, Doom built for an
older revision failed with "ui.lua:1: unexpected symbol", from deep inside
the interface kit, which said nothing about the real cause. Every image now
carries a stamp of the protocols it speaks, and a program whose stamp
differs is refused with a sentence: built for another Kosmos, build it
again.

**The mouse (0.10.236).** Measured: on the M700 the mouse stopped after
2,998 seconds and 48,726 good reports, because one report failed with a USB
Transaction Error. That is a glitch on the bus, not a fault in the mouse,
and the driver had given up on the device for good. Now a failed report is
asked for again: the controller's side of the connection is reset and the
request sent once more. The device is given up only after five failures in
a row, which means it is really going. Under QEMU the test pretends one
report failed and watches the mouse carry on, and with the recovery switched
off the same test fails.

## 4 October, evening - Kosmos Write's first step: a document and its file

**In short:** nothing to look at yet, but the first piece of Kosmos Write is
done. There is now a way to hold a document in memory (its paper, margins,
header and footer, styles, and styled paragraphs) and a `.write` file to
keep it in. That file is an ordinary zip that any computer can open. Inside
it, the document is text a person can read, and pictures sit beside it.

**What.** Three small kits rather than code inside an application, because
Diego wants Kosmos Present and Kosmos Sheets built on the same technology:
one for styled text, one for Write's page around that text, and one for a
document's file. The zip library learned to make an archive from things in
memory and to read a single entry back out.

**Why it is built this way.** A document arrives as a file, and a file can
come from anywhere. So when one is opened, every field is checked against
what a document may contain: numbers are held to sensible ranges (a margin
can't be wider than the paper), and anything unknown is left behind. The
editor never sees a field it didn't expect. Also, every number says its
unit in its name (`size_pt`, `width_mm`), because a page is measured in
millimetres, type in points, and Word in twentieths of a point. This
project has already been bitten twice by a number that didn't say which
unit it was in.

**Measured.** A hundred-page document - a thousand paragraphs - takes about
870 KB of memory as Lua tables and 500 KB as text. Python on the Mac opened a
`.write` file made inside Kosmos and found every byte where it should be.

**Next:** W2, setting the paragraphs into lines and the lines onto pages,
measured exactly as the PDF will be, so the screen and the PDF never
disagree about where a line breaks.

## 4 October, night - Kosmos Write sets pages

**In short:** Kosmos Write can now lay a document out on pages: paragraphs
broken into lines, lines placed down the page, pages filled and numbered.
Still nothing on screen, but this is the part that decides where every word
of every page goes, for the screen and the PDF alike.

**What.** The setting does what a word processor's does. It breaks lines
between words, puts lines down the page with the spacing the style asks
for, and aligns each line left, right, centred or justified. It honours
indents and tabs. It never leaves one line of a paragraph stranded at the
bottom of a page or alone at the top of the next. It keeps a heading on the
same page as what it heads, and numbers the pages.

**Why one setting matters.** If the screen measured text one way and the
PDF another, a line could break after "the" on screen and after "page" in
the PDF, and the printout would not be what you saw. So both use the same
numbers: each letter's width exactly as the font file states it, not
rounded to screen pixels. Inside the machine those widths come from a small
new piece of C that reads them from the fonts Kosmos carries. Each font
also says its own family name and weight, which is how the Font menu will
list them.

**How it was checked.** On the Mac, the setting was run against a pretend
font where every letter is half an em wide, so every line's position could
be worked out by hand: 45 checks. Then a new suite boots Kosmos and has it
lay out a real paragraph in IBM Plex Serif, while the Mac lays out the same
paragraph by itself, using its own reading of the same font file. The lines
broke in exactly the same places, word for word.

**A bug it found that was not Write's.** The paragraph came back from the
machine with "Café" garbled and "résumé" intact. The test harness had
decoded the machine's output in chunks, and an accented letter split across
two chunks became two error marks. It had never been noticed because
nothing had printed an accent through it before. Fixed in both places that
had it.

**Next:** W3, the PDF - the part Diego called key.

## 4 October, night - Kosmos writes its first PDF

**In short:** Kosmos Write can export a PDF, the part Diego called key. A
document set inside Kosmos comes out as a PDF that Kosmos's own viewer,
macOS and any other reader open the same way, with the fonts inside it.

**What.** Every font the document uses travels inside the PDF, so it looks
the same on a computer that has never heard of IBM Plex. Each letter goes
exactly where the page layout put it, using the same widths. The text stays
real text: it can be searched and copied out of the PDF.

**A wrong turn worth recording.** The first version wrote its fonts the
old, simple way, which can only say about two hundred different characters.
Kosmos's own PDF viewer refused to draw a single letter of it, because it
only reads the modern way of embedding fonts. That was the viewer being
right. The writer was changed to the modern form, and gained every
character a font has along the way.

**How it was checked.** Three readers, none of them the writer. Inside the
machine, Kosmos's own PDF viewer draws every glyph of every page. On the
Mac, a test takes the file apart piece by piece: every font is the original
font file byte for byte, every letter's width matches the font, and every
piece of text is on the right page at the right spot. Then macOS renders it.

**Visible impact.** A three-page PDF with a bold title, a subtitle,
headings, and justified text in italic, bold, underline, strike-through,
red, accents, a euro sign and curly quotes. Diego has a copy.

**Measured, and what is next.** The file is 664 KB, and 652 KB of that is
fonts: each one is embedded whole, about 93 KB. The next step is to embed
only the letters a document actually uses, then the Write window itself.

**Later: the PDF, twelve times smaller.** Each font in the PDF now carries
only the letters the document actually uses, the way every serious PDF
producer does it. The same three pages went from 664 KB to 56 KB and look
identical. The test checks that every letter shown kept its exact shape,
that nothing else was kept, and that the font's internal checksums are
right. Kosmos's own reader was the first to notice when, as a deliberate
experiment, one letter was left out.

## 4 October, late - Kosmos Write gets a window, and a premise

**In short:** Kosmos Write opens a window now. It shows a document's pages
exactly as they will print, lets you zoom, and exports the PDF with one key.
Diego also set down a rule for the whole system: apps are built out of
shared parts, never out of private copies.

**What.** Pages sit on a dark desk. Zoom goes from 50% to 300%, and Export
PDF writes the file next to the document. You can't type yet; that's the
next step, followed by the panels for formatting and page setup.

**The claim it is tested against.** What you see on the screen is what
prints. The test opens a document in the window and takes a picture of the
screen. It then exports the PDF, has macOS draw that PDF at the same size,
and compares the two line by line. Every line of text starts, ends and sits
within three pixels of where the other puts it, and they are two different
drawing engines.

**A bug found on the way, years old in spirit.** Kosmos's PDF viewer had
always drawn letters too small: about 90% of their size in the book it was
first written for, and 77% in IBM Plex. It measured fonts by their total
height instead of by the "em" that PDF sizes refer to. Nothing had ever
checked the size of a drawn letter. Now a test does.

**The premise.** Diego: "Kosmos os works as a micro services operating
systems where kits, services, drivers and servers work to supply with
functionality to apps. Apps then orchestrate all this into useful
productive apps." Anything an app needs that another app could want (PDF,
compression, video, audio, 3D, page drawing) belongs in a shared kit, used
through one door, and never copied. The same day had already broken it
twice: the PDF writer copied a helper from the zip library, and reading and
writing PDF ended up in three places. Both are now first on the roadmap to
put right, followed by a review of every app for the same thing.

**Then the premise, applied.** The two places it had been broken that day
were fixed first. Everything to do with PDF now goes through one library
that any app can use to open, draw or write a PDF. Four separate copies of
the same "read a PDF file a piece at a time" code went, from the viewer, two
tools and a test. The memory helpers the PDF writer had copied from the zip
library are shared by both now. A list of eight more places that each wrote
the same few lines is on the roadmap, to be gone through next. Kosmos's own
PDF viewer is also tested for the first time: it opens a PDF that Kosmos
Write exported and draws every letter.

## 4 October, the end of the night - you can type in Kosmos Write

**In short:** Kosmos Write is an editor now. Click anywhere on the page and
type: Return, Backspace and Delete, the arrows, Home and End, selecting with
Shift or by dragging, copy, cut and paste, undo and redo, and Control-S to
save. Diego asked for this first so he could test it.

**How it is built.** Following the premise from earlier in the evening,
almost none of it lives in the app. What typing does to styled text, how a
paragraph splits, and where the caret is on the page are all in the shared
kits, ready for Kosmos Present's text boxes and Kosmos Sheets' cells. The
app decides what each key means.

**Checked the way a person would do it.** A test types into a new document
through the emulator's keyboard: a mistake, Backspace, Return, Home, undo,
redo, a selection typed over, a click in the margin. It saves, and the Mac
reads the file to confirm it says exactly what the keys meant.

## 4 October, later still - Kosmos Write as it was drawn, and pictures

**In short:** Kosmos Write now looks like its mockup, `docs/write.html`,
and most of what the mockup shows works. There is a toolbar, a Format panel
for text and paragraphs, and a Document panel for paper, margins and the
header. A View shows the pages as thumbnails. Add Page starts a new page.
It has numbered and bulleted lists, drop caps, ligatures, hyphenation in
English and Spanish, and facing pages for a book. It exports Word documents
as well as PDF, and now it takes pictures, each with a caption under it.

**What each step was, in plain terms.**
- **The window.** The buttons, steppers, colour swatches and menus are new
  controls in the shared toolkit, so the next app gets them for free.
- **Lists and drop caps.** A list's numbers are worked out when the page is
  set, so moving a paragraph renumbers the list on its own.
- **Ligatures.** "fi" and "fl" are drawn as one joined letter, as a printed
  book draws them, in the PDF as well as on the screen.
- **Hyphenation.** This uses Franklin Liang's 1983 method, which is still
  how TeX breaks words, with TeX's own lists of word patterns copied in
  unchanged. It finds that "computer" breaks as com-puter and "computadora"
  as compu-tado-ra.
- **Word.** The `.docx` export writes the same paragraphs, styles, lists and
  page setup. Word opens it as a document, not as a picture of one.
- **Pictures.** The Media button opens the system's file panel at
  Pictures. A chosen PNG or JPEG goes in centred, at its own size or the
  column's width if that is smaller, with a caption ready for typing. The
  `.write` file keeps the picture exactly as it came. The PDF holds it once,
  however many pages show it: a JPEG goes in unchanged, and anything else
  is compressed with the same code Kosmos already uses to save PNGs. Word
  gets the picture's own file.

**Why it is built this way.** This follows the premise from earlier in the
evening. Kosmos Write adds no new C code for pictures. Reading them, scaling
them and compressing them are things the system already had, and the app
only connects them.

**Checked the way a person would do it.** One test makes a picture of sky
over sea inside the machine, puts it in a document and exports it. Then the
Mac decodes the PDF's picture and checks that the top is sky and the bottom
is sea. Another test uses the window itself: it clicks Media, chooses a
picture in the file panel, types a caption, saves, and checks that the saved
file holds the picture byte for byte. That test also found a small fault in
the file panel, in every app, not only this one. Return does not open the
first file until the list has been clicked. It is on the roadmap.

## 4 October, later still - tables in Kosmos Write

**In short:** Kosmos Write has tables. The Table button puts in three rows
by three columns with a header row. Tab moves to the next cell and makes a
new row at the end, Shift-Tab goes back, and Return moves down. The Format
panel can add or remove rows and columns, turn the header row on or off,
or delete the table. A table that runs past the foot of a page continues
on the next one, with its header row repeated at the top, and it exports to
PDF and to Word as a real table.

**How it is built.** The interesting decision is that every cell is an
ordinary paragraph. Everything Kosmos Write already knew how to do with a
paragraph works in a cell for free: typing, bold, fonts, colours, undo,
and the panel showing what the caret is in. Each table row is placed on
the page as if it were one line of text, so the rules that already decide
where a page breaks decide where a table breaks too.

**Found on the way.** The Tab key had never reached Kosmos Write at all,
because a window keeps Tab for moving between its controls unless the
view asks for it. So "Tab types a tab", written weeks ago, had never once
worked, and no test had pressed it. The table test was the first to press
Tab. Two older faults also surfaced and are fixed: choosing a style over a
range that included a picture lost the picture, and deleting a range that
ended on a picture left it stuck in the paragraph that remained.

## 4 October, later still - text boxes

**In short:** the Text button puts a box of text into the document, 80 mm
wide and centred, with a border. Return inside it starts a new line, and
the panel sets its width, a pale fill and whether it has a border. Shift
and Return now start a new line inside any paragraph, as in Pages.

**How it is built.** A text box is a table with a single cell that is drawn
as a box. Everything tables learned an hour earlier - typing, the caret,
the PDF, Word - came with it, and all that is new is where the box stands
and how it looks. In Word it arrives as a one-cell table, so it is still a
box someone can edit there.

## 4 October, later still - shapes

**In short:** the Shape button offers a rectangle, a rounded rectangle, an
oval, a triangle, a star and an arrow. Each goes into the document as a
coloured shape, and the panel changes its kind, its size and its colour.

**How it is built.** The setting describes a shape the way it already
described a table's lines: a short list of outlines in points. The screen
fills a polygon as a fan of triangles from its centre, which works as long
as every corner can be seen from the centre. A test checks exactly that
for the star, the arrow and the triangle, because a shape that broke the
rule would spill paint outside itself. The PDF draws the same outlines as
true curves and lines, and Word gets its own built-in star, oval and
arrow, so someone can still edit them there.

**Found on the way.** Kosmos's own PDF viewer draws only text and
pictures, so table lines and shapes in an exported PDF show in macOS's
Preview but not in Kosmos itself. That is a job for the PDF viewer, and
it is on the list.

## 4 October, later still - charts

**In short:** the Chart button puts in a column, bar, line or pie chart,
with a year's seasons as numbers to start from. Edit Data in the panel
shows the chart's numbers as a table right above it. Type over a number
and the chart changes as you type.

**How it is built.** A chart is a table whose numbers are drawn instead of
shown. The first row names the series and the first column the categories.
So editing a chart's data needed nothing new: it is the table editing
built earlier in the evening. Word receives a real chart with the numbers
written inside it, so it is still a chart, and editable, there.

**Found on the way.** In the test the chart sat on the right-hand page of
a two-page spread. The view scrolled only far enough to show the caret at
the chart's left edge, so the chart itself was hidden under the panel. The
view now brings the whole object into sight when it fits.

## 4 October, later still - comments, and the whole mockup built

**In short:** select some words and press Comment. The words turn pale
yellow, and the panel shows a field to type the comment into. Comments
travel with the document. In the PDF each one is a note that any PDF
reader shows as a comment. In Word it is Word's own comment, attached to
the same words.

**How it is built.** A comment is a mark on the words it is about, not
part of how they look, so it never changes their font or style. Typing
inside commented words keeps them commented, but typing just after them
does not, because the next words are not what the comment was about. A
saved file keeps only the comments whose words still exist.

**With this, every button and panel in the mockup Diego approved,
`docs/write.html`, does something:**
- View, Zoom and Add Page;
- Insert, Table, Chart, Text, Shape, Media and Comment;
- Export, Format and Document.

Each was built as a reusable kit with a test that does what a person would
do, and with a deliberate break to prove that the test would notice.

## 4 October, night - the review before 0.11 finds a hole in the kernel

**In short:** before Kosmos goes to version 0.11, its own rules say the code
is read again as it now is. That reading found a real security hole in the
microkernel, and it is fixed and tested.

**What it was.** When a server answers a program's request, the kernel has
to know which waiting program the answer is for. It used to give the server
the raw memory address of that program's record inside the kernel, and take
it back with the answer. A comment said this was safe, because a made-up
number would simply be refused. It was not. The kernel used the number
before checking it, so a program could fake a record in its own memory and
make the kernel write wherever it chose. On a real computer, that is how a
program takes over the machine.

**What it is now.** The answer carries a ticket instead: which slot in the
kernel's own list of threads, and which of that thread's requests. The
kernel looks the slot up itself, checks the request is still the one
waiting, and checks the answer comes from the program that received it.
Anything else is refused without being touched. A test now tries four kinds
of forgery, and a version of the kernel with one check removed fails it.

**And a second one, smaller.** Drivers may map hardware into their memory,
but never ordinary RAM. The check knew only one stretch of RAM, from when
the kernel used only one. A PC has several, so that check now covers all of
them.

## 4 October, late night - Kosmos 0.11

**In short:** Kosmos is now version 0.11. The step from 0.10 marks two
things: Kosmos Write, a full word processor built over one long day, and
a careful re-reading of the whole system before the version number moved.

**Why a re-reading.** Kosmos's own rules say that before a new minor
version, the code is read again as it now is, not as it was meant to be.
Software gathers leftovers: code for ideas that were replaced, and comments
that were true once and are not any more. Since 0.10.0 about 215,000 lines
had changed. Six reviewers read them in parallel, each one required to
prove every finding with a search of the whole tree.

**What it found.** About 150 things.
- **A security hole in the microkernel**, now closed and tested: a program
  could have tricked the kernel into writing wherever it liked.
- **About fifteen real bugs**, most of them small, but some a person would
  hit:
  - the Network settings window turned off name lookups whenever Apply was
    pressed;
  - Kosmos Write could lose a picture when saving;
  - Tracker could not cut and paste a file larger than a megabyte;
  - sorting a big folder took time that grew with the square of its size.
- **Unused code**, about two thousand lines in all, mostly left behind
  when the system servers moved from Lua to C one by one.
- **Many comments** that described the machine as it was months ago.

Every change ran through the full test run, all 90 suites in under ten
minutes, before 0.11 was made.

**What it left for next.** Every place where the same code is written twice.
Diego's premise is that a second copy is a defect, and those copies are
the next job. They are written down on the roadmap as a list, so nothing
found is forgotten.

## 5 October - Kosmos Write loses its old title bar

**In short:** in the looks with no title bars, Plex and Plex Night, Kosmos
Write still had one. It now draws its own header strip like the other
windows, with the close, minimise and maximise buttons in it.

**What.** Kosmos has looks where windows have no bar above them: the top
strip of the window itself is the title bar, and the window manager puts the
three buttons at its right end. A window gets that only if it says it has a
strip of its own. Kosmos Write draws every pixel itself, for the pages, and
it never said so, so the window manager kept giving it the old bar.

**Why it matters.** A look should be the same everywhere. One window in the
old style stands out, and it was the newest application on the system.

**What you see.** In Plex and Plex Night, Write's top strip shows "Kosmos
Write" and the document's name, with the three buttons at the right.
Dragging the strip moves the window, and a double click maximises it. The
tools sit under it. In the looks that keep title bars nothing changes. Write
also redraws when the look changes, which it had not done at all before: it
used to keep the old colours until you next typed or clicked.

**How it is checked.** The display test already opened several windows in
Plex and checked that each one's buttons land in its own strip. Write is now
one of them, with a check that its tools moved under the strip.

## 5 October - /Temporary stops running out of room

**In short:** the place Kosmos keeps files in memory, `/Temporary`, could
hold only 128 files of 16 KB each. It now holds as many as fit in half of
the machine's memory, and gives memory back when a file is deleted.

**What.** `/Temporary` is a filesystem in memory. On a computer without a
disk, the person's own folder, `/Home`, lives there too. Its limits were
fixed numbers chosen for the first, small test machine: 128 files, none
larger than 16 KB. A PDF with five fonts did not fit, nor a song, nor a
21 KB program the code editor wanted to run from there.

**Why it was like that, and what replaced it.** A fixed size has one good
property: when it is full, it says so cleanly, at a known moment. That
property is kept. The store now grows as files arrive, but everything it
holds is counted against one limit, half of the machine's memory, decided
once when it starts. Past the limit a write is refused and nothing is left
half done. When a file is deleted its memory goes back.

**What you see.** Bigger files work in `/Temporary`, and in `/Home` on a
machine with no disk. When memory really is full, the message names the
file: "no room for /Home/song.flac in memory", where it used to say
"/Temporary is full" even for a file in `/Home`.

**How it is checked.** The store's logic was moved into its own file so it
can be tested on the Mac without booting Kosmos: five hundred files, a
300 KB file, the limit refusing, memory coming back, forty programs
waiting on it at once. 33 checks, and each of three deliberately broken
versions fails them.

## 5 October - the Open window listens to the keyboard

**In short:** when an Open or Save window appears, the keyboard now works
in it straight away. Before, Return did nothing until something was
clicked.

Every application shares the same Open and Save windows, so this was true
everywhere. Now the arrow keys and Return choose a file the moment Open
appears, and in Save you can type the name and press Return. The display
test opens both and answers them with the keyboard alone.

## 5 October - one copy of each thing, the first half

**In short:** Kosmos's rule is that a piece of work is done in one place
and everything that needs it uses that place. A review found dozens of
pieces written twice or more. The first half of them are now one.

**What.** Three groups:
- **Shared memory.** Many programs set up a block of shared memory by hand,
  the same four lines each time. They all use one library now. Three of the
  hand-written copies, in Doom, Quake and the Super Nintendo, were the same
  file loader, and two others had a bug waiting: they would have written a
  file's second piece over its first.
- **The C kits.** Pictures had their layout copied into four kits. There
  were two decompressors where one does the job, so one left. Base64, the
  encoding that sends a file as text, was written once in C for 3D files
  and again in slow Lua for the remote command line. Several small helpers
  were copied up to five times. Each is one now.
- **The browser.** It had its own scrollbar, its own way of joining web
  addresses (which got `..` wrong), and its own copies of other helpers.
  It uses the shared ones now.

**What it found on the way.** A leak. When a program was done with a block
of shared memory and gave it back, the memory was not actually freed until
the program quit. The window manager never quits, and every picture,
wallpaper and window it was ever shown stayed in memory. Fixed: giving a
block back now frees it.

**Why it matters.** Two copies drift apart, and a bug fixed in one stays in
the other. Each copy removed is one less place for that to happen, and the
leak only came to light because the copies were being merged.

**How it is checked.** The whole test run, all 90 suites, passed. A new
test holds the shared-memory library to putting every piece of a file in
its right place, and fails against the old loops' mistake.

## 5 October - one copy of each thing, the second half

**In short:** the rest of the duplicated pieces the review found are now
one each. Two small applications went away because Preferences already
does what they did.

**What.** Mostly small helpers that many programs had written for
themselves: cutting a name to fit with "...", wrapping words, stepping
through letters that take more than one byte, writing a file size, a
number with thousands commas, a time as 3:07, a date in a file's name, a
network address. Each lives in one place now and everything uses it. The
three network programs - the web server, the remote command line and the
remote screen - shared a page of start-up code each had copied; it is one
library. Groove now saves songs as readable text like every other file of
settings, instead of as a small program it had to run to read back.

**What you see.** Names that did not fit used to be cut off in the middle,
sometimes in the middle of a letter; they now end in "...". The separate
Startup Apps and Date & Time windows are gone: Preferences has the same
pages, and those two windows were second copies of them.

**Why it matters.** The cutting helpers were the clearest case: most of the
dozen copies could cut a letter like "é" in half and print a broken
character. Fixed once, it is fixed everywhere.

## 13 October - memory a program has used, not memory it might use

**In short:** every program used to take about 5.7 MB the moment it
started, whether it needed it or not. Now it takes memory a page at a time,
as it actually uses it. The M700's desktop went from 682 MB to 524 MB, and
a small server from about 5.8 MB to 16 KB.

**What.** A program is given three pieces of private memory: room for its
own variables, a heap for what it creates as it runs, and a stack. All
three used to be set aside in full, and cleared, before the program ran
its first instruction. Now the kernel only notes where they are. The first
time the program touches a page of one of them, the processor stops it,
the kernel gives that one page, and the program carries on without knowing
anything happened. The edges stay forbidden, so a program that runs off
the end of its stack or heap is still caught at once.

**The surprise.** The first try saved only the heap and the stack. Every
program still took 3.4 MB of variable space, because its first few
instructions cleared that whole area to zero, page by page, "to be sure".
The area already holds zeros, so clearing it again bought nothing and made
every program use every page of it. That clearing is gone.

**Why it matters.** Most of the 3.4 MB is decoding tables for films and
music, sitting in every program because every program is built from the
same image. A file server never plays a film, so it never touches them
now. With forty programs on the desktop, that is a lot of memory that was
never used.

**What you see.** The Processes window's memory column: small servers now
show tens of kilobytes instead of 5.8 MB, and the total at the top fell by
about 160 MB on the same desktop.

**How it is checked.** The whole test run, 84 suites, passed. A new kernel
test makes a program that never runs and checks that it holds nothing
until touched, that a touched page holds exactly the right bytes, that the
edges are refused, and that every page comes back when the program ends.
It was deliberately broken twice to be sure it notices: once by copying
nothing, once by not giving the heap back.

## 8 November - Pewter's desktop, and folders that stay where they were left

**In short:** Pewter's desktop is now arranged as Mac OS's was: the
Drive at the top right, the Trash at the bottom right. Open a folder there
and it opens in a window of its own, named for it. Move that window,
make it bigger, switch it to a list, close it - and the next time you
open that folder it comes back exactly so. Open it while it is already
open and the window you have is brought forward, rather than a second
one. Astra's desktop is unchanged, and its icons stay where you put them
there.

**What.** There is no new program. Pewter's desktop is Tracker's desktop
told one more word, `pewter`. The rule for where an icon goes gained two
options: start the columns from the right, and keep the bottom right
corner for the Trash. Where each icon has been dragged is kept in its own
pair of attributes for Pewter, so the same Desktop folder has two
arrangements and dragging on one desktop does not move anything on the
other.

A folder opened from it is a *spatial* Tracker: no sidebar, and a folder
inside opens another window rather than this one going there. What the
window keeps - where it is, its size and how it shows the files - is
written onto the folder itself, as attributes, whenever one of them
changes and when it closes.

**One window a folder.** Before a spatial window opens, it asks the
window manager whether a window already is that folder, and if so brings
it forward and ends. A title could not answer that, because two folders
can have the same name; so a window can now say which document it is, a
path, and the window manager keeps it and lists it.

**A detail that mattered.** Tracker's main file had exactly as many local
variables as Lua allows, two hundred, and the first build refused it.
Pewter's flags went into one small table, with one of the old ones.

**How it is checked.** A new test of 9 checks, two machines at once. On
Pewter:
- the Drive is at the top right and the Trash at the bottom right;
- the Trash opens as a spatial folder, with no sidebar;
- opened again, it is the same window, brought forward;
- moved by its title bar, resized by its corner and made a list, and each
  is kept;
- closed by its box and opened again, it is in the same place, at the
  same size, in the same view.

On Astra the desktop keeps its own arrangement, and the Drive opens in
an ordinary window. Two controls each fail it: a window that never
writes what it keeps, and a window manager that forgets which folder a
window is.

**Left for later.** Every keep writes to the folder, which tells the
folder above it that something changed, so the desktop lists itself
again. And on Astra the first row of the desktop sits under the Deskbar,
which this test found and which is older than it.

## 8 November - Pewter's menus move to the top of the screen

**In short:** On Pewter there is now one menu bar across the top of the
screen, as on a Mac. At its left is the Kosmos menu. Next come the menus
of whichever application is in front, and they change as you click
from one window to another. At its right are the sound, the network and
the clock, and the Application menu, which lists what is running. Press
a title and that application's own menu drops down under it. No
application was changed for this. Astra is unchanged: its menus stay
inside the windows.

**What.** A desktop's file now also says where menus go: in the window,
or on the screen. Under Pewter every window tells the window manager the
names of its menus, and only the names. A window with a real menu bar
(the PDF viewer has File, Edit, View and Go) sends those. One whose menu
is the "…" button in its header sends that button, named after the
application. The new menu bar program draws the names for the window in
front. A press on one is passed back to that window, which opens its own
menu under it. So what the menu does still happens inside the
application that wrote it.

**Moved, not copied.** The Deskbar already read the battery, the sound
and the network, and built the Kosmos menu. Pewter's bar wanted the
same, so both now live in two small libraries that both bars use, rather
than in two copies.

**A detail that mattered.** The first picture showed the PDF viewer
losing its title bar's boxes when its menu was opened from the bar.
Pressing the bar had given the bar the focus, so the keys would have
gone there too. The bar is now pressed without taking the focus from the
application in front, as a Mac's menu bar is.

**Measured.** The kit now says, for every menu it opens, how long after
the press it was drawn. Under QEMU: about 30 ms from Pewter's bar,
against 11 from a title inside the window on Astra. Drawing the menu
takes about 14 on both. The rest is a wait: the window manager hands the
bar its press, then composes the screen, and only then reads the bar's
request. On Astra the application draws its menu while the window
manager composes. QEMU's composing is many times slower than the M700's,
so it is measured there before anything changes. If it shows, there is a
fix: the window manager sends a press on a title straight to the
application.

**How it is checked.** A new test of 11 checks, two machines at once,
in about seventeen seconds. On Pewter:
- the PDF viewer and the Text Editor hand their menus over and draw none
  themselves;
- the bar follows whichever is clicked;
- View opens from the bar with the PDF viewer still in front;
- New window, chosen from the Text Editor's menu in the bar, opens a
  second window;
- the Kosmos menu opens.

On Astra the PDF viewer keeps its menus in its own title bar. Two
controls each fail it: a window manager that passes nothing on, and a
bar that takes the focus.

**Left for later.** The "…" button stays in the window as well as in the
bar. A title in the bar is not highlighted while its menu is open. The
Kosmos menu has no search yet.

## 8 November - Pewter's windows get Mac OS's title bar

**In short:** On Pewter, every window now wears a title bar in the manner
of Mac OS 9's Platinum: fine horizontal stripes, the window's name in the
middle, the close box alone at the left, and the zoom and collapse boxes
together at the right. Collapse - the box, or a double click on the bar -
folds the window up to just its title bar, and does it again to open it
out. Astra's windows are unchanged.

**What.** A desktop's file now says which kind of title bar its windows
wear, and Pewter's says Platinum. The window manager tells each window
which one when it opens, so it draws the right bar from its first frame.
The bar itself is drawn by the window, like every title bar since October.

**A choice worth knowing.** The three boxes are drawn and pressed by the
window manager, not by the application, which is how Astra's coloured
circles already work. The reason is a window that has stopped answering:
it can still be closed, because the thing under the close box is the
window manager rather than the application. The cost was teaching the
compositor a second layout - one box at the left and two at the right
instead of three together - which fitted in a spare field the message
already carried, so no message grew.

**Collapse costs the application nothing.** The window manager just shows
the window as tall as its title bar, and everything below is the desktop
again. The application keeps its size and is not even told, so opening
it out shows whatever it drew in the meantime.

**How it is checked.** A new test of 9 checks, two machines at once, in
about fifteen seconds. One opens all 25 applications of the gallery under
Pewter and checks that each placed its boxes Platinum's way. The other
opens Calculator and presses it: the close box's colour on the screen,
collapse by its box, open again by a double click, close by the close
box. To show the test notices, it was run against a window manager that
treated the whole title bar as boxes - the way Astra's run of three is
measured - and it failed, because the double click on the bar never
reached the window.

**Left for later.** In Platinum a window behind loses its stripes; here
it keeps them, because an application is not yet told when its window
stops being in front. And Tracker's and Preferences' title bars are their
right-hand pane, so their close box sits at that pane's left rather than
at the window's.

## 8 November - a desktop is a file, and the machine asks which one

**In short:** Kosmos now asks which desktop you want each time it starts.
It shows a card for each one, Astra and Pewter, with the one you used last
already picked. Once you choose, the same screen stays up as a loader. It
lists each piece of that desktop and each application that opens with it
as it starts, with how long it took, and goes away once all of them have
arrived. Switch Desktop... in the Kosmos menu closes everything and brings
the chooser back without restarting.

**What.** A desktop is now a small text file in `/Kosmos/Desktops`, listing
the programs that make it up. Astra's names what has always run: the
notification banners, the desktop's icons and the Deskbar. Pewter's names
the same three for now, until Pewter's own menu bar and Control Strip are
built. There is still one window manager. A third desktop would be a third
file.

**The part that moved.** The applications that open when the desktop does
(Tracker, the monitor, the log), plus Mail's and Automations' background
helpers, used to be started by the Deskbar. Pewter will have no Deskbar,
so it would have opened none of them, and the loader could not have listed
them. The window manager starts them now. A name on the Startup page that
is no longer a program used to be skipped silently. Now it is tried, and
the loader says it did not start, so a stale tick is visible.

**The surprise.** The window manager's main file had reached the limit
of 200 local names the Lua language allows in one chunk. Adding the new
code failed to compile until its pieces were gathered into one table.

**How it is checked.** A new test of 13 checks, in about twenty seconds.
It boots to the chooser, starts Astra, and checks that the loader heard
each piece arrive and that a program that does not exist was reported as
failed. It then switches desktop from the menu, starts Pewter, and checks
that the choice was remembered. To show the test notices, it was run
against a window manager that reports a failed start as a success, and it
failed.

## 8 November - Xavier's engine answers its first question, on the Mac

**In short:** Xavier, the assistant planned this morning, needs an engine
that runs a language model. We wrote our own in C rather than port one,
and it now runs Microsoft's phi-2 on the Mac. Asked "What is the capital
of France?", it answers "The capital of France is Paris." It is checked
against llama.cpp, the best-known engine. Both cut the text into the same
pieces every time, and at every word of an answer they agree on what
comes next, or both find two words almost equally likely. Nothing of
this runs on Kosmos yet; that is the next steps.

**What.** The LLM Kit's core, `user/kits/llm/llm_core.c`. It reads a GGUF
file, the format llama.cpp writes, in place. It turns text into tokens as
GPT-2 does and runs phi-2's 32 layers one token at a time. It allocates
nothing once the model is open, and it checks every number in the file
against the file before using it. The model was converted on the Mac
twice, at 8 bits a weight (2.96 GB) and at 4 (1.57 GB), and checked
against Hugging Face's checksums before the originals were deleted.

**Why our own.** Kosmos has no POSIX, no threads library and no GPU API,
so any engine would have been rewritten around its edges. One we wrote
can be read from end to end, and the GPU part was always going to be
ours whichever engine we used (`docs/xavier.md`, E1 to E3).

**The finding: llama.cpp does not agree with itself.** The first test
asked for llama.cpp's numbers exactly and failed. A trace showed the
first two layers identical to the last bit and the difference growing
from about the tenth. So llama.cpp was built a second way, plain C instead
of the Mac's vector instructions, and compared with itself. Its two builds
differ by up to 0.62 on a score of about 20 and choose different words
where two are nearly tied. Adding numbers in a different order changes
how they round, and 32 layers make that grow. Our engine is a third
order of adding, and it lands as far from either build as they land from
each other. So the test asks what llama.cpp can promise about itself,
about twice over.

**How it is checked.** 62 checks a model, in the host suite, about 20
seconds for both. Each check of llama.cpp's answers runs against a file
in the tree, so llama.cpp itself is needed only to write them again. Four
broken copies of the engine each fail it: a wrong GELU, a missing bias,
a tokenizer merging in the wrong order, and RoPE pairing the wrong
numbers. The last fails by only two words, because phi-2 rotates few of
its numbers and the test prompts are short. That is written down as the
thinnest check.

**Measured.** On one core of the Mac, with no vector instructions: 9.0
tokens a second at 8 bits, 4.1 at 4. The 4-bit model is slower here
because unpacking its half-bytes costs more than it saves without vector
instructions. Vector instructions are step X2.

## 8 November - Xavier's engine answers inside Kosmos

**In short:** The language model now runs inside Kosmos, not only on the
Mac. One server holds it, `/Running/llm`, and anything may ask it a
question. `xavier "question"` at the prompt is the first thing that does.
Under QEMU it answers with the tiny test model, and every answer is
exactly the one the same engine gives on the Mac.

**What.** A server in C, `user/servers/llm.c`. It loads a model from
`/Home/Models` when asked, four megabytes at a time, and answers one
question at a time. The answer is written into a piece of shared memory
the asker reads, a word at a time as the words are made, so the text
never travels in a message. A program waiting for more words is answered
when there are some, so nothing checks again and again. The server can
read `/Home/Models` and no other file on the disk, through a door of its
own in the disk server. `llm.lua` is how a program asks.

**Why one server.** A model and its memory of the conversation are
hundreds of megabytes. One copy, held by one process, is what every
application asks, rather than a copy in each. That is the rule Kosmos
keeps for everything: kits and servers supply, applications orchestrate.

**How it is checked.** The tiny model (two layers, random weights, phi-2's
own shape and words) goes onto a test disk in all three of its forms. Fifteen
answers come back from the server, and each must be exactly, byte for
byte, what the same engine answers on the Mac. Not llama.cpp's answer:
the tiny model's two best words are often a hundredth apart, closer than
our engine's own difference from llama.cpp, so the test would decide
nothing. The chain is the host test holding the engine to llama.cpp, and
this one holding the server to the engine. It also checks the refusals,
that a mistyped model name keeps the loaded model, that a stopped answer
ends, and that thirty answers give all their memory back. Breaking the
server on purpose, skipping one word of the question and keeping the
memory, fails fourteen checks. 33 seconds.

**Found on the way.** On QEMU with no source of randomness the disk
server keeps its extra doors shut, and whoever asks through one waits
for ever. The LLM server did, the first time it ran. It now asks the disk
nothing until somebody wants a model, and the shut door is on the
roadmap to answer "no" instead of nothing.

**Measured.** Under QEMU, which is not a real speed: the tiny model loads
in 37 ms and answers 86 words a second. phi-2 itself is next, on the M700
when it is on.

## 8 November - phi-2 answers inside Kosmos, and memory above four gigabytes

**In short.** Xavier asked the real phi-2 a question inside Kosmos for the
first time, and it answered: "The capital of France is Paris." The first
try crashed the whole machine instead, and finding out why fixed a fault
any PC with more than about three gigabytes would have met: the kernel
could not reach memory above four gigabytes, although it handed it out.

**What happened.** The model is 1.5 GB, held in one region. A PC's memory
is not one block - devices take a hole below four gigabytes and the rest
of the RAM sits above it - and the page allocator managed both pieces. But
the kernel's window onto physical memory, the one it writes pages through,
was drawn only over the first piece. The first page anyone asked for above
the hole was a page the kernel could not touch, and clearing it was a
crash. Nothing had asked for that much memory at once before.

**Why no test caught it.** There was a test for exactly this, and it was
right; every test machine had 512 MB, which is one piece. The kernel's
suite now boots with 5 GB, so the hole and the memory above it are there
every run - and with the old window that suite crashes on the same line.

**What a person notices.** Nothing, until an application wants a lot of
memory - a model, a large picture, a game - on a machine with a lot of it.
Then it works rather than stopping the computer.

**Measured.** Under QEMU's emulated cores: the model in in 1.8 s, the
12-token question read in 109 s, the answer written at 0.11 tokens a
second. Those are an emulator's numbers; a real core is the next question
(`docs/xavier.md`).

## 8 November - Xavier's window, phi-2 on the M700, and a drive for models

**In short.** Xavier has a window now: questions in, answers written as
they come, conversations kept as files and opened again. phi-2 was copied
to the M700 so it can answer there, and the copy found the file server
telling a Mac that a writable folder was read only. And because a model is
too large for the stick's `/Home`, models get a drive of their own,
`/Models`.

**What happened.** The window was built to the drawing Diego agreed. Each
conversation is a plain file in `/Home/Xavier`. When a question would not
fit in what the model holds, the oldest turn is forgotten rather than the
question refused. The test asks four questions through the window, as a
person would, and holds every answer to the engine's own on the Mac, byte
for byte.

Copying 1.5 GB to the M700 over the network found two faults. The file
server marked every shared folder read only. That was true once and stayed
written after it stopped being true. A Mac believes the mark and refused to
copy, while the tool our tests use never reads it. Then the servers did not
come back after a reboot, because each one started only when told to.
Diego chose to have them all start with the machine.

**Why a drive for models.** `/Home` on the stick is a 2 GB partition, and
with phi-2 in it 295 MB were left - on a 128 GB stick whose other 88 GB
nobody uses. Making `/Home` larger means rewriting it, files and all.
A partition of its own, as virtual machines' disks already have in
`/Sandboxes`, leaves `/Home` alone. It also gives the next model room.

**What a person notices.** Xavier is an application rather than a command.
A Mac mounts Kosmos's writable folders as writable. Sharing comes back on
its own after a reboot. Once the stick has the new partition, models
live in `/Models`, and `/Home` has its space back.

**Measured.** The copy: 1,568,676,736 bytes in 301 s, about 5.2 MB a
second over the M700's wired network, and the Mac's SHA-256 at the end.

## 9 November - watching the model think

**In short.** The LLM Monitor has a second page, Inside, that shows the
model at work on each token: which earlier word each attention head looks
at and how hard, how many of each layer's neurons fired, how the token's
meaning gathers through the layers, and what it might say next with the
odds of each. Nothing is recorded unless the page is open, and with it
open every answer is the same, to the bit.

**What happened.** Diego asked to see the network at work, so he could
understand how a language model reads a question and writes an answer.
The page was drawn first and agreed. Then the engine learned to keep a
short record of each token's passage: per layer, the length of the
token's running vector, the count of neurons whose input was above
nought, and for each head the token it looked at hardest and the share of
its attention that look took. At the end come the ten likeliest next
tokens.

The engine writes those records into a ring of 256 in memory the window
can read. The window asks for them every two seconds, and when it stops
asking, the engine stops recording. At the prompt, `llm inside` asks a
question and prints the same record a line per token.

**Why it is built like that.** A token goes by many times a second, so
its record is data in shared memory, never a message (*control by
message, data by shared memory*). And watching must not change what is
watched, so the tests hold every answer given with the page open to the
answer given with it shut.

**How it is proved.** The test works out every head's strongest look
again on its own, from the head's queries and the stored keys, and holds
the record to it. Each check was then broken on purpose, once per check:
a look recorded one token too far back, the wrong share, neurons counted
below nought, the stream squared, odds not divided by their sum, a batch
given one position. Every one of those breakages failed (`testing.md`
18.732).

**What a person notices.** Open the LLM Monitor and choose Inside while
Xavier answers. The question's words light up as they are read, then
each new word arrives with an arc back to the word its chosen head
looked at. Each layer is a column that brightens where its heads look
hard. Click a head to follow it; Left and Right step back through the
last 256 tokens.

## 9 November - models from Hugging Face, from Xavier's window

**In short.** Xavier's window has a Models page now: the models on this
machine, a search of Hugging Face, and a model brought here, paused,
picked up again and loaded - all from the window, without it ever waiting
on the network.

**What.** The model menu in Xavier's header ends with "Get more in
Models...". The page lists what is in `/Models`, with Load and Remove, and
underneath, what Hugging Face has that matches the words searched for: for
each repository, the one file the LLM Kit would take from it - the smallest
it can read - its size, and Get. A download shows its bar and a Pause
button; Go on picks it up from where it stopped, and the file is checked
against the fingerprint Hugging Face publishes before it can be loaded.

**Why it is built the way it is.** The rule here is that a window never
waits on the network, because a window that waits stops drawing. The
first plan was to fetch the search a little at a time between the
window's frames. But Hugging Face is a secure site, and setting up a
secure connection is a conversation the connection has to wait through.
So the search and each download run as `models` - the same program as at
the prompt - in a process of their own, writing how far they have got to a
small file the window reads four times a second.

**And which models it offers.** Tried on the M700 against the real Hugging
Face, the search for "phi" offered Phi-4, Phi-3.5 and a Qwen - and the
engine can run none of them: it has phi-2's arithmetic and no other. Hugging
Face says which architecture each repository holds, so the page now asks,
and a model the engine cannot run is listed with that reason instead of a
Get button.

**Visible.** In Xavier's window, the model menu's last line; and `models
search` at the prompt now says each repository's architecture.

**Measured.** On the M700 the search came back in 1.7 seconds, with the
window drawing all the while. In the test, a download cut off a third of
the way in, resumed, paused at five and a half megabytes of eight and a
half, and finished whole and checked.

## 9 November - a second model: Qwen2.5 0.5B

**In short.** The LLM engine now runs a second kind of model, Qwen2.5
0.5B, as well as phi-2. It is about a quarter of phi-2's size, and it
answers the same questions llama.cpp's answers, word for word.

**Why.** Diego asked whether Xavier could ever write 20 or 30 words a
second on the M700. Not with phi-2: writing one token means reading every
one of the model's numbers from memory once, and phi-2's are 1.6 GB. The
memory can only go so fast, which puts phi-2 at about 12 to 15 tokens a
second however good the arithmetic gets. Qwen2.5 0.5B is 0.43 GB, so the
same memory reads it three or four times as often.

**What.** A new model is not just a new file. Each family does its
arithmetic in its own order, and Qwen's differs from phi-2's in seven
places. It normalises its numbers a simpler way. Several of its question
heads share one memory head, so it remembers a conversation in a seventh
of the room. It thinks about what it has read and then what to say, one
after the other, where phi-2 does both at once. It squashes its numbers
with a gate rather than a curve. And it cuts words up a little
differently: "2026" is four pieces, and "(x" is one. The engine has each
of those now, said where it differs, so phi-2's path is untouched. Xavier
asks Qwen in the format it was trained on, ChatML, where each turn says
whose it is.

**How we know it is right.** The same way phi-2 was checked: llama.cpp,
the reference engine, ran the model on the Mac and wrote down every
answer, and ours is held to it - 105 checks, every one passing. Asked
"What is the capital of France?", both say "The capital of France is
Paris." And four deliberately broken copies of the engine, each with one
of the new pieces wrong, all fail: the check is not passing by accident.
Writing that check turned up a bug in the test itself, which had been
reading past the end of a buffer and getting away with it.

**Measured.** On the Mac, on one plain core, Qwen writes 26 tokens a
second, and 37 on four. On the M700 it is still to be measured - Diego is
using Xavier there, and it needs a restart first.

## 9 November - selecting a field's text with the mouse

**In short.** Diego asked why he could not select text with the mouse in
a text field. The answer was a decision written down months ago: a field
was one line, and the one selection anybody makes in one line is all of
it. It was the wrong decision - a hand expects what every other system
does - and now a field selects the way the editor does.

**What.** Dragging across a field selects what it passes over. A double
click takes a word. Shift with the arrow keys, Home and End stretches the
selection, and Control with the arrows goes a word at a time. Typing,
Backspace, Delete, cut and paste act on what is selected. The editor got
the double click too.

**Why it was small.** The editor already knew all of this, in `textbuf`,
the piece that holds its lines, caret and selection. The field now lends
its one line to `textbuf` for every move and takes the caret back, so the
rules for what a word is, and for a letter that takes several bytes, are
written once - exactly the premise about not keeping a second copy of
anything.

**Visible impact.** Every field in every application: the browser's
address bar, Tracker's search, Xavier's question, the Settings fields.

**How we know.** A new test opens a window with one field and drives it
with QEMU's own mouse and keyboard - a double click, a drag, the keys -
reading back after each what the field says is selected: seven of seven.
Run against the field as it was this morning, all six selection checks
fail.


## 9 November - downloads from far away, and Qwen on the M700

**In short.** Kosmos downloaded from Hugging Face at about 160 KB/s
while the Mac beside it took the same file at 4.9 MB/s. The cause was a
deliberate limit in Kosmos's network stack; the limit is gone, and what
it was protecting against is now handled properly.

**What.** When one computer sends to another, the receiver says how much
it is ready to take before the sender must stop and wait for a reply.
Kosmos said 16 KB. Across the room a reply takes a thousandth of a
second, so that was plenty; to a server across an ocean it takes a tenth,
and 16 KB a tenth of a second is 160 KB a second. It was 16 KB because
Kosmos threw away anything that arrived out of order - after one lost
piece, everything behind it had to be sent again, and with a bigger
allowance that became a flood of resending that once stalled a large copy
from the Mac. Now a piece that arrives early is kept, in the place it
will have once the gap fills, and the allowance is four times larger.

**Measured.** Losing one piece in 53 on the way in, 8 MB used to not
arrive at all within three minutes; now it arrives at 5.5 MB/s, every
byte checked. With a delay like a far server's, 0.9 MB/s where 16 KB
could never give more than 0.31. The next step, which lets the allowance
grow past 64 KB, is what reaches the Mac's speed.

**And Qwen.** Diego suggested copying the model from the Mac over the
network share instead of waiting for the download - 74 seconds. On the
M700 it writes 18.3 word pieces a second, four times phi-2, because it
is a quarter the size and the machine's memory is what sets the pace.

## 9 November, evening - the allowance past 64 KB, and why Xavier talked nonsense

**In short.** Downloads from far away are now eleven times faster on the
M700. And Xavier's strange answers were not a bug: the model was too
small for them, and the next size up answers properly once Xavier tells
it what it is for.

**What.** The allowance a receiver offers is written in sixteen bits, so
it cannot say more than 64 KB - unless both ends agree, when they first
connect, to multiply it (RFC 7323's window scale). Kosmos now offers that,
and gives a connection that asks for it a larger buffer; the Network Kit's
downloads ask for 4 MB. Testing it needed a server of our own, because
QEMU's built-in network answers with a TCP that never multiplies - so
`tools/farpeer.py` is a small TCP sender on the far side of QEMU, that
holds every frame 50 ms like a distant server. It found a real bug: the
first connection to a machine on the same network was refused, because
the stack failed when it did not yet know that machine's hardware address
instead of waiting for it.

**Measured.** 9.2 MB/s from 50 ms away, where 64 KB allows 1.25. On the
M700 from Hugging Face's CDN, 1.83 MB/s against 160 KB/s before; the Mac
gets 4.9, so something else still holds the M700.

**And Xavier.** Diego asked about the Big Bang in Spanish and got
nonsense. The same question asked of llama.cpp, the program the kit is
tested against, gives the same nonsense: a model of half a billion
numbers does not know enough. Qwen2.5 1.5B answers it properly - but told
only "you are the assistant in Kosmos" it refused anything not about
computers. Agreed next: the kit reads the 1.5B (its output matrix is in a
format the kit did not read, Q6_K), the preamble says Xavier answers
anything, and words are drawn rather than always the likeliest, so an
answer does not go round in circles.

**Built the same night.** The kit now reads the 1.5B model - its last
table of numbers is packed in a format called Q6_K, six bits a number,
which the kit learned to read and is checked against llama.cpp on 105
points. Xavier is told it is a helpful assistant that answers anything in
the language it is asked. And each word is now drawn by its odds rather
than always the likeliest, with a seed: the same seed draws the same
answer, which is what lets the tests still check Xavier's answers word
for word against the kit run on the Mac.

## 9 November, late - the 1.5B on the M700, and its last table in AVX2

**In short.** The bigger model now answers properly on the M700, and with
one loop rewritten it writes 13 words a second there - more than twice
the 5.6 it managed when it first arrived.

**What.** A model's last step turns its numbers into a score for each of
the 151,936 words it knows. That table is stored in its own format, Q6_K,
and it is 233 million numbers to multiply for every word written - a
sixth of the model's work. It was the one multiplication the kit had no
AVX2 version of, so on the M700 it ran 16 bytes at a time while the rest
ran 32, at about three times the cost per number. It now has one, written
as the others are, and it gives exactly the same answer to the last bit:
a test compares the two on twenty-four thousand rows.

**A thing found on the way.** The Mac runs x86 programs through Rosetta,
which can run AVX2 but tells programs it cannot, so the test had never
checked any of the AVX2 loops on the Mac. With one setting it now does,
on every gate.

**Measured on the M700**, the same question and answer: 4.57 s with
Diego's settings (8 processors, the GPU helping), from 5.89; and **2.44 s
on four processors with the GPU off** - 13 words a second. The GPU's help
and the second processor on each core now cost the 1.5B more than they
give; which settings to keep is Diego's choice.

## 9 November, late - the Story page: watching an answer arrive

**In short.** The LLM Monitor has a third page, Story, which takes one
word the model wrote and shows, layer by layer, what the model would have
said had it stopped there - so a person can see the answer form. On
Qwen2.5 1.5B asked for France's capital, the middle layers say
" located" and from layer 24 of 28 every layer says " Paris".

**What.** A model is a stack of layers, each adding to a list of numbers
it hands up - the word's "stream". Normally only the last layer's stream
is turned into words. The Story page turns *every* layer's into words, the
same way (a trick known as the logit lens). To do that without slowing
the answer, the server keeps the last 64 words' streams while the Monitor
watches, and works out the guesses only when the page asks, for one word,
in one pass. Each row also says which earlier words that layer looked
back at, leaving out the first token, which almost every head stares at
and which says nothing.

**Why.** Diego asked where the neural network was in the Inside page and
how to see the words being made; a grid of numbers did not say. The
Story page says it in words.

**Visible impact.** Overview, Inside, Story. Left and Right step through
the words, a click on a layer tells of it. Watching costs the answer
nothing; asking costs about one word's last step.

## 9 November, night - a film from a USB stick, at speed

**In short.** Reading a file from a USB stick went from about 22 KB a
second to well over 100 MB a second under QEMU: 4 MB now takes 38 ms,
where a quarter of a megabyte took twelve seconds.

**What.** Asked for a piece of a file, the stick's server walked from the
file's very first cluster to the piece every time, and *read* every sector
it walked past, one USB transfer each - so the further into a file, the
more it read again, and a whole file cost the square of its size. Now it
finds its place from the table of clusters alone, remembers where the last
read stopped, reads clusters that sit side by side in one transfer of up
to a megabyte, and writes into memory the program shares with it rather
than a kilobyte a message.

**Why.** Diego asked for everything capped for speed to be fixed; this
was the second on the list, and reading it found it far worse than the
cap.

**Visible impact.** A film or a photo library on a stick opens as quickly
as one on the disk. On the way the system's image grew one page past the
room it has below the heap, and the heap was moved two megabytes up.

## 9 November, night - memory as fast as it should be

**In short.** Saving a 4 MB file to `/Temporary`, the part of the system
kept in memory, took a third of a second and reading it back over half a
second; now they take 7 and 17 thousandths. After the USB fix above,
memory had become fifteen times slower than a stick.

**What.** `/Temporary` passed files a kilobyte per message, both ways -
four thousand round trips for 4 MB. Now a large write hands the server a
region of memory holding the bytes, in one request, and a read gets the
rest of the file through a region the same way.

**Visible impact.** Programs that hand each other data through
`/Temporary` - a replicant's state, a picture between two applications -
no longer pay a round trip per kilobyte.

## 11 November, night - ICU: the first of WebKit's foundations

**In short.** ICU, the library every browser uses to understand text in
the world's languages, runs on Kosmos, and gives exactly the answers it
gives on a Mac.

**What.** ICU 79.1 - about 450 C++ files and 33 MB of data describing
hundreds of languages - is downloaded, checked against its published
fingerprint, and compiled for Kosmos without changing a line of it. A
test program sorts words the German way and the Swedish way (Swedish puts
"ä" after "z"), splits Thai, which is written without spaces, into words
with a dictionary, and writes a number as Americans, Germans and the
French do; the same program built on the Mac must say the same sentence
character for character.

**Why.** JavaScriptCore and WTF, the next pieces of the browser, use ICU
from their very first file. Nothing of WebKit could be compiled before it.

**Visible impact.** None yet on the screen; it is a foundation. The test
image carries all of ICU's languages and is 64 MB, and reads and runs in
under half a second. Making it smaller - keeping only the languages
wanted - is a later step, once there is a browser to measure it against.

## 11 November, late night - WTF: WebKit's own foundation runs

**In short.** WTF and bmalloc run on Kosmos. WTF is the library that
WebKit, and so the browser, is written on, and bmalloc is its memory
allocator. A test program uses its threads, its text, its timers and its
files, and has Nebula stop one of its threads mid-run and look inside it.

**What.** WTF and bmalloc are about 750 files of C++ from the same
WebKitGTK release the browser will come from. A script downloads the
release, checks it against its published fingerprint, and applies five
small, explained changes to a copy of it, never to the original. Then it
compiles the copy for both processors.

Most of the work was the rest of the system learning what a large ported
program asks of it:

- the C++ library learned to work with folders and files;
- the POSIX layer learned about a dozen new calls: removing a whole tree,
  copying a file, making temporary names;
- the C library learned to change a thread's rounding mode.

Several things are refused on purpose, and each says why. Kosmos has no
file links. A file's times belong to the server that keeps it. A thread's
stack is the slot it was born in.

**Why.** JavaScriptCore, the JavaScript engine that comes next, is written
on WTF from its first line. Two things it needs most are here now. First,
the ability to stop a thread and read its registers, which is how its
garbage collector finds what is still in use. Linux does this with
signals, and Kosmos has none, so Nebula's own suspend call does it
instead. Second, a run loop that can be woken from another thread.

**Visible impact.** None on the screen yet. The test's one sentence is
checked word for word. Each important part was also deliberately broken
in a separate build, to show the test notices, and it did: the
suspension, the rounding, and the removing of files. The rounding test
itself was wrong twice before it was right. Dividing 1 by 3, or 2 by 3,
gives the same answer whether you round down or to the nearest number,
so neither could tell whether the mode had changed; 1 divided by 10 can.

## 12 November, morning - JavaScript held to its standard

**In short.** test262, the official test suite of the JavaScript language,
now runs on Kosmos. 22,408 of its 23,015 language tests pass, and
`make test262` checks that it stays that way, in two minutes.

**What.** test262 is the suite the language's own committee keeps: one
small file for every rule of the specification, more than twenty
thousand of them for the language alone. Every engine (V8, SpiderMonkey,
JavaScriptCore) is measured against it. It is fetched at one exact commit,
and runs in eight Kosmos machines side by side. Each test runs in a fresh
JavaScript world, usually twice: once in strict mode and once without.
test262 asks the engine for a few helpers of its own, such as a second
global world or a way to detach an array buffer, and the driver now
provides them.

**Why.** W4d showed one script working. This shows the engine working
across the whole language, on Kosmos, and that matters because Kosmos is
underneath every one of those tests: its memory, its threads, its files.
And it found three things wrong with Kosmos rather than with JavaScript:

- **Asking to protect zero bytes was refused.** Linux says yes and Kosmos
  said "invalid argument", and JavaScriptCore stops the program when it
  hears no. Resizable buffers, which WebAssembly is built on, hit it
  every time. Fixed, and now tested.
- **Garbage piled up between tests.** The engine was told there was
  garbage, not to collect it, and about one test in 800 ran the machine
  out of memory. It is now collected every 20 tests.
- **Three tests need more than 512 MB.** Each compiles 65,536 regular
  expressions. The test machines now have a gigabyte. Real machines have
  sixteen.

**Visible impact.** `make test262` answers in two minutes with a count, the
tests that newly pass and the tests that newly fail. The first run took
eleven minutes, most of it waiting on machines stuck in the problems
above. Of the 607 failures, 543 are features this JavaScriptCore release
does not have (`using`, two `import` proposals) or that need a module
loader the driver does not have yet, which WebCore brings later. The
other 64 are where JavaScriptCore itself disagrees with the
specification. None crashes and none hangs. And again every crash died
without a word, which is still the follow-up most worth doing.

## 12 November, small hours - JavaScript runs on Kosmos

**In short.** JavaScriptCore, the JavaScript engine in Safari and in the
WebKit that JetSki is being built from, runs on Kosmos. A script with the
modern language in it prints, on Kosmos, exactly what the same script
prints in Node on this Mac.

**What.** JavaScriptCore is the largest piece of WebKit after the browser
engine itself: about two and a half thousand C++ files. It is compiled
from the same release as WTF, with six more small, explained changes,
for both processors, without the JIT (the part that turns JavaScript
into machine code while it runs). What runs is its interpreter, which
reads JavaScript's bytecode one instruction at a time.

It becomes `jsc.elf`, an application installed in `/Home/Apps/JSC`, as
Doom is. Beside it is `jsc.lua`, so `jsc script.js` at the prompt runs a
file. WebKit's own test shell was not used: it is mostly WebKit's test
harness. Instead about two hundred lines of Kosmos's drive the engine the way any
program embedding it would, and give the script one function, `print`.

**Why.** This is the step where the browser stops being a plan. Every web
page that does anything runs JavaScript, and this is the engine that will
run it in JetSki. It is also the largest program yet run on Kosmos's
POSIX layer, and it found what that layer still lacked. Two dozen maths
functions came from the same musl release as the others. Randomness now
comes from Nebula instead of a `/dev/urandom` that Kosmos does not have.
And a C++ hook for static objects is now accepted, because a process here
never needs to tidy up on its way out.

**Visible impact.** At the prompt, `jsc` runs JavaScript. The test runs a
script with closures, classes, generators, a Proxy, a regular expression,
numbers formatted for Germany and in dollars by ICU, a 2-to-the-64 BigInt,
a hundred thousand objects through the garbage collector, and an `async`
function. Both scripts take 0.8 seconds in QEMU's emulator. Three parts
were broken on purpose to show the test notices: the randomness, a maths
function, and how deep a thread's stack is said to be. It noticed all
three.

The first run died silently, with only an exit code. A Kosmos program
cannot print its own last words yet: only the process that owns the
console may write to it. Finding out took a throwaway kernel that let
everyone write, and a throwaway `abort` that printed the stack it was
called from. That pointed at the random device in one go, and fixing how
a dying program is heard is now near the top of the follow-ups.

## 12 November, afternoon - the libraries pictures are made of

**In short.** The four libraries every browser uses for pictures now run on
Kosmos: zlib for compression, libpng, libjpeg-turbo and libwebp. Given the
test pictures their authors ship, they answer on Kosmos exactly what they
answer on a Mac.

**What.** This is the first of the five steps before WebKit can draw a
page (W5a in `docs/w5.md`). Each library is the original release,
downloaded and checked against what its publisher says it is, and built
with its own build files for Kosmos. Each keeps the fast code written for
the processor's vector unit: NEON on ARM, SSE2 and AVX2 on Intel.

**Why.** WebKit does not decode pictures itself; it asks these libraries.
If one of them is wrong on Kosmos, every page is wrong in a way that would
be very hard to trace back from a page. So each is proved on its own
first.

**How it is proved.** A small test program reads 216 pictures from the four
libraries' own test sets, including files that are broken on purpose. It
decodes each one two or three ways, encodes some back, and writes a line
of fingerprints for each. The same program built on the Mac writes the same
439 lines. That includes the lossy encoders, where the Mac's vector code
and the guest's are different instruction sets and still give the same
bytes. Broken files fail with the same message in both.

**Found on the way.** libjpeg-turbo can bring along a second PNG reader
and a second zlib of its own. Kosmos is meant to have one of each, so that
part is switched off; WebKit doesn't use it. libwebp's own reference
picture is from 2010 and is now off by one in places from its own decoder,
the same on every machine, so it is held to "within one".

**Visible impact.** Nothing on the screen yet. It is the ground WebCore
stands on. It also clears the way to replace Kosmos's own picture decoder
and deflate with these libraries later, which the roadmap has as steps of
their own. `testing.md` 18.781 has the measurements.

## 12 November, evening - the libraries text is made of

**In short.** The two libraries every browser uses for text now run on
Kosmos: HarfBuzz, which decides which glyphs a line of text becomes and
where each one goes, and FreeType, which reads a font and draws its
glyphs. HarfBuzz passes its own shaping suite on Kosmos, all 7,761 cases.

**What.** This is the second of W5's five steps (W5b in `docs/w5.md`).
Both libraries are the original releases, downloaded and checked, and
built with their own build files. FreeType uses the libpng and zlib that
arrived in W5a for colour glyphs and compressed fonts, so the system still
has one of each.

**Why.** Shaping is where text goes wrong quietly. Arabic letters join
differently at the start, middle and end of a word; Devanagari reorders
vowels; a font can swap two letters for one. A shaper that is slightly
wrong draws text that looks plausible and is wrong. So it is held to the
answers HarfBuzz's authors wrote down.

**How it is proved.** HarfBuzz's tests are files of cases: a font, some
characters, and the glyphs and positions expected. Its own runner needs a
tool, `hb-shape`, that is built on GLib, which Kosmos does not have. So a
small C program does what `hb-shape` does for those tests, written from
its sources. On Kosmos and on the Mac it shapes every case, checks it
against the expected glyphs, and fingerprints each test file. It also has
FreeType render the first glyphs of all 545 fonts in the suite. The two
machines agree on all 835 lines, and on Kosmos it takes 5.3 seconds.

**Found on the way.** Kosmos's maths library lacked three small functions
HarfBuzz uses (`hypotf`, `roundf`, `scalbnf`); they come from the same musl
release the rest of it does. One of them had been declared for months
with nothing behind it, which nobody noticed until something called it.
And HarfBuzz's own list of test files has a second list of disabled ones
next to it, which the first runner read by mistake.

**Visible impact.** Nothing on the screen yet. This is what will lay out
every line of text in the browser, and later what Astra's own text could
stand on. `testing.md` 18.783 has the numbers.

## 12 November, night - the database a browser keeps

**In short.** SQLite now runs on Kosmos. It is the small database inside
nearly every browser, holding history, cookies, bookmarks and what web
pages store. It passes its own heavy test program on Kosmos: 46,391 tests,
with no errors.

**What.** This is the first half of W5's third step (W5c in `docs/w5.md`).
SQLite is downloaded as its authors ship it, checked against their
published checksum, and compiled unchanged. Two of its own test programs
run inside Kosmos. One is `fuzzcheck`, which throws tens of thousands of
odd and hostile queries and damaged database files at it. The other is
`speedtest1`, which does ordinary work and reduces every answer to one
fingerprint. Two further tests are ours. In one, two connections compete
for the same database, and the second must wait its turn. In the other,
rows are written, the machine is turned off, and the same rows are read
back.

**Why.** A browser trusts its database with things a person cares about
and never sees. That database has to survive bad input, two parts of
the program writing at once, and the power going off.

**How it is proved.** The same programs are built on the Mac from the
same files with the same settings, and every number is compared: how many
tests each fuzz file holds, and the fingerprint of `speedtest1`'s answers.
Then two deliberate breakages, which must make the test fail. One removes
half of a fuzz file's tests from the Mac's side. The other flips one
byte of the database on the disk between the two boots. Both fail.

**Found on the way.** SQLite itself was never the problem; everything it
found was in Kosmos.

- **Its messages vanished.** A program started from the command line may
  not write to the screen directly. The C library's `printf` already had a
  fallback, but the layer ported code uses did not.
- **It ended the whole program when it gave up**, taking its last words
  with it. And it needed more stack than a Kosmos program starts with -
  eight megabytes, where a program here has a quarter of one.
- **It ran out of address space.** Growing a large block a little at a
  time used up a new piece of the program's 4 GB of addresses on every
  step, and Kosmos never reuses an address. Large blocks now grow with
  room to spare.
- **Two of SQLite's programs, run one after the other, shared settings**
  that on the Mac each would have had to itself.
- **It had nowhere to put temporary files.** Kosmos has no `/tmp`; its
  temporary place is `/Temporary`.
- **Deleting a file while it is still open lost the file.** On Unix the
  file lives on until it is closed. It does here now too.

**Measurements.** `fuzzcheck` takes 80 seconds in the emulator against 4
on the Mac; the whole test, including building the Mac's copies, takes
2 minutes 27. `testing.md` 18.784 has the rest.

**Visible impact.** Nothing on the screen. This is where the browser's
history, cookies and site storage will live. One question goes to Diego
before WebCore arrives: whether Nebula should start reusing addresses a
program has given back, because a browser runs for days.

## 12 November, late - the XML a browser reads, and the cookies it may keep

**In short.** Two more of the browser's libraries now run on Kosmos.
libxml2 reads XML, which the web uses for feeds, SVG pictures and XSLT.
libpsl knows the public suffix list, the list of web addresses like
`co.uk` that belong to nobody, so a site cannot set a cookie for all of
them. Both pass their own test programs on Kosmos with exactly the Mac's
results: 3,886 tests for libxml2 and 324,516 for libpsl.

**What.** This finishes W5's third step (W5c in `docs/w5.md`). Both
libraries are downloaded as their authors ship them, checked against
published checksums, and compiled unchanged. libxml2's own test program,
`runtest`, reads about 3,500 test documents and compares what it makes of
each with the answer kept beside it. libpsl's five tests ask its list about
hundreds of thousands of addresses.

**Why.** WebKit hands every XML document to libxml2, and checks every
cookie's domain against libpsl, and so will curl in the next step. A
mistake in either is invisible until a page breaks or a cookie leaks.

**How it is proved.** The same programs are built on the Mac and every
count is compared, section by section. Then the tests are broken on
purpose, once each way, and must fail: one expected answer changed by one
byte, and half of libpsl's test list taken away.

**What it found.** Not in the libraries. Four things in Kosmos:

- Kosmos had no `glob`, the function that lists the files matching a
  pattern like `test/*.xml`. Without it, `runtest` found no test files
  and reported success, having tested nothing. It is written now, and is
  checked against the Mac's own on 7,497 questions. That check caught a
  bug in it on the first run.
- `fscanf`, which reads numbers and words from a file, never said when the
  file had ended. A program that reads records until there are no more,
  and then asks "did the file end, or was it garbled?", was always told
  garbled. That made 13 of libxml2's tests fail. It is fixed, and has a
  test of its own.
- `setenv` did not exist. Kosmos has no environment variables at all, on
  purpose, so `setenv` now exists and says no.
- The machinery for running another program's `main` inside a kit had been
  written for SQLite last night. It was needed again, so it became one
  shared piece instead of a copy.

**Visible impact.** None yet on screen. Two more pieces of the browser are
in place. Next is the secure connection, OpenSSL and curl.

**Measurements.** `runtest`'s 3,886 tests take 23 seconds in the emulator;
on the Mac they take 1.6. The whole suite takes 32 seconds.

## 12 November, late - the Mac Pro's first boot

**In short.** Kosmos booted on a third machine, a 2013 Mac Pro, and got
most of the way up. It stopped because Apple's firmware had left a hardware
breakpoint set, and Kosmos never cleared it. It clears them now.

**What happened.** Diego wrote 0.11.363 to a stick and started the Mac Pro
from it. Kosmos's own loader ran, then the kernel came up:
- the screen at 1720 by 1440;
- ACPI, the power button and the timers;
- the sound chip;
- the processor's random-number instruction.

Then, while starting the processes that make up the system, it stopped with
two panics on the screen: "debug", then "divide error".

**Why.** An x86 processor has debug registers. A debugger uses them to ask
the processor to stop the moment a program touches a given address or
device port. The Mac's firmware had left one of these set on port 0x80,
the port it uses to show its own progress codes. Kosmos touches that same
port for a microsecond's pause while setting up the old interrupt
controller. So the processor stopped there and raised a debug exception.
Kosmos does not expect one, so it panicked.

The "divide error" was not a division at all. The panic happened halfway
through moving that old interrupt controller's signals out of the slots the
processor reserves for its own exceptions. So when the timer ticked, it
arrived in slot 0, the one that means "divide error".

The ThinkPad's and the M700's firmware leave the debug registers clear,
which is why this waited for a third machine.

**How it was found.** There was only a photograph: no serial port, no log
to copy. The kernel on the stick has no symbols. Its bytes at the address
on the screen were matched against today's kernel, which has them. That
landed on `pic_init`, eleven bytes in, just after its first write to port
0x80. A debug exception at that instruction can only be an I/O breakpoint.

**The fix.** Every processor core now sets its debug registers back to
how the processor resets them, as it installs its exception table. That is
two instructions, in 0.11.391.

**Visible impact.** The Mac Pro should now get past step 10. What comes
after is the next experiment. The firmware also said that VT-x is locked
on, and that the machine's Broadcom network card has no driver here.

**Measurements.** None. QEMU cannot show this, because its firmware never
leaves a breakpoint set, so the Mac Pro is the test.

## 12 November, night - the lock on a browser's connections

**In short.** OpenSSL, the library that makes a connection secure - the
padlock in a browser's address bar - now runs on Kosmos. Its own test
programs pass in Kosmos with exactly the Mac's results: 13,399 checks of
its ciphers and signatures, and 6,875 of its big-number arithmetic and its
TLS handshakes.

**What.** The first half of W5's fourth step (W5d in `docs/w5.md`).
OpenSSL is downloaded as released, checked against its published checksum
and compiled unchanged, with its hand-written assembly kept - the fast
paths that use the processor's own encryption instructions. Three of its
test programs run in Kosmos: one that feeds every cipher, digest and
signature known answers, one for the big numbers keys are made of, and
one that makes two ends of a TLS connection talk to each other in memory,
every way the protocol allows.

**Why.** Every page a browser fetches over `https://` goes through this
library, and so will curl, next. A mistake in it is silent: a wrong
answer looks just like a right one until somebody else reads your
traffic.

**How it is proved.** The same programs are built on the Mac, and every
result line from Kosmos is compared with the Mac's, in order. Then it is
broken on purpose: one expected answer changed, and the test must fail;
one byte of the test server's certificate changed, and 691 handshake
tests fail.

**What it found.**

- The emulator's default processor has no AES instructions, so OpenSSL
  had quietly used its slow, plain-C path, and the fast assembly every
  real machine runs was never tested. The suites now run QEMU's fullest
  processor.
- The control had to be aimed twice. The first broke a certificate the
  tests never open. The second broke a byte of the server's key that
  signing never reads, and even the Mac passed with it. A control that
  passes is a question, not a result.
- OpenSSL asked Kosmos for a handful of ordinary functions: waiting on
  several connections at once (`select`), two old ways of looking up names,
  and on ARM a way of asking which instructions the processor has, which
  Kosmos answers from what it already knows rather than by trying each one.

**Visible impact.** None on screen yet. The browser's secure connections
have their engine.

**Measurements.** The cipher checks take 29 seconds in the emulator; two
post-quantum signature files take 98 seconds by themselves; the TLS tests
36. The three run side by side.

## 12 November, night - the Mac Pro's desktop

**In short.** Kosmos runs on the Mac Pro now, to the desktop. The fix for
the firmware's leftover breakpoint was all it needed: the same image that
stopped at step 10 of 12 now gets to the windows, the Deskbar and the
pointer.

**What.** A 2013 Mac Pro, Kosmos's third real machine after the ThinkPad
and the M700. The first write of the new stick had not landed - the Mac
could not read its boot partition, so the firmware went straight to macOS
- and the second did. This Machine names it a MacPro6,1, finds its Xeon's
eight processors and 32 GB, and lists 68 devices, eight of them driven,
its sound among them.

**What it does not have yet.** A network: its Broadcom card has no driver,
so it cannot be worked on over the network as the M700 is. And its two
FirePro cards are shown the firmware's framebuffer, not driven.

**Visible impact.** A photograph of the desktop on a Mac, with Log,
Monitor and This Machine open on it.

## 12 November, night - a page fetched the way a browser fetches it

**In short.** curl, the program and library that fetches things from the
web - and what WebKit will fetch every page through - now runs on Kosmos.
Inside Kosmos it fetched pages over HTTPS from a server on the Mac:
compressed, in pieces, behind a redirect, with cookies, refusing the
certificates it should refuse. Every answer matched the same curl on the
Mac.

**What.** The second half of W5's fourth step (W5d). curl is built as
released, on OpenSSL from earlier tonight, the zlib compressor and the
public suffix list from yesterday. Its own command-line program runs in
Kosmos, ten commands, each compared with the Mac's.

**Why.** A browser is mostly waiting for the network. Everything WebKit
asks of the web - a page, a picture, a cookie - goes through curl.

**What it found.** curl's build asks the system what it has, by trying to
compile and link a small program for each thing. On Kosmos those trials
were compiled and never linked, so every question got a yes: 18 of the
functions curl asks about do not exist here, and it would have believed
in all of them. Now each trial is linked against a list of everything
Kosmos really defines, read from its own compiled code, so a yes means
yes. Two smaller things: a header had to bring in another, as Linux's and
the Mac's do; and the system's empty "you may not have this" headers went
onto every ported library's path, not only OpenSSL's.

**And the gate.** The test run had grown to 17 minutes. Seven of them were
building ARM programs no test was reading, since ARM testing is paused;
now they are built only when something reads them.

**Visible impact.** None on screen. The browser can now fetch.

**Measurements.** 8 MB over HTTPS in 1.3 seconds inside the emulator.

## 13 November 2026 - The library tests run when the libraries change

**In short.** The test run was past Diego's ten-minute limit, mostly because
of the big libraries the browser needs - OpenSSL, SQLite, curl and the rest.
Their tests now run only when something they depend on has changed. The
run is back to 9:32.

**What.** Each library's test names what it depends on: the layer that
lets ported code run in Kosmos, the compilers, its own build script and
its own test. Before running, the test run takes a fingerprint of those
files. If it matches the fingerprint from the last time that test passed,
the test is skipped and named as skipped. Before any push, every test runs
regardless.

**Why.** Diego chose this out of three options (the others were a longer
limit, or splitting the slow tests further). A library's own tests give
the same answer until the library or what it sits on changes, and work on
a window or a driver touches neither.

**Visible impact.** None on screen. Development is quicker: a change to an
application no longer waits behind four minutes of SQLite.

**Measurements.** 10:47 with every library test, 9:32 with the eleven
skipped. Those eleven are ten minutes of test time, but the run ends when
its longest tests end, so the wall clock saved is 74 seconds.


## 13 November 2026 - Skia draws on Kosmos

**In short.** Skia, the graphics engine Chrome and Android draw with and
the one WebKit uses for its pictures, now runs on Kosmos. It draws eight
test pictures - shapes, colour gradients, text, photographs, blurs and
shadows - and every pixel of every one matches what the same Skia draws on
the Mac. With this, all of the libraries the browser needs are built.

**What.** WebKit carries its own copy of Skia, and Kosmos now builds it
with WebKit's own build files. Skia finds its fonts in a folder and draws
their letters with FreeType, the font engine Kosmos already had. A test
program draws eight scenes: stars and curves, lines with different
corners and dashes, every kind of gradient, text in four sizes and three
styles, PNG and JPEG photographs shrunk and stretched in four different
ways, blend modes, see-through layers, blurs, a shadow, a recorded drawing
played back, and colours converted into another colour space. Each scene
is reduced to one fingerprint of its pixels. The same program, built for
the same kind of processor, runs on the Mac, and the two lists of
fingerprints must be identical.

**Why.** WebKit draws everything through Skia: every page, every letter,
every picture. Before WebKit is asked to draw a page (the next step, W6),
Skia has to be shown drawing correctly on its own. That way, a wrong
pixel in a page can be blamed on the right layer.

**What it needed from Kosmos.** Skia mistook Kosmos for a Mac and had to
be told it is a Unix-like system. Five of its files used C++ text streams,
which Kosmos's C++ library leaves out; they now format numbers the
ordinary C way. The C library gained thirteen more maths functions (all
taken unchanged from musl, where the others came from), and the POSIX
layer gained counting semaphores and the ability to read a file into
memory by mapping it. That last one is why the photographs could not be
loaded the first time: Skia opens image files that way and has no other
route.

**Visible impact.** None on screen yet. This is the drawing engine the
browser will paint with.

**Measurements.** Eight scenes, five photographs, three font families,
23 lines of output in all, every one identical to the Mac's. It takes 0.6
seconds inside the emulator.

## 13 November 2026 - WebKit becomes a Kosmos port

**In short.** WebKit is now built as a port of its own, named Kosmos,
rather than borrowing the port meant for building JavaScript alone. The
browser's next step, drawing a web page, needs parts of WebKit that the
borrowed port turns off. The missing source files now come from WebKit's
own history, at exactly the version Kosmos already uses.

**What.** WebKit is built in "ports", one for each system it runs on:
Linux desktops (GTK), embedded devices (WPE), PlayStation, the Mac, and
JSCOnly, which builds only the JavaScript engine. Until now Kosmos used
JSCOnly. Kosmos now has a port of its own: a file of options, a list of
the operating-system files WebKit's foundation uses here, and a three-line
patch that adds Kosmos to the list of ports WebKit accepts. The released
source Kosmos builds from leaves out the code for the network library
Kosmos uses (curl) and the PlayStation port Kosmos follows, because it was
packaged for Linux desktops. So those 89 files are fetched from WebKit's
own repository, at the exact commit the release was cut from, and laid
over the release. A file that exists in both must match byte for byte, or
the build stops.

**Why.** For now the port builds exactly what JSCOnly did, and that is
deliberate. Changing how WebKit is built and changing what it does are two
different risks, so they are taken one at a time. This step was checked by
comparing the two configurations. They differ only in the port's own
name, and every test still gives the same answers.

**What it found.** On a Mac, WebKit's build looks for Apple's developer
kit before it does anything else, whatever it is building for. JSCOnly
was let through. Any other port is stopped, so the Kosmos port skips that
check. The comparison also caught one option that had been copied from
PlayStation's port and would have changed the memory allocator, so it was
taken out. And the programs built against WebKit's headers are now told
which port they belong to. Without that, WebKit and the programs that use
it would have disagreed about which system they were on. The WTF test now
reports the port, and fails when that information is missing.

**Visible impact.** None. This is groundwork for drawing a page.

**Measurements.** 89 files fetched; 3,447 build steps from scratch; the
WTF, JavaScriptCore and Skia tests pass with every line as before.

## 13 November 2026 - WebCore lays out a page on Kosmos

**In short.** WebCore is the part of WebKit that reads a web page and
works out where everything on it goes. It now runs on Kosmos. Given
fourteen of WebKit's own CSS tests, it places every box exactly where
WebKit expects, on every system WebKit runs on. Nothing is drawn yet:
this step is the layout, and painting comes next.

**What.** A browser engine does four jobs: it reads a page, works out
its layout, paints it and runs its scripts. WebKit's own tests check the
layout by printing it as text. Each test comes with the text it should
produce, a tree of boxes with their positions and sizes. A small Kosmos
program, the harness, now makes a WebCore page with nothing around it:
no window, no network, no GPU. It hands that page a test and prints the
tree. The new test gives it fourteen of CSS 2.1's tests, about how a box
placed at an absolute position finds what it is positioned against. The
fourteen trees are compared with WebKit's expected text line by line,
and every line matches.

**Why.** Text is what WebKit's own builders check every port with, so
it is the fairest test there is. A picture of a page depends on the
fonts and on how they are drawn. A tree of boxes depends only on whether
the engine did its job.

**How it was built.** Most of the Kosmos port is borrowed rather than
written. Each other port handles the same needs, and their files were
used as they are: network and encryption as PlayStation's port does
them, drawing and text as WPE's, form controls from GTK's own theme, and
the screen, fonts and file types from PlayStation's port. What Kosmos
needed of its own fits in eight small files: shared memory as Nebula
regions, the folder fonts are read from, an empty clipboard,
accessibility with no screen reader behind it, keyboard handling and
network security settings. It also took six patches, most of them one
line that adds Kosmos to a list of systems already named there.

**What it found.** Kosmos's GL for 3D, TinyGL, uses some of the same
function names as the GL that WebKit links to. The two had to agree
whose each name is: TinyGL keeps the twenty names it already defines,
and neither is ever called, because there is no GPU. A page also asks
the program that made it how to load files, and the harness's answer is
"you don't", since the test's page is handed over whole. A real loader,
one that fetches over the network, is a later step. And the system
gained a few small things along the way: maths functions, the clean-up
of a thread's private variables when it ends, and a field in the kernel
renamed because C++ cannot use the word `class` as a name.

**Visible impact.** None on the desktop yet. Kosmos now has a browser
engine that understands a page's layout, and the next step is to paint
that page.

**Measurements.** The full WebKit build is 7,827 steps. The test image
is 145 MB. The fourteen pages are laid out in 1.4 seconds in the guest.

## 13 November 2026 - WebCore paints a page on Kosmos

**In short.** The browser engine now draws pages as well as laying them
out. Each of the fourteen test pages is painted into a picture on
Kosmos, and every picture is identical to the one WebKit's Linux port
expects, down to the last pixel.

**What.** Painting is the step after layout: the boxes, borders and
colours turned into pixels. WebCore paints through Skia, the drawing
library that already runs on Kosmos, straight into a Kosmos graphics
surface, the same kind of surface an application's window is made of.
The picture is then saved with the PNG writer Kosmos's screenshots use.

**How it is checked.** WebKit's tests come with pictures of how each
page should look on GTK, its Linux port. The test now compares Kosmos's
picture with GTK's, pixel by pixel. All fourteen match exactly, 480,000
pixels each. A control changes a single pixel of an expected picture,
and the test catches it.

**Why only these pages.** These tests have no text. Text is drawn by
each system's own font machinery, so a page with words would differ
from GTK's picture by a shade here and there even when it is right.
Pages with text are held to WebKit's layout text, as Diego decided.

**Visible impact.** Still none on the desktop. What it means is that
WebCore can now turn a page into pixels on Kosmos. Running scripts and
fetching pages over the network come next (W6d), then the browser's
window (W7).

**Measurements.** The suite takes about 9 seconds; 14 of 14 trees and
14 of 14 pictures match.

## 13 November 2026 - WebCore lays out text on Kosmos

**In short.** The browser engine now lays out pages with words in them,
not only boxes. Twenty-five more of WebKit's tests, with text, are laid
out on Kosmos exactly as WebKit expects. Getting there found a bug that
made some pages lose all their text depending on what was opened before
them.

**What.** These tests use Ahem, a font WebKit made for testing, whose
every letter is a plain square. That makes text measure the same on
every system, so the layout can be compared exactly. Kosmos now passes
all 39 tests in the set: the fourteen from before and twenty-five with
text, in ruby (the small annotations above Japanese text), tables,
right-to-left text, soft hyphens and white space.

**The bug.** Most of the new pages first came out with no text at all,
and which ones changed with the order they ran in. The cause was a font
name in lowercase. CSS treats "ahem" and "Ahem" as the same font, and so
does WebKit's memory of which fonts it has looked up. Kosmos's font
folder did not, so a page asking for "ahem" found nothing, and WebKit
remembered that nothing for every later page asking for "Ahem". Font
names are now matched regardless of case, as Linux's font system does.

**Three smaller things** were about running tests the way WebKit's own
test runner does: letters measured in whole pixels, scroll bars of a
fixed size, and a page's bytes read the way a browser reads them.

**Visible impact.** Still none on the desktop. One thing is now known to
be missing for real pages: text that names no font, which is most text
on the web, has no font on Kosmos yet. That is the next step.

**Measurements.** 39 of 39 layouts and 14 of 14 pictures match; the
suite runs in about 22 seconds in the guest.

## 13 November 2026 - The first words WebKit drew on Kosmos

**In short.** A web page that does not choose a font, which is most of
the web, now gets one on Kosmos, and WebKit has drawn its first sentence
here: "Kosmos lays out a page in its own fonts."

**What.** Pages ask for fonts by kind as often as by name: "serif",
"sans-serif", "monospace", or nothing, which means the browser's default.
On Linux a font system answers which font each kind is. Kosmos had no
answer, so such text had no font and took no space. Now serif is IBM
Plex Serif, sans-serif is IBM Plex Sans and monospace is IBM Plex Mono,
the faces Kosmos already uses.

**How it is checked.** For each kind, the test lays out a page asking
for it beside a page asking for the IBM Plex face by name, and the two
must come out the same, with the text taking space. A control puts the
wrong face beside "serif" and the test catches it.

**Visible impact.** Still not on the desktop, but the picture the test
saves is real text, drawn by WebKit on Kosmos for the first time.


## 13 November 2026 - JavaScript runs in WebKit on Kosmos

**In short.** Pages now run their scripts and load the pictures beside
them, and are laid out once they have finished loading, as a browser
shows them. Thirty-seven more of WebKit's own tests pass, 76 of 76 in
all. On the way, the tests found a bug in Kosmos's networking text
functions that crashed any page with a picture.

**What.** Until now WebKit was handed a page's text and laid it out.
Real pages are more than that: a script moves things, a picture arrives a
moment later, and what you see is the page after all of that. The test
harness now does what a browser tab does on a small scale: it feeds the
page in as if it had been downloaded, lets its scripts run, answers its
requests for files from the disk, and waits for the page to say it has
finished loading.

**The bug.** Every page that loaded a picture made the program stop dead
with no message. Two things made that slow to find. First, a program
that stops itself on Kosmos has its last message thrown away, because
only one process is allowed to write to the console; that is now on the
roadmap. Second, adding diagnostic lines made the crash disappear. The
cause, once found: before fetching anything, WebKit asks "is this host
name actually an IPv6 address?", and Kosmos's answer was "I don't know
what IPv6 is", which WebKit understands as "yes". So every address was
taken as an IPv6 address made of whatever bytes happened to be lying in
memory, and when those were all zeros WebKit refused the request in a way
that crashed. Adding diagnostics changed those bytes, which is why the
crash hid. Kosmos now reads and writes IPv6 addresses properly, checked
against the Mac's own functions on 855 cases.

**Visible impact.** Still not on the desktop: this is the engine, tested
without a window. But it is now an engine that runs JavaScript, and the
next steps are real sites and then a window.

**Measurements.** 76 of 76 layouts, 14 of 14 pictures pixel for pixel,
4 of 4 default fonts; 35 seconds in the guest. The address functions
agree with the Mac on 67 texts and 788 written addresses.

## 13 November 2026 - Wikipedia, drawn by WebKit on Kosmos

**In short.** The first real web page drawn on Kosmos: Wikipedia's
article on BeOS, laid out and painted by WebKit inside Kosmos.

**What.** Kosmos has no browser window yet and cannot fetch pages from
the internet in WebKit yet, which is the next big step. So the page was
saved on the Mac first, with its style sheets and its 129 pictures, and
handed to WebKit on Kosmos as files. Everything on it - the layout, the
fonts, the links, the table at the side, the BeOS logo and screenshot -
was worked out and drawn by WebKit running on Kosmos.

**What it found.** The two pictures in the side table came out empty at
first. Wikipedia asks for them to be decoded "in the background", which a
browser picks up on its next frame; the test draws one frame only. Now,
as WebKit does when it takes a snapshot, it finishes decoding before it
draws.

**Visible impact.** A picture of a real web page, made on Kosmos. Not yet
something you can click: that is the window, W7.

## 14 November 2026 - YouTube, The Verge, Apple and CNN, drawn on Kosmos

**In short.** WebKit on Kosmos now draws four of the sites Diego uses -
YouTube, The Verge, apple.com and CNN - well enough to recognise each
one at a glance. What is still missing is decoders, and YouTube, with its
videos, is now the first priority.

**What.** Each site was opened in Chrome on the Mac, its scripts allowed
to build the page, and the result saved with every style sheet, font and
picture. That copy is handed to WebKit on Kosmos, which lays it out and
paints it in 4 to 7 seconds. Chrome's own picture of each page is kept
beside it, so the two can be compared.

**Why recorded and not live.** Kosmos's WebKit cannot fetch from the
internet yet; that is the next step (W7). YouTube's page is built
entirely by its JavaScript, which needs the network, so for now the page
is saved as it looked after the scripts ran.

**What it found.** Text in a site's own font came out invisible: a
browser hides text for a moment while its font arrives, and the test drew
in that moment. It now waits for the fonts. And the gaps are formats:
YouTube's thumbnails are AVIF, the sites' own fonts are WOFF2, and
Apple's big tiles are videos - none of which this build can decode yet.

**Visible impact.** Pictures of four real sites made on Kosmos, close to
Chrome's. Diego decided the same day that YouTube must work perfectly and
that every decoder the web needs comes first: AVIF and WOFF2, then AV1,
VP9, H.264, Opus and AAC for video.

**Measurements.** Five recordings, 7 to 31 MB each; painted in 4 to 7 s
each in the guest under emulation.

## 14 November 2026 - Tests that run only for what changed

**In short.** `make test` no longer runs every suite on every change. Each
suite remembers what it was standing on the last time it passed, and runs
again only if some of that has changed. A change to a document runs
nothing; a change to one library's test runs that test. A change to
anything shared still runs everything, which is the safe way round.

**What.** Three changes. First, each of the web's ported libraries is
configured again only when its own build settings change. Before, any
edit to the build script reconfigured all of them, which took over a
minute. Second, a test program now says for itself which kit it carries.
Before, each needed a line in a file that every program on the system is
built from, so adding a test meant rebuilding and testing everything.
Third, the gate keeps a fingerprint of what each suite depends on: for a
system suite that is the whole tree, minus the documents and the files
only one library's test uses. Before the gate starts a suite, it compares
the fingerprint with the one recorded at that suite's last pass.

**Why.** Diego, after a twenty-minute gate for one library: "we cant spend
these hoours running tests all the time for every compile". The
libraries' suites already worked this way (since 13 November). This
extends the same idea to every suite.

**Visible impact.** Committing a document or a test now takes seconds, not
twenty minutes. `make test ALL=1` still runs everything, and so does
`make prepush`, before every push.

**Measurements.** Computing a fingerprint of the whole tree takes 0.2 s.
A build-script edit that changes no library's settings: 2 s, where it had
been 74 s.

## 14 November 2026 - YouTube's thumbnails and the sites' own fonts

**In short.** The browser engine on Kosmos now draws AVIF pictures and
WOFF2 fonts. YouTube's thumbnails, which were blank, appear, and every
recorded site is set in its own typefaces instead of a stand-in.

**What.** The libraries added earlier the same day (Brotli, woff2, dav1d,
libavif) were switched on inside WebKit. WebKit's own tests for the two
formats now run on Kosmos: each test page is drawn beside a reference
page that shows the same thing another way, and the two must match pixel
for pixel.

**What it found.** The Verge ran out of memory: with its fonts decoded,
the 512 MB test machine was too small for a news site. Its test machine
now has 2 GB, still less than any real computer Kosmos runs on.

**Visible impact.** A YouTube search for BeOS, drawn on Kosmos, with every
thumbnail. Video is next.

## 14 November 2026 - Films taken apart and decoded, as FFmpeg would

**In short.** Kosmos has a Media Kit: one piece of the system that opens
a film or a song and turns it into pictures and sound. It is the whole of
FFmpeg, the library nearly every video player uses, built for Kosmos with
its hand-written fast code. On fourteen test files it gives exactly the
pictures FFmpeg's own tool gives on this Mac.

**What.** YouTube's videos are AV1 or VP9 pictures with Opus sound in
WebM files, or H.264 with AAC in MP4. Until now Kosmos had a small piece of
FFmpeg that only knew H.264 and AAC, without the fast code. The whole of
FFmpeg is now built for Kosmos, with the processor-specific parts that
make 1080p possible. The kit wraps it in a small door that every
application uses the same way: give it bytes, and get back pictures and
sound with times in microseconds.

**Why.** Diego decided video comes first, and that every application
which plays something stands on one kit: the browser, the Video app and
Music.

**How it is checked.** On this Mac, Homebrew's FFmpeg makes fourteen small
films and songs from test patterns, in every format the kit reads. Kosmos
decodes all of them in QEMU. Every frame and every sample must match the
same test run on this Mac, and every picture must also match what
FFmpeg's own `framemd5` tool says. That second reference is a program
somebody else built. A truncated film and a file of noise must be refused.
To show the check works, one byte is changed in a file: the check fails,
as it should.

**Measurements.** 336 pictures and 17 streams, all identical; 2.3 seconds
in the test machine. Speed on a real processor is the next measurement.


## 14 November 2026 - The browser in three processes

**In short.** JetSki, the browser, now runs the way Safari does: as three
programs that talk to each other. One is the window, one draws each page,
and one fetches from the network. A page drawn this way is identical, pixel
for pixel, to the one GTK's WebKit draws. When the program drawing a page
is killed, the window notices and draws the page again in a new one.

**What.** WebKit, the engine under Safari, splits a browser into processes:
the UI process (the window), a web process for each tab (it lays out the
page and runs its scripts), and a network process that fetches everything.
They talk constantly in both directions, and Kosmos's messages are built
differently from Linux's or macOS's. So the connection between them was
written for Kosmos. Each direction is a ring in shared memory. A short
one-way signal (a pulse) says there is more to read. Anything handed over,
such as a picture's memory, travels as a capability. Each program is
started with only what it needs: a web process cannot see `/Home` or reach
the network, so a hostile page has nothing there to take.

**Why.** Diego made the browser the work now, for daily use. WebKit's
loader for the network only exists in this three-process shape, so the
processes had to come first.

**Visible impact.** Nothing on screen yet: the window is the next step.
What exists is the engine, running as a browser does, headless.

**How it is checked.** `x86-webkit2` loads two test pages, waits for the
picture, and compares it with GTK's expected picture pixel for pixel. Then
it kills the drawing process halfway through a load, and the window must
say so and finish the page in a new process. To show the comparison bites,
one expected pixel is changed, and the test fails.

**What it found.** Two bugs a design document could not show. First, a
drawing process that crashed said nothing, because only programs written in
Lua could print. Now a C program is handed a console, the way servers are.
Second, the window ended its first channel before answering the call that
replaced it. Ending a channel cancels every call waiting on it, so the
other side's first call always failed. The answer now goes first.

**Measurements.** 2.1 seconds in the test machine for the three loads, the
killed one included. WebKit2's own code compiled to 205 objects.

## 14 November 2026 - Pages from the network

**In short.** The browser now fetches pages over the network itself: the
page, then every font and picture it names, by WebKit's own loader in its
network process. Getting there needed a pipe, which Kosmos did not have,
and a fix to a bug in WebKit that crashed the window on the second page.

**What.** WebKit's network process uses curl. curl keeps one thread
asleep, waiting on its sockets, and wakes it by writing a byte down a pipe
whenever there is new work. Kosmos had no pipes, so curl was built without
that wake-up: it fetched the first page and then slept through the request
for its font. The POSIX layer, the part of a ported program that speaks
Unix to it, now has a pipe. Like every descriptor here, it never leaves its
process. The hard part was waking a thread that waits on a pipe and on
sockets at once, because a wait on sockets is held in the network stack.
So a write to a pipe also asks the stack to look again.

**Why.** W7b is the browser using the network. Everything after it, such
as HTTPS, cookies and YouTube, stands on pages arriving this way.

**Visible impact.** Still headless: the window is W7c. Eight test pages
are now fetched from a server and painted exactly as their references.

**How it is checked.** `x86-sites` serves WebKit's own font and picture
tests from this Mac. The guest loads each one by address and compares it
pixel for pixel with its reference. To show it bites, the server refuses
one font, and that page comes out wrong. A second control takes the
pipe's nudge to the stack away, and the test of a pipe waited on beside a
socket then sleeps until its deadline.

**What it found.** Four things. First, curl's missing wake-up, above.
Second, a program that crashed said nothing, because its last words went
somewhere only the console's owner may write; now they go to its own
console. Third, the second page crashed the window, and the bug is in
WebKit itself. Its timers live in a red-black tree, and removing a node
left that node still pointing at its old neighbours, through pointers that
check whether what they point at still exists. So when a neighbour was
freed, WebKit stopped the program. Kosmos's run loop keeps timers in that
tree and GTK's does not, which is why nobody else has met it. A patch
clears the links as a node is removed. Fourth, a reference page drew blank
because the test server lacked the font it named, and nothing had checked
that the reference's own requests were answered.

**Measurements.** Eight pages and six fonts and pictures, each page in a
web process of its own.

## 14 November 2026 - HTTPS in the browser

**In short.** The browser now loads pages over HTTPS, trusting the same
list of certificate authorities as the rest of Kosmos, plus any a person
adds. It loaded example.com from the internet.

**What.** An HTTPS site proves who it is with a certificate, signed by an
authority the browser must already trust. Kosmos has one list of those,
Mozilla's, which its own TLS code already used. Now the browser's network
process gets the same list, built into its program, and also trusts any
certificate file put in `/Home/Settings/JetSki/Certificates/`. That folder
is how you would trust a server of your own.

**Why.** Almost every site is HTTPS, so without this the browser could
only reach test servers.

**Visible impact.** Still no window; that is next. In the test machine, a
real site, example.com, loads over HTTPS from the internet.

**How it is checked.** The test now serves some pages over HTTPS, signed
by a test authority that is placed in the certificates folder, and they
must paint as their references. A server whose certificate was signed by
an authority nobody added must be refused. To show the check bites, the
test authority is left out: every HTTPS page is then refused, and the test
fails.

**Measurements.** Mozilla's list is 225 KB inside a 160 MB program. One
page in the run is refused, as it should be.

## 14 November 2026 - Fonts as files, and a page with its words

**In short:** Kosmos's fonts are now files in `/Kosmos/Fonts`, where
WebKit looks for them by default, so a real page loaded from the internet -
Wikipedia's article on BeOS - now paints with its text instead of without
it. The Broadcom network chip's decoding, written beside this work for the
Mac Pro and the iMac, is folded in and tested.

**What changed.** The faces the system image carries were reachable only
from inside the drawing kit. The server that serves the image's files now
offers them as a folder too, and the system mounts it for every program.
While doing that, the list of those folders turned out to be written twice
- once for the shell, once for a program started from a file - and the new
test failed because only one had the new row. They are one table now.

The test harness also learned when a big page is finished: Wikipedia draws
everything before it says it has finished loading, so the harness now waits
for a picture since the page began and then a quiet second.

**What you can see.** Wikipedia's BeOS page, fetched over HTTPS and drawn by
WebKit on Kosmos, with its words in IBM Plex.

**Measured.** The fonts check: 23 of 23 in `x86-loader`, and 1 of 23 failing
before the second list was fixed. The Broadcom decode: 60 of 60, and 1 of
60 failing when a receive's error bits are ignored.

## 14 November 2026 - The browser in a window

**In short:** JetSki now opens as a window on Kosmos, and Wikipedia's BeOS
article loads into it from the internet, with its text, pictures and layout,
under an address bar. Getting there found a flaw every long-running program
had: memory that was freed still used up addresses, for good.

**What changed.** JetSki is an application installed in `/Home/Apps/JetSki`.
Its window is written in Lua like every Kosmos app; the page inside it is
drawn by WebKit, in the browser's own image, straight into the window's
pixels. Between the window's own events, the browser lets WebKit run for a
few milliseconds, and paints as soon as WebKit has a new picture.

The first real page crashed it. Each time WebKit paints, it copies the whole
page's picture - about 3.7 MB - and frees the copy. Kosmos's memory
allocator got large blocks from the kernel at fresh addresses and, once
freed, gave them back along with their addresses, which are never handed out
again. A process has four gigabytes of them, so a minute of Wikipedia's
repainting used them all up while using almost no memory. Now the allocator
reserves its addresses once and, when a large block is freed, gives back
only the memory behind it, keeping the addresses for the next one.

**What you can see.** Wikipedia's BeOS page in a JetSki window on Kosmos.

**Measured.** The new memory check frees and asks again for a 4 MB block
1200 times - 4.8 GB in all - and gets the same address every time; the old
allocator refused it on the 704th. The window's test: 5 of 5, and 2 of 5
failing when the page is missing.

## 14 November 2026 - Clicking, scrolling and typing in the browser

**In short:** JetSki's page can now be used, not only looked at. A link
can be clicked, the wheel scrolls the page, and text can be typed into a
page's fields.

**What changed.** WebKit has no ready-made way to be handed a click or a
key. Every system it runs on builds those itself from its own events, so
Kosmos now does too: when a click, the wheel or a key reaches JetSki's
window over the page, it is turned into WebKit's own event and handed to
the page. Keys needed one more piece. WebKit decides what Backspace, the
arrows or a letter do in a text field through code each system supplies,
and the code Kosmos had borrowed from the PlayStation port left that part
empty. So the page saw every key and typed nothing. Kosmos now has its own
version of that piece, which deletes, moves the cursor, selects and types.

**What you can see.** In JetSki, links lead somewhere, pages scroll, and a
search box can be typed into.

**Measured.** The browser's test now clicks a link, turns the wheel and
types three letters, and the page's own script confirms each one. When the
page is handed none of these, or only no wheel, or only no keys, exactly
that check fails. The test also found that elements fixed to the window
(`position: fixed`) are not drawn yet, which is next.

## 14 November 2026 - Everything on a page, painted

**In short:** Parts of web pages that never showed up in JetSki now do:
headers and bars that stay put while the page scrolls, anything rotated or
scaled, and anything see-through.

**What changed.** Browsers on most systems hand those parts of a page to
the graphics card to put together separately. Kosmos has nothing to do
that with, but WebKit was still setting those parts aside for it, so they
were simply never drawn. WebKit also switched the setting back on for any
system it didn't recognise. Now Kosmos tells WebKit it has no such help,
and WebKit draws everything into the one picture itself.

**What you can see.** Pages with fixed headers, rotated pictures or faded
panels look as they should. Google search loads and runs, but Google
answers with its "are you a robot" check. Keeping cookies, and telling
sites properly what browser this is, come next.

**Measured.** A test page of seven kinds of box now shows all seven,
where three were missing. The browser's test types into a field that stays
fixed on screen, which before this change was not drawn at all.

## 14 November 2026 - Google search works

**In short:** You can search Google in JetSki. It used to take the
browser for a robot and answer every search with its "unusual traffic"
check; now it answers with results.

**What changed.** Two things were wrong. First, the browser kept no
cookies at all. WebKit only says where to keep them on Apple's systems,
and elsewhere it looked for a file in no folder, so nothing was ever
kept. JetSki now keeps them in a file of its own in `/Home`.

Cookies alone didn't satisfy Google. Before showing results, its page
checks the browser, and JetSki answered three questions the way a program
pretending to be a browser does: it named no operating system, it
said its window was zero pixels across, and a picture drawn in the page
read back empty. Now it names Kosmos, it gives its real window size, and
it can turn a drawing into a PNG, which needed WebKit built with its
picture writers.

**What you can see.** A Google search shows its results: the search box,
the result list, the side panel. Sites that remember you between pages
now do, and they still remember you after the machine restarts.

**Measured.** The browser's test now has 14 checks, up from 8. A cookie
from the server and one from the page's script both come back with the
next request, and the lasting one is in the file on the disk. The page
also reports the platform, the window size and a canvas picture. With the
fixes taken out, exactly the new checks fail and nothing else. One fault in
the build turned up along the way: changing WebKit's build options had
silently done nothing to a build that already existed.

## 14 November 2026 - Videos play in the browser

**In short:** A web page's video now plays in JetSki. It's the first
step towards YouTube, which is the next goal.

**What changed.** WebKit handles a video's controls and timeline, and hands
the actual playing to a "media engine" each system supplies. Apple's uses
AVFoundation and Linux's uses GStreamer. Kosmos now has its own engine,
built on the Media Kit, the same piece the Video app will use. The film is
downloaded through the browser's normal network path, taken apart and
decoded on a thread of its own, and each picture is converted to screen
colours by the same fast routine as every film on Kosmos. It's then shown
when its time comes.

**What you can see.** A page with a video in WebM or MP4 (VP9, AV1 or
H.264) plays its picture. There's no sound yet; that's a later step.
YouTube doesn't use plain videos like this. Its player feeds the browser
pieces of the film itself (Media Source Extensions), and that's next.

**Measured.** The browser's test now plays a six-second VP9 film made on
the spot, and checks that it ran for over a second, is the right size, and
shows its colour in the window: 17 checks. With the film missing, exactly
the two film checks fail.

## 14 November 2026 - A film handed over in pieces

**In short:** JetSki now plays video the way YouTube delivers it: the
page fetches the film in chunks and gives them to the browser one at a
time, instead of handing it an address.

**What changed.** Browsers call this Media Source Extensions. The page
creates a "media source", adds one buffer for the picture and another for
the sound, and keeps appending pieces of film as it downloads them. WebKit
already knows the rules (what is buffered, where to start after a seek,
what to throw away when memory runs short), so Kosmos only supplies two
things for each buffer: something that reads the pieces as they arrive
and splits them into frames, and something that decodes the frames and
passes the pictures on to be shown. Both are built on the Media Kit, the
same part that plays the films.

**What you can see.** The browser test now plays two films side by side.
One plays from an address and the other is appended in pieces, cut
deliberately in the middle of the film's own structure. Both show their
colour in the window. There's still no sound. YouTube itself is next.

**A bug the test found.** When the film was replaced by an error page,
the browser should have said "this isn't a film". Instead it said nothing
and waited forever, because FFmpeg's WebM reader read the error page as
the start of a very long piece and waited for the rest. A page declares
what kind of film it's appending, so Kosmos now checks the first few bytes
against that and refuses straight away, as other browsers do.

## 14 November 2026 - YouTube plays

**In short:** YouTube videos now play in JetSki, Kosmos's browser. The
picture works and the sound doesn't yet.

**What happened.** With the previous step in place (the page handing the
browser a film in pieces), I pointed JetSki at YouTube. Its player asked
the browser about forty questions on what it could play, chose AV1 for
the picture and Opus for the sound, and fed the film in small chunks.
"Me at the zoo" played to the end. Then YouTube's autoplay moved on to the
next video, which started with an ad, and that played too, with the
"Skip" button over it.

**A bug it showed.** YouTube's own JavaScript is heavy, and on an emulated
machine it kept the browser busy for seconds at a time. When the film ran
out of downloaded picture during one of those moments, the player's clock
had already run on, so when more film arrived it continued from where the
clock had got to and skipped the seconds in between. The clock now stops
where the picture ran out. The browser test does the same thing on
purpose (its page keeps itself busy for four and a half seconds), and it
fails if the old behaviour comes back.

**Another one, not fixed yet.** The first version of that test kept the
page busy while the test was typing, and the first letter typed never
arrived. A key pressed while a page is busy should wait its turn, not
disappear. It's on the list.

**Next:** sound, through the Playback Kit.

## 14 November 2026 - Films in the browser have sound

**In short:** a film in JetSki is now heard as well as seen. Its sound is
decoded by the same kit that decodes its picture, played by the kit that
plays every song on Kosmos, and the film's clock now follows the sound.

**What happened.** The browser already took the sound track apart and
then threw it away. Now the same thread that decodes a film's picture
decodes its sound with the Media Kit (the one Opus decoder on the
system), and hands the result to the Playback Kit. That kit has a thread
of its own at sound's priority and sends the sound to the audio server,
like Music and Video do. To make that possible, the Playback Kit was split
into a part any C program can use and a thin layer for Lua programs, and a
small new kit opens a sound stream from C. The browser's web process is
now given the system's audio device, while its network process isn't.

**Why the clock moved.** Until now the film's time came from a stopwatch
started on Play. Now it comes from how much sound the device has actually
played, so a picture is shown when its sound is heard. When the picture
runs out (still downloading), the sound pauses and the clock waits with
it.

**How it is tested.** The browser test's machine now has a sound card,
and QEMU records what it plays to a file. The test film's sound is a
440 Hz tone, and the test checks that over four seconds of a 440 Hz tone
come out. The first try heard nothing, and that turned out to be a feature
working: the test page had its films muted, and muting now really turns
the stream's fader down. There are two controls: a silent sound track,
and the film muted on purpose. Each fails exactly the sound check.

**On real hardware.** The same evening the build went to the M700 over the
network, and Diego opened YouTube there: "youtube plays with sound on the
m700".

**Next:** the Video app moved onto the same Media Kit, so it plays every format the
browser does, starting with Diego's Big Buck Bunny.

## 14 November 2026 - The browser window can be resized

**In short:** JetSki's window now has a grip in its corner and can be made
any size, and the web page inside reflows to fit. Diego found it couldn't
be resized on the M700.

**What happened.** Two things were missing. JetSki opened its window
without asking for a resizable one, so there was no grip. And when the
window did change size, WebKit's view kept the new size to itself: the
part of WebKit Kosmos borrows (from the PlayStation port) stores a new
size but never passes it to the page. A small patch makes it pass it on.

**Visible impact.** Drag the corner and the page reflows, the way a
browser should.

**How it is tested.** The browser test drags the grip 300 pixels left and
100 up and checks two things: that JetSki got smaller, and that the page
itself saw its new width - the page has a listener that writes its width
into its title. The control (JetSki not telling WebKit) fails exactly the
second check. The control was itself broken at first, by a quirk of Lua
that joined two lines into one call; it is the second time a control
needed its own control.

**Also that evening.** Diego asked why one page had five web processes
using 1.2 GB. It is WebKit keeping pages ready: the last two for Back,
and others warm for five minutes in case he returns. He wants it fast and
kept, as long as memory goes back when it isn't needed and nothing leaks,
so the next memory work is making sure it does.

## 14 November 2026 - A click on a link shows at once

**In short:** clicking a link in JetSki now shows straight away that
something is happening: a blue bar under the toolbar, "Opening
news.google.com..." in the status line, and the reload button turns into
a stop button. And the Process Viewer shows memory in MB.

**Why it looked like nothing happened.** WebKit only says a page has started
loading once it has decided to follow the link and, for another site, has
started a fresh process for it. On the M700 that takes a second or more,
and until then the screen didn't change. Diego's clicks were arriving; they
just weren't shown.

**What changed.** JetSki now listens for the very first thing a link does,
WebKit asking "may I go there?", and starts the indicator then. The test
clicks a link and checks JetSki says so before the new page arrives; with
that part switched off, it fails exactly there.

**The Process Viewer** writes memory the way Tracker writes file sizes,
with the same function: 640 KB, 4.3 MB, 607 MB.

## 14 November 2026 - Tabs

**In short:** JetSki has tabs. They sit along the top of the window, as
drawn and agreed on 11 November: each with a close button, a + to open
another, and the window's three buttons at the right end. Control-T opens
a tab, Super with a number jumps to one, and a link meant to open in a
new window now opens in a new tab.

**What happened.** Underneath, each tab is its own WebKit page with its own
process; the one shown is visible and the rest wait in the background.
The surprise was that links meant for a new window had been doing nothing
at all, because JetSki never answered WebKit's "open a new window" call.
Google News opens its articles that way, so that's likely part of why
Diego's clicks seemed to do nothing.

**One key that isn't the browser's.** Control-W closes a tab in other
browsers, but in Kosmos it's the desktop's own prefix key (Control-W then
Q, and others), so it never reaches an application. A tab closes with its
close button.

**How it is tested.** The browser test opens a tab, types an address
into it, switches back with Super-1, closes the first tab with its close
button, and clicks a "new window" link and checks it opens in a tab. With
Control-T switched off, exactly that check fails.

## 15 November 2026 - The Verge opens, and a page that stops says why

**In short:** The Verge and Mercado Libre stopped JetSki's page process
the moment they opened. The cause was in Nebula: a thread was told it had
a megabyte of stack and was given a quarter of one. That is fixed. A page
that stops now says why in its own tab, and JetSki no longer leaves the
processes it started behind as zombies.

**Why it crashed.** JavaScriptCore guards against running out of stack by
asking how big its stack is, and stopping a deep recursion well before the
end with "Maximum call stack size exceeded". The POSIX layer answered a
megabyte, which is the slot each thread has; but Nebula mapped only the
top 256 KB of it, and nothing below ever appeared. The Verge's scripts are
deep enough to parse past 256 KB, so JavaScriptCore walked off the end of
what was really there and the process died on a page fault. Now a thread's
whole slot is filled in as it is touched, so the number JavaScriptCore
reads is true.

**Seeing what went wrong.** Diego: "right now we are blind on whats going
in". The fault was on the serial line and nowhere else. Now Nebula writes
down how a process died - the kind of fault, the instruction, the address
it touched, the thread, the stack - and hands that to the parent when it
collects the child. JetSki collects its web processes as they end, and a
tab whose page stopped shows **This page stopped**, with Reload, Close tab
and Copy details, and **What happened** underneath. The function's name is
not there yet: that needs a kit that reads a program's symbols, next.

**The zombies.** WebKit starts a process per site, and JetSki never
collected one, so each that ended kept its slot in the process table until
JetSki quit. It collects them now, every pump, without waiting.

**How it is tested.** A POSIX test thread touches every page of the stack
it is told it has; with the fix switched off, it dies at 256 KB. The kernel
test that dereferences null now reads the record its child left; with the
record not written, it fails. And the browser test kills a tab's page
process and checks the tab says so, names the process and how it ended;
with the explanation switched off, it fails there.

## 15 November 2026 - A crash says which function

**In short:** when a page in JetSki stops, *What happened* now names the
function the fault was in, not only its address. It comes from a new
Debug Kit, which any program can use.

**Why it was needed.** For The Verge, the address was `0x43131296`. Turning
that into "JavaScriptCore's parser, reading a member expression" took a
Mac, the build's own copy of the program and a developer's tool. Now the
machine does it itself.

**How.** Every program keeps a table of its functions and where each one
starts. JetSki's has 252,000. The kit reads that table once, sorts it, and
finds an address by halving the search. The names themselves are 28 MB, so
it reads only the one it needs. C++ names come out in their written form
(`WebKit::WebPageProxy::changeFont(...)`), not the linker's coded spelling.

**How it is tested.** On the Mac, the kit is held to the toolchain's own
list of functions, in the system's image and JetSki's, and with its sort
switched off it gets 70 of 249 wrong. In the browser test, JetSki names a
known function from its own file on the virtual disk; with the kit not
asked, that check fails.

## 15 November 2026 - Web pages from the disk

**In short:** JetSki opens HTML files from the machine itself. Type a path
such as `/Home/page.html`, press Control-O and choose one in the Open
window, or drop one on the window. The page loads with the files beside
it, its scripts, styles and pictures.

**The snag.** The page itself loaded at once, but a script next to it was
refused. WebKit allows a page from the disk to use another file only if
both are on the same disk, and checks that by looking at both files. The
part of the browser that runs the page is deliberately not allowed to see
the disk at all on Kosmos, so the check always failed. Apple's iPhone
browser and Linux's sandboxed one skip that check for exactly this
reason, and now Kosmos does too. Which files a page can reach is still
decided elsewhere, by the browser's main process.

**How it is tested.** The browser test types a path, opens a second page
with the Open window, and drops a third on the window, checking each
page's title. Each of the four pieces switched off makes exactly its own
check fail.

## 15 November 2026 - Web Inspector

**In short:** JetSki has Safari's Web Inspector. Press Control-Alt-I, or
the bug button at the right of the address bar, and the inspector opens
beside the page: Elements, Console, Sources, Network and the rest, exactly
as Safari has them. It is not a copy. It is WebKit's own inspector, a
web page that comes with WebKit, shown in a second panel of the window.

**What it took.** Very little of the inspector is Kosmos's: a file that
gives WebKit somewhere to put the inspector's view, which JetSki then draws
beside the page and sends the mouse and keys to. Three things were in the
way. The keyboard could not say Control and Alt together with a letter,
so Control-Alt-I arrived as a Tab; it can now, for every application.
WebKit refuses to let its inspector read files from the disk unless they
sit where the system already allows every page to read, so JetSki's
inspector folder is now such a place. And the test machine, at 512 MB,
ran out of memory with three pages and the inspector open; it now has a
gigabyte, still less than any computer Kosmos is meant for.

**How it is tested.** The browser test presses Control-Alt-I, checks that
the page became narrower and that the inspector's half is drawn, then
presses it again and checks the page is whole. Switching off either
opening or drawing the inspector makes its own check fail.

**Not yet:** *Inspect Element* on the right-click menu, which opens
Elements on what is under the pointer. WebKit's own right-click menus are
turned off in this build, and the item waits for them. The Settings
switch for developer features waits for JetSki's Settings window; until
then, the features are always on.

## 15 November 2026 - JetSki, part of Kosmos

**In short:** JetSki is now part of Kosmos rather than an application you
install. It is too big to go inside the file Kosmos boots from (208 MB,
against 43 MB for the whole rest of the system), so it lives on the disk,
in `/Home/System/Apps/JetSki`. Every program sees it as
`/Kosmos/Apps/JetSki`, beside the applications built into Kosmos, and
nothing can change it there.

**Why this way.** Diego decided that JetSki replaces the old browser, and
a browser has to be on every machine, not only on the ones somebody
installed it on. Putting it inside the boot file would have made every
boot and every test machine five times larger and kept 170 MB of memory
taken all the time. On the disk, it costs nothing until it is opened.

**Keeping it in step.** A copy on the disk has to match the Kosmos that
runs it, or it may not start. A USB stick is built with both at once, so
its copy always matches. A network boot is different: the Mac sends the
new system, but the disk keeps whatever copy it had. So when you log in,
a small program called `systemapps` asks the Mac's package server whether
its JetSki is newer, and if it is, downloads it and swaps it in. Each copy
carries a label saying which build it came from, which is how the program
can tell.

**A fix on the way.** Installing a package used to read the whole download
into the program's memory, and then every file again to check it. That
was fine for Doom; it is not fine for a 72 MB download that unpacks to
208 MB. Packages now go straight to the disk as they arrive, and are
checked a few megabytes at a time, using the same code that downloads
Xavier's AI models.

**How it is tested.** The browser test now opens JetSki from
`/Kosmos/Apps/JetSki` and checks that writing a file there is refused.
The file-types test checks that JetSki is listed as Kosmos's own. The
package tests now run with a real disk, as every machine has, and check
that a download leaves nothing behind.

## 15 November 2026 - One WebKit, in /Kosmos/WebKit

**In short.** The big program JetSki runs in is now Kosmos's WebKit,
`webkit.elf`, in a folder of its own, `/Kosmos/WebKit`, read only like
the rest of Kosmos. JetSki names it, and Mail will name it too, so there
is one copy of the web engine on the machine, not one per application.

**What.** `jetski.elf` was renamed `webkit.elf` and moved, with Web
Inspector's pages, from JetSki's folder into `/Home/System/WebKit`,
which every program sees as `/Kosmos/WebKit`. JetSki's folder now holds
only JetSki's own Lua. A program's first lines say which image it runs
in; JetSki's now say `image webkit`, a name rather than a file. Only
names Kosmos itself lists can be used that way, so a program cannot
point at a file somebody put somewhere else. WebKit is a package of its
own, and a network boot brings it the way it brings JetSki.

**Why.** Diego chose one WebKit image for Mail and JetSki to share, in a
folder of its own, so Mail does not depend on an application being
there. This is the first of six steps; the next turns JetSki's door into
WebKit's, made of views, which Mail's message pane will be one of.

**How it is tested.** The browser test installs WebKit in its new place,
opens JetSki, and checks that both folders refuse writes. Its control
puts the image back beside JetSki, where it used to be found, and JetSki
must not start: it says `/Kosmos/WebKit/webkit.elf: not there`.

## 15 November 2026 - A text field that outlived its text

**In short.** JetSki stopped on the M700 a few seconds after switching to
a new, empty tab. The address field still had its cursor at the end of
the previous page's long address, and the next time it was drawn, Lua
refused a position past the end of the now empty text. Every text field
now brings its cursor back inside its text before it is drawn, typed in
or clicked.

**Why it happened.** A field's cursor is a position in its text. A window
that changes the text by assigning it, as JetSki does when tabs switch,
does not move the cursor. The field already guarded against that when
it worked out a selection, but not where it works out which part of a
long text to show.

**How it is tested.** The field test now gives a field 300 characters,
so it scrolls, then empties it underneath the cursor and types one
letter. With the old code the window ends with the same error the M700
showed; with the fix the field holds the letter.

## 15 November 2026 - WebKit's door, made of views

**In short.** The code JetSki used to drive WebKit was JetSki's own: one
page, with tabs inside it. It is now the WebKit Kit, Kosmos's one door to
the web engine, and it deals in *views* - a page each, of a size, with
rules fixed when it is made. JetSki's tabs are views. Mail's message pane
will be one too, with scripts off and links handed on rather than
followed.

**What.** `user/kits/webkit/` holds the kit, in C++ because WebKit is. A
view is made with two options: whether its pages may run scripts, and
whether a link pressed in it is followed or only reported, so its owner
can decide - Mail will send it to JetSki. A view is shown or dark, as a
browser's background tabs are, and paints into pixels its owner holds.
What a person sees in JetSki did not change: its window code speaks to a
small table that turns tabs into views.

**Why.** Mail and JetSki must stand on the same door, or there would be
two copies of the hardest piece of the system. A door shaped like JetSki
- tabs, one page - fits nothing else; a door shaped like a page fits
both.

**What it found.** One of the browser test's deliberately broken builds
had stopped breaking anything five revisions ago: the line it removed had
been rewritten, so it removed nothing and passed. Every such edit now
stops the test when its line is not there.

**How it is tested.** The whole browser test passes unchanged, 38 checks,
and the command prompt - the system's own image - is told there is no
kit called `webkit`, so the kit lives in WebKit's image alone. Five
broken builds each fail only their own checks.

## 15 November 2026 - The browser was waiting on the disk

**In short.** JetSki felt slow because the USB stick was slow to write,
not because the browser was. Kosmos used to write every small change to
the disk and wait for it to land. Now it groups changes and writes them
together, about once a second or whenever a program asks for its data to
be safe. Forty small files now take 81 writes to the stick instead of 286.

**What happened.** Diego found JetSki too slow to use. Timing added to
the browser showed pages waiting tens of seconds for files from a server
that had already answered, while the processor sat 98.7% idle. Counting
the disk's writes showed the cause: over one page load the stick spent
67 of 75 seconds writing. A browser writes constantly: cookies, saved
site data, its databases. Kosmos's filesystem committed each of those
writes on its own, and told the stick to flush every time.

**Why grouping is safe.** Every mainstream system does the same thing
with a page cache. A program that needs its data on the disk asks for it
(`fsync`), and that still works exactly as before. A power cut can lose
the last second's changes, but it can never break the filesystem,
because a group is written whole or not at all.

**What it changes.** Fewer, larger writes to the stick. How much faster
pages load on the M700 is not measured yet: that needs the machine
booted with this build.

## 15 November 2026 - The M700's own disk, seen

**In short.** Kosmos can now read the M700's internal drive, the SATA disk
Windows is on, and the Drives window lists it. It cannot write to it yet:
nothing in Kosmos is given the right to. That is deliberate, until
`/Home` has a partition of its own there.

**What happened.** The SATA driver used to find the disk, print what it
was, and stop. Now it stays and answers requests for the disk's blocks.
It uses the same protocol the USB driver uses for sticks, at a new place,
`/Devices/disks`. The Drives window asks both.

**Why read only first.** The disk holds Windows. The next step, `/Home`
on that disk, needs a partition Diego makes in Windows, and a write path
that only the disk server holds.

## 15 November 2026 - `/Home` on the M700's own disk

**In short.** Kosmos can now keep `/Home` on the M700's internal SATA
drive, in a partition Windows makes and Kosmos formats only when asked.
It never writes a byte outside that partition, and a test holds it to
that. On the way, grouped commits turned out to have made SQLite five
times slower by scattering its database across the disk. Half of that is
fixed; the other half is still being looked for.

**What happened.** Diego chose the safe way to share the drive with
Windows. Windows shrinks its own partition and makes a new one, left
unformatted. Kosmos is told that partition's unique ID when it boots, and
finds it on the SATA disk. Then:
- a blank partition there is not formatted by itself, as a blank stick
  is. A drive inside the machine holding Windows is not a disk with
  nothing to lose, so `mkfs --yes` has to ask;
- the disk server can write only inside the range it claimed. The SATA
  driver refuses anything else, and a program cannot read inside it
  either.

The new test makes a disk shaped like the M700's: a partition table,
Windows' partition with bytes of its own, and Kosmos's partition. It
boots three times, formats, saves a file and reads it back. Then it
checks that every byte outside Kosmos's partition is unchanged.

**The bug it found.** `mkfs` could not reach a disk that held no
filesystem. When there is none, `/Home` is put in memory, and that took
away the names `mkfs` uses to reach the disk. Those names now stay.

**The slowdown.** Grouped commits made SQLite's speed test take 258 s
where it took 49. The database had ended in 1317 pieces where it had 26.
Every rewrite puts the new copy in new blocks and frees the old ones.
Inside a group, freed blocks were kept until the group committed, in
case a failed step had to be undone. So nothing could go back into the
hole the last rewrite left.

But a block the group itself made, freed by an earlier step, was never
part of the last commit, and no undo can reach it. Those are now given
back at the next step.

**What it changes.**
- speedtest1: 96 s, from 258; still about twice the 49 s without groups.
- The database: 809 pieces, from 1317.

**Still open.** What makes up the rest. Committing a group early when it
has freed many blocks changed nothing, so it is something else, and the
next step is logging where each block of the database goes.

## 15 November 2026 - SQLite's other half: writing over the page

**In short.** SQLite on Kosmos is now faster than it was before writes
were grouped: its speed test takes 34 s, where it took 49 s before
grouping and 258 s just after. A program ported from elsewhere, like
SQLite, now writes over a file's existing blocks when it rewrites bytes
inside the file, instead of putting every rewrite somewhere new.

**What was wrong.** kfs writes every change into new blocks and frees the
old ones. That is what keeps a file whole if the power goes. SQLite
rewrites small 1 KB pages inside 4 KB blocks, over and over. Without
groups, the freed block was free at once, so the next rewrite went
straight back into it. With groups, the old block is held until the group
commits, so each rewrite went somewhere new, and the database ended
scattered across the disk in hundreds of pieces.

**What Diego chose.** A ported program's write inside a file goes over the
file where it is. Its own journal protects it from a crash: SQLite saves
the old pages to a journal and asks for it to be kept (`fsync`) before it
writes over the database, and here `fsync` commits the group. Kosmos's own
programs, and any write that makes a file longer, still write into new
blocks.

**What it changes.** The database stays in a few dozen pieces, and a
rewritten page is one block written, with nothing allocated or recorded.

## 15 November 2026 - Cookies a second at a time

**In short.** The Verge now loads in 13 seconds on the M700, where it
took 32. Most of the time was spent on the disk, saving cookies one at a
time.

**What was wrong.** Diego asked why one website wrote so much. The POSIX
layer, where ported programs like WebKit meet Kosmos's files, can now
count what a program does to each file. One load of The Verge set 261
cookies. WebKit saved each in a SQLite transaction of its own: a journal
file made, written, made safe on the disk, and deleted, then the database
written and made safe. That is 522 trips to the disk for one page. On
other systems WebKit puts SQLite in WAL mode, which makes this cheap. WAL
needs memory shared between processes, which SQLite on Kosmos does not
have, so it fell back to the expensive way.

**The fix.** A small patch to WebKit. The first cookie opens a
transaction, and it is committed a second later, or when the browser
closes. Chromium keeps its cookies the same way. A crash can lose the
last second's cookies, never the database.

**What it changes.**

| | before | after |
|---|---|---|
| The Verge, one load | 32 s | 13 s |
| written to the disk | 129 MB | 23 MB |
| time writing | 15.7 s | 2.1 s |

**Still open.** Each commit of the filesystem writes about 250 KB, for a
database of 90 KB. That is kfs's own cost, every program that saves pays
it, and it is next.

## 16 November 2026 - Privacy and Ad Blocking, the engine on

**In short.** JetSki can now stop a page's adverts and trackers before
they are fetched. Nothing is blocked yet on real sites, because the lists
that say what to block (EasyList and the others) come in the next step.
What exists now is the machinery, proved on a test page.

**What it is.** Safari blocks adverts with a feature of WebKit called
content blockers: a list of rules in JSON ("block any address ending in
`ad.js`") that WebKit compiles into a fast matcher once and then checks
every request against. Kosmos's browser is built on WebKit, so rather than
write a blocker of our own we switched that feature on. Adblock Plus did
the same on Safari: it converts its filter lists into these rules.

**What was needed.**
- The feature was off in our build of WebKit. Switched on, it wanted one
  thing WebKit only had on Windows: a way to hand the compiled list to each
  page's process as shared memory. Kosmos's version copies it into a
  region that process can read and not write.
- The WebKit Kit, which every app that shows web pages uses, now finds
  rule lists in `/Kosmos/Filters` and `/Home/Settings/Filters`, compiles
  them, and applies them to every page. A page asked for in the
  milliseconds before they are ready waits for them.
- A small patch to WebKit so an application hears what was blocked. The
  shield in JetSki's toolbar will show those counts.

**How it is tested.** The browser suite's test page now has an advert
script and a tracking pixel, and the suite writes a list that blocks both.
The web server on the Mac must never be asked for either file, and the
page must still work as before. The list compiled in 72 ms.

**Next.** The converter: EasyList, EasyPrivacy and EasyList Cookie
turned into WebKit's rules, so the system blocks from its first boot.

## 16 November 2026 - Privacy and Ad Blocking, the lists

**In short.** JetSki now blocks with the same lists Adblock Plus starts
from: EasyList for adverts, EasyPrivacy for trackers, and EasyList Cookie
for cookie banners. Getting there found an old bug in how the browser's
processes hand each other memory, which had never shown because nothing
had sent two pieces at once.

**What it is.** The lists are written in Adblock Plus's own syntax, about
120,000 lines between them. A Lua library, `filters.lua`, translates them
into the rules WebKit's blocker reads. That happens on the Mac when the
system is built, in about half a second a list, so each machine gets them
already translated: 1.6 MB gzipped. The first time JetSki starts, WebKit
compiles them into its fast matcher and keeps the result on the disk. The
next time, it recognises the lists by their bytes and uses what it kept.

**The bug.** With the real lists, every page crashed before it loaded.
The compiled files turned out to be perfect: same sizes, and a copy of
WebKit's matcher written in Python ran them happily. The browser's main
process saw them perfectly too. The page's process did not. When a
message carried two shared pieces of memory, ours handed them over in the
wrong order, so each list was read under the other's name - and the
matcher found itself reading JSON text as if it were its own code. WebKit
on Linux puts them in the order its decoder expects; ours now does the
same.

**What you see.** A page in JetSki now loads without most of its adverts
and trackers, and nothing is fetched for them. Under the emulator the
three lists take about 90 s to compile the first time; how long the M700
takes is the next measurement.

## 16 November 2026 - JetSki's Settings

**In short.** JetSki has a Settings page now: Control and comma, or the
menu beside the address. It has the home page, whether JetSki opens on it
or on the tabs it had, zoom, pictures, JavaScript and the search engine,
and it remembers them. Building it found three small faults that every
user had been living with.

**What it is.** Settings is a tab like any other, but JetSki draws it
itself rather than WebKit, with the same cards the system's other
settings windows use. A change is written to a file in
`/Home/Settings/JetSki` the moment it is made and handed to WebKit at
once: switch JavaScript off and the next page loads without running its
scripts.

**What it found.** Control-L put the cursor in the address but did not
select what was there, so typing an address added it to the old one -
the test asked for `about:settingshttp://...`. The page kept keys it
should have passed to the window, which is why Control-comma did nothing
at first. And the cards cut a long description off mid-word, in every
application that has them; it now runs onto a second line.

**How it is proved.** The test switches JavaScript off and opens a page
whose script changes its own title: the title must stay as served. Then
on again, and it must change. As a control, JetSki was made to ignore the
switch, and the test failed.

**And the M700.** The blocking lists from this morning take 70 s to
compile the first time JetSki starts there, in the background, and 3.6 s
to be found every time after.

## 16 November 2026 - Five seconds

**In short.** Diego set a standard: a website loads in JetSki within five
seconds, on any computer, or the browser is at fault. The build from this
morning broke it on most of the top 20, and the reason was that every
program's files on `/Home` were being read a kilobyte at a time. They are
read a megabyte at a time now.

**What happened.** Every time JetSki starts, it reads back its compiled
blocking lists - 67 MB - and holds the first page until they are in. That
took 3.8 seconds on the M700, so even Google took five.

**Why it was slow.** Programs ported to Kosmos, WebKit among them, reach
their files through the Storage Kit, and for `/Home` it asked the disk
server for one kilobyte per message. The disk server could always fill a
shared piece of memory of any size in one request; this path had simply
never used it. Now a large read or write gets a region of its own, a
megabyte a request.

**What you see.** The lists are in after a third of a second instead of
four, and 16 of the 20 sites load within five seconds. The New York
Times, eBay, Canvas and Reddit still do not, and they are next. Every
program on Kosmos that was ported from elsewhere reads and writes large
files faster too, not only JetSki.


## 16 November 2026 - A cache that had no room

**In short.** JetSki fetched every file of every page on every visit,
because WebKit had given its disk cache no space. It asks the system how
much room the disk has, and Kosmos used to answer "I can't say", which WebKit
read as none. Now `/Home` says how big it is and how much is free. Giving the
cache room then found a bug in WebKit that had been waiting for it.

**What happened.** WebKit sizes its disk cache from the free space where
the cache lives. Kosmos's `statvfs`, the call that answers that, said "not
supported", because there was no request to ask the disk server. So the
cache was 0 bytes and nothing was kept.

**What changed.** The disk server answers a new request, "how much room",
with the disk's size and its free bytes. The Storage Kit asks it, and
`statvfs` answers from it. A test writes a megabyte and checks the free
space fell by at least that much. Its control, a disk server that says
nothing is free, fails both checks.

**What it woke.** The first full gate with the cache on failed JetSki twice:
WebKit's network process died the moment it touched a cached page. One of
WebKit's functions, which reads a cache file's dates, has a branch for
macOS, Linux's GNOME and Windows, and none for anything else. On Kosmos it
ran off its end without returning. The compiler is allowed to assume that
never happens, so it left out every path through the function, and the
machine ran straight into the function next to it in memory. A WebKit
patch now gives Kosmos its branch. Turning that kind of mistake into a
build error, so the rest of them are named instead of found by a crash, is
on the roadmap for the next full WebKit build.

**Visible impact.** A page seen before can come from the disk instead of
the network. How much faster that makes the top 23 is measured on the
M700 next.

## 16 November 2026 - The padlock

**In short.** JetSki now says how each page came, at the start of its
address: a green padlock for a secure page, and words for anything less.
A page whose certificate doesn't check out is replaced by JetSki's own page
saying why, with a button to open it anyway in that one tab. Building it
found a WebKit bug that wrote every certificate's name as "true".

**What.** The address field reads as the old browser's did:
- a green padlock alone when the page came over HTTPS and its certificate
  checked out;
- *Not encrypted* for plain HTTP;
- *Not secure* for a page opened anyway, or a secure page with something
  plain in it;
- *This machine* for a file, *JetSki* for JetSki's own pages;
- *Refused* when JetSki would not open the page.

Pressing the padlock opens a panel: who the certificate is for, who
issued it, and until when.

**Why.** Diego noticed JetSki was missing what the old browser had: a way
to tell a safe page from one that isn't.

**How.** WebKit already refused a bad certificate. curl checks it, and
WebKit asks the page's owner whether to go on; nobody answered, so WebKit
said no. The kit now answers. A server the person chose to open anyway is
let through, for that tab only and remembered nowhere. Anything else is
refused, with the reason kept for the page JetSki shows. The certificate's
details come from WebKit's own record of the page.

**The bug.** The first test passed everything except one line: the
certificate was "for true". WebKit's helper for reading certificates calls
a function to turn a name into text, and the curl version of it never
defined one. So the call found a general-purpose one, which turns any
pointer into the word "true". Every port of WebKit that uses curl has this.
A small WebKit patch adds the missing function.

**Visible impact.** Every page says how it came, and a refused certificate
is a clear page rather than a status line saying "error 60". A new suite,
`x86-padlock`, checks each case against servers on the Mac in 22 seconds.

## 16 November 2026 - The shield

**In short.** JetSki now shows what its ad blocking did: a shield beside
the address with the number of requests kept out of this page, a panel
naming who was blocked, and this month's totals across every site. One
switch in that panel turns blocking off for the site you are on.

**What.** Press the shield and the panel says which lists are in use, how
many requests were kept out of this page and how many cookies refused, the
hosts blocked most, and the month's totals. The switch at the bottom,
*Block adverts and trackers on* the site, turns blocking off there; the
shield is drawn struck through on that site from then on, and the page
reloads with everything it asks for.

**Why.** Diego asked for it: to see how many adverts and cookies were
blocked, totals by month, and a button to switch blocking off for one
site.

**How.** WebKit already reports each request its rule lists block; the kit
now keeps the host of each as well. JetSki adds the page's counts to the
month's as they grow and writes them to a small file in
`/Home/Settings/Filters`. Turning a site off uses WebKit's own switch, the
one Safari uses: when a page from that site is about to load, WebKit is
told to load it with its blockers off. The first design, a list of allowed
sites compiled as another rule list, could not have worked, because WebKit
judges each list alone - that was found and corrected before any of it was
built.

**Visible impact.** The shield and its panel; the padlock suite gains a
step that checks the count, the month's total and the switch against a
server on the Mac, and a control that proves the switch is what lets the
blocked file through.

## 16 November 2026 - The JIT

**In short.** JavaScript on Kosmos is no longer only interpreted. A
function that runs often is now turned into machine code by JavaScriptCore's
two first compilers, Baseline and DFG, and runs as that - without Nebula
ever letting a page be writable and runnable at the same address.

**What.** JavaScriptCore keeps the code it compiles in one pool of 64 MB.
On Kosmos that pool is a region mapped twice into the process: once read
and execute, where the code runs, and once read and write, where it is
written. Every write JavaScriptCore makes into its code - a new function,
or a jump patched later - goes through a small writer that copies into the
second mapping.

**Why.** Most of what keeps the New York Times, Reddit and CNN slow on the
M700 is script, and an interpreter is several times slower than compiled
code. Diego had said the browser needs the JIT; it came after the cache,
the padlock and the shield, in the order agreed.

**How.** This is the shape iOS uses, and JavaScriptCore already had it:
its *separated heap*. iOS makes the second mapping with a Mach call;
here it is a region mapped twice, which Nebula could already do. Three
things needed changing in WebKit: telling it Kosmos uses the separated
heap, handing it a pool already mapped, and making ten of the Intel code
writer's patching writes go through the writer - on Intel they were
stores in place, which iOS, being ARM, never hit. The Baseline compiler
could not be built without DFG, so both came on together.

**Visible impact.** Nothing to see yet; the numbers come from the M700.
The JavaScriptCore suite now checks that a hot function is compiled and
gives the interpreter's answer, and a control that writes the wrong
mapping crashes, as it must. test262 passes one more test than before.

Along the way the gate turned out to have been testing the wrong image
after any `make test262`: both builds share a directory and make compares
times, not variants. The image now records what it was built as.

## 16 November 2026 - What the JIT changed

**In short.** On the M700 the JIT runs JavaScript 3.4 times faster, and
pages load no faster at all. So the slow sites are not slow because of
their scripts, and the next step is to find what they do wait on.

**What.** `jsc` gained a `--no-jit` switch, so one engine can run the
same script both ways: 65 ms with the JIT, 220 ms without. Then the 23
sites twice: 15 of them within five seconds, exactly as before.

**Why it matters.** It rules a cause out. Reddit, CNN, the New York Times
and the Uruguayan papers spend their seconds somewhere other than
running script - the network, building the page, painting it, or the
messages between JetSki's processes. Measuring those stage by stage comes
before FTL or WebAssembly, which would only make the fast part faster.


## 16 November 2026 - The folder read a thousand times

**In short.** Every web request JetSki made cost about 70 ms before it
went anywhere, because of one folder. WebKit keeps its disk cache in a
folder of 3,606 files, and the disk server re-read that whole folder from
the disk every time anything inside it was touched. The server now keeps
a folder in memory whatever its size. A request costs 6 ms now, and eight
together 28 ms instead of half a second.

**What.** Two fixes to the disk, found one after the other. First the
SATA driver stopped polling: a command now ends when the controller
raises its interrupt, instead of being checked once every 4 ms. That
helped less than expected (43 ms to 36), and counting the disk's commands
showed why: during one page the disk was 86% busy reading 4 KB pieces, a
gigabyte in a few seconds. Moving the browser's cache aside cut a
start's reads from 530 MB to 82. The disk server's cache kept only small
reads, and a folder of 3,606 names is forty blocks - so it counted as a
large read and was never kept. Looking up any file in that folder read
all forty blocks from the disk again.

**Why it matters.** The browser asks its cache about every request, so
this was a tax on all of them, and it grew with every site visited: the
fuller the cache, the bigger the folder, and the slower everything got.
It also explains the "22 ms floor" left with the cache off: the same
large folders, elsewhere on the path.

**Visible impact.** Pages that make many requests start quickly, and a
browser that has been used for a while is no longer slower than a fresh
one. The 23 sites' numbers are in `testing.md` 18.849.

**Measured.** On the M700: a 10 KB request 43 + 30 ms before, 4 + 2
after; 1 MB 43 + 45 before, 4 + 14 after; eight 10 KB requests at once
523 ms before, 28 after. Disk read for one JetSki start 530 MB before,
66 after.

## 17 November 2026 - One connection, made fast

**In short.** HTTP/2 lets the browser send all its requests to a site
over one connection instead of six. On Kosmos that made the New York
Times take 48 seconds instead of 4.5, because one Kosmos connection could
only bring in about 6 MB a second from across town. The connection's
buffer is bigger now, the driver keeps more frames waiting, and when
frames go missing Kosmos tells the sender exactly which ones (selective
acknowledgements). One connection now reads 40 to 50 MB a second, faster
than this Mac's, and 22 of the 23 test sites load within five seconds.

**What.** Three changes in the network, each one found by measuring
after the last. A connection can only have as much data on the way as
its receive buffer holds, and a POSIX program like curl got the stack's
default 64 KB: 64 KB every 11 ms is 5.9 MB a second. Giving those
programs 2 MB made things *worse*, which the network driver's new
counter explained: the sender now sent in big bursts, and the small queue
between the network card's driver and the network server (32 frames)
overflowed, so frames were dropped. A bigger queue (256) helped but did
not stop it. What changed everything was selective acknowledgement: before,
when frames were lost the sender could only learn of one gap per round
trip, or wait a second for its timer; now each acknowledgement names
what has arrived past the gaps, and only the missing frames are sent
again.

**Why it matters.** A web page today pulls most of its weight from a few
servers, and with HTTP/2 each of them is one connection, so one
connection's speed is the browser's speed. Videos, pictures and large
scripts all arrive faster, and they no longer queue behind each other.

**Visible impact.** CNN loads in 5.0 seconds instead of 11.2, El País in
2.7 instead of 5.6, and the New York Times is back at 5.0. Reddit's page
crashed in this run, on a different problem: a new thread found its
stack missing. It is on the roadmap.

**Measured.** One 10 MB video from 11 ms away, on the M700: 5.9 MB/s
before; with a 2 MB buffer alone, 1.3 to 7.2; with 256 frame slots, 1.3
to 3.4 (one 48); with selective acknowledgements, 38.9 to 52.7, six times
out of six. This Mac reads the same video at 37 MB/s. Details in
`testing.md` 18.850.

## 17 November 2026 - Two cores, one page table

**In short.** Reddit's page crashed because a brand-new thread found its
stack missing. The cause was in the kernel's handling of memory maps:
when two processor cores added memory to the same corner of one program
at the same instant, each built a new page table for it and the second
replaced the first, so the first core's memory silently vanished. Now
each program's map has a lock, so only one core edits it at a time.

**What.** A program's memory is described by page tables, a tree of
pages that says where each address lives. A table at the bottom covers
two megabytes and is made the first time anything is put there. Two
cores could each see "no table here yet", each make one, and each write
it into the tree - and the second write erased the first. Whatever the
first core had just mapped was in a table nothing pointed at any more.

**Why it hid.** The kernel's own map had the same bug and got a lock
months ago (18.422). A program's map was written when a program had one
thread, so one core at a time was guaranteed by construction; when
programs got threads spread over four cores, nobody went back to it. And
a table is only missing the first time, so it takes two cores touching a
fresh two megabytes at the same instant - a browser making threads is
exactly that, and even then it was seen once.

**Visible impact.** A page whose process made threads quickly - Reddit
first - could die with a thread that never ran its first instruction.
That should no longer happen, anywhere.

**Measurements.** A new kernel test makes every core map a page into the
same fresh two megabytes at the same moment, 256 times: **680 of 1,024
pages lost before the fix, none after.**


## 17 November 2026 - The browser was waiting for a clock

**In short.** Scrolling a page in JetSki took 14 milliseconds from a turn
of the wheel to a new picture, and almost half of it was nobody working:
the picture was ready and waiting for JetSki to look. Now WebKit taps
JetSki on the shoulder when it has something, and the same scroll is 9.4
milliseconds. On the way, it turned out WebKit's "monotonic" clock on
Kosmos was really the calendar clock, and different in every process.

**What.** JetSki talks to two things: the window manager (keys, the
mouse, the wheel) and WebKit (pages, pictures). It used to ask the window
manager for news, waiting up to one scheduler tick, and then give WebKit
a few milliseconds to handle its messages. A picture that arrived between
those two moments sat until the tick ran out. Now WebKit's connection
says "messages arrived", and the WebKit Kit sends the window manager a
pulse with a code the window manager gave JetSki - the same mechanism
Tracker uses to hear that a folder changed. The window manager answers
JetSki's waiting question at once, and JetSki lets WebKit run.

**Why.** Diego chose to fix the waiting before taking out copies or using
the GPU, because the measurement said the waiting was the bigger part.
And a first, cheaper guess - simply giving WebKit longer after each turn
- was tried and changed nothing, which is how the real cause (the order
of JetSki's loop) showed itself.

**Visible.** Scrolling answers about a third faster. When nothing is
happening, nothing runs: with a page open, the machine was 99.8% idle.

**Measured** (M700, Wikipedia's BeOS page, 40 notches): wheel to picture
14.1 ms before, 9.4 after; the picture's wait after arriving 3.9 ms
before, 0.2 after. What is left is work: 5.6 ms painting the strip that
scrolled into view and 3.0 ms drawing the page into the window - which is
what the next two steps, the copies and the GPU, are for.

## 17 November 2026 - Drawing only what changed

**In short.** Scrolling a plain page in JetSki now takes 2.5 milliseconds
from the wheel to the new picture on the M700, down from 5.7, and
Wikipedia 5 down from 7.5. The browser stopped copying the whole page
around for every frame, and when only a corner of a page changes, only
that corner is drawn and handed to the screen.

**What.** Four copies came out. WebKit's own store of the page was copied
in full before being drawn into the window; now it is drawn straight from
its own pixels. Scrolling moved that store through a spare copy; now the
rows slide in place. The web process made a fresh shared buffer for every
update; now it reuses the last one. And JetSki redrew its whole window -
tabs, address bar, page, status line - for every picture WebKit sent,
even a picture where only a little box changed. Now WebKit says which
rectangle changed, and JetSki draws and sends just that rectangle. The
catch is that a window has two buffers taking turns on the screen, so each
buffer keeps its own list of what it has missed; a test checks that a
change is never lost from one of them. And one more surprise: every turn
of the wheel drew a whole frame of its own before the new picture had even
arrived, which doubled the frames and made the real one wait.

**Why.** This is the step Diego chose before using the GPU: take out the
work that should not be there at all, then give what is left to the
graphics hardware.

**Visible.** Scrolling feels quicker, and a page that animates something
small (Wikipedia does, about twenty times a second) costs the machine a
twelfth of what it did.

**Measured** (M700, 40 notches): plain page, wheel to picture 5.7 ms to
2.5 and 80 frames to 40; Wikipedia 7.5 ms to 5.0-5.7; a still Wikipedia
draws 0.08 million pixels a frame instead of 0.96. What remains on
Wikipedia is WebKit repainting its whole view on every scroll, which needs
layers and a GL driver - a later step.

## 17 November 2026 - The page is never copied

**In short.** JetSki no longer copies the web page into its window at
all. WebKit draws the page into two buffers that the window manager reads
directly and puts on the screen, so the browser's own copy - about 0.7 ms
of every scrolled frame on the M700 - is gone.

**What.** Until now WebKit drew the page into its own store, and JetSki
copied the part that changed into its window, which the window manager
then put on the screen. Now the window manager can be handed a second
picture to lay over part of a window - an *inset* - and WebKit's store is
that picture. It has two buffers, like every window: WebKit draws into one
while the screen shows the other, then they swap. The buffer that was on
the screen has missed what changed in the meantime - a scroll, a box
redrawn - so before WebKit draws into it again it catches up, sliding its
rows by every scroll it missed in one move and copying across the
rectangles that changed.

**Why.** Diego chose this over having the graphics chip do the copy: a
copy nobody makes is cheaper than a copy made faster, and the M700's
blitter is slow at reading an application's buffers. The second buffer
was his choice too, once it was clear that showing WebKit's single store
would let the screen catch it half drawn during a scroll.

**Visible.** Scrolling costs a little less; a page that animates costs the
browser nothing at all for each picture, since it no longer touches the
pixels. And the inset is a piece any application can use - a film player
or a 3D view could hand its picture to the screen the same way.

**Measured** (M700, 40 notches): the page drawn into the window 0 times
(before, 0.7 ms a scrolled frame); wheel to picture 2.7 ms on a plain page,
5.3 on Wikipedia; a still Wikipedia's 598 pictures in 24 seconds reached
the screen without JetSki copying a pixel.

## 17 November 2026 - Why Wikipedia repainted everything

**In short.** Every turn of the wheel on Wikipedia made the browser redraw
the whole page, not just the strip that scrolled into view. The reason was
inside WebKit: on each scroll it re-measured the boxes that stick to the
top of the screen - Wikipedia's contents and tools columns - and that made
it re-measure and redraw everything around them. Kosmos now just moves
those boxes. A scrolled frame costs the page's process 3.1-3.4 ms instead
of 3.8.

**What.** Some boxes on a page are "sticky": they scroll with the page
until they reach the top, then stay there. Wikipedia's two side columns
are. WebKit, after every scroll, asked for these boxes to be laid out
again - re-measured from scratch - and because a sticky box sits inside
the page's normal flow, every container around it was re-measured too.
Wikipedia's page is a grid, and a grid that is re-measured redraws all its
contents, so the whole view was redrawn every time. But where a sticky box
sits after a scroll doesn't need measuring at all: it is worked out from
the scroll position when the box is placed on screen. So Kosmos skips the
re-measuring and just places the boxes again.

**How it was found.** By asking: the page's process was made to say, for
every scroll, who asked for something big to be redrawn - and the answer
was the page's main containers, each twice, from top to bottom, which is
what re-measuring looks like. The first version of the fix forgot that the
boxes are placed before the scroll's new position is known, and left them
one step behind; a new test page that sticks a header and a column inside
grids, as Wikipedia does, caught it straight away.

**Visible.** Scrolling Wikipedia is a little lighter; the sticky columns
look the same.

**Measured** (M700, 40 notches): the re-measuring 1.2 ms to 0.5 a frame;
wheel to picture 5.3 ms to 4.7-4.9. What is left is drawing the page,
2.4-2.7 ms, because half the view really does change.

## 17 November 2026 - Never waiting for a picture

**In short.** Every so often the browser's window stopped for half a
second. When it was asked to show a page whose picture wasn't ready yet,
the part of WebKit inside the browser asked the page's process for one
and waited - up to 500 ms - with the whole window frozen meanwhile. Now
it doesn't wait: it shows nothing new and draws the picture when it
comes. On the M700 those moments went from 126 ms each, the worst half a
second, to under a millisecond.

**What.** A page in the browser lives in another process, which sends
pictures of it as they are ready. WebKit was written for desktops where a
window must produce its pixels the moment it is asked, so when it had no
picture it demanded one and sat waiting. It also threw its picture away
ten seconds after it was last used, so a page left still and then shown
again paid the same wait. On Kosmos the picture always arrives on its own,
so waiting buys nothing: the browser returns at once, and keeps the
picture of a page that is on screen.

**How it was found.** The numbers had said it for days - "most 501 ms"
beside the time taken to draw the page into the window - in the test runs
and on the M700, and nobody had read that line. Following it led to the
one function that waits with a half-second timeout.

**Visible.** No more half-second stops while a page loads or after it
has sat still.

**Measured.** M700, Wikipedia: 126 ms, worst 501, to 0.6, worst 0.8. The
test now fails any run where a paint waits 400 ms or more: 504.6 without
the fix, 3.8 with it.

## 17 November 2026 - One browser

**In short.** Kosmos had two web browsers: the old one, built in
September on the NetSurf engine, and JetSki, on WebKit, which is the one
anyone would use. The old one is gone. Everything that used to open it -
a link in an email, the IDE's and Cafesa3D's tutorials, the cheat sheet
in the menu - now opens JetSki.

**What.** Removing an application means first finding what leans on it.
Mail sent its links to it; the IDE and Cafesa3D opened their tutorials in
it; the menu's Documentation entries started it; the dock had it pinned;
and favorites - bookmarks kept as files - opened in it. All of those now
go to JetSki through one small library, so there is one way to say "show
this page", and it opens a new tab in the JetSki already open rather than
a window each time.

**The catch was the tutorials.** They are carried inside the system's
image, and only the old browser knew how to read a page from there. So
the image now shows them as ordinary files, in a folder called
`/Kosmos/Documentation`, which JetSki - like any program - can open.

**What is left of it.** The old engine still draws HTML emails in Mail,
so its C code stays, as a kit, until Mail draws them with WebKit; then it
goes too.

**Visible.** One browser in the dock and the menu; the tutorials open in
JetSki.

## 17 November 2026 - Downloads

**In short.** Clicking a link to a zip file, an installer or a PDF its
site wants saved used to do nothing useful in JetSki: the page "failed".
Now the file is saved into the Downloads folder, a button in the bar
shows how far it has got, and when it is done a notification says so -
pressing it opens the file.

**What.** WebKit already knows when something is not a page; it needs to
be told, each time, "download that". The kit now says so for anything
the page cannot show and anything its server calls an attachment, and
picks the file's name - the one the server suggested, with `(2)` added
when there is already one. WebKit's network process, the part that talks
to servers, writes the bytes into the file itself, so the browser window
does no copying at all; it only hears how many bytes have arrived.

**Why it took three runs.** The first gave the file the name
`kosmos-test (9999).bin`, because the check for "is this name taken?"
found the download itself. The second said each download was a page that
had failed: WebKit reports the page as stopped a moment *before* it says
the download began, so the browser has to remember it chose to download
at the moment it chose.

**Visible.** A downloads button in JetSki's bar with a thin progress line;
Control-J lists them, with Cancel, Open, Show in Tracker and Retry.
Pause is not there yet - the networking library WebKit uses here cannot
pick a download up again from where it stopped.

## 17 November 2026 - The right button

**In short.** Right-clicking a page in JetSki now offers what you
pointed at: a link can be opened in a new tab or copied, a picture's
address the same, and selected words copied or searched for. Moving the
pointer over a link shows where it goes at the foot of the window.

**What.** WebKit tells the browser what the pointer is over every time it
moves onto something new. The kit keeps that, and asks the page for the
selected words whenever a click or a key might have changed them; the
menu is built from both at the moment of the press.

**Why it matters.** Opening a link in a tab of its own and copying an
address are two of the things a browser is used for most, and without
them every link was a one-way trip.

**Found on the way.** The downloads panel added a day earlier had never
appeared: JetSki paints its own panels, and that one had been added to
the window without being added to the painting. Its test checked what
JetSki *said*, not what was on the screen.

## 17 November 2026 - Write, taken apart

**In short.** Kosmos Write's editor - the part that takes your typing,
moves the caret and remembers what to undo - and its page view, its
formatting panel, its toolbar and its charts were all inside Write. They
are now separate pieces any application can use, so Mail's new
composer, a spreadsheet and a presentation program will share them
instead of copying them. Write works exactly as before.

**How it was done.** By a second worker beside this one - a "lane" -
which may only touch its own files and never builds or commits; this
session built the system and ran Write's tests on it before taking it in.

**What it found.** Typing one letter into the middle of a 100-page
document takes 1.5 ms here, and nearly all of that is laying out the
pages again - every page, on every key. That is the next thing to fix for
long documents.

## 17 November 2026 - Find on the page

**In short.** Control-F in JetSki now finds words on the page: every
match is marked, the bar says which one of how many you are looking at,
and Return moves to the next. It is the same bar as Write's.

**What.** WebKit already knew how to find and mark words. What it did not
tell a program like JetSki was *which* match was showing - only how many
there were. A small piece added to Kosmos's side of WebKit asks for that
too, and the bar shows it.

**Why it matters.** Finding a word on a long page is one of the things a
browser is used for every day, and the bar being one kit for Write and
JetSki means Mail and Sheets get the same one rather than a third copy.

**Found on the way.** Pressing Return in any field on a page - a search
box - had been opening the downloads panel instead of reaching the page,
since downloads arrived. The keyboard sends Return and Control-J as the
same code, and Control-J was the downloads' key. Downloads moved to
Control-Shift-J, and the test now types into a page's field and presses
Return, which would have caught it.

## 17 November 2026 - The dock's magnifying glass

**In short.** Moving the pointer along the dock now makes the icon under
it grow, and the ones beside it a little less, the way the Mac's dock
does; a slider in Appearance sets how much, or turns it off.

**What.** A window can only draw inside itself, and a grown icon is taller
than the dock. So while the pointer is over the dock, a second, invisible
window sits over it and the space above it, and draws the whole row of
icons grown - and, when the dock floats, the dock's own pill, widened to
fit them. The dock underneath still takes the clicks, matched against
what the grown row shows, so you click what you see.

**Why it matters.** Diego asked for it as a priority. It is the kind of
small, live thing that makes a desktop feel responsive: the icons follow
the pointer at sixty frames a second, and only that small window is
redrawn as they do.

**Found on the way.** The dock's test of its transparency read a pixel at
the dock's right end - which, with the pointer still resting on the dock,
was now covered by a grown icon. A saved picture of the screen showed it
at once, where three runs of guessing had not.

## 17 November 2026 - Starlight's amber plate

**In short.** Under the new Starlight desktop, the window you are working
in carries its name on an amber plate at its top-left corner, cut on a
slant; every other window's plate is a quiet indigo. Click another window
and the amber moves to it at once.

**What.** It is the first of Starlight's identity cues - Diego wanted the
desktop to stop looking like a copy of macOS and have something of its own,
as BeOS had its yellow tab. The window draws its header and leaves room;
the window manager, the one process that knows which window is in front,
draws the plate as a small picture and the compositor lays it on top. So
the change of focus costs two small pictures, not a redraw of any window.

**Why it matters.** It is the "you are here" of the screen, made in the
system's own manner rather than borrowed, and it is built so a window
cannot get it wrong: it does not know whether it is in front, and does not
need to.

**Found on the way.** The first picture had the whole top bar amber,
because every look paints its bar in the same colour as its tabs; a look
can now give its bar a colour of its own. The second picture had no top
bar at all - it had crashed on a shape its drawing kit did not offer -
and the test had passed, because it only looked for processes that died.
It now looks for a program stopped by an error too, and was shown to fail
when the shape was taken away again.


## 18 November 2026 - Starlight's cut corner

**In short.** Under Starlight, menus and the floating dock now have their
top-left corner cut on a slant instead of rounded, the cut's two ends
softened; the other corners stay round. It is the second of Starlight's
signatures, after the amber plate.

**What.** Diego asked for "a good mix between geometric look and some
more pleasant rounded or soften style", so the cut is one corner only, and
where its slant meets the edges it curves rather than making a point. The
compositor, which already rounds every window's corners by putting back
what was behind them, puts back a slanted piece instead on a menu's
top-left; the dock draws its own outline.

**Why it matters.** It is a shape no other desktop uses, on the things
that float, and it costs nothing a frame: the corner is restored once per
repaint, as the rounded one was.

**Found on the way.** The first check of it saw no difference at all
between a cut and a round corner - because a night-blue menu on a
night-blue desktop differs by only fourteen levels, and the check was
looking for thirty. Printing the two corners as characters showed the cut
was there all along.

## 18 November 2026 - Kosmos's star

**In short.** Under Starlight, Kosmos's mark is now a four-pointed star:
on the top bar's amber Kosmos button, before each window's title, and as
a small "glint" under each running application in the dock - amber for
the one you are using - where other desktops put a dot.

**What.** Astra means stars, and the star was the third of the signatures
agreed for Starlight. Diego found the first drawing's star too spiky, so
its sides curve inward, a softer shape. One small function draws it
everywhere.

**Why it matters.** Together with the amber plate and the cut corner, it
is what makes a screenshot of Kosmos recognisable as Kosmos rather than a
copy of something else.

## 18 November 2026 - Windows that unroll

**In short.** Under Starlight a new window now appears as its title strip
first, and the rest of it rolls down beneath it in about a sixth of a
second - like a blind being lowered.

**What.** The window manager already knew how to show only the top of a
window, for collapsing one to its title bar. The unfurl uses the same
trick for a moment: it shows a little more of the window each frame until
all of it is there. Nothing is stretched or faded, so it costs less than
the zoom-and-fade other desktops use.

**Why it matters.** Motion is part of a desktop's personality, and this
one is Kosmos's own. Under QEMU it ran at about eighty steps a second.
Closing - the window rolling back up into its title - comes next.

## 18 November 2026 - Liquid Glass

**In short.** Kosmos has a third family of looks, Liquid Glass and Liquid
Glass Night. The dock, notifications' banners, every menu and the windows
the kit draws are now made of frosted glass over the wallpaper. Their edges
bend and catch the light, the window in front has a warm rim, and Kosmos's
own marks are made of glass too: the amber title plate, round buttons like
drops, a small star of light at the front window's corner. Built
overnight in fourteen revisions, 0.11.472 to 0.11.486, and pushed.

**What.** A look says what the material is; the window manager and the
scene draw it. Once, when the wallpaper is chosen, the wallpaper is
blurred into a picture of its own - "the glass". After that a glass
surface never blurs anything: it reads the glass where it lies, which is
why frosting costs a copy rather than a blur at every frame. The edge is a
small table made once - "the lens" - that says, for each pixel near a
rounded edge, where to read the glass from, so the edge seems to bend what
is behind it; the light is added on top, toward the upper left. Where the
bend is strongest the colours part, amber one way and indigo the other.

**Why.** Diego's "Love the Liquid Glass!" on 17 November, and the bet the
whole desktop makes: a material that looks expensive and costs almost
nothing, because the expensive part is done once. And the cues are
Kosmos's - the plate, the star, the warm light on the window in front - so
it is Kosmos made of glass, not a copy of someone else's.

**What you see.** Over a wallpaper, the dock and menus show the wallpaper
softened through them; a window's ground is the same glass, quieter so its
words read for minutes. The window you are using has an amber-white rim
and an amber plate of glass with its title; the others are cool and
plain. `make shot` now takes the gallery a second time in Liquid Glass.

**Measured.** On the M700, dragging About over the wallpaper: 5.6 ns a
pixel and 1.35 ms a frame in glass, against 3.6 ns and 1.07 ms in Plex
Night, the worst pass 2.28 ms against 2.29 - well inside the 8 ms the plan
allowed. And clicking between two windows used to compose the one going
back whole; now only its shadow, buttons, plate and rim are composed - ten
changes of focus 3.48 million pixels where they were 4.93.

**Found on the way.** The window in front was taken to be the last in the
list, which a notification's banner had become - so the window being typed
into looked like one behind. The star was drawn under the plate, which
covered the corner it sits in. And a change to the gallery's test machine
gave two machines one network port, which three other suites caught at
once.

## 18 November 2026 - Glass that shows what is really behind it

**In short.** Liquid Glass's floating pieces - the dock, its tips,
notifications' banners and menus - can now show what is truly under them,
blurred, rather than the wallpaper's glass. It is a choice in Appearance,
*Glass shows what is behind*, and off until it is measured on the M700.
The same day the dock's pill and the banners got the bent, lit rim windows
already had, and a glass window stays glass while it is being resized.
0.11.487 to 0.11.491.

**What.** Until now every glass surface read one picture made once, the
wallpaper blurred, which is why glass cost a copy rather than a blur. Live
glass copies what is on the screen under each floating piece from the
frame being composed and blurs that, so a window passing under the dock
shows through it. The rims needed each strip to say where its glass is
inside its picture - a banner's card sits inside a margin for its shadow
- so the edge is bent at the glass and not at the picture's border.

**Why it matters.** It is the difference between glass and a picture of
glass. It is also the first effect that costs something every frame, which
is why it is off: the bar is under a millisecond a pass more on the M700.

**Measured.** With it on, the dock's row over About moved 61.6 levels; over
the bare wallpaper, 0.0 - the check that it really reads what is behind.

## 18 November 2026 - Starlight's night sky

**In short.** Under Starlight the dock, menus and banners are now glass
too, but a glass of Starlight's own: it shades from a violet dusk at the
top of the screen to night at the bottom, with a faint dust of stars in
it, and a thin amber horizon runs along the top of each piece. Its windows
stay solid. 0.11.492.

**What.** The gradient and the star dust are put into the wallpaper's
blurred copy once, when it is made, so they cost nothing a frame; the
horizon is one row of pixels per piece.

**Measured.** Shading the glass for a 1920 by 1080 screen takes 1.3 ms,
once.

## 18 November 2026 - Windows that roll up, and a plate that lights from the left

**In short.** A Starlight window closing now rolls back up into its title
plate in about a tenth of a second, the other half of the unroll it opens
with; and a window coming to the front no longer turns amber at once - the
amber sweeps in from the left. With these Starlight's motion, S4, is
built. 0.11.497 and 0.11.498.

**What.** The window is really closed the moment you ask: its program is
told, its memory given back, and nothing can click or type into it. What
rolls up is a copy of its last picture, shortened a little each frame and
then let go. The sweep draws the plate as it was behind, and the plate in
front copied over it as far as the sweep has reached; only the plate is
redrawn while it runs.

**Measured.** The roll is 120 ms, the sweep 90 ms.

## 18 November 2026 - A look is colours; a desktop is a shape

**In short.** Choosing a look in Appearance now changes colours and
transparency and nothing else. Whether windows have the star, the amber
plate, the cut corner, bevels, room for a title bar and how deep their
shadow is belongs to the desktop - Astra, Starlight or Pewter. 0.11.496
and 0.11.506.

**What.** Diego chose Liquid Glass on Astra and got Starlight's amber plate
and star, because the looks carried those. He said "the theme is colors and
transparency", and then of the last three, "yes move all three to the
desktop". So each `.desktop` file names six keys of style and every look
is worn over them.

**Why it matters.** A desktop keeps its character whatever colours a person
picks, and a look can be added without deciding how windows are built.

## 18 November 2026 - JetSki saves, has a menu, and remembers

**In short.** JetSki, the browser, can save a page (Control-S: one MHTML
file with everything in it, or the HTML alone), has a menu behind its three
dots with every command and its key, and keeps favorites - the star beside
the address - and a history of every page shown, a file a day. Its one
file of nearly three thousand lines is now six. 0.11.500 to 0.11.504.

**What.** The Save window is the system's own, opening at Downloads with
the page's name. Favorites are files in `/Home/Favorites`, which Tracker
shows as it shows any file; history is a file a day in JetSki's settings
folder, thirty days kept. Both are the libraries the old browser had, which
had had nobody using them since it went. The menu's rows and the keys are
one function each, so a row can never do something its key does not; Print,
Page source and History's list are there and greyed until each is built.

**Why.** The browser is most of a working day, and these are the basics a
person reaches for without thinking. The split was forced: the main file
had used up all the room Lua gives one chunk, and the star only fitted by
squeezing names together.

**Measured.** A saved page of the IDE tutorial: 4,793 bytes as MHTML,
3,974 as HTML. JetSki's main file went from 2,938 lines to 2,164.

## 18 November 2026 - Tests that fail only when the machine is busy

**In short.** A run of the whole test gate kept failing one suite at a time
- a film's sound, a tone's pitch, a drag, a profiler's count, a program
dying in `malloc` - each passing when run alone. Most were the tests, not
Kosmos, and were fixed or explained; one is now instrumented to say exactly
why the next time. 0.11.493 to 0.11.505.

**What.** The film test's program was written to a temporary folder, and
only a program the system image carries may ask for the audio band, so its
sound ran below the desktop and stalled under load; it now plays through
`listen`, a new program anyone can use, which is in the band. The
profiler's test counted ticks a busy Mac never delivered. The tone was QEMU's
sound model slowed by the host, the drag a test reading the first of many
queued moves. The `malloc` death came and went on identical binaries, so
the kernel now records which of three reasons it refused a page for.

**Why it matters.** A test that fails for no reason teaches people to ignore
failures. Each one either fixed at its cause or made to say its cause is
the only way the gate stays worth reading.

## 18 November 2026 - A dock button for something that is gone

**In short.** Diego could not open the browser on the M700. JetSki was fine;
he was pressing an old dock button for the browser removed in 0.11.461,
which wrote an error to the log and showed nothing. Now such a button is
drawn faded, its tip says the program is not installed, and pressing it
says the same. 0.11.507.

**Why it matters.** Nothing on the desktop should fail silently. The dock
checks on its own clock, never while drawing or on a press, and keeps the
button, since installing the program again brings it back.

## 19 November 2026 - The review before 0.12.0, first half

**In short.** Before the next minor version, the whole tree was read by
eight helpers in parallel, one an area, and they reported 205 findings
without changing anything. The first four batches are done: six real bugs
fixed, about two thousand lines the old browser left behind removed, dead
code elsewhere removed, and second copies of the same helper merged into
one. Diego took all seven decisions left to him. 0.11.508 to 0.11.515.

**What.** The bugs: any program could take away another's name in
`/Running`, and it kept a capability slot for every request it did not
store; what an interrupt wakes was written three times and had drifted, so
on ARM a key could wait for the next tick; Telnet could split a command
over two reads. Diego's decisions: Paint moves to Demos until it is
redesigned, the web kit's own HTML layout goes (Mail draws with NetSurf),
one release image for every screen size, and the kernel's limit of eight
processors goes - the Mac Pro has twenty-four, and the kernel now asks the
machine how many it has and allocates for those. The merging changed two
outputs on purpose: UTF-8 is read by one rule everywhere, and a PNG is
written through the Compress Kit.

**Why.** Complexity accumulates by addition; nothing removes code when the
idea behind it changes. `CLAUDE.md` asks for this review before every
minor version, and there had not been one since 0.11.0.

**Measured.** About 3,800 lines left the tree with Diego's decisions and
about 580 with the merging; a PNG is 19% smaller and written 29% faster.
The gate had grown to 12:41 against a ten-minute budget, because it ran ten
suites at once on ten cores while each suite mostly waits; twelve at once
brought it to 9:48 with every suite, and 8:21 on a normal day.

**Next.** Batches 5 to 8 - comments and documents, work nobody reads,
things to measure before changing, tests nothing runs - then 0.12.0, and
then a 3D API, in QEMU first.

## 19 November 2026 - The review before 0.12.0, second half

**In short.** The review before 0.12.0 is finished. Its second half made
the documents say what the system is, took out work done repeatedly for
nothing, measured eight things that might be slow and fixed the two that
were, and put four forgotten tests back to work. A limit of 24 running
applications went too. 0.11.516 to 0.12.0.

**What.** `state.md`, which every session reads first, had grown to
thirteen thousand lines of every session since September, and is now a
hundred, with the rest kept in a file of its own; `CLAUDE.md` and the README
were brought up to date, and comments in about fifty files. The window
manager stopped keeping a full-size picture for every window that already
supplies its own - most of them - and the Deskbar stopped building a string
twelve times a second only to compare it. The check that the system image
is intact, run four times every boot, now reads eight bytes at a time.
Creating a thread in a program that already has thousands no longer slows
with the square of their number. A layout checker that three documents
claimed was in the gate, and was not, now is - and on its first run it
found its own rule too narrow for the Bluetooth driver. Eleven suites that
ran only on ARM run on x86 too, and one of them had silently gone stale.

**Why.** `CLAUDE.md` asks for a review before every minor version,
because nothing else removes what has stopped being needed. "Measure
first" was the rule for anything that looked slow: six of the eight
candidates turned out not to be worth changing, or could not be measured
on this Mac at all.

**Measured.** A boot under emulation reaches its prompt in 1.345 s where
it took 1.567, because the image check went from 178 ms to 36 ms of
hashing. At 2,000 threads, making one more took 3.6 ms with interrupts
off, and now takes 0.014 ms. The scan for sleeping threads costs 0.2% of
one core at 2,000 threads and was left alone. The display tests' pixel
loops turned out to take under a second of a two-minute suite, which is
nearly all waiting.

**Next.** The 3D API, G1 to G5 in QEMU, then the daily-driver list in its
order: keys, the clipboard, JetSki's find, print and history.

## 19 November 2026 - LLVM runs inside Kosmos

**In short.** The first step towards 3D: LLVM, the compiler the software
Vulkan driver uses to turn shaders into processor code, now builds for
Kosmos and runs inside it. A test hands it two small functions while the
system runs, calls the code it produced, and checks the answers. 0.12.1.

**What.** LLVM 23.1.2's release is fetched and checked like WebKit's, and
built by its own build system with Kosmos's compiler and C++ library, for
both of Kosmos's processors. Seven patches, each a file anybody can read,
teach it about a system with no signals, no child processes, no users and
no locale-aware streams. The C library gained a handful of maths functions
from musl to go with it.

**Why.** Vulkan in QEMU, before any GPU, means `lavapipe`, and `lavapipe`
is LLVM underneath: without a working LLVM there is no software Vulkan at
all. It was also the newest kind of program here - one that writes
processor code and then runs it - and the question was whether Nebula's
rule, that a page is never writable and executable at once, would get in
the way. It does not: LLVM writes a function, then flips its pages to
executable before running it, which is exactly what the rule allows. The
design had assumed it would need JavaScriptCore's trick of mapping the same
memory twice, and that was wrong.

**Measured.** Building LLVM from nothing takes three minutes on this Mac,
and nothing at all when nothing changed. The test program is 84 MB, nearly
all of it LLVM, and runs in 0.8 s. Refusing executable pages makes it fail,
which is how the test is known to be testing something.

**Next.** Mesa's half of the same step: its utilities, its shader language
NIR and its Vulkan runtime, built against Kosmos's POSIX layer.

## 19 November 2026 - Mesa's shader compiler runs inside Kosmos

**In short.** The second half of the first step towards 3D: Mesa, the
graphics library whose software Vulkan driver comes next, now builds for
Kosmos, and its shader compiler runs inside it. A test has it build a tiny
shader, simplify it to a single number, print it and pack it up and unpack
it again unchanged. 0.12.2.

**What.** Mesa 26.2.5's release, fetched and checked, is built by its own
build system, Meson, told that there is a system called Kosmos. Two patches
teach Mesa about it: a POSIX-like system with no dynamic loading and no
signals. Kosmos's C library and POSIX layer gained the dozen ordinary
functions Mesa uses - sorting with a context, a stream that writes into
memory, a barrier for threads and the like.

**Why.** Meson finds out what a system has by compiling and linking tiny
test programs, and a Kosmos program cannot be linked on its own like that.
The first attempt told it to ignore what was missing, and every question
came back yes - which looks like success and is the opposite. So a small
linker wrapper links the test, then checks itself that everything the
test calls really exists in Kosmos's libraries, and fails by name when it
does not. Getting that honest is what made the rest of the port honest.

**Measured.** The test program is 38 MB and runs in 0.2 s. Breaking the
memory stream makes it fail at exactly that step, which is how the test is
known to be testing something.

**Next.** G2: `lavapipe` itself, Mesa's Vulkan driver on LLVM, drawing a
triangle and a cube into a picture that is checked against a reference.

## 19 November 2026 - Vulkan draws a triangle on Kosmos

**In short.** Kosmos has Vulkan, the modern 3D API, for the first time. A
test program draws a triangle with Mesa's software Vulkan driver and
reads the picture back, and every pixel is where it should be. 0.12.3.

**What.** `lavapipe` is a Vulkan driver that draws with the processor
rather than a graphics card, compiling each shader with LLVM while the
program runs. It is now built into a test image. There is no Vulkan
loader - the piece that normally opens a driver from a file - because
nothing on Kosmos is loaded from a file at run time; the driver is linked
in and called directly.

**Why it took the turns it did.** Two failures said something about the
system. One part of Mesa writes processor code into memory that is
writable and executable at the same time. Nebula never allows that - a
page is one or the other - so that path is simply not used on Kosmos, and
LLVM, which writes first and then switches the page over, does the job.
And the first crash said nothing at all, because a Lua program's last
words had nowhere to go; the kit now hands them the console, as the web
kit already did.

**Measured.** The test image is 109 MB, almost all LLVM, and draws in
1.5 s inside QEMU. Moving one corner by a pixel makes 7 pixels wrong, and
refusing executable pages stops the shader compiler - both fail the test,
which is how it is known to be looking.

**Next.** A textured cube held to a reference picture, and a
minute of Khronos's own conformance tests.

## 19 November 2026 - Vulkan draws a cube on Kosmos

**In short.** Vulkan now draws something three-dimensional on Kosmos: a
cube, turned in space, its faces checkerboards taken from a texture, the
near faces hiding the far ones. Every pixel is checked. 0.12.4.

**What.** The triangle was a flat shape drawn in pixels. The cube uses
what real 3D programs use: a list of corners in memory, a matrix that
turns and places them and gives them perspective, a picture (a texture)
painted onto each face, and a depth buffer, which remembers how far away
each pixel is so that the back of the cube cannot paint over the front.

**How it is checked.** The test chooses the angle, the distance, the lens
and the two colours, and then works out the answer by itself: for every
pixel it follows a line from the eye through that pixel and asks which
face of the cube it meets first, and which square of the checkerboard.
Where a pixel sits exactly on an edge, either side is accepted; nowhere
else is.

**Measured.** 1430 pixels of cube on three faces, all correct, in about
two seconds inside QEMU. Turning the depth buffer off makes 182 pixels
wrong; painting one face's checks backwards makes 282 wrong. A first
attempt at that second check flipped a face the camera cannot see, and
the test rightly passed - a reminder that a control has to be aimed at
something the test is looking at.

**Next.** A minute of Khronos's own Vulkan conformance tests.

## 19 November 2026 - C++ learns to print

**In short.** Kosmos's C++ library can now format and parse text and read
and write files the standard C++ way, with "streams". It was left out a
year ago because nothing needed it; Khronos's Vulkan conformance tests do.

**What.** C++ programs usually build their messages with streams - `out
<< "found " << n << " errors"` - and those streams are tied to the idea
of a locale: the language and country whose way of writing numbers and
dates a program follows. Kosmos has one locale, plain "C", and the C++
library had been built with the whole locale machinery switched off.

**What it took.** Telling the C++ library that Kosmos is a system with one
locale, as LLVM's own C library is - a small patch, kept beside the
untouched original as the other ports' patches are - and filling in the
parts of Kosmos's C library that a locale-aware program expects to exist,
each giving the "C" answer.

**Measured.** A test formats and parses numbers through streams and
writes a file and reads it back. Pointing the file at a folder that does
not exist makes the test fail, so it is known to be looking.

**Next.** The Vulkan conformance tests themselves.

## 19 November 2026 - Khronos's Vulkan tests pass on Kosmos

**In short.** Kosmos now runs the official test suite that every Vulkan
driver in the world is held to, and a minute of it - 3565 tests - passes
with none failing, every time the gate runs. The 3D API's second step is
done. 0.12.6.

**What.** Khronos, the group behind Vulkan, publishes a conformance suite:
tens of thousands of small programs, each asking a driver one precise
question and judging its answer. A driver that wants to call itself
Vulkan has to pass it. Kosmos now builds that suite as an application of
its own and runs two of its groups - what the driver says about itself,
and the API's rules - inside QEMU.

**What it found.** On Linux there is a small program between an
application and the driver, the Vulkan "loader", and it answers some
questions before the driver ever sees them: "is there a debugging layer
called X?" (no), "destroy this device" when the device is nothing (do
nothing). Kosmos has no loader, so those questions reached the driver,
which crashed on one and answered another wrongly. The driver now does
the loader's part. It also found two small mistakes in the driver itself.

And three things about Kosmos, written into the roadmap: a program's
stack has a fixed size, smaller than Linux's, so the suite runs on a
thread of its own; the software renderer draws wrongly on processors
older than any Kosmos runs on; and when memory runs out, a program is
stopped rather than told - so the tests that use memory up on purpose are
left out for now.

**Measured.** 3565 tests in 41 seconds: 1439 pass, 2126 ask for things the
software driver does not offer and say so, none fail. Running the same
minute on an old processor makes two tests fail, and the run names them -
so it is known to be looking.

**Next.** A window and a spinning cube on the screen, by Vulkan.

## 19 November 2026 - A Vulkan cube in a window

**In short.** A program drawing with Vulkan now puts its picture in an
ordinary Kosmos window on the desktop, and the gate checks every pixel of
it on the screen. The 3D API's third step. 0.12.7.

**What.** Until today Vulkan on Kosmos drew into memory, and a test read
the memory back. A real program wants a window, and Vulkan reaches a
window through a "surface", which each operating system supplies for
itself - Windows has one, Wayland has one, and now Kosmos has
`VK_KOSMOS_surface`. A program opens a window, hands it to Vulkan, and
gets a short ring of images: it draws into one while the window shows the
last, and "presents" each when it is done.

**Why it matters.** This is the shape every Vulkan program on Kosmos -
games, Cafesa3D one day, the flight simulator - will use, and it is the
same on the software renderer today and on the M700's GPU after G7.

**What went wrong on the way.** Mesa keeps a small table with one slot per
window system, sized to the last one it knew. Kosmos's came after it, so
writing Kosmos's slot spilled into the next thing in memory, and the
program crashed as it closed. It was found by running the old test with
the new Mesa: it crashed too, so the fault was Mesa's table and not the
new test.

**Measured.** 30 frames in 0.6 seconds under emulation, which says nothing
about speed; and the 3565 conformance tests still pass with none failing.

**Next.** `intelgpu` for many programs at once (G6), then Vulkan on the
M700's GPU (G7).

## 19 November 2026 - Two programs' memory on one GPU

**In short.** The M700's graphics chip can now keep two pieces of work in
memory of their own: each believes it writes to the same address, and each
lands on its own page. That is what lets many programs share the GPU
without seeing each other's pictures. 0.12.8.

**What.** Until now the GPU ran Kosmos's work through one queue, with one
view of memory shared by everything. Modern drivers - Linux's, and the
Vulkan driver Kosmos will run - give each program a "context": its own
queue, and its own page table translating the addresses it uses into real
memory, exactly as the processor does for processes. Today the driver
built both, the way Intel's Linux driver does, and ran them on the chip.

**How it was made safe.** A wrong bit in this kind of structure does not
give an error: the engine simply stops. So everything that could be
checked without the chip was checked on the Mac first - 2185 checks
against the numbers in Linux's own source. And the first engine moved is
the video engine, which the desktop never uses, so if it had stopped,
nothing anyone was looking at would have stopped with it.

**Measured.** One context: 300 writes of 300, 11 microseconds each. Two
contexts at once, the same address in each: every time, each page held
only its own number. Given one page table between them on purpose, the
check saw at once that one write had landed on the other's page.

**Next.** The render engine and the blitter - the ones that draw the
desktop - move onto contexts too, with a timer that notices a stuck engine
and a reset that frees it.

## 19 November 2026 - The desktop's drawing in contexts, and a stuck GPU freed

**In short.** The parts of the M700's graphics chip that draw the desktop
now run their work the way modern drivers do, each in a context with its
own view of memory, and the driver can notice when one of them gets stuck
and reset it without restarting anything. 0.12.9.

**What.** Yesterday's step tried contexts on the video engine, which
nothing on the desktop uses. Today the two that matter moved: the render
engine, which composes windows and runs 3D and compute, and the blitter,
which copies pictures. Their memory goes through a page table that mirrors
the one the driver already kept, entry for entry, so every address it
hands them still means the same page. Nothing that draws had to change.

**The stuck engine.** A GPU can be given work that never ends - a bug in a
shader, or a program's mistake - and until now that would have frozen the
screen until a restart. Now the driver watches: if an engine has work and
has finished nothing for three seconds, it is stopped and reset the way
Intel's Linux driver does it, the unfinished work is dropped, and the
engine carries on with whatever comes next. It was tried on purpose, on
both engines, with a command that loops for ever: each was freed after 3.0
seconds and wrote its next number at once. With the reset itself taken out,
the engine stayed stuck - which is how we know it was the reset that freed
it.

**Measured.** Every GPU check that passed before passes on contexts: the
copies exact to the pixel, the 3D picture 4096 pixels of 4096, the compute
fill, and the desktop with the spinning cube at about 120 frames a second.
The price: handing the GPU a piece of work now takes 11 to 14 millionths
of a second instead of 3 to 4, because the engine loads a context each
time. A frame lasts 16 thousandths, so nobody can feel it, and there is a
known way to take it back if a frame ever needs it.

**Next.** Contexts for programs rather than only the driver's own - the
protocol that will let a Vulkan program on Kosmos send the GPU its own work.

## 19 November 2026 - Programs given the GPU, each in a room of its own

**In short.** A program on Kosmos can now send the GPU work of its own, in
a context that is only its own: memory it placed where it chose, commands
run in order, and a note back when they are done. Two programs at once,
using the same addresses, never saw each other's memory or registers. A
program whose work loops for ever is stopped after three seconds and can
carry on. And on a machine with no Intel GPU - the emulator the tests run
in - the same requests are answered by a stand-in, so the tests hold all
of it. 0.12.10.

**What.** This is the door Mesa's Vulkan driver for Intel will use, next.
A program asks for a context, hands the driver pieces of its memory with
the address the GPU should see each at, and gives it batches of commands
to run in that context. Each batch says a number the context's timeline
reaches when it is done. The program reads that number from a page the
driver handed it read-only, or asks to be woken when it comes. Nothing
here waits on the GPU: every request is answered at once.

**Why a stand-in.** The emulator has no Intel GPU, and a protocol only one
machine can run is a protocol the tests never see. So the bookkeeping -
whose context, which memory where, what may run - is written without
touching hardware, and an "engine" is something it hands work to. On the
M700 that is the real engines. In the emulator it is a few hundred lines
that run the simple commands - store a number, set a register, read it
back - on the processor, through the same page tables.

**Two things the M700 taught.**
- **The cost.** At first every context was saved and restored whole each
  time it ran. On the render engine that cost 88 millionths of a second
  per piece of work, seven times what it had been. Now a context is
  restored only when another one ran in between. If it was the last to
  run, the engine still holds its state, and the cost is back to 18.
- **The leak.** A new context took over whatever the previous one left
  in the engine's registers. The test showed it: the second program read
  the first one's number. Now each engine's empty state is recorded once
  when it starts, and every new context begins from it. With that turned
  off on purpose, the leak came back - which is how we know it is what
  closed it.

**Measured.** Two contexts, one address, each its own page and register,
four wake-ups of four, on the M700 and in the emulator. A looping batch is
stopped after 3,010 ms, and the context's next store lands. Every earlier
GPU check still passes. A wake-up can be up to 4 ms late, because the
driver looks at the GPU once a tick. The GPU's own interrupt would make
that microseconds, and is on the list.

**Next.** G7: Mesa's Intel Vulkan driver, `anv`, speaking this protocol on
the M700 - `vkcube` on the real GPU.

## 19 November 2026 - Intel's Vulkan driver meets the M700

**In short.** Mesa's Vulkan driver for Intel graphics, the one Linux
uses, now builds for Kosmos and finds the M700's GPU: "Intel(R) HD
Graphics 530", Vulkan 1.4, six gigabytes it can use. Its shader compiler
runs inside a Kosmos program and turns a small compute shader into twenty
of the GPU's own instructions. Nothing is drawn with it yet; that is the
next three steps. 0.12.11.

**What.** On Linux the driver finds a GPU by opening a device file and
talks to the kernel through `ioctl`, a call that Kosmos has on purpose
never had. Mesa already supports two Linux kernel drivers for Intel, i915
and Xe, and picks between them in a few dozen places. Kosmos becomes a
third choice. It finds the GPU by asking the namespace for
`/Devices/gpu`, and does the kernel driver's part with Kosmos's own
operations: a buffer is a shared region, and "is this work done?" is a
number on a page, or a pulse.

**Why it was done this way.** Removing i915's and Xe's code would have
meant editing every file that mentions them. Instead it stays in the
build, compiled against an `ioctl` that refuses, and is never called.
What Kosmos changes is 28 of Mesa's files, each change one choice made
by the new type, plus about 450 lines of its own.

**Something the compiler taught.** It crashed on its first run. All of a
compiler's types live in one shared table, and a Vulkan driver builds
that table when a program starts using it. Calling the compiler directly,
as the test does, skipped that step, so the table was never made.

**Measured.** On the M700 the device is listed with its real name, PCI
id, Vulkan version and memory. In the emulator, which has no Intel GPU,
no device is listed and there is no error, which is the right answer for
a machine without one. The compute shader becomes 20 Gen9 instructions
(288 bytes, no spills) on both. Told to compile for a newer Intel GPU
instead, the test fails as it should, so the check really looks at which
GPU it compiled for.

## 20 November 2026 - The M700's GPU does Vulkan's work

**In short.** For the first time a Vulkan program on Kosmos has had real
work done by the M700's graphics chip. It filled half a buffer with a
fixed value and had a small compute shader write the other half, and
every one of the 16,384 numbers came back right. Making the device takes
four milliseconds, and each piece of work about four to six and a half.
0.12.12.

**What.** Yesterday the driver could see the GPU but could not use it.
Using it takes three things. The driver's memory has to be placed in
the GPU's own address space: each buffer is a Kosmos shared region,
bound where the driver asks. Its work has to be handed over: a list of
commands, run in the program's own "room" on the GPU, made in G6. And it
has to know when the work is done: a number on a page the GPU writes,
or a pulse that wakes it.

**Something that hid itself.** The first runs died with no message.
Getting a message out showed the C library itself had crashed while
formatting one. Mesa had written `%m`, a Linux shorthand for "the reason
the last call failed", which Kosmos's formatter did not know. So the
error report crashed instead of reporting the error. With `%m` added,
the real error appeared: the driver hands out small buffers carved from
bigger ones, and maps one by mapping its big buffer at an offset, which
Kosmos had refused. Now a big buffer is mapped once and shared by all
the small ones carved from it.

**Why it was done this way.** The Linux driver gives each program
several queues, each able to use the memory. Kosmos gives a program one
GPU room per device, made as the device is, because the driver places
memory before any queue exists. One queue is enough for now, and the
roadmap says what a second would need.

**Measured.** On the M700: the device in 4.4 ms, the fill in 3-4 ms and
the compute shader in 6.5 ms, each timed from writing the commands to
reading the answer. To prove the check reads real GPU work, every batch
was swapped for an empty one. The GPU still said "done", but the buffer
held nothing, and the check failed as it should. The M700's own test
suite now runs this every time.

**Next.** G7c: the spinning cube, `vkcube`, drawn by the GPU in a window.

## 20 November 2026 - A cube drawn by the M700's GPU, in a window

**In short.** The M700's graphics chip has now drawn a picture through
Vulkan, and it shows in a window on the desktop. The picture is a small
cube in two colours. It matches, pixel for pixel, a reference worked out
from the cube's geometry. 0.12.13.

**What.** A Vulkan program draws into "swapchain" images and hands each
one to the window system to show. Kosmos's window system only accepted
images drawn by the processor. On a GPU, Mesa's driver draws into the
GPU's own image, in its own tiled layout, then has the GPU copy it into
a plain buffer the processor can read. Kosmos now copies that buffer
into the window, but only after the GPU says the copy is finished. Mesa
waits for that only on software devices.

**Something found on the way.** Programs are sent to the M700 over
Telnet. A 58 MB send left Telnet answering nobody. The receiving loop
took each 76-character line off the front of a list, which moves every
line behind it, so 800,000 lines cost hundreds of billions of moves.
QEMU never caught it: its network delivered the lines a few at a time,
so they never piled up. The loop now takes the whole pile in one pass.
A new check makes them pile up in QEMU, by sending 4 MB while the
session is still busy with another program. That check fails on the old
loop and passes on the new one.

**Measured.** 300 frames in 4.5 s, about 66 a second at 64 by 64 pixels.
At that size the number is the round trip, not the GPU's drawing speed.
A full-size window is next on the list for that. Two checks show the
test can fail: a reference made at a slightly different angle disagrees
on 640 pixels, and a screen without the cube finds no window.

**Next.** G7d: the Vulkan conformance tests (dEQP-VK) on the M700, and
what share of them pass.

## 20 November 2026 - Khronos's own tests, on the M700's GPU

**In short.** Khronos, the group that writes the Vulkan standard, ships
a suite of tests every Vulkan driver is held to. Kosmos ran the same
3,565 of them on the M700's graphics chip that it already runs on the
software renderer. None failed. Running them found one real mistake, now
fixed, and showed why the GPU answers slowly. 0.12.14.

**What.** The tests ask the driver thousands of questions: make this,
refuse that, report what you can do. 1,441 passed. 2,124 were features
this driver honestly says it lacks, mostly "sparse" memory, which Kosmos
does not offer yet. The first run failed three. A program may ask for a
queue at high priority, and a driver that has no high priority must
refuse it. Kosmos had said yes.

**Something found.** A new measurement times how long one tiny piece of
GPU work takes, from handing it over to hearing it is done. At best it
was 0.18 ms, but usually 8 ms, half a frame. The GPU finishes almost at
once. Kosmos's GPU driver only notices when it next looks, and it looks
on the system's clock tick, every 4 ms. The GPU can raise an interrupt
the moment it writes "done", and the driver has never listened for it.
That is next.

**Next.** G7e: the GPU's interrupt, and the same measurement again.

## 20 November 2026 - The GPU says when it is done

**In short.** Kosmos's GPU driver now listens for the M700's graphics
chip to say "done", instead of checking every 4 ms. A small piece of GPU
work used to take 8 ms to be heard as finished. Now it takes a fifth of
a millisecond. The spinning cube's window went from 73 frames a second
to more than 1,500. 0.12.15.

**What.** Hardware can interrupt the processor to say something
happened. Kosmos's disk, network and USB drivers already listened for
theirs. The GPU's driver never had: the layer that hands drivers their
devices gave the GPU no interrupt at all. Now it hands one over, and the
driver switches the GPU's "work done" signals on.

**Something that hid.** With every main switch on, nothing came. Each of
the GPU's engines has a small mask of its own as well, and it was still
closed. Linux's driver writes it, and once Kosmos did the same, the
signals arrived at the first piece of work.

**Measured.** From handing the GPU work to hearing it was done: 8 ms
before, 0.22 ms now, and the worst of a hundred tries 0.26 ms where it
was 13.5. Khronos's tests all still pass, and a little faster.

**Next.** G7 is finished. The next step is OpenGL and WebGL on the
M700's GPU (G8), or the flight simulator once Diego has agreed its
design.

## 20 November 2026 - The GPU was running slow for Vulkan

**In short.** The M700's graphics chip was drawing Vulkan's pictures at
its slowest speed, about a tenth of its fastest. The driver only asked
for full speed when it ran Kosmos's own compute work, and Vulkan's work
comes another way. It now asks whenever the chip starts. A spinning cube
in a window a thousand pixels across went from 72 frames a second to
388. 0.12.17.

**How it was found.** The cube was fast in a small window and slow in a
large one, so each frame was timed in three parts: waiting for the chip,
copying the picture into the window, and handing the window over.
Waiting for the chip was ten of the fourteen milliseconds, at the same
cost for every pixel at every size. That pointed at the chip's speed, not
at memory. The driver's log had no line saying it had asked for its
clock, which it says every time it does.

**Visible impact.** Anything drawn with Vulkan on the M700 is about five
times faster at large sizes. The chip still goes to sleep when there is
nothing to draw.

**Next.** At large sizes, copying the finished picture into the window
now takes almost as long as drawing it. Drawing straight into the window
removes that copy.

## 20 November 2026 - The tests back under ten minutes

**In short.** Before running its tests, the gate builds every system image
they use. That step had grown to over two minutes, mostly by redoing work
for nothing. A small file saying which ad-block lists one image was built
with was rewritten by every build, including builds of other images. So
the 10 MB table made from those lists was compiled again every time.
Fixing that, and building some images side by side, took the step from
137 seconds to 43. The whole gate is 9 minutes 8 seconds, inside Diego's
ten. 0.12.18.

## 20 November 2026 - Flight agreed, and the tile server learns to fetch from several places

**In short.** Diego agreed the flight simulator's design ("Yes to all")
and asked for gamepad support too, now step F7. The first step, F1, is
built. The server that fetches map tiles from the internet could follow
only one source at a time. Flight needs two at once: the satellite
photographs of a city and its elevation. Now it keeps as many as are
asked for, each fetching on its own, so a slow photo server does not hold
up the elevation. The protocol's code moved out of Maps into one shared
library, so Flight will not need a copy of it. 0.12.19.

- **What a person sees**: nothing yet in Maps, which works as before. At
  the prompt, `open tiles fetch png URL 12/1200/1500` fetches tiles and
  says where each is kept, which is useful when a map shows grey.
- **How it is proved**: a test serves elevation and slowed-down satellite
  tiles at once. All the elevation arrives while the satellite tiles are
  still coming, and every tile decodes from the cache. Changed to one
  queue, the test catches it.
- **What was learned**: only programs carried in the system image are
  handed the services they declare, and only by a starter that holds
  them. The test first tried a program on the disk, which is correctly
  refused.

## 20 November 2026 - The dock that vanished

**In short.** Diego noticed that moving the pointer off the dock made it
disappear, and that it came back only when the pointer came close again.
The magnifying lens over the dock thought it was still on screen after it
had shrunk away, so the dock drew itself without its icons. The repaint
that should have put them back was asked for by a flag the UI kit never
looked at. Both are fixed, and the kit now honours that flag, which
eighteen applications had been setting to no effect. 0.12.19.

- **What a person sees**: the dock stays put when the pointer leaves it.
  GPU Monitor's Vulkan row says "1.4, Mesa anv" instead of "not yet".
- **How it is proved**: the dock's test now asks the Deskbar itself
  whether its last paint, as the lens closed, drew the icons. With the
  old code put back, that check fails, and only that one.
- **What was learned**: a first check looked at the dock's pixels and
  passed on the broken code too. The Deskbar repaints once a second when
  something wakes it, the test woke it, and the icons were back before
  the picture was taken. A check that reads what a program says it did
  cannot be raced that way.

## 20 November 2026 - Where a place is, and how high

**In short.** The second step of the flight simulator, F2, is built. A map
kit now knows which tile of the world covers a place, how many metres a
pixel of it is, and how high the ground is, read from the elevation tiles
the tile server fetches. Maps had been doing the first two itself; it now
asks the kit, so Flight will not need its own copy. 0.12.20.

- **What a person sees**: nothing new yet. Maps' scale bar comes from the
  kit now and reads the same.
- **How it is proved**: on the Mac, against places and heights worked out
  by hand, Berlin and Montevideo among them; in Kosmos, every elevation
  tile the test serves is read through the kit, and must give back the
  heights that were written into it, to the metre. Moving the point a
  height is read at by half a pixel makes both fail.

## 20 November 2026 - One door to Vulkan

**In short.** The third step of the flight simulator, F3, draws Flight's
world with the GPU, and its first part is built. Every C program that
draws with Vulkan starts with the same few hundred lines: open the
device, make buffers and images, record and submit work, build a
pipeline, and put a picture in a window. They were the spinning cube's
own; they are now a kit of their own, `kvk.c`, which the cube and the
renderer that comes next both stand on. 0.12.21.

- **What a person sees**: nothing new. The cube draws exactly as it did.
- **How it is proved**: the cube's suites, which compare its picture pixel
  by pixel, pass on the moved code. Turning the depth test the wrong way
  round inside the kit makes 1389 of their pixels wrong.
- **What it took away**: the cube's kit went from 1376 lines to 850.

## 20 November 2026 - A picture checked by a ray

**In short.** The renderer the flight simulator's world will be drawn by
is built: F3b. It draws a sky, then shapes in front of one another, lit by
the sun and fading into haze with distance, using Vulkan. 0.12.22.

- **What a person sees**: nothing yet - it draws into memory for its test.
  F3c puts it in a window, and on the M700's own GPU.
- **How it is proved**: the test chooses a small scene - a ground and two
  boxes - and works out on the Mac, by casting a ray through every pixel,
  what colour each must be. Kosmos draws it and every pixel is compared;
  all 3977 that are not on an edge match. Leaving out the haze, the
  sunlight, or drawing the sky the wrong way makes between 1059 and 2417
  pixels wrong.

## 20 November 2026 - The picture in a window

**In short.** The flight simulator's renderer now draws into a window on
the desktop, not just into memory: the first half of F3c. 0.12.23.

- **What a person sees**: a small window with a sky, green ground and two
  boxes, the camera turning round them for a second and stopping.
- **How it is proved**: the test takes a picture of QEMU's screen, finds
  the window in it, and checks every pixel against the colour it worked
  out on the Mac. All 8966 match. Stopping the camera one step later, or
  never showing a frame, makes it fail.
- **Next**: the same scene on the M700's own Intel GPU.

## 20 November 2026 - The ground in pieces

**In short.** The flight simulator's ground can now be cut into pieces
and each piece built as a mesh: F4a. Near the aircraft the pieces are
small and detailed, far away large and coarse, and they always fit
together with no gap. 0.12.24.

- **What a person sees**: nothing yet; F4b draws it with satellite images.
- **How it is proved**: on the Mac, against a flat slope and a round hill
  whose every height is known, and by checking that the chosen pieces
  cover the ground exactly once - whether there is room for hundreds or
  only twenty-five. Breaking the numbering, the triangles' direction, or
  how an edge is lit each makes it fail.

## 20 November 2026 - Ground you can see

**In short.** The flight simulator's ground is drawn: F4b. Each piece of
it is painted with its own picture, blended smoothly between the
picture's dots, lit by the sun and fading into haze. 0.12.25.

- **What a person sees**: still a test picture - 2.5 km up over Montevideo,
  looking towards the horizon, the ground made of 262 pieces.
- **How it is proved**: the pictures are made from a rule the test knows,
  so for every pixel it works out what must be there - which piece, which
  spot of its picture, how lit - and checks. All 8320 that are not on an
  edge match. Painting a piece with its neighbour's picture, not blending,
  or turning the picture sideways each makes it fail.
- **Found on the way**: the first test could not tell blending from not
  blending, because its pictures changed too gently. A pattern was added so
  it can.

## 20 November 2026 - A path over a city

**In short.** The flight simulator can now be written in Lua: F4c. A
program opens a window, asks the ground which map tiles it needs, hands
them over, and draws a frame - the way Flight will, every frame. 0.12.26.

- **What a person sees**: still a test - a small window flying north over
  Montevideo at 2.5 km, sixteen frames, the ground in a colour per detail
  level.
- **How it is proved**: the test knows what the ground and the pictures
  are, so for the last frame it works out every pixel and finds the
  window on the screen that matches. All 8229 that are not on an edge do.
  Uploading the pictures in the wrong byte order, ignoring the heights,
  reading heights from the wrong tile, or mixing up the pieces each makes
  it fail.
- **What it costs today**: under emulation a frame takes about 1.7 s,
  most of it building new pieces as the eye moves. F4d moves that work
  off the frame, which is the next step.

## 20 November 2026 - Building the ground on the side

**In short.** The pieces of ground are now made on a helper thread, so a
frame does not wait for them: F4d. While a piece is being made, a coarser
one already made stands in for it. And pieces left behind are thrown
away. 0.12.27.

- **What a person will feel**: flying over new ground no longer makes the
  picture stall while it is prepared - it starts a little blurry and
  sharpens. Under emulation, frames got 2.4 times quicker.
- **How it is proved**: the same flight as before, then checking that the
  stand-ins happened, that pieces were given back, and that once all is
  built every pixel is exactly right. Turning off the stand-ins, the
  giving back, or giving a piece its neighbour's picture each makes it fail.
- **And the aeroplane**: Diego chose a Cessna 172 from Sketchfab, by
  Viva_1, free with credit. It has moving parts - the flaps, the ailerons,
  the rudder, the elevator and the propeller - so they can move with the
  controls. It is in the tree, with its licence beside it, for F5.

## 20 November 2026 - Flight flies

**In short.** Flight is playable: a Cessna 172 over a real city's tiles,
flown with the arrow keys. 0.12.28.

- **What a person sees**: a window with the sky and the ground, and the
  Cessna in front of the camera, its wings, tail and propeller textured.
  The arrows bank and pitch it, A and Z the throttle, F the flaps, V the
  view. The ground is the real place's satellite photograph over its real
  hills, fetched as you fly.
- **How it is proved**: the test boots the desktop, opens Flight, serves
  it hills and pictures from the Mac, and presses the keys through the
  virtual machine's own keyboard - and checks that the aircraft rolled,
  turned, rolled back, climbed, and changed view.
- **Found on the way**: applications installed on the disk were never
  given the permissions their own header asks for; now they are, for the
  ones any program may ask.
- **How it was built**: four helpers worked beside the main session at
  once - the renderer's textured models, the aircraft file's reader, the
  picture decoders made usable from any thread, and the slowest test split
  in two - and the main session folded each in.

## 20 November 2026 - A driver for the Mac Pro's graphics cards

**In short.** The Mac Pro's two AMD FirePro cards have a driver of their
own, written to be tried on the machine tonight. 0.12.29.

- **What it does**: at boot it finds both cards and says which they are -
  a D300, D500 or D700 - how much memory each has, how many compute units,
  and how the firmware left them. It reads back the screen the firmware lit
  and checks it is the same one Kosmos draws on, and it gives each card's
  copy engines their first work: fill a block of the card's memory, copy
  it, and say how long that took. Typed `gpu cursor 5`, it shows colour
  bars for five seconds by switching the screen to a picture of its own,
  and back.
- **Why it matters**: until now the cards were only a screen the firmware
  left behind. This is the same first road the M700's Intel GPU took
  before the desktop was composed on it.
- **What is not known yet**: whether any of it is right on the card. QEMU
  has no AMD GPU, so what is tested here is the arithmetic - 56 checks
  against Linux's own register files - and the Mac Pro's boot is the test
  of the rest. Every line it prints begins `amdgpu:`, and anything that
  surprises it is said in capitals and stops it touching that card.
- **How it was built**: a helper wrote the driver beside the Flight work,
  touching none of the kernel; the main session added what the kernel
  needed to find the cards and folded it in.

## 20 November 2026 - The Mac Pro gets its network, and its GPUs' engines wake

**In short.** The Mac Pro has an Ethernet driver now, and the AMD driver
reaches the cards' command processors - the first steps towards full 3D
and Vulkan on them. 0.12.30.

- **Why the network first**: every step of the graphics work has to be
  tried on the Mac Pro itself, and without the network each try was a USB
  stick written and booted. With it, the Mac Pro can be reached over
  Telnet and its screen watched, as the M700's was while its GPU was
  brought up.
- **What the graphics driver can do now**: hear the card's interrupts,
  give the card a map of memory it can reach and separate maps for
  separate programs, and start the card's command processor with AMD's own
  firmware and give it its first commands. That is the engine that draws 3D.
- **What comes next**: the 3D engine's setup, programs' work on it, and
  then Mesa's AMD Vulkan driver on top - the same road the M700's Intel GPU
  took.
- **What is known**: the arithmetic of both drivers, checked against
  Linux's own files on this Mac - 82 checks for the network, 279 for the
  graphics engines. **What is not**: anything on the real chips. The Mac
  Pro's boot is the test.

## 20 November 2026 - Flight's instruments

**In short.** Flight's chase view now shows a strip of instruments along
its bottom: speed, altitude, heading, climb, throttle and flaps. 0.12.31.

- **What it is**: the same row of numbers a pilot glances at, drawn in the
  system's own font on a dark band you can see the ground through.
- **How it is drawn**: the 3D Kit gained *overlays* - a flat picture laid
  over the 3D frame by the graphics card, after the scene. Any program
  drawing 3D can use one now: a game's score, a viewer's caption.
- **Why it costs nothing**: the picture is redrawn and handed to the card
  only when one of its numbers changes, not every frame.
- **How it is checked**: the test reads the words Flight says it drew, and
  looks for the band on the screen - and with the strip switched off, the
  test fails, as it should.

## 20 November 2026 - Flight's cockpit

**In short.** Flight's cockpit view now has the six round instruments
every small aeroplane has. 0.12.32.

- **What they are**: airspeed, the artificial horizon and the altimeter
  on top; the turn coordinator, the heading and the climb rate below -
  the same six, in the same places, that a pilot learns on.
- **How they are drawn**: like the strip in the chase view, as a flat
  picture laid over the 3D frame, and only redrawn when a needle would
  visibly move.
- **How it is checked**: the test switches to the cockpit and looks for
  the panel where it must be on the screen; with the panel switched off,
  the test fails.

## 20 November 2026 - Flight's graphics panel

**In short.** Flight shows what the graphics card does for every frame,
in a panel in its top right corner. 0.12.33.

- **What it shows**: frames a second, a line of the last frames' times,
  how long the graphics device took for a frame and how long the
  processor did, how many draw calls and triangles a frame has, and how
  many pieces of the ground are drawn.
- **Why it matters**: Flight exists to show what a 3D API does, and this
  is where it is read. On the Mac Pro's and the M700's own GPUs it will
  say how fast each really is.
- **What it stands on**: the graphics device now times itself (Vulkan's
  timestamps), and the 3D Kit counts what it asked for - both there for
  any program that draws 3D.
- **How it is checked**: the device's time must fit inside the frame's,
  since a frame waits for the device; a fault that made it a hundred
  times too large fails the test.


## 20 November 2026 - Flight on the M700, and why it was slow

**In short.** Flight flew on the M700, slowly, and the machine looked
idle while it did. The graphics library thought the M700 had one
processor. Told the truth, it draws on all eight: from 7 frames a second
to 11. 0.12.34.

- **What happened**: Flight runs on the processor for now (its software
  renderer, lavapipe), because the M700's own graphics driver hangs the
  card on Flight's first frame. And it ran on one of the eight
  processors only, which is why the machine read an eighth busy.
- **Why**: Mesa asks how many processors there are only on systems it
  recognises, and Kosmos is not one of them, so it assumed one. A
  six-line change asks Kosmos.
- **What is still slow, and why**: part of the work cannot be shared out -
  preparing two million triangles a frame happens on one processor. The
  answer is the graphics card, and fewer triangles far away when there is
  none.
- **Also**: the strip at the bottom once stopped following the flight.
  It has not happened since, and Flight now says in its log what the
  strip shows, so the test holds it to the flight's real speed.

## 20 November 2026 - The M700's own GPU draws Flight

**In short.** Flight now runs on the M700's graphics card instead of its
processor: a frame every 10 ms where it took 85 to 89. What had been
hanging the card was Kosmos's own Intel driver, which left out a list of
registers Linux sets up on this kind of card before anything runs. 0.12.35.

- **What happened**: Mesa's Intel driver, which Flight uses on the M700,
  hung the card on Flight's first frame, so Flight was drawn by the
  processor instead, slowly.
- **Why**: Linux, before a program's work reaches an Intel card of this
  generation, sets a few dozen registers the hardware needs - workarounds
  Intel documents - and grants a program's work permission to write five
  more. Kosmos's driver did none of it. The permission, the "whitelist",
  is what Flight's frames needed: without it the card stopped after the
  sky when the next thing was drawn.
- **How it was found**: by elimination, on the machine. The card's own
  registers say where it stopped; the frame was cut down until one change
  made it stop or not - no buildings, no sky, one setting at a time - and
  each of Linux's lists was put back and taken out again to see which one
  mattered.
- **Visible impact**: Flight flies smoothly on the M700, about a hundred
  frames a second, with the same picture.
- **Also**: the investigation left tools behind - the driver now says
  where the card stood and why when it hangs, and a program can have
  Mesa's Intel driver write out every batch it sends.
