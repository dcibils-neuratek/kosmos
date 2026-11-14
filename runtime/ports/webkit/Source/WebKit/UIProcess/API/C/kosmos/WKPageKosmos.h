/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A page's picture, into a surface** (`docs/w7.md` W7a): the last
 * picture the web process sent, painted into pixels a caller holds - a
 * window's surface in JetSki, a region in the suite. Kosmos's pixels have
 * a pitch, which is almost never the width times four, so it is said.
 * Where the page has sent nothing yet, its background colour, or white.
 *
 * **And what a person does to it** (W7c): the pointer, the wheel and keys,
 * made into WebKit's own events here - WebKit has no public call for any
 * of them, each port building them from its toolkit's - and handed to the
 * page as GTK's and WPE's views hand theirs. Positions are in the view's
 * pixels; modifiers are the `WKKosmosModifier` bits.
 */

#pragma once

#include <WebKit/WKBase.h>
#include <WebKit/WKGeometry.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

WK_EXPORT void WKPagePaintKosmos(WKPageRef, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch, WKRect paintRect);

enum {
    kWKKosmosModifierShift = 1 << 0,
    kWKKosmosModifierControl = 1 << 1,
    kWKKosmosModifierAlt = 1 << 2,
    kWKKosmosModifierMeta = 1 << 3,
};

enum {
    kWKKosmosMouseDown,
    kWKKosmosMouseUp,
    kWKKosmosMouseMove,
};

/* `button`: 0 the left, 1 the middle, 2 the right, -1 none (a move with
 * nothing held). `buttons` is what is held, as the DOM counts them: 1 the
 * left, 2 the right, 4 the middle. `clicks` is 1, 2 for a double click. */
WK_EXPORT void WKPageMouseKosmos(WKPageRef, int type, int button, unsigned buttons, int x, int y, int clicks, unsigned modifiers);

/* `dx`, `dy` in pixels, positive towards the start - up and left, as
 * WebKit counts a wheel; `ticks` the notches. */
WK_EXPORT void WKPageWheelKosmos(WKPageRef, int x, int y, float dx, float dy, float ticksX, float ticksY, unsigned modifiers);

/* One key going down or coming up. `key` and `code` are the DOM's names
 * ("a", "KeyA"; "Enter", "Enter"), `text` what it types, if anything, in
 * UTF-8, `virtualKey` Windows's code, which WebCore's editing commands
 * are keyed by everywhere but the Mac. */
WK_EXPORT void WKPageKeyKosmos(WKPageRef, bool down, const char* key, const char* code, const char* text, int virtualKey, unsigned modifiers);

#ifdef __cplusplus
}
#endif
