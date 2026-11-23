// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/StreamProbe.h"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <memory>

namespace Kinefin {
namespace {
// A range probe answer is one byte; a server that ignores the range is cut off well before a whole file
constexpr qint64 kMaxProbeBytes = 64 * 1024;
} // namespace

StreamProbe::StreamProbe(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_network(network)
{
}

StreamProbe::~StreamProbe()
{
    abort();
}

void StreamProbe::abort()
{
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void StreamProbe::start(const QUrl &url)
{
    abort();
    QNetworkRequest request(url);
    // No cookie is sent or kept, and no redirect is followed, whatever the server sets
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Range", "bytes=0-0");
    request.setTransferTimeout(TimeoutMs);
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    auto received = std::make_shared<qint64>(0);
    connect(reply, &QNetworkReply::readyRead, this, [reply, received] {
        *received += reply->readAll().size();
        if (*received > kMaxProbeBytes)
            reply->abort();
    });
    QTimer::singleShot(TimeoutMs, reply, [reply] { reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply != m_reply)
            return;
        m_reply = nullptr;
        Q_EMIT finished(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    });
}

} // namespace Kinefin
