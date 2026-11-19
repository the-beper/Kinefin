// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "models/ItemSummary.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QMetaType>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

namespace Kinefin {
class JellyfinClient;

// One `a(sssida{sv})` entry of `org.kde.krunner1.Match`: the category relevance is KRunner's
// Moderate (50) or Low (30), never the name of a category
struct KRunnerMatch {
    QString id;
    QString text;
    QString iconName;
    int categoryRelevance = 0;
    double relevance = 0;
    QVariantMap properties;
};
QDBusArgument &operator<<(QDBusArgument &argument, const KRunnerMatch &match);
const QDBusArgument &operator>>(const QDBusArgument &argument, KRunnerMatch &match);

// One `a(sss)` entry of `org.kde.krunner1.Actions`
struct KRunnerAction {
    QString id;
    QString text;
    QString iconName;
};
QDBusArgument &operator<<(QDBusArgument &argument, const KRunnerAction &action);
const QDBusArgument &operator>>(const QDBusArgument &argument, KRunnerAction &action);

// Server text made fit for KRunner: the MPRIS rules, every `<` as U+FF1C (no renderer takes it
// for markup) and at most 200 code points
QString krunnerText(const QString &text);
// The rows a search answer becomes, in the server's order: text, icon by type, relevance and
// subtext; playable items also carry their action. The caller drops nothing but an invalid entry
QList<KRunnerMatch> krunnerMatches(const QList<ItemSummary> &items, const QString &term);

// The `org.kde.krunner1` runner of Kinefin, exported at `/runner` under
// `io.github.the_beper.Kinefin.Runner`: KRunner's queries become one combined Items request on
// channel 6144, its Run reaches the shell through openRequested and playRequested
class KRunnerProvider final : public QObject, protected QDBusContext {
    Q_OBJECT
  public:
    static constexpr int Channel = 6144;
    static constexpr int MinTermLength = 3;
    static constexpr int DebounceMs = 300;
    static constexpr int ReplyTimeoutMs = 3000;
    static constexpr int MaxResults = 10;
    static constexpr int RequestBudget = 60;
    static constexpr int MaxCachedTerms = 16;
    static constexpr int MaxOfferedIds = 50;
    static constexpr qint64 OfferedIdValidityMs = 2 * 60 * 1000;
    static constexpr int MaxTokenLength = 256;
    static constexpr qint64 TokenValidityMs = 10 * 1000;
    static constexpr auto ServiceName = "io.github.the_beper.Kinefin.Runner";
    static constexpr auto ObjectPath = "/runner";

    KRunnerProvider(JellyfinClient &client, const QDBusConnection &bus, QObject *parent = nullptr);
    ~KRunnerProvider() override;

    // Exports the object and takes the bus name; false, with one warning, when there is no session
    // bus or the name is taken
    bool registerOnBus();
    // The call the bus is delivering to this object: the adaptor reads it for the delayed Match reply
    QDBusMessage callMessage() const { return calledFromDBus() ? message() : QDBusMessage(); }

  Q_SIGNALS:
    void openRequested(const QString &itemId, const QString &activationToken);
    void playRequested(const QString &itemId, qint64 resumeTicks, const QString &activationToken);

  private:
    friend class KRunnerAdaptor;

    struct Offered {
        QString type;
        qint64 positionTicks = 0;
        qint64 at = 0;
    };
    struct CachedTerm {
        QList<KRunnerMatch> matches;
        QHash<QString, Offered> offered;
    };

    void handleMatch(const QString &query, const QDBusMessage &message);
    void handleRun(const QString &itemId, const QString &actionId);
    void setActivationToken(const QString &token);
    void teardown();
    void startRequest();
    void onItemsReady(const QJsonArray &items, int channel);
    void onListingFailed(const QString &family, int channel);
    void answerPending(const QList<KRunnerMatch> &matches);
    void cancelPending();
    void releaseChannel();
    QString takeToken();
    bool budgetSpent();
    void offer(const QHash<QString, Offered> &offered);
    void remember(const QString &term, const CachedTerm &entry);

    JellyfinClient &m_client;
    QDBusConnection m_bus;
    bool m_registered = false;
    QObject *m_adaptor = nullptr;
    QElapsedTimer m_clock;
    QTimer m_debounce;
    QTimer m_timeout;
    QDBusMessage m_pending;
    QString m_pendingTerm;
    bool m_pendingActive = false;
    QHash<QString, CachedTerm> m_cache;
    QStringList m_cacheOrder;
    QHash<QString, Offered> m_offered;
    QStringList m_offeredOrder;
    QList<qint64> m_requestTimes;
    QString m_token;
    qint64 m_tokenAt = 0;
};
} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::KRunnerMatch)
Q_DECLARE_METATYPE(Kinefin::KRunnerAction)
Q_DECLARE_METATYPE(QList<Kinefin::KRunnerMatch>)
Q_DECLARE_METATYPE(QList<Kinefin::KRunnerAction>)
