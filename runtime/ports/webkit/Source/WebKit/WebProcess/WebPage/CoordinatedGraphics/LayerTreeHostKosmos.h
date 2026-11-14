/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **No compositor yet** (`docs/w7.md` W7a): a page on Kosmos is painted on
 * the processor, into the bitmap its web process hands the UI process, as
 * PlayStation's port does with accelerated compositing off - which Kosmos
 * keeps off (`WebPreferencesPlayStation.cpp`). The drawing area holds a
 * layer tree host it never makes (`DrawingAreaCoordinatedGraphics.cpp`,
 * whose upstream code already sets it to none on a port with neither
 * coordinated graphics nor TextureMapper's layers), and needs the class
 * whole to compile its calls. This is that class, and nothing makes one.
 * A compositor on the GPU replaces it, measured first (*Fast*).
 */

#pragma once

#include "LayerTreeContext.h"
#include <WebCore/DisplayRefreshMonitor.h>
#include <WebCore/GraphicsLayerFactory.h>
#include <WebCore/PlatformScreen.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Forward.h>

namespace WebCore {
class GraphicsLayer;
class Region;
}

namespace WebKit {

class LayerTreeHost {
public:
    const LayerTreeContext& layerTreeContext() const LIFETIME_BOUND { return m_layerTreeContext; }
    void setLayerTreeStateIsFrozen(bool) { }
    void scheduleRenderingUpdate() { }
    void setRootCompositingLayer(WebCore::GraphicsLayer*) { }
    void setViewOverlayRootLayer(WebCore::GraphicsLayer*) { }
    void setNonCompositedContentsNeedDisplay(const WebCore::IntRect&) { }
    void updateRenderingWithForcedRepaint() { }
    void updateRenderingWithForcedRepaintAsync(CompletionHandler<void()>&& completionHandler) { completionHandler(); }
    void sizeDidChange() { }
    void pauseRendering() { }
    void resumeRendering() { }
    WebCore::GraphicsLayerFactory* graphicsLayerFactory() { return nullptr; }
    void backgroundColorDidChange() { }
    RefPtr<WebCore::DisplayRefreshMonitor> createDisplayRefreshMonitor(WebCore::PlatformDisplayID) { return nullptr; }
    WebCore::PlatformDisplayID displayID() const { return 0; }
#if ENABLE(DAMAGE_TRACKING)
    void resetDamageHistoryForTesting() { }
    void foreachRegionInDamageHistoryForTesting(Function<void(const WebCore::Region&)>&&) { }
#endif

private:
    LayerTreeHost() = delete;
    LayerTreeContext m_layerTreeContext;
};

} // namespace WebKit
