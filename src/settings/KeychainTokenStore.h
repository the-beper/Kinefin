// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>

#include "settings/TokenStore.h"

namespace QKeychain {
class Job;
}

namespace Kinefin {
// Tokens in the system keyring through QtKeychain (KWallet on a native Plasma build, the Secret Service through libsecret elsewhere and in the Flatpak), under the service of the app id. One job runs at a time, in the order the operations were asked, so a remove followed by a write of the same key keeps its meaning. There is no timeout: a keyring that never answers (an unanswered unlock prompt) keeps its operation and the ones behind it pending; without a keyring every operation fails
class KeychainTokenStore final : public TokenStore {
    Q_OBJECT

  public:
    explicit KeychainTokenStore(QObject *parent = nullptr);

    void read(const QString &key, ReadDone done) override;
    void write(const QString &key, const QString &token, Done done) override;
    void remove(const QString &key, Done done) override;

  private:
    struct Operation {
        enum class Kind { Read, Write, Remove };
        Kind kind{};
        QString key;
        QString token;
        ReadDone readDone;
        Done done;
    };

    void enqueue(Operation operation);
    void startNext();
    void finish(QKeychain::Job *job);

    // The front operation is the one running while m_running is set
    QList<Operation> m_queue;
    bool m_running = false;
};
} // namespace Kinefin
