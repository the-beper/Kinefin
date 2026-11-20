// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/KRunnerProvider.h"

#include "api/JellyfinClient.h"
#include "integration/Mpris.h"
#include "models/SearchModel.h"

#include <KLocalizedString>

#include <QDBusAbstractAdaptor>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QJsonValue>
#include <QLatin1String>

#include <algorithm>
#include <utility>

namespace Kinefin {
namespace {
// The one category KRunner shows: a title never outranks an application launch
constexpr int kModerateRelevance = 50;
constexpr int kLowRelevance = 30;
constexpr int kMaxTextCodePoints = 200;
constexpr qint64 kRequestWindowMs = 60'000;

void registerKRunnerTypes()
{
    static const bool once = [] {
        qRegisterMetaType<KRunnerMatch>();
        qRegisterMetaType<KRunnerAction>();
        qRegisterMetaType<QList<KRunnerMatch>>("QList<Kinefin::KRunnerMatch>");
        qRegisterMetaType<QList<KRunnerAction>>("QList<Kinefin::KRunnerAction>");
        qDBusRegisterMetaType<KRunnerMatch>();
        qDBusRegisterMetaType<KRunnerAction>();
        qDBusRegisterMetaType<QList<KRunnerMatch>>();
        qDBusRegisterMetaType<QList<KRunnerAction>>();
        return true;
    }();
    Q_UNUSED(once);
}

QString iconFor(const QString &type)
{
    // Breeze's video-x-generic is a fixed grey filmstrip while video-symbolic is the same shape in the text colour; series, seasons and episodes share the TV icon, and the subtext names the kind
    if (type == QLatin1String("BoxSet"))
        return QStringLiteral("folder-videos");
    if (type == QLatin1String("Series") || type == QLatin1String("Season")
        || type == QLatin1String("Episode"))
        return QStringLiteral("video-television");
    return QStringLiteral("video-symbolic");
}

QString subtextFor(const ItemSummary &item)
{
    if (item.type == QLatin1String("Movie"))
        return item.year > 0 ? i18nc("@item:inmenu movie with its year", "Movie, %1", item.year)
                             : i18n("Movie");
    if (item.type == QLatin1String("Series"))
        return item.year > 0 ? i18nc("@item:inmenu series with its year", "Series, %1", item.year)
                             : i18n("Series");
    if (item.type == QLatin1String("Episode")) {
        const QString series = krunnerText(item.seriesName);
        return !series.isEmpty() && item.seasonNumber >= 0 && item.episodeNumber >= 0
                   ? i18nc("@item:inmenu series, season and episode", "%1, S%2:E%3", series,
                           item.seasonNumber, item.episodeNumber)
                   : i18n("Episode");
    }
    return i18n("Collection");
}
} // namespace

QDBusArgument &operator<<(QDBusArgument &argument, const KRunnerMatch &match)
{
    argument.beginStructure();
    argument << match.id << match.text << match.iconName << match.categoryRelevance
             << match.relevance << match.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, KRunnerMatch &match)
{
    argument.beginStructure();
    argument >> match.id >> match.text >> match.iconName >> match.categoryRelevance
        >> match.relevance >> match.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const KRunnerAction &action)
{
    argument.beginStructure();
    argument << action.id << action.text << action.iconName;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, KRunnerAction &action)
{
    argument.beginStructure();
    argument >> action.id >> action.text >> action.iconName;
    argument.endStructure();
    return argument;
}

QString krunnerText(const QString &text)
{
    QString clean = mprisText(text);
    clean.replace(QLatin1Char('<'), QChar(0xFF1C));
    // The cap counts code points, and a pair is never cut in half
    int cut = 0;
    int points = 0;
    while (cut < clean.size() && points < kMaxTextCodePoints) {
        cut += clean.at(cut).isHighSurrogate() && cut + 1 < clean.size() ? 2 : 1;
        ++points;
    }
    clean.truncate(cut);
    return clean;
}

QList<KRunnerMatch> krunnerMatches(const QList<ItemSummary> &items, const QString &term)
{
    QList<KRunnerMatch> matches;
    int rank = 0;
    for (const ItemSummary &item : items) {
        if (!item.valid || item.virtualItem || item.name.isEmpty())
            continue;
        const QString name = item.name.trimmed();
        const bool exact = QString::compare(name, term, Qt::CaseInsensitive) == 0;
        const bool prefix = name.startsWith(term, Qt::CaseInsensitive);
        KRunnerMatch match;
        match.id = item.id;
        // milou shows the subtext only on the current row: an episode names its series in the text
        match.text = item.type == QLatin1String("Episode") && !item.seriesName.isEmpty()
                         ? krunnerText(i18nc("@item:inmenu episode and the series it belongs to",
                                             "%1 — %2", item.name, item.seriesName))
                         : krunnerText(item.name);
        match.iconName = iconFor(item.type);
        match.categoryRelevance = exact ? kModerateRelevance : kLowRelevance;
        match.relevance = exact ? 1.0 : prefix ? 0.9 : std::max(0.1, 0.8 - 0.02 * rank);
        match.properties = {{QStringLiteral("subtext"), subtextFor(item)}};
        const bool playable = isPlayableVideo(item.type);
        if (playable && item.positionTicks > 0)
            match.properties.insert(QStringLiteral("actions"),
                                    QStringList{QStringLiteral("resume")});
        else if (playable)
            match.properties.insert(QStringLiteral("actions"), QStringList{QStringLiteral("play")});
        else
            match.properties.insert(QStringLiteral("actions"), QStringList{});
        matches.append(match);
        ++rank;
        if (matches.size() >= KRunnerProvider::MaxTitles + KRunnerProvider::MaxEpisodes)
            break;
    }
    return matches;
}

// org.kde.krunner1: every call goes to the provider; Match answers later, from the server's reply.
// An adaptor, as QtDBus relays a call to it while the QDBusContext (and so the message a delayed
// reply needs) lives on the object the adaptor belongs to
class KRunnerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.krunner1")
  public:
    explicit KRunnerAdaptor(KRunnerProvider *provider)
        : QDBusAbstractAdaptor(provider), m_provider(provider)
    {
    }

  public Q_SLOTS:
    QList<KRunnerAction> Actions() const
    {
        return {{QStringLiteral("play"), i18n("Play"), QStringLiteral("media-playback-start")},
                {QStringLiteral("resume"), i18n("Resume"), QStringLiteral("media-playback-start")}};
    }
    void Match(const QString &query) { m_provider->handleMatch(query, m_provider->callMessage()); }
    void Run(const QString &matchId, const QString &actionId)
    {
        m_provider->handleRun(matchId, actionId);
    }
    void SetActivationToken(const QString &token) { m_provider->setActivationToken(token); }
    void Teardown() { m_provider->teardown(); }

  private:
    KRunnerProvider *m_provider;
};

KRunnerProvider::KRunnerProvider(JellyfinClient &client, const QDBusConnection &bus,
                                 QObject *parent)
    : QObject(parent), m_client(client), m_bus(bus)
{
    registerKRunnerTypes();
    m_clock.start();
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(DebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, &KRunnerProvider::startRequest);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(ReplyTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_pendingActive)
            return;
        releaseChannel();
        // The half that came in time is answered, as a failed half would be, and not cached
        if (!m_titles && !m_episodes) {
            answerPending({});
            return;
        }
        m_partFailed = true;
        if (!m_titles)
            m_titles = QJsonArray();
        if (!m_episodes)
            m_episodes = QJsonArray();
        finishIfComplete();
    });
    connect(&m_client, &JellyfinClient::itemsReady, this,
            [this](const QJsonArray &items, int, int channel) { onItemsReady(items, channel); });
    connect(&m_client, &JellyfinClient::listingFailed, this,
            [this](const QString &family, int, const QString &, int channel) {
                onListingFailed(family, channel);
            });
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] {
        teardown();
        m_offered.clear();
        m_offeredOrder.clear();
        m_token.clear();
        m_tokenAt = 0;
    });
}

KRunnerProvider::~KRunnerProvider()
{
    if (!m_registered)
        return;
    m_bus.unregisterService(QString::fromLatin1(ServiceName));
    m_bus.unregisterObject(QString::fromLatin1(ObjectPath));
}

bool KRunnerProvider::registerOnBus()
{
    if (m_registered)
        return true;
    if (!m_bus.isConnected()) {
        qWarning("KRunner is off: no session bus");
        return false;
    }
    m_adaptor = new KRunnerAdaptor(this);
    if (!m_bus.registerObject(QString::fromLatin1(ObjectPath), this,
                              QDBusConnection::ExportAdaptors)) {
        delete m_adaptor;
        m_adaptor = nullptr;
        qWarning("KRunner is off: the object path is taken");
        return false;
    }
    // The wildcard plugin file matches an owned subname: never queue or hand the name over
    const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply =
        m_bus.interface()->registerService(QString::fromLatin1(ServiceName),
                                           QDBusConnectionInterface::DontQueueService,
                                           QDBusConnectionInterface::DontAllowReplacement);
    if (!reply.isValid() || reply.value() != QDBusConnectionInterface::ServiceRegistered) {
        m_bus.unregisterObject(QString::fromLatin1(ObjectPath));
        delete m_adaptor;
        m_adaptor = nullptr;
        qWarning("KRunner is off: the bus name is taken");
        return false;
    }
    m_registered = true;
    return true;
}

void KRunnerProvider::handleMatch(const QString &query, const QDBusMessage &message)
{
    // A newer query supersedes the one still waiting, and its requests: a late answer must not stand for the new term
    if (m_pendingActive) {
        releaseChannel();
        answerPending({});
    }
    m_titles.reset();
    m_episodes.reset();
    const QString term = SearchModel::normalized(query);
    QDBusMessage reply = message;
    reply.setDelayedReply(true);
    if (term.size() < MinTermLength || !m_client.isAuthenticated() || m_client.isPlainHttpRemote()
        || budgetSpent()) {
        m_bus.send(reply.createReply(QVariant::fromValue(QList<KRunnerMatch>())));
        return;
    }
    if (const auto cached = m_cache.constFind(term); cached != m_cache.cend()) {
        offer(cached->offered);
        m_bus.send(reply.createReply(QVariant::fromValue(cached->matches)));
        return;
    }
    m_pending = reply;
    m_pendingTerm = term;
    m_pendingActive = true;
    m_debounce.start();
}

void KRunnerProvider::startRequest()
{
    if (!m_pendingActive)
        return;
    if (!m_client.isAuthenticated() || m_client.isPlainHttpRemote() || budgetSpent()) {
        answerPending({});
        return;
    }
    ItemQuery titles;
    titles.searchTerm = m_pendingTerm;
    titles.includeItemTypes = QStringLiteral("Movie,Series,BoxSet");
    titles.recursive = true;
    titles.limit = MaxTitles;
    ItemQuery episodes = titles;
    episodes.includeItemTypes = QStringLiteral("Episode");
    episodes.limit = MaxEpisodes;
    m_titles.reset();
    m_episodes.reset();
    m_partFailed = false;
    if (!m_client.fetchItems(titles, Channel) || !m_client.fetchItems(episodes, EpisodeChannel)) {
        releaseChannel();
        answerPending({});
        return;
    }
    // The budget counts server requests, not refused calls: one entry for each of the two
    for (int request = 0; request < 2; ++request)
        m_requestTimes.append(m_clock.elapsed());
    m_timeout.start();
}

void KRunnerProvider::onItemsReady(const QJsonArray &items, int channel)
{
    if ((channel != Channel && channel != EpisodeChannel) || !m_pendingActive)
        return;
    (channel == Channel ? m_titles : m_episodes) = items;
    finishIfComplete();
}

void KRunnerProvider::finishIfComplete()
{
    if (!m_pendingActive || !m_titles || !m_episodes)
        return;
    m_timeout.stop();
    QJsonArray items = *m_titles;
    for (const QJsonValue &value : std::as_const(*m_episodes))
        items.append(value);
    QList<ItemSummary> summaries;
    QHash<QString, Offered> offered;
    for (const QJsonValue &value : std::as_const(items)) {
        if (!value.isObject())
            continue;
        const ItemSummary item = parseItemSummary(value.toObject());
        if (!item.valid || item.virtualItem || item.name.isEmpty())
            continue;
        summaries.append(item);
        offered.insert(item.id, Offered{item.type, item.positionTicks, 0});
    }
    const QList<KRunnerMatch> matches = krunnerMatches(summaries, m_pendingTerm);
    // Only the rows that fit keep an id
    QHash<QString, Offered> kept;
    for (const KRunnerMatch &match : matches) {
        if (const auto found = offered.constFind(match.id); found != offered.cend())
            kept.insert(match.id, *found);
    }
    offer(kept);
    if (!m_partFailed)
        remember(m_pendingTerm, CachedTerm{matches, kept});
    answerPending(matches);
}

void KRunnerProvider::onListingFailed(const QString &family, int channel)
{
    if ((channel != Channel && channel != EpisodeChannel) || family != QLatin1String("Items")
        || !m_pendingActive)
        return;
    m_client.releaseListingChannel(channel);
    m_partFailed = true;
    (channel == Channel ? m_titles : m_episodes) = QJsonArray();
    finishIfComplete();
}

void KRunnerProvider::handleRun(const QString &itemId, const QString &actionId)
{
    const auto found = m_offered.constFind(itemId);
    if (found == m_offered.cend())
        return;
    if (found->at + OfferedIdValidityMs <= m_clock.elapsed()) {
        m_offered.remove(itemId);
        return;
    }
    if (actionId.isEmpty()) {
        Q_EMIT openRequested(itemId, takeToken());
        return;
    }
    if (!isPlayableVideo(found->type))
        return;
    if (actionId == QLatin1String("play"))
        Q_EMIT playRequested(itemId, 0, takeToken());
    else if (actionId == QLatin1String("resume") && found->positionTicks > 0)
        Q_EMIT playRequested(itemId, found->positionTicks, takeToken());
}

void KRunnerProvider::setActivationToken(const QString &token)
{
    const bool printable = std::all_of(token.cbegin(), token.cend(), [](QChar character) {
        return character.unicode() >= 0x20 && character.unicode() <= 0x7e;
    });
    if (token.size() > MaxTokenLength || !printable)
        return;
    m_token = token;
    m_tokenAt = m_clock.elapsed();
}

QString KRunnerProvider::takeToken()
{
    const QString token = std::exchange(m_token, QString());
    const qint64 at = std::exchange(m_tokenAt, 0);
    return !token.isEmpty() && at + TokenValidityMs > m_clock.elapsed() ? token : QString();
}

void KRunnerProvider::teardown()
{
    cancelPending();
    releaseChannel();
    m_cache.clear();
    m_cacheOrder.clear();
}

void KRunnerProvider::answerPending(const QList<KRunnerMatch> &matches)
{
    if (!m_pendingActive)
        return;
    m_pendingActive = false;
    m_debounce.stop();
    m_timeout.stop();
    m_bus.send(m_pending.createReply(QVariant::fromValue(matches)));
    m_pending = {};
    m_pendingTerm.clear();
}

void KRunnerProvider::cancelPending()
{
    if (!m_pendingActive)
        return;
    m_pendingActive = false;
    m_debounce.stop();
    m_timeout.stop();
    m_bus.send(m_pending.createReply(QVariant::fromValue(QList<KRunnerMatch>())));
    m_pending = {};
    m_pendingTerm.clear();
}

void KRunnerProvider::releaseChannel()
{
    m_timeout.stop();
    m_client.releaseListingChannel(Channel);
    m_client.releaseListingChannel(EpisodeChannel);
}

bool KRunnerProvider::budgetSpent()
{
    const qint64 now = m_clock.elapsed();
    m_requestTimes.erase(std::remove_if(m_requestTimes.begin(), m_requestTimes.end(),
                                        [now](qint64 at) { return at + kRequestWindowMs <= now; }),
                         m_requestTimes.end());
    return m_requestTimes.size() >= RequestBudget;
}

void KRunnerProvider::offer(const QHash<QString, Offered> &offered)
{
    const qint64 now = m_clock.elapsed();
    for (auto it = m_offered.begin(); it != m_offered.end();) {
        if (it->at + OfferedIdValidityMs <= now)
            it = m_offered.erase(it);
        else
            ++it;
    }
    for (auto it = offered.cbegin(); it != offered.cend(); ++it) {
        Offered value = it.value();
        value.at = now;
        m_offered.insert(it.key(), value);
        m_offeredOrder.removeAll(it.key());
        m_offeredOrder.append(it.key());
    }
    while (m_offeredOrder.size() > MaxOfferedIds)
        m_offered.remove(m_offeredOrder.takeFirst());
}

void KRunnerProvider::remember(const QString &term, const CachedTerm &entry)
{
    m_cache.insert(term, entry);
    m_cacheOrder.removeAll(term);
    m_cacheOrder.append(term);
    while (m_cacheOrder.size() > MaxCachedTerms)
        m_cache.remove(m_cacheOrder.takeFirst());
}
} // namespace Kinefin

#include "KRunnerProvider.moc"
