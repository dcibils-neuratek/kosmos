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
written down here in case it is ever wanted, not built. **Built on 13
October**, because the setting applied at the start could not avoid it: by
the time `/Home` can be read, its servers run on every core.

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
  when anything is refused. **Built 13 October (0.11.214)**, and live
  rather than at a restart: applying at the start met the same wall as
  changing live, since `/Home` is read through servers already running on
  every core, so each core now changes over by itself - the one asking at
  once, every other at its next tick, under its own lock (`change_over`,
  `testing.md` 18.609). The choice is kept by name (`scheduling.lua`) and
  the window manager puts it back at every start; no Restart button is
  needed.
- **G2** - the window redesigned as drawn (`docs/scheduler.html`): the
  modes as cards, a timeline of who ran on each core, the bands as a
  ladder, the quantum with what it trades, and the numbers that matter.
  **Built 14 October (0.11.215)** (`testing.md` 18.610): each core records
  the band it ran at every tick, its switches, and each band's longest
  wait from wake to run (`SYS_SCHED_LANE`); the window draws them. Game
  mode's card and its tile, a game's slowest frame, wait for G3 and G5 and
  say so.
- **G3** - the `GAME` band and its guard; game mode for one application -
  a header's `kosmos: game` while in front, Super+G for the one in front,
  or chosen in the window - and a notification as it starts and ends.
  **Built 14 October (0.11.216)** (`testing.md` 18.611): the band between
  normal and display, a second's budget; given only by a process's parent
  (`process_set_game`), which the window manager is for what it opens; the
  header word is the `kosmos: kind` games already have (`game`, `emulator`)
  rather than a new `kosmos: game`; the bands' names are the kernel's
  (`schedinfo.band`), so Processes, `htop`, init and the Scheduler keep no
  list of their own.
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

**Answered, 11 October**: 2 - yes, the normal threads moved off the game's
core once when game mode starts; 3 - yes, 1000 Hz while a game plays,
measured first. 1 asked back ("You mean to have a per app scheduling game
mode instead of a system wide setting?") and answered in conversation.

**Question 1, decided** (Diego, 11 October 2026): "Let's do per app game
mode yes", and how an application comes to have it:

- **A game says so in its header**: `-- kosmos: game`, as an application
  declares its icon. Doom, Quake and the Super Nintendo carry it; while game
  mode is on, one of them in front has the band and the core.
- **Super+G** - "Windows has this super+g key shortcut that sets game mode
  for the app in front" - turns game mode on or off for **whatever
  application is in front**, game or not. A global key, so it is the window
  manager's, listed in Shortcuts and changeable there like the others.
- **A notification** when it starts and when it ends: "Game mode on for
  Doom - core 4 is its own", and "Game mode off".
- **In the Scheduler, choosing Game mode asks which application**: the
  running ones and those that say they are games, and any can be chosen -
  "I might want use a audio production app in game mode that is not really
  a game but I want a full core for me and better responsiveness". Its
  sound thread keeps the audio band, which is above the game's; game mode
  gives the rest of it a core of its own.

So **game mode belongs to an application**, never to the machine: the
policy stays Balanced, and one application at a time has the band and the
core.

