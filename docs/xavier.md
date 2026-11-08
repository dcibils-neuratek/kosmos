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
feature set and architecture are here. The mockup (`xavier.html`) and the
diagram (`xavier-architecture.png`) follow once the decisions at the end
are taken, because the diagram is what the first decision draws.

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

1. **X1 - the LLM Kit on the Mac.** GGUF, the tokenizer, phi-2's forward
   pass in scalar C. The host test is against llama.cpp built on the Mac:
   the same tokens, and the same greedy answer to a fixed question.
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
9. **X9 - speech.** Whisper (tiny or base, English) on the LLM Kit's tensor
   code, and eSpeak NG out. Hold a key, speak, hear the answer.
10. **X10 - doing things.** Xavier proposes an Automations step, shows it,
    and runs it when confirmed. After Automations is built, and with a
    model that is good at it, which phi-2 is not.

## Decisions that are Diego's

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
