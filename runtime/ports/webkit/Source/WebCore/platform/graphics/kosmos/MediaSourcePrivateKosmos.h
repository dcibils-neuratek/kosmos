/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A MediaSource on Kosmos** (`docs/media.md` M3): what a page's
 * `new MediaSource()` is underneath. WebCore's base class keeps the source
 * buffers, the duration, the buffered ranges and the ready state; this
 * makes the source buffers and tells the player when something changed.
 * It is as thin as WebCore's own mock, on purpose: the rules of MSE live
 * in WebCore, once, and a port that restated them would be a second copy.
 */

#pragma once

#if ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)

#include "MediaSourcePrivate.h"
#include <wtf/ThreadSafeWeakPtr.h>

namespace WebCore {

class MediaPlayerPrivateKosmos;

class MediaSourcePrivateKosmos final : public MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateKosmos> create(MediaPlayerPrivateKosmos&, MediaSourcePrivateClient&);
    virtual ~MediaSourcePrivateKosmos();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Kosmos; }

    RefPtr<MediaPlayerPrivateInterface> player() const final;
    void setPlayer(MediaPlayerPrivateInterface*) final;
    RefPtr<MediaPlayerPrivateKosmos> kosmosPlayer() const { return m_player.get(); }

private:
    MediaSourcePrivateKosmos(MediaPlayerPrivateKosmos&, MediaSourcePrivateClient&);

    AddStatus addSourceBuffer(const ContentType&, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>&) final;
    void durationChanged(const MediaTime&) final;
    void notifyActiveSourceBuffersChanged() final;

    ThreadSafeWeakPtr<MediaPlayerPrivateKosmos> m_player;
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::MediaSourcePrivateKosmos)
static bool isType(const WebCore::MediaSourcePrivate& source) { return source.platformType() == WebCore::MediaPlatformType::Kosmos; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)
