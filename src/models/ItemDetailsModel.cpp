// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ItemDetailsModel.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTime>
#include <QVariantMap>

#include <algorithm>

namespace Kinefin {
namespace {
constexpr int kBackdropWidth = 1920;
constexpr int kLogoWidth = 600;
constexpr int kPosterWidth = 400;
constexpr int kPersonWidth = 240;
constexpr qint64 kTicksPerSecond = 10'000'000;
constexpr int kMaxBadgeText = 12;

QString clock(qint64 ticks)
{
    const qint64 seconds = ticks / kTicksPerSecond;
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600)
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// The width tells the class, since a wide film is letterboxed to fewer lines; the height stands in when the server sends no width
QString resolutionName(const StreamSummary &video)
{
    // The wider of the two readings: a 4:3 1440x1080 picture is 1080p by its height
    const int width = std::max(video.width, video.height * 16 / 9);
    if (width <= 0)
        return {};
    if (width >= 7000)
        return QStringLiteral("8K");
    if (width >= 3200)
        return QStringLiteral("4K");
    if (width >= 2400)
        return QStringLiteral("1440p");
    if (width >= 1800)
        return QStringLiteral("1080p");
    return width >= 1200 ? QStringLiteral("720p") : QStringLiteral("SD");
}

QString hdrName(const QString &rangeType)
{
    if (rangeType == QLatin1String("HDR10Plus"))
        return QStringLiteral("HDR10+");
    if (rangeType == QLatin1String("HDR10") || rangeType == QLatin1String("HLG"))
        return rangeType;
    // The Dolby Vision variants all start with DOVI; an invalid one is no badge
    if (rangeType.startsWith(QLatin1String("DOVI")) && rangeType != QLatin1String("DOVIInvalid"))
        return QStringLiteral("Dolby Vision");
    return {};
}

// A codec the server names with its ffmpeg token: the usual name, or else the token in capitals and capped
QString codecName(const QString &codec, const QHash<QString, QString> &names)
{
    const QString token = codec.toLower();
    const auto known = names.constFind(token);
    if (known != names.constEnd())
        return *known;
    if (token.startsWith(QLatin1String("pcm")))
        return QStringLiteral("PCM");
    return codec.toUpper().left(kMaxBadgeText);
}

QString videoCodecName(const QString &codec)
{
    static const QHash<QString, QString> names{
        {QStringLiteral("hevc"), QStringLiteral("HEVC")},
        {QStringLiteral("h264"), QStringLiteral("H.264")},
        {QStringLiteral("av1"), QStringLiteral("AV1")},
        {QStringLiteral("vp9"), QStringLiteral("VP9")},
        {QStringLiteral("vp8"), QStringLiteral("VP8")},
        {QStringLiteral("mpeg2video"), QStringLiteral("MPEG-2")},
        {QStringLiteral("mpeg4"), QStringLiteral("MPEG-4")},
        {QStringLiteral("vc1"), QStringLiteral("VC-1")}};
    return codecName(codec, names);
}

QString audioCodecName(const QString &codec)
{
    static const QHash<QString, QString> names{
        {QStringLiteral("truehd"), QStringLiteral("TrueHD")},
        {QStringLiteral("eac3"), QStringLiteral("Dolby Digital Plus")},
        {QStringLiteral("ac3"), QStringLiteral("Dolby Digital")},
        {QStringLiteral("dts"), QStringLiteral("DTS")},
        {QStringLiteral("aac"), QStringLiteral("AAC")},
        {QStringLiteral("flac"), QStringLiteral("FLAC")},
        {QStringLiteral("opus"), QStringLiteral("Opus")},
        {QStringLiteral("mp3"), QStringLiteral("MP3")},
        {QStringLiteral("vorbis"), QStringLiteral("Vorbis")},
        {QStringLiteral("alac"), QStringLiteral("ALAC")}};
    return codecName(codec, names);
}

// "5.1" from the server's layout ("5.1(side)", "stereo"), or from the channel count when the layout says nothing usable
QString channelLayoutName(const StreamSummary &audio)
{
    const QString layout = audio.channelLayout.toLower();
    if (layout == QLatin1String("mono"))
        return QStringLiteral("1.0");
    if (layout == QLatin1String("stereo"))
        return QStringLiteral("2.0");
    static const QRegularExpression numeric(QStringLiteral("^(\\d{1,2}\\.\\d)"));
    const QRegularExpressionMatch match = numeric.match(layout);
    if (match.hasMatch())
        return match.captured(1);
    switch (audio.channels) {
    case 1:
        return QStringLiteral("1.0");
    case 2:
        return QStringLiteral("2.0");
    case 6:
        return QStringLiteral("5.1");
    case 8:
        return QStringLiteral("7.1");
    default:
        return {};
    }
}

QVariantMap badge(const QString &kind, const QString &text)
{
    return {{QStringLiteral("kind"), kind}, {QStringLiteral("text"), text}};
}

QVariantMap streamMap(const StreamSummary &stream)
{
    return {{QStringLiteral("index"), stream.index},
            {QStringLiteral("type"), stream.type},
            {QStringLiteral("codec"), stream.codec},
            {QStringLiteral("language"), stream.language},
            {QStringLiteral("title"), stream.title},
            {QStringLiteral("width"), stream.width},
            {QStringLiteral("height"), stream.height},
            {QStringLiteral("channels"), stream.channels},
            {QStringLiteral("bitrate"), stream.bitrate},
            {QStringLiteral("isDefault"), stream.isDefault},
            {QStringLiteral("isForced"), stream.isForced},
            {QStringLiteral("isExternal"), stream.isExternal}};
}
} // namespace

ItemDetailsModel::ItemDetailsModel(JellyfinClient *client, const QString &itemId, int channel,
                                   QObject *parent)
    : QObject(parent), m_client(client), m_itemId(itemId), m_channel(channel),
      m_similar(client, false, this), m_below(client, channel + BelowChannelOffset, this)
{
    if (m_client) {
        connect(m_client, &JellyfinClient::itemsReady, this, &ItemDetailsModel::onItemsReady);
        connect(m_client, &JellyfinClient::similarItemsReady, this,
                &ItemDetailsModel::onSimilarReady);
        connect(m_client, &JellyfinClient::listingFailed, this, &ItemDetailsModel::onListingFailed);
        connect(m_client, &JellyfinClient::itemFavoriteUpdated, this,
                [this](const QString &id, bool favorite) {
                    const auto canonical = [](QString value) {
                        return value.remove(QLatin1Char('-')).toLower();
                    };
                    if (canonical(id) != canonical(m_itemId))
                        return;
                    if (m_details.valid) {
                        const bool changed = m_details.favorite != favorite;
                        m_details.favorite = favorite;
                        if (changed)
                            Q_EMIT detailsChanged();
                    }
                    if (m_favoriteBusy || m_favoriteError) {
                        m_favoriteBusy = false;
                        m_favoriteError = false;
                        Q_EMIT favoriteWriteStateChanged();
                    }
                });
        connect(m_client, &JellyfinClient::itemFavoriteUpdateFailed, this,
                [this](const QString &id, bool, int) {
                    if (!m_favoriteBusy
                        || QString(id).remove(QLatin1Char('-')).toLower()
                               != QString(m_itemId).remove(QLatin1Char('-')).toLower())
                        return;
                    m_favoriteBusy = false;
                    m_favoriteError = true;
                    Q_EMIT favoriteWriteStateChanged();
                });
        // The same item may be visible on multiple detail pages at once.
        connect(m_client, &JellyfinClient::itemPlayedUpdated, this,
                [this](const QString &id, bool played) {
                    const auto canonical = [](QString value) {
                        return value.remove(QLatin1Char('-')).toLower();
                    };
                    if (canonical(id) != canonical(m_itemId))
                        return;
                    if (m_details.valid) {
                        m_details.played = played;
                        // A manual watched-state change invalidates any old resume point.
                        // The next Refresh fetches the server's authoritative user data.
                        m_details.resumePositionTicks = 0;
                        if (playable())
                            m_details.playedPercentage = played ? 100 : 0;
                        Q_EMIT detailsChanged();
                    }
                    if (m_playedBusy || m_playedError) {
                        m_playedBusy = false;
                        m_playedError = false;
                        Q_EMIT playedWriteStateChanged();
                    }
                });
        // The Undo of a Mark as Watched gives the resume point back
        connect(m_client, &JellyfinClient::itemProgressUpdated, this,
                [this](const QString &id, qint64 position, double percentage) {
                    if (!m_details.valid
                        || QString(id).remove(QLatin1Char('-')).toLower()
                               != QString(m_itemId).remove(QLatin1Char('-')).toLower())
                        return;
                    m_details.resumePositionTicks = position;
                    m_details.playedPercentage = percentage;
                    Q_EMIT detailsChanged();
                });
        connect(m_client, &JellyfinClient::itemPlayedUpdateFailed, this,
                [this](const QString &id, bool, int) {
                    if (!m_playedBusy
                        || QString(id).remove(QLatin1Char('-')).toLower()
                               != QString(m_itemId).remove(QLatin1Char('-')).toLower())
                        return;
                    m_playedBusy = false;
                    m_playedError = true;
                    Q_EMIT playedWriteStateChanged();
                });
        // A 401, a disconnect, a server switch and a sign-out all end with this signal
        connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
    }
    reload();
}

ItemDetailsModel::~ItemDetailsModel()
{
    // The page is gone: its requests have nobody to answer, and the client forgets the channel
    if (m_client) {
        m_client->releaseListingChannel(m_channel);
        m_client->releaseListingChannel(m_channel + BelowChannelOffset);
    }
}

bool ItemDetailsModel::playable() const
{
    return isPlayableVideo(m_details.type);
}

bool ItemDetailsModel::hasSimilar() const
{
    // The server answers an empty envelope for these: no request
    return m_details.type != QLatin1String("Episode") && m_details.type != QLatin1String("Season");
}

void ItemDetailsModel::reload()
{
    ItemQuery query;
    query.itemIds = {m_itemId};
    query.fields = {QStringLiteral("Overview"),      QStringLiteral("Genres"),
                    QStringLiteral("People"),        QStringLiteral("Chapters"),
                    QStringLiteral("MediaSources"),  QStringLiteral("Taglines"),
                    QStringLiteral("OriginalTitle"), QStringLiteral("Studios"),
                    QStringLiteral("CanDownload"),   QStringLiteral("CanDelete"),
                    QStringLiteral("RemoteTrailers")};
    m_reloadOverlapsFavoriteWrite = m_favoriteBusy;
    m_reloadOverlapsPlayedWrite = m_playedBusy;
    setStatus(Loading);
    if (!m_client || !m_client->fetchItems(query, m_channel))
        setStatus(Failed);
}

void ItemDetailsModel::retrySimilar()
{
    if (m_status == Ready && m_similarStatus == Failed)
        requestSimilar();
}

bool ItemDetailsModel::toggleFavorite()
{
    if (!m_client || m_status != Ready || !m_details.valid || m_favoriteBusy)
        return false;
    m_favoriteBusy = true;
    m_favoriteError = false;
    Q_EMIT favoriteWriteStateChanged();
    if (!m_client->setItemFavorite(m_itemId, !m_details.favorite)) {
        m_favoriteBusy = false;
        m_favoriteError = true;
        Q_EMIT favoriteWriteStateChanged();
        return false;
    }
    return true;
}

bool ItemDetailsModel::togglePlayed()
{
    // Parent series/seasons have aggregated watched state. Do not pretend that
    // marking the parent alone marks all child episodes as watched.
    if (!m_client || m_status != Ready || !m_details.valid || !playable() || m_playedBusy)
        return false;
    m_playedBusy = true;
    m_playedError = false;
    Q_EMIT playedWriteStateChanged();
    // A rewatch stopped halfway is in progress, not watched: the server keeps Played on it, and the toggle finishes it
    const bool watched = m_details.played && m_details.resumePositionTicks <= 0;
    if (!m_client->setItemPlayed(m_itemId, !watched)) {
        m_playedBusy = false;
        m_playedError = true;
        Q_EMIT playedWriteStateChanged();
        return false;
    }
    return true;
}

void ItemDetailsModel::requestSimilar()
{
    if (!hasSimilar()) {
        m_similar.setItems({});
        setSimilarStatus(Idle);
        return;
    }
    // The old similar items stay until the answer replaces them
    setSimilarStatus(Loading);
    if (!m_client || !m_client->fetchSimilarItems(m_itemId, SimilarLimit, m_channel))
        setSimilarStatus(Failed);
}

void ItemDetailsModel::onItemsReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_status != Loading)
        return;
    if (items.isEmpty()) {
        // The item is gone (or never was visible to this user)
        m_details = {};
        m_similar.setItems({});
        m_below.clear();
        Q_EMIT detailsChanged();
        setSimilarStatus(Idle);
        setStatus(NotFound);
        return;
    }
    ItemDetails parsed = parseItemDetails(items.first().toObject());
    // Ids compare without case and without dashes: the same GUID is written both ways
    const auto bare = [](QString id) { return id.remove(QLatin1Char('-')).toLower(); };
    if (!parsed.valid || bare(parsed.id) != bare(m_itemId)) {
        setStatus(Failed);
        return;
    }
    // The write's own answer (or its failure) is the newer truth than a read that raced it
    if (m_reloadOverlapsFavoriteWrite && m_details.valid)
        parsed.favorite = m_details.favorite;
    if (m_reloadOverlapsPlayedWrite && m_details.valid) {
        parsed.played = m_details.played;
        parsed.resumePositionTicks = m_details.resumePositionTicks;
        parsed.playedPercentage = m_details.playedPercentage;
    }
    m_details = parsed;
    Q_EMIT detailsChanged();
    setStatus(Ready);
    m_below.load(m_details);
    requestSimilar();
}

void ItemDetailsModel::onSimilarReady(const QJsonArray &items, int, int channel)
{
    if (channel != m_channel || m_similarStatus != Loading)
        return;
    m_similar.setItems(items);
    setSimilarStatus(Ready);
}

void ItemDetailsModel::onListingFailed(const QString &family, int, const QString &, int channel)
{
    if (channel != m_channel)
        return;
    if (family == QLatin1String("Items") && m_status == Loading)
        setStatus(Failed);
    else if (family == QLatin1String("Similar") && m_similarStatus == Loading)
        setSimilarStatus(Failed);
}

void ItemDetailsModel::clear()
{
    if (m_favoriteBusy || m_favoriteError) {
        m_favoriteBusy = false;
        m_favoriteError = false;
        Q_EMIT favoriteWriteStateChanged();
    }
    if (m_playedBusy || m_playedError) {
        m_playedBusy = false;
        m_playedError = false;
        Q_EMIT playedWriteStateChanged();
    }
    m_details = {};
    m_similar.setItems({});
    m_below.clear();
    Q_EMIT detailsChanged();
    setSimilarStatus(Idle);
    if (m_status != NotFound)
        setStatus(Idle);
}

void ItemDetailsModel::setStatus(Status status)
{
    if (status == m_status)
        return;
    m_status = status;
    Q_EMIT statusChanged();
}

void ItemDetailsModel::setSimilarStatus(Status status)
{
    if (status == m_similarStatus)
        return;
    m_similarStatus = status;
    Q_EMIT similarStatusChanged();
}

QUrl ItemDetailsModel::backdropUrl() const
{
    return m_client && !m_details.backdropTag.isEmpty()
               ? m_client->imageUrl(m_details.backdropItemId, QStringLiteral("Backdrop"),
                                    kBackdropWidth, m_details.backdropTag)
               : QUrl();
}

QUrl ItemDetailsModel::logoUrl() const
{
    return m_client && !m_details.logoTag.isEmpty()
               ? m_client->imageUrl(m_details.logoItemId, QStringLiteral("Logo"), kLogoWidth,
                                    m_details.logoTag)
               : QUrl();
}

QUrl ItemDetailsModel::posterUrl() const
{
    return m_client && !m_details.primaryTag.isEmpty() && !m_details.id.isEmpty()
               ? m_client->imageUrl(m_details.id, QStringLiteral("Primary"), kPosterWidth,
                                    m_details.primaryTag)
               : QUrl();
}

QVariantList ItemDetailsModel::people() const
{
    QVariantList result;
    for (const PersonSummary &person : m_details.people) {
        QUrl image;
        if (m_client && !person.id.isEmpty() && !person.primaryTag.isEmpty())
            image = m_client->imageUrl(person.id, QStringLiteral("Primary"), kPersonWidth,
                                       person.primaryTag);
        result.append(QVariantMap{{QStringLiteral("id"), person.id},
                                  {QStringLiteral("name"), person.name},
                                  {QStringLiteral("role"), person.role},
                                  {QStringLiteral("type"), person.type},
                                  {QStringLiteral("imageUrl"), image}});
    }
    return result;
}

QVariantList ItemDetailsModel::trailers() const
{
    QVariantList result;
    for (const TrailerSummary &trailer : m_details.trailers) {
        result.append(QVariantMap{{QStringLiteral("name"), trailer.name},
                                  {QStringLiteral("url"), trailer.url}});
    }
    return result;
}

QVariantList ItemDetailsModel::chapters() const
{
    QVariantList result;
    for (const ChapterSummary &chapter : m_details.chapters) {
        result.append(QVariantMap{{QStringLiteral("name"), chapter.name},
                                  {QStringLiteral("startTicks"), chapter.startTicks},
                                  {QStringLiteral("time"), clock(chapter.startTicks)}});
    }
    return result;
}

QVariantList ItemDetailsModel::mediaBadges() const
{
    QVariantList result;
    if (m_details.sources.isEmpty())
        return result;
    const StreamSummary *video = nullptr;
    const StreamSummary *audio = nullptr;
    int subtitles = 0;
    for (const StreamSummary &stream : m_details.sources.constFirst().streams) {
        if (stream.type == QLatin1String("Video") && !video)
            video = &stream;
        else if (stream.type == QLatin1String("Audio")
                 && (!audio || (stream.isDefault && !audio->isDefault)))
            audio = &stream;
        else if (stream.type == QLatin1String("Subtitle"))
            ++subtitles;
    }
    if (video) {
        const QString resolution = resolutionName(*video);
        if (!resolution.isEmpty())
            result.append(badge(QStringLiteral("resolution"), resolution));
        const QString hdr = hdrName(video->videoRangeType);
        if (!hdr.isEmpty())
            result.append(badge(QStringLiteral("hdr"), hdr));
        if (!video->codec.isEmpty())
            result.append(badge(QStringLiteral("video"), videoCodecName(video->codec)));
    }
    if (audio && !audio->codec.isEmpty()) {
        const QString layout = channelLayoutName(*audio);
        const QString name = audioCodecName(audio->codec);
        result.append(badge(QStringLiteral("audio"),
                            layout.isEmpty() ? name : name + QLatin1Char(' ') + layout));
    }
    if (subtitles > 0)
        result.append(badge(QStringLiteral("subtitles"), QString::number(subtitles)));
    return result;
}

QVariantList ItemDetailsModel::mediaSources() const
{
    QVariantList result;
    for (const SourceSummary &source : m_details.sources) {
        QVariantList video;
        QVariantList audio;
        QVariantList subtitles;
        for (const StreamSummary &stream : source.streams) {
            if (stream.type == QLatin1String("Video"))
                video.append(streamMap(stream));
            else if (stream.type == QLatin1String("Audio"))
                audio.append(streamMap(stream));
            else if (stream.type == QLatin1String("Subtitle"))
                subtitles.append(streamMap(stream));
        }
        result.append(QVariantMap{{QStringLiteral("id"), source.id},
                                  {QStringLiteral("name"), source.name},
                                  {QStringLiteral("container"), source.container},
                                  {QStringLiteral("size"), source.size},
                                  {QStringLiteral("bitrate"), source.bitrate},
                                  {QStringLiteral("video"), video},
                                  {QStringLiteral("audio"), audio},
                                  {QStringLiteral("subtitles"), subtitles}});
    }
    return result;
}
} // namespace Kinefin
