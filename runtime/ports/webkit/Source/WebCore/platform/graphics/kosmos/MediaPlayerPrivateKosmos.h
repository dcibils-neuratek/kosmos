/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Kosmos's media engine** (`docs/media.md` M2): what a `<video>` plays
 * with. WebCore runs the element - its controls, its events, its timeline -
 * and asks an engine for everything that touches a film; Apple's is
 * AVFoundation and GTK's GStreamer, and this one is the Media Kit
 * (`user/kits/media/media.h`), which JetSki shares with the Video app.
 *
 * **The bytes come through the page's own loader**, the network process,
 * cookies and all, into memory as they arrive; the kit reads them through
 * its `struct media_io`, and a read past what has arrived waits - on the
 * decode thread, never on the page's.
 *
 * **One decode thread a player**: the container taken apart, the picture
 * decoded, and each frame turned into the page's pixels by the gfx kit's
 * `gfx_i420` - the one conversion every film here goes through - into a
 * short queue the page's thread shows from when each frame's time comes.
 *
 * **The clock** is the counter from `play()`, until the Playback Kit's
 * position is put under it with the sound (M4). Sound is not played yet:
 * a film says it has none, so a page's own controls show it muted.
 */

#pragma once

#if ENABLE(VIDEO) && PLATFORM(KOSMOS)

#include "MediaPlayerPrivate.h"
#include "PlatformTimeRanges.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Threading.h>
#include <wtf/ThreadSafeWeakPtr.h>

#include <skia/core/SkImage.h>

namespace WebCore {

class NativeImage;
class PlatformMediaResource;

class MediaPlayerPrivateKosmos final
    : public MediaPlayerPrivateInterface
    , public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaPlayerPrivateKosmos, WTF::DestructionThread::Main> {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerPrivateKosmos);
public:
    static void registerMediaEngine(MediaEngineRegistrar);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);
    static void getSupportedTypes(HashSet<String>&);

    explicit MediaPlayerPrivateKosmos(MediaPlayer&);
    ~MediaPlayerPrivateKosmos();

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::Kosmos; }

    void load(const String&) final;
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
#endif
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) final;
#endif
    void cancelLoad() final;

    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }

    FloatSize naturalSize() const final { return m_size; }
    bool hasVideo() const final { return m_hasVideo; }
    bool hasAudio() const final { return false; }
    void setPageIsVisible(bool) final { }

    MediaTime duration() const final;
    MediaTime currentTime() const final;
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    bool ended() const final { return m_ended; }

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    const PlatformTimeRanges& buffered() const final;
    bool didLoadingProgress() const final;
    unsigned long long totalBytes() const final;

    void paint(GraphicsContext&, const FloatRect&) final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final;
    DestinationColorSpace colorSpace() final;

    unsigned decodedFrameCount() const final { return m_decoded; }
    unsigned droppedFrameCount() const final { return m_dropped; }
    String engineDescription() const final { return "Kosmos"_s; }

    /* The page's loader, handing over what arrives (main thread). */
    void responded(long long expectedLength);
    void arrived(std::span<const uint8_t>);
    void finished(bool failed);

    /* The Media Kit's `struct media_io`, on the decode thread. */
    long readBytes(uint8_t*, size_t);
    int64_t seekBytes(int64_t offset, int whence);

private:
    struct Frame {
        int64_t pts { 0 };
        sk_sp<SkImage> image;
    };

    void decode();
    void tick();
    void setNetworkState(MediaPlayer::NetworkState);
    void setReadyState(MediaPlayer::ReadyState);
    void stopThread();
    template<typename F> void onMain(F&&);

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RefPtr<PlatformMediaResource> m_resource;
    RefPtr<Thread> m_thread;
    RunLoop::Timer m_ticker;

    /* Shared with the decode thread, under m_lock. */
    mutable Lock m_lock;
    Condition m_changed;
    Vector<uint8_t> m_bytes WTF_GUARDED_BY_LOCK(m_lock);
    long long m_expected WTF_GUARDED_BY_LOCK(m_lock) { -1 };
    size_t m_position WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_complete WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_stop WTF_GUARDED_BY_LOCK(m_lock) { false };
    Deque<Frame> m_queue WTF_GUARDED_BY_LOCK(m_lock);
    bool m_endOfFilm WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<int64_t> m_seekTo WTF_GUARDED_BY_LOCK(m_lock);

    /* The page's thread only. */
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    FloatSize m_size;
    bool m_hasVideo { false };
    int64_t m_duration { -1 };
    bool m_paused { true };
    bool m_ended { false };
    double m_startedAt { 0 };           /* the film's time when play() came */
    MonotonicTime m_playedAt;
    int64_t m_skipBefore { 0 };         /* after a seek: frames before it are not shown */
    Frame m_current;
    RefPtr<NativeImage> m_currentImage;
    mutable PlatformTimeRanges m_buffered;
    mutable size_t m_reportedBytes { 0 };
    unsigned m_decoded { 0 };
    unsigned m_dropped { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(KOSMOS)
