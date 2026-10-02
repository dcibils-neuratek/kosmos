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
