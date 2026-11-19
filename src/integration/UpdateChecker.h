// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <optional>

class QNetworkReply;

namespace Kinefin {

// Checks Kinefin's own published releases, independently of the Jellyfin server. The releases/latest endpoint supplies only a redirect: no untrusted HTML or release assets are downloaded, and only a same-origin, final-version tag is ever offered to the user as a URL to open in their browser
class UpdateChecker : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

  public:
    explicit UpdateChecker(QObject *parent = nullptr);
    // A local endpoint and explicit version are used by the offline HTTP tests
    UpdateChecker(const QString &currentVersion, const QUrl &latestUrl, QObject *parent = nullptr);
    ~UpdateChecker() override;

    bool checking() const { return !m_reply.isNull(); }
    Q_INVOKABLE bool check();
    Q_INVOKABLE void cancel();

    // -1: candidate older; 0: equal; 1: candidate newer; nullopt: invalid (bounded, numeric) version. Leading v, pre-release labels and malformed components cannot turn into an update prompt
    static std::optional<int> compareVersions(const QString &current, const QString &candidate);

  Q_SIGNALS:
    void checkingChanged();
    void updateAvailable(const QString &version, const QUrl &releaseUrl);
    void upToDate();
    void checkFailed();

  private:
    QUrl releaseFromRedirect(const QUrl &target) const;

    const QString m_currentVersion;
    const QUrl m_latestUrl;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};

} // namespace Kinefin
