# The Scheduler: a game mode, and a window that explains

Diego, 11 October 2026: "I cant change the scheduler policy any more", "i
want to add a new policy that is 'game mode' that prioritizes single
threaded apps like games. You need to investigate what is the best
scheduler for this", "the app is very raw. it should be redesigned to
better explain what it does, how changing the scheduler affects performance
and responsiveness, and else in a visual way", "mockup first".

This is the investigation and the design, for Diego to decide on, beside the
window drawn in `docs/scheduler.html`. Nothing here is built.

## What there is today

- **Two policies**, swappable because the policy is a table of functions
  (`kernel/sched.h`): **priority** (`sched_prio.c`, the default) - strict
  bands, round robin inside each, a thread that outranks the running one
  taking the processor at once - and **round-robin** (`sched_rr.c`), fair
  and nothing else.
- **Six bands**: input, audio, display, normal, low, idle. A higher band
  always runs first; audio's is guarded by a budget (a thread there that
  runs a full ring's time without sleeping drops back).
- **The quantum**: how long a thread may run before an equal gets a turn -
  ten ticks, 40 ms at `TICK_HZ` 250 - changeable.
- **Every thread has a home core**, the least busy one when it was made,
  and never moves. Each core has its own queue.

## Why the policy cannot be changed

`sched_switch_to` refuses once more than one processor schedules - every
runnable thread had to be drained from the old policy and poured into the
new, and with a queue per core that seemed to mean stopping every core at
once. Spreading across cores became the default in 0.10.22, so the switch
has been refused ever since, and the window said only "could not".

**The fix, as Diego put it**: "perhaps changing policy needs a restart of
the machine?", "i dont care if it changes it on real time", "it just might
need a reboot". So the policy is **a choice kept as a setting and applied
when Kosmos starts**, before a second core takes work - the moment swapping
is a local operation anyway. The window says so beside the choice and offers
Restart now; Nebula reads it from the boot's command line
(`opt/kosmos/sched=round-robin`) or the setting init passes on. The live swap
stays possible on one core, where the suite exercises it.

Changing live on every core *could* be done - a thread never leaves its
home, so each core could change its own queue at its next decision - and is
written down here in case it is ever wanted, not built.

**Game mode is not a policy swap**: it is Balanced with a band and a core
given to one application, so it turns on and off at once.

## What a game wants from a scheduler

A game like Doom, Quake or the Super Nintendo is, in the way that matters,
one thread: it runs a frame - input, simulation, drawing - then waits for
the next. What spoils it is never the average; it is the frame that comes
late. So what a game wants:

1. **Not to be interrupted in the middle of a frame** by work that could
   have waited - a background index, a download, a build.
2. **To start the moment its next frame is due**, not at the next tick.
3. **A warm processor**: its caches still holding its data when it runs
   again, which means the same core and not one shared with a busy
   neighbour.
4. **The rest of the machine still working**: the screen composed, the
   sound playing, the keys read - those *must* stay above it.

## What others do, and what fits Kosmos

| | What it does | Fit |
|---|---|---|
| **Windows Game Mode** | the game in front gets priority for processor and GPU; background updates and work are held back | the foreground idea is right; "held back" is vague |
| **Feral's GameMode on Linux** | raises the game's priority (nice), its I/O priority, sets the processors to full clock, can pin it to the fastest cores | a bag of knobs, most of which Kosmos does not have |
| **Gaming kernels** (BORE, EEVDF's latency-nice, CachyOS's) | favour threads that run in short bursts and sleep - interactive ones - over long-running ones | clever and general, but a fairness scheduler; Kosmos is a priority one |
| **Consoles** (PlayStation, Xbox) | **the game gets cores of its own**; the system is kept to one or two | **the best fit**: Kosmos already gives every thread a home core |

**The console's answer is the one to take**, because Kosmos is already halfway
there: a thread lives on one core. Game mode gives the game a core of its
own and keeps everything else off it.

## Game mode, as designed

When game mode is on for an application - chosen in the Scheduler, or by
an application that declares `kosmos: needs game`, while its window is in
front:

1. **A band of its own**: `GAME`, between normal and display. The game
   outranks every normal thread - downloads, builds, indexing - and is
   still below the screen, the sound and the keys, which must never wait
   for it. Guarded as audio's band is: a thread there that has run for a
   whole second without sleeping drops back to normal until it sleeps, so a
   game that hangs cannot take a core away for good.
2. **A core of its own**: the game's busiest thread is given the least busy
   core, and while game mode is on, **no new thread is placed there** - and
   the normal threads already living there are moved off it, once, while
   none of them is running. This is the one new power in Nebula: a thread
   moved to another home while it is not running, which `docs/smp.md`'s
   "a thread has a home and does not migrate" has to be revisited for, and
   says what it would cost.
3. **No turns taken from it**: on its own core it has no peer, so the
   quantum never cuts its frame. Elsewhere nothing changes.
4. **Woken on time**: a game sleeping until its next frame wakes at the next
   tick - up to 4 ms late at 250 Hz. Game mode asks the timer for 1000 Hz
   while it is on (1 ms), until the one-shot timer arrives (`architecture.md`
   §5) and makes the question go away.

**When the game leaves the front** - another window takes the keys - its
threads drop to normal and its core is opened to new threads again, so a
game in the background is just another program.

## How it is measured

On the M700, on real cores (`-icount` cannot answer this): Doom, Quake and
the Super Nintendo, each with `make stress`'s load beside them, and each
says its own frame times. **What matters is the slowest frames**: the 99th
percentile and the worst of a minute, game mode off and on - and the input
band's longest wait, which game mode must not move. The Scheduler window
shows the same numbers live.

## The steps

- **G1** - the policy chosen in the window, kept as a setting and applied
  when Kosmos starts, with Restart now beside it; the window saying why
  when anything is refused.
- **G2** - the window redesigned as drawn (`docs/scheduler.html`): the
  modes as cards, a timeline of who ran on each core, the bands as a
  ladder, the quantum with what it trades, and the numbers that matter.
- **G3** - the `GAME` band and its guard; game mode for the application in
  front, chosen in the window.
- **G4** - a core of its own: placement kept off it, and the one move of a
  sleeping thread.
- **G5** - 1000 Hz while it is on; measured against Doom, Quake and the Super
  Nintendo under load.

## For Diego to decide

1. **Which game gets it**: the application in front, when it is marked a
   game (`kosmos: needs game`) or chosen in the window (recommended) - or
   any application in front, always?
2. **A core of its own**: allow Nebula to move a sleeping thread to another
   home, once, when game mode starts (recommended - it is what makes the
   core truly the game's), or only keep new threads off the core?
3. **1000 Hz while a game plays**: yes (recommended, measured first), or
   wait for the one-shot timer?
