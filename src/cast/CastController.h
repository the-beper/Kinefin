// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cast/CastTargetsModel.h"
#include "cast/DlnaRenderer.h"
#include "cast/Ssdp.h"
#include "cast/UpnpDescription.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

class QNetworkReply;

namespace Kinefin {

// Discovery and control of the DLNA renderers the picker lists. A search runs only while it is asked for; a renderer is listed once its description, SCPD and GetProtocolInfo answer. The renderer's own network traffic stays in its DlnaRenderer
class CastController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(Kinefin::CastTargetsModel *targets READ targets CONSTANT)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

  public:
    explicit CastController(QObject *parent = nullptr);
    explicit CastController(const SsdpSearch::Options &options, QObject *parent = nullptr);
    ~CastController() override;

    CastTargetsModel *targets() const { return m_targets; }
    bool searching() const;
    QString error() const { return m_error; }

    Q_INVOKABLE void search();
    Q_INVOKABLE void stopSearch();
    // Adds a renderer whose description the caller already read, as a discovery answer would (tests, a future manual entry)
    void addRenderer(const RendererInfo &info);

    bool load(const QString &udn, const QUrl &url, const QString &mime, const QString &title,
              qint64 startMs, qint64 durationMs, const QUrl &serverBase, bool direct);
    // The active transcode again from `url`, which starts at `startMs`
    bool reload(const QUrl &url, qint64 startMs);
    void setDurationMs(qint64 durationMs);
    bool pause();
    bool resume();
    bool seek(qint64 positionMs);
    bool stop();
    bool setVolume(int volume);
    bool setMuted(bool muted);

    QString activeUdn() const;
    QString activeName() const;
    const RendererInfo *infoFor(const QString &udn) const;
    // The name the picker shows for a renderer: its friendly name, else its address
    QString nameFor(const QString &udn) const;
    bool active() const;
    bool isPlaying() const;
    bool isPaused() const;
    std::optional<qint64> positionMs() const;
    std::optional<qint64> durationMs() const;
    bool canPause() const;
    bool canSeek() const;
    bool hasVolume() const;
    int volume() const;
    bool muted() const;

  Q_SIGNALS:
    void searchingChanged();
    void errorChanged();
    void targetsChanged();
    void playing();
    void paused();
    void buffering();
    void stopped();
    void ended();
    void failed(Kinefin::DlnaRenderer::Failure failure);
    void positionChanged();
    void volumeChanged();

  private:
    void fetchRenderer(const SsdpResponse &response);
    void fetchDocument(const QUrl &url, int limit, int timeoutMs,
                       std::function<void(bool, const QByteArray &)> done);
    void fetchProtocolInfo(const RendererInfo &info, std::function<void(QStringList)> done);
    void setActive(DlnaRenderer *renderer);
    void clearActive();
    void setError(const QString &error);
    void connectRenderer(DlnaRenderer *renderer);
    void addTarget(const RendererInfo &info);
    void dropUnseen();

    CastTargetsModel *m_targets = nullptr;
    SsdpSearch m_search;
    QNetworkAccessManager m_discovery;
    QHash<QString, DlnaRenderer *> m_renderers;
    QSet<QString> m_pending;
    // The UDNs that answered the running or the last search
    QSet<QString> m_seen;
    DlnaRenderer *m_active = nullptr;
    QString m_error;
};

} // namespace Kinefin
