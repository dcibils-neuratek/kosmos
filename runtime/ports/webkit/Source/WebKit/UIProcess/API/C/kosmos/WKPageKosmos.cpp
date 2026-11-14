/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `WKPagePaintKosmos` (`WKPageKosmos.h`): PlayStation's `WKPagePaint` with
 * a pitch, on Skia alone, and with the background drawn where the drawing
 * area had nothing - PlayStation's draws it only for a colour that cannot
 * be seen.
 */

#include "config.h"
#include "WKPageKosmos.h"

#include "DrawingAreaProxyCoordinatedGraphics.h"
#include "WKAPICast.h"
#include "WebPageProxy.h"
#include <WebCore/Color.h>
#include <WebCore/Region.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>

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
