/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Where WebCore's fonts are on Kosmos** (`docs/w6.md` W6b;
 * `0015-fonts-from-a-folder.patch`): a folder, read by Skia's manager for
 * one. `/Kosmos/Fonts/` unless a program says otherwise before its first
 * page - a test harness, with the fonts it is held to beside it.
 */

#pragma once

namespace WebCore {

// The folder, ending in a slash.
const char* kosmosFontFolder();
// Read once, when the first font is asked for: set it before then.
void setKosmosFontFolder(const char*);

} // namespace WebCore
