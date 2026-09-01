# The Kosmos Book — outline

Laid out the way the Be Book is: areas rather than a tutorial, each one
readable on its own, and ordered so that reading front to back also works.

Chapters are `NN-name.md` in this folder and are composed into one PDF.

Diagrams are ASCII inside a fenced block, the way `architecture.md` already
does it. They survive every format the text might end up in, they diff
cleanly, and a picture that has to be redrawn in a separate tool is a
picture that goes stale the first time the design moves.

**What this is not.** `docs/` is for somebody about to change the code:
terse, current, and it assumes you have the tree open. This is for somebody
learning how the thing works, read in order. The documents are the source
material and not the draft — a chapter that reads like `design.md` with the
headings changed has failed.

**How it gets written.** As we go, and mostly *after* the thing it describes
works. A chapter written before the code is a chapter describing what was
intended, and this project has already changed its mind in public several
times — that is the interesting part and it only survives if the writing
follows the building.

---

## Part I — Why

| | Chapter | Sources that already exist |
|---|---|---|
| 01 | What Kosmos is, and who it is for - **written** | `design.md` §1, §15 |
| 02 | Thirty years of taking notes | *new* — the systems tried, what each taught |
| 03 | Learning by doing: the method | `CLAUDE.md`, `testing.md` §18.7 |
| 04 | The principles, and why they are not laws | `CLAUDE.md` Principles |

## Part II — The machine

| | Chapter | Sources |
|---|---|---|
| 05 | AArch64 in the parts that matter | `state.md` decisions |
| 06 | The boot, narrated | `boot.c`, `main.c` |
| 07 | `arch/` and `hal/`: which CPU, which board | `hal.md` |

## Part III — The kernel

| | Chapter | Sources |
|---|---|---|
| 08 | Why a microkernel, and what it refuses to know | `design.md` §2, §4.1 |
| 09 | Threads, address spaces, and no allocator | `design.md` §4.1 |
| 10 | IPC: a rendezvous, and nothing buffered | `design.md` §4.2 |
| 11 | Capabilities: indices, never names | `design.md` §4.3 |

## Part IV — The userland

| | Chapter | Sources |
|---|---|---|
| 12 | Lua at EL0, and the language split | `design.md` §5, §6 |
| 13 | Namespaces: what you did not mount does not exist | `design.md` §4.4 |
| 14 | Servers, and writing one | `architecture.md` |
| 15 | Hot reload, and why the design exists for it | `design.md` §10 |

## Part V — Pixels

| | Chapter | Sources |
|---|---|---|
| 16 | The path a pixel takes | `gfx.md` §19.7 |
| 17 | Surfaces, sharing, and the commit | `gfx.md` §19.4, §19.8 |
| 18 | The window manager | `ui.md` |
| 19 | The UI kit, and its BeOS lineage | `ui.md`, `beos.md` |
| 20 | A software 3D renderer, and what it decided | `g3d.lua` |

## Part VI — Storage

| | Chapter | Sources |
|---|---|---|
| 21 | Attributes, indices and live queries | `design.md` §8.2 |
| 22 | The disk: a format, borrowed | `design.md` §8.3, `kfs.lua` |
| 23 | The journal | *not yet built* |

## Part VII — The desktop

| | Chapter | Sources |
|---|---|---|
| 24 | Tracker and the Deskbar | `ui.md`, `beos.md` |
| 25 | Replicants | `ui.md` |
| 26 | The applications | the apps themselves |

## Part VIII — Knowing it works

| | Chapter | Sources |
|---|---|---|
| 27 | Testing a system with no operating system under it | `testing.md` |
| 28 | Measurement, and what QEMU cannot tell you | `testing.md` §18.3 |
| 29 | Things that broke, and what each one taught | `state.md` |

## Appendices

- A — The decision log, in full (`README.md`)
- B — Glossary (`glossary.md`)
- C — The numbers: sizes, budgets, benchmarks
