/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A MediaSource on Kosmos (`MediaSourcePrivateKosmos.h`, `docs/media.md` M3).
 */

#include "config.h"
#include "MediaSourcePrivateKosmos.h"

#if ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)

#include "ContentType.h"
#include "MediaPlayerPrivateKosmos.h"
#include "MediaSourcePrivateClient.h"
#include "SourceBufferPrivateKosmos.h"

namespace WebCore {

Ref<MediaSourcePrivateKosmos> MediaSourcePrivateKosmos::create(MediaPlayerPrivateKosmos& player, MediaSourcePrivateClient& client)
{
    Ref source = adoptRef(*new MediaSourcePrivateKosmos(player, client));
    client.setPrivateAndOpen(source.copyRef());
    return source;
}

MediaSourcePrivateKosmos::MediaSourcePrivateKosmos(MediaPlayerPrivateKosmos& player, MediaSourcePrivateClient& client)
    : MediaSourcePrivate(client)
    , m_player(player)
{
}

MediaSourcePrivateKosmos::~MediaSourcePrivateKosmos() = default;

RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateKosmos::player() const
{
    return m_player.get();
}

void MediaSourcePrivateKosmos::setPlayer(MediaPlayerPrivateInterface* player)
{
    m_player = static_cast<MediaPlayerPrivateKosmos*>(player);
}

MediaSourcePrivate::AddStatus MediaSourcePrivateKosmos::addSourceBuffer(const ContentType& type, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& out)
{
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = type
    };
    if (MediaPlayerPrivateKosmos::supportsType(parameters) == MediaPlayer::SupportsType::IsNotSupported)
        return AddStatus::NotSupported;
    RefPtr player = m_player.get();
    if (!player)
        return AddStatus::NotSupported;
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(SourceBufferPrivateKosmos::create(*this, *player, type));
        out = m_sourceBuffers.last();
    }
    out->setMediaSourceDuration(duration());
    return AddStatus::Ok;
}

void MediaSourcePrivateKosmos::durationChanged(const MediaTime& duration)
{
    MediaSourcePrivate::durationChanged(duration);
    callOnMainThread([weak = m_player] {
        if (RefPtr player = weak.get())
            player->mediaSourceDurationChanged();
    });
}

void MediaSourcePrivateKosmos::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->activeSourceBuffersChanged();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE) && PLATFORM(KOSMOS)
