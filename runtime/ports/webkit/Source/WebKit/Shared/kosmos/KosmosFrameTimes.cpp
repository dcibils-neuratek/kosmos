/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The frame times `KosmosFrameTimes.h` describes. One image runs every
 * role, so the flag and both tallies live here for whichever role this
 * process is; each uses its own half.
 */

#include "config.h"
#include "KosmosFrameTimes.h"

#include <algorithm>
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

Stage incorporated;

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
    web = { };
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
    if (!web.flushing) {
        web.flushing = true;
        RunLoop::mainSingleton().dispatchAfter(1_s, sayWebFrames);
    }
}

void kosmosUIIncorporated(double milliseconds, bool)
{
    incorporated.add(milliseconds);
}

unsigned kosmosTakeIncorporated(double* average, double* most)
{
    unsigned count = incorporated.count;
    *average = incorporated.average();
    *most = incorporated.most;
    incorporated = { };
    return count;
}

} // namespace WebKit
