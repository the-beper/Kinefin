// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

namespace Kinefin {

// The token-less range probe of a direct play: the renderer fetches the URL with no credential, so the same request without one tells whether the server lets it in, before anything reaches the renderer. One `Range: bytes=0-0` GET, no cookies, at most kMaxProbeBytes read, a 5 s timeout
class StreamProbe : public QObject {
    Q_OBJECT

  public:
    static constexpr int TimeoutMs = 5000;

    explicit StreamProbe(QNetworkAccessManager *network, QObject *parent = nullptr);
    ~StreamProbe() override;

    bool running() const { return !m_reply.isNull(); }
    // Starts the probe of `url`; a running one is aborted first and its answer dropped
    void start(const QUrl &url);
    // Aborts without finished()
    void abort();

  Q_SIGNALS:
    // The HTTP status the server sent, 0 when there was none (timeout, connection error)
    void finished(int status);

  private:
    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
};

} // namespace Kinefin
