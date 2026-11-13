/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The editor's platform half (docs/w6.md W6b).
 *
 * Nothing is copied and nothing is pasted yet: the pasteboard is empty
 * (PasteboardKosmos.cpp), and in WebCore's generic branch, which Kosmos
 * falls into, the web content a pasteboard is handed has no fields to
 * fill. When the pasteboard is the window manager's clipboard (W7), these
 * hand it the selection's text and markup as GTK's and WPE's do.
 */

#include "config.h"
#include "Editor.h"

#include "Pasteboard.h"

namespace WebCore {

void Editor::pasteWithPasteboard(Pasteboard*, OptionSet<PasteOption>)
{
}

void Editor::platformCopyFont()
{
}

void Editor::platformPasteFont()
{
}

void Editor::writeImageToPasteboard(Pasteboard&, Element&, const URL&, const String&)
{
}

void Editor::writeSelectionToPasteboard(Pasteboard&)
{
}

} // namespace WebCore
