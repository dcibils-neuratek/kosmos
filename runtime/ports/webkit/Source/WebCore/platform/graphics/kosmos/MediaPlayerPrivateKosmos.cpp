/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Kosmos's media engine on the Media Kit (`MediaPlayerPrivateKosmos.h`,
 * `docs/media.md` M2).
 */

#include "config.h"
#include "MediaPlayerPrivateKosmos.h"

#if ENABLE(VIDEO) && PLATFORM(KOSMOS)

#include "ContentType.h"
#include "GraphicsContext.h"
#include "MediaPlayer.h"
#include "NativeImage.h"
#include "PlatformMediaResourceLoader.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include <skia/core/SkData.h>
#include <skia/core/SkImage.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

extern "C" {
#include <media.h>
#include <yuv.h>
}

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateKosmos);

/* How many frames the decode thread keeps ready ahead of the clock. */
static constexpr size_t framesAhead = 4;

/* What a frame's time is shown at: the counter while there is no sound. */
static constexpr Seconds tickEvery = 1_s / 120.;

// ---------------------------------------------------------------------------
// What it plays: what the Media Kit decodes, of what a page can show. Sound
// alone is M4's, when there is something to play it through.

static const HashSet<String>& containers()
{
    static NeverDestroyed<HashSet<String>> types = HashSet<String> {
        "video/webm"_s, "video/mp4"_s, "video/x-m4v"_s, "video/quicktime"_s,
        "video/ogg"_s, "video/x-matroska"_s,
    };
    return types;
}

static bool decodes(const String& codec)
{
    static constexpr ASCIILiteral prefixes[] = {
        "avc1"_s, "avc3"_s, "av01"_s, "vp8"_s, "vp9"_s, "vp09"_s, "vp08"_s,
        "opus"_s, "vorbis"_s, "mp4a"_s, "flac"_s, "mp3"_s, "theora"_s,
    };
    for (auto prefix : prefixes) {
        if (codec.startsWithIgnoringASCIICase(prefix))
            return true;
    }
    return false;
}

MediaPlayer::SupportsType MediaPlayerPrivateKosmos::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto container = parameters.type.containerType().convertToASCIILowercase();
    if (container.isEmpty() || !containers().contains(container))
        return MediaPlayer::SupportsType::IsNotSupported;
    auto codecs = parameters.type.codecs();
    if (codecs.isEmpty())
        return MediaPlayer::SupportsType::MayBeSupported;
    for (auto& codec : codecs) {
        if (!decodes(codec))
            return MediaPlayer::SupportsType::IsNotSupported;
    }
    return MediaPlayer::SupportsType::IsSupported;
}

void MediaPlayerPrivateKosmos::getSupportedTypes(HashSet<String>& types)
{
    types = containers();
}

class MediaPlayerFactoryKosmos final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(MediaPlayerFactoryKosmos);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactoryKosmos);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::Kosmos; }
    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new MediaPlayerPrivateKosmos(player));
    }
    void getSupportedTypes(HashSet<String>& types) const final { MediaPlayerPrivateKosmos::getSupportedTypes(types); }
    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return MediaPlayerPrivateKosmos::supportsType(parameters);
    }
};

void MediaPlayerPrivateKosmos::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryKosmos>());
}

// ---------------------------------------------------------------------------
// The page's loader: the response, the bytes and the end, each handed to the
// player on the page's thread.

class KosmosMediaResourceClient final : public PlatformMediaResourceClient {
public:
    static Ref<KosmosMediaResourceClient> create(MediaPlayerPrivateKosmos& player)
    {
        return adoptRef(*new KosmosMediaResourceClient(player));
    }

private:
    explicit KosmosMediaResourceClient(MediaPlayerPrivateKosmos& player)
        : m_player(player)
    {
    }

    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completion) final
    {
        RefPtr player = m_player.get();
        if (player) {
            if (response.httpStatusCode() >= 400)
                player->finished(true);
            else
                player->responded(response.expectedContentLength());
        }
        completion(ShouldContinuePolicyCheck::Yes);
    }

    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final
    {
        if (RefPtr player = m_player.get())
            player->arrived(buffer.span());
    }

    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError&) final
    {
        if (RefPtr player = m_player.get())
            player->finished(true);
    }

    void loadFailed(PlatformMediaResource&, const ResourceError&) final
    {
        if (RefPtr player = m_player.get())
            player->finished(true);
    }

    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final
    {
        if (RefPtr player = m_player.get())
            player->finished(false);
    }

    ThreadSafeWeakPtr<MediaPlayerPrivateKosmos> m_player;
};

// ---------------------------------------------------------------------------

MediaPlayerPrivateKosmos::MediaPlayerPrivateKosmos(MediaPlayer& player)
    : m_player(player)
    , m_ticker(RunLoop::mainSingleton(), "MediaPlayerPrivateKosmos::tick"_s, this, &MediaPlayerPrivateKosmos::tick)
{
}

MediaPlayerPrivateKosmos::~MediaPlayerPrivateKosmos()
{
    cancelLoad();
}

template<typename F>
void MediaPlayerPrivateKosmos::onMain(F&& work)
{
    callOnMainThread([weak = ThreadSafeWeakPtr { *this }, work = std::forward<F>(work)]() mutable {
        if (RefPtr self = weak.get())
            work(*self);
    });
}

void MediaPlayerPrivateKosmos::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateKosmos::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateKosmos::load(const String& address)
{
    RefPtr player = m_player.get();
    if (!player)
        return;
    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);

    m_resource = player->mediaResourceLoader()->requestResource(ResourceRequest(URL { address }), 0);
    if (!m_resource) {
        setNetworkState(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    m_resource->setClient(KosmosMediaResourceClient::create(*this));
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateKosmos::load(const URL&, const LoadOptions&, MediaSourcePrivateClient&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateKosmos::load(MediaStreamPrivate&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

void MediaPlayerPrivateKosmos::stopThread()
{
    {
        Locker locker { m_lock };
        m_stop = true;
        m_changed.notifyAll();
    }
    if (m_thread) {
        m_thread->waitForCompletion();
        m_thread = nullptr;
    }
}

void MediaPlayerPrivateKosmos::cancelLoad()
{
    m_ticker.stop();
    if (m_resource) {
        m_resource->shutdown();
        m_resource = nullptr;
    }
    stopThread();
}

// ---------------------------------------------------------------------------
// The bytes, as they arrive.

void MediaPlayerPrivateKosmos::responded(long long expectedLength)
{
    {
        Locker locker { m_lock };
        m_expected = expectedLength > 0 ? expectedLength : -1;
    }
    if (m_thread)
        return;
    m_thread = Thread::create("Kosmos media"_s, [this] {
        decode();
    });
}

void MediaPlayerPrivateKosmos::arrived(std::span<const uint8_t> bytes)
{
    Locker locker { m_lock };
    m_bytes.append(bytes);
    m_changed.notifyAll();
}

void MediaPlayerPrivateKosmos::finished(bool failed)
{
    {
        Locker locker { m_lock };
        m_complete = true;
        m_changed.notifyAll();
    }
    if (failed) {
        setNetworkState(MediaPlayer::NetworkState::NetworkError);
        return;
    }
    setNetworkState(MediaPlayer::NetworkState::Loaded);
}

long MediaPlayerPrivateKosmos::readBytes(uint8_t* into, size_t room)
{
    Locker locker { m_lock };
    while (m_position >= m_bytes.size() && !m_complete && !m_stop)
        m_changed.wait(m_lock);
    if (m_stop)
        return -1;
    if (m_position >= m_bytes.size())
        return 0;
    size_t n = std::min(room, m_bytes.size() - m_position);
    memcpy(into, m_bytes.span().subspan(m_position).data(), n);
    m_position += n;
    return static_cast<long>(n);
}

int64_t MediaPlayerPrivateKosmos::seekBytes(int64_t offset, int whence)
{
    Locker locker { m_lock };
    int64_t to;
    if (whence == 0)
        to = offset;
    else if (whence == 1)
        to = static_cast<int64_t>(m_position) + offset;
    else {
        // From the end: the end is known when the server said the length, or
        // when everything has arrived.
        while (m_expected < 0 && !m_complete && !m_stop)
            m_changed.wait(m_lock);
        int64_t end = m_expected >= 0 ? m_expected : static_cast<int64_t>(m_bytes.size());
        to = end + offset;
    }
    if (to < 0 || m_stop)
        return -1;
    m_position = static_cast<size_t>(to);
    return to;
}

extern "C" {

static long ioRead(void* context, uint8_t* into, size_t room)
{
    return static_cast<MediaPlayerPrivateKosmos*>(context)->readBytes(into, room);
}

static int64_t ioSeek(void* context, int64_t offset, int whence)
{
    return static_cast<MediaPlayerPrivateKosmos*>(context)->seekBytes(offset, whence);
}

}

// ---------------------------------------------------------------------------
// The decode thread: the container, the picture's decoder, and frames into
// the queue as pixels.

void MediaPlayerPrivateKosmos::decode()
{
    struct media_io io { };
    io.ctx = this;
    io.read = ioRead;
    io.seek = ioSeek;
    {
        Locker locker { m_lock };
        io.size = m_expected;
    }

    char why[160];
    struct media_source* source = media_open(&io, why, sizeof why);
    if (!source) {
        onMain([](auto& self) { self.setNetworkState(MediaPlayer::NetworkState::FormatError); });
        return;
    }

    int picture = -1;
    struct media_stream stream { };
    for (int i = 0; i < media_stream_count(source); i++) {
        struct media_stream candidate;
        if (media_stream(source, i, &candidate) == 0 && candidate.kind == MEDIA_VIDEO) {
            picture = i;
            stream = candidate;
            break;
        }
    }
    struct media_decoder* decoder = picture >= 0 ? media_decoder_open(source, picture, 0, why, sizeof why) : nullptr;
    if (!decoder) {
        media_close(source);
        onMain([](auto& self) { self.setNetworkState(MediaPlayer::NetworkState::FormatError); });
        return;
    }

    onMain([width = stream.width, height = stream.height, duration = stream.duration_us](auto& self) {
        self.m_size = FloatSize(width, height);
        self.m_hasVideo = true;
        self.m_duration = duration;
        if (RefPtr player = self.m_player.get()) {
            player->characteristicChanged();
            player->durationChanged();
            player->sizeChanged();
        }
        self.setReadyState(MediaPlayer::ReadyState::HaveMetadata);
    });

    // BT.709 for a high-definition picture and BT.601 below it, which is what
    // a film that says nothing was made with.
    const struct gfx_yuv_matrix* matrix = stream.height >= 720 ? &gfx_yuv_bt709 : &gfx_yuv_bt601;
    bool ended = false;

    while (true) {
        {
            Locker locker { m_lock };
            while (!m_stop && !m_seekTo && (m_queue.size() >= framesAhead || ended))
                m_changed.wait(m_lock);
            if (m_stop)
                break;
            if (auto to = std::exchange(m_seekTo, std::nullopt)) {
                m_queue.clear();
                m_endOfFilm = false;
                ended = false;
                media_seek(source, *to);
                media_decoder_flush(decoder);
                continue;
            }
        }

        struct media_packet packet;
        int got = media_read(source, &packet);
        if (got < 0)
            break;
        if (got > 0 && packet.stream != picture) {
            media_packet_free(&packet);
            continue;
        }
        media_decode(decoder, got > 0 ? &packet : nullptr);
        if (got > 0)
            media_packet_free(&packet);

        struct media_frame frame;
        int out;
        while ((out = media_frame(decoder, &frame)) == 1) {
            if (frame.format != MEDIA_YUV420 || frame.bits != 8 || frame.planes < 3)
                continue;
            auto info = SkImageInfo::Make(frame.width, frame.height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
            size_t pitch = info.minRowBytes();
            auto pixels = SkData::MakeUninitialized(pitch * frame.height);
            gfx_i420(static_cast<uint32_t*>(pixels->writable_data()), pitch,
                frame.plane[0], frame.plane[1], frame.plane[2],
                frame.stride[0], frame.stride[1], frame.stride[2],
                frame.width, frame.height, matrix);
            auto image = SkImages::RasterFromData(info, std::move(pixels), pitch);
            bool first;
            {
                Locker locker { m_lock };
                first = m_queue.isEmpty() && !m_endOfFilm;
                m_queue.append({ frame.pts_us, std::move(image) });
            }
            if (first)
                onMain([](auto& self) { self.tick(); });
        }
        if (got == 0 || out == -1) {
            Locker locker { m_lock };
            m_endOfFilm = true;
            ended = true;
        }
    }

    media_decoder_close(decoder);
    media_close(source);
}

// ---------------------------------------------------------------------------
// The clock, and what is shown.

MediaTime MediaPlayerPrivateKosmos::duration() const
{
    if (m_duration < 0)
        return m_readyState >= MediaPlayer::ReadyState::HaveMetadata ? MediaTime::positiveInfiniteTime() : MediaTime::zeroTime();
    return MediaTime(m_duration, 1000000);
}

MediaTime MediaPlayerPrivateKosmos::currentTime() const
{
    double now = m_startedAt;
    if (!m_paused)
        now += (MonotonicTime::now() - m_playedAt).seconds();
    if (m_duration >= 0)
        now = std::min(now, m_duration / 1e6);
    return MediaTime::createWithDouble(now);
}

void MediaPlayerPrivateKosmos::play()
{
    if (!m_paused)
        return;
    if (m_ended) {
        m_ended = false;
        m_startedAt = 0;
        Locker locker { m_lock };
        m_seekTo = 0;
        m_changed.notifyAll();
    }
    m_paused = false;
    m_playedAt = MonotonicTime::now();
    m_ticker.startRepeating(tickEvery);
    onMain([](auto& self) {
        if (RefPtr player = self.m_player.get())
            player->playbackStateChanged();
    });
}

void MediaPlayerPrivateKosmos::pause()
{
    if (m_paused)
        return;
    m_startedAt = currentTime().toDouble();
    m_paused = true;
    m_ticker.stop();
    onMain([](auto& self) {
        if (RefPtr player = self.m_player.get())
            player->playbackStateChanged();
    });
}

Ref<MediaTimePromise> MediaPlayerPrivateKosmos::seekToTarget(const SeekTarget& target)
{
    auto time = target.time;
    if (m_duration >= 0 && time.toDouble() > m_duration / 1e6)
        time = MediaTime(m_duration, 1000000);
    m_startedAt = time.toDouble();
    m_playedAt = MonotonicTime::now();
    m_ended = false;
    m_skipBefore = static_cast<int64_t>(m_startedAt * 1e6);
    {
        Locker locker { m_lock };
        m_seekTo = m_skipBefore;
        m_changed.notifyAll();
    }
    onMain([](auto& self) {
        if (RefPtr player = self.m_player.get())
            player->timeChanged();
    });
    return MediaTimePromise::createAndResolve(time);
}

void MediaPlayerPrivateKosmos::tick()
{
    int64_t now = static_cast<int64_t>(currentTime().toDouble() * 1e6);
    std::optional<Frame> show;
    bool endOfFilm;
    {
        Locker locker { m_lock };
        // The newest frame whose time has come; any before it were late.
        while (!m_queue.isEmpty() && (m_queue.first().pts <= now || (!m_current.image && !show))) {
            auto frame = m_queue.takeFirst();
            if (frame.pts < m_skipBefore && !m_queue.isEmpty())
                continue;
            if (show)
                m_dropped++;
            show = WTF::move(frame);
            if (!m_current.image && m_paused)
                break;
        }
        endOfFilm = m_endOfFilm && m_queue.isEmpty();
        m_changed.notifyAll();
    }

    RefPtr player = m_player.get();
    if (show) {
        bool first = !m_current.image;
        m_current = WTF::move(*show);
        m_currentImage = nullptr;
        m_decoded++;
        if (player) {
            if (first) {
                setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
                player->firstVideoFrameAvailable();
            }
            player->repaint();
        }
    }

    if (endOfFilm && !m_paused && m_duration >= 0 && now >= m_duration) {
        m_startedAt = m_duration / 1e6;
        m_paused = true;
        m_ended = true;
        m_ticker.stop();
        if (player)
            player->timeChanged();
    }
}

void MediaPlayerPrivateKosmos::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled() || !m_current.image)
        return;
    if (RefPtr image = nativeImageForCurrentTime())
        context.drawNativeImage(*image, rect, FloatRect({ }, image->size()));
}

RefPtr<NativeImage> MediaPlayerPrivateKosmos::nativeImageForCurrentTime()
{
    if (!m_current.image)
        return nullptr;
    if (!m_currentImage)
        m_currentImage = NativeImage::create(sk_sp<SkImage>(m_current.image));
    return m_currentImage;
}

DestinationColorSpace MediaPlayerPrivateKosmos::colorSpace()
{
    return DestinationColorSpace::SRGB();
}

// ---------------------------------------------------------------------------
// What has arrived.

unsigned long long MediaPlayerPrivateKosmos::totalBytes() const
{
    Locker locker { m_lock };
    return m_expected > 0 ? m_expected : m_bytes.size();
}

bool MediaPlayerPrivateKosmos::didLoadingProgress() const
{
    Locker locker { m_lock };
    bool progressed = m_bytes.size() != m_reportedBytes;
    m_reportedBytes = m_bytes.size();
    return progressed;
}

const PlatformTimeRanges& MediaPlayerPrivateKosmos::buffered() const
{
    m_buffered.clear();
    if (m_duration <= 0)
        return m_buffered;
    double share;
    {
        Locker locker { m_lock };
        share = m_complete ? 1.0 : (m_expected > 0 ? static_cast<double>(m_bytes.size()) / m_expected : 0.0);
    }
    if (share > 0)
        m_buffered.add(MediaTime::zeroTime(), MediaTime::createWithDouble(std::min(share, 1.0) * m_duration / 1e6));
    return m_buffered;
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(KOSMOS)
