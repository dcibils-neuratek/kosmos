# Rendering on several machines

Written before it is built (`roadmap.md` 4l, 5d). Diego, 26 September
2026: "What if we have a cluster of kosmos machines with cafesa installed
and we could use these machines as a rendering node", "a main node and
slave nodes that act as rendering nodes so we can use not only the cores in
the current machine but remote nodes of other machines", "This might need a
good architecture behind the ray tracer rendering plugin so it can leverage
multiple nodes as well as multiple cores".

This page is for agreeing the shape. Nothing in it is built.

---

## 1. What the tracer already gives this

Three properties of `k3d_trace.c`, each there for another reason, are
most of a render farm:

- **A render is independent pieces of work.** A picture is tiles; a tile
  is traced once a pass; the workers take `(tile, pass)` jobs from a
  counter. A machine across a network is a worker that is further away.
- **Every sample's randomness comes from its tile, its pass and its
  pixel** - PCG seeded from the three, never from a clock or a thread. So
  a pass traced on another machine is the same numbers it would have been
  here. The tutorial's car, rendered twice on the same machine by two
  different runs, was the same picture bit for bit (`testing.md` 18.198).
- **The scene travels as a file already.** Saving writes glTF with
  Cafesa3D's own numbers (18.199), and the reader that opens it trusts
  nothing in it - which is exactly what a node must do with its main
  machine's bytes.

**One honest caveat about "the same picture".** Here, each pixel's samples
are summed in pass order into one float. Across machines, each node sums
its own passes and the main machine sums the nodes' totals: the same
numbers added in another grouping, which in floating point differs in the
last bits. The picture is the same to the eye and not to `cmp`. Keeping it
bit-exact would mean each node sending every pass separately, which costs a
frame's worth of floats a pass; a sum in double precision on both sides is
the cheaper answer and is proposed.

---

## 2. The shape

```
            Cafesa3D (the main machine)
                  |  k3.render{ ..., nodes = { "10.0.0.7", "10.0.0.8" } }
                  v
      a render job - passes(), rays(), paint(), stop() - as today
          |                    |                     |
   local workers        node 10.0.0.7         node 10.0.0.8
   (a thread a core)    rendernode, N cores   rendernode, M cores
```

**Behind the same interface Cafesa3D already uses.** `k3.render` answers
a job with `passes`, `rays`, `paint` and `stop`. A render that uses other
machines answers a job of the same shape, so Cafesa3D does not change when
the machines do - the same argument `design.md` 7.1 makes for a GPU.

**Split by passes, not by tiles.** Each node is given whole passes of the
whole picture - node one passes 1, 4, 7 ..., node two 2, 5, 8 ..., the
local cores 3, 6, 9 ... - traces them with all its cores, and sends back
its sum. Every node's result then improves the whole picture at once, which
is what a progressive render wants to show; and a node that is slower
simply contributes fewer passes, since the next pass goes to whoever
finishes first. Splitting by tiles would leave a slow node's tiles visibly
behind.

**A node is a program**, `rendernode`, run on each machine that renders:
it listens on one port, takes one render at a time, and has the 3D Kit, the
network and nothing else - no file system, since a scene arrives as bytes
and a result leaves as bytes.

---

## 3. What crosses the network

A declared protocol, `user/include/renderproto.h`, as `audioproto.h` is:
fixed fields a message, so a node cannot be sent a shape it has no field
for. Control by message; the bulk - a scene, a sum of passes - as a length
and then its bytes on the same connection, since across machines there is
no shared region to put it in.

| Message | From | Carries |
|---|---|---|
| `HELLO` | main | the protocol's version, and the farm's key (below) |
| `SCENE` | main | the glTF file's bytes, the camera, the size, the bounces, Final or Preview |
| `PASSES` | main | which passes to trace, as a first and a step |
| `SUM` | node | how many passes it has done, and their sum: width by height by three floats |
| `STOP` | main | the render is over or changed; forget it |

A 1280 by 720 sum is eleven megabytes of floats, so a node sends one when
asked, a few times a render, rather than after every pass. Compressing
them is a later question, answered by measuring.

---

## 4. What a node trusts

**Nothing about the scene.** It is parsed by `scenefile.lua`, which holds
every number to its range and skips what it cannot take, and a mesh the 3D
Kit refuses is skipped rather than fatal (18.199).

**Who may use it.** A node renders for whoever knows the farm's key - a
secret written on each machine once, sent in `HELLO`, and compared in
constant time - so a machine on the same network cannot borrow every core
it has. This is the first thing in Kosmos that listens for strangers, and
it should be the first thing reviewed for that.

**How much it will do.** A render's size and passes are held to limits the
node sets for itself, so a main machine cannot ask for a picture larger
than the node's memory.

---

## 5. When a node goes away

A node that stops answering - unplugged, crashed, asleep - has its passes
given to whoever finishes next, and its last sum is still counted, since a
pass it summed is a pass done. A render never waits for a node; it waits
for passes.

---

## 6. Nodes that are not Kosmos

The tracer is portable C and already builds on the Mac for its tests
(`tools/test_trace.c`), so `rendernode` can be built for macOS and Linux
from the same files and join a farm of Kosmos machines. That is the fastest
way to have one: the Mac's ten cores beside a ThinkPad's eight.

---

## 7. Steps, each used and tested before the next

1. **`renderproto.h` and `rendernode`**, and a Cafesa3D that renders
   through one node and no local cores - so the whole path is exercised by
   the simplest farm there is. Tested with two QEMU machines joined by
   QEMU's socket network: the picture through the node against the picture
   rendered locally, within the floating-point caveat above.
2. **Several nodes and the local cores together**, passes handed out to
   whoever is free.
3. **A node dropping out** mid-render, in the same two-machine test: its
   passes finished by the others.
4. **`rendernode` for the Mac**, built by the Makefile's host rules.
5. **Finding nodes** - named by hand in the Render tab first; announcing
   themselves on the network later, if naming them turns out to be the
   annoying part.

**What it needs first**: nothing else on the roadmap. Saving (step 5) is
done, the tracer's passes are deterministic, and the network stack's TCP
has carried the browser and `host` for months.
