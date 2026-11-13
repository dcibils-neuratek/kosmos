/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The pasteboard WebCore asks for, with nothing behind it yet
 * (docs/w6.md W6b).
 *
 * Kosmos has a clipboard - the window manager holds it - and a page will
 * reach it when JetSki has a window to paste into (W7). Until then every
 * pasteboard is empty: it holds nothing, says it holds nothing, and a
 * write is dropped. That is the truth about a page with no window, and it
 * is what WebCore's empty clients assume of a pasteboard too.
 */

#include "config.h"
#include "Pasteboard.h"

#include "PasteboardContext.h"

namespace WebCore {

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context)
    : m_context(WTF::move(context))
{
}

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context));
}

bool Pasteboard::hasData()
{
    return false;
}

Vector<String> Pasteboard::typesSafeForBindings(const String&)
{
    return { };
}

Vector<String> Pasteboard::typesForLegacyUnsafeBindings()
{
    return { };
}

String Pasteboard::readOrigin()
{
    return { };
}

String Pasteboard::readString(const String&)
{
    return { };
}

String Pasteboard::readStringInCustomData(const String&)
{
    return { };
}

void Pasteboard::writeString(const String&, const String&) { }
void Pasteboard::clear() { }
void Pasteboard::clear(const String&) { }

void Pasteboard::read(PasteboardPlainText&, PlainTextURLReadingPolicy, std::optional<size_t>) { }
void Pasteboard::read(PasteboardWebContentReader&, WebContentReadingPolicy, std::optional<size_t>) { }
void Pasteboard::read(PasteboardFileReader&, std::optional<size_t>) { }

void Pasteboard::write(const Color&) { }
void Pasteboard::write(const PasteboardURL&) { }
void Pasteboard::write(const PasteboardImage&) { }
void Pasteboard::write(const PasteboardBuffer&) { }
void Pasteboard::write(const PasteboardWebContent&) { }
void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&) { }
void Pasteboard::writeCustomData(const Vector<PasteboardCustomData>&) { }
void Pasteboard::writeMarkup(const String&) { }
void Pasteboard::writePlainText(const String&, SmartReplaceOption) { }

Pasteboard::FileContentState Pasteboard::fileContentState()
{
    return FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace()
{
    return false;
}

} // namespace WebCore
