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
 * **The sound** (M4) is a MediaSource's: its source buffer's decode thread
 * decodes the sound track with the Media Kit - the one Opus, Vorbis and
 * AAC decoder there is - and hands the Playback Kit 16-bit PCM, whose
 * thread converts it to the device's rate and writes it into a stream of
 * the audio server's. A film from an address has no sound yet.
 *
 * **The clock is the sound's** while there is sound: where the film's
 * time was when its first sound since the last seek was handed over, and
 * the frames the device has played of the stream since - so a picture is
 * shown when its sound is heard, and when the sound runs dry the clock
 * waits with it. Without sound - no device, no sound track, a film from
 * an address - it is the counter from `play()`.
 *
 * **A MediaSource** (M3) is the other way in, and YouTube's: the page
 * appends the film in pieces to source buffers (`SourceBufferPrivateKosmos`),
 * whose decode threads fill the same queue of frames this shows from. The
 * ready state, the duration and what is buffered are then the
 * MediaSource's, and the clock waits at the end of what is buffered.
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

struct media_frame;
struct playback;
struct audio_stream;

namespace WebCore {

class MediaSourcePrivateKosmos;
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
    bool hasAudio() const final { return m_hasAudio; }
    void setVolume(float) final;
    void setMuted(bool) final;
    bool timeIsProgressing() const final { return !m_paused && !m_stalled; }
    void setPageIsVisible(bool) final { }

    MediaTime duration() const final;
    MediaTime currentTime() const final;
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    bool ended() const final { return m_ended; }

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final;
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

    /* A decoded picture as the page's pixels, through `gfx_i420`: null when
     * it is not one this converts yet (8-bit 4:2:0 only). */
    static sk_sp<SkImage> picture(const struct media_frame&);

#if ENABLE(MEDIA_SOURCE)
    /* A source buffer's decode thread: a frame into the queue, waiting for
     * room; false when the player is stopping. A frame of an earlier
     * generation - decoded before a flush - is dropped. */
    bool deliver(int64_t pts, sk_sp<SkImage>&&, unsigned generation);
    unsigned frameGeneration();
    void flushFrames();

    /* A source buffer's decode thread: decoded sound, for the Playback Kit,
     * waiting for room; false when the player is stopping. Sound of an
     * earlier generation - decoded before `flushSound` - is dropped. */
    bool deliverSound(int64_t pts, const struct media_frame&, unsigned generation);
    unsigned soundGeneration();
    void flushSound();

    /* From the MediaSource and its source buffers, on the page's thread. */
    void videoTrackFound(FloatSize);
    void audioTrackFound();
    void mediaSourceDurationChanged();
    void activeSourceBuffersChanged();
    void readyStateFromMediaSourceChanged() final;
    void characteristicsFromMediaSourceChanged() final;
    void mediaSourceHasRetrievedAllData() final;
    MediaTime maxTimeSeekable() const final { return duration(); }
#endif

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
    std::optional<double> soundTime() const;
    void setSoundRunning(bool);
    void closeSound();
    void applyVolume() WTF_REQUIRES_LOCK(m_soundLock);
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
    unsigned m_generation WTF_GUARDED_BY_LOCK(m_lock) { 0 };

    /* The sound, between the page's thread and a decode thread. Every call
     * to the player is made under the lock, which makes its commands' one
     * writer whichever thread holds it. */
    mutable Lock m_soundLock;
    Condition m_soundChanged;
    struct playback* m_playback WTF_GUARDED_BY_LOCK(m_soundLock) { nullptr };
    std::unique_ptr<struct audio_stream> m_stream WTF_GUARDED_BY_LOCK(m_soundLock);  /* null until the first sound */
    unsigned m_soundGeneration WTF_GUARDED_BY_LOCK(m_soundLock) { 0 };
    unsigned m_streamGeneration WTF_GUARDED_BY_LOCK(m_soundLock) { ~0u };
    int64_t m_soundBase WTF_GUARDED_BY_LOCK(m_soundLock) { 0 };  /* the film's time, us, at frames_played 0 */
    int m_soundRate WTF_GUARDED_BY_LOCK(m_soundLock) { 0 }, m_soundChannels WTF_GUARDED_BY_LOCK(m_soundLock) { 0 };
    bool m_noSound WTF_GUARDED_BY_LOCK(m_soundLock) { false };      /* no device, or it would not open */
    bool m_soundPlaying WTF_GUARDED_BY_LOCK(m_soundLock) { false };
    bool m_soundStop WTF_GUARDED_BY_LOCK(m_soundLock) { false };
    float m_volume WTF_GUARDED_BY_LOCK(m_soundLock) { 1 };
    bool m_muted WTF_GUARDED_BY_LOCK(m_soundLock) { false };
    Vector<int16_t> m_pcm;              /* the decode thread's: a frame as 16 bits */

    /* The page's thread only. */
    bool m_hasAudio { false };
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    FloatSize m_size;
    bool m_hasVideo { false };
    int64_t m_duration { -1 };
    bool m_paused { true };
    bool m_ended { false };
    bool m_stalled { false };           /* the clock at the end of what is buffered */
#if ENABLE(MEDIA_SOURCE)
    RefPtr<MediaSourcePrivateKosmos> m_mediaSource;
#endif
    double m_startedAt { 0 };           /* the film's time when play() came */
    MonotonicTime m_playedAt;
    int64_t m_skipBefore { 0 };
    int64_t m_lastClock { -1 };         /* what tick() last saw, and since when */
    MonotonicTime m_clockStillSince;         /* after a seek: frames before it are not shown */
    Frame m_current;
    RefPtr<NativeImage> m_currentImage;
    mutable PlatformTimeRanges m_buffered;
    mutable size_t m_reportedBytes { 0 };
    unsigned m_decoded { 0 };
    unsigned m_dropped { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(KOSMOS)
