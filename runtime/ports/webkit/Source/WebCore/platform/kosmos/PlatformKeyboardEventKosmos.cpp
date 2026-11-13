/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A key down, sorted into the key and the character it typed
 * (docs/w6.md W6b), as GTK's port sorts it: the key keeps its
 * identifier and loses its text, the character the other way round.
 * Kosmos's port has no input method, so there is no event one already
 * handled.
 */

#include "config.h"
#include "PlatformKeyboardEvent.h"

namespace WebCore {

void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type type, bool backwardCompatibilityMode)
{
    ASSERT(m_type == PlatformEvent::Type::KeyDown);
    ASSERT(type == Type::RawKeyDown || type == Type::Char);
    m_type = type;

    if (backwardCompatibilityMode)
        return;

    if (type == PlatformEvent::Type::RawKeyDown) {
        m_text = String();
        m_unmodifiedText = String();
    } else
        m_keyIdentifier = String();
}

} // namespace WebCore
