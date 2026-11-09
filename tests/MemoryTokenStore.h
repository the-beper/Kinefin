// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// An in-memory TokenStore for tests, with the failure and timing switches the credential paths need. Header-only, like FakeJellyfin.h.
#pragma once

#include "settings/TokenStore.h"

#include <QHash>
#include <QList>
#include <QTimer>

#include <functional>

namespace {

class MemoryTokenStore final : public Kinefin::TokenStore {
  public:
    QHash<QString, QString> tokens;
    bool failReads = false;
    bool failWrites = false;
    bool failRemoves = false;
    // Keeps every operation (and its effect) back until release().
    bool hold = false;
    int reads = 0;
    int writes = 0;
    int removes = 0;

    void read(const QString &key, ReadDone done) override
    {
        ++reads;
        schedule([this, key, done] {
            const bool ok = !failReads && tokens.contains(key);
            done(ok, ok ? tokens.value(key) : QString());
        });
    }
    void write(const QString &key, const QString &token, Done done) override
    {
        ++writes;
        schedule([this, key, token, done] {
            const bool ok = !failWrites;
            if (ok)
                tokens.insert(key, token);
            done(ok);
        });
    }
    void remove(const QString &key, Done done) override
    {
        ++removes;
        schedule([this, key, done] {
            const bool ok = !failRemoves;
            if (ok)
                tokens.remove(key);
            done(ok);
        });
    }

    void release()
    {
        hold = false;
        const auto held = std::exchange(m_held, {});
        for (const auto &action : held)
            QTimer::singleShot(0, this, action);
    }

  private:
    void schedule(std::function<void()> action)
    {
        if (hold)
            m_held.append(std::move(action));
        else
            QTimer::singleShot(0, this, std::move(action));
    }
    QList<std::function<void()>> m_held;
};

} // namespace
