// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/Mpris.h"

#include "player/PlayerController.h"
#include "settings/ProfilePaths.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <algorithm>
#include <cmath>

namespace Kinefin {
namespace {
const QString kService = QStringLiteral("org.mpris.MediaPlayer2.Kinefin.profile_");
const QString kPlayerInterface = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString kNoTrack = QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack");
constexpr qint64 kTicksPerMicrosecond = 10;
// A time a bus peer sends is cut to this many microseconds (about 35 years) before it becomes ticks, so no multiplication overflows
constexpr qlonglong kMaxMicroseconds = 1LL << 50;

qlonglong boundedMicroseconds(qlonglong value)
{
    return std::clamp(value, -kMaxMicroseconds, kMaxMicroseconds);
}

QString loopStatusOf(int repeat)
{
    switch (repeat) {
    case 1:
        return QStringLiteral("Playlist");
    case 2:
        return QStringLiteral("Track");
    default:
        return QStringLiteral("None");
    }
}

QString playbackStatusOf(PlayerController::Status status)
{
    switch (status) {
    case PlayerController::Status::Playing:
        return QStringLiteral("Playing");
    case PlayerController::Status::Paused:
        return QStringLiteral("Paused");
    default:
        return QStringLiteral("Stopped");
    }
}
} // namespace

QString mprisServiceName(const QString &profileId)
{
    if (!validProfileId(profileId))
        return {};
    QString element = profileId;
    element.replace(QLatin1Char('.'), QLatin1Char('_')).replace(QLatin1Char('-'), QLatin1Char('_'));
    return kService + element;
}

QDBusObjectPath mprisTrackId(const QString &itemId)
{
    QString hex = itemId.toLower();
    hex.remove(QLatin1Char('-'));
    const bool valid =
        !hex.isEmpty() && hex.size() <= 64 && std::all_of(hex.cbegin(), hex.cend(), [](QChar c) {
            return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                   || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        });
    return QDBusObjectPath(valid ? QStringLiteral("/io/github/the_beper/Kinefin/track/") + hex
                                 : kNoTrack);
}

QString mprisText(const QString &text)
{
    // By code point: a format character outside the first plane (the tag characters) is a surrogate pair
    QString clean;
    for (const char32_t point : text.toUcs4()) {
        switch (QChar::category(point)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            continue;
        default:
            clean.append(QString::fromUcs4(&point, 1));
        }
    }
    clean.truncate(512);
    // The cut never leaves half a pair behind
    if (!clean.isEmpty() && clean.back().isHighSurrogate())
        clean.chop(1);
    return clean;
}

// org.mpris.MediaPlayer2: the application itself
class MprisRootAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)
  public:
    explicit MprisRootAdaptor(Mpris *mpris) : QDBusAbstractAdaptor(mpris), m_mpris(mpris) {}
    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Kinefin"); }
    QString desktopEntry() const { return QStringLiteral("io.github.the_beper.Kinefin"); }
    QStringList supportedUriSchemes() const { return {}; }
    QStringList supportedMimeTypes() const { return {}; }

  public Q_SLOTS:
    void Raise() { Q_EMIT m_mpris->raiseRequested(); }
    void Quit() { Q_EMIT m_mpris->quitRequested(); }

  private:
    Mpris *m_mpris;
};

// org.mpris.MediaPlayer2.Player: the controls, each a call into PlayerController that does nothing where the matching Can* is false
class MprisPlayerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)
  public:
    explicit MprisPlayerAdaptor(Mpris *mpris)
        : QDBusAbstractAdaptor(mpris), m_mpris(mpris), m_player(mpris->player())
    {
    }

    QString playbackStatus() const { return playbackStatusOf(m_player.status()); }
    QString loopStatus() const { return loopStatusOf(m_player.repeatMode()); }
    void setLoopStatus(const QString &status)
    {
        if (status == QLatin1String("None"))
            m_player.setRepeatMode(0);
        else if (status == QLatin1String("Playlist"))
            m_player.setRepeatMode(1);
        else if (status == QLatin1String("Track"))
            m_player.setRepeatMode(2);
    }
    double rate() const { return m_player.rate(); }
    void setRate(double value)
    {
        if (!std::isfinite(value))
            return;
        // The specification's way to pause
        if (value == 0) {
            if (m_player.status() == PlayerController::Status::Playing)
                m_player.pause();
            return;
        }
        m_player.setRate(
            std::clamp(value, PlayerController::MinimumRate, PlayerController::MaximumRate));
    }
    bool shuffle() const { return m_player.shuffle(); }
    void setShuffle(bool on) { m_player.setShuffle(on); }
    QVariantMap metadata() const { return m_mpris->metadata(); }
    double volume() const { return m_player.muted() ? 0.0 : m_player.volume() / 100.0; }
    void setVolume(double value)
    {
        if (!std::isfinite(value))
            return;
        const int level = int(std::lround(std::clamp(value, 0.0, 1.0) * 100));
        // Turning it up unmutes, as the player's own volume control does; a write that changes nothing reaches nothing (each change is a report to the server)
        if (level > 0 && m_player.muted())
            m_player.toggleMute();
        if (level != m_player.volume())
            m_player.setVolume(level);
    }
    // While an item loads, the engine still holds the previous one's time
    qlonglong position() const
    {
        return m_player.status() == PlayerController::Status::Stopped || m_player.loading()
                   ? 0
                   : m_player.positionTicks() / kTicksPerMicrosecond;
    }
    double minimumRate() const { return PlayerController::MinimumRate; }
    double maximumRate() const { return PlayerController::MaximumRate; }
    bool canGoNext() const { return m_player.hasNext(); }
    bool canGoPrevious() const { return m_player.hasPrevious(); }
    bool canPlay() const { return m_player.status() != PlayerController::Status::Stopped; }
    bool canPause() const { return canPlay(); }
    bool canSeek() const { return m_player.canSeek() && !m_player.loading(); }
    bool canControl() const { return true; }

  public Q_SLOTS:
    void Next()
    {
        if (m_player.hasNext())
            m_player.next();
    }
    void Previous()
    {
        if (m_player.hasPrevious())
            m_player.previous();
    }
    void Pause()
    {
        if (m_player.status() == PlayerController::Status::Playing)
            m_player.pause();
    }
    void PlayPause()
    {
        if (m_player.status() != PlayerController::Status::Stopped)
            m_player.togglePause();
    }
    void Stop() { m_player.stop(); }
    void Play()
    {
        if (m_player.status() == PlayerController::Status::Paused)
            m_player.resume();
    }
    void Seek(qlonglong offset)
    {
        if (m_player.canSeek())
            m_player.seek(std::max<qint64>(
                0, m_player.positionTicks() + boundedMicroseconds(offset) * kTicksPerMicrosecond));
    }
    // Ignored for another track, which the caller saw before a change, and for a position outside the item
    void SetPosition(const QDBusObjectPath &trackId, qlonglong position)
    {
        if (m_player.itemId().isEmpty() || position < 0 || position > kMaxMicroseconds)
            return;
        const qint64 ticks = position * kTicksPerMicrosecond;
        if (trackId != mprisTrackId(m_player.itemId())
            || (m_player.durationTicks() > 0 && ticks > m_player.durationTicks()))
            return;
        m_player.seek(ticks);
    }
    // Nothing to open: the app plays only what the server lists
    void OpenUri(const QString &) {}

  Q_SIGNALS:
    void Seeked(qlonglong position);

  private:
    Mpris *m_mpris;
    PlayerController &m_player;
};

Mpris::Mpris(PlayerController &player, const QString &profileId, const QDBusConnection &bus,
             QObject *parent)
    : QObject(parent), m_player(player), m_bus(bus), m_service(mprisServiceName(profileId))
{
    m_root = new MprisRootAdaptor(this);
    m_playerAdaptor = new MprisPlayerAdaptor(this);
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(0);
    connect(&m_coalesce, &QTimer::timeout, this, &Mpris::sendChanges);
    connect(&m_player, &PlayerController::changed, this, &Mpris::scheduleChange);
    connect(&m_player, &PlayerController::seeked, this, [this](qint64 ticks) {
        if (m_registered)
            Q_EMIT m_playerAdaptor->Seeked(ticks / kTicksPerMicrosecond);
    });
    m_sent = playerProperties();
}

Mpris::~Mpris()
{
    if (!m_registered)
        return;
    m_bus.unregisterService(m_service);
    m_bus.unregisterObject(QString::fromLatin1(ObjectPath));
}

bool Mpris::registerOnBus()
{
    if (m_registered)
        return true;
    if (m_service.isEmpty()) {
        qWarning("MPRIS is off: the profile id cannot form a bus name");
        return false;
    }
    if (!m_bus.isConnected()) {
        qWarning("MPRIS is off: no session bus");
        return false;
    }
    if (!m_bus.registerObject(QString::fromLatin1(ObjectPath), this,
                              QDBusConnection::ExportAdaptors)) {
        qWarning("MPRIS is off: the object path is taken");
        return false;
    }
    // Never queued and never handed over: one player per profile, the name is someone else's when taken
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
        m_bus.interface()->registerService(m_service, QDBusConnectionInterface::DontQueueService,
                                           QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        m_bus.unregisterObject(QString::fromLatin1(ObjectPath));
        qWarning("MPRIS is off: the bus name is taken");
        return false;
    }
    m_registered = true;
    m_sent = playerProperties();
    return true;
}

void Mpris::setArtwork(const QString &itemId, const QUrl &file)
{
    if (itemId != m_player.itemId())
        return;
    m_artItem = itemId;
    m_artFile = file;
    scheduleChange();
}

QVariantMap Mpris::metadata() const
{
    const QString itemId = m_player.itemId();
    QVariantMap values{
        {QStringLiteral("mpris:trackid"), QVariant::fromValue(mprisTrackId(itemId))}};
    if (itemId.isEmpty())
        return values;
    if (m_player.durationTicks() > 0 && !m_player.loading())
        values.insert(QStringLiteral("mpris:length"),
                      qlonglong(m_player.durationTicks() / kTicksPerMicrosecond));
    // An episode shows as its label and name by its series, anything else by its name
    const bool episode = !m_player.subtitle().isEmpty();
    const QString title = mprisText(episode ? m_player.subtitle() : m_player.title());
    if (!title.isEmpty())
        values.insert(QStringLiteral("xesam:title"), title);
    if (episode && !m_player.title().isEmpty())
        values.insert(QStringLiteral("xesam:artist"), QStringList{mprisText(m_player.title())});
    if (m_artItem == itemId && m_artFile.isValid())
        values.insert(QStringLiteral("mpris:artUrl"), m_artFile.toString());
    return values;
}

QVariantMap Mpris::playerProperties() const
{
    return {{QStringLiteral("PlaybackStatus"), m_playerAdaptor->playbackStatus()},
            {QStringLiteral("LoopStatus"), m_playerAdaptor->loopStatus()},
            {QStringLiteral("Rate"), m_playerAdaptor->rate()},
            {QStringLiteral("Shuffle"), m_playerAdaptor->shuffle()},
            {QStringLiteral("Metadata"), metadata()},
            {QStringLiteral("Volume"), m_playerAdaptor->volume()},
            {QStringLiteral("CanGoNext"), m_playerAdaptor->canGoNext()},
            {QStringLiteral("CanGoPrevious"), m_playerAdaptor->canGoPrevious()},
            {QStringLiteral("CanPlay"), m_playerAdaptor->canPlay()},
            {QStringLiteral("CanPause"), m_playerAdaptor->canPause()},
            {QStringLiteral("CanSeek"), m_playerAdaptor->canSeek()}};
}

void Mpris::scheduleChange()
{
    if (m_registered)
        m_coalesce.start();
}

void Mpris::sendChanges()
{
    const QVariantMap now = playerProperties();
    QVariantMap changed;
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        if (m_sent.value(it.key()) != it.value())
            changed.insert(it.key(), it.value());
    }
    m_sent = now;
    if (changed.isEmpty())
        return;
    QDBusMessage signal = QDBusMessage::createSignal(
        QString::fromLatin1(ObjectPath), QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"));
    signal << kPlayerInterface << changed << QStringList();
    m_bus.send(signal);
}
} // namespace Kinefin

#include "Mpris.moc"
