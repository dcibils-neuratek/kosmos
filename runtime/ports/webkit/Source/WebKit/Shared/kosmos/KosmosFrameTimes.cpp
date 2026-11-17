/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The frame times `KosmosFrameTimes.h` describes. One image runs every
 * role, so the flag and both tallies live here for whichever role this
 * process is; each uses its own half.
 */

#include "config.h"
#include "KosmosFrameTimes.h"

#include <algorithm>
#include <atomic>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {

bool kosmosFramesTraced = false;

namespace {

struct Stage {
    unsigned count = 0;
    double sum = 0, most = 0;
    void add(double ms)
    {
        count++;
        sum += ms;
        most = std::max(most, ms);
    }
    double average() const { return count ? sum / count : 0; }
};

struct WebFrames {
    Stage rendering, region, painting, whole;
    Stage scrolledRendering, scrolledPainting, scrolledWhole;
    unsigned long scrolledPixels = 0, scrolledPainted = 0;
    unsigned scrolledWholeBounds = 0, said = 0;
    unsigned scrolled = 0;
    unsigned long pixels = 0;
    MonotonicTime first, last;
    bool flushing = false;
} web;

// A turn of the wheel's hops: to the web process's main thread, to the
// display that answers it, and to that display's picture sent - each from
// the turn in the UI process, on the one clock both read (patch 0040).
struct Wheels {
    Stage onMain, displayBegan, sent;
    MonotonicTime turned;
    bool waiting = false;
} wheels;

Stage incorporated, incorporatedScrolled, fromRing;
std::atomic<double> lastRead { 0 };

void sayWebFrames()
{
    auto now = MonotonicTime::now();
    if (now - web.last < 1_s) {
        RunLoop::mainSingleton().dispatchAfter(web.last + 1_s - now, sayWebFrames);
        return;
    }
    web.flushing = false;
    if (!web.whole.count)
        return;
    WTFLogAlways("webkit: frames painted %u (%u scrolled) in %.0f ms, %.1f Mpx: each %.1f ms, most %.1f - "
        "rendering %.1f, most %.1f; region %.2f, most %.2f; painting %.1f, most %.1f",
        web.whole.count, web.scrolled, (web.last - web.first).milliseconds(), web.pixels / 1e6,
        web.whole.average(), web.whole.most, web.rendering.average(), web.rendering.most,
        web.region.average(), web.region.most, web.painting.average(), web.painting.most);
    if (web.scrolledWhole.count)
        WTFLogAlways("webkit: frames scrolled %u, %.2f Mpx each, %.2f painted, %u painted their bounds whole: each %.1f ms, most %.1f - rendering %.1f, most %.1f; painting %.1f, most %.1f",
            web.scrolledWhole.count, web.scrolledPixels / 1e6 / web.scrolledWhole.count,
            web.scrolledPainted / 1e6 / web.scrolledWhole.count, web.scrolledWholeBounds,
            web.scrolledWhole.average(), web.scrolledWhole.most,
            web.scrolledRendering.average(), web.scrolledRendering.most,
            web.scrolledPainting.average(), web.scrolledPainting.most);
    if (wheels.sent.count)
        WTFLogAlways("webkit: wheels %u, after the turn: on the main thread %.1f ms, most %.1f; "
            "its display began %.1f, most %.1f; its picture sent %.1f, most %.1f",
            wheels.sent.count, wheels.onMain.average(), wheels.onMain.most,
            wheels.displayBegan.average(), wheels.displayBegan.most, wheels.sent.average(), wheels.sent.most);
    web = { };
    wheels = { };
}

} // namespace

void kosmosWebFrame(double rendering, double region, double painting, bool scrolled, unsigned long pixels)
{
    auto now = MonotonicTime::now();
    if (!web.whole.count)
        web.first = now;
    web.last = now;
    web.rendering.add(rendering);
    web.region.add(region);
    web.painting.add(painting);
    web.whole.add(rendering + region + painting);
    web.scrolled += scrolled;
    web.pixels += pixels;
    if (scrolled) {
        web.scrolledRendering.add(rendering);
        web.scrolledPainting.add(painting);
        web.scrolledWhole.add(rendering + region + painting);
        web.scrolledPixels += pixels;
    }
    if (wheels.waiting) {
        wheels.waiting = false;
        double whole = rendering + region + painting;
        wheels.displayBegan.add((now - wheels.turned).milliseconds() - whole);
        wheels.sent.add((now - wheels.turned).milliseconds());
    }
    if (!web.flushing) {
        web.flushing = true;
        RunLoop::mainSingleton().dispatchAfter(1_s, sayWebFrames);
    }
}

void kosmosWebDirty(const WebCore::IntRect& bounds, const Vector<WebCore::IntRect, 1>& rects, const WebCore::IntSize& scrolled, bool wholeBounds)
{
    unsigned long area = 0;
    for (auto& rect : rects)
        area += static_cast<unsigned long>(rect.width()) * rect.height();
    web.scrolledPainted += wholeBounds ? static_cast<unsigned long>(bounds.width()) * bounds.height() : area;
    web.scrolledWholeBounds += wholeBounds;
    if (web.said >= 3)
        return;
    web.said++;
    StringBuilder line;
    for (auto& rect : rects)
        line.append(' ', rect.x(), ',', rect.y(), ' ', rect.width(), 'x', rect.height());
    WTFLogAlways("webkit: scrolled by %d,%d, bounds %d,%d %dx%d%s, %zu rectangles:%s",
        scrolled.width(), scrolled.height(), bounds.x(), bounds.y(), bounds.width(), bounds.height(),
        wholeBounds ? " painted whole" : "", rects.size(), line.toString().utf8().data());
}

void kosmosWebWheelOnMain(MonotonicTime turned)
{
    wheels.onMain.add((MonotonicTime::now() - turned).milliseconds());
    // A second turn before the first's display: the display answers both,
    // and is timed from the first.
    if (!wheels.waiting) {
        wheels.turned = turned;
        wheels.waiting = true;
    }
}

void kosmosUIIncorporated(double milliseconds, bool scrolled)
{
    incorporated.add(milliseconds);
    if (scrolled)
        incorporatedScrolled.add(milliseconds);
    double read = lastRead.load(std::memory_order_relaxed);
    if (read > 0)
        fromRing.add((MonotonicTime::now().secondsSinceEpoch().seconds() - read) * 1000 - milliseconds);
}

void kosmosRecordRead()
{
    lastRead.store(MonotonicTime::now().secondsSinceEpoch().seconds(), std::memory_order_relaxed);
}

unsigned kosmosTakeIncorporated(double* average, double* most, double* ring, double* ringMost, double* scrolled, double* scrolledMost)
{
    *scrolled = incorporatedScrolled.average();
    *scrolledMost = incorporatedScrolled.most;
    incorporatedScrolled = { };
    unsigned count = incorporated.count;
    *average = incorporated.average();
    *most = incorporated.most;
    *ring = fromRing.average();
    *ringMost = fromRing.most;
    incorporated = { };
    fromRing = { };
    return count;
}

} // namespace WebKit
