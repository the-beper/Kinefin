// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace Kinefin {
// Where access tokens live: the one place allowed to hold them outside memory (KWallet in the app, a fake in tests). Every operation is asynchronous and its callback runs from the event loop, never from inside the call; a callback may arrive after the caller lost interest, so callers guard it. Keys are opaque to the store. A token is never logged, by the store or by its callers
class TokenStore : public QObject {
    Q_OBJECT

  public:
    using ReadDone = std::function<void(bool ok, const QString &token)>;
    using Done = std::function<void(bool ok)>;

    explicit TokenStore(QObject *parent = nullptr) : QObject(parent) {}

    // `ok` is false when the store is unavailable or denied access, or the key does not exist
    virtual void read(const QString &key, ReadDone done) = 0;
    virtual void write(const QString &key, const QString &token, Done done) = 0;
    // Removing a key that does not exist succeeds
    virtual void remove(const QString &key, Done done) = 0;
};
} // namespace Kinefin
