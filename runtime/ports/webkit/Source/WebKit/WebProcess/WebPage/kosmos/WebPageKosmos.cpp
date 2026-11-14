/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A page's platform hooks on Kosmos: PlayStation's (`WebPagePlayStation.cpp`,
 * its notice below), with the one it left unwritten - **keys that edit**
 * (`docs/w7.md` W7c). PlayStation's `handleEditingKeyboardEvent` returned
 * false, so a text field took focus and keypresses and never a character.
 *
 * As Windows's port had it before it went: a key going down looks itself up
 * in a table of editing commands by Windows's virtual key and its modifiers
 * - Backspace deletes, the arrows move, Shift with them selects - and a
 * keypress that is none of those inserts its text, if what has the focus
 * can be edited. Copy, cut and paste are not here: the window manager owns
 * those keys and sends JetSki the intent (`ui.lua`, `EDITS`).
 */
/*
 * Copyright (C) 2020 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebPage.h"

#include <WebCore/Editor.h>
#include <WebCore/EventNames.h>
#include <WebCore/KeyboardEvent.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Node.h>
#include <WebCore/NotImplemented.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/Page.h>
#include <WebCore/PointerCharacteristics.h>
#include <WebCore/Settings.h>
#include <WebCore/UserAgent.h>

namespace WebKit {
using namespace WebCore;

void WebPage::platformInitialize(const WebPageCreationParameters&)
{
}

void WebPage::platformReinitializeAccessibilityToken()
{
}

void WebPage::platformDetach()
{
}

bool WebPage::platformCanHandleRequest(const ResourceRequest&)
{
    notImplemented();
    return false;
}

String WebPage::platformUserAgent(const URL& url) const
{
    if (url.isNull() || !m_page->settings().needsSiteSpecificQuirks())
        return emptyString();

    return WebCore::standardUserAgentForURL(url);
}

bool WebPage::hoverSupportedByPrimaryPointingDevice() const
{
    return true;
}

bool WebPage::hoverSupportedByAnyAvailablePointingDevice() const
{
    return true;
}

std::optional<PointerCharacteristics> WebPage::pointerCharacteristicsOfPrimaryPointingDevice() const
{
    return PointerCharacteristics::Fine;
}

OptionSet<PointerCharacteristics> WebPage::pointerCharacteristicsOfAllAvailablePointingDevices() const
{
    return PointerCharacteristics::Fine;
}

namespace {

enum : unsigned { Shift = 1, Control = 2, Alt = 4 };

struct EditingKey {
    int virtualKey;
    unsigned modifiers;
    ASCIILiteral command;
};

// Windows's virtual keys: 0x08 Backspace, 0x21-0x28 the page keys, End,
// Home and the arrows, 0x2e Delete, then the letters by their capitals.
constexpr EditingKey keyDownCommands[] = {
    { 0x25, 0, "MoveLeft"_s },
    { 0x25, Shift, "MoveLeftAndModifySelection"_s },
    { 0x25, Control, "MoveWordLeft"_s },
    { 0x25, Control | Shift, "MoveWordLeftAndModifySelection"_s },
    { 0x27, 0, "MoveRight"_s },
    { 0x27, Shift, "MoveRightAndModifySelection"_s },
    { 0x27, Control, "MoveWordRight"_s },
    { 0x27, Control | Shift, "MoveWordRightAndModifySelection"_s },
    { 0x26, 0, "MoveUp"_s },
    { 0x26, Shift, "MoveUpAndModifySelection"_s },
    { 0x28, 0, "MoveDown"_s },
    { 0x28, Shift, "MoveDownAndModifySelection"_s },
    { 0x21, 0, "MovePageUp"_s },
    { 0x21, Shift, "MovePageUpAndModifySelection"_s },
    { 0x22, 0, "MovePageDown"_s },
    { 0x22, Shift, "MovePageDownAndModifySelection"_s },
    { 0x24, 0, "MoveToBeginningOfLine"_s },
    { 0x24, Shift, "MoveToBeginningOfLineAndModifySelection"_s },
    { 0x24, Control, "MoveToBeginningOfDocument"_s },
    { 0x24, Control | Shift, "MoveToBeginningOfDocumentAndModifySelection"_s },
    { 0x23, 0, "MoveToEndOfLine"_s },
    { 0x23, Shift, "MoveToEndOfLineAndModifySelection"_s },
    { 0x23, Control, "MoveToEndOfDocument"_s },
    { 0x23, Control | Shift, "MoveToEndOfDocumentAndModifySelection"_s },
    { 0x08, 0, "DeleteBackward"_s },
    { 0x08, Shift, "DeleteBackward"_s },
    { 0x08, Control, "DeleteWordBackward"_s },
    { 0x2e, 0, "DeleteForward"_s },
    { 0x2e, Control, "DeleteWordForward"_s },
    { 'A', Control, "SelectAll"_s },
    { 'Z', Control, "Undo"_s },
    { 'Z', Control | Shift, "Redo"_s },
    { 'Y', Control, "Redo"_s },
};

// Enter types a line, which is a command rather than its text, "\r".
constexpr EditingKey keyPressCommands[] = {
    { 0x0d, 0, "InsertNewline"_s },
    { 0x0d, Shift, "InsertLineBreak"_s },
};

template<size_t size>
std::optional<ASCIILiteral> commandFor(const EditingKey (&table)[size], int virtualKey, unsigned modifiers)
{
    for (auto& key : table) {
        if (key.virtualKey == virtualKey && key.modifiers == modifiers)
            return key.command;
    }
    return std::nullopt;
}

} // namespace

bool WebPage::handleEditingKeyboardEvent(WebCore::KeyboardEvent& event)
{
    auto* platformEvent = event.underlyingPlatformEvent();
    RefPtr node = dynamicDowncast<Node>(event.target());
    if (!platformEvent || !node)
        return false;
    RefPtr frame = node->document().frame();
    if (!frame)
        return false;

    unsigned modifiers = (platformEvent->shiftKey() ? Shift : 0)
        | (platformEvent->controlKey() ? Control : 0) | (platformEvent->altKey() ? Alt : 0);
    auto& editor = frame->editor();

    if (event.type() == eventNames().keydownEvent) {
        auto name = commandFor(keyDownCommands, platformEvent->windowsVirtualKeyCode(), modifiers);
        if (!name)
            return false;
        auto command = editor.command(*name);
        // Text arrives with the keypress, after the page has seen the key.
        return !command.isTextInsertion() && command.execute(&event);
    }

    if (event.type() != eventNames().keypressEvent || !editor.canEdit())
        return false;

    if (auto name = commandFor(keyPressCommands, platformEvent->windowsVirtualKeyCode(), modifiers))
        return editor.command(*name).execute(&event);

    // Nothing typed by a control character or with Control or Alt held.
    if (event.charCode() < ' ' || (modifiers & (Control | Alt)))
        return false;

    return editor.insertText(platformEvent->text(), &event);
}

void WebPage::getPlatformEditorState(LocalFrame& frame, EditorState& result) const
{
    notImplemented();
}

} // namespace WebKit
