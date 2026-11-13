/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Where WebCore's fonts are on Kosmos** (`docs/w6.md` W6b;
 * `0015-fonts-from-a-folder.patch`): a folder, read by Skia's manager for
 * one. `/Kosmos/Fonts/` unless a program says otherwise before its first
 * page - a test harness, with the fonts it is held to beside it.
 */

#pragma once

#include <skia/core/SkFontMgr.h>
#include <skia/core/SkString.h>

namespace WebCore {

// The folder, ending in a slash.
const char* kosmosFontFolder();
// Read once, when the first font is asked for: set it before then.
void setKosmosFontFolder(const char*);

// `name` as `manager` spells it, if one of its families is `name` in
// another case; `name` itself if none is. CSS ignores a family's case and
// a folder's manager does not (`0020-font-families-in-any-case.patch`).
// A generic family - `serif`, `sans-serif`, `monospace` and the rest - is
// first one of Kosmos's fonts, IBM Plex's three.
SkString kosmosFontFamily(SkFontMgr& manager, const char* name);

} // namespace WebCore
