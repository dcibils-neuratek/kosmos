<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Changes to NetSurf's libraries, applied as the build makes them

`runtime/upstream/netsurf/` is what NetSurf released, byte for byte, and stays
that way - the rule `lua/upstream/` keeps. What Kosmos changes in it is here,
as a patch a file, applied by the Makefile into `build/` and compiled in place
of the upstream file (`WEB_PATCHED`). Each one says what it does and why.

## `libdom/src/events/`: no event nobody can hear

libdom fires DOM mutation events - `DOMNodeInserted`, `DOMSubtreeModified` and
the rest - as the parser builds a document: several for every node inserted,
each made, stamped with `time(NULL)`, dispatched through the node's ancestors
and freed. A browser without JavaScript has no listener for any of them, and
the document the web kit parses has no default actions, so every one was work
for nothing.

It was most of a large page's time. Profiling Wikipedia's Dam article on 30
September 2026 (`roadmap.md` 6zz g) put 66% of the machine's busy time in
`time()` alone - fixed first, in the libc, where it belongs - and after that
the events were still the largest part of what the browser did: the
timestamps, and a good part of `malloc`.

So `event_target.c` counts the listeners that exist in the process, and
`dispatch.c`'s five dispatchers return at once, the event not cancelled, when
there are none and the document has no default actions. The moment anything
adds a listener - a script engine, one day - every event is made and
dispatched exactly as upstream does.
