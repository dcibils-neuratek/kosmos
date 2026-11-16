# JavaScriptCore's JIT on Kosmos

*Agreed 14 November 2026 (Diego: "JavaScript need JIT for sure") and put
in the order on the 16th: the disk cache, the padlock and the shield,
**the JIT**, HTTP/2, the GPU. Step 1 - Baseline and DFG - built in 0.11.449;
measured on the M700 next.*

**In short.** JavaScriptCore runs every script in its interpreter, the
LLInt, today. A JIT turns a function that runs often into machine code,
which is several times faster - and most of what keeps NYT, Reddit and CNN
over five seconds on the M700 is script. A JIT writes code and then runs
it, and Nebula never lets a page be writable and executable at one address.
So the code lives in one region mapped twice: written through one address,
run from another. That is how iOS does it, and JavaScriptCore already has
the code for it.

---

## What is there before it

- **The LLInt in assembly**, not the C loop: `ENABLE_C_LOOP` is 0 in the
  x86-64 build, so the offline assembler already writes the interpreter
  for this processor, and the JIT's tiers start from it.
- **Two mappings of one region**: `SYS_MEM_MAP` with `MEM_MAP_RX` maps a
  region read and execute, beside its read and write mapping (`docs/w2.md`
  part 2, 0.11.371). Built for this and never used until now.
- **Regions given on touch** (`MEM_ON_TOUCH`): pages arrive as they are
  first touched, so a large pool costs only what is written.
- **Threads stopped and read** (W2 part 3), which the garbage collector
  already uses to scan another thread's registers.

## How JavaScriptCore writes code

Every write into the pool of compiled code is `performJITMemcpy`'s. On a
system where pages can be writable and executable at once it is a store in
place. On iOS - and here - it is the *separated heap*
(`ENABLE_SEPARATED_WX_HEAP`): the pool JavaScriptCore knows, and runs, is
read and execute; a second mapping of it is read and write, and a writer
function is handed the offset into the pool and copies there.

iOS makes the second mapping with Mach's `mach_vm_remap`. Here it is the
region's own two mappings:

| | where | what it is |
|---|---|---|
| the pool | `kosmos_mem_map_rx(region)` | where code runs; what JavaScriptCore calls executable memory |
| the writer's | `kosmos_mem_map(region)` | written by `kosmosWriteToJITRegion` alone |

The region is 64 MB, given on touch. A web process's code is a few
megabytes; the size is address space, not memory.

## Step 1 - the Baseline and DFG JITs

**Baseline and DFG together, not Baseline alone.** The plan was the
Baseline JIT first and DFG as step 2, and the first build said why not: on
64 bits the Baseline JIT's own files use DFG's types - its inline caches
are DFG's `UnlinkedPropertyInlineCache`, its compilation results DFG's -
and upstream never builds one without the other. A 64-bit Linux build has
both. Patching DFG out of the Baseline JIT would be a port of our own to
keep; turning DFG on is the shape everybody else runs. DFG's
signal-based traps need a machine context (`HAVE_MACHINE_CONTEXT`), which
Kosmos does not claim, so its traps poll.

Patch 0037 (`runtime/patches/webkit/`), and WTF's `KosmosJITPool`:

- **The separated heap on Kosmos** (`PlatformEnable.h`).
- **The pool** (`ExecutableAllocator.cpp`, and
  `wtf/kosmos/KosmosJITPool.cpp` in Kosmos's port): the region made, mapped
  twice - in WTF, whose Kosmos files are the ones that speak to Nebula,
  since JavaScriptCore compiles its files together and none of them can be
  given the kernel's headers alone - and handed to JavaScriptCore as an already-mapped reservation
  (`PageReservation::adoptMapped`). Commit and decommit do nothing: pages
  come as they are touched, and **Nebula cannot yet give back part of a
  region**, so code that is freed keeps its pages until the process ends.
  That is the step's known cost; a call to discard a region's pages is the
  answer when it is measured to matter.
- **x86-64's writes into finished code** (`X86Assembler.h`): a repatched
  jump, call, pointer, immediate or run of nops went through
  `performJITMemcpy` only in the mprotect mode, and was stored in place
  otherwise - which, with the pool read and execute, is a write into a
  page that refuses it. Ten such writes now take the separated heap's
  path. ARM64's always did, which is why iOS never needed this.
- **The JIT and DFG on, FTL off** (`runtime/cxx/jsc.cmake`).

Held to:

- **test262**, as the interpreter is (`make test262`, `bench/test262.json`):
  no test that passed may fail.
- **`x86-jsc`**: a hot function - called three thousand times - must be
  compiled, and the bytes the writer copied into the pool must grow while
  it runs (18,876 the first time), with the answer the interpreter gives.
  Pages touched do not prove it: new code lands in pages the first tiers
  already touched.
- **A control**: the writer pointed at the read-and-execute mapping
  instead of the writable one - which faults at the first write
  (`cr2 0x140000000`, a write to a page that refuses it), proving that
  every write went through the second mapping and that the pool really is
  not writable where it runs.
- **`x86-webkit2`, `x86-sites`, `x86-jetski`** and the rest of WebKit's
  suites, since the same build is JetSki's.
- **The 23 sites on the M700** (`tools/top20.py`), before and after: the
  number this is for.

## What it showed (0.11.449, on the M700)

`testing.md` 18.847. Script is **3.4 times faster** - 65 ms against
220 ms for the same `jsc -e`, the same image with `--no-jit` - and the
23 sites load **no faster**: 15 of 23 within five seconds, as on the
interpreter. So the JIT is kept, because script that runs is faster and
pages that run more of it later (a site in use, not loading) will show
it; and the five-second sites are not waiting on JavaScript. Where they
do wait is the next measurement: each load broken into the network,
parsing, style and layout, painting and the hops between JetSki's
processes, before anything is changed. FTL and WebAssembly wait for that,
since they would speed up the same stage that is not the slow one.

## Then

2. **FTL**, on B3.
3. **WebAssembly**, BBQ then OMG, with its bounds checked in code, since
   Kosmos has no signals to catch an access past them.

Each one measured on the M700 against the one before, and kept only if it
pays.
