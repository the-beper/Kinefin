// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "KWalletTokenStore.h"

#include <QLoggingCategory>
#include <QTimer>
#include <KWallet>

namespace Kinefin {
Q_LOGGING_CATEGORY(walletLog, "kinefin.settings.wallet")

namespace {
const QString folderName = QStringLiteral("Kinefin");
} // namespace

KWalletTokenStore::KWalletTokenStore(QObject *parent) : TokenStore(parent) {}

KWalletTokenStore::~KWalletTokenStore()
{
    delete m_wallet;
}

void KWalletTokenStore::read(const QString &key, ReadDone done)
{
    enqueue({Operation::Kind::Read, key, {}, std::move(done), {}});
}

void KWalletTokenStore::write(const QString &key, const QString &token, Done done)
{
    enqueue({Operation::Kind::Write, key, token, {}, std::move(done)});
}

void KWalletTokenStore::remove(const QString &key, Done done)
{
    enqueue({Operation::Kind::Remove, key, {}, {}, std::move(done)});
}

void KWalletTokenStore::enqueue(Operation operation)
{
    m_queue.append(std::move(operation));
    if (m_ready)
        // Never run from inside the call: the callback contract is event-loop delivery.
        QTimer::singleShot(0, this, [this] { runQueued(); });
    else if (!m_opening)
        openWallet();
}

void KWalletTokenStore::openWallet()
{
    if (!KWallet::Wallet::isEnabled()) {
        qCWarning(walletLog) << "KWallet is disabled";
        QTimer::singleShot(0, this, [this] { failQueued(); });
        return;
    }
    // Window id 0: there is no native parent to attach the unlock prompt to.
    m_wallet = KWallet::Wallet::openWallet(KWallet::Wallet::LocalWallet(), 0,
                                           KWallet::Wallet::Asynchronous);
    if (!m_wallet) {
        qCWarning(walletLog) << "KWallet could not be opened";
        QTimer::singleShot(0, this, [this] { failQueued(); });
        return;
    }
    m_opening = true;
    connect(m_wallet, &KWallet::Wallet::walletOpened, this, &KWalletTokenStore::onOpened);
    connect(m_wallet, &KWallet::Wallet::walletClosed, this, &KWalletTokenStore::onClosed);
}

void KWalletTokenStore::onOpened(bool success)
{
    m_opening = false;
    if (success && m_wallet) {
        const bool folderReady =
            (m_wallet->hasFolder(folderName) || m_wallet->createFolder(folderName))
            && m_wallet->setFolder(folderName);
        if (folderReady) {
            m_ready = true;
            runQueued();
            return;
        }
        qCWarning(walletLog) << "the Kinefin folder of KWallet is not usable";
    } else {
        qCWarning(walletLog) << "KWallet refused access or is unavailable";
    }
    dropWallet();
    failQueued();
}

void KWalletTokenStore::onClosed()
{
    // Closed by the user or the daemon: the next operation opens it again. Work still waiting for an open that will not complete now fails instead of waiting forever.
    const bool wasOpening = m_opening;
    dropWallet();
    if (wasOpening)
        failQueued();
}

void KWalletTokenStore::dropWallet()
{
    m_ready = false;
    m_opening = false;
    if (m_wallet) {
        m_wallet->disconnect(this);
        m_wallet->deleteLater();
        m_wallet.clear();
    }
}

void KWalletTokenStore::runQueued()
{
    // A callback may enqueue more work: take what is queued now.
    const QList<Operation> batch = std::exchange(m_queue, {});
    for (const Operation &operation : batch)
        run(operation);
}

void KWalletTokenStore::run(const Operation &operation)
{
    if (!m_ready || !m_wallet) {
        // The wallet was closed between queueing and running: open it again for this one.
        m_queue.append(operation);
        if (!m_opening)
            openWallet();
        return;
    }
    bool ok = false;
    QString token;
    switch (operation.kind) {
    case Operation::Kind::Read:
        ok = m_wallet->readPassword(operation.key, token) == 0 && !token.isEmpty();
        if (!ok)
            token.clear();
        break;
    case Operation::Kind::Write:
        ok = m_wallet->writePassword(operation.key, operation.token) == 0;
        break;
    case Operation::Kind::Remove:
        ok = !m_wallet->hasEntry(operation.key) || m_wallet->removeEntry(operation.key) == 0;
        break;
    }
    // Deferred, whatever happened above: callbacks only ever run from the event loop.
    QTimer::singleShot(0, this, [operation, ok, token] {
        if (operation.readDone)
            operation.readDone(ok, token);
        else if (operation.done)
            operation.done(ok);
    });
}

void KWalletTokenStore::failQueued()
{
    const QList<Operation> batch = std::exchange(m_queue, {});
    for (const Operation &operation : batch) {
        if (operation.readDone)
            operation.readDone(false, {});
        else if (operation.done)
            operation.done(false);
    }
}
} // namespace Kinefin
