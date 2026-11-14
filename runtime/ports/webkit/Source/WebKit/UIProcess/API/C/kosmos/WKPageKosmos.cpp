/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `WKPagePaintKosmos` (`WKPageKosmos.h`): PlayStation's `WKPagePaint` with
 * a pitch, on Skia alone, and with the background drawn where the drawing
 * area had nothing - PlayStation's draws it only for a colour that cannot
 * be seen.
 *
 * And the pointer, the wheel and keys (`WKPageMouseKosmos` and the rest):
 * WebKit's events made here from numbers, and given to the page through
 * the `NativeWeb*Event` constructors patch 0024 adds for Kosmos.
 */

#include "config.h"
#include "WKPageKosmos.h"

#include "DrawingAreaProxyCoordinatedGraphics.h"
#include "NativeWebKeyboardEvent.h"
#include "NativeWebMouseEvent.h"
#include "NativeWebWheelEvent.h"
#include "WKAPICast.h"
#include "WebPageProxy.h"
#include <WebCore/Color.h>
#include <WebCore/Region.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <wtf/HexNumber.h>
#include <wtf/MonotonicTime.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

static void paintBackground(SkCanvas* canvas, const std::optional<WebCore::Color>& color, const WebCore::IntRect& rect)
{
    SkPaint paint;
    paint.setColor(color ? SkColor(*color) : SK_ColorWHITE);
    canvas->drawRect(SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height()), paint);
}

void WKPagePaintKosmos(WKPageRef pageRef, uint32_t* pixels, unsigned width, unsigned height, unsigned pitch, WKRect wkPaintRect)
{
    if (!pixels || !width || !height || pitch < width * 4)
        return;
    auto info = SkImageInfo::MakeN32Premul(width, height, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::WrapPixels(info, pixels, pitch, nullptr);
    if (!surface)
        return;
    auto* canvas = surface->getCanvas();
    auto paintRect = WebKit::toIntRect(wkPaintRect);
    paintRect.intersect(WebCore::IntRect(0, 0, width, height));

    RefPtr page = WebKit::toImpl(pageRef);
    auto& backgroundColor = page->backgroundColor();
    if (auto* drawingArea = page->drawingArea()) {
        WebCore::Region unpainted;
        downcast<WebKit::DrawingAreaProxyCoordinatedGraphics>(*drawingArea).paint(canvas, paintRect, unpainted);
        for (const auto& rect : unpainted.rects())
            paintBackground(canvas, backgroundColor, rect);
    } else
        paintBackground(canvas, backgroundColor, paintRect);
}

static OptionSet<WebKit::WebEventModifier> modifiersOf(unsigned bits)
{
    OptionSet<WebKit::WebEventModifier> out;
    if (bits & kWKKosmosModifierShift)
        out.add(WebKit::WebEventModifier::ShiftKey);
    if (bits & kWKKosmosModifierControl)
        out.add(WebKit::WebEventModifier::ControlKey);
    if (bits & kWKKosmosModifierAlt)
        out.add(WebKit::WebEventModifier::AltKey);
    if (bits & kWKKosmosModifierMeta)
        out.add(WebKit::WebEventModifier::MetaKey);
    return out;
}

void WKPageMouseKosmos(WKPageRef pageRef, int type, int button, unsigned buttons, int x, int y, int clicks, unsigned modifiers)
{
    auto eventType = type == kWKKosmosMouseDown ? WebKit::WebEventType::MouseDown
        : type == kWKKosmosMouseUp ? WebKit::WebEventType::MouseUp
        : WebKit::WebEventType::MouseMove;
    auto which = button == 0 ? WebKit::WebMouseEventButton::Left
        : button == 1 ? WebKit::WebMouseEventButton::Middle
        : button == 2 ? WebKit::WebMouseEventButton::Right
        : WebKit::WebMouseEventButton::None;
    WebCore::DoublePoint position(x, y);

    WebKit::WebMouseEvent event({ eventType, modifiersOf(modifiers), MonotonicTime::now() }, which,
        static_cast<unsigned short>(buttons), position, position, 0, 0, 0,
        eventType == WebKit::WebEventType::MouseMove ? 0 : clicks, 0, WebKit::WebEventInputSource::UserDriven);
    WebKit::toImpl(pageRef)->handleMouseEvent(WebKit::NativeWebMouseEvent(event));
}

void WKPageWheelKosmos(WKPageRef pageRef, int x, int y, float dx, float dy, float ticksX, float ticksY, unsigned modifiers)
{
    WebCore::IntPoint position(x, y);

    WebKit::WebWheelEvent event({ WebKit::WebEventType::Wheel, modifiersOf(modifiers), MonotonicTime::now() },
        position, position, WebCore::FloatSize(dx, dy), WebCore::FloatSize(ticksX, ticksY),
        WebKit::WebWheelEvent::Granularity::ScrollByPixelWheelEvent);
    WebKit::toImpl(pageRef)->handleNativeWheelEvent(WebKit::NativeWebWheelEvent(event));
}

void WKPageKeyKosmos(WKPageRef pageRef, bool down, const char* key, const char* code, const char* text, int virtualKey, unsigned modifiers)
{
    auto keyName = String::fromUTF8(key ? key : "Unidentified");
    auto codeName = String::fromUTF8(code ? code : "Unidentified");
    auto typed = down && text ? String::fromUTF8(text) : String();

    // WebCore's old name for a key: "U+0041" for a character, the DOM's
    // name otherwise, which for the keys WebCore asks about is the same.
    String identifier = keyName;
    if (keyName.length() == 1)
        identifier = makeString("U+"_s, hex(keyName.convertToASCIIUppercase()[0], 4));

    WebKit::WebKeyboardEvent event({ down ? WebKit::WebEventType::KeyDown : WebKit::WebEventType::KeyUp, modifiersOf(modifiers), MonotonicTime::now() },
        typed, typed, keyName, codeName, identifier, virtualKey, virtualKey, 0, false, false, false);
    WebKit::toImpl(pageRef)->handleKeyboardEvent(WebKit::NativeWebKeyboardEvent(event));
}
