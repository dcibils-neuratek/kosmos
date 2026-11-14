/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A page's picture, into a surface** (`docs/w7.md` W7a): the last
 * picture the web process sent, painted into pixels a caller holds - a
 * window's surface in JetSki, a region in the suite. Kosmos's pixels have
 * a pitch, which is almost never the width times four, so it is said.
 * Where the page has sent nothing yet, its background colour, or white.
 */

#pragma once

#include <WebKit/WKBase.h>
#include <WebKit/WKGeometry.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

WK_EXPORT void WKPagePaintKosmos(WKPageRef, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch, WKRect paintRect);

#ifdef __cplusplus
}
#endif
