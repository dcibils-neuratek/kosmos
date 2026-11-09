# Xavier

An assistant that answers in a window and aloud, run on this machine.
Diego, 8 November 2026: "i want to add an llm powered assistant called
Xavier", "a chat and audio interface to chat with Xavier to general
questions and answers and in the following versions, do stuff in the OS
like write an email, open settings, open files and else", "for now we will
use this small model https://huggingface.co/microsoft/phi-2", "its small
enought to run on CPU but my plan is to be able to run a lot of it on the
available GPU". And on the engine: "we can use other llm engine if needed",
"not just llama.cpp".

**A plan, written before anything is built, for Diego to decide.** Its
feature set and architecture are here. The mockup is `xavier.html`, drawn
on 8 November when Diego asked to see it ("I need to see the mockup of
the Xavier app"), and approved ("mockup is great"); the diagram is
`xavier-architecture.png`, drawn from `xavier-architecture.html` after X3,
with the steps still to come dashed.

## What a person can do

**Version 1, questions and answers:**

- **Ask in a window.** Type a question, and the answer appears a word at a
  time as Xavier writes it. Stop it mid-answer. Ask a follow-up that
  remembers the conversation so far, up to what the model can hold.
- **Ask aloud.** Hold a key, speak, let go. Xavier hears it, shows what it
  heard as the question, answers, and reads the answer out. Silence
  during it is a button.
- **Keep conversations.** Each one is a file in `/Home/Xavier`, listed in
  the window's sidebar, opened again where it was left, and deleted to the
  Trash like anything else.
- **See what it costs.** Words a second, how long the question took to
  read, and where it ran - processor or GPU - in the window's foot.
- **Ask from anywhere.** A system key opens Xavier over whatever is in
  front, as the launcher pad opens.

**Models from Hugging Face, with a tool** - Diego, the same day: "we need
a way to load models from huggingface easily", "with a tool":

- **`models search phi`** - the models on Hugging Face that come as GGUF,
  with their sizes and quantisations, from its public API.
- **`models get <repo> [file]`** - a model into `/Home/Models`, resumed if
  it was cut off, and held to the SHA-256 Hugging Face lists for the file
  before it is used.
- **`models`** - what is in `/Home/Models`, how large, and which one the
  server has loaded; **`models load <name>`** and **`models rm <name>`**.
- **A model that is not published as GGUF** - phi-2 is one, safetensors
  only - **is converted on the Mac**: `tools/hfmodel.py microsoft/phi-2
  Q4_0` downloads it, runs llama.cpp's converter and quantiser, and puts
  the file where a Kosmos disk or the M700 can take it. Converting needs
  PyTorch, which is not coming to Kosmos.
- Later, a Models page in Xavier's window, drawn in its mockup.

**What the engine is doing, to be seen** - Diego, the same day: "a way yo
see LLM inference engine metrics like models loaded, usage, tokens per
second, etc":

- **The models loaded**: name, file, size, quantisation, memory held, and
  where it runs - processor, GPU, or both.
- **Its use**: who is asking (each application by name), the requests
  waiting and running, the context each conversation holds.
- **Its speed**: tokens a second reading the question and writing the
  answer, for the last request and over the last minute, and the time to
  the first word.
- **`llm` at the prompt** prints it, and a window shows it live, as GPU
  Monitor shows the GPU - drawn first, as every window is.

**Light coding, as Claude Code does it** - Diego, 8 November 2026: "It's
also key that we can do some light coding with Xavier like Claude code":

- **Ask about code.** "What does this function do", "why does this fail",
  with the file or the selection in the IDE handed over as the context.
- **Have it write or change code.** Xavier reads the files it is pointed
  at, proposes an edit, and **shows it as a diff to accept or refuse**,
  file by file - nothing is written that a person has not seen.
- **Have it run what it wrote.** A Lua program or a test, run as the IDE
  runs one, its output read back so Xavier can fix what failed - each run
  shown and confirmed first, as every action is.
- **Only what it is handed.** Xavier reaches the project folder it was
  opened on and nothing else: a capability, not a convention.
- **A model for code.** phi-2 writes a little code but holds 2048 tokens,
  too few to read a file and change it. Qwen2.5-Coder at 1.5B or 3B
  (Apache 2.0, published as GGUF, 32K tokens) fits the M700 and the Mac
  Pro, and is trained to call tools. It is a second architecture (Qwen2:
  RMSNorm, SwiGLU, grouped-query attention), so the LLM Kit grows a
  second forward pass beside phi-2's, held to llama.cpp the same way.

`xavier.html`, *Light coding*, draws it: the conversation beside the
change, each file's diff with Accept and Refuse, and the run asked for on
its own.

**Xavier uses the computer** - Diego, 8 November 2026: "It's key that
Xavier can use the computer to do tasks so kosmos needs to expose all
available methods to use the computer to Xavier". So **every way a person
can use Kosmos is a way Xavier can**, through one door, and the door is the
system's rather than Xavier's (*Using the computer*, below).

**Later versions, Xavier does things:** writes a mail in Mail, opens a
page of Preferences, finds and opens a file, starts an application.
**Every action is shown and confirmed before it happens**, and is said as
the Automations step it would run (below), so what Xavier can do is
exactly what a person could automate, no more.

## Architecture

### Who supplies each piece

The premise (*kits supply, applications orchestrate*) asked of every
piece: does another application want this? Most of Xavier is yes.

| Piece | Supplied by | New? |
|---|---|---|
| Reading a model file (GGUF), the tokenizer, the forward pass, sampling | **the LLM Kit** (`user/kits/llm/`, `/Kosmos/Kits/llm`), C | **new**. Write's "rewrite this", Mail's "draft a reply" and the IDE want it too |
| The model, loaded once and shared | **an LLM server**, `/Running/llm`, C | **new**. 1.6 GB loaded once for every application, not once each, on an 8 GB machine |
| Matrix work on the GPU | **the Compute Kit**, with new kernels in `eukernels.lua` | kernels new, the kit exists (`docs/compute.md`) |
| Several cores on one answer | **threads in a process** (`docs/threads.md`, steps 4 to 8) | the steps are planned; 0 to 3 are done |
| The wide vector registers (AVX2, FMA) | **Nebula's lazy FP save**, which saves only SSE today (`fxsave`) | **new**: `xsave` in `arch/x86_64/` |
| The microphone | **the audio server**, a capture ring beside the playback ring | **new**. Nothing in Kosmos records sound yet. A recorder, the Camera's sound, a call want it |
| Speech to text | **the Speech Kit**: Whisper on the LLM Kit's tensor code | **new**. A dictation key in every text field wants it |
| Text to speech | **the Speech Kit**: eSpeak NG first, vendored C | **new**. Reading a page or a notification aloud wants it |
| Downloading a model into a file | **the Network Kit**: an HTTPS download streamed into a file in C, with ranges to resume, and a running SHA-256 from the **Crypto Kit** | **new**. Packages and the browser's downloads want it; `fetch.lua` holds a reply in a Lua string, which is no way to carry 1.6 GB |
| Finding models | **`models.lua`**, a program, asking Hugging Face's API with `http.lua` | **new** |
| Converting one | **`tools/hfmodel.py`**, on the Mac, around llama.cpp's own converter | **new**, a host tool |
| The engine's numbers | **the LLM server**, answering `stats` in a declared shape (`llmproto.h`) | **new**. `llm` at the prompt and the monitor window read the same reply |
| The window, the conversation, the actions | **`xavier.lua`**, an application in Lua | the only part that is Xavier's |
| Doing things in the OS | **Automations' steps** (`docs/automations.md`) | Automations is designed and not built yet |

### The engine: which one

**The GPU part is ours whichever engine is chosen.** llama.cpp reaches a
GPU through CUDA, Metal, Vulkan, SYCL or OpenCL, and Kosmos has none of
them. Its GPU is the Compute Kit: kernels in Gen9 assembly, assembled by
`eu.lua`. So "most of it on the GPU" means writing the matrix kernels as
Compute Kit kernels, for any engine.

- **E1 - our own engine in C, the LLM Kit** *(recommended)*. A GGUF
  reader, a byte-level BPE tokenizer, phi-2's forward pass, and the
  quantised matrix-vector product, in the shape of Karpathy's `llama2.c`
  (MIT, one C file). The block formats are taken from ggml's
  `ggml-quants.c`, which is plain C, so the bytes of a GGUF file are read
  exactly as llama.cpp reads them. About two to three thousand lines. Held
  on the Mac to llama.cpp itself by a host test: the same tokens for a
  text, and the same next token for a prompt, step after step.
  For: no C++ runtime; every line readable here; vector types for SSE2
  and AVX2 as `gfx.c` is written; a GPU backend shaped for the Compute
  Kit; and the same tensor code serves Whisper. Against: each new model
  architecture is code we write. phi-2 is one, and the Llama family (Phi-3,
  Qwen, Mistral and most of what is published) is one more.
- **E2 - llama.cpp and ggml, ported.** Every architecture llama.cpp knows,
  and whisper.cpp nearly free. But it is C++17: it needs a C++ runtime on
  Kosmos's libc, with exceptions, `std::thread` over our threads, `std::regex`
  (its tokenizer uses it), streams and files. That would be the largest
  port in the tree. And the GPU backend is still ours to write, as a ggml
  backend.
- **E3 - another small C engine.** There are many, and Diego linked one:
  ByteSized, "a 3-kilobyte inference engine" in C. The article gives no
  repository, licence, weight format, threads, GPU or speeds, so it is not
  an engine phi-2 could run on. It does show what E1 bets on: inference
  needs far less code than llama.cpp's size suggests. `llama2.c` is the
  better-known one, and E1 starts from it.

### phi-2, as it is

- **2.7 billion parameters**, MIT licensed, 2048 tokens of context, its
  vocabulary GPT-2's (51,200 tokens).
- **A base model, not a chat model.** Microsoft trained it on text and code
  and did not tune it to converse. It answers well in its own format,
  `Instruct: <question>` then `Output:`, and it rambles past the answer
  unless stopped. Xavier v1 uses that format and stops at a blank line.
  The engine reads any GGUF in a supported architecture, so a chat model
  is a file to download later, not a rewrite.
- **Its shape**: 32 layers, 2560 wide, 32 heads; the attention and the
  MLP in parallel off one LayerNorm; rotary positions on 32 of each head's
  80 dimensions; GELU; biases throughout. In GGUF at 4 bits (Q4_0) it is
  about 1.6 GB, at 8 bits (Q8_0) about 2.9 GB.

### The busiest path, and what the GPU can buy

**Writing an answer reads every weight once per word.** At 4 bits that is
about 1.6 GB per token. The M700's memory is two channels of DDR4, at
most about 34 GB/s, so **no engine on that machine writes much more than
20 tokens a second**, whatever runs the arithmetic. An estimate to be
measured, not a figure.

**The GPU is in the same memory.** The HD Graphics 530 is on the
processor's die and reads the same DDR4 at the same rate. So for writing
an answer, the GPU's 24 EUs are not much faster than four cores that keep
the memory busy (with AVX2 and threads). **Where the GPU pays is reading
the question**: a prompt of 500 tokens is about 2.8 trillion operations,
done at once as a matrix-matrix product, which is arithmetic rather than
memory, and that is the work for EUs. So the GPU kernels come in this
order: the prompt (matrix-matrix) first, then the answer (matrix-vector).

**On the processor today it would be slow**: one core, SSE2 only. AVX2
with FMA does twice SSE2's work per instruction, and four cores do four
times one core's. Both are steps below, and both are measured on the M700
when they land.

### Data, regions and messages

- **The model is a region**, read from `/Home/Models/<name>` when
  something asks for it (`LLM_OP_LOAD`), four megabytes at a time through
  the disk server's own door to that folder, and later mapped into the
  GPU's buffers from there. About 3 seconds at `diskfs`'s 500 MB/s.
- **The answer is a stream.** The words come because a clock is running,
  not because someone asked for each, so they go into a region the server
  makes for the answer and hands back with the reply to ASK
  (`llmring.h`): append only - one side writes, one side reads, and
  `written` is the one number both touch, which only grows - so there is
  no read index to hand back and nothing to wrap. WAIT is a message held
  by the server until `written` has moved. The same will hold for the
  microphone's samples and the speech going out.
- **A question is one message** (a declared shape in `llmproto.h`): up
  to 1536 bytes of text, the most tokens to answer with, and whether to
  stop at a blank line. A one-shot payload, asked because a person asked,
  which is what a message is for. A conversation longer than that is X4's,
  and moves into a region with it. Stop is a message.

### C or Lua

The kit, the server, the speech kit and the capture ring are C: loops over
bytes and numbers, and work on another program's behalf. Xavier's window
is Lua. No pixel, sample or weight goes through a Lua table.

## The steps

Each step has its own test before the next starts, and the host does the
first two without booting anything.

1. **X1 - the LLM Kit on the Mac. Built, 8 November 2026** (*X1, as
   built*, below). GGUF, the tokenizer, phi-2's forward pass in scalar C.
   The host test is against llama.cpp built on the Mac: the same tokens,
   and its answers - within what llama.cpp gives against itself, which is
   not the same answer token for token.
2. **X2 - vectors.** Q4_0 and Q8_0 matrix-vector products in vector types,
   SSE2, with the scalar loop kept as the reference the host test holds
   them to. *Under way* (*X2, as it stands*): the lanes and their test are
   written; which form wins is measured on the M700.
3. **X3 - an answer on Kosmos. Built under QEMU, 8 November 2026** (*X3,
   as built*, below). The LLM server, the model from `/Home/Models`, and
   `xavier "question"` at the prompt printing the answer. Its speed on the
   M700 is still to be measured.
4. **X4 - the window. Built, 0.11.322.** Chat, streaming, stop,
   conversations kept, drawn first in `xavier.html` and agreed.
5. **X5 - every core. Built, 0.11.324** (*Every core, as built*), with the
   question read in batches (0.11.323).
6. **X6 - AVX2. Built, 0.11.325** (*AVX2, as built*): XSAVE in Nebula's
   switch, then the kit's loops widened.
6b. **Settings. Built, 0.11.326** (*Settings*, below; drawn in
   `xavier.html`): cores, vector loops and the GPU, the LLM server's for
   every application, and Compare. And **a model chosen for each
   conversation. Built, 0.11.328** (*A model for each conversation*,
   below), drawn first the same day.
7. **X7 - the GPU.** Compute Kit kernels for the prompt, then for the
   answer, each measured against the processor on the M700. Brought
   forward on 8 November, after Settings.
8. **X8 - the microphone.** HDA capture in the audio server, the ring, and
   the Realtek ALC on the M700 and the T14.
9. **X8b - models and metrics.** The download into a file in C,
   `models` against Hugging Face, `tools/hfmodel.py`, the server's `stats`,
   `llm` at the prompt, and the monitor window, drawn first. `stats` comes
   with X3, because the speed of X3 is its first number.
10. **X9 - speech.** Whisper (tiny or base, English) on the LLM Kit's tensor
   code, and eSpeak NG out. Hold a key, speak, hear the answer.
11. **X10 - using the computer.** Every method Kosmos has, in one catalogue
    (*Using the computer*): the system's own, applications' controls by
    name, the screen last. Xavier proposes a step, shows it, and runs it
    when confirmed. After Automations is built, and with a
    model that is good at it, which phi-2 is not.
12. **X11 - light coding.** Qwen2.5-Coder in the LLM Kit (a second
   forward pass, held to llama.cpp by the tiny-model test's way), the tool
   calls - read a file, propose an edit, run a program - each shown and
   confirmed, and Xavier in the IDE on the project it was opened on. Drawn
   in `xavier.html` before it is built.

## Using the computer

**One door, which Kosmos supplies and Xavier is a user of.** Automations
already designs it: the UI kit publishes **every control by name** - "the
New mail button", "the To field", "the selected row" - so that every
application is scriptable the day it is written, and `automations.md`
names "an AI agent driving Kosmos" as one of the door's users. Xavier is
that user. Nothing in Kosmos is written for Xavier alone; an application
that adds a button has given Xavier the button.

**Three levels, and Xavier takes the highest that does the task:**

1. **What the system supplies, as itself.** Files through the namespace
   (read, write, find, the Trash), programs at the prompt with their
   output, servers through their declared protocols, settings as the
   files Preferences writes. Exact, fast, and the same for a script.
2. **Applications by their controls' names.** Automations' steps: open
   Mail, press "New mail", type into "To", choose View > Zoom. Exact too,
   because a name does not move when a window does.
3. **The screen, when nothing above reaches** - built already, for VNC,
   and Xavier's as it stands. The window manager's `/Running/wm/remote`
   lends the screen (`watch` and `watched`: each composed frame copied
   into a region the borrower shares, with the rectangles that changed -
   `wmproto.screen()`, which `screenshot` uses too) and takes a pointer
   and keys (`pointer` {x, y, buttons}, `key` {code, chars}) the desktop
   cannot tell from a real mouse and keyboard. It is mounted only in a
   program the desktop launches whose header says `kosmos: needs
   desktop`, so Xavier is lent it as `vncd` is, and nothing else can
   drive the screen at all. While Xavier drives, the desktop shows the
   banner `vncd` shows for a viewer allowed the keys and the pointer.
   Last, because a click at a place lands on something else once a window
   moves - and because seeing the picture needs a model that can see,
   which phi-2 cannot. **That model is Qwen's** - Diego, 8 November 2026:
   "Go with qwen vision". Qwen2.5-VL-3B, or one of the 2B models tuned on
   Qwen2-VL to operate interfaces (ShowUI, UI-TARS), chosen from Hugging
   Face when X10 starts: they read the text on a screen and answer with
   the place of what was asked for. Their language half is Qwen2, the
   architecture X11 brings for Qwen2.5-Coder, so vision adds the image
   encoder (a ViT) in front of a forward pass the kit already has, not a
   third one. A whole 1920x1080 screen is two to three thousand image
   tokens, so a window is cropped to before it is read, and this is where
   the GPUs pay most.

**One catalogue the model reads.** Every method above is described in one
declared shape - its name, what it does, its arguments, and whether it
changes anything - generated from what exists: the programs in
`/Kosmos/Programs`, each running application's published controls, the
protocols' operations. The model is told the catalogue and asks for a
step by name; the server never parses free text into an action.

**Only what it is handed, and nothing without a yes.** Xavier is a
process with a namespace like any other: what it was not handed, it
cannot reach. Within that, a step that only reads (a file, a window's
title, the screen) runs and is shown; a step that changes anything is
shown as the Automations step it is and waits for yes; sending, deleting
and paying ask twice. Every step taken is written to the conversation's
file, so what Xavier did is always on the record.

**What it waits on**: Automations' first step, the UI kit publishing every
control - which is the larger piece, and Automations' own (`automations.md`,
*Publishing every control*); a model good at calling tools, which phi-2 is
not (Qwen2.5 Instruct at 3B fits the M700, and the Mac Pro's 32 GB and two
cards could hold 7B or more); and X10, which this widens from "Xavier
proposes an Automations step" to every method there is.

## X1, as built

`user/kits/llm/llm_core.c`, about 1,300 lines with its comments, and
`llm_core.h`. C that compiles for the Mac and for Kosmos alike, takes no
memory of its own after `llm_open` - the file is read where it lies, and
the tokenizer's tables, the F16 attention cache and the working vectors
come from the caller's allocator once - and trusts nothing in the file:
every count, length, offset and type is held to the buffer first.

- **GGUF** versions 2 and 3; tensors in F32, F16, Q8_0 and Q4_0.
- **The tokenizer**: GPT-2's byte-level BPE, the pre-tokenizer phi-2's
  file names (GPT-2's own split), user-defined tokens - phi-2's runs of
  spaces - always taken whole, and `<|endoftext|>` only when asked for, as
  llama.cpp's `parse_special`.
- **phi-2's forward pass**: LayerNorm, Q, K and V with their biases, NeoX
  RoPE on 32 of each head's 80 numbers, attention over an F16 cache, and
  the MLP beside it off the same norm, GELU, the output with its bias.
- **llama.cpp's arithmetic, where it decides a rounding**: each input
  quantized to Q8_0 and dotted in integers block by block, q and the
  softmax's weights rounded to F16, GELU through F16, RoPE's angle a
  running product in float, LayerNorm's sums in double.

**The models** are made on the Mac, from Microsoft's safetensors, by
llama.cpp b12205's own tools (`build/downloads/llama.cpp-b12205`):
`convert_hf_to_gguf.py --outtype q8_0` in `build/venv-gguf`, then
`llama-quantize --pure` to Q4_0. `build/models/phi-2.Q8_0.gguf` is 2.96
GB and `phi-2.Q4_0.gguf` 1.57 GB, with their SHA-256 in
`build/models/SHA256SUMS`; the safetensors were held to Hugging Face's
sums before they were converted, and deleted after. X8b makes this a tool.

**The test** is `tools/test_llm.c`, in `make host-check` for both models
(a skip that says so where there is no model), 62 checks each, about 20
seconds for the two. llama.cpp's answers are in the tree
(`tools/llm_reference_phi-2.Q8_0.txt`, `.Q4_0.txt`), written by
`tools/llm_reference.cpp` against libllama - `make llm-reference` - so the
test needs the model and our C, and not llama.cpp:

- every text's tokens, ten of them chosen to be awkward, plain and with
  special tokens - **exactly the same ids**, all twenty;
- one token's ten largest logits within 0.10; each longer prompt's top
  token llama.cpp's or a near-tie, and its ten largest within 2.0;
- twelve answer tokens a prompt, fed llama.cpp's own so the two cannot
  wander apart: at each, the kit's choice llama.cpp's, or both rating the
  other's within 0.5 of their best.

**Why not the same answer token for token: llama.cpp does not give it
against itself.** The first trace put layers 0 and 1 bit for bit on
llama.cpp's and the difference growing from about layer 10, so llama.cpp
was built a second way - plain C, no NEON, no repacking - and the two
compared on the same prompts:

| | one token | 12-token prompt | 7-token prompt | answers |
|---|---|---|---|---|
| llama.cpp plain against its NEON | 0.052 | 0.18 - 0.24 | 0.61 - 0.62 | part at near-ties: Q8_0's prompt 2 at its first token, 14.69 against 14.66 |
| the kit against llama.cpp NEON | 0.028 - 0.052 | 0.20 - 0.45 | 0.34 - 1.36 | 1 near-tie in 72 steps |
| the kit against llama.cpp plain | 0.032 - 0.056 | 0.21 - 0.30 | 0.73 - 0.82 | 1 near-tie |

The order a sum is taken in moves a Q8_0 rounding, and over 32 layers that
grows. The kit is a third order and sits as far from either build as they
sit from each other, so the bounds are about twice llama.cpp's own spread.

**And the bounds bite.** Four copies of the kit, one mistake each, all
fail: GELU without its cubic term is 1.37 out on the first token and the
MLP's down bias left out 1.25, against 0.10; BPE merging the highest rank
first fails every text; and RoPE paired GPT-J's way fails two answer steps
and no logit bound - phi-2 turns only 32 of 80 numbers, none at position
0, on short prompts. That is the thinnest of the checks, and a longer
prompt is the way to thicken it when X2 runs fast enough to afford one.

**The answers**: "The capital of France is Paris." to `Instruct: What is
the capital of France?\nOutput:`, and a Fibonacci body to `def
fibonacci(n):`.

**How fast**, scalar, one thread, on the Mac's M-series core - a reference
for X2, not a figure about Kosmos: **4.1 tokens a second on Q4_0, 9.0 on
Q8_0.** Q4_0 is slower here, against the bandwidth argument above, because
its nibbles cost scalar shifts and masks that Q8_0's bytes do not; that is
exactly the loop X2 puts in vectors.

## X2, as it stands

`user/kits/llm/llm_dot.c`: Q8_0 and Q4_0 rows in GCC's vector types, 16
bytes wide in `pmaddwd`'s shape, beside the scalar loops they are held to,
**to the bit**, by `tools/test_llmdot.c` - natively and under Rosetta, 2352
rows each, every length to 96 blocks and phi-2's 80 and 320, random and the
corners. Bit for bit because only the integer work inside a block is in
lanes; the float sum across blocks stays in order.

**What the Mac showed, and what it cannot.** Measured on phi-2 itself
(`test_llm`), one thread:

| | Q8_0 | Q4_0 |
|---|---|---|
| X1, scalar | 9.1 tok/s | 4.1 |
| explicit vectors, as first written (32 bytes) | 5.8 | 5.8 |
| the scalar loop, moved to `llm_dot.c` | 5.3 | - |
| explicit vectors, 16 bytes, scales from a 64K table | 2.2 | 2.6 |
| the scalar loop, F16 converted the fast way | **9.2** | 4.1 |

- **The time was in the scales, not the multiplying.** A block is two
  `sdot`s; moved out of the file, clang compiled the F16 conversion of its
  two scales branch-free, about fifty instructions. A normal number is now
  three operations with a branch for the rest (`llm_core.h`), and the
  input's scales are converted once a product (`xs`) rather than once a
  row. A 256 KB table was worse: read at random, it fights the weights for
  the cache.
- **On this Mac the plain loop wins**, because clang targets an M1, which
  has `dotprod`, and makes the plain loop `sdot`; it does not see `sdot` in
  the explicit lanes. **That says nothing about Kosmos**: GCC builds
  Kosmos, for an i5-6500T and for ARMv8.0, neither of which has `sdot` -
  and GCC's x86 output for the plain loop is already `pmaddwd`, four of
  them a block. Which form is faster there is measured on the M700, and
  the kit chooses it - which is the run-time choice X2b needs anyway.

## X3, as built

**`/Running/llm`** (`user/servers/llm.c`, role 36): one process, started
by init and idle, holding nothing, until something asks for a model. It
reaches the disk through a fourth door of `diskfs`'s (after `/Home`,
`/Keyring` and `/Home/Cache/Maps`), which reaches `/Home/Models` and
nothing else, so the server can read a model and no other file.

- **The protocol** (`user/include/llmproto.h`): LOAD a model by its file's
  name, never a path; ASK, answered at once with the answer's region; WAIT,
  held until the answer has moved; STOP; STATS. One answer at a time - a
  second ASK is refused as BUSY.
- **Nothing makes a caller wait behind work.** A LOAD's reply is held
  while the file comes in, four megabytes a turn; the answer is made a
  token a turn; between turns the server receives without waiting, and it
  blocks only when there is nothing to do. And it asks the disk nothing
  until somebody asks for a model.
- **The kit's memory is regions**, one for each table it asks for, so
  letting a model go gives back every page - tables, attention cache and
  file - with no heap to fragment between one model and the next. A LOAD
  refused (a name mistyped, a file that is not a model) leaves the model
  that was loaded where it was.
- **`llm.lua`** is the client - `load`, `ask`, an answer's `wait`, `all`
  and `stop`, `stats` - and **`xavier`** at the prompt is its first user:
  phi-2's `Instruct: ... Output:` and the words as they come.

**Held by `x86-llm`** (`tools/run_llm.py`, 45 checks, 33 seconds). The
tiny phi-2 in its three forms goes into `/Home/Models`, and fifteen answers
read out of the server's regions are the kit's own answers on the Mac
(`tools/llm_answer.c`), **byte for byte**. Byte for byte and not
llama.cpp's tokens, because the tiny model's two best tokens are often a
hundredth apart - inside the kit's own error against llama.cpp - while the
kit against itself on another processor differs in the fifth digit. So the
chain is: `test_llm` holds the kit to llama.cpp, and `x86-llm` holds the
server to the kit. Then the refusals, a model kept, a stop, a paragraph
not cut short, and every answer's region given back over thirty answers.
The control - the prompt's first token skipped and no region given back -
fails fourteen checks.

**phi-2 itself, asked in Kosmos** (8 November 2026, a 6 GB QEMU guest
on four emulated cores): `xavier --tokens 10 What is the capital of
France?` answered **"The capital of France is Paris."** - the same words
llama.cpp gives - and ended at eight tokens of the ten it was allowed, at
the end of its line. Whether that end was the blank line or phi-2's own
end-of-text is not something the run says, so the stop at a blank line is
still not proved by a model that writes one; the tiny model's random
weights never do.

| under QEMU (TCG), phi-2 Q4_0 | |
|---|---|
| a load, 1496 MB | 1.8 s |
| the prompt, 12 tokens | 109 s |
| answering | 0.11 tokens a second |

The first try panicked the kernel: the model's region was the first thing
to ask for pages above four gigabytes, which the kernel's window into
physical memory did not reach (`testing.md` 18.714, fixed).

| under QEMU (TCG), the tiny phi-2 Q4_0 | |
|---|---|
| a load, 3.7 MB | 37 ms |
| the first token, after a one-token prompt | 14 ms |
| answering | 86 tokens a second |

Those are QEMU's numbers and say nothing of a real core (`CLAUDE.md`).

**Found on the way.** A diskfs door shut for want of entropy - QEMU with
no `virtio-rng` - is an endpoint nobody receives on, and a caller waits on
it for good; the LLM server did, at start, until it was made to ask the
disk nothing until a model is wanted, and a shut door now answers, refusing (`roadmap.md`). And
on one core the shell's prompt comes before the servers init starts after
it have run at all, so a program typed at once finds no `/Running/llm`;
the suite waits for the server's line.

## Where the models live (8 November 2026)

**Why**: `/Home` on a stick is 2 GB, the size `STICK_HOME_MB` gives it when
a stick is written (8 October, `docs/diskfs.md`, *No limit*), and phi-2 at
Q4_0 is 1.5 GB of it - copied to the M700 over SMB that day, with 295 MB
left. A 7B model at Q8_0 is 7.7 GB. The M700's stick is a 128 GB Kingston
with 88 GB in no partition at all. Three ways were put to Diego: write the
stick again with a larger `/Home`; a drive of the models' own; or `/Home`
grown in place (G2 and G3, dropped on 8 October). Diego: "Let's do option 2".

**What**: `/Models`, a partition of its own, exactly as `/Sandboxes` is for
guests' disks (`docs/linux.md` L1) - so a model of gigabytes fills its own
drive and never `/Home`:

- **its server**: `diskfs.c` a third time, as the role `modelsfs`, on the
  partition `opt/kosmos/models` names by its GUID and nothing else; blank,
  it is formatted with `/Models` at its root. A drive named only by GUID
  is `named` in `diskfs.c` now, which `/Sandboxes` was by a flag of its own;
- **its mount**: the shell mounts `/Models` on the same word, and every
  program is handed it as `/Sandboxes` is - a person puts a model there;
- **the LLM server** is handed that drive where the machine has it, and the
  `/Home/Models` door where it does not - every machine with one disk, which
  is every QEMU suite. init decides by which it hands over, `llm.lua` by the
  same word (`llm.MODELS`), so the two cannot disagree;
- **its blocks** guarded by the USB driver with the other two drives';
- **the Drives list** names it Models, opening at `/Models` - one table of
  the drives a disk server holds now, in `drivelist.lua`, for all three;
- **on a stick in use**: `tools/stick_drive.py --drive models`, which was
  `sandboxes_drive.py` - one tool for either drive, the GPT moved to the
  stick's end, the partition added after the last, `/Home` untouched. 48 GB
  unless `--mb` says.

**And a copy of any size**: a model goes from `/Home/Models` to `/Models`
with `cp`, and `files.copy` refused a file over its 1 MB window, since
`write_from` replaces a file whole. A file past the window is copied a piece
at a time now - read at an offset into the region, written at that offset
through the disk protocol's `DISK_OP_WRITE_AT`, which the namespace had no
door to (`fs.write_from_at`). So `mv` between drives, and Tracker's copy,
work at any size too.

**Tests**: `usb_models` in `x86-usb-2`; `test_stick_drive.py` in `host`
(`testing.md` 18.716).

## All the acceleration there is

Diego's 4, as steps. Each is chosen at run time from what the machine
reports - CPUID's leaves and ARM's ID registers already reach every
process through `sysinfo`, which `/Devices/cpu` reads - and each is held
to the scalar reference by a host test, so the fastest path and the
plainest one give the same answer.

| | M700 (i5-6500T) | Mac Pro 2013 (Xeon E5 v2) | QEMU, ARM |
|---|---|---|---|
| Vector lanes | SSE2, **AVX2** | SSE2, AVX (floats only: no AVX2, no FMA) | NEON; `dotprod` if the core has it |
| F16 scales | F16C | F16C | NEON's `fcvtl` |
| Cores | 4 | 4 to 12 | 4 |
| GPU | Intel HD 530, Gen9, 24 EUs, shares RAM | **two AMD FirePro**, GCN 1.0, 2-6 GB each, 160-264 GB/s | none |

- **X2, the baseline**: SSE2 and NEON, which every x86-64 and ARMv8 core
  has. **Measured first on a real Kosmos core**, not on this Mac: the
  Mac's clang targets an M1 with `dotprod` and turns the plain scalar loop
  into `sdot`, which no Kosmos target compiler does, so the Mac says which
  form clang likes and nothing about the M700 (*X2, as it stands*, below).
- **X2b, AVX2**: Nebula saves the wider registers (`xsave`, `XCR0`), the
  kit compiles its loops a second time with `target("avx2,f16c")` and
  chooses them where CPUID says so. The M700 has it; the Mac Pro does not.
- **Every core**: a product's rows split across the cores. Needs threads
  in a process (`threads.md` 4-8). Decoding is bound by memory, so this
  buys what the memory can feed - on the M700 about two cores' worth, on
  the Mac Pro's four memory channels more.
- **X9, the M700's GPU**: the Compute Kit's EU kernels, reading the
  question's tokens a batch at a time, where the GPU pays most.
- **The Mac Pro's AMD cards** - new, and the largest: GCN 1.0 is
  documented to the instruction (AMD's *Southern Islands* ISA), and
  Linux's `radeon` and `amdgpu` drivers are the rest of the description. A
  driver of our own and a GCN backend for the Compute Kit. phi-2 at Q4_0
  fits in one card's memory, and a token is bound by that memory, so on a
  D700 about ten times the M700's ceiling. **Two cards**: the layers split
  between them, half each, or one card the model and the other the
  desktop. Its own design document before any of it.

## Making it fast (8 November 2026)

**Agreed** - Diego asked "What can we do to make llm work super fast?",
and said "Yes" to the order below. The section above lists every
acceleration there is; this one says which come first, and why.

**What bounds it.** There are two speeds, bounded by different things:

- **Writing the answer is bound by memory.** Every token reads every
  weight once: 1.6 GB for phi-2 at Q4_0. The M700's two channels of DDR4
  give about 25-30 GB/s in practice, so its ceiling is about 15-18 tokens
  a second. That is an estimate from the hardware, to be held to a
  measurement.
- **Reading the question is bound by arithmetic.** Its tokens can share
  one read of the weights, so it is where vector width and a GPU pay. It
  decides how long a person waits before the first word.

**The order:**

1. **Measure on the M700**: X3's first real tok/s, and X2's loops on GCC's
   code rather than the Mac's clang. Every step after is chosen by that
   number.
2. **One push, three parts**, to bring phi-2 near the ceiling:
   - **every core**: a product's rows split across threads (`threads.md`),
     since one core cannot draw the whole of the memory's bandwidth;
   - **AVX2 (X2b)**: `xsave` in Nebula and the loops compiled a second
     time, chosen where CPUID says so;
   - **the question read in batches**: a matrix by a matrix, the weights
     read once a batch rather than once a token.
3. **Weights laid out for the vector unit** when a model is loaded - Q4_0
   blocks interleaved, as llama.cpp's repack does - so the inner loop does
   no shuffling.
4. **Fewer bytes a token**: a smaller Qwen (0.5B or 1.5B, in step with the
   vision models chosen for X7), and **speculative decoding** - a small
   model drafts a few tokens and the large one checks them in one batched
   pass.
5. **The GPUs**: the M700's EUs for reading questions (X9), since they
   share its memory and cannot speed up writing an answer; the Mac Pro's
   two FirePro cards, at 160-264 GB/s, about ten times the M700's
   ceiling, in a document of their own first.

### The question in batches, as built (0.11.323)

`llm_step_batch` puts up to `LLM_BATCH` (32) tokens through the model
together, and `llm_step` is that with one. The server reads a question a
batch per turn, so STOP is still looked for between batches.

**Batching alone bought nothing on this Mac: 1.0x.** It read phi-2 Q4_0 at
about four tokens a second, about 6.5 GB/s of weights, well under what
the Mac's memory delivers. So the scalar loops were bound by arithmetic,
and reading a weight once a batch saved traffic nobody was waiting on.
Q8_0 was also twice as fast as Q4_0, twice its bytes: Q4_0's time was
unpacking its nibbles.

**What paid is a row against four inputs** (`llm_dot4_q8_0`,
`llm_dot4_q4_0` in `llm_dot.c`): a block's weights unpacked and its scale
converted once, then met by four inputs. A question of 52 tokens on the
Mac, scalar and on one core:

| | a token at a time | in batches | |
|---|---|---|---|
| Q4_0 | 12.9 s | 2.56 s | 5.0x |
| Q8_0 | 5.7 s | 2.56 s | 2.2x |

**The answer is the same to the bit.** Each (row, token) dot is the same
integer work and the same float sum in block order. Token *t* attends to
the positions up to its own, and every position's K and V are written
before any token of the batch reads them. `test_llm` holds a 52-token
question, split as 32 + 20 and as 5 + 32 + 15, to the token-at-a-time
read: the logits and every position of the cache, byte for byte.
`test_llmdot` holds the four-input loops to the scalar ones on every row,
natively and through Rosetta. **Control**: the four-input loop given input
0's scales for all four fails 3510 of `test_llmdot`'s rows, and all four
of `test_llm`'s batch checks.

Writing an answer is unchanged, one token at a time. Its turn comes with
threads, where the memory's ceiling is what bounds it.

### Every core, as built (0.11.324)

**The kit cuts and the caller runs.** `llm_parallel` gives the kit a way
to run a job in parts. The kit cuts each product of 256 rows a part or
more into one run of whole rows a part. No sum is split, so the answer is
a single thread's to the bit. The kit calls no thread itself: on the Mac
the test gives it a pthread pool, and in Kosmos the server gives it a
worker a core.

**The server's workers live as long as an answer.** `ask_begin` starts one
on each core (`kosmos_thread_start_on`), and `answer_end` stops them. A
product is about two milliseconds and there are about 130 a token, so a
part is handed over by a counter in memory rather than by a message: the
server's thread adds one to `gen`, and each worker does its rows and adds
one to `finished`. A worker waiting between products gives its core away
with `kosmos_yield`. A server with nothing to answer runs nothing.

On this Mac, four parts, scalar loops:

| phi-2 | reading a 15-token question | writing 12 tokens |
|---|---|---|
| Q4_0 | 1.23 s -> 0.50 s (2.5x) | 4.0 -> 12.5 tokens a second (3.1x) |
| Q8_0 | 0.80 s -> 0.36 s (2.2x) | 9.4 -> 19.9 tokens a second (2.1x) |

These are the Mac's cores and the Mac's memory; the M700's are the number
that matters, and its memory is the ceiling there (*What bounds it*).

**Found on the way, in the harness and in the server** (`testing.md`
18.719). The x86 machine every suite boots has one core, so the pool never
started and `x86-llm` passed without it. The suite now boots four and checks
that the server said it cut across them. With four, the second question
crashed the server: its workers began counting from 0, saw the first
answer's last product at once, and wrote through a pointer to it, long
gone. They now begin from the count as it stands when they are made.

### AVX2, as built (0.11.325)

**Nebula first.** An AVX register is twice an SSE one, and the switch kept
only the SSE half (FXSAVE). Where the processor has XSAVE, Nebula now puts
AVX into XCR0 and switches with `xsave64`/`xrstor64`. A kernel test holds
it: two threads' upper halves survive their preemptions (`testing.md`
18.720).

**Then the kit, a second time.** `llm_dot_avx2.c` is the same four loops
in the same vector types, compiled with `target("avx2")`: a block is one
register where SSE2 needed two. Each byte widens to 16 bits and `pmaddwd`
does the multiplying, which is exact. `pmaddubsw` is quicker and saturates
when both values are -128, so it is not used. FMA is left out of the
target, because the compiler could then round a sum once where the
reference rounds twice. `llm_open` chooses these loops only where CPUID
and XGETBV both say yes, and the answer is the scalar's to the bit either
way.

**Not measured yet where it counts.** Rosetta has no AVX and QEMU's AVX2 is
emulated, so this Mac cannot say what it buys. The M700 can, and it is the
first thing to measure when it is back on the network.

## Settings

**Agreed on 8 November 2026** - Diego: "Can we use gpu for the llm
inference if available? We can have configuration in Xavier app to enable
gpu and cpu features and else", then, after the drawing, "Agreed, server
settings for everyone, go ahead".

**What a person can do**: see what the machine has; choose how many cores
a question is cut across (Auto, one a core, or a number); choose the
vector loops (Auto, AVX2, base vectors, plain) - the same answer to the bit
in each; read questions and write answers on the GPU, when there is one the
server can use; and **Compare**, which asks the loaded model one question in
each setting and shows how fast each read and wrote, then **Use the
fastest**. `llm` at the prompt shows and changes the same settings.

**What it is made of**:

- **The LLM server holds them**, for every application that asks: one
  model, so one place it runs. `LLM_OP_SETTINGS` in `llmproto.h`, a
  declared `struct llm_settings` both ways, and every reply carries the
  settings, what the machine has (processors, cores, the loops, a GPU) and
  what the last answer ran on. A setting the machine cannot run is refused
  as `LLM_ERR_CANNOT` and changes nothing.
- **Kept beside the models**, in `llm.settings` in `/Models` or
  `/Home/Models` - the one folder the server's door reaches - and read when
  the server starts.
- **The kit chooses its loops** (`llm_use_loops`, `llm_loops_have`): one
  place that knows which loops exist and which this processor runs. The
  server applies the settings as each question begins, so a change is in
  force from the next one and never in the middle of an answer.
- **Cores**: the server's workers, as many as asked. Auto is one a core:
  CPUID leaf 0xB says how many processors a core has.
- **Compare is Xavier's**, in Lua: it sets, asks and reads the server's
  numbers between frames, as an answer is read, and puts the settings back
  when it is done. Nothing new in the server for it.
- **The GPU's switches are drawn and off** until the Compute Kit runs the
  model (X7).

No diagram of its own: it is a part of Xavier and of the LLM server, both
already drawn in `xavier-architecture.png`, and adds no kit or server.

**Compare on the M700** (0.11.326, phi-2 at Q4_0, a Core i7-6700: eight
processors on four cores), tokens a second:

```
setting              question   answer
8 cores, AVX2           5.8       5.3    the fastest at writing
4 cores, AVX2           6.2       4.1    Auto
8 cores, base           4.9       4.3
2 cores, AVX2           4.2       3.8
4 cores, base           3.5       3.2
2 cores, base           3.0       2.8
1 core,  plain          1.6       1.6
```

**Auto is not the fastest there, and the guess it was built on was
wrong.** It is one worker a core, on the reasoning that two spinning
workers on one core's two processors fight for it. Writing says otherwise:
an answer is a product per token, a row at a time, and waits on memory
more than on arithmetic - which is what a second processor on a core is
for. Reading a question, which is batched and arithmetic-bound, is the
other way round (6.2 against 5.8). One machine is one measurement, so Auto
stays as it is until a second one says the same; Compare is how a person
finds it on theirs, and Use the fastest keeps it.

**What costs almost nothing, and is felt:** the answer is already written
word by word, and the cache already held across a conversation. The model
can also stay loaded between questions, and the LLM server's band keep the
desktop first while it works.

## A model for each conversation

**Asked on 8 November 2026** - Diego: "xavier needs a way for me to select
the llm model to use in every conversation" - and drawn in `xavier.html`
(*A model for each conversation*). **Built in 0.11.328.**

**What a person can do**: press the model chip in the header and choose
from every model in the models folder, each with its size, the one in
force marked and the one loaded saying so. The choice becomes this
conversation's and the one a new conversation begins with. Opening a
conversation loads its model when it is not the one in, and the box and
the foot say so while it comes - its size, and how long is left from how
fast it has come so far. The list down the side says each conversation's
model, and every answer's note says which model wrote it, so a
conversation that changed model partway shows where.

**What it is made of**: nothing new below Xavier. The LLM server already
let go of one model for another on a `LOAD` (`load_begin` looks for the
file before it lets the old one go, so a name not there costs a refusal
and not the model in). `conversations.lua` keeps the conversation's model,
as it did, and now each turn's: the model's file name at the end of the
`turn` line, read as the conversation's when a file kept before has none.
The model chosen last is `/Home/Xavier/chosen-model`, one name in a file.

**Left out of the drawing, on purpose**: the word on what each model is
like ("smaller and quicker") - nothing here knows it yet, and an invented
one would be read as a fact; and "Get more in Models...", which waits for
Models. A model the server cannot find is not asked for again until it is
chosen, and the conversation is answered by the model in, whose name its
answer then carries.

## Asking from anywhere

**Built in 0.11.329**, as drawn in `xavier.html` (*Asking from anywhere*).
**Super+X** opens a box over whatever is in front: ask with Return, read
the answer as it is written, Escape to go back, Tab to carry the
conversation into Xavier's window. It is Xavier's own - `assistant --ask`,
`user/lib/xavierbox.lua` - so one entry in the launcher, as Shortcuts over
everything is `shortcuts --modal`, and its conversations are the window's:
a new one, on the model chosen last, kept after each answer.

**Every conversation now begins with the day and the machine**
(`conversations.preamble`), which the model cannot know: the day only, so
the start is the same text all day and the server's cache keeps it. The
person's name, which the drawing has, is left out: Kosmos keeps no user's
name anywhere yet. `xavier` at the prompt does not say it.

Left for later: a Xavier window already open is not raised by Tab - a
second one opens on the conversation.

## The LLM Monitor

**What a person can do with it**, as drawn in `xavier.html` (*What the
engine is doing*):

- see which model is in, how large, how much it can hold, and where the
  last answer ran - cores and loops - or how far a load has come;
- see the three speeds that matter: **writing**, tokens a second for an
  answer, with the last minute's average and the best; **reading**, tokens
  a second through a question, and how many were already held; and the
  **first word**, how long a person waited, with the last minute's range;
- watch five minutes of writing as a graph, a column every two seconds,
  gaps between answers included;
- see **who is asking**: every application by name, what it is doing -
  writing, with what its conversation holds, loading the model, or idle
  since when - and the answers, tokens read and tokens written it has had,
  and how often it was refused because another was being answered;
- read the same at the prompt with `llm`.

**What it is made of.** Nothing new but the window: `llmmon.lua`, Lua,
drawn with what GPU Monitor, Network Monitor and Disk Monitor draw with
(`ui.history`, the header, the monitors' dark graph ground), and the LLM
server's `STATS` reply, which `llm` at the prompt and Xavier read too.
**The server counts and the window samples**, as every monitor here does:
it asks every two seconds and takes the difference between two readings,
so the server keeps no history and a window opened late starts its graph
from when it opened. The reply grew from 424 bytes to 912 (`llmproto.h`):
how long the server has been up, every token read and written since it
started, the refusals, and a table of eight askers. All of it is a few
counters bumped where the tokens are already counted; the busiest path is
unchanged, one add a token.

**Who asked is the kernel's word, not the message's.** The server reads
`SYS_SENDER` for each message - the name of the process it came from, as
the notification server does - so an application cannot appear as
another, or as nobody. Processes of one name are one row: Xavier's window
and its Super+X box are both `assistant`, and the window shows the name the
application declares (`kosmos: name Xavier`); `xavier` at the prompt is
"xavier, at the prompt". Past eight names, the one asked longest ago gives
up its row.

**One difference from the drawing, and why.** The drawing has a second
asker *waiting, 1st*: a queue. The server answers one question at a time
and refuses a second as busy, and a queue would mean holding the second
asker's reply until its turn - which stops whatever asked, and every window
asks from its own loop. So refusals are counted by who was refused, and a
queue waits for an ASK that answers at once with "queued", which is its
own piece of work. "Tokens today" is "since the server started", which is
what the server can say without a clock of its own.

**Built in 0.11.331** (`testing.md` 18.725). Left for X7: the Loaded tile
says "processor" or "GPU" when the GPU can answer.

## Models

**What a person can do with it**, at the prompt for now (*What a person
can do*, above): see what is in the models folder, how large, in which
quantisation, which one is loaded and which download was cut short; search
Hugging Face for repositories that publish GGUF; list a repository's files,
each with its size and quantisation and, when the LLM Kit cannot read one,
why; bring a file here, picked up where it stopped if the network dropped,
and held to the SHA-256 Hugging Face publishes before it is given its
name; load it; remove one that is not loaded.

**What it is made of**, and what is new:

- **`models.lua`**, a library - the one door to the models folder and to
  Hugging Face, so `models` at the prompt and the Models page to come
  cannot disagree. It asks Hugging Face's public API, with no account:
  `/api/models?search=...&filter=gguf` for a search,
  `/api/models/<repo>/tree/main` for a repository's files - which carries
  each large file's SHA-256 as its LFS object id - and
  `/<repo>/resolve/main/<file>` for the bytes, which answers with a 302 to
  Hugging Face's store and an address signed for a while. `models.source`
  and `models.name` put a mirror, or a test's server, in its place.
- **`download.lua` and the Network Kit's `download`** - new, and a kit
  rather than part of Models, because Packages and the browser's downloads
  want the same: **a file of any size from HTTP or HTTPS onto a disk, with
  no byte of it through Lua.** The TLS Kit's Lua door hands ciphertext to
  Lua as strings and `http.lua` holds a reply in one, which is right for a
  page and wrong for 1.6 GB. So the download is C, built from what the
  map tiles are built from - `netclient`, `tls_core`, `httpc` - and
  `httpc` learnt to stream a 200's or a 206's body into a sink that says
  how much room it has, with `Range: bytes=N-` and `Accept-Encoding:
  identity`; a redirect or a refusal is still held whole, it being small.
- **The bytes go through a region**, 4 MB, made once: the connection fills
  it, `fs.write_from_at` writes it at its offset, and the Crypto Kit's new
  `sha256_begin()` sums it in place - `update(at, n)` takes the region's
  address, not a string. Lua sees how many bytes, never which.
- **The capability is the namespace's**, as for `ns.connect`:
  `fs.download("/Network", spec)` - the program asks the namespace, which
  holds the network, and the kit gets the capability from there.

**Cut, resumed, checked.** A download goes into `<file>.part`. Run again,
the bytes already there are summed first, then the rest asked for with a
Range; a server that answers 200 instead is taken from the start. Only
when the whole file is the size and the sum the repository lists is it
renamed, so a file in the folder without `.part` is one that checked out.
A whole file whose sum is wrong is removed and said so - going on would
resume nothing and fail the same way.

**The busiest path** is a model coming in: per 4 MB, the network's reads
into the region, one write of it at its offset, and a SHA-256 over it -
all C, one Lua call each. A quantisation the kit does not read (Q4_K_M,
the commonest on Hugging Face) is listed with why, and not refused by the
listing, so a person can still see it is there.

**Built in 0.11.332** (`testing.md` 18.726). Left: the Models page in
Xavier's window, as drawn; `tools/hfmodel.py`, for a model that is not
published as GGUF; and the quantisations the kit does not yet read.

## X7, the GPU: the design (9 November 2026)

**What a person sees**: Settings' GPU switch, drawn off since 0.11.326,
turns on where the machine has the HD 530; the LLM Monitor's Loaded tile
says "GPU" when it answered there; and Compare runs a question on both and
shows the two speeds. The answer is the same word for word, on either.

**The same answer, by construction.** The processor's Q4_0 product is, for
a row, `sum += (float)s * (sw * xs[b])` block after block - `s` the
integer dot of 32 nibbles less eight with 32 bytes of the input, exact;
`sw` the block's scale, a half-precision float made single. The kernel does
exactly that, in that order, one lane a row: `kc` rounds as C rounds and
fuses nothing, and a half-precision float made single is exact (made from
its bits, not by `exp2`, which is the GPU's own and close). So every
product is the processor's to the bit, and so is every token. The host
test holds it there: the tiny phi-2's answers through `tools/eu_emu.c`
against the processor's, word for word.

**What it is made of:**

| Piece | Supplied by | New or as it is |
|---|---|---|
| The kernel: rows of Q4_0 (and Q8_0) times inputs in Q8_0 | `llmgpu.lua`, in Kosmos's kernel language; compiled to the GPU's bytes on the Mac when the server is built, since the server is C and `kc.lua` is Lua | **new** |
| `asfloat`, a float from its bits | `kc.lua` | **new**, one built-in |
| The model in the GPU's table | the driver's `GPU_OP_BUFFER_IMPORT`, given **a run of a region's pages** rather than all of them, so the model is imported a layer a buffer without mapping 1.6 GB once per buffer; and **its GTT slots given back** when a buffer is forgotten - today they are emptied and never handed out again, so a second model would run the 4 GB table out | **changed** |
| A product sent, waited for, read | the Compute Kit's C core, `compute_pack_run`, from the LLM server - a server asking the GPU's driver as the window server does | as it is |
| Where the kit chooses | `llm_core.c`: a product hook, `m->gpu`, tried before the processor's rows, which says no for anything it cannot do (an odd number of blocks, a type it has no kernel for, no GPU) | **new**, small |
| The switch, the tile, Compare | Settings, the Monitor, `llmproto.h`'s `gpu_have` and `ran_gpu`, there since 0.11.326 | as it is |

**Data, regions and messages.** The weights stay where they are - in the
LLM server's region, read from the file - and the GPU reads them there:
each layer's pages imported once, at load. The input, quantised by the
processor as now, goes into a small buffer of the server's (Q8_0's 34-byte
blocks become 32 bytes of values and a float of scale, so every read is a
whole dword); the output is a buffer of floats. A product is one
`GPU_OP_RUN` message and a fence; no number travels in it.

**The layout the kernel reads.** A Q4_0 block is 18 bytes, so only every
other one starts on a dword, and the data port reads dwords. Two blocks
are 36 bytes - nine dwords - so the kernel takes them in pairs, which needs
an even number of blocks to a row: phi-2's 2560 and 10240 columns are 80
and 320 blocks, and the tiny model's are even too. A row that is not goes
to the processor.

**The busiest path**, writing an answer: per token, about 200 products -
each one message and one wait - and 1.6 GB read by the EUs. Memory bounds
it as it bounds the processor (*The busiest path, and what the GPU can
buy*, above), so the first kernel - a lane a row, sixteen rows a thread -
is measured against four cores with AVX2 rather than assumed faster. Where
the GPU should pay is reading the question: the same kernel over a batch
of inputs reads each row once for all of them, as `dot4` does on the
processor.

**Steps:**

1. **G1 - the kernel**: `asfloat` in `kc`; `llmgpu.lua`'s `q4_0` for Q4_0; held in
   the emulator to `llm_dot_q4_0` bit for bit on random rows, odd scales
   and subnormal ones included, with a control. **Built in 0.11.333** (`user/lib/llmgpu.lua`; `testing.md` 18.727).
2. **G2 - the driver**: an import of a run of pages, and GTT slots given
   back and used again; `test_intelgpu_*` on the Mac. **Built in 0.11.334**
   (`igpu_slots` in `intelgpu_decode.c`; `testing.md` 18.728).
3. **G3 - the kit and the server**: the hook, the layers imported at load,
   the switch on; the tiny model's answers through the emulator on the Mac
   word for word with the processor's; under QEMU, no GPU, said so.
   **Built in 0.11.334** (`llm_offload` in `llm_core.c`, `llm_gpu.c`, the
   server's device; `test_llm --gpu`; the switches in Xavier's Settings and
   `llm set gpu` at the prompt; `testing.md` 18.728).
4. **G4 - the M700**: the same answers on the HD 530, then the speeds -
   writing and reading - beside four cores with AVX2, in Compare and the
   Monitor. Needs a netboot. **Done on 9 November (0.11.335)**: phi-2's
   answer word for word on the HD 530, and 4.51 tokens a second against
   the processor's 4.65 on four cores and 5.44 on eight, on one core
   (`testing.md` 18.729, every step from 0.65 measured).
5. **G5 - faster**: a batch of inputs a run for the question; rows split
   across a thread's lanes with a sum at the end if a lane a row starves
   the memory; Q8_0's kernel. **Begun in 0.11.335**: the weight laid out
   for the GPU in tiles of 16 rows, a pair's inputs read once for every
   lane (`lanev` in `kc`), the sums in cached memory, GELU from a table.
   **Next**: each product's rows split between the GPU and the processor,
   both reading at once; the pool's workers waiting without spinning.

## Decisions that are Diego's

**Taken on 8 November 2026** - Diego: "E1, server yes, convert phi-2, go
ahead". So: the engine is our own LLM Kit in C (1), one server holds the
model for every application (2), and phi-2 is converted on the Mac with
llama.cpp's converter, at Q8_0 and Q4_0 (3). The voice (5) is still
open.

**And 4, the same day, wider than it was asked** - Diego, during X2: "Make
sure we make the most performant llm inference using all available
acceleration", "for the hardware you are running on every time", and,
about the Mac Pro he boots Kosmos on next, "we have amd gpus", "dual". So
the kit uses **every acceleration the machine it is running on has,
chosen when it runs** - SSE2 and NEON always, AVX2 where the processor has
it (with `xsave` in Nebula to keep its registers), ARM's `dotprod` where
that has it, every core once threads exist, and the GPUs: Intel's EUs on
the M700, and **both** of the Mac Pro's AMD cards (*All the acceleration
there is*, below). Asked whether it should be assembly: not by default -
one C source compiled once a target (`__attribute__((target))`) and
chosen at run time, with assembly only for the GPU kernels, which have no
compiler here, and for a CPU loop a measurement shows the compiler cannot
express.

1. **The engine**: E1, our own LLM Kit in C *(recommended)*, or E2,
   llama.cpp with a C++ runtime ported.
2. **A server holding the model** *(recommended)*, or the kit inside
   Xavier's process only, until a second application wants it.
3. **The model file**: phi-2 converted to GGUF on the Mac with llama.cpp's
   own converter from the Hugging Face weights, at Q4_0 (1.6 GB) and Q8_0
   (2.9 GB). Where it lives on the M700 is a question too: the stick's
   `/Home` is 512 MB, so it needs a larger `/Home` or a partition of its
   own. *Taken: a partition of its own, `/Models` (8 November; *Where the
   models live*).*
4. **AVX2 through `xsave`** in the kernel (X6), and when. *Taken: yes,
   and every other acceleration too, chosen at run time.*
5. **The voice**: eSpeak NG first, clear and robotic. A neural voice
   (Piper) runs on ONNX, which would be a second engine to bring, so it
   is later if at all.
