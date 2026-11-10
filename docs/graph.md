# The Graph Kit: a program as a graph of tasks, on the processor's cores first and the GPU after

*Designed 1 November 2026. Nothing here is built. The choices in
"Decisions" are Diego's and the recommended ones are marked.*

Diego, 1 November 2026, after an afternoon of asking what an operating
system that lived on a GPU would be: keep Kosmos as it is, and add a GPU
backend for the applications that fit that shape - a program written as a
graph of tasks with continuations, where the processor is the plane of
input and output and the GPU the plane of computation. Not an operating
system on the GPU; a way for a program to be written so that it can run
there. And its first stage on the processor alone: "plan the architecture,
the high-level design, the pseudocode and the diagrams first, then we code".

This page is that plan, written before any of it, as `threads.md` and
`compute.md` were: what a program can do, what it is made of, the
decisions, the busiest path, and the steps - each one checkable on its own.
`graph-architecture.png` is the picture.

---

## What this is, and what it is not

**It is a kit.** `use("/Kosmos/Kits/graph")`. A program declares its work
as tasks and the edges between them, hands the declaration to the kit, and
the kit runs it - on worker threads across the machine's cores in stage 1,
and with the data-parallel tasks on the GPU through the Compute Kit in
stage 2. The program never makes a thread, never takes a lock and never
waits: a task runs, emits the tasks that follow it, and ends.

**It is not an operating system on the GPU**, and the afternoon's
argument is recorded here so it is not had again. A GPU cannot boot
itself, cannot take an interrupt from a disk or a keyboard, and runs
branchy control code - a parser, a scheduler, a filesystem - a tenth as
fast as a core does, with thirty-one lanes idle. A desktop whose control
plane lived there would pay for every click with a round trip and burn
sixty watts polling for the next one. What *does* pay is a program whose
work is GPU-shaped - many items, the same arithmetic on each - with its
control kept small and its data never copied. That is a runtime inside
Kosmos, on top of Nebula's threads and the Compute Kit, and that is what
this is.

**And it is the shape `threads.md` step 8 was going to have anyway.** A
Lua thread there is "an interpreter of its own ... threads talk through
channels: a value sent is copied ... a worker's answer arrives in the
window's event loop as an event". A graph of tasks is that, with the
channels drawn by the program as edges instead of wired by hand, and the
question "which thread does this" answered by the kit rather than by the
program. Stage 1 of this page *is* step 8, in a shape that stage 2 can
move to the GPU without the program changing.

**The M700's GPU is the one we have, and it is modest.** Twenty-four
execution units of 2016; Cafesa3D's tracer ran 10.8 times slower on it
than on the processor until it was rewritten for the GPU's memory system,
and the House still renders faster on the processor (`state.md`, 19
October). The kit is designed for the model, not for that card: a program
written as a graph runs on four cores today and on whatever GPU Kosmos
drives next, and the measurements - not the design - decide what goes
where.

## What a program can do

With the Graph Kit, from Lua:

- **Declare its work as tasks**, each of one of three kinds. A **scalar**
  task is a Lua function in its own file, run by one worker in a Lua state
  of its own: a parser, a decoder's driver, anything with decisions in it.
  A **work** task is a C function from a kit, by name, run by one worker:
  a loop over bytes that `CLAUDE.md` says is never Lua. A **parallel**
  task is a Compute Kit kernel over a grid: the same arithmetic on every
  item. In stage 1 a parallel task runs on the processor through the
  fallback the program gives it; in stage 2 it is a run on the GPU.
- **Draw the edges**: which task's end emits which. A task that fans out -
  one per page, one per stream, one per tile - says so, and the kit counts
  the join.
- **Hand data around without copying it**: a buffer is a Compute Kit
  buffer, a region every task sees and the GPU will see, named in a task's
  arguments by handle. What travels with a task is small and is copied -
  numbers, strings, a table of either - by the serialiser the runtime has.
- **Keep state**: a table of fields in a region, one writer a field,
  counters the kit keeps atomically; what a program would otherwise keep
  in globals.
- **Emit**: from any task, `emit(name, args)` queues the next; it returns
  at once. Nothing in the kit waits for anything.
- **Hear the answers as events**, in the window's own loop, where a click
  arrives: a task declared `to = "window"` ends by handing its result to
  the program's main state, which reads it on its tick - as GPU Fractal
  reads its ticket.
- **See what it cost**: every task's time, by kind and by name, and how
  busy each worker was, from the kit's own clock, so the measurement that
  decides what moves to the GPU is in the program from the first day.

**And what it may not do**, which is the discipline that makes the rest
safe: a task may not block, may not touch another task's state, and may
not reach a buffer it was not handed. A Lua task that calls `sys.sleep`,
or `receive`, or opens a file through the namespace directly, errors with
its line said; files come in through a scalar task that asks the window
state for them, or through `graph.read(path)`, which is a request to the
main state and a continuation, as everything is.

## What it is made of

| Piece | Supplied by | New or as it is |
|---|---|---|
| The declaration - tasks, edges, buffers, state - and what a task may call | **`graph.lua`**, a new library in `user/lib/`: reads the program's table, checks it (a task named but never defined, an edge to nothing, a fan-out with no join), and hands the kit a flat description | **new**, Lua |
| Workers, queues, the join counters, the clock | **the Graph Kit**, `user/kits/graph/`, `use("/Kosmos/Kits/graph")`: one worker thread a core by `SYS_THREAD_CREATE` (`threads.md` step 3), a single-producer single-consumer ring of task records a worker, a futex a worker sleeps on when its ring is empty, and `emit` as the only door in | **new**, C |
| A Lua task's interpreter | **a `lua_State` a worker**, made as `lua_glue.c` makes a program's, with `use`, `sys` and the kit bound and the process's capabilities shared (`threads.md` step 1: capabilities are the process's) - and with `sleep`, `receive` and the namespace's own calls bound to functions that refuse | **new**, C, in the kit |
| A C task | a kit's function named in `graph_work.h`, the table a kit registers its workers in - the first ones `gfx`'s decoders and the PDF Kit's page | **new** door, existing code |
| A parallel task on the processor (stage 1) | the task's `fallback`: a C function over an item range, from the same table, which the kit spreads over the workers | **new** door |
| A parallel task on the GPU (stage 2) | **the Compute Kit**: `compute.run` with the task's kernel, buffers and grid, and the ticket polled by the kit's own worker zero | as it is, with `GPU_OP_RUNS` for a fan-out of kernels |
| Buffers | the Compute Kit's buffers, as they are: a region, write-combining, in the GPU's table when there is one | as it is |
| Arguments and results | the runtime's serialiser, as a message's payload is written today | as it is |
| The answer in the window | the program's tick (`win.tick_every`), reading `graph.events()` - a one-way queue from the kit into the main state | **new**, small; a pulse when `pulses.md` is built |
| The first program | **Contact Sheet** (decision G3): every picture in a folder decoded on the cores at once, thumbnails arriving as they finish, the time each took and the whole | **new** app, drawn before it is written |

**Control by message, data by shared memory**, one layer down: a task
record is 64 bytes - what to run, where its arguments are, where its
answer goes - and the bytes a task works on are in buffers that never
move. A ring of task records between two threads is the single-producer
single-consumer ring with indices `CLAUDE.md` asks for, and nothing else
is shared between workers but the state's counters, which are atomic.

**C or Lua.** The declaration and its check are Lua: they run once, when a
program starts, and their bug is a stack trace. The kit is C: it is on the
path every task takes, and a garbage collector there would be the audio
server's mistake again. A scalar task is Lua in its own state, with its
own collector, so a task collecting never pauses the window. A work task
and a fallback are C. Nothing per item is Lua.

## The model

A program is a graph. The nodes are tasks; the edges say what a task's
end emits. The kit runs it as a queue of task records across workers that
never wait for one another.

**A task is one of three kinds.**

- `scalar` - a Lua function, in its own file, run on one worker in that
  worker's Lua state. For decisions: parsing, driving a decoder, deciding
  what to do with a result.
- `work` - a C function from a kit, by name. For loops over bytes on the
  processor: decoding a JPEG, inflating a stream, copying a tile.
- `parallel` - a kernel over a grid: items that are each the same
  arithmetic. On the GPU it is a Compute Kit run; on the processor it is
  the task's fallback spread over the workers in ranges. **Stage 1 has
  only the fallback**; the kernel field is read, checked by `kc.lua` and
  not run.

**A task never waits.** It runs to the end and emits. A task that needs
something another task produces does not ask for it: it is emitted *by*
that task, with the result in its arguments. So there is no blocking,
no lock and no deadlock, and a worker is either running a task or asleep.

**A fan-out is a task emitted many times, and a join is a count.** A task
declared `fanout = true` is emitted once per item by its parent, and the
task after it is declared `join = "<name>"`: the kit keeps a counter in
the state for each fan-out, every instance adds one as it ends, and the
one that brings it to the count emits the join. That is the atomic the
afternoon's PDF reader needed for `all_streams_done_for_page`, kept by the
kit rather than written in every program.

**State is a table of fields in a region, one writer a field.** The
program declares it; the kit lays it out; a task reads any field and
writes only the fields it is declared to write (`writes = {...}`), which
the kit checks when the program starts rather than at run time. Counters
the kit keeps are atomic; nothing else needs to be, because one writer a
field is the single-producer rule applied to a word.

**Buffers are Compute Kit buffers.** Made by the main state, handed to a
task by handle in its arguments, and seen by every task and by the GPU.
A Lua task sees one as the userdata it already is, in its own state; a C
task gets the bytes through `compute_buffer_bytes`, as the 3D Kit does.

**Results go where the declaration says.** `to = "<task>"` emits that
task with the result as arguments; `to = "window"` queues it for the main
state's `graph.events()`; nothing, and the result is dropped.

**The GPU changes none of this.** In stage 2 a `parallel` task's kernel is
run instead of its fallback, by worker zero, which submits the run and
polls the ticket as it does its other work; the task's end emits as any
task's does. The program's declaration does not change, which is the
whole point of the model: what moved was where one kind of task runs.

## The ABI

What a program says, and what crosses into the kit. The declaration is a
Lua table, because it is written once by a person; what crosses is a
declared shape, because the kit is a system component and a table is not
a boundary (`CLAUDE.md`, *A server receives exactly what it expects*).

**The declaration**, `graph.app{...}`:

```lua
local graph = use("/Kosmos/Libraries/graph.lua")

local g = graph.app {
  workers = "cores",                 -- or a number; "cores" is one a core
  buffers = { sheet = compute.picture(W, H) },
  state = {
    done = 0,                        -- counters the kit keeps atomically
    failed = 0,
  },
  tasks = {
    list   = { kind = "scalar",   file = "sheet/list.lua",   to = "decode", fanout = true },
    decode = { kind = "work",     fn = "gfx.decode_picture", to = "place" },
    place  = { kind = "work",     fn = "gfx.scale_into",     to = "window",
               writes = { "done" }, join = "decode" },
    shade  = { kind = "parallel", kernel = "sheet/shade.kc", fallback = "sheet.shade_range",
               grid = { W / 16, H }, to = "window" },
  },
}

g:emit("list", { folder = "/Home/Pictures" })

win.tick_every = counter_hz // 60
function win:tick()
  for ev in g:events() do ... end  -- one thumbnail placed; the whole done
end
```

**A task record**, the 64 bytes a worker's ring carries, in
`user/include/graphproto.h`:

```c
struct graph_task {
    uint32_t task;        /* index in the declaration's task table      */
    uint32_t instance;    /* which one of a fan-out; 0 otherwise        */
    uint32_t args_off;    /* arguments: offset and length in the args    */
    uint32_t args_len;    /*   arena, serialised                         */
    uint32_t join;        /* the fan-out this instance counts toward     */
    uint32_t range_lo;    /* a fallback's item range, [lo, hi)           */
    uint32_t range_hi;
    uint32_t flags;       /* GRAPH_TO_WINDOW, GRAPH_LAST_OF_FANOUT       */
    uint64_t issued;      /* the kit's clock when it was emitted         */
    uint8_t  reserved[24];
};
```

**The rules a task is held to**, checked when the program starts, never
at run time:

- every `to` names a task or `"window"`; every `join` names a task that
  is a fan-out; every `writes` names a state field; a `parallel` task has
  a `fallback` (stage 1) and a `kernel` that `kc.lua` accepts;
- a `scalar` task's file is read and loaded once in every worker's state
  when the program starts, and its function is `return`ed by the file;
- a `work` task's `fn` is in `graph_work.h`'s table, or the program is
  refused with the name said.

**What the kit exports to a task.** In a Lua task: `emit(name, args)`,
`state` (reads; writes only to declared fields, or an error with the line),
the buffers by name, `log(...)` to the UART through the kit (the state has
no console), and nothing of `sys` that waits. In a C task: the same, as
`graph_emit`, `graph_state`, `graph_buffer`, `graph_log`.

## Pseudocode

**The kit's scheduler** - the whole of it, because the point is how little
there is:

```c
/* graph_core.c - one worker thread a core, a ring each, a futex each. */

static void worker(long index)
{
    struct worker *w = &kit->workers[index];
    w->L = scalar_state_for(w);            /* its own lua_State, the tasks' files loaded */
    for (;;) {
        struct graph_task t;
        if (!ring_pop(&w->ring, &t)) {     /* empty: steal from a neighbour, else sleep */
            if (!steal(w, &t)) { futex_wait(&w->sleeping, 1); continue; }
        }
        uint64_t began = sys_ticks();
        run_one(w, &t);                    /* scalar: call in w->L; work: table[fn]; */
                                           /* parallel: fallback over [lo, hi)       */
        account(t.task, sys_ticks() - began);
        if (t.join && atomic_add(&kit->joins[t.join], 1) + 1 == kit->fanout_count[t.join])
            emit_record(kit->join_target[t.join], /* args: the join's result */);
    }
}

int graph_emit(uint32_t task, const void *args, size_t len, uint32_t instance)
{
    struct graph_task t = { .task = task, .instance = instance, .issued = sys_ticks() };
    t.args_off = arena_push(kit->args, args, len); t.args_len = len;
    struct worker *w = least_loaded();     /* a count a worker, no lock */
    if (!ring_push(&w->ring, &t)) return GRAPH_ERR_FULL;   /* bounded; the caller sees it */
    futex_wake(&w->sleeping, 1);
    return 0;
}
```

A `parallel` task's fallback is emitted as `workers` records with ranges,
each `GRAPH_LAST_OF_FANOUT`-counted into a join the kit makes for it, so
a fallback over a million items is the same path as a fan-out of pages.

**A scalar task**, as the program writes it - `sheet/list.lua`:

```lua
-- Every picture in the folder, one decode task each. Runs once, on one worker.
return function(args, emit, state, buffers)
  local names = graph.read_dir(args.folder)       -- a request to the main state, answered
  state.count = #names                            -- declared writes = { "count" }
  for i, name in ipairs(names) do
    emit("decode", { path = args.folder .. "/" .. name, slot = i })
  end
end
```

**A work task**, registered by a kit - `gfx`'s decoder in `graph_work.h`'s
table:

```c
/* gfx: decode one picture into a buffer of its own, and say which. */
static int decode_picture(struct graph_call *c)
{
    const char *path = graph_arg_string(c, "path");
    struct picture *p = gfx_decode_file(path);          /* png.c, jpeg.c - as they are */
    uint32_t b = graph_buffer_adopt(c, p->pixels, p->w * p->h * 4);
    graph_emit_to(c, "place", /* { slot, buffer = b, w, h } */);
    return 0;
}
```

**Contact Sheet**, the window's side, in full shape:

```lua
-- kosmos: application
local g = graph.app{ ... as above ... }
g:emit("list", { folder = folder })

function win:tick()
  for ev in g:events() do
    if ev.task == "place" then
      damage(slot_rect(ev.slot))                  -- the thumbnail is already in `sheet`
      status(("%d of %d, %.1f ms each, %d workers busy")
             :format(g.state.done, g.state.count, g:time("decode") / 1e6, g:busy()))
    elseif ev.task == "shade" then
      damage(all)
    end
  end
end
```

Nothing in the program is a thread, a lock or a wait, and nothing in it
changes when `shade` moves to the GPU.

## Decisions, Diego's

**G1 - what it is called and where it lives.**

- **A - the Graph Kit**, `use("/Kosmos/Kits/graph")`, with `graph.lua` as
  its Lua door in `user/lib/` *(recommended)*. A kit is code you run in
  your own process, which is what this is; "graph" is what the program
  writes.
- **B - the Compute Kit grows** to take tasks and workers. One door for
  everything the GPU touches, and a kit that then owns threads, Lua states
  and queues as well as kernels and buffers - two things in one place.
- **C - the Thread Kit**, `threads.md`'s step 8 under its own name, with
  the graph as the way it is used. Honest about the lineage; but a program
  would reach for threads where it should reach for tasks.

**G2 - how a Lua task gets its interpreter.**

- **A - a state a worker, made once, every scalar task's file loaded into
  each** *(recommended)*. A task is a call into a warm state; the cost is
  memory - a state a core, with the program's task files in each - and the
  gain is no state made per task, which at a thousand tasks a second is the
  difference between a kit and a bottleneck.
- **B - a state a task**, made and closed each time. Nothing shared, and
  `lua_newstate` plus the files' loading on every task.
- **C - one state, locked**, the workers taking turns. Not recommended: it
  is one core with extra steps.

**G3 - the first program, which is what proves the model.**

- **A - Contact Sheet** *(recommended)*: every picture in a folder decoded
  on the cores at once through `gfx`'s decoders, each thumbnail placed as
  it is ready, a `parallel` shade over the sheet at the end, and the times
  shown. It has a fan-out, a join, a work task, a scalar task and a parallel
  one; it is a thing a person wants (Gallery's thumbnails made parallel are
  its second user); and it is honest, because Kosmos cannot decode on four
  cores today. Drawn before it is written, as an app is.
- **B - GPU Fractal written as a graph.** Smallest, and it already runs on
  both the GPU and the processor; but it is one parallel task and a paint,
  so it would prove the kit can do what a ticket already does.
- **C - the solar simulation as a graph.** `solar.lua` says its core "is
  not ours and is not touched", and a graph is a rewrite; so no.

**G4 - where the scheduler lives when the GPU is there.**

- **A - on the processor, with the GPU taking runs** *(recommended, and
  the only one for the M700)*. Worker zero submits a `parallel` task as
  `compute.run` - or a fan-out of them as `GPU_OP_RUNS`, 16 at a time - and
  polls the ticket between its other tasks. The afternoon's "resident
  kernel", a scheduler living on the GPU, needs preemption the Intel Gen9
  render engine does not have for compute: a kernel that never ends would
  hold the engine the desktop draws with.
- **B - a resident kernel on the GPU**, the afternoon's design in full.
  Revisited when a GPU with compute preemption and a documented doorbell is
  the target, and measured against A before it is chosen.

**G5 - how a result reaches the window.**

- **A - the window's tick reads `g:events()`** *(recommended for stage
  1)*: as GPU Fractal reads its ticket, on `win.tick_every`, which every
  app that draws on a clock has. A result waits at most a frame.
- **B - a pulse** to the window's endpoint (`pulses.md`), which wakes the
  window's loop the moment a task ends. The right shape, and not built;
  the kit takes it when it exists, with A kept for a program that has no
  endpoint.

## The busiest path

**Contact Sheet over a folder of 48 photos, 4 workers, stage 1.** The
window emits `list` - one record, one `futex_wake`, under a microsecond.
`list` on worker 0 reads the folder through the main state (one request,
one continuation) and emits 48 `decode` records into the four rings, 12
each, with the serialised arguments - a path and a slot - 60 bytes a
record in the arena. Each worker pops, calls `gfx_decode_file` in C -
tens of milliseconds a JPEG of 12 megapixels, the whole cost of the
program - adopts the pixels as a buffer, and emits `place`, which scales
into `sheet`'s rectangle for that slot, adds one to `done`, and queues an
event. The window's next tick damages one rectangle. The 48th `place`
brings the join to 48 and emits `shade`, which in stage 1 is 4 records of
a range each over the fallback; its join emits the last event. **The
processor's part is one message a frame and a damage a thumbnail; the
kit's, 48 pops, 97 pushes and 48 counts, each bounded and lockless; the
time is the decoders', on four cores where today it is on one.** Expected
on the M700: a 48-photo folder in a quarter of the time Gallery takes,
the number the suite will record.

**What a task costs to emit**: a serialisation of its arguments, an arena
push, a ring push and a wake - a few microseconds, which is the floor
under a task's size. A task that does less than that is the wrong grain,
and the kit's own time, shown beside the tasks', will say so.

## Stage 1 and after

Stage 1 is the model on the processor, with no GPU in it; the gate runs
in QEMU, where there is no GPU, so every step here is checked there.

1. **S1 - `graphproto.h` and the kit's core**, no Lua yet: workers a
   core, rings, the futex, `graph_emit`, fan-out and join counters, the
   clock; a C test that emits ten thousand records through four workers
   and counts them, and one that fans out and joins. Every pool the kit
   keeps is bounded and sized when the program starts; a full ring is
   refused with a number, never grown under a running worker.
2. **S2 - `graph_work.h` and the first work tasks**: `gfx`'s decoders and
   `scale_into` registered; a test that decodes the test pictures on four
   workers and holds the pixels to the one-worker result.
3. **S3 - a Lua state a worker**: made as a program's is, with `use` and
   the refusing `sys`; a scalar task's file loaded into each; `emit`,
   `state` and the buffers bound; a test that runs a Lua task on each
   worker and one that proves a task cannot sleep, receive or read the
   namespace. **This is `threads.md` step 8.**
4. **S4 - `graph.lua`**: the declaration read, every rule checked with the
   line said, the flat description handed to the kit; `g:events()`,
   `g:time`, `g:busy`. Host tests of the checks, as `kc.lua`'s are.
5. **S5 - `parallel` on the processor**: a fallback spread in ranges, its
   join made by the kit; `kernel` read and checked by `kc.lua` and not run.
6. **S6 - Contact Sheet**, drawn first (`sheet.html`), then written; its
   suite under QEMU with the test pictures, the dated screenshot, and the
   number on the M700.
7. **S7 - the measurement**: Gallery's thumbnails through the kit, before
   and after, because a kit that buys nothing is complexity for nothing
   (`threads.md` step 9's rule).

Stage 2, designed on its own page when stage 1 has a number: a `parallel`
task run through the Compute Kit by worker zero, `GPU_OP_RUNS` for a
fan-out of kernels, buffers already in the GPU's table, and Contact
Sheet's `shade` moved there with the program unchanged - the test of the
whole idea. Stage 3, if a GPU with compute preemption is ever the target:
G4 B, measured against G4 A.

**What stage 1 decides.** Whether a program written as a graph is clearer
than the same program written with threads and channels, which is what
`threads.md` step 8 would otherwise give. If Contact Sheet is simpler
this way, the kit is right and the GPU is a backend for it. If it is more
tangled, the problem was never the GPU, and step 8 is built plain.
