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
the Xavier app"); the diagram (`xavier-architecture.png`) follows the
decisions still open at the end.

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

- **The model is a region**, read from `/Home/Models/<name>.gguf` once
  when the server starts and mapped into the GPU's buffers from there.
  About 3 seconds at `diskfs`'s 500 MB/s.
- **The answer is a stream.** The words come because a clock is running,
  not because someone asked for each, so they go into a single-producer,
  single-consumer ring in a region the server shares with the window, and
  a message says only how far it has written. The same holds for the
  microphone's samples and the speech going out.
- **A question is one message** (a declared shape in `llmproto.h`): the
  conversation's handle, where its text starts in the shared region, and
  how long it is. Stop is a message.

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
   them to.
3. **X3 - an answer on Kosmos.** The LLM server, the model from
   `/Home/Models`, and `xavier "question"` at the prompt printing the
   answer. Then its speed on the M700, measured.
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
11. **X10 - doing things.** Xavier proposes an Automations step, shows it,
    and runs it when confirmed. After Automations is built, and with a
    model that is good at it, which phi-2 is not.

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

## Decisions that are Diego's

**Taken on 8 November 2026** - Diego: "E1, server yes, convert phi-2, go
ahead". So: the engine is our own LLM Kit in C (1), one server holds the
model for every application (2), and phi-2 is converted on the Mac with
llama.cpp's converter, at Q8_0 and Q4_0 (3). The kernel's AVX2 (4) and
the voice (5) are still open, and neither is needed before X6 and X9.

1. **The engine**: E1, our own LLM Kit in C *(recommended)*, or E2,
   llama.cpp with a C++ runtime ported.
2. **A server holding the model** *(recommended)*, or the kit inside
   Xavier's process only, until a second application wants it.
3. **The model file**: phi-2 converted to GGUF on the Mac with llama.cpp's
   own converter from the Hugging Face weights, at Q4_0 (1.6 GB) and Q8_0
   (2.9 GB). Where it lives on the M700 is a question too: the stick's
   `/Home` is 512 MB, so it needs a larger `/Home` or a partition of its
   own.
4. **AVX2 through `xsave`** in the kernel (X6), and when.
5. **The voice**: eSpeak NG first, clear and robotic. A neural voice
   (Piper) runs on ONNX, which would be a second engine to bring, so it
   is later if at all.
