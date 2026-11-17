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

Stage incorporated, fromRing;
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

void kosmosUIIncorporated(double milliseconds, bool)
{
    incorporated.add(milliseconds);
    double read = lastRead.load(std::memory_order_relaxed);
    if (read > 0)
        fromRing.add((MonotonicTime::now().secondsSinceEpoch().seconds() - read) * 1000 - milliseconds);
}

void kosmosRecordRead()
{
    lastRead.store(MonotonicTime::now().secondsSinceEpoch().seconds(), std::memory_order_relaxed);
}

unsigned kosmosTakeIncorporated(double* average, double* most, double* ring, double* ringMost)
{
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
