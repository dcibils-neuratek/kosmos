# Pulses: one-way messages in Nebula

*For Diego to decide, 14 October 2026. Nothing here is built.*

Diego, 14 October 2026, asked why `maild` tells `autod` about new mail
through a file rather than by publish and subscribe: "Go ahead with files
And then we will design pulses". This is that design.

## The problem

Nebula's IPC is **call and reply**. A sender waits until the receiver
answers, which is what makes most of Kosmos simple: a request and its
answer are one step, nothing is lost in between, and the receiver's
priority can be lent to the caller.

It is also why publish and subscribe cannot be built today without
something waiting:

- **A publisher that calls its subscribers waits on each of them.** `maild`
  calling `autod` waits while `autod` is in the middle of a step - a press
  that waits for a window can take five seconds. That breaks *nothing waits
  on a server* (`CLAUDE.md`).
- **A subscriber that holds a call cannot answer anything else.** The
  window manager's `poll` is this pattern - the window calls, the manager
  holds the reply until an event - and it works because a window has
  nothing else to answer. `autod` does: the Automations window's Run now
  and status. A Lua process is one thread.

So every place that needs to be *told* polls instead: `autod` reads the
mail file and lists folders every five seconds; the Deskbar and Monitor ask
on a clock; nothing can say "this file changed" (`automations.md`).

## What already exists, and is the same shape

- **An interrupt line counts** (`kernel/irq.c`): `irq_deliver`, from the
  interrupt handler, adds one to the line's `pending` and wakes the driver
  waiting on it; nobody waits for the driver. That is a notification - a
  one-way signal with no payload - and it is already safe from an
  interrupt, which is the hardest caller there is.
- **An endpoint can be watched** (`ipc_endpoint_watch`): one thread waits
  on several, which is what a server answering both requests and pulses
  needs.

## The design

**A pulse is a small fixed message sent to an endpoint without waiting.**
QNX's word and QNX's shape: a `code` (what happened) and a `value` (a
number with it), 16 bytes. It is received by the same receive a request is,
so a server's loop does not change shape - it gets either a request (answer
it) or a pulse (act on it; there is nobody to answer).

```lua
sys.pulse(cap, code, value)      -- queued; returns at once, never waits
local msg, who = sys.receive(ep)
if msg.pulse then ... msg.code, msg.value, msg.count ... end   -- `who` is nil
```

(`sys.receive`'s fourth answer is taken - the window manager's raw frames -
so a pulse says what it is in the table it comes as.)

**Bounded, and coalescing when full.** Each endpoint has a ring of pulses,
32 by default, carved from the endpoint's slab rather than any heap (*no
heap for kernel objects*). When the ring is full a pulse of a `code`
already queued adds to that one's count instead of taking a slot - so a
subscriber that falls behind learns "mail arrived, 7 times" rather than the
sender being refused or made to wait. A pulse of a new code into a full
ring is refused with `IPC_ERR_FULL`, and the sender carries on. **A pulse
never blocks and is never silently lost**: it is queued, merged, or refused
with a number the sender sees.

**Who may pulse an endpoint**: whoever holds a capability to it - the same
rule as calling. Subscribing is a call that hands the publisher a
capability to the subscriber's endpoint (`fs.send(..., ep)`, as `/Running`
registration already does), so a publisher reaches exactly its subscribers
and nobody else.

**What carries the data**: nothing. A pulse says *that* something happened;
the subscriber asks for *what* with an ordinary call when it is ready -
`maild`'s "what arrived after number N" - which is *control by message*,
and keeps a pulse small enough that a full ring is cheap.

**Priority**: a pulse wakes the receiver at the receiver's own priority; it
lends nothing, since nobody waits on it.

## What it would replace

| Today | With pulses |
|---|---|
| `autod` reads `/Temporary/maild/arrived` every 5 s | `maild` pulses `autod` once per batch; `autod` asks what came |
| `autod` lists watched folders every 5 s | the filesystem server pulses a watcher when a folder changes - a step of its own after this |
| the Deskbar asks the network, battery and sound on its clock | each server pulses what changed |
| the window manager's `post` to a window, held until its next poll | unchanged: a window already has nothing else to answer |

## Steps

- **P1 - the kernel**: the ring in each endpoint, `SYS_PULSE`, receive
  returning a pulse, coalescing and refusing; a kernel test on both
  architectures with controls (a full ring merging; a dead endpoint
  refusing; a pulse from an interrupt handler's context, if a driver should
  ever want one).
- **P2 - the runtime**: `sys.pulse`, a pulse from `sys.receive` as a table, and the
  C kit for servers (`kosmos_pulse`).
- **P3 - the first user**: `maild` and `autod`; `mailarrived.lua` asks
  instead of reading a file, and the file goes.
- **P4 - folders**: a change pulse from `diskfs` and `ramfs` to whoever
  watches a folder, and `autod` stops listing.

## For Diego to decide

1. **Pulses as above** - QNX's shape, a code and a value, coalescing when
   full - or **seL4's notification**: one word of bits per endpoint that
   senders OR into, no queue at all. seL4's is smaller and can never be
   full, but says only *which* things happened, not how many or in what
   order. I recommend QNX's.
2. **The ring's size** - 32 a endpoint, or grown as the pools grow.
3. **P4 in this piece or later** - folders telling is the larger change,
   two filesystem servers in C; I recommend after P3 has been lived with.
