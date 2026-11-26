// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "KeychainTokenStore.h"

#include <QLoggingCategory>
#include <QTimer>
#include <qt6keychain/keychain.h>

namespace Kinefin {
Q_LOGGING_CATEGORY(keychainLog, "kinefin.settings.keychain")

namespace {
// The app id: QtKeychain's KWallet backend uses the service as the wallet folder, so the "Kinefin" folder of the KWallet-only builds is never read or overwritten
const QString serviceName = QStringLiteral("io.github.the_beper.Kinefin");
} // namespace

KeychainTokenStore::KeychainTokenStore(QObject *parent) : TokenStore(parent) {}

void KeychainTokenStore::read(const QString &key, ReadDone done)
{
    enqueue({Operation::Kind::Read, key, {}, std::move(done), {}});
}

void KeychainTokenStore::write(const QString &key, const QString &token, Done done)
{
    enqueue({Operation::Kind::Write, key, token, {}, std::move(done)});
}

void KeychainTokenStore::remove(const QString &key, Done done)
{
    enqueue({Operation::Kind::Remove, key, {}, {}, std::move(done)});
}

void KeychainTokenStore::enqueue(Operation operation)
{
    m_queue.append(std::move(operation));
    if (m_running)
        return;
    m_running = true;
    // Never start from inside the call: the callback contract is event-loop delivery
    QTimer::singleShot(0, this, [this] { startNext(); });
}

void KeychainTokenStore::startNext()
{
    if (m_queue.isEmpty()) {
        m_running = false;
        return;
    }
    const Operation &operation = m_queue.first();
    // No parent, autoDelete on: a running job is never deleted (QtKeychain cannot cancel one), and one that finishes after this store is gone calls nothing
    QKeychain::Job *job = nullptr;
    switch (operation.kind) {
    case Operation::Kind::Read:
        job = new QKeychain::ReadPasswordJob(serviceName);
        break;
    case Operation::Kind::Write: {
        auto *writeJob = new QKeychain::WritePasswordJob(serviceName);
        writeJob->setTextData(operation.token);
        job = writeJob;
        break;
    }
    case Operation::Kind::Remove:
        job = new QKeychain::DeletePasswordJob(serviceName);
        break;
    }
    job->setKey(operation.key);
    connect(job, &QKeychain::Job::finished, this, &KeychainTokenStore::finish);
    job->start();
}

void KeychainTokenStore::finish(QKeychain::Job *job)
{
    // Copied now, before anything can delete the job
    const QKeychain::Error error = job->error();
    QString text;
    if (const auto *readJob = qobject_cast<QKeychain::ReadPasswordJob *>(job))
        text = readJob->textData();
    const Operation operation = m_queue.takeFirst();

    bool ok = false;
    const char *what = "remove";
    switch (operation.kind) {
    case Operation::Kind::Read:
        what = "read";
        ok = error == QKeychain::NoError && !text.isEmpty();
        break;
    case Operation::Kind::Write:
        what = "write";
        ok = error == QKeychain::NoError;
        break;
    case Operation::Kind::Remove:
        ok = error == QKeychain::NoError || error == QKeychain::EntryNotFound;
        break;
    }
    if (!ok) {
        text.clear();
        // Fixed text, the operation and the code: never the key, the token or the backend's message. A read of an entry that is not there is expected once for every user after the update
        if (operation.kind == Operation::Kind::Read && error == QKeychain::EntryNotFound)
            qCDebug(keychainLog) << "keyring" << what << "found no entry";
        else
            qCWarning(keychainLog) << "keyring" << what << "failed, error" << int(error);
    }

    // Deferred: callbacks only ever run from the event loop, and a callback may enqueue more work, which the next start then finds
    QTimer::singleShot(0, this, [operation, ok, text] {
        if (operation.readDone)
            operation.readDone(ok, text);
        else if (operation.done)
            operation.done(ok);
    });
    QTimer::singleShot(0, this, [this] { startNext(); });
}
} // namespace Kinefin
