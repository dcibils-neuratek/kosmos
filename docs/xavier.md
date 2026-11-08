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
4. **X4 - the window.** Chat, streaming, stop, conversations kept, drawn
   first in `xavier.html` and agreed.
5. **X5 - four cores.** After `threads.md` steps 4 to 8: the products split
   by rows across threads.
6. **X6 - AVX2.** `xsave` in Nebula's lazy FP save, then the kernels
   widened. A change to `arch/`, and Diego's to agree.
7. **X7 - the GPU.** Compute Kit kernels for the prompt, then for the
   answer, each measured against the processor on the M700.
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
   own.
4. **AVX2 through `xsave`** in the kernel (X6), and when. *Taken: yes,
   and every other acceleration too, chosen at run time.*
5. **The voice**: eSpeak NG first, clear and robotic. A neural voice
   (Piper) runs on ONNX, which would be a second engine to bring, so it
   is later if at all.
