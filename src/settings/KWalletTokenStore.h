// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QPointer>

#include "settings/TokenStore.h"

namespace KWallet {
class Wallet;
}

namespace Kinefin {
// Tokens in the user's default KWallet, folder "Kinefin". The wallet is opened asynchronously on the first operation (so an unlock prompt never blocks the UI and nothing is asked at startup when no token is needed), kept open, and reopened after it was closed. Operations queued while it opens run in order; if KWallet is disabled, missing or refused, they fail
class KWalletTokenStore final : public TokenStore {
    Q_OBJECT

  public:
    explicit KWalletTokenStore(QObject *parent = nullptr);
    ~KWalletTokenStore() override;

    void read(const QString &key, ReadDone done) override;
    void write(const QString &key, const QString &token, Done done) override;
    void remove(const QString &key, Done done) override;

  private:
    struct Operation {
        enum class Kind { Read, Write, Remove };
        Kind kind;
        QString key;
        QString token;
        ReadDone readDone;
        Done done;
    };

    void enqueue(Operation operation);
    void openWallet();
    void onOpened(bool success);
    void onClosed();
    void runQueued();
    void run(const Operation &operation);
    void failQueued();
    void dropWallet();

    QPointer<KWallet::Wallet> m_wallet;
    QList<Operation> m_queue;
    bool m_opening = false;
    bool m_ready = false;
};
} // namespace Kinefin
