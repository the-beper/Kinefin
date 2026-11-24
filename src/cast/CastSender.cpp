// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastSender.h"

#include "cast/UpnpDescription.h"

#include <QHostInfo>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace Kinefin {
namespace {
Q_LOGGING_CATEGORY(senderLog, "kinefin.cast.sender")

constexpr QLatin1StringView kMediaReceiverApp{"CC1AD845"};
constexpr QLatin1StringView kAmbientApp{"E8C28D3C"};
constexpr int kMaxContentIdBytes = 1024;
constexpr qint64 kMaxStartMs = 24LL * 3600 * 1000;
constexpr qsizetype kMaxStatusEntries = 8;
constexpr qsizetype kMaxApplications = 8;
constexpr qsizetype kMaxSessionIdChars = 256;
constexpr qsizetype kMaxTitleChars = 512;
constexpr qsizetype kMaxTrackNameChars = 128;
constexpr qsizetype kMaxLanguageChars = 35;
// The device's state names are short; a longer one is cut in the log
constexpr qsizetype kMaxLoggedStatusChars = 32;
constexpr int kReconnectSearchMs = 3000;
// The channel is aborted this long after its CLOSE frames were written, so that the peer reads them first
constexpr int kCloseGraceMs = 250;
constexpr double kMaxPlaybackRate = 4.0;
// A FINISHED this far before the duration is not the end of the item
constexpr qint64 kEndToleranceMs = 10'000;
// The server's first extraction of a text track from a large file took 142 to 159 s
constexpr int kTrackStartExtraMs = 180'000;
constexpr qint64 kWarmupMaxBytes = 2 * 1024 * 1024;
constexpr int kWarmupTimeoutMs = 240'000;

const QString kReceiverId = QString(kCastPlatformId);
const QString kReceiverNs = QString(kCastNsReceiver);
const QString kMediaNs = QString(kCastNsMedia);

QJsonObject typedMessage(QLatin1StringView type)
{
    return QJsonObject{{QStringLiteral("type"), QString(type)}};
}

QString urlText(const QUrl &url)
{
    return url.toString(QUrl::FullyEncoded);
}

// The server's token must never reach the device: a URL that names one in its query is refused, whatever the key's case
bool queryCarriesToken(const QUrl &url)
{
    static const QStringList keys = {QStringLiteral("api_key"),
                                     QStringLiteral("apikey"),
                                     QStringLiteral("token"),
                                     QStringLiteral("access_token"),
                                     QStringLiteral("accesstoken"),
                                     QStringLiteral("x-emby-token"),
                                     QStringLiteral("x-mediabrowser-token")};
    const QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    return std::any_of(items.cbegin(), items.cend(), [](const auto &item) {
        return keys.contains(item.first, Qt::CaseInsensitive);
    });
}

bool urlAcceptable(const QUrl &url, const QUrl &base)
{
    const bool web =
        url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https");
    return url.isValid() && web && url.userInfo().isEmpty() && isSameOrigin(url, base)
           && !queryCarriesToken(url) && urlText(url).toUtf8().size() <= kMaxContentIdBytes;
}

quint32 requestIdOf(const QJsonObject &json)
{
    const QJsonValue value = json.value(QLatin1String("requestId"));
    if (!value.isDouble())
        return 0;
    const double number = value.toDouble();
    if (number < 1 || number > std::numeric_limits<quint32>::max() || number != std::floor(number))
        return 0;
    return static_cast<quint32>(number);
}

qint64 sessionIdOf(const QJsonObject &entry)
{
    const QJsonValue value = entry.value(QLatin1String("mediaSessionId"));
    if (!value.isDouble())
        return 0;
    const double number = value.toDouble();
    return number >= 1 && number < 9.0e15 ? static_cast<qint64>(number) : 0;
}

qint64 millisecondsOf(double seconds)
{
    if (!(seconds > 0))
        return 0;
    return static_cast<qint64>(std::llround(std::min(seconds * 1000.0, double(kMaxStartMs))));
}

QUrl withHost(const QUrl &url, const QString &from, const QString &host)
{
    if (host.isEmpty() || url.isEmpty() || url.host().compare(from, Qt::CaseInsensitive) != 0)
        return url;
    QUrl copy = url;
    copy.setHost(host);
    return copy;
}
} // namespace

const char *CastSender::phaseName(Phase phase)
{
    switch (phase) {
    case Phase::Idle:
        return "idle";
    case Phase::Preparing:
        return "preparing";
    case Phase::Connecting:
        return "connecting";
    case Phase::Launching:
        return "launching";
    case Phase::Loading:
        return "loading";
    case Phase::Active:
        return "active";
    case Phase::Reconnecting:
        return "reconnecting";
    case Phase::Stopping:
        return "stopping";
    }
    return "unknown";
}

QString CastSender::targetId(const CastDeviceInfo &info)
{
    return QStringLiteral("cast:") + info.id;
}

CastSender::CastSender(CastDeviceInfo info, const Options &options, QObject *parent)
    : CastRenderer(parent), m_info(std::move(info)), m_options(options)
{
    m_network.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    m_statusClock.start();
    connect(&m_probe, &StreamProbe::finished, this, &CastSender::onProbeFinished);

    for (QTimer *timer :
         {&m_startTimer, &m_stuckTimer, &m_stepTimer, &m_stopTimer, &m_reconnectTimer})
        timer->setSingleShot(true);
    connect(&m_startTimer, &QTimer::timeout, this, &CastSender::onStartTimeout);
    m_stuckTimer.setInterval(std::max(1, m_options.stuckMs));
    connect(&m_stuckTimer, &QTimer::timeout, this, [this] {
        if (!m_started && (m_phase == Phase::Loading || m_phase == Phase::Active)) {
            qCWarning(senderLog) << "no moving position after the reply to the load";
            fail(Failure::Refused);
        }
    });
    connect(&m_stepTimer, &QTimer::timeout, this, &CastSender::onStepTimeout);
    connect(&m_stopTimer, &QTimer::timeout, this, &CastSender::proceedStop);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &CastSender::reconnectAttempt);
    m_pollTimer.setInterval(std::max(1, m_options.statusPollMs));
    connect(&m_pollTimer, &QTimer::timeout, this, &CastSender::pollStatus);
    m_tickTimer.setInterval(std::max(1, m_options.positionTickMs));
    connect(&m_tickTimer, &QTimer::timeout, this, &CastSender::tick);
}

CastSender::CastSender(CastDeviceInfo info, QObject *parent)
    : CastSender(std::move(info), Options(), parent)
{
}

CastSender::~CastSender()
{
    m_probe.abort();
    abortWarmup();
    if (m_channel) {
        // No signal of the channel may reach this object while it is being destroyed
        QObject::disconnect(m_channel, nullptr, this, nullptr);
        delete m_channel;
        m_channel = nullptr;
    }
}

bool CastSender::load(const CastLoad &request)
{
    if (!urlAcceptable(request.url, request.serverBase) || request.startMs < 0
        || request.startMs > kMaxStartMs)
        return false;
    // The text track and the poster are optional: one that fails the rules is left out
    const bool hasTrack = !request.track.url.isEmpty();
    const bool hasImage = !request.imageUrl.isEmpty();
    const bool keepTrack = hasTrack && urlAcceptable(request.track.url, request.serverBase);
    const bool keepImage = hasImage && urlAcceptable(request.imageUrl, request.serverBase);
    // A new load after a stop does not wait for the stop to end: it replaces the media anyway
    if (m_phase == Phase::Stopping)
        finishSession();
    else if (m_phase != Phase::Idle)
        return false;
    if (hasTrack && !keepTrack)
        qCInfo(senderLog) << "the text track is left out";
    if (hasImage && !keepImage)
        qCInfo(senderLog) << "the poster is left out";

    ++m_generation;
    m_originalBase = request.serverBase;
    m_baseHost = request.serverBase.host();
    m_resolvedHost.clear();
    m_url = request.url;
    m_trackUrl = keepTrack ? request.track.url : QUrl();
    m_imageUrl = keepImage ? request.imageUrl : QUrl();
    m_contentId.clear();
    m_mime = request.mime;
    m_title = request.title.left(kMaxTitleChars);
    m_subtitle = request.subtitle.left(kMaxTitleChars);
    m_trackLanguage = request.track.language.left(kMaxLanguageChars);
    m_trackName = request.track.name.left(kMaxTrackNameChars);
    m_direct = request.direct;
    m_durationMs = std::clamp<qint64>(request.durationMs, 0, kMaxStartMs);
    m_baseMs = request.startMs;
    m_offsetMs = request.direct ? 0 : request.startMs;
    m_loadStartSec = request.direct ? static_cast<double>(request.startMs) / 1000.0 : 0.0;
    m_rate = 1.0;
    m_started = false;
    m_loggedStatus.clear();
    m_transport.reset();
    m_sessionId.clear();
    m_transportId.clear();
    m_sessionListed = false;
    m_launchRequest = 0;
    m_launchRetried = false;
    m_resumeRequest = 0;
    m_loadRequestId = 0;
    m_loadInFlight = false;
    m_mediaSessionId = 0;
    m_oldMediaSessionId = 0;
    m_firstLeaf.clear();
    m_candidateAddress.clear();
    m_candidatePort = 0;
    m_attempt = 0;
    m_resume = Resume::None;
    m_statusClock.restart();
    m_phase = Phase::Preparing;
    qCDebug(senderLog) << "load" << (m_direct ? "direct" : "transcode");
    m_startTimer.start(startDeadlineMs());
    prepare();
    return true;
}

void CastSender::prepare()
{
    QHostAddress literal;
    const bool named = m_originalBase.scheme() == QLatin1String("http") && !m_baseHost.isEmpty()
                       && !literal.setAddress(hostWithoutBrackets(m_originalBase));
    if (!named) {
        proceedAfterResolve();
        return;
    }
    const quint64 generation = m_generation;
    const QPointer<CastSender> self(this);
    lookupHost(m_baseHost, [self, generation](const QHostAddress &address) {
        if (self.isNull() || generation != self->m_generation || self->m_phase != Phase::Preparing)
            return;
        self->applyResolved(address);
        self->proceedAfterResolve();
    });
}

void CastSender::lookupHost(const QString &host, std::function<void(const QHostAddress &)> done)
{
    if (m_options.resolveHost) {
        m_options.resolveHost(host, std::move(done));
        return;
    }
    const AddressPolicy policy =
        m_options.mediaAddressPolicy ? m_options.mediaAddressPolicy : &isAllowedRendererAddress;
    QHostInfo::lookupHost(host, this, [policy, done = std::move(done)](const QHostInfo &info) {
        const QList<QHostAddress> addresses = info.addresses();
        for (const QHostAddress &address : addresses) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol && policy(address)) {
                done(address);
                return;
            }
        }
        done(QHostAddress());
    });
}

void CastSender::findDevice(const QString &deviceId,
                            std::function<void(const std::optional<CastDeviceInfo> &)> done)
{
    if (m_options.resolveDevice) {
        m_options.resolveDevice(deviceId, std::move(done));
        return;
    }
    MdnsSearch::Options search;
    search.windowMs = kReconnectSearchMs;
    auto *finder = new MdnsSearch(search, this);
    const auto reported = std::make_shared<bool>(false);
    const auto finish = [finder, reported, done](const std::optional<CastDeviceInfo> &device) {
        if (*reported)
            return;
        *reported = true;
        finder->stop();
        finder->deleteLater();
        done(device);
    };
    connect(finder, &MdnsSearch::deviceFound, finder,
            [deviceId, finish](const CastDeviceInfo &found) {
                if (found.id == deviceId)
                    finish(found);
            });
    connect(finder, &MdnsSearch::finished, finder, [finish] { finish(std::nullopt); });
    if (!finder->start())
        finish(std::nullopt);
}

void CastSender::applyResolved(const QHostAddress &address)
{
    const AddressPolicy policy =
        m_options.mediaAddressPolicy ? m_options.mediaAddressPolicy : &isAllowedRendererAddress;
    if (address.isNull() || !policy(address))
        return;
    const QHostAddress plain = normalizedAddress(address);
    QString host = plain.toString();
    if (plain.protocol() == QAbstractSocket::IPv6Protocol)
        host = QStringLiteral("[%1]").arg(host);
    m_resolvedHost = host;
    const QUrl url = rewritten(m_url);
    const QUrl track = rewritten(m_trackUrl);
    const QUrl image = rewritten(m_imageUrl);
    const auto fits = [](const QUrl &candidate) {
        return urlText(candidate).toUtf8().size() <= kMaxContentIdBytes;
    };
    // A name that would be longer as an address and push a URL over the limit stays a name
    if (!fits(url) || !fits(track) || !fits(image)) {
        m_resolvedHost.clear();
        return;
    }
    m_url = url;
    m_trackUrl = track;
    m_imageUrl = image;
}

QUrl CastSender::rewritten(const QUrl &url) const
{
    return withHost(url, m_baseHost, m_resolvedHost);
}

void CastSender::proceedAfterResolve()
{
    m_contentId = urlText(m_url);
    if (!m_trackUrl.isEmpty())
        warmTrack(m_trackUrl);
    if (m_direct)
        m_probe.start(m_url);
    else
        openChannel();
}

void CastSender::warmTrack(const QUrl &url)
{
    abortWarmup();
    QNetworkRequest request(url);
    // No cookie is sent or kept and no redirect is followed; no transfer timeout, the server may answer only after minutes
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    QNetworkReply *reply = m_network.get(request);
    m_warmup = reply;
    const auto received = std::make_shared<qint64>(0);
    connect(reply, &QNetworkReply::readyRead, reply, [reply, received] {
        *received += reply->readAll().size();
        if (*received > kWarmupMaxBytes)
            reply->abort();
    });
    QTimer::singleShot(kWarmupTimeoutMs, reply, [reply] { reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply == m_warmup)
            m_warmup = nullptr;
    });
}

void CastSender::abortWarmup()
{
    if (QNetworkReply *reply = m_warmup) {
        m_warmup = nullptr;
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void CastSender::onProbeFinished(int status)
{
    if (m_phase != Phase::Preparing)
        return;
    if (status != 206) {
        qCWarning(senderLog) << "the stream answered" << status << "without credentials";
        // A sign-in asked for is the server's rule; anything else is a stream that may still play another way
        fail(status == 401 || status == 403 ? Failure::ServerRefused : Failure::Refused);
        return;
    }
    openChannel();
}

void CastSender::openChannel()
{
    m_phase = Phase::Connecting;
    createChannel(m_info.address, m_info.port);
    m_channel->open();
}

void CastSender::createChannel(const QHostAddress &address, quint16 port)
{
    closeChannel();
    m_channel = new CastChannel(address, port, m_options.channel, this);
    connect(m_channel, &CastChannel::opened, this, &CastSender::onChannelOpened);
    connect(m_channel, &CastChannel::failed, this, &CastSender::onChannelFailed);
    connect(m_channel, &CastChannel::messageReceived, this, &CastSender::onMessage);
    connect(m_channel, &CastChannel::virtualConnectionClosed, this,
            &CastSender::onVirtualConnectionClosed);
}

void CastSender::closeChannel()
{
    if (!m_channel)
        return;
    CastChannel *channel = m_channel;
    m_channel = nullptr;
    QObject::disconnect(channel, nullptr, this, nullptr);
    if (channel->state() == CastChannel::State::Open) {
        channel->close();
        QTimer::singleShot(kCloseGraceMs, channel, &QObject::deleteLater);
    } else {
        channel->close();
        channel->deleteLater();
    }
}

void CastSender::onChannelOpened()
{
    if (!m_channel)
        return;
    if (m_phase == Phase::Connecting) {
        m_firstLeaf = m_channel->authenticatedLeafHash();
        m_channel->connectTo(kReceiverId);
        m_phase = Phase::Launching;
        m_launchRetried = false;
        sendLaunch();
    } else if (m_phase == Phase::Reconnecting && m_resume == Resume::Opening) {
        // Another device on the old address, or the same address and a different device: nothing is sent to it
        if (m_channel->authenticatedLeafHash() != m_firstLeaf) {
            stoppedByDevice("another device answered the reconnect");
            return;
        }
        m_info.address = m_candidateAddress;
        m_info.port = m_candidatePort;
        m_channel->connectTo(kReceiverId);
        m_resume = Resume::ReceiverStatus;
        m_resumeRequest =
            m_channel->request(kReceiverId, kReceiverNs, typedMessage(QLatin1String("GET_STATUS")),
                               m_options.launchTimeoutMs);
        if (m_resumeRequest == 0) {
            attemptFailed();
            return;
        }
        m_stepTimer.start(m_options.launchTimeoutMs);
    }
}

void CastSender::onChannelFailed(CastChannel::FailReason reason, const QString &)
{
    qCWarning(senderLog) << "channel failed:"
                         << QMetaEnum::fromType<CastChannel::FailReason>().valueToKey(
                                static_cast<int>(reason))
                         << "while" << phaseName(m_phase);
    closeChannel();
    if (m_phase == Phase::Idle)
        return;
    if (m_phase == Phase::Stopping) {
        finishSession();
        announceOwnStop();
        return;
    }
    const bool refused = reason == CastChannel::FailReason::AuthRefused;
    if (m_phase == Phase::Reconnecting) {
        if (refused)
            fail(Failure::Untrusted);
        else
            attemptFailed();
        return;
    }
    if (refused) {
        fail(Failure::Untrusted);
        return;
    }
    const bool linkLost = reason == CastChannel::FailReason::Lost
                          || reason == CastChannel::FailReason::Protocol
                          || reason == CastChannel::FailReason::Flood;
    if (linkLost && m_phase == Phase::Active && m_mediaSessionId != 0)
        beginReconnect();
    else
        fail(Failure::NotResponding);
}

void CastSender::onMessage(const QString &source, const QString &ns, const QJsonObject &json)
{
    if (ns == kCastNsReceiver && source == kCastPlatformId)
        onReceiverMessage(json);
    else if (ns == kCastNsMedia && !m_transportId.isEmpty() && source == m_transportId)
        onMediaMessage(json);
}

void CastSender::onVirtualConnectionClosed(const QString &source)
{
    if (m_transportId.isEmpty() || source != m_transportId)
        return;
    if (m_phase == Phase::Stopping)
        proceedStop();
    else if (m_phase == Phase::Loading || m_phase == Phase::Active)
        stoppedByDevice("the app closed its connection");
}

void CastSender::sendLaunch()
{
    QJsonObject body = typedMessage(QLatin1String("LAUNCH"));
    body.insert(QStringLiteral("appId"), QString(kMediaReceiverApp));
    m_launchRequest = m_channel->request(kReceiverId, kReceiverNs, body, m_options.launchTimeoutMs);
    if (m_launchRequest == 0) {
        fail(Failure::NotResponding);
        return;
    }
    m_stepTimer.start(m_options.launchTimeoutMs);
}

void CastSender::handleLaunchError(const QJsonObject &json)
{
    if (m_phase != Phase::Launching)
        return;
    const quint32 id = requestIdOf(json);
    if (id != 0 && id != m_launchRequest)
        return;
    const QString reason = json.value(QLatin1String("reason")).toString();
    const bool cancelled = reason == QLatin1String("CANCELLED");
    qCWarning(senderLog) << "LAUNCH_ERROR, cancelled:" << cancelled;
    if (cancelled && !m_launchRetried) {
        m_launchRetried = true;
        sendLaunch();
        return;
    }
    fail(Failure::NotResponding);
}

auto CastSender::readApplications(const QJsonObject &status) const -> QList<App>
{
    QList<App> apps;
    const QJsonArray array = status.value(QLatin1String("applications")).toArray();
    const qsizetype count = std::min<qsizetype>(array.size(), kMaxApplications);
    for (qsizetype i = 0; i < count; ++i) {
        const QJsonObject object = array.at(i).toObject();
        apps.append(App{object.value(QLatin1String("appId")).toString(),
                        object.value(QLatin1String("sessionId")).toString(),
                        object.value(QLatin1String("transportId")).toString()});
    }
    return apps;
}

void CastSender::applyVolume(const QJsonObject &volume)
{
    bool changed = false;
    const QJsonValue level = volume.value(QLatin1String("level"));
    if (level.isDouble()) {
        const int value =
            static_cast<int>(std::lround(std::clamp(level.toDouble(), 0.0, 1.0) * 100.0));
        changed = !m_volumeSeen || value != m_volume;
        m_volume = value;
        m_volumeSeen = true;
    }
    const QJsonValue muted = volume.value(QLatin1String("muted"));
    if (muted.isBool()) {
        changed = changed || muted.toBool() != m_muted;
        m_muted = muted.toBool();
    }
    const QJsonValue controlType = volume.value(QLatin1String("controlType"));
    if (controlType.isString())
        m_volumeFixed = controlType.toString() == QLatin1String("fixed");
    if (changed)
        Q_EMIT volumeChanged(m_volume, m_muted);
}

void CastSender::onReceiverMessage(const QJsonObject &json)
{
    const QString type = json.value(QLatin1String("type")).toString();
    if (type == QLatin1String("LAUNCH_ERROR")) {
        handleLaunchError(json);
        return;
    }
    if (type != QLatin1String("RECEIVER_STATUS"))
        return;
    const QJsonObject status = json.value(QLatin1String("status")).toObject();
    applyVolume(status.value(QLatin1String("volume")).toObject());
    const QList<App> apps = readApplications(status);
    const auto listsOurSession = [this, &apps] {
        return std::any_of(apps.cbegin(), apps.cend(), [this](const App &app) {
            return !m_sessionId.isEmpty() && app.sessionId == m_sessionId;
        });
    };
    switch (m_phase) {
    case Phase::Launching:
        for (const App &app : apps) {
            // The status may arrive without a transportId at first: the wait goes on
            if (app.appId == kMediaReceiverApp && !app.sessionId.isEmpty()
                && app.sessionId.size() <= kMaxSessionIdChars && !app.transportId.isEmpty()
                && app.transportId.toUtf8().size() <= kMaxCastIdBytes) {
                joinApp(app);
                return;
            }
        }
        break;
    case Phase::Loading:
    case Phase::Active: {
        const bool ours = listsOurSession();
        m_sessionListed = ours;
        // An idle device lists its ambient app, which also comes back after our app STOP
        const bool onlyAmbient =
            !apps.isEmpty() && std::all_of(apps.cbegin(), apps.cend(), [](const App &app) {
                return app.appId == kAmbientApp;
            });
        if (!ours && !onlyAmbient)
            stoppedByDevice("our session is not in the receiver status");
        break;
    }
    case Phase::Reconnecting:
        if (m_resumeRequest != 0 && requestIdOf(json) == m_resumeRequest)
            handleResumeReceiverStatus(apps);
        break;
    case Phase::Stopping:
        m_sessionListed = listsOurSession();
        break;
    case Phase::Idle:
    case Phase::Preparing:
    case Phase::Connecting:
        break;
    }
}

void CastSender::joinApp(const App &app)
{
    m_stepTimer.stop();
    m_sessionId = app.sessionId;
    m_transportId = app.transportId;
    m_sessionListed = true;
    m_phase = Phase::Loading;
    m_channel->connectTo(m_transportId);
    if (!sendLoad()) {
        qCWarning(senderLog) << "the LOAD could not be sent";
        fail(Failure::Refused);
    }
}

bool CastSender::sendLoad()
{
    if (!m_channel)
        return false;
    const quint32 id =
        m_channel->request(m_transportId, kMediaNs, loadBody(), m_options.startTimeoutMs);
    if (id == 0)
        return false;
    m_loadRequestId = id;
    m_loadInFlight = true;
    return true;
}

QJsonObject CastSender::loadBody() const
{
    QJsonObject metadata{{QStringLiteral("metadataType"), 0}, {QStringLiteral("title"), m_title}};
    if (!m_subtitle.isEmpty())
        metadata.insert(QStringLiteral("subtitle"), m_subtitle);
    if (!m_imageUrl.isEmpty()) {
        QJsonArray images;
        images.append(QJsonObject{{QStringLiteral("url"), urlText(m_imageUrl)}});
        metadata.insert(QStringLiteral("images"), images);
    }
    QJsonObject media{{QStringLiteral("contentId"), m_contentId},
                      {QStringLiteral("contentType"), m_mime},
                      {QStringLiteral("streamType"), QStringLiteral("BUFFERED")},
                      {QStringLiteral("metadata"), metadata}};
    if (m_durationMs > 0)
        media.insert(QStringLiteral("duration"), static_cast<double>(m_durationMs) / 1000.0);
    QJsonObject body = typedMessage(QLatin1String("LOAD"));
    body.insert(QStringLiteral("sessionId"), m_sessionId);
    body.insert(QStringLiteral("autoplay"), true);
    body.insert(QStringLiteral("currentTime"), m_loadStartSec);
    if (!m_trackUrl.isEmpty()) {
        const QString language =
            m_trackLanguage.isEmpty() ? QStringLiteral("und") : m_trackLanguage;
        const QJsonObject track{{QStringLiteral("trackId"), 1},
                                {QStringLiteral("type"), QStringLiteral("TEXT")},
                                {QStringLiteral("trackContentId"), urlText(m_trackUrl)},
                                {QStringLiteral("trackContentType"), QStringLiteral("text/vtt")},
                                {QStringLiteral("subtype"), QStringLiteral("SUBTITLES")},
                                {QStringLiteral("language"), language},
                                {QStringLiteral("name"), m_trackName}};
        QJsonArray tracks;
        tracks.append(track);
        media.insert(QStringLiteral("tracks"), tracks);
        QJsonArray active;
        active.append(1);
        body.insert(QStringLiteral("activeTrackIds"), active);
    }
    body.insert(QStringLiteral("media"), media);
    return body;
}

void CastSender::onMediaMessage(const QJsonObject &json)
{
    const QString type = json.value(QLatin1String("type")).toString();
    const quint32 requestId = requestIdOf(json);
    const bool status = type == QLatin1String("MEDIA_STATUS");
    switch (m_phase) {
    case Phase::Reconnecting:
        if (status)
            handleResumeMediaStatus(json);
        return;
    case Phase::Stopping:
        if (status)
            handleMediaStatus(json);
        return;
    case Phase::Loading:
    case Phase::Active:
        break;
    case Phase::Idle:
    case Phase::Preparing:
    case Phase::Connecting:
    case Phase::Launching:
        return;
    }
    if (status) {
        handleMediaStatus(json);
    } else if (type == QLatin1String("LOAD_FAILED")) {
        // The id of our load, or none while one is waiting; the failure of a load we replaced is not ours
        if (requestId != 0 ? requestId == m_loadRequestId : m_loadInFlight) {
            qCWarning(senderLog) << "LOAD_FAILED";
            fail(Failure::Refused);
        }
    } else if (type == QLatin1String("INVALID_REQUEST")) {
        if (requestId != 0 && requestId == m_loadRequestId) {
            qCWarning(senderLog) << "INVALID_REQUEST for the load";
            fail(Failure::Refused);
        }
    } else if (type == QLatin1String("LOAD_CANCELLED")) {
        // The cancellation of an older load of ours is expected when we replace one; ours being cancelled is another sender's doing
        if (m_loadInFlight && requestId != 0 && requestId == m_loadRequestId)
            stoppedByDevice("the load was cancelled");
    }
}

void CastSender::handleMediaStatus(const QJsonObject &json)
{
    const QJsonArray entries = json.value(QLatin1String("status")).toArray();
    const quint32 requestId = requestIdOf(json);
    QJsonObject chosen;
    bool found = false;
    qint64 chosenId = -1;
    const qsizetype count = std::min<qsizetype>(entries.size(), kMaxStatusEntries);
    for (qsizetype i = 0; i < count; ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        const qint64 id = sessionIdOf(entry);
        if (m_mediaSessionId != 0 && id == m_mediaSessionId) {
            chosen = entry;
            found = true;
            break;
        }
        if (id >= chosenId) {
            chosen = entry;
            chosenId = id;
            found = true;
        }
    }
    if (found)
        handleEntry(chosen, requestId);
}

void CastSender::handleEntry(const QJsonObject &entry, quint32 requestId)
{
    const qint64 id = sessionIdOf(entry);
    const QString state = entry.value(QLatin1String("playerState")).toString();
    const QString reason = entry.value(QLatin1String("idleReason")).toString();
    const bool idle = state == QLatin1String("IDLE");
    const bool loading = entry.value(QLatin1String("extendedStatus"))
                             .toObject()
                             .value(QLatin1String("playerState"))
                             .toString()
                         == QLatin1String("LOADING");
    const QJsonValue content =
        entry.value(QLatin1String("media")).toObject().value(QLatin1String("contentId"));
    const bool hasContent = content.isString();
    const bool ours = hasContent && content.toString() == m_contentId;
    const bool reply = requestId != 0 && requestId == m_loadRequestId;

    if (m_phase == Phase::Stopping) {
        if (idle)
            proceedStop();
        return;
    }
    if (m_phase != Phase::Loading && m_phase != Phase::Active)
        return;

    // The session our own reload replaces is interrupted, and what it says from then on is not ours
    if (m_loadInFlight && !reply) {
        if (idle && reason == QLatin1String("INTERRUPTED"))
            return;
        if (m_oldMediaSessionId != 0 && id == m_oldMediaSessionId)
            return;
    }

    if (m_mediaSessionId == 0) {
        // Our session is the one that answers the LOAD; a reload's first status may come without the requestId
        const bool adoptable = id != 0 && (reply || (!idle && (!hasContent || ours)));
        if (!adoptable)
            return;
        adoptSession(id);
    } else if (id != m_mediaSessionId) {
        if (id < m_mediaSessionId)
            return;
        if ((idle && reason == QLatin1String("INTERRUPTED")) || (hasContent && !ours)) {
            stoppedByDevice("another media session took over");
            return;
        }
        if (!hasContent)
            return;
        adoptSession(id);
    }
    logStatus(entry, state, reason);

    if (reply && !idle)
        m_loadInFlight = false;
    if (idle) {
        handleIdle(reason, loading);
        return;
    }
    if (state == QLatin1String("BUFFERING")) {
        if (m_started)
            applyPosition(entry);
        else
            m_stuckTimer.stop();
        setTransport(Transport::Transitioning);
    } else if (state == QLatin1String("PLAYING")) {
        handlePlaying(entry);
    } else if (state == QLatin1String("PAUSED")) {
        markStarted();
        applyPosition(entry);
        setTransport(Transport::Paused);
    }
}

// Once per change of state or idle reason of our session, with the device's own duration of the stream at that moment (a FINISHED long before the item's end then shows how much the device had fetched)
void CastSender::logStatus(const QJsonObject &entry, const QString &state, const QString &reason)
{
    const QString status =
        state.left(kMaxLoggedStatusChars) + QLatin1Char('/') + reason.left(kMaxLoggedStatusChars);
    if (status == m_loggedStatus)
        return;
    m_loggedStatus = status;
    const QJsonValue duration =
        entry.value(QLatin1String("media")).toObject().value(QLatin1String("duration"));
    qCDebug(senderLog).nospace() << "media status " << status << " at "
                                 << entry.value(QLatin1String("currentTime")).toDouble(-1)
                                 << " s, device duration "
                                 << (duration.isDouble() ? duration.toDouble() : -1.0) << " s";
}

void CastSender::handleIdle(const QString &reason, bool loading)
{
    if (reason.isEmpty()) {
        // The initial load, not an end; an IDLE with neither is ignored
        if (loading)
            setTransport(Transport::Transitioning);
        return;
    }
    if (reason == QLatin1String("FINISHED")) {
        endedByDevice();
    } else if (reason == QLatin1String("ERROR")) {
        qCWarning(senderLog) << "the device reported a playback error";
        fail(Failure::Refused);
    } else if (reason == QLatin1String("CANCELLED") || reason == QLatin1String("INTERRUPTED")) {
        // Our own STOP is handled in Stopping and our own reload before this point
        stoppedByDevice("the media was cancelled or interrupted");
    }
}

void CastSender::handlePlaying(const QJsonObject &entry)
{
    const QJsonValue time = entry.value(QLatin1String("currentTime"));
    const double seconds = time.isDouble() ? time.toDouble() : 0.0;
    if (!m_started) {
        // The reply to the LOAD says PLAYING at the start before anything plays: only a position that moved is a start (an HEVC stream on a Nest Hub stays there with no error)
        if (seconds <= m_loadStartSec) {
            setTransport(Transport::Transitioning);
            if (!m_stuckTimer.isActive())
                m_stuckTimer.start();
            return;
        }
        markStarted();
    }
    applyPosition(entry);
    setTransport(Transport::Playing);
}

void CastSender::adoptSession(qint64 id)
{
    m_mediaSessionId = id;
    m_phase = Phase::Active;
    if (!m_pollTimer.isActive())
        m_pollTimer.start();
    if (!m_tickTimer.isActive())
        m_tickTimer.start();
}

void CastSender::markStarted()
{
    if (m_started)
        return;
    m_started = true;
    m_loadInFlight = false;
    m_startTimer.stop();
    m_stuckTimer.stop();
}

void CastSender::applyPosition(const QJsonObject &entry)
{
    const QJsonValue time = entry.value(QLatin1String("currentTime"));
    if (time.isDouble()) {
        m_baseMs = millisecondsOf(time.toDouble()) + m_offsetMs;
        m_statusClock.restart();
    }
    const QJsonValue rate = entry.value(QLatin1String("playbackRate"));
    m_rate = rate.isDouble() ? std::clamp(rate.toDouble(), 0.0, kMaxPlaybackRate) : 1.0;
}

void CastSender::setTransport(Transport state)
{
    if (m_transport && *m_transport == state)
        return;
    m_transport = state;
    Q_EMIT stateChanged(state, positionMs(), m_durationMs);
}

qint64 CastSender::positionMs() const
{
    qint64 value = m_baseMs;
    if (m_phase == Phase::Active && m_transport == Transport::Playing && m_started
        && m_statusClock.isValid())
        value += static_cast<qint64>(static_cast<double>(m_statusClock.elapsed()) * m_rate);
    value = std::max<qint64>(value, 0);
    if (m_durationMs > 0)
        value = std::min(value, m_durationMs);
    return value;
}

void CastSender::freezePosition()
{
    m_baseMs = positionMs();
    m_statusClock.restart();
}

void CastSender::setDurationMs(qint64 durationMs)
{
    if (durationMs <= 0 || durationMs > kMaxStartMs || durationMs == m_durationMs)
        return;
    m_durationMs = durationMs;
    m_baseMs = std::min(m_baseMs, m_durationMs);
    Q_EMIT positionChanged(positionMs(), m_durationMs);
}

bool CastSender::hasVolume() const
{
    return m_volumeSeen && !m_volumeFixed;
}

bool CastSender::canControl() const
{
    return m_phase == Phase::Active && m_mediaSessionId != 0 && m_channel
           && m_channel->state() == CastChannel::State::Open;
}

bool CastSender::sendMediaCommand(QLatin1StringView type, QJsonObject extra)
{
    if (!canControl())
        return false;
    extra.insert(QStringLiteral("type"), QString(type));
    extra.insert(QStringLiteral("mediaSessionId"), static_cast<double>(m_mediaSessionId));
    return m_channel->request(m_transportId, kMediaNs, extra) != 0;
}

bool CastSender::play()
{
    return sendMediaCommand(QLatin1String("PLAY"));
}

bool CastSender::pause()
{
    return sendMediaCommand(QLatin1String("PAUSE"));
}

bool CastSender::seek(qint64 target)
{
    if (!m_direct || target < 0 || !canControl())
        return false;
    if (m_durationMs > 0)
        target = std::min(target, m_durationMs);
    const QJsonObject extra{{QStringLiteral("currentTime"), static_cast<double>(target) / 1000.0},
                            {QStringLiteral("resumeState"), QStringLiteral("PLAYBACK_START")}};
    if (!sendMediaCommand(QLatin1String("SEEK"), extra))
        return false;
    // The slider must not jump back while the device catches up
    m_baseMs = target;
    m_statusClock.restart();
    Q_EMIT positionChanged(positionMs(), m_durationMs);
    return true;
}

bool CastSender::reload(const QUrl &url, qint64 startMs, const QUrl &subtitleUrl)
{
    const bool live = m_phase == Phase::Active || m_phase == Phase::Loading;
    if (m_direct || !live || m_transportId.isEmpty() || !m_channel
        || m_channel->state() != CastChannel::State::Open || startMs < 0 || startMs > kMaxStartMs
        || !urlAcceptable(url, m_originalBase)
        || (!subtitleUrl.isEmpty() && !urlAcceptable(subtitleUrl, m_originalBase)))
        return false;
    const QUrl newUrl = rewritten(url);
    const QUrl newTrack = rewritten(subtitleUrl);
    if (urlText(newUrl).toUtf8().size() > kMaxContentIdBytes
        || urlText(newTrack).toUtf8().size() > kMaxContentIdBytes)
        return false;
    const QUrl oldUrl = m_url;
    const QUrl oldTrack = m_trackUrl;
    const QString oldContent = m_contentId;
    m_url = newUrl;
    m_trackUrl = newTrack;
    m_contentId = urlText(m_url);
    m_loadStartSec = 0.0;
    if (!sendLoad()) {
        m_url = oldUrl;
        m_trackUrl = oldTrack;
        m_contentId = oldContent;
        return false;
    }
    // A reload that follows an unanswered one keeps the session the first one replaced
    if (m_mediaSessionId != 0)
        m_oldMediaSessionId = m_mediaSessionId;
    m_mediaSessionId = 0;
    m_phase = Phase::Loading;
    m_started = false;
    m_loggedStatus.clear();
    m_transport.reset();
    m_offsetMs = startMs;
    m_baseMs = startMs;
    m_statusClock.restart();
    for (QTimer *timer : {&m_pollTimer, &m_tickTimer, &m_stuckTimer})
        timer->stop();
    m_startTimer.start(startDeadlineMs());
    return true;
}

bool CastSender::sendVolume(const QJsonObject &volume)
{
    const bool live =
        m_phase == Phase::Launching || m_phase == Phase::Loading || m_phase == Phase::Active;
    if (!live || !m_channel || m_channel->state() != CastChannel::State::Open || !hasVolume())
        return false;
    QJsonObject body = typedMessage(QLatin1String("SET_VOLUME"));
    body.insert(QStringLiteral("volume"), volume);
    return m_channel->request(kReceiverId, kReceiverNs, body) != 0;
}

bool CastSender::setVolume(int level)
{
    if (level < 0 || level > 100)
        return false;
    if (!sendVolume(QJsonObject{{QStringLiteral("level"), static_cast<double>(level) / 100.0}}))
        return false;
    m_volume = level;
    Q_EMIT volumeChanged(m_volume, m_muted);
    return true;
}

bool CastSender::setMuted(bool muted)
{
    if (!sendVolume(QJsonObject{{QStringLiteral("muted"), muted}}))
        return false;
    m_muted = muted;
    Q_EMIT volumeChanged(m_volume, m_muted);
    return true;
}

void CastSender::pollStatus()
{
    if (m_phase != Phase::Active || !m_channel || m_channel->state() != CastChannel::State::Open)
        return;
    m_channel->request(m_transportId, kMediaNs, typedMessage(QLatin1String("GET_STATUS")));
}

void CastSender::tick()
{
    if (m_phase == Phase::Active && m_transport == Transport::Playing)
        Q_EMIT positionChanged(positionMs(), m_durationMs);
}

void CastSender::onStartTimeout()
{
    if (m_started || m_phase == Phase::Idle || m_phase == Phase::Stopping)
        return;
    qCWarning(senderLog) << "no PLAYING in time while" << phaseName(m_phase);
    // The device never got the stream when the load did not go out: it is not answering
    fail(m_phase == Phase::Loading || m_phase == Phase::Active ? Failure::Refused
                                                               : Failure::NotResponding);
}

void CastSender::onStepTimeout()
{
    if (m_phase == Phase::Launching) {
        qCWarning(senderLog) << "no app after the LAUNCH";
        fail(Failure::NotResponding);
    } else if (m_phase == Phase::Reconnecting) {
        attemptFailed();
    }
}

bool CastSender::stop()
{
    if (m_phase == Phase::Idle)
        return false;
    if (m_phase == Phase::Stopping)
        return true;
    if (m_phase != Phase::Loading && m_phase != Phase::Active) {
        // Nothing was loaded: the probe and the channel are all there is to cancel
        finishSession();
        announceOwnStop();
        return true;
    }
    ++m_generation;
    m_probe.abort();
    abortWarmup();
    for (QTimer *timer : {&m_pollTimer, &m_tickTimer, &m_startTimer, &m_stuckTimer, &m_stepTimer,
                          &m_reconnectTimer})
        timer->stop();
    freezePosition();
    m_phase = Phase::Stopping;
    if (sendMediaStop()) {
        m_stopTimer.start(m_options.stopTimeoutMs);
        return true;
    }
    proceedStop();
    return true;
}

bool CastSender::sendMediaStop()
{
    if (m_mediaSessionId == 0 || !m_channel || m_channel->state() != CastChannel::State::Open)
        return false;
    QJsonObject body = typedMessage(QLatin1String("STOP"));
    body.insert(QStringLiteral("mediaSessionId"), static_cast<double>(m_mediaSessionId));
    return m_channel->request(m_transportId, kMediaNs, body) != 0;
}

void CastSender::sendAppStop()
{
    // A late STOP of the app can end another sender's session
    if (!m_channel || m_channel->state() != CastChannel::State::Open || !m_sessionListed
        || m_sessionId.isEmpty())
        return;
    QJsonObject body = typedMessage(QLatin1String("STOP"));
    body.insert(QStringLiteral("sessionId"), m_sessionId);
    m_channel->request(kReceiverId, kReceiverNs, body);
}

void CastSender::proceedStop()
{
    if (m_phase != Phase::Stopping)
        return;
    m_stopTimer.stop();
    sendAppStop();
    finishSession();
    announceOwnStop();
}

int CastSender::startDeadlineMs() const
{
    return m_options.startTimeoutMs + (m_trackUrl.isEmpty() ? 0 : kTrackStartExtraMs);
}

void CastSender::announceOwnStop()
{
    // Queued, so that it never arrives inside the stop() call that caused it; a load that started meanwhile drops it
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation == m_generation && m_phase == Phase::Idle)
            Q_EMIT stopped();
    });
}

void CastSender::beginReconnect()
{
    qCInfo(senderLog) << "connection lost, reconnecting";
    freezePosition();
    m_phase = Phase::Reconnecting;
    m_attempt = 0;
    m_resume = Resume::None;
    for (QTimer *timer : {&m_pollTimer, &m_tickTimer, &m_startTimer, &m_stuckTimer, &m_stepTimer})
        timer->stop();
    closeChannel();
    if (m_options.reconnectAttempts <= 0) {
        fail(Failure::NotResponding);
        return;
    }
    reconnectAttempt();
}

void CastSender::reconnectAttempt()
{
    if (m_phase != Phase::Reconnecting)
        return;
    ++m_attempt;
    m_resume = Resume::Resolving;
    const quint64 generation = m_generation;
    const QPointer<CastSender> self(this);
    findDevice(m_info.id, [self, generation](const std::optional<CastDeviceInfo> &device) {
        if (self.isNull() || generation != self->m_generation
            || self->m_phase != Phase::Reconnecting || self->m_resume != Resume::Resolving)
            return;
        if (!device || device->id != self->m_info.id) {
            self->attemptFailed();
            return;
        }
        self->m_candidateAddress = device->address;
        self->m_candidatePort = device->port;
        self->m_resume = Resume::Opening;
        self->createChannel(device->address, device->port);
        self->m_channel->open();
    });
}

void CastSender::attemptFailed()
{
    if (m_phase != Phase::Reconnecting)
        return;
    qCInfo(senderLog) << "reconnect attempt" << m_attempt << "failed";
    m_stepTimer.stop();
    m_resume = Resume::None;
    closeChannel();
    if (m_attempt >= m_options.reconnectAttempts) {
        fail(Failure::NotResponding);
        return;
    }
    m_reconnectTimer.start(m_options.reconnectDelayMs);
}

void CastSender::handleResumeReceiverStatus(const QList<App> &apps)
{
    if (m_resume != Resume::ReceiverStatus || !m_channel)
        return;
    for (const App &app : apps) {
        if (m_sessionId.isEmpty() || app.sessionId != m_sessionId)
            continue;
        if (app.transportId.isEmpty() || app.transportId.toUtf8().size() > kMaxCastIdBytes)
            break;
        m_transportId = app.transportId;
        m_resume = Resume::MediaStatus;
        m_channel->connectTo(m_transportId);
        m_resumeRequest =
            m_channel->request(m_transportId, kMediaNs, typedMessage(QLatin1String("GET_STATUS")),
                               m_options.launchTimeoutMs);
        if (m_resumeRequest == 0) {
            attemptFailed();
            return;
        }
        m_stepTimer.start(m_options.launchTimeoutMs);
        return;
    }
    // Our session is gone: another sender or the device's own controls ended it
    stoppedByDevice("our session is gone after the reconnect");
}

void CastSender::handleResumeMediaStatus(const QJsonObject &json)
{
    if (m_resume != Resume::MediaStatus)
        return;
    const quint32 requestId = requestIdOf(json);
    const bool reply = requestId != 0 && requestId == m_resumeRequest;
    const QJsonArray entries = json.value(QLatin1String("status")).toArray();
    const qsizetype count = std::min<qsizetype>(entries.size(), kMaxStatusEntries);
    for (qsizetype i = 0; i < count; ++i) {
        const QJsonObject entry = entries.at(i).toObject();
        const QString content = entry.value(QLatin1String("media"))
                                    .toObject()
                                    .value(QLatin1String("contentId"))
                                    .toString();
        if (m_mediaSessionId == 0 || sessionIdOf(entry) != m_mediaSessionId
            || content != m_contentId)
            continue;
        qCInfo(senderLog) << "reconnected to the same media session";
        m_stepTimer.stop();
        m_resume = Resume::None;
        m_attempt = 0;
        m_phase = Phase::Active;
        m_pollTimer.start();
        m_tickTimer.start();
        if (!m_started)
            m_startTimer.start(startDeadlineMs());
        handleEntry(entry, 0);
        return;
    }
    // The device plays something else or nothing
    if (reply)
        stoppedByDevice("the device plays something else after the reconnect");
}

void CastSender::fail(Failure failure)
{
    if (m_phase == Phase::Idle)
        return;
    qCWarning(senderLog) << "failed:"
                         << QMetaEnum::fromType<Failure>().valueToKey(static_cast<int>(failure))
                         << "while" << phaseName(m_phase);
    freezePosition();
    // As a DLNA renderer is stopped: the device would stay in our app on a stream that does not play. The channel close that follows flushes both
    if (failure == Failure::Refused && (!m_transportId.isEmpty() || m_mediaSessionId != 0)) {
        sendMediaStop();
        sendAppStop();
    }
    finishSession();
    Q_EMIT failed(failure);
}

void CastSender::endedByDevice()
{
    if (m_phase == Phase::Idle || m_phase == Phase::Stopping)
        return;
    // A truncated file or a transcode whose encoder died finishes early: the device gave up, the item did not end
    if (m_durationMs > 0 && m_durationMs - positionMs() > kEndToleranceMs) {
        if (m_started) {
            stoppedByDevice("the device finished long before the end");
        } else {
            qCWarning(senderLog) << "the device finished long before the end, before it started";
            fail(Failure::Refused);
        }
        return;
    }
    if (m_durationMs > 0)
        m_baseMs = m_durationMs;
    else
        freezePosition();
    finishSession();
    Q_EMIT ended();
}

void CastSender::stoppedByDevice(const char *reason)
{
    if (m_phase == Phase::Idle || m_phase == Phase::Stopping)
        return;
    qCInfo(senderLog) << "stopped by the device:" << reason << "while" << phaseName(m_phase) << "at"
                      << positionMs() << "ms";
    freezePosition();
    finishSession();
    Q_EMIT stopped();
}

void CastSender::finishSession()
{
    ++m_generation;
    m_probe.abort();
    abortWarmup();
    for (QTimer *timer : {&m_pollTimer, &m_tickTimer, &m_startTimer, &m_stuckTimer, &m_stepTimer,
                          &m_stopTimer, &m_reconnectTimer})
        timer->stop();
    closeChannel();
    m_phase = Phase::Idle;
    m_resume = Resume::None;
    m_loadInFlight = false;
    m_transport = Transport::Stopped;
}

} // namespace Kinefin
