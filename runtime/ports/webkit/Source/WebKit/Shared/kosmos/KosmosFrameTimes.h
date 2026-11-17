/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A frame's time, traced** (`webkit.md` *Fast*, step 1a; `testing.md`
 * 18.853-18.854): while JetSki's trace is on, the web process times each
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

namespace WebKit {

extern bool kosmosFramesTraced;

// The web process: one `display()`, its stages in milliseconds.
void kosmosWebFrame(double rendering, double region, double painting, bool scrolled, unsigned long pixels);

// The UI process: one update taken in.
void kosmosUIIncorporated(double milliseconds, bool scrolled);

// The UI process's updates since the last take: how many, on average, the most.
unsigned kosmosTakeIncorporated(double* average, double* most);

} // namespace WebKit
