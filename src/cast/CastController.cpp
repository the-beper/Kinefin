// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastController.h"

#include "cast/CastProfile.h"
#include "cast/CastTargetsModel.h"
#include "cast/DlnaRenderer.h"
#include "cast/RendererProfile.h"
#include "cast/Soap.h"

#include <QHostAddress>
#include <QLoggingCategory>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <KLocalizedString>

#include <memory>
#include <utility>

Q_LOGGING_CATEGORY(castLog, "kinefin.cast.controller")

namespace Kinefin {
namespace {
// The protocol leads, so that a TV that offers both DLNA and Google Cast lists two rows that differ in their text and not only in a suffix of the name
QString protocolDetail(const QString &protocol, const QString &model, const QHostAddress &address)
{
    if (model.isEmpty())
        return i18nc("@info:label protocol and network address of a device", "%1, %2", protocol,
                     address.toString());
    return i18nc("@info:label protocol, model and network address of a device", "%1, %2, %3",
                 protocol, model, address.toString());
}

QString targetDetail(const RendererInfo &info)
{
    QString model = info.model.trimmed();
    if (model.isEmpty())
        model = info.manufacturer.trimmed();
    return protocolDetail(i18nc("@item the DLNA protocol", "DLNA"), model, info.address);
}
} // namespace

CastController::CastController(QObject *parent)
    : CastController(SsdpSearch::Options{}, MdnsSearch::Options{}, parent)
{
}

CastController::CastController(const SsdpSearch::Options &options, QObject *parent)
    : QObject(parent), m_targets(new CastTargetsModel(this)), m_search(options)
{
    m_discovery.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    connect(&m_search, &SsdpSearch::deviceFound, this, &CastController::fetchRenderer);
    connect(&m_search, &SsdpSearch::finished, this, [this] {
        // A device that answered neither search is dropped once both are over
        if (!searching())
            dropUnseen();
        Q_EMIT searchingChanged();
    });
}

CastController::CastController(const SsdpSearch::Options &ssdp, const MdnsSearch::Options &mdns,
                               QObject *parent)
    : CastController(ssdp, parent)
{
    m_mdns = new MdnsSearch(mdns, this);
    connect(m_mdns, &MdnsSearch::deviceFound, this, &CastController::onCastDeviceFound);
    connect(m_mdns, &MdnsSearch::deviceGone, this,
            [this](const QString &id) { m_seen.remove(QStringLiteral("cast:") + id); });
    connect(m_mdns, &MdnsSearch::finished, this, [this] {
        if (!searching())
            dropUnseen();
        Q_EMIT searchingChanged();
    });
}

CastController::~CastController() = default;

bool CastController::searching() const
{
    return m_search.running() || (m_mdns && m_mdns->running());
}

void CastController::search()
{
    setError({});
    if (searching())
        return;
    m_seen.clear();
    const bool ssdp = m_search.start();
    const bool mdns = m_mdns && m_mdns->start();
    if (!ssdp && !mdns)
        setError(i18n("Could not search the network"));
    else if (!ssdp || (m_mdns && !mdns))
        setError(
            i18n("Some devices may not be listed: the search could not cover the whole network"));
    Q_EMIT searchingChanged();
}

void CastController::stopSearch()
{
    m_search.stop();
    if (m_mdns)
        m_mdns->stop();
    Q_EMIT searchingChanged();
}

void CastController::fetchRenderer(const SsdpResponse &response)
{
    if (const auto *known =
            qobject_cast<const DlnaRenderer *>(m_renderers.value(response.udn, nullptr))) {
        // Only the address that listed a device speaks for it: another host answering with its UDN is ignored, and a device that really moved is read again once a search no longer finds it at the old address
        if (response.sender != known->info().address)
            return;
        m_seen.insert(response.udn);
        // Read again when its description moved on the same host, unless it plays now
        if (known == m_active || known->info().location == response.location
            || m_pending.contains(response.udn)) {
            if (m_targets->indexOf(response.udn) < 0)
                addTarget(known->info());
            return;
        }
    } else {
        m_seen.insert(response.udn);
        if (m_pending.contains(response.udn))
            return;
    }
    m_pending.insert(response.udn);
    fetchDocument(response.location, kMaxDescriptionBytes, kDescriptionTimeoutMs,
                  [this, response](bool ok, const QByteArray &body) {
                      if (!ok) {
                          qCWarning(castLog) << "no description from" << response.udn;
                          m_pending.remove(response.udn);
                          return;
                      }
                      QString error;
                      const auto parsed = parseDeviceDescription(
                          body, response.location, response.sender, response.udn, &error);
                      if (!parsed) {
                          qCWarning(castLog) << "bad description from" << response.udn << error;
                          m_pending.remove(response.udn);
                          return;
                      }
                      auto info = std::make_shared<RendererInfo>(*parsed);
                      fetchDocument(info->avTransport.scpdUrl, kMaxDescriptionBytes,
                                    kDescriptionTimeoutMs,
                                    [this, info](bool ok, const QByteArray &scpd) {
                                        QString scpdError;
                                        if (!ok || !parseAvTransportScpd(scpd, *info, &scpdError)) {
                                            qCWarning(castLog) << "bad AVTransport SCPD from"
                                                               << info->udn << scpdError;
                                            m_pending.remove(info->udn);
                                            return;
                                        }
                                        fetchProtocolInfo(*info, [this, info](QStringList sink) {
                                            info->sinkMimeTypes = std::move(sink);
                                            addRenderer(*info);
                                            m_pending.remove(info->udn);
                                        });
                                    });
                  });
}

void CastController::fetchDocument(const QUrl &url, int limit, int timeoutMs,
                                   std::function<void(bool, const QByteArray &)> done)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = m_discovery.get(request);
    // The transfer timeout only bounds the silence between bytes: this bounds the whole answer
    QTimer::singleShot(timeoutMs, reply, [reply] { reply->abort(); });
    auto body = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this, [reply, body, tooLarge, limit] {
        body->append(reply->readAll());
        if (body->size() > limit) {
            *tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [reply, body, tooLarge, done = std::move(done)] {
        body->append(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = !*tooLarge && reply->error() == QNetworkReply::NoError && status == 200;
        const QByteArray data = *body;
        reply->deleteLater();
        done(ok, data);
    });
}

void CastController::fetchProtocolInfo(const RendererInfo &info,
                                       std::function<void(QStringList)> done)
{
    const QByteArray body =
        soapRequestBody(info.connectionManager.serviceType, QStringLiteral("GetProtocolInfo"), {});
    const QByteArray action =
        soapActionHeader(info.connectionManager.serviceType, QStringLiteral("GetProtocolInfo"));
    if (body.isEmpty() || action.isEmpty() || !info.connectionManager.controlUrl.isValid()) {
        done({});
        return;
    }
    QNetworkRequest request(info.connectionManager.controlUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(kSoapContentType));
    request.setRawHeader("SOAPACTION", action);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(kSoapTimeoutMs);
    QNetworkReply *reply = m_discovery.post(request, body);
    QTimer::singleShot(kSoapTimeoutMs, reply, [reply] { reply->abort(); });
    auto response = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this, [reply, response, tooLarge] {
        response->append(reply->readAll());
        if (response->size() > kMaxProtocolInfoResponseBytes) {
            *tooLarge = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, response, tooLarge, done = std::move(done)] {
                response->append(reply->readAll());
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok =
                    !*tooLarge && reply->error() == QNetworkReply::NoError && status == 200;
                const QByteArray data = *response;
                reply->deleteLater();
                if (!ok) {
                    done({});
                    return;
                }
                const SoapResult parsed = parseSoapResponse(data, QStringLiteral("GetProtocolInfo"),
                                                            {QStringLiteral("Sink")});
                done(parsed.status == SoapStatus::Ok
                         ? parseSinkProtocolInfo(parsed.values.value(QStringLiteral("Sink")))
                         : QStringList{});
            });
}

void CastController::addRenderer(const RendererInfo &info)
{
    if (CastRenderer *old = m_renderers.value(info.udn, nullptr)) {
        if (old == m_active)
            return;
        old->deleteLater();
    }
    auto *renderer = new DlnaRenderer(info, this);
    m_renderers.insert(info.udn, renderer);
    connectRenderer(renderer);
    addTarget(info);
}

void CastController::addTarget(const RendererInfo &info)
{
    CastTargetsModel::Target target;
    target.udn = info.udn;
    target.name = info.name.isEmpty() ? info.address.toString() : info.name;
    target.detail = targetDetail(info);
    target.kind = QStringLiteral("dlna");
    m_targets->add(std::move(target));
    Q_EMIT targetsChanged();
}

void CastController::addCastDevice(const CastDeviceInfo &info, const CastSender::Options &options)
{
    const QString target = CastSender::targetId(info);
    if (CastRenderer *old = m_renderers.value(target, nullptr)) {
        if (old == m_active)
            return;
        old->deleteLater();
    }
    auto *sender = new CastSender(info, options, this);
    m_renderers.insert(target, sender);
    connectRenderer(sender);
    addCastTarget(info);
}

void CastController::addCastTarget(const CastDeviceInfo &info)
{
    CastTargetsModel::Target target;
    target.udn = CastSender::targetId(info);
    target.name = info.name.isEmpty() ? info.address.toString() : info.name;
    target.detail = protocolDetail(i18nc("@item the Google Cast protocol", "Google Cast"),
                                   info.model.trimmed(), info.address);
    target.kind = QStringLiteral("cast");
    m_targets->add(std::move(target));
    Q_EMIT targetsChanged();
}

void CastController::onCastDeviceFound(const CastDeviceInfo &info)
{
    const QString target = CastSender::targetId(info);
    m_seen.insert(target);
    if (const auto *known = qobject_cast<const CastSender *>(m_renderers.value(target, nullptr))) {
        // The same device at the same place: kept as it is; a device that moved is read again unless it plays now
        if ((known->info().address == info.address && known->info().port == info.port)
            || known == m_active) {
            if (m_targets->indexOf(target) < 0)
                addCastTarget(known->info());
            return;
        }
    }
    addCastDevice(info);
}

bool CastController::hasTarget(const QString &id) const
{
    return m_renderers.contains(id);
}

QJsonObject CastController::deviceProfile(const QString &id, bool burnInSubtitles,
                                          bool transcodeOnly) const
{
    if (const auto *cast = qobject_cast<const CastSender *>(m_renderers.value(id, nullptr)))
        return castDeviceProfile(cast->info().model, burnInSubtitles, transcodeOnly);
    if (const RendererInfo *info = infoFor(id))
        return rendererDeviceProfile(info->sinkMimeTypes, burnInSubtitles, transcodeOnly);
    return {};
}

QString CastController::mimeFor(const QString &id, const QString &container, bool transcode) const
{
    if (qobject_cast<const CastSender *>(m_renderers.value(id, nullptr)))
        return castMime(container, transcode);
    if (const RendererInfo *info = infoFor(id))
        return rendererMime(transcode ? QStringLiteral("ts") : container, info->sinkMimeTypes);
    return {};
}

bool CastController::takesTextTrack(const QString &id) const
{
    return qobject_cast<const CastSender *>(m_renderers.value(id, nullptr)) != nullptr;
}

void CastController::dropUnseen()
{
    // A device that did not answer this search is off, gone or moved: its row and its renderer go, the renderer that plays stays
    const QString keep = activeUdn();
    for (auto it = m_renderers.begin(); it != m_renderers.end();) {
        const QString udn = it.key();
        if (udn == keep || m_seen.contains(udn) || m_pending.contains(udn)) {
            ++it;
            continue;
        }
        m_targets->remove(udn);
        it.value()->deleteLater();
        it = m_renderers.erase(it);
    }
    Q_EMIT targetsChanged();
}

void CastController::connectRenderer(CastRenderer *renderer)
{
    connect(renderer, &CastRenderer::stateChanged, this,
            [this, renderer](CastRenderer::Transport state, qint64, qint64) {
                if (renderer != m_active)
                    return;
                switch (state) {
                case CastRenderer::Transport::Playing:
                    Q_EMIT playing();
                    break;
                case CastRenderer::Transport::Paused:
                    Q_EMIT paused();
                    break;
                case CastRenderer::Transport::Transitioning:
                    Q_EMIT buffering();
                    break;
                case CastRenderer::Transport::Stopped:
                case CastRenderer::Transport::NoMedia:
                    break;
                }
            });
    connect(renderer, &CastRenderer::positionChanged, this, [this, renderer] {
        if (renderer == m_active)
            Q_EMIT positionChanged();
    });
    connect(renderer, &CastRenderer::volumeChanged, this, [this, renderer] {
        if (renderer == m_active)
            Q_EMIT volumeChanged();
    });
    connect(renderer, &CastRenderer::ended, this, [this, renderer] {
        if (renderer != m_active)
            return;
        clearActive();
        Q_EMIT ended();
    });
    connect(renderer, &CastRenderer::stopped, this, [this, renderer] {
        if (renderer != m_active)
            return;
        clearActive();
        Q_EMIT stopped();
    });
    connect(renderer, &CastRenderer::failed, this, [this, renderer](CastRenderer::Failure failure) {
        if (renderer != m_active)
            return;
        clearActive();
        Q_EMIT failed(failure);
    });
    connect(renderer, &CastRenderer::textTrackReady, this, [this, renderer] {
        if (renderer == m_active)
            Q_EMIT textTrackReady();
    });
}

void CastController::setActive(CastRenderer *renderer)
{
    m_active = renderer;
}

void CastController::clearActive()
{
    m_active = nullptr;
}

void CastController::setError(const QString &error)
{
    if (error == m_error)
        return;
    m_error = error;
    Q_EMIT errorChanged();
}

bool CastController::load(const QString &udn, const QUrl &url, const QString &mime,
                          const QString &title, qint64 startMs, qint64 durationMs,
                          const QUrl &serverBase, bool direct)
{
    CastLoad request;
    request.url = url;
    request.mime = mime;
    request.title = title;
    request.startMs = startMs;
    request.durationMs = durationMs;
    request.serverBase = serverBase;
    request.direct = direct;
    return load(udn, request);
}

bool CastController::load(const QString &id, const CastLoad &request)
{
    CastRenderer *renderer = m_renderers.value(id, nullptr);
    if (!renderer || !renderer->load(request))
        return false;
    setActive(renderer);
    return true;
}

bool CastController::reload(const QUrl &url, qint64 startMs, const QUrl &subtitleUrl)
{
    return m_active && m_active->reload(url, startMs, subtitleUrl);
}

void CastController::setDurationMs(qint64 durationMs)
{
    if (m_active)
        m_active->setDurationMs(durationMs);
}

bool CastController::pause()
{
    return m_active && m_active->pause();
}

bool CastController::resume()
{
    return m_active && m_active->play();
}

bool CastController::seek(qint64 positionMs)
{
    return m_active && m_active->seek(positionMs);
}

bool CastController::stop()
{
    return m_active && m_active->stop();
}

bool CastController::setVolume(int volume)
{
    return m_active && m_active->setVolume(volume);
}

bool CastController::setMuted(bool muted)
{
    return m_active && m_active->setMuted(muted);
}

QString CastController::activeUdn() const
{
    return m_active ? m_active->id() : QString();
}

QString CastController::activeName() const
{
    return m_active ? nameFor(m_active->id()) : QString();
}

QString CastController::nameFor(const QString &udn) const
{
    if (const auto *cast = qobject_cast<const CastSender *>(m_renderers.value(udn, nullptr))) {
        const QString name = cast->info().name.trimmed();
        return name.isEmpty() ? cast->info().address.toString() : name;
    }
    const RendererInfo *info = infoFor(udn);
    if (!info)
        return {};
    const QString name = info->name.trimmed();
    return name.isEmpty() ? info->address.toString() : name;
}

const RendererInfo *CastController::infoFor(const QString &udn) const
{
    const auto *renderer = qobject_cast<const DlnaRenderer *>(m_renderers.value(udn, nullptr));
    return renderer ? &renderer->info() : nullptr;
}

bool CastController::active() const
{
    return m_active != nullptr;
}

bool CastController::isPlaying() const
{
    return m_active && m_active->transport() == CastRenderer::Transport::Playing;
}

bool CastController::isPaused() const
{
    return m_active && m_active->transport() == CastRenderer::Transport::Paused;
}

std::optional<qint64> CastController::positionMs() const
{
    if (!m_active || !m_active->transport())
        return std::nullopt;
    return m_active->positionMs();
}

std::optional<qint64> CastController::durationMs() const
{
    if (!m_active || m_active->durationMs() <= 0)
        return std::nullopt;
    return m_active->durationMs();
}

bool CastController::canPause() const
{
    return m_active && m_active->canPause();
}

bool CastController::canSeek() const
{
    return m_active && m_active->canSeek();
}

bool CastController::hasVolume() const
{
    return m_active && m_active->hasVolume();
}

int CastController::volume() const
{
    return m_active ? m_active->volume() : -1;
}

bool CastController::muted() const
{
    return m_active && m_active->muted();
}

} // namespace Kinefin
