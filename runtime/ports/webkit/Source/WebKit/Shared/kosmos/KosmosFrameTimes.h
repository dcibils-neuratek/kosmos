/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A frame's time, traced** (`webkit.md` *Fast*, step 1a; `testing.md`
 * 18.853-18.855): while JetSki's trace is on, the web process times each
 * `display()` - rendering (style, layout), the region made for the picture,
 * and painting it - and the UI process the picture taken into its backing
 * store (`incorporateUpdate`, a scroll's move included). The web process
 * says one line once a second has passed with no frame; the UI process's
 * numbers are taken by the WebKit Kit into its own `webkit: frames` line.
 *
 * Whether it is on is the UI process's to say (`WKKosmosSetFramesTraced`)
 * and travels to a web process in its launch page, since a web process
 * cannot see `/Home`, where the setting is.
 */

#pragma once

#include <WebCore/IntRect.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Vector.h>

namespace WebKit {

extern bool kosmosFramesTraced;

// The web process: one `display()`, its stages in milliseconds.
void kosmosWebFrame(double rendering, double region, double painting, bool scrolled, unsigned long pixels);

// The web process: a turn of the wheel, turned in the UI process at `turned`,
// reaching the main thread (patch 0039). The display that follows is timed
// from it, and the line says how long each hop took.
void kosmosWebWheelOnMain(MonotonicTime turned);

// The web process: a scrolled display's dirty rectangles, their bounds, the
// scroll so far, and whether the bounds are painted whole (patch 0042).
void kosmosWebDirty(const WebCore::IntRect& bounds, const Vector<WebCore::IntRect, 1>& rects, const WebCore::IntSize& scrolled, bool wholeBounds);

// The UI process: one update taken in.
void kosmosUIIncorporated(double milliseconds, bool scrolled);

// Either process: a record read off a connection's ring, by its receiving
// thread. An update taken in is timed from the last one read - its own,
// since the receiving thread hands it on as it reads it.
void kosmosRecordRead();

// The UI process's updates since the last take: how many, on average, the
// most; and from the record read off the ring to the main thread taking it.
unsigned kosmosTakeIncorporated(double* average, double* most, double* fromRing, double* fromRingMost, double* scrolled, double* scrolledMost);

} // namespace WebKit
