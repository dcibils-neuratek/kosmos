/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A SourceBuffer on Kosmos** (`docs/media.md` M3): where a page's
 * `appendBuffer()` goes, which is how YouTube plays - its player fetches a
 * film in pieces and hands each to the browser, rather than giving the
 * browser an address.
 *
 * **Two threads a source buffer.** The *demux* thread reads what was
 * appended through the Media Kit's `struct media_io`, as though it were
 * one file arriving slowly, and turns it into an initialization segment
 * and samples; WebCore's base class does the rest of MSE with them - the
 * timestamp offset, the append window, the buffered ranges, eviction. **An
 * append is finished when the demux thread asks for bytes and there are
 * none**: everything appended has been read and what it made has been
 * handed over, in order, on the page's thread, ahead of the answer.
 *
 * The *decode* thread decodes the samples WebCore enqueues - from the key
 * picture before the current time, again after a seek - and hands each
 * frame to the player's queue, which shows it when its time comes. A sound
 * track's samples are kept by WebCore and counted here, until the Playback
 * Kit plays them (M4).
 */

#pragma once

#if ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)

#include "SourceBufferPrivate.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/Threading.h>
#include <wtf/ThreadSafeWeakPtr.h>

#include <media.h>

namespace WebCore {

class MediaPlayerPrivateKosmos;
class MediaSampleKosmos;
class MediaSourcePrivateKosmos;

class SourceBufferPrivateKosmos final : public SourceBufferPrivate {
public:
    static Ref<SourceBufferPrivateKosmos> create(MediaSourcePrivateKosmos&, MediaPlayerPrivateKosmos&, const ContentType&);
    virtual ~SourceBufferPrivateKosmos();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Kosmos; }

    /* The Media Kit's `struct media_io`, on the demux thread. */
    long readBytes(uint8_t*, size_t);

private:
    SourceBufferPrivateKosmos(MediaSourcePrivateKosmos&, MediaPlayerPrivateKosmos&, const ContentType&);

    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    void removedFromMediaSource() final;
    bool canSwitchToType(const ContentType&) final;

    void flush(TrackID) final;
    void enqueueSample(Ref<MediaSample>&&, TrackID) final;
    void allSamplesInTrackEnqueued(TrackID) final;
    bool isReadyForMoreSamples(TrackID) final;
    void notifyClientWhenReadyForMoreSamples(TrackID) final;

    /* The demux thread, and what it hands to the page's thread. */
    void demux();
    void handOver();
    void emit(struct media_packet&, int64_t duration);
    void drained();
    void finishAppend(uint64_t consumed, bool failed);

    /* The decode thread. */
    void decode();
    void stopThreads();

    ThreadSafeWeakPtr<MediaPlayerPrivateKosmos> m_player;
    const char* m_container;    /* what `addSourceBuffer` said: "webm" or "mp4" */

    /* What has been appended, between the page's thread and the demuxer's. */
    Lock m_ioLock;
    Condition m_ioChanged;
    Vector<uint8_t> m_bytes WTF_GUARDED_BY_LOCK(m_ioLock);
    size_t m_read WTF_GUARDED_BY_LOCK(m_ioLock) { 0 };
    uint64_t m_consumed WTF_GUARDED_BY_LOCK(m_ioLock) { 0 };
    bool m_drained WTF_GUARDED_BY_LOCK(m_ioLock) { true };
    bool m_stop WTF_GUARDED_BY_LOCK(m_ioLock) { false };

    /* The page's thread only. */
    RefPtr<Thread> m_demuxer;
    std::optional<MediaPromise::Producer> m_appending;
    uint64_t m_appended { 0 };
    TrackID m_videoTrack { 0 };
    unsigned m_audioSamples { 0 };

    /* The demux thread only. */
    struct Track {
        TrackID id { 0 };
        bool video { false };
        float width { 0 }, height { 0 };
        int64_t lastDuration { 0 };
        std::optional<struct media_packet> held;
    };
    Vector<Track> m_tracks;
    Vector<Ref<MediaSampleKosmos>> m_batch;

    /* Between the page's thread and the decoder's. */
    Lock m_decodeLock;
    Condition m_decodeChanged;
    struct Enqueued {
        RefPtr<MediaSampleKosmos> sample;   /* null: the end, give up what is held */
        unsigned generation { 0 };
    };
    Deque<Enqueued> m_samples WTF_GUARDED_BY_LOCK(m_decodeLock);
    bool m_flushDecoder WTF_GUARDED_BY_LOCK(m_decodeLock) { false };
    bool m_wantsMore WTF_GUARDED_BY_LOCK(m_decodeLock) { false };
    bool m_stopDecoding WTF_GUARDED_BY_LOCK(m_decodeLock) { false };
    struct media_decoder* m_decoder WTF_GUARDED_BY_LOCK(m_decodeLock) { nullptr };
    RefPtr<Thread> m_decodeThread WTF_GUARDED_BY_LOCK(m_decodeLock);
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)
