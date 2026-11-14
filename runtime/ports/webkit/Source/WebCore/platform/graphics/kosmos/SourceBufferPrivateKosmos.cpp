/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A SourceBuffer on Kosmos (`SourceBufferPrivateKosmos.h`, `docs/media.md`
 * M3).
 */

#include "config.h"
#include "SourceBufferPrivateKosmos.h"

#if ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)

#include "AudioTrackPrivate.h"
#include "ContentType.h"
#include "MediaDescription.h"
#include "MediaPlayerPrivateKosmos.h"
#include "MediaSample.h"
#include "MediaSourcePrivateKosmos.h"
#include "SharedBuffer.h"
#include "VideoTrackPrivate.h"
#include <wtf/MainThread.h>
#include <wtf/NativePromise.h>
#include <wtf/PrintStream.h>

namespace WebCore {

/* How many samples wait for the decoder before WebCore is told to hold the
 * rest: about half a second of film, and a few kilobytes each. */
static constexpr size_t samplesAhead = 16;

/* How many samples go to the page's thread in one task. */
static constexpr size_t batchOf = 32;

/* A duration for the last sample of an append when its container did not
 * say one - WebM's blocks seldom do - and there is no next to measure it
 * by: the one before it, or these. */
static constexpr int64_t pictureGuess = 33333, soundGuess = 20000;

static MediaTime fromUs(int64_t us)
{
    return MediaTime(us, 1000000);
}

static int64_t toUs(const MediaTime& time)
{
    return time.toTimeScale(1000000).timeValue();
}

/* A codec's name as MSE compares them: a FourCC, `vp09` and `opus`. */
static String fourCC(const char* codec)
{
    static constexpr std::pair<ASCIILiteral, ASCIILiteral> names[] = {
        { "vp9"_s, "vp09"_s }, { "vp8"_s, "vp08"_s }, { "av1"_s, "av01"_s },
        { "h264"_s, "avc1"_s }, { "hevc"_s, "hev1"_s }, { "opus"_s, "opus"_s },
        { "aac"_s, "mp4a"_s }, { "vorbis"_s, "vorb"_s }, { "flac"_s, "fLaC"_s },
        { "mp3"_s, ".mp3"_s },
    };
    auto name = String::fromLatin1(codec);
    for (auto& [ours, theirs] : names) {
        if (name == ours)
            return theirs;
    }
    return name;
}

// ---------------------------------------------------------------------------
// What WebCore keeps: a track, its description, and a sample - the Media
// Kit's packet, whose bytes are FFmpeg's and shared by every copy.

class KosmosVideoTrack final : public VideoTrackPrivate {
public:
    static Ref<KosmosVideoTrack> create(TrackID id) { return adoptRef(*new KosmosVideoTrack(id)); }
    TrackID id() const final { return m_id; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit KosmosVideoTrack(TrackID id)
        : m_id(id)
    {
    }
    TrackID m_id;
};

class KosmosAudioTrack final : public AudioTrackPrivate {
public:
    static Ref<KosmosAudioTrack> create(TrackID id) { return adoptRef(*new KosmosAudioTrack(id)); }
    TrackID id() const final { return m_id; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit KosmosAudioTrack(TrackID id)
        : m_id(id)
    {
    }
    TrackID m_id;
};

class KosmosMediaDescription final : public MediaDescription {
public:
    static Ref<KosmosMediaDescription> create(String&& codec, bool video) { return adoptRef(*new KosmosMediaDescription(WTF::move(codec), video)); }
    bool isVideo() const final { return m_video; }
    bool isAudio() const final { return !m_video; }
    bool isText() const final { return false; }

private:
    KosmosMediaDescription(String&& codec, bool video)
        : MediaDescription(WTF::move(codec))
        , m_video(video)
    {
    }
    bool m_video;
};

class KosmosPacket final : public ThreadSafeRefCounted<KosmosPacket> {
public:
    static Ref<KosmosPacket> create(const struct media_packet& packet) { return adoptRef(*new KosmosPacket(packet)); }
    ~KosmosPacket() { media_packet_free(&m_packet); }
    const struct media_packet& packet() const { return m_packet; }

private:
    explicit KosmosPacket(const struct media_packet& packet)
        : m_packet(packet)
    {
    }
    struct media_packet m_packet;
};

class MediaSampleKosmos final : public MediaSample {
public:
    static Ref<MediaSampleKosmos> create(Ref<KosmosPacket>&& packet, TrackID track, MediaTime pts, MediaTime dts, MediaTime duration, unsigned flags, FloatSize size)
    {
        return adoptRef(*new MediaSampleKosmos(WTF::move(packet), track, pts, dts, duration, flags, size));
    }

    MediaTime presentationTime() const final { return m_pts; }
    MediaTime decodeTime() const final { return m_dts; }
    MediaTime duration() const final { return m_duration; }
    TrackID trackID() const final { return m_track; }
    size_t sizeInBytes() const final { return m_packet->packet().size; }
    FloatSize presentationSize() const final { return m_size; }
    void offsetTimestampsBy(const MediaTime& offset) final
    {
        m_pts += offset;
        m_dts += offset;
    }
    void setTimestamps(const MediaTime& pts, const MediaTime& dts) final
    {
        m_pts = pts;
        m_dts = dts;
    }
    SampleFlags flags() const final { return static_cast<SampleFlags>(m_flags); }
    PlatformSample platformSample() const final { return PlatformSample { static_cast<const MockSampleBox*>(nullptr) }; }
    Type type() const final { return Type::None; }
    void dump(PrintStream& out) const final
    {
        out.print("{PTS(", m_pts, "), DTS(", m_dts, "), duration(", m_duration, "), flags(", m_flags, ")}");
    }
    Ref<MediaSample> createNonDisplayingCopy() const final
    {
        return create(m_packet.copyRef(), m_track, m_pts, m_dts, m_duration, m_flags | IsNonDisplaying, m_size);
    }

    /* The packet as the decoder is handed it: WebCore's times, which the
     * timestamp offset has moved, and the container's bytes. */
    struct media_packet forDecoder() const
    {
        struct media_packet out = m_packet->packet();
        out.own = nullptr;
        out.pts_us = toUs(m_pts);
        out.dts_us = toUs(m_dts);
        out.duration_us = toUs(m_duration);
        out.key = (m_flags & IsSync) != 0;
        return out;
    }
    bool displays() const { return !(m_flags & IsNonDisplaying); }

private:
    MediaSampleKosmos(Ref<KosmosPacket>&& packet, TrackID track, MediaTime pts, MediaTime dts, MediaTime duration, unsigned flags, FloatSize size)
        : m_packet(WTF::move(packet))
        , m_track(track)
        , m_pts(pts)
        , m_dts(dts)
        , m_duration(duration)
        , m_flags(flags)
        , m_size(size)
    {
    }

    Ref<KosmosPacket> m_packet;
    TrackID m_track;
    MediaTime m_pts, m_dts, m_duration;
    unsigned m_flags;
    FloatSize m_size;
};

// ---------------------------------------------------------------------------

Ref<SourceBufferPrivateKosmos> SourceBufferPrivateKosmos::create(MediaSourcePrivateKosmos& source, MediaPlayerPrivateKosmos& player, const ContentType& type)
{
    return adoptRef(*new SourceBufferPrivateKosmos(source, player, type));
}

SourceBufferPrivateKosmos::SourceBufferPrivateKosmos(MediaSourcePrivateKosmos& source, MediaPlayerPrivateKosmos& player, const ContentType& type)
    : SourceBufferPrivate(source)
    , m_player(player)
    , m_container(type.containerType().endsWithIgnoringASCIICase("/webm"_s) ? "webm" : "mp4")
{
}

SourceBufferPrivateKosmos::~SourceBufferPrivateKosmos()
{
    stopThreads();
}

void SourceBufferPrivateKosmos::stopThreads()
{
    {
        Locker locker { m_ioLock };
        m_stop = true;
        m_ioChanged.notifyAll();
    }
    if (m_demuxer) {
        m_demuxer->waitForCompletion();
        m_demuxer = nullptr;
    }
    // A decode thread waiting for room in a paused player's queue is let
    // go: a new generation turns its wait into a drop.
    if (RefPtr player = m_player.get()) {
        player->flushFrames();
        player->flushSound();
    }
    RefPtr<Thread> decoder;
    {
        Locker locker { m_decodeLock };
        m_stopDecoding = true;
        m_decodeChanged.notifyAll();
        decoder = std::exchange(m_decodeThread, nullptr);
    }
    if (decoder)
        decoder->waitForCompletion();
    Locker locker { m_decodeLock };
    if (m_decoder) {
        media_decoder_close(m_decoder);
        m_decoder = nullptr;
    }
}

void SourceBufferPrivateKosmos::removedFromMediaSource()
{
    stopThreads();
    SourceBufferPrivate::removedFromMediaSource();
}

bool SourceBufferPrivateKosmos::canSwitchToType(const ContentType& type)
{
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = type
    };
    return MediaPlayerPrivateKosmos::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
}

// ---------------------------------------------------------------------------
// An append: the bytes to the demuxer, and the answer when it has read them.

Ref<MediaPromise> SourceBufferPrivateKosmos::appendInternal(Ref<SharedBuffer>&& data)
{
    auto bytes = data->span();
    m_appended += bytes.size();
    {
        Locker locker { m_ioLock };
        m_bytes.append(bytes);
        m_drained = false;
        m_ioChanged.notifyAll();
    }
    m_appending.emplace();
    Ref promise = m_appending->promise();
    if (!m_demuxer) {
        m_demuxer = Thread::create("Kosmos demux"_s, [this] {
            demux();
        });
    }
    return promise;
}

void SourceBufferPrivateKosmos::finishAppend(uint64_t consumed, bool failed)
{
    if (!m_appending)
        return;
    if (failed) {
        std::exchange(m_appending, std::nullopt)->reject(PlatformMediaError::ParsingError);
        return;
    }
    // Only the answer to the newest bytes: a drain posted before an append
    // arrived is about bytes that came before it.
    if (consumed == m_appended)
        std::exchange(m_appending, std::nullopt)->resolve();
}

/* Unread bytes are dropped; what FFmpeg holds of a box it was reading is
 * not, which is the one part of `abort()` this does not do yet - YouTube
 * appends whole segments, so there is seldom a box half read. */
void SourceBufferPrivateKosmos::resetParserStateInternal()
{
    Locker locker { m_ioLock };
    m_consumed += m_bytes.size() - m_read;
    m_bytes.clear();
    m_read = 0;
}

long SourceBufferPrivateKosmos::readBytes(uint8_t* into, size_t room)
{
    Locker locker { m_ioLock };
    while (m_read >= m_bytes.size() && !m_stop) {
        if (!m_drained) {
            m_drained = true;
            uint64_t consumed = m_consumed;
            locker.unlockEarly();
            drained();
            callOnMainThread([weak = ThreadSafeWeakPtr { *this }, consumed] {
                if (RefPtr self = weak.get())
                    self->finishAppend(consumed, false);
            });
            return readBytes(into, room);
        }
        m_ioChanged.wait(m_ioLock);
    }
    if (m_stop)
        return -1;
    size_t n = std::min(room, m_bytes.size() - m_read);
    memcpy(into, m_bytes.span().subspan(m_read).data(), n);
    m_read += n;
    m_consumed += n;
    // What has been read is not read again: the front goes once it is
    // large, so a long film does not keep every byte it was handed.
    if (m_read >= (1u << 20)) {
        m_bytes.removeAt(0, m_read);
        m_read = 0;
    }
    return static_cast<long>(n);
}

extern "C" {

static long appendedRead(void* context, uint8_t* into, size_t room)
{
    return static_cast<SourceBufferPrivateKosmos*>(context)->readBytes(into, room);
}

}

// ---------------------------------------------------------------------------
// The demux thread.

void SourceBufferPrivateKosmos::demux()
{
    struct media_io io { };
    io.ctx = this;
    io.read = appendedRead;
    io.seek = nullptr;
    io.size = -1;

    auto fail = [this] {
        bool stopping;
        {
            Locker locker { m_ioLock };
            stopping = m_stop;
        }
        if (!stopping) {
            callOnMainThread([weak = ThreadSafeWeakPtr { *this }] {
                if (RefPtr self = weak.get())
                    self->finishAppend(0, true);
            });
        }
    };

    char why[160];
    struct media_source* source = media_open_appended(&io, m_container, why, sizeof why);
    if (!source) {
        fail();
        return;
    }

    // The initialization segment: a track for each picture and sound
    // stream, and a decoder for the first picture - or, in a buffer with
    // none, the first sound, which is how YouTube appends them.
    SourceBufferPrivateClient::InitializationSegment segment;
    int64_t duration = MEDIA_NO_TIME;
    struct media_decoder* decoder = nullptr;
    TrackID decodedTrack = 0;
    bool decodesVideo = false;
    int firstAudio = -1;
    m_tracks.clear();
    for (int i = 0; i < media_stream_count(source); i++) {
        struct media_stream stream;
        Track track;
        if (media_stream(source, i, &stream) || (stream.kind != MEDIA_VIDEO && stream.kind != MEDIA_AUDIO)) {
            m_tracks.append(track);
            continue;
        }
        track.id = static_cast<TrackID>(i + 1);
        track.video = stream.kind == MEDIA_VIDEO;
        if (stream.duration_us != MEDIA_NO_TIME)
            duration = std::max(duration, stream.duration_us);
        if (track.video) {
            track.width = stream.width;
            track.height = stream.height;
            segment.videoTracks.append({ KosmosMediaDescription::create(fourCC(stream.codec), true), KosmosVideoTrack::create(track.id) });
            if (!decoder) {
                decoder = media_decoder_open(source, i, 0, why, sizeof why);
                decodedTrack = track.id;
                decodesVideo = true;
            }
        } else {
            segment.audioTracks.append({ KosmosMediaDescription::create(fourCC(stream.codec), false), KosmosAudioTrack::create(track.id) });
            if (firstAudio < 0)
                firstAudio = i;
        }
        m_tracks.append(WTF::move(track));
    }
    segment.duration = duration != MEDIA_NO_TIME ? fromUs(duration) : MediaTime::invalidTime();
    if (!decoder && firstAudio >= 0) {
        decoder = media_decoder_open(source, firstAudio, 0, why, sizeof why);
        decodedTrack = static_cast<TrackID>(firstAudio + 1);
    }
    if (!decoder && decodedTrack)
        WTFLogAlways("media: no decoder for the %s - %s", decodesVideo ? "picture" : "sound", why);

    if (decoder) {
        Locker locker { m_decodeLock };
        m_decoder = decoder;
        m_decodeThread = Thread::create("Kosmos decode"_s, [this] {
            decode();
        });
    }
    FloatSize size;
    for (auto& track : m_tracks) {
        if (decodesVideo && track.id == decodedTrack)
            size = FloatSize(track.width, track.height);
    }
    callOnMainThread([weak = ThreadSafeWeakPtr { *this }, segment = WTF::move(segment), decodedTrack, decodesVideo, size]() mutable {
        RefPtr self = weak.get();
        if (!self)
            return;
        self->m_decodedTrack = decodedTrack;
        self->m_decodesVideo = decodesVideo;
        if (RefPtr player = self->m_player.get(); player && decodedTrack) {
            if (decodesVideo)
                player->videoTrackFound(size);
            else
                player->audioTrackFound();
        }
        self->didReceiveInitializationSegment(WTF::move(segment));
    });

    // The samples: each handed over with a duration, which a WebM block
    // seldom carries - so one is held until the next of its track says how
    // long it lasted.
    while (true) {
        struct media_packet packet;
        int got = media_read(source, &packet);
        if (got <= 0) {
            if (got < 0)
                fail();
            break;
        }
        if (packet.stream < 0 || static_cast<size_t>(packet.stream) >= m_tracks.size() || !m_tracks[packet.stream].id) {
            media_packet_free(&packet);
            continue;
        }
        if (packet.dts_us == MEDIA_NO_TIME)
            packet.dts_us = packet.pts_us;
        if (packet.pts_us == MEDIA_NO_TIME)
            packet.pts_us = packet.dts_us;
        if (packet.pts_us == MEDIA_NO_TIME) {
            media_packet_free(&packet);
            continue;
        }
        auto& track = m_tracks[packet.stream];
        if (track.held) {
            auto held = *std::exchange(track.held, std::nullopt);
            int64_t lasted = packet.dts_us - held.dts_us;
            if (lasted > 0)
                track.lastDuration = lasted;
            emit(held, track.lastDuration);
        }
        if (packet.duration_us > 0) {
            track.lastDuration = packet.duration_us;
            emit(packet, packet.duration_us);
        } else
            track.held = packet;
        if (m_batch.size() >= batchOf)
            handOver();
    }

    for (auto& track : m_tracks) {
        if (track.held)
            media_packet_free(&*track.held);
    }
    m_batch.clear();
    media_close(source);
}

void SourceBufferPrivateKosmos::emit(struct media_packet& packet, int64_t duration)
{
    auto& track = m_tracks[packet.stream];
    if (duration <= 0)
        duration = track.video ? pictureGuess : soundGuess;
    unsigned flags = (packet.key || !track.video) ? MediaSample::IsSync : MediaSample::None;
    FloatSize size = track.video ? FloatSize(track.width, track.height) : FloatSize();
    m_batch.append(MediaSampleKosmos::create(KosmosPacket::create(packet), track.id,
        fromUs(packet.pts_us), fromUs(packet.dts_us), fromUs(duration), flags, size));
}

/* Everything appended has been read: what is held goes too, with the
 * duration of the one before it, and the batch is handed over. */
void SourceBufferPrivateKosmos::drained()
{
    for (auto& track : m_tracks) {
        if (track.held) {
            auto held = *std::exchange(track.held, std::nullopt);
            emit(held, track.lastDuration);
        }
    }
    handOver();
}

void SourceBufferPrivateKosmos::handOver()
{
    if (m_batch.isEmpty())
        return;
    callOnMainThread([weak = ThreadSafeWeakPtr { *this }, batch = std::exchange(m_batch, { })]() mutable {
        RefPtr self = weak.get();
        if (!self)
            return;
        for (auto& sample : batch)
            self->didReceiveSample(WTF::move(sample));
    });
}

// ---------------------------------------------------------------------------
// What WebCore enqueues to be shown, and the decode thread that shows it.

bool SourceBufferPrivateKosmos::isReadyForMoreSamples(TrackID track)
{
    if (track != m_decodedTrack)
        return true;
    Locker locker { m_decodeLock };
    return m_samples.size() < samplesAhead;
}

void SourceBufferPrivateKosmos::notifyClientWhenReadyForMoreSamples(TrackID track)
{
    if (track != m_decodedTrack)
        return;
    Locker locker { m_decodeLock };
    m_wantsMore = true;
}

void SourceBufferPrivateKosmos::enqueueSample(Ref<MediaSample>&& sample, TrackID track)
{
    if (track != m_decodedTrack) {
        m_otherSamples++;
        return;
    }
    RefPtr player = m_player.get();
    if (!player)
        return;
    unsigned generation = m_decodesVideo ? player->frameGeneration() : player->soundGeneration();
    Locker locker { m_decodeLock };
    m_samples.append({ static_cast<MediaSampleKosmos*>(sample.ptr()), generation });
    m_decodeChanged.notifyAll();
}

void SourceBufferPrivateKosmos::allSamplesInTrackEnqueued(TrackID track)
{
    if (track != m_decodedTrack)
        return;
    RefPtr player = m_player.get();
    if (!player)
        return;
    unsigned generation = m_decodesVideo ? player->frameGeneration() : player->soundGeneration();
    Locker locker { m_decodeLock };
    m_samples.append({ nullptr, generation });
    m_decodeChanged.notifyAll();
}

void SourceBufferPrivateKosmos::flush(TrackID track)
{
    if (track != m_decodedTrack)
        return;
    {
        Locker locker { m_decodeLock };
        m_samples.clear();
        m_flushDecoder = true;
        m_decodeChanged.notifyAll();
    }
    if (RefPtr player = m_player.get()) {
        if (m_decodesVideo)
            player->flushFrames();
        else
            player->flushSound();
    }
}

void SourceBufferPrivateKosmos::decode()
{
    struct media_decoder* decoder;
    {
        Locker locker { m_decodeLock };
        decoder = m_decoder;
    }
    while (true) {
        Enqueued next;
        bool flush;
        bool ask = false;
        {
            Locker locker { m_decodeLock };
            while (!m_stopDecoding && m_samples.isEmpty() && !m_flushDecoder)
                m_decodeChanged.wait(m_decodeLock);
            if (m_stopDecoding)
                break;
            flush = std::exchange(m_flushDecoder, false);
            if (!m_samples.isEmpty())
                next = m_samples.takeFirst();
            if (m_wantsMore && m_samples.size() < samplesAhead / 2) {
                m_wantsMore = false;
                ask = true;
            }
        }
        if (ask) {
            callOnMainThread([weak = ThreadSafeWeakPtr { *this }] {
                RefPtr self = weak.get();
                if (self && self->m_decodedTrack)
                    self->provideMediaData(self->m_decodedTrack);
            });
        }
        if (flush)
            media_decoder_flush(decoder);
        if (!next.sample && !flush) {
            // The end of the stream: what the decoder holds comes out, and
            // it is ready for more after.
            media_decode(decoder, nullptr);
        } else if (next.sample) {
            auto packet = next.sample->forDecoder();
            media_decode(decoder, &packet);
        } else
            continue;

        RefPtr player = m_player.get();
        struct media_frame frame;
        while (media_frame(decoder, &frame) == 1) {
            if (!player)
                continue;
            if (frame.kind == MEDIA_AUDIO) {
                if (!player->deliverSound(frame.pts_us, frame, next.generation))
                    return;
                continue;
            }
            auto image = MediaPlayerPrivateKosmos::picture(frame);
            if (image && !player->deliver(frame.pts_us, WTF::move(image), next.generation))
                return;
        }
        if (!next.sample)
            media_decoder_flush(decoder);
    }
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)
