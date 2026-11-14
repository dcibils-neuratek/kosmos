/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Kosmos's input, as WebKit's events** (`docs/w7.md` W7c): where a key,
 * the pointer and the wheel from the window manager become WebKit's
 * `WebKeyboardEvent`, `WebMouseEvent` and `WebWheelEvent`, as each port's
 * `WebEventFactory` turns its toolkit's. WebKit includes this header by
 * name in the web process whatever the port; Kosmos's factory arrives with
 * the window, and until then the class has nothing in it.
 */

#pragma once

namespace WebKit {

class WebEventFactory {
};

} // namespace WebKit
