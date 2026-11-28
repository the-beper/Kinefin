// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConnectionController.h"

#include "api/MediaSourceSelection.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <KLocalizedString>

#include <algorithm>

#include "integration/LinkRouter.h"
#include "settings/KeychainTokenStore.h"
#include "settings/ProfilePaths.h"

namespace Kinefin {
Q_LOGGING_CATEGORY(controllerLog, "kinefin.app.connection")
// The store drops a view with more genre ids than it accepts: it must hold what the model sends
static_assert(BrowseStateStore::MaxGenreIds >= MaxGenreIds);
static_assert(BrowseStateStore::MaxHashes >= MaxFilterValues);
static_assert(BrowseStateStore::MaxYears >= MaxFilterValues);

QString ConnectionController::scopeKey(const QString &type, const QString &subtab,
                                       const QString &libraryId)
{
    QString id = libraryId.toLower();
    id.remove(QLatin1Char('-'));
    const auto withLibrary = [&id](const QString &key) -> QString {
        if (id.isEmpty())
            return key;
        return key + QLatin1Char(':') + id;
    };
    if (subtab.isEmpty())
        return withLibrary(type);
    const bool movies = type == QLatin1String("movies");
    const bool shows = type == QLatin1String("tvshows");
    if ((movies || shows) && subtab == QLatin1String("favorites"))
        return withLibrary(type + QStringLiteral("/favorites"));
    // Collections belong to no one library, and the list is the same under both types
    if ((movies || shows) && subtab == QLatin1String("collections"))
        return QStringLiteral("movies/collections");
    if (shows && subtab == QLatin1String("episodes"))
        return withLibrary(QStringLiteral("tvshows/episodes"));
    if (shows && subtab == QLatin1String("network") && !id.isEmpty())
        return QStringLiteral("tvshows/network:") + id;
    return {};
}
namespace {
// The library tiles of Home: Jellyfin draws the library's name into its image, which stays sharp only with the pixels of a tile on a HiDPI screen
constexpr int kLibraryImageWidth = 960;

bool isLoopbackHost(const QUrl &url)
{
    if (url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    QHostAddress address;
    return address.setAddress(url.host()) && address.isLoopback();
}

// Fixed text, never the server's: a certificate error stops the attempts instead of falling back to HTTP
QString certificateErrorText()
{
    return i18n("The server's certificate is not valid: check the certificate, the address and the "
                "port, or turn on Ignore Certificate Errors in Settings");
}

// A name from the server is one line of text: control characters, line separators, zero-width and bidirectional marks become spaces, and whitespace runs one space
QString singleLine(const QString &name)
{
    QString clean;
    clean.reserve(name.size());
    for (const QChar c : name) {
        const char16_t u = c.unicode();
        const bool control = c.category() == QChar::Other_Control || u == 0x2028 || u == 0x2029
                             || (u >= 0x200b && u <= 0x200f) || (u >= 0x202a && u <= 0x202e)
                             || (u >= 0x2066 && u <= 0x2069) || u == 0x061c || u == 0xfeff;
        clean.append(control ? QLatin1Char(' ') : c);
    }
    return clean.simplified();
}

// The libraries a tab lists: movies and shows
QString tabOf(const QString &collectionType)
{
    return collectionType == QLatin1String("movies") || collectionType == QLatin1String("tvshows")
               ? collectionType
               : QString();
}

// The tokens the browse state store keeps the feature filters under
struct StoredFeature {
    LibraryFeature feature;
    QLatin1String token;
};
constexpr StoredFeature kStoredFeatures[] = {
    {LibraryFeature::Subtitles, QLatin1String("HasSubtitles")},
    {LibraryFeature::Trailer, QLatin1String("HasTrailer")},
    {LibraryFeature::SpecialFeatures, QLatin1String("HasSpecialFeature")},
    {LibraryFeature::ThemeSong, QLatin1String("HasThemeSong")},
    {LibraryFeature::ThemeVideo, QLatin1String("HasThemeVideo")}};

QString storedEpisodeKind(EpisodeKind kind)
{
    switch (kind) {
    case EpisodeKind::All:
        break;
    case EpisodeKind::Specials:
        return QStringLiteral("Specials");
    case EpisodeKind::Missing:
        return QStringLiteral("Missing");
    case EpisodeKind::Unaired:
        return QStringLiteral("Unaired");
    }
    return QStringLiteral("All");
}

EpisodeKind episodeKindFromStored(const QString &stored)
{
    if (stored == QLatin1String("Specials"))
        return EpisodeKind::Specials;
    if (stored == QLatin1String("Missing"))
        return EpisodeKind::Missing;
    if (stored == QLatin1String("Unaired"))
        return EpisodeKind::Unaired;
    return EpisodeKind::All;
}

// The digests of the selected texts, then those of an earlier session that no listed text has named yet: each once, at most what the store keeps. The texts themselves never reach the store
QStringList storedHashes(const QStringList &texts, const QStringList &pending)
{
    QStringList hashes;
    const auto add = [&hashes](const QString &hash) {
        if (hashes.size() < BrowseStateStore::MaxHashes && !hashes.contains(hash))
            hashes.append(hash);
    };
    for (const QString &text : texts)
        add(BrowseStateStore::hashText(text));
    for (const QString &hash : pending)
        add(hash);
    return hashes;
}

BrowseStateStore::ViewState toStored(const LibraryItemsModel::ViewState &state)
{
    const LibraryFilterState &filters = state.filters;
    BrowseStateStore::ViewState stored;
    stored.sortKey = state.sortBy;
    stored.descending = state.descending;
    if (filters.played == PlayedState::Played)
        stored.statusFlags.append(QStringLiteral("IsPlayed"));
    else if (filters.played == PlayedState::Unplayed)
        stored.statusFlags.append(QStringLiteral("IsUnplayed"));
    if (filters.resumable)
        stored.statusFlags.append(QStringLiteral("IsResumable"));
    if (filters.favorites)
        stored.statusFlags.append(QStringLiteral("IsFavorite"));
    for (const StoredFeature &entry : kStoredFeatures) {
        if (filters.features.testFlag(entry.feature))
            stored.features.append(entry.token);
    }
    if (filters.quality == VideoQuality::HD)
        stored.quality = QStringLiteral("HD");
    else if (filters.quality == VideoQuality::SD)
        stored.quality = QStringLiteral("SD");
    stored.is4K = filters.is4K;
    stored.is3D = filters.is3D;
    stored.videoTypes = filters.videoTypes;
    stored.genreIds = filters.genreIds;
    stored.tagHashes = storedHashes(filters.tags, state.pendingTagHashes);
    stored.ratingHashes = storedHashes(filters.officialRatings, state.pendingRatingHashes);
    // The model takes any year of 1 to 9999, the store the plausible ones: a view must not be refused whole for one odd year
    for (const int year : filters.years) {
        if (year >= BrowseStateStore::MinYear && year <= BrowseStateStore::MaxYear
            && stored.years.size() < BrowseStateStore::MaxYears)
            stored.years.append(year);
    }
    stored.seriesStatus = filters.seriesStatus;
    stored.episodeKind = storedEpisodeKind(filters.episodeKind);
    stored.viewMode = state.viewMode;
    stored.imageType = state.imageType;
    stored.showTitle = state.showTitle;
    stored.showYear = state.showYear;
    stored.cardLayout = state.cardLayout;
    return stored;
}

// A sort key the model does not know keeps the default
LibraryItemsModel::ViewState fromStored(const BrowseStateStore::ViewState &stored)
{
    LibraryItemsModel::ViewState state;
    state.sortBy = LibraryItemsModel::sortByFromStored(stored.sortKey);
    if (stored.statusFlags.contains(QLatin1String("IsUnplayed")))
        state.filters.played = PlayedState::Unplayed;
    else if (stored.statusFlags.contains(QLatin1String("IsPlayed")))
        state.filters.played = PlayedState::Played;
    state.filters.resumable = stored.statusFlags.contains(QLatin1String("IsResumable"));
    state.filters.favorites = stored.statusFlags.contains(QLatin1String("IsFavorite"));
    for (const StoredFeature &entry : kStoredFeatures) {
        if (stored.features.contains(entry.token))
            state.filters.features |= entry.feature;
    }
    if (stored.quality == QLatin1String("HD"))
        state.filters.quality = VideoQuality::HD;
    else if (stored.quality == QLatin1String("SD"))
        state.filters.quality = VideoQuality::SD;
    state.filters.is4K = stored.is4K;
    state.filters.is3D = stored.is3D;
    state.filters.videoTypes = stored.videoTypes;
    state.descending = stored.descending;
    state.filters.genreIds = stored.genreIds;
    // The texts are not stored: the model resolves the digests when the facet lists load
    state.pendingTagHashes = stored.tagHashes;
    state.pendingRatingHashes = stored.ratingHashes;
    state.filters.years = stored.years;
    state.filters.seriesStatus = stored.seriesStatus;
    state.filters.episodeKind = episodeKindFromStored(stored.episodeKind);
    state.viewMode = stored.viewMode;
    state.imageType = stored.imageType;
    state.showTitle = stored.showTitle;
    state.showYear = stored.showYear;
    state.cardLayout = stored.cardLayout;
    return state;
}
} // namespace

ConnectionController::ConnectionController(const QString &profileId, const QString &deviceId,
                                           const QString &version, TokenStore *store,
                                           QObject *parent)
    : QObject(parent), m_ownedStore(store ? nullptr : new KeychainTokenStore), m_client(this),
      m_sync(&m_client, store ? store : m_ownedStore.get(), profileId, this),
      m_downloads(&m_client, profileId)
{
    // The sync connects to the client first, so on every shared signal it has already saved, restored or forgotten when the slots below run
    m_identityValid = m_client.setDeviceIdentity(QStringLiteral("Kinefin"), deviceId, version);
    if (!m_identityValid)
        qCWarning(controllerLog) << "the profile's device id or the version is not header-safe";
    // The queue is kept in the profile's data folder (empty for an id that is not a valid folder name: nothing is kept)
    const QString dataDirectory = profileDataDirectory(profileId);
    if (!dataDirectory.isEmpty()) {
        m_queue.setStoragePath(dataDirectory + QStringLiteral("/queue.json"));
        m_browse.setStoragePath(dataDirectory + QStringLiteral("/browse.json"));
    }
    m_remoteCommands.attach(&m_socket);
    // A library scan only wakes the rule scanner: LibraryChanged is a hint, never an item list
    connect(&m_socket, &JellyfinSocketClient::libraryChanged, this,
            [this] { m_downloads.scheduleRuleScan(); });
    // A change of user data (this device's playback, another device, a write from another session) is a hint: the ids are read again in one round a second at most, and what reads back goes to the models through the client's user data signals
    m_userDataRefresh.setSingleShot(true);
    m_userDataRefresh.setInterval(1000);
    connect(&m_socket, &JellyfinSocketClient::userDataChanged, this,
            [this](const QStringList &ids) {
                for (const QString &id : ids) {
                    if (m_userDataPending.size() >= UserDataPendingMax)
                        break;
                    m_userDataPending.insert(id.toLower().remove(QLatin1Char('-')));
                }
                if (!m_userDataHeld && !m_userDataRefresh.isActive())
                    m_userDataRefresh.start();
            });
    connect(&m_userDataRefresh, &QTimer::timeout, this, [this] {
        QStringList round;
        while (!m_userDataPending.isEmpty() && round.size() < UserDataRound) {
            const auto first = m_userDataPending.begin();
            round.append(*first);
            m_userDataPending.erase(first);
        }
        if (!round.isEmpty())
            m_client.refreshUserData(round);
        if (!m_userDataPending.isEmpty() && !m_userDataHeld)
            m_userDataRefresh.start();
    });
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_removedFilter.reset();
        m_userDataPending.clear();
        m_userDataRefresh.stop();
        m_socketLastSeenMs = 0;
        m_socketWasUp = false;
    });
    // The socket carries no replay: what changed while it was down is not told again, so a long gap asks the page in front to read its data once more (a short one is a network blip and keeps the scroll). The gap runs from the last frame the socket had, on the wall clock: the drop is noticed late (a silent one after the dead-peer window) and a suspend takes no time on a monotonic clock
    connect(&m_socket, &JellyfinSocketClient::stateChanged, this,
            [this](JellyfinSocketClient::State state) {
                switch (state) {
                case JellyfinSocketClient::State::Reconnecting:
                    if (m_socketWasUp && m_socketLastSeenMs == 0)
                        m_socketLastSeenMs = m_socket.lastActivityMs();
                    break;
                case JellyfinSocketClient::State::Authenticated: {
                    const bool missed = m_socketLastSeenMs > 0
                                        && QDateTime::currentMSecsSinceEpoch() - m_socketLastSeenMs
                                               >= m_socketGapMs;
                    m_socketLastSeenMs = 0;
                    m_socketWasUp = true;
                    if (missed)
                        Q_EMIT serverEventsMissed();
                    break;
                }
                case JellyfinSocketClient::State::Disconnected:
                    m_socketLastSeenMs = 0;
                    m_socketWasUp = false;
                    break;
                case JellyfinSocketClient::State::Connecting:
                    break;
                }
            });
    m_offlineRefresh.setSingleShot(true);
    m_offlineRefresh.setInterval(1000);
    connect(&m_offlineRefresh, &QTimer::timeout, this, [this] { refreshOfflineAvailable(); });
    connect(&m_downloads, &DownloadsController::revisionChanged, this, [this] {
        if (!m_offlineRefresh.isActive())
            m_offlineRefresh.start();
    });
    connect(this, &ConnectionController::stateChanged, this, [this] {
        refreshOfflineAvailable();
        const bool online = state() == Authenticated;
        m_downloads.setOnline(online);
        if (online) {
            m_downloads.reconcileOutbox();
            m_downloads.scanRules();
        }
    });
    // The error is accepted only while the setting is on; otherwise it is remembered, so a probe that ends in an error afterwards is told from a plain failure
    connect(&m_probeNetwork, &QNetworkAccessManager::sslErrors, this, [this](QNetworkReply *reply) {
        if (m_ignoreCertificateErrors)
            reply->ignoreSslErrors();
        else
            reply->setProperty("certificateRejected", true);
    });
    // The network coming back reconnects the socket at once instead of after its backoff (best effort: no backend, no wake-up)
    if (QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this](QNetworkInformation::Reachability reachability) {
                    if (reachability == QNetworkInformation::Reachability::Online)
                        m_socket.reconnectNow();
                });
    }
    // A session that ends keeps its browse state, written now
    connect(&m_client, &JellyfinClient::credentialsCleared, &m_browse, &BrowseStateStore::flush);
    // The models empty themselves on the same signal: nothing is left to be a one-off view
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] { m_oneOff.clear(); });
    // Before the handlers that change the state, so a page reading the user in a state binding sees the new one
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] {
        refreshCurrentUserId();
        refreshCurrentUserIsAdministrator();
    });
    connect(&m_client, &JellyfinClient::loginSucceeded, this, [this] {
        refreshCurrentUserId();
        refreshCurrentUserIsAdministrator();
    });
    connect(&m_client, &JellyfinClient::userProfileRefreshed, this,
            [this] { refreshCurrentUserIsAdministrator(); });
    connect(&m_browse, &BrowseStateStore::saveFailed, this,
            [] { qCWarning(controllerLog) << "could not save the browse state"; });
    connectViewPersistence(&m_movies);
    connectViewPersistence(&m_shows);
    connectViewPersistence(&m_library);
    // The list of a sub-tab opens straight into the view stored under its own key, and its edits are kept there
    m_subtabs.setViewStateProvider([this](const QString &) {
        const QString key = subtabViewKey();
        const auto stored = key.isEmpty()
                                ? std::nullopt
                                : m_browse.view(m_client.serverId(), m_client.userId(), key);
        if (stored)
            return fromStored(*stored);
        // Episodes start in the aired order
        return m_subtabs.currentSubtab() == LibrarySubtabController::Episodes
                   ? LibraryItemsModel::defaultViewState(LibraryScope::Episodes)
                   : LibraryItemsModel::ViewState();
    });
    connect(&m_subtabs, &LibrarySubtabController::listViewStateEdited, this, [this] {
        LibraryItemsModel *list = m_subtabs.activeList();
        const QString key = subtabViewKey();
        if (list && !key.isEmpty())
            m_browse.setView(m_client.serverId(), m_client.userId(), key,
                             toStored(list->viewState()));
    });
    connect(&m_client, &JellyfinClient::itemFavoriteUpdated, this, [this](const QString &id) {
        m_cardFavoriteRequests.remove(QString(id).remove(QLatin1Char('-')).toLower());
    });
    connect(&m_client, &JellyfinClient::itemFavoriteUpdateFailed, this, [this](const QString &id) {
        if (m_cardFavoriteRequests.remove(QString(id).remove(QLatin1Char('-')).toLower()))
            Q_EMIT favoriteUpdateFailed(id);
    });
    connect(&m_client, &JellyfinClient::itemPlayedUpdated, this,
            [this](const QString &id, bool played) {
                if (m_cardPlayedRequests.remove(QString(id).remove(QLatin1Char('-')).toLower()))
                    Q_EMIT playedUpdated(id, played);
            });
    connect(&m_client, &JellyfinClient::itemPlayedUpdateFailed, this, [this](const QString &id) {
        if (m_cardPlayedRequests.remove(QString(id).remove(QLatin1Char('-')).toLower()))
            Q_EMIT playedUpdateFailed(id);
    });
    connect(&m_client, &JellyfinClient::itemRefreshFinished, this,
            &ConnectionController::itemRefreshFinished);
    connect(&m_client, &JellyfinClient::collectionCreated, this,
            &ConnectionController::collectionCreated);
    connect(&m_client, &JellyfinClient::collectionCreateFailed, this,
            &ConnectionController::collectionCreateFailed);
    connect(&m_client, &JellyfinClient::collectionItemsAdded, this,
            &ConnectionController::collectionItemsAdded);
    connect(&m_client, &JellyfinClient::collectionItemsAddFailed, this,
            &ConnectionController::collectionItemsAddFailed);
    connect(&m_client, &JellyfinClient::itemDeleted, this, &ConnectionController::itemDeleted);
    connect(&m_client, &JellyfinClient::itemDeleteFailed, this,
            &ConnectionController::itemDeleteFailed);
    connect(&m_client, &JellyfinClient::itemUpdated, this, &ConnectionController::itemUpdated);
    connect(&m_client, &JellyfinClient::itemUpdateFailed, this,
            &ConnectionController::itemUpdateFailed);
    connect(&m_client, &JellyfinClient::remoteSearchFinished, this,
            &ConnectionController::remoteSearchFinished);
    connect(&m_client, &JellyfinClient::remoteSearchFailed, this,
            &ConnectionController::remoteSearchFailed);
    connect(&m_client, &JellyfinClient::remoteSearchApplied, this,
            &ConnectionController::remoteSearchApplied);
    connect(&m_client, &JellyfinClient::remoteSearchApplyFailed, this,
            &ConnectionController::remoteSearchApplyFailed);
    connect(&m_client, &JellyfinClient::itemImageUpdated, this,
            &ConnectionController::itemImageUpdated);
    connect(&m_client, &JellyfinClient::itemImageUpdateFailed, this,
            &ConnectionController::itemImageUpdateFailed);
    connect(&m_client, &JellyfinClient::itemSubtitleUpdated, this,
            &ConnectionController::itemSubtitleUpdated);
    connect(&m_client, &JellyfinClient::itemSubtitleUpdateFailed, this,
            &ConnectionController::itemSubtitleUpdateFailed);
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] {
        m_cardFavoriteRequests.clear();
        m_cardPlayedRequests.clear();
    });
    // The favorites are scanned over the libraries the server lists, whatever the Home preferences hide; the model drops its snapshot when the list changes, and a lost session empties the list
    connect(this, &ConnectionController::librariesChanged, this,
            [this] { m_favorites.setLibraries(m_libraries); });
    m_quickConnectTimer.setInterval(5000);
    connect(&m_quickConnectTimer, &QTimer::timeout, &m_client,
            [this] { m_client.pollQuickConnect(); });
    connect(&m_sync, &ServerCredentialSync::credentialsChanged, this, [this] {
        refreshServers();
        refreshUsers();
    });
    connect(&m_sync, &ServerCredentialSync::restoreFinished, this, [this] { onRestoreFinished(); });
    connect(&m_sync, &ServerCredentialSync::saveFailed, this, [this] {
        setError(i18n("Could not save the session. You will have to sign in again next time"));
    });
    connect(&m_client, &JellyfinClient::serverReady, this, [this] { onServerReady(); });
    connect(&m_client, &JellyfinClient::loginSucceeded, this, [this] { onLoginSucceeded(); });
    connect(&m_client, &JellyfinClient::sessionExpired, this,
            [this](const QString &userId) { onSessionExpired(userId); });
    // The record keeps the name and avatar the picker shows up to date
    connect(&m_client, &JellyfinClient::userProfileRefreshed, this,
            [this](const QString &userId, const QString &userName, const QString &imageTag) {
                m_sync.updateUser(m_client.serverId(), userId, userName, imageTag);
            });
    connect(&m_client, &JellyfinClient::publicUsersReady, this,
            [this](const QList<PublicUser> &users) {
                m_publicSource = users;
                refreshPublicUsers();
            });
    connect(&m_client, &JellyfinClient::publicUsersFailed, this, [this] {
        m_publicSource.clear();
        refreshPublicUsers();
    });
    connect(&m_client, &JellyfinClient::requestFailed, this,
            [this](int status) { onRequestFailed(status); });
    connect(&m_client, &JellyfinClient::serverCertificateRejected, this, [this] {
        if (m_state != Connecting)
            return;
        m_connectionFallback.clear();
        setError(certificateErrorText());
        setState(Idle);
    });
    connect(&m_client, &JellyfinClient::userViewsReady, this,
            [this](const QJsonArray &items) { onUserViews(items); });
    // The session's configuration is read once after sign-in for Home: a load of the preferences page that ends Ready adopts the server's data too, and a verified save projects at once
    connect(&m_userConfiguration, &UserConfigurationModel::stateChanged, this, [this] {
        if (m_state != Authenticated)
            return;
        if (m_userConfiguration.state() == UserConfigurationModel::Ready
            && m_userConfiguration.loaded()) {
            const UserPreferencesSnapshot snapshot = m_userConfiguration.snapshot();
            applyPreferences(&snapshot);
        } else if (m_userConfiguration.state() == UserConfigurationModel::Failed
                   && !m_preferencesSettled) {
            applyPreferences(nullptr);
        }
    });
    connect(&m_userConfiguration, &UserConfigurationModel::configurationApplied, this,
            [this](const UserPreferencesSnapshot &snapshot) { applyPreferences(&snapshot); });
    connect(&m_client, &JellyfinClient::credentialsCleared, this,
            [this] { resetHomeProjection(); });
    connect(this, &ConnectionController::librariesChanged, this,
            &ConnectionController::homeProjectionChanged);
    connect(&m_client, &JellyfinClient::quickConnectCodeReady, this, [this](const QString &code) {
        if (m_state != QuickConnectPending)
            return;
        m_quickConnectCode = code;
        Q_EMIT quickConnectCodeChanged();
        m_quickConnectTimer.start();
    });
    refreshServers();
}

void ConnectionController::setQuickConnectPollInterval(std::chrono::milliseconds interval)
{
    m_quickConnectTimer.setInterval(interval);
}

ConnectionController::~ConnectionController()
{
    // The handlers of the client's and the sync's signals touch members declared after the client, which are gone before the client is: none may run while the rest is torn down
    m_client.disconnect(this);
    m_sync.disconnect(this);
}

void ConnectionController::start()
{
    if (m_state != Idle || m_servers.isEmpty())
        return;
    // The most recent login is the last entry
    connectTo(m_servers.constLast().toMap().value(QStringLiteral("url")).toString());
}

// Discovery is deliberately independent of the authenticated JellyfinClient. Never
// forward passwords, cookies or the Jellyfin access token to a host being tested.
// Only well-formed HTTP(S) URLs are accepted. An address with a scheme keeps it: the fallback only moves to the other standard port of the same scheme. An address without one (host, host:port, [ipv6]:port) tries https then http
QList<QUrl> ConnectionController::connectionCandidates(const QString &address,
                                                       bool standardPortFallback)
{
    const QString text = address.trimmed();
    const bool explicitScheme = text.contains(QLatin1String("://"));
    QUrl url(explicitScheme ? text : QLatin1String("https://") + text);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || !url.userName().isEmpty() || !url.password().isEmpty() || url.hasQuery()
        || url.hasFragment() || url.port(-1) == 0 || address.size() > 2048)
        return {};
    if (!explicitScheme) {
        if (!standardPortFallback)
            return {url};
        QUrl plain(url);
        plain.setScheme(QStringLiteral("http"));
        // A typed port is tried as given on both schemes; none means the four usual endpoints, https first
        if (url.port(-1) != -1)
            return {url, plain};
        QUrl secureAlternative(url);
        secureAlternative.setPort(8920);
        QUrl plainPrimary(plain);
        plainPrimary.setPort(8096);
        return {url, secureAlternative, plainPrimary, plain};
    }
    // An address with a scheme (a saved server) is exactly that endpoint
    return {url};
}

void ConnectionController::setReachability(const QString &key, bool testing, bool reachable,
                                           const QString &message, const QUrl &resolved)
{
    const QVariantMap state{{QStringLiteral("testing"), testing},
                            {QStringLiteral("reachable"), reachable},
                            {QStringLiteral("message"), message},
                            {QStringLiteral("resolvedUrl"), resolved.toString()}};
    if (m_serverReachability.value(key).toMap() != state) {
        m_serverReachability.insert(key, state);
        Q_EMIT serverReachabilityChanged();
    }
    if (!testing)
        Q_EMIT serverTested(key, reachable, message);
}

void ConnectionController::startProbeCheck(const QString &key, const QList<QUrl> &candidates,
                                           int index, quint64 generation)
{
    if (m_probeGeneration.value(key) != generation)
        return;
    if (index >= candidates.size()) {
        m_probeReplies.remove(key);
        setReachability(
            key, false, false,
            i18n("Server not reachable: check the address, the port and the network connection"));
        return;
    }
    QUrl endpoint = candidates.at(index);
    QString path = endpoint.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    endpoint.setPath(path + QStringLiteral("/System/Info/Public"));
    QNetworkRequest request(endpoint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(4000);
    QNetworkReply *reply = m_probeNetwork.get(request);
    m_probeReplies.insert(key, reply);
    connect(
        reply, &QNetworkReply::finished, this, [this, key, candidates, index, generation, reply] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = reply->error() == QNetworkReply::NoError && status == 200
                            && reply->bytesAvailable() <= 32768;
            const QByteArray body = ok ? reply->readAll() : QByteArray();
            const bool certificateRejected = reply->error() != QNetworkReply::NoError
                                             && reply->property("certificateRejected").toBool();
            reply->deleteLater();
            if (m_probeGeneration.value(key) != generation)
                return;
            if (certificateRejected) {
                m_probeReplies.remove(key);
                setReachability(key, false, false, certificateErrorText());
                return;
            }
            if (ok) {
                const QJsonDocument json = QJsonDocument::fromJson(body);
                if (json.isObject()
                    && !json.object().value(QStringLiteral("Id")).toString().isEmpty()) {
                    m_probeReplies.remove(key);
                    // A key without a scheme was tried on several endpoints: the message says which one answered
                    QString message = i18n("Jellyfin server reachable");
                    if (!key.contains(QLatin1String("://")))
                        message = i18n("Jellyfin server reachable at %1",
                                       candidates.at(index).toString());
                    setReachability(key, false, true, message, candidates.at(index));
                    return;
                }
            }
            startProbeCheck(key, candidates, index + 1, generation);
        });
}

void ConnectionController::testServer(const QString &address, bool standardPortFallback)
{
    const QString key = address.trimmed();
    const QList<QUrl> candidates = connectionCandidates(key, standardPortFallback);
    if (key.isEmpty())
        return;
    const quint64 generation = ++m_testGeneration;
    m_probeGeneration.insert(key, generation);
    if (QNetworkReply *previous = m_probeReplies.take(key)) {
        previous->disconnect(this);
        previous->abort();
        previous->deleteLater();
    }
    if (candidates.isEmpty()) {
        setReachability(key, false, false, i18n("Enter a valid server address"));
        return;
    }
    setReachability(key, true, false, i18n("Testing connection…"));
    startProbeCheck(key, candidates, 0, generation);
}

void ConnectionController::connectToWithFallback(const QString &address)
{
    if (m_state != Idle && m_state != NeedsLogin)
        return;
    const QList<QUrl> candidates = connectionCandidates(address, true);
    if (candidates.isEmpty()) {
        setError(i18n("Enter a valid server address"));
        return;
    }
    m_connectionFallback = candidates.mid(1);
    connectTo(candidates.constFirst().toString());
    if (m_state != Connecting)
        m_connectionFallback.clear();
    m_schemeGuessed = !m_connectionFallback.isEmpty();
}

void ConnectionController::tryNextConnection()
{
    if (m_connectionFallback.isEmpty())
        return;
    const QUrl next = m_connectionFallback.takeFirst();
    m_client.disconnectFromServer();
    if (!m_client.connectToServer(next)) {
        tryNextConnection();
        return;
    }
    m_serverUrl = next.toString();
    Q_EMIT serverUrlChanged();
}

std::optional<ServerCredential> ConnectionController::offlineUser() const
{
    QList<DownloadRecord> records = m_downloads.store()->records();
    std::sort(records.begin(), records.end(),
              [](const DownloadRecord &left, const DownloadRecord &right) {
                  return left.updatedAt > right.updatedAt;
              });
    const QList<ServerCredential> saved = m_sync.credentials();
    for (const DownloadRecord &record : records) {
        if (record.state != QLatin1String("completed") && record.state != QLatin1String("exported"))
            continue;
        for (const ServerCredential &entry : saved) {
            // A user who has to type the password cannot be that user without a server
            if (entry.serverId.compare(record.serverId, Qt::CaseInsensitive) != 0
                || entry.userId.compare(record.userId, Qt::CaseInsensitive) != 0
                || !entry.remembered || entry.askPassword)
                continue;
            if (m_downloads.hasLocalFile(record))
                return entry;
        }
    }
    return std::nullopt;
}

bool ConnectionController::canWorkOffline() const
{
    return offlineUser().has_value();
}

void ConnectionController::refreshOfflineAvailable()
{
    const bool available = canWorkOffline();
    if (available == m_offlineAvailable)
        return;
    m_offlineAvailable = available;
    Q_EMIT offlineAvailableChanged();
}

bool ConnectionController::workOffline()
{
    if (m_state == Offline)
        return true;
    if (m_state == Authenticated)
        return false;
    const std::optional<ServerCredential> entry = offlineUser();
    if (!entry)
        return false;
    m_sync.cancelRestore();
    m_client.disconnectFromServer();
    m_connectionFallback.clear();
    m_schemeGuessed = false;
    clearQuickConnect();
    if (!m_libraries.isEmpty()) {
        m_libraries.clear();
        Q_EMIT librariesChanged();
    }
    setError({});
    m_serverUrl = entry->url;
    Q_EMIT serverUrlChanged();
    m_offlineUserName = entry->username;
    m_downloads.setOfflineOwner(entry->serverId, entry->userId);
    setState(Offline);
    return true;
}

void ConnectionController::connectTo(const QString &address)
{
    if (m_state != Idle && m_state != NeedsLogin)
        return;
    setError({});
    m_schemeGuessed = false;
    QString text = address.trimmed();
    if (!text.contains(QLatin1String("://")))
        text.prepend(QLatin1String("https://"));
    if (!m_identityValid) {
        setError(i18n("The device identity of this profile is invalid"));
        return;
    }
    if (!m_client.connectToServer(QUrl(text))) {
        setError(i18n("Enter a valid server address"));
        return;
    }
    // A keyring read still pending belongs to the connection that went: its answer must not land on this one
    m_sync.cancelRestore();
    clearQuickConnect();
    setLoginUserName({});
    m_serverUrl = QUrl(text).toString();
    Q_EMIT serverUrlChanged();
    setState(Connecting);
}

void ConnectionController::onServerReady()
{
    if (m_state != Connecting)
        return;
    m_connectionFallback.clear();
    const QUrl base = m_client.serverUrl();
    m_serverUrl = base.toString();
    Q_EMIT serverUrlChanged();
    m_publicSource.clear();
    refreshUsers();
    // Plain HTTP to another host asks first. A saved server (the same address and public id, for as long as one of its users is saved) skips it: the sign-in that saved it was confirmed, so this tells apart another server at the same address but not one that repeats the id (see SECURITY.md). An address whose scheme was guessed always asks, or blocking HTTPS would downgrade a saved host unnoticed
    if (base.scheme() != QLatin1String("http") || isLoopbackHost(base))
        proceedAfterProbe();
    else if (m_sync.isSavedServer() && !m_schemeGuessed)
        allowInsecureHttp();
    else
        setState(ConfirmInsecure);
}

void ConnectionController::onRestoreFinished()
{
    // A restored session already made this Authenticated; any other outcome (the keyring could not be read) goes to the sign-in of that user
    if (m_state != Connecting || m_client.isAuthenticated())
        return;
    setError(i18n("Could not read the saved session. Sign in again"));
    askPasswordOf(m_restoringUserId);
}

void ConnectionController::proceedAfterProbe()
{
    const QList<ServerCredential> saved = m_sync.restorableUsers();
    if (saved.isEmpty()) {
        setState(NeedsLogin);
    } else if (saved.size() > 1) {
        setState(ChoosingUser);
    } else if (saved.constFirst().remembered && !saved.constFirst().askPassword) {
        restoreUser(saved.constFirst().userId);
    } else {
        askPasswordOf(saved.constFirst().userId);
    }
}

void ConnectionController::restoreUser(const QString &userId)
{
    setError({});
    m_restoringUserId = userId;
    setState(Connecting);
    // A stored session being read from the keyring keeps this on Connecting until it is known whether it restores
    if (!m_sync.restoreUser(userId))
        askPasswordOf(userId);
}

void ConnectionController::askPasswordOf(const QString &userId)
{
    const auto user = savedUser(userId);
    setLoginUserName(user ? singleLine(user->username) : QString());
    setState(NeedsLogin);
}

std::optional<ServerCredential> ConnectionController::savedUser(const QString &userId) const
{
    for (const ServerCredential &entry : m_sync.restorableUsers()) {
        if (entry.userId == userId)
            return entry;
    }
    return std::nullopt;
}

void ConnectionController::confirmInsecureHttp()
{
    if (m_state != ConfirmInsecure)
        return;
    allowInsecureHttp();
}

void ConnectionController::allowInsecureHttp()
{
    m_client.setAllowInsecureHttp(true);
    // Credentials may travel now: the saved users of the server become usable
    refreshUsers();
    proceedAfterProbe();
}

void ConnectionController::login(const QString &username, const QString &password)
{
    if (m_state != NeedsLogin)
        return;
    setError({});
    setState(SigningIn);
    if (!m_client.authenticate(username, password) && m_state == SigningIn) {
        setError(i18n("Enter a username"));
        setState(NeedsLogin);
    }
}

void ConnectionController::startQuickConnect()
{
    if (m_state != NeedsLogin)
        return;
    setError({});
    setState(QuickConnectPending);
    if (!m_client.initiateQuickConnect() && m_state == QuickConnectPending) {
        setError(i18n("Quick Connect is not available"));
        setState(NeedsLogin);
    }
}

void ConnectionController::cancelQuickConnect()
{
    if (m_state != QuickConnectPending)
        return;
    m_client.cancelQuickConnect();
    clearQuickConnect();
    setState(NeedsLogin);
}

void ConnectionController::cancel()
{
    m_connectionFallback.clear();
    m_schemeGuessed = false;
    if (m_state == Idle)
        return;
    if (m_state == Offline) {
        m_offlineUserName.clear();
        setState(Idle);
        return;
    }
    if (m_state == Authenticated)
        preparePlaybackDisconnect();
    clearQuickConnect();
    m_sync.cancelRestore();
    m_client.disconnectFromServer(); // keeps the stored session: only a 401 or sign-out forgets
    if (!m_libraries.isEmpty()) {
        m_libraries.clear();
        Q_EMIT librariesChanged();
    }
    setError({});
    setState(Idle);
}

void ConnectionController::preparePlaybackDisconnect()
{
    // Connected directly from main.cpp: this queues the one Stopped report before
    // credentials, network requests and the playback queue can be destroyed.
    Q_EMIT aboutToLeaveSession();
    if (!m_client.hasPendingPlaybackReports())
        return;
    // A bounded drain: JellyfinClient serialises reports and may retry Stopped
    // once. Exclude user input so a second logout cannot reenter this sequence.
    QEventLoop wait;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setInterval(5000);
    connect(&m_client, &JellyfinClient::playbackReportsSettled, &wait, &QEventLoop::quit);
    connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
    deadline.start();
    if (m_client.hasPendingPlaybackReports())
        wait.exec(QEventLoop::ExcludeUserInputEvents);
}

void ConnectionController::leaveSession()
{
    preparePlaybackDisconnect();
    clearQuickConnect();
    m_client.forgetSession();
    // The draft and the libraries of the preferences belong to the session that ended (the model also clears on credentialsCleared, which a session without a token does not emit)
    m_userConfiguration.clear();
    resetHomeProjection();
    if (!m_libraries.isEmpty()) {
        m_libraries.clear();
        Q_EMIT librariesChanged();
    }
    setError({});
}

void ConnectionController::forgetUserData(const QString &userId)
{
    const QString serverId = m_client.serverId();
    m_queue.forgetStoredUser(serverId, userId);
    // Downloads are not removed with the user: they stay on disk, hidden by the owner filter, and come back when that user signs in to the server again
    m_sync.forgetUser(serverId, userId);
}

void ConnectionController::signOut()
{
    if (m_state == Idle)
        return;
    if (m_state == Offline) {
        m_offlineUserName.clear();
        setState(Idle);
        return;
    }
    if (m_state == Authenticated && !m_client.userId().isEmpty()) {
        removeUser(m_client.userId());
        return;
    }
    // No signed-in user, so no record to forget
    cancel();
}

void ConnectionController::removeUser(const QString &userId)
{
    if (m_state != Authenticated && m_state != ChoosingUser && m_state != NeedsLogin)
        return;
    // A signed-in user whose record could not be saved has none to find, and still signs out
    const bool signedIn = m_state == Authenticated && m_client.userId() == userId;
    if (!signedIn && !savedUser(userId))
        return;
    const QString serverId = m_client.serverId();
    forgetUserData(userId);
    // Another user's record goes while this session carries on
    if (m_state == Authenticated && !signedIn) {
        Q_EMIT storedUserForgotten(serverId, userId);
        return;
    }
    if (signedIn)
        leaveSession();
    // Once the session is left: the player keeps its place on leaving, and that has to go too
    Q_EMIT storedUserForgotten(serverId, userId);
    landAfterSessionEnded();
}

void ConnectionController::signOutUser(const QString &userId)
{
    if (m_state != Authenticated && m_state != ChoosingUser && m_state != NeedsLogin)
        return;
    const bool signedIn = m_state == Authenticated && m_client.userId() == userId;
    if (!signedIn && !savedUser(userId))
        return;
    const QString serverId = m_client.serverId();
    m_queue.forgetStoredUser(serverId, userId);
    m_sync.forgetToken(serverId, userId);
    // No session of this user to end: the player forgets what it kept of the user
    if (!signedIn) {
        Q_EMIT storedUserForgotten(serverId, userId);
        return;
    }
    leaveSession();
    // Once the session is left: the player keeps its place on leaving, and that has to go too
    Q_EMIT storedUserForgotten(serverId, userId);
    landAfterSessionEnded();
}

void ConnectionController::landAfterSessionEnded()
{
    if (m_sync.restorableUsers().isEmpty()) {
        m_client.disconnectFromServer();
        if (!m_libraries.isEmpty()) {
            m_libraries.clear();
            Q_EMIT librariesChanged();
        }
        setError({});
        setState(Idle);
    } else {
        setLoginUserName({});
        setState(ChoosingUser);
    }
}

void ConnectionController::switchUser()
{
    if (m_state != Authenticated)
        return;
    // The token and the saved queue stay: the picker lists this user again, and choosing it needs no password
    leaveSession();
    // A session that could not be saved leaves nobody to pick
    if (m_sync.restorableUsers().isEmpty()) {
        setLoginUserName({});
        setState(NeedsLogin);
        return;
    }
    setState(ChoosingUser);
}

void ConnectionController::chooseUser(const QString &userId)
{
    const bool switching = m_state == Authenticated;
    if (m_state != ChoosingUser && m_state != NeedsLogin && !switching)
        return;
    const auto user = savedUser(userId);
    if (!user)
        return;
    if (switching) {
        if (userId == m_client.userId())
            return;
        // The token and the saved queue of the user who leaves stay, as Switch User keeps them
        leaveSession();
    }
    setError({});
    if (user->remembered && !user->askPassword)
        restoreUser(userId);
    else
        askPasswordOf(userId);
}

void ConnectionController::addUser()
{
    if (m_state != Authenticated && m_state != ChoosingUser && m_state != NeedsLogin)
        return;
    leaveSession();
    setLoginUserName({});
    setState(NeedsLogin);
}

void ConnectionController::chooseAnotherUser()
{
    if (m_state != NeedsLogin || m_users.isEmpty())
        return;
    setError({});
    setLoginUserName({});
    setState(ChoosingUser);
}

void ConnectionController::setUserAskPassword(const QString &userId, bool ask)
{
    if (m_state == Idle || m_state == Connecting)
        return;
    m_sync.setAskPassword(m_client.serverId(), userId, ask);
}

void ConnectionController::loadPublicUsers()
{
    m_client.fetchPublicUsers();
}

bool ConnectionController::openAdminDashboard() const
{
    const auto link = adminDashboardUrl(m_serverUrl);
    return link && QDesktopServices::openUrl(*link);
}

void ConnectionController::removeServer(const QString &serverId)
{
    for (const QVariant &entry : std::as_const(m_servers)) {
        const auto server = entry.toMap();
        if (server.value(QStringLiteral("serverId")).toString() != serverId)
            continue;
        const QString url = server.value(QStringLiteral("url")).toString();
        m_probeGeneration.remove(url);
        if (QNetworkReply *reply = m_probeReplies.take(url)) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
        if (m_serverReachability.remove(url))
            Q_EMIT serverReachabilityChanged();
        break;
    }
    const bool active = m_state != Idle && m_client.serverId() == serverId;
    // Unlike a sign-out, forgetting a server takes the browse state of all its users with it
    m_browse.forgetServer(serverId);
    m_browse.flush();
    m_queue.forgetStored(serverId);
    m_sync.forgetServer(serverId);
    if (active) {
        if (m_state == Authenticated)
            preparePlaybackDisconnect();
        clearQuickConnect();
        m_sync.cancelRestore();
        m_client.disconnectFromServer();
        m_libraries.clear();
        Q_EMIT librariesChanged();
        setError({});
        setState(Idle);
    }
    // After the playback let go of the session, which keeps the player's place on leaving
    Q_EMIT storedServerForgotten(serverId);
    refreshServers();
}

void ConnectionController::reloadLibraries()
{
    if (m_state != Authenticated)
        return;
    clearLibraryError();
    // A configuration that could not be loaded is tried again with the libraries; a load already under way is left alone
    if (!m_hasPreferences)
        m_userConfiguration.load();
    m_client.fetchUserViews();
}

void ConnectionController::onLoginSucceeded()
{
    clearQuickConnect();
    setError({});
    refreshServers();
    setState(Authenticated);
    m_queue.restoreFor(m_client.serverId(), m_client.userId());
    // Home waits for the configuration (its answer, or its failure) before it shows tiles and loads rows
    resetHomeProjection();
    m_userConfiguration.load();
    m_client.fetchUserViews();
}

void ConnectionController::onSessionExpired(const QString &userId)
{
    clearQuickConnect();
    m_libraries.clear();
    Q_EMIT librariesChanged();
    refreshServers();
    // The sync has dropped that user's token and kept the record: the sign-in starts with its name. The client stays on the probed server, so signing in again needs no new address
    const auto user = savedUser(userId);
    setLoginUserName(user ? singleLine(user->username) : QString());
    setError(i18n("Your session has expired. Sign in again"));
    setState(NeedsLogin);
}

void ConnectionController::onRequestFailed(int status)
{
    switch (m_state) {
    case Connecting:
        // Any failed candidate moves on, as the Test probe does: a web server answering 404 on 443 is no Jellyfin server. A rejected certificate never gets here, it empties the list
        if (!m_connectionFallback.isEmpty()) {
            tryNextConnection();
            return;
        }
        m_connectionFallback.clear();
        setError(i18n(
            "Could not reach a Jellyfin server. Check the address and your network connection"));
        setState(Idle);
        break;
    case SigningIn:
        setError(status == 401 ? i18n("Wrong username or password")
                               : i18n("Signing in failed. Try again, or check the server"));
        setState(NeedsLogin);
        break;
    case QuickConnectPending:
        setError(m_quickConnectCode.isEmpty()
                     ? i18n("Quick Connect is not available on this server")
                     : i18n("Quick Connect failed or expired. Start it again"));
        clearQuickConnect();
        setState(NeedsLogin);
        break;
    case Authenticated:
        // A 401 here was already reported as an expired session
        if (m_client.isAuthenticated()) {
            setError(i18n("Could not load the libraries. Try again"));
            m_libraryError = true; // after setError(), which resets it
        }
        break;
    default:
        break;
    }
}

bool ConnectionController::openGenreRails(LibraryItemsModel *model)
{
    if ((model != &m_library && model != &m_movies && model != &m_shows) || !model->isOpen())
        return false;
    if (!m_genreRails.open(model->collectionType(), model->libraryId()))
        return false;
    m_genreRailsType = model->collectionType();
    return true;
}

bool ConnectionController::openResumeItems()
{
    return m_state == Authenticated && m_resumeItems.open();
}

bool ConnectionController::setFavorite(const QString &itemId, bool favorite)
{
    if (m_state != Authenticated || !m_client.setItemFavorite(itemId, favorite))
        return false;
    m_cardFavoriteRequests.insert(QString(itemId).remove(QLatin1Char('-')).toLower());
    return true;
}

bool ConnectionController::setPlayed(const QString &itemId, bool played)
{
    if (m_state != Authenticated || !m_client.setItemPlayed(itemId, played))
        return false;
    m_cardPlayedRequests.insert(QString(itemId).remove(QLatin1Char('-')).toLower());
    return true;
}

bool ConnectionController::undoPlayed(const QString &itemId, qint64 positionTicks, bool played)
{
    if (m_state != Authenticated)
        return false;
    // Without a resume point unwatched is all there was
    const bool sent = positionTicks > 0
                          ? m_client.restoreItemProgress(itemId, positionTicks, played)
                          : m_client.setItemPlayed(itemId, false);
    if (sent)
        m_cardPlayedRequests.insert(QString(itemId).remove(QLatin1Char('-')).toLower());
    return sent;
}

QUrl ConnectionController::streamUrl(const QString &itemId, const QString &mediaSourceId) const
{
    return m_state == Authenticated ? m_client.streamUrl(itemId, mediaSourceId) : QUrl();
}

bool ConnectionController::refreshItem(const QString &itemId)
{
    return m_state == Authenticated && m_client.refreshItem(itemId);
}

bool ConnectionController::createCollection(const QString &name, const QString &itemId)
{
    return m_state == Authenticated && isValidJellyfinId(itemId)
           && m_client.createCollection(name, QStringList{itemId});
}

bool ConnectionController::addToCollection(const QString &collectionId, const QString &itemId)
{
    return m_state == Authenticated && m_client.addToCollection(collectionId, itemId);
}

bool ConnectionController::deleteItem(const QString &itemId)
{
    return m_state == Authenticated && m_client.deleteItem(itemId);
}

bool ConnectionController::updateItem(const QString &itemId, const QVariantMap &fields)
{
    return m_state == Authenticated && m_client.updateItem(itemId, fields);
}

bool ConnectionController::remoteSearch(const QString &itemId, const QString &type,
                                        const QString &name, int year)
{
    return m_state == Authenticated && m_client.remoteSearch(itemId, type, name, year);
}

bool ConnectionController::applyRemoteSearch(const QString &itemId, const QVariantMap &result)
{
    return m_state == Authenticated && m_client.applyRemoteSearch(itemId, result);
}

bool ConnectionController::uploadImage(const QString &itemId, const QString &imageType, int index,
                                       const QUrl &fileUrl)
{
    return m_state == Authenticated && m_client.uploadImage(itemId, imageType, index, fileUrl);
}

bool ConnectionController::deleteImage(const QString &itemId, const QString &imageType, int index)
{
    return m_state == Authenticated && m_client.deleteImage(itemId, imageType, index);
}

bool ConnectionController::moveImage(const QString &itemId, const QString &imageType, int index,
                                     int newIndex)
{
    return m_state == Authenticated && m_client.moveImage(itemId, imageType, index, newIndex);
}

int ConnectionController::takeImagesChannel()
{
    if (m_nextImagesChannel >= ItemDetailsModel::FirstChannel - 1)
        m_nextImagesChannel = EditImagesModel::FirstChannel;
    return m_nextImagesChannel++;
}

EditImagesModel *ConnectionController::createEditImages(const QString &itemId, QObject *owner,
                                                        const QStringList &types)
{
    if (m_state != Authenticated || !m_client.isAdministrator() || !owner
        || !isValidJellyfinId(itemId))
        return nullptr;
    return new EditImagesModel(&m_client, itemId, takeImagesChannel(), types, owner);
}

RemoteImagesModel *ConnectionController::createRemoteImages(const QString &itemId,
                                                            const QString &imageType,
                                                            QObject *owner)
{
    if (m_state != Authenticated || !m_client.isAdministrator() || !owner
        || !isValidJellyfinId(itemId))
        return nullptr;
    return new RemoteImagesModel(&m_client, itemId, imageType, takeImagesChannel(), owner);
}

bool ConnectionController::uploadSubtitle(const QString &itemId, const QUrl &fileUrl,
                                          const QString &language, bool forced)
{
    return m_state == Authenticated && m_client.uploadSubtitle(itemId, fileUrl, language, forced);
}

bool ConnectionController::deleteSubtitle(const QString &itemId, int index)
{
    return m_state == Authenticated && m_client.deleteSubtitle(itemId, index);
}

bool ConnectionController::openFavorites()
{
    if (m_state != Authenticated)
        return false;
    // Reopening the page rescans: favorites change elsewhere, and a snapshot is never shown as current after a visit; a scan already under way is kept
    if (m_favorites.status() == FavoritesModel::Loading)
        m_favorites.load();
    else
        m_favorites.reload();
    return true;
}

bool ConnectionController::openLibrary(const QString &libraryId)
{
    // Rails belong to the library they were opened for
    m_genreRails.clear();
    for (const QVariant &entry : std::as_const(m_libraries)) {
        const QVariantMap library = entry.toMap();
        if (library.value(QStringLiteral("id")).toString() != libraryId)
            continue;
        const QString collectionType = library.value(QStringLiteral("collectionType")).toString();
        // A tile that opens a page of its own (the playlists) has no library grid
        if (!LibraryItemsModel::browseQuery(collectionType))
            return false;
        const QString name = library.value(QStringLiteral("name")).toString();
        // Only the collections are remembered here: the movie and TV libraries belong to their tabs
        const auto stored = collectionType == QLatin1String("boxsets")
                                ? m_browse.view(m_client.serverId(), m_client.userId(),
                                                scopeKey(collectionType, {}, libraryId))
                                : std::nullopt;
        return stored ? m_library.open(libraryId, collectionType, name, fromStored(*stored))
                      : m_library.open(libraryId, collectionType, name);
    }
    return false;
}

LibraryItemsModel *ConnectionController::typeModel(const QString &type)
{
    if (type == QLatin1String("movies"))
        return &m_movies;
    if (type == QLatin1String("tvshows"))
        return &m_shows;
    return nullptr;
}

QVariantList ConnectionController::typeLibraries(const QString &type) const
{
    QVariantList result;
    if (tabOf(type).isEmpty())
        return result;
    for (const QVariant &entry : m_libraries) {
        if (entry.toMap().value(QStringLiteral("tab")).toString() == type)
            result.append(entry);
    }
    return result;
}

bool ConnectionController::hasType(const QString &type) const
{
    return !typeLibraries(type).isEmpty();
}

QString ConnectionController::resolveScope(const QString &type, const QString &requested) const
{
    const QVariantList libraries = typeLibraries(type);
    if (libraries.size() < 2)
        return {};
    for (const QVariant &entry : libraries) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == requested)
            return requested;
    }
    return {};
}

QString ConnectionController::chooseScope(const QString &type, const QString &libraryId)
{
    const QString serverId = m_client.serverId();
    const QString userId = m_client.userId();
    const QString scope = resolveScope(
        type, libraryId.isEmpty() ? m_browse.selection(serverId, userId, type) : libraryId);
    // With one library there is nothing to select: the stored choice is kept for when there are two again
    if (typeLibraries(type).size() >= 2)
        m_browse.setSelection(serverId, userId, type, scope);
    return scope;
}

bool ConnectionController::openType(const QString &type, const QString &libraryId)
{
    if (m_state != Authenticated || !typeModel(type) || !hasType(type))
        return false;
    return openTypeScope(type, chooseScope(type, libraryId));
}

bool ConnectionController::openRecentlyAdded(const QString &type, const QString &libraryId)
{
    if (m_state != Authenticated || !typeModel(type) || !hasType(type))
        return false;
    LibraryItemsModel *model = typeModel(type);
    const QString scope = chooseScope(type, libraryId);
    model->setSearchTerm({});
    LibraryItemsModel::ViewState view;
    // Only the order and the filters are the one-off's: how the items are shown stays what the scope has stored
    if (const auto stored =
            m_browse.view(m_client.serverId(), m_client.userId(), scopeKey(type, {}, scope))) {
        const LibraryItemsModel::ViewState kept = fromStored(*stored);
        view.viewMode = kept.viewMode;
        view.imageType = kept.imageType;
        view.showTitle = kept.showTitle;
        view.showYear = kept.showYear;
        view.cardLayout = kept.cardLayout;
    }
    view.sortBy = QStringLiteral("DateCreated");
    view.descending = true;
    m_oneOff.remove(model);
    if (!openScopeView(type, scope, view))
        return false;
    m_oneOff.insert(model, view);
    // The one-off list is the main sub-tab's
    m_subtabs.select(LibrarySubtabController::Main);
    return true;
}

bool ConnectionController::openTypeScope(const QString &type, const QString &scope)
{
    LibraryItemsModel *model = typeModel(type);
    const bool wasOneOff = m_oneOff.remove(model) > 0;
    if (!wasOneOff && model->isOpen() && model->libraryId() == scope) {
        // The page may have been left (the Home tab) with the scope still open: the sub-tabs come back, a page already showing them changes nothing
        m_subtabs.open(type, scope, libraryNameOf(type, scope));
        return true;
    }
    return openScopeView(type, scope, std::nullopt);
}

QString ConnectionController::libraryNameOf(const QString &type, const QString &libraryId) const
{
    for (const QVariant &entry : typeLibraries(type)) {
        const QVariantMap library = entry.toMap();
        if (library.value(QStringLiteral("id")).toString() == libraryId)
            return library.value(QStringLiteral("name")).toString();
    }
    return {};
}

void ConnectionController::setSubtabsEnabled(const QString &type, const QStringList &ids)
{
    QList<LibrarySubtabController::Subtab> enabled;
    for (const QString &id : ids) {
        if (const auto subtab = LibrarySubtabController::subtabFromId(id))
            enabled.append(*subtab);
    }
    m_subtabs.setEnabled(type, enabled);
}

void ConnectionController::closeSubtabs()
{
    m_subtabs.close();
}

QString ConnectionController::subtabViewKey() const
{
    const QString type = m_subtabs.type();
    switch (m_subtabs.currentSubtab()) {
    case LibrarySubtabController::Favorites:
        return scopeKey(type, QStringLiteral("favorites"), m_subtabs.libraryId());
    case LibrarySubtabController::Collections:
        return scopeKey(type, QStringLiteral("collections"), {});
    case LibrarySubtabController::Episodes:
        return scopeKey(type, QStringLiteral("episodes"), m_subtabs.libraryId());
    case LibrarySubtabController::Networks:
        return m_subtabs.networkId().isEmpty()
                   ? QString()
                   : scopeKey(type, QStringLiteral("network"), m_subtabs.networkId());
    case LibrarySubtabController::Main:
    case LibrarySubtabController::Suggestions:
    case LibrarySubtabController::Genres:
    case LibrarySubtabController::Upcoming:
        break;
    }
    return {};
}

bool ConnectionController::openScopeView(const QString &type, const QString &scope,
                                         const std::optional<LibraryItemsModel::ViewState> &view)
{
    LibraryItemsModel *model = typeModel(type);
    // The sub-tabs of the scope that closes give their data and channels back first, the rails they drive included
    m_subtabs.close();
    // Rails belong to the library they were opened for
    if (m_genreRailsType == type)
        m_genreRails.clear();
    const QString name = libraryNameOf(type, scope);
    bool opened = false;
    if (view) {
        opened = model->open(scope, type, name, *view);
    } else {
        const auto stored =
            m_browse.view(m_client.serverId(), m_client.userId(), scopeKey(type, {}, scope));
        opened = model->open(scope, type, name,
                             stored ? fromStored(*stored) : LibraryItemsModel::ViewState());
    }
    if (opened)
        m_subtabs.open(type, scope, name);
    return opened;
}

void ConnectionController::keepTypeModelsConsistent()
{
    for (const QString &type : {QStringLiteral("movies"), QStringLiteral("tvshows")}) {
        LibraryItemsModel *model = typeModel(type);
        if (!hasType(type)) {
            m_oneOff.remove(model);
            model->clear();
            if (m_subtabs.type() == type)
                m_subtabs.close();
        } else if (model->isOpen() && model->libraryId() != resolveScope(type, model->libraryId()))
            openTypeScope(type, {});
    }
}

void ConnectionController::connectViewPersistence(LibraryItemsModel *model)
{
    connect(model, &LibraryItemsModel::viewStateEdited, this, [this, model] {
        const QString type = model->collectionType();
        if (!model->isOpen())
            return;
        // The tile-opened model remembers collections only, always per library
        const bool boxset = type == QLatin1String("boxsets") && !model->libraryId().isEmpty();
        if (model == &m_library && !boxset)
            return;
        const QString key = scopeKey(type, {}, model->libraryId());
        LibraryItemsModel::ViewState state = model->viewState();
        const auto oneOff = m_oneOff.constFind(model);
        if (oneOff != m_oneOff.constEnd()) {
            if (state.sortBy == oneOff->sortBy && state.descending == oneOff->descending) {
                // The user did not pick a sort: the one-off order is not theirs to keep, the filter and genres are
                const auto stored = m_browse.view(m_client.serverId(), m_client.userId(), key);
                const LibraryItemsModel::ViewState kept =
                    stored ? fromStored(*stored) : LibraryItemsModel::ViewState();
                state.sortBy = kept.sortBy;
                state.descending = kept.descending;
                // The same for the filters the user left alone: the one-off has none, the scope may
                if (state.filters == oneOff->filters
                    && state.pendingTagHashes == oneOff->pendingTagHashes
                    && state.pendingRatingHashes == oneOff->pendingRatingHashes) {
                    state.filters = kept.filters;
                    state.pendingTagHashes = kept.pendingTagHashes;
                    state.pendingRatingHashes = kept.pendingRatingHashes;
                }
            } else {
                m_oneOff.remove(model);
            }
        }
        m_browse.setView(m_client.serverId(), m_client.userId(), key, toStored(state));
    });
}

void ConnectionController::clearLibraryError()
{
    // Only the libraries' own error: a failed token save, say, stays until the user leaves
    if (m_libraryError) {
        m_libraryError = false;
        setError({});
    }
}

void ConnectionController::onUserViews(const QJsonArray &items)
{
    clearLibraryError();
    m_libraries.clear();
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QString id = item.value(QStringLiteral("Id")).toString();
        const QString collectionType = item.value(QStringLiteral("CollectionType")).toString();
        const QJsonValue tags = item.value(QStringLiteral("ImageTags"));
        const QString tag = tags.isObject()
                                ? tags.toObject().value(QStringLiteral("Primary")).toString()
                                : QString();
        m_libraries.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("tab"), tabOf(collectionType)},
            {QStringLiteral("name"), singleLine(item.value(QStringLiteral("Name")).toString())},
            {QStringLiteral("collectionType"), collectionType},
            {QStringLiteral("imageUrl"),
             tag.isEmpty()
                 ? QString()
                 : m_client.imageUrl(id, QStringLiteral("Primary"), kLibraryImageWidth, tag)
                       .toString(QUrl::FullyEncoded)},
            // The server's Playlists view opens the Playlists page
            {QStringLiteral("browsable"), LibraryItemsModel::browseQuery(collectionType).has_value()
                                              || collectionType == QLatin1String("playlists")}});
    }
    keepTypeModelsConsistent();
    Q_EMIT librariesChanged();
    // Every library the server lists, music included: the preferences name any of them
    QList<LibraryOption> options;
    for (const QVariant &library : std::as_const(m_libraries)) {
        const QVariantMap entry = library.toMap();
        options.append({entry.value(QStringLiteral("id")).toString(),
                        entry.value(QStringLiteral("name")).toString()});
    }
    m_userConfiguration.setLibraries(options);
    m_librariesLoaded = true;
    Q_EMIT homeProjectionChanged();
    // Without the configuration's answer the rows wait for it (applyPreferences loads them then)
    if (m_preferencesSettled)
        m_home.load(recentLibraries(), hidePlayedInLatest());
}

ItemDetailsModel *ConnectionController::createItemDetails(const QString &itemId, QObject *owner)
{
    if (m_state != Authenticated || !owner || !isValidJellyfinId(itemId))
        return nullptr;
    const int channel = m_nextDetailsChannel;
    m_nextDetailsChannel += ItemDetailsModel::ChannelsPerModel;
    return new ItemDetailsModel(&m_client, itemId, channel, owner);
}

void ConnectionController::holdUserDataRefresh(bool hold)
{
    if (hold == m_userDataHeld)
        return;
    m_userDataHeld = hold;
    if (hold)
        m_userDataRefresh.stop();
    else if (!m_userDataPending.isEmpty())
        m_userDataRefresh.start();
}

QVariantList ConnectionController::savedFilters(const QString &kind) const
{
    QVariantList names;
    for (const auto &saved : m_browse.savedFilters(m_client.serverId(), m_client.userId(), kind))
        names.append(QVariantMap{{QStringLiteral("name"), saved.name}});
    return names;
}

QString ConnectionController::saveFilters(const QString &kind, const QString &name,
                                          const QVariantMap &filters, bool replace)
{
    LibraryItemsModel::ViewState view;
    view.filters = LibraryItemsModel::filtersFromVariant(filters);
    const auto result = m_browse.saveFilter(m_client.serverId(), m_client.userId(), kind,
                                            name.trimmed(), toStored(view), replace);
    using Result = BrowseStateStore::SaveResult;
    if (result == Result::Saved || result == Result::Replaced) {
        ++m_savedFiltersRevision;
        Q_EMIT savedFiltersChanged();
    }
    switch (result) {
    case Result::Saved:
        return QStringLiteral("saved");
    case Result::Replaced:
        return QStringLiteral("replaced");
    case Result::Exists:
        return QStringLiteral("exists");
    case Result::Full:
        return QStringLiteral("full");
    case Result::Invalid:
        break;
    }
    return QStringLiteral("invalid");
}

bool ConnectionController::applySavedFilters(LibraryItemsModel *list, const QString &name)
{
    if (!list || !list->isOpen())
        return false;
    const auto stored =
        m_browse.savedFilter(m_client.serverId(), m_client.userId(), list->filterKind(), name);
    if (!stored)
        return false;
    const LibraryItemsModel::ViewState view = fromStored(*stored);
    list->applyFilterSet(view.filters, view.pendingTagHashes, view.pendingRatingHashes);
    return true;
}

QString ConnectionController::renameSavedFilters(const QString &kind, const QString &name,
                                                 const QString &newName)
{
    const auto result = m_browse.renameSavedFilter(m_client.serverId(), m_client.userId(), kind,
                                                   name, newName.trimmed());
    using Result = BrowseStateStore::RenameResult;
    if (result == Result::Renamed) {
        ++m_savedFiltersRevision;
        Q_EMIT savedFiltersChanged();
        return QStringLiteral("renamed");
    }
    switch (result) {
    case Result::Missing:
        return QStringLiteral("missing");
    case Result::Exists:
        return QStringLiteral("exists");
    default:
        break;
    }
    return QStringLiteral("invalid");
}

bool ConnectionController::removeSavedFilters(const QString &kind, const QString &name)
{
    const auto saved = m_browse.savedFilters(m_client.serverId(), m_client.userId(), kind);
    const auto found = std::find_if(saved.cbegin(), saved.cend(), [&name](const auto &entry) {
        return entry.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    if (found == saved.cend()
        || !m_browse.removeSavedFilter(m_client.serverId(), m_client.userId(), kind, found->name))
        return false;
    m_removedFilter = *found;
    m_removedFilterOwner = m_client.serverId() + QLatin1Char('/') + m_client.userId();
    m_removedFilterAge.start();
    ++m_savedFiltersRevision;
    Q_EMIT savedFiltersChanged();
    return true;
}

bool ConnectionController::undoRemoveSavedFilters()
{
    // An undo is for the moment of the notification: a set removed long ago is not brought back by a key
    if (!m_removedFilter || !m_removedFilterAge.isValid() || m_removedFilterAge.hasExpired(15000))
        return false;
    const auto removed = std::exchange(m_removedFilter, std::nullopt);
    if (std::exchange(m_removedFilterOwner, QString())
        != m_client.serverId() + QLatin1Char('/') + m_client.userId())
        return false;
    const auto result = m_browse.saveFilter(m_client.serverId(), m_client.userId(), removed->kind,
                                            removed->name, removed->state, false);
    if (result != BrowseStateStore::SaveResult::Saved)
        return false;
    ++m_savedFiltersRevision;
    Q_EMIT savedFiltersChanged();
    return true;
}

void ConnectionController::reloadHome()
{
    if (m_state != Authenticated)
        return;
    m_home.reload();
}

namespace {
// The server writes ids in one case, but the preference lists and the library list are compared the way the preferences editor does: case-insensitively
QString libraryKey(const QVariant &library)
{
    return library.toMap().value(QStringLiteral("id")).toString().toLower();
}

QSet<QString> keySet(const QStringList &ids)
{
    QSet<QString> keys;
    for (const QString &id : ids)
        keys.insert(id.toLower());
    return keys;
}
} // namespace

QVariantList ConnectionController::homeLibraries() const
{
    if (!m_preferencesSettled)
        return {};
    QVariantList tiles = m_libraries;
    if (!m_hasPreferences)
        return tiles;
    // A library the order does not name goes after the named ones, in server order; an id of the order that is no library is just never found
    QHash<QString, int> rank;
    for (const QString &id : std::as_const(m_preferences.orderedViews)) {
        if (!rank.contains(id.toLower()))
            rank.insert(id.toLower(), int(rank.size()));
    }
    const int unlisted = int(rank.size());
    std::stable_sort(tiles.begin(), tiles.end(), [&](const QVariant &a, const QVariant &b) {
        return rank.value(libraryKey(a), unlisted) < rank.value(libraryKey(b), unlisted);
    });
    const QSet<QString> hidden = keySet(m_preferences.myMediaExcludes);
    tiles.removeIf([&](const QVariant &library) { return hidden.contains(libraryKey(library)); });
    return tiles;
}

QVariantList ConnectionController::recentLibraries() const
{
    if (!m_preferencesSettled)
        return {};
    QVariantList recent = m_libraries;
    if (m_hasPreferences) {
        // jellyfin-web builds these rows from /UserViews, which already drops MyMediaExcludes, so a library hidden from Home loses its row too
        QSet<QString> hidden = keySet(m_preferences.myMediaExcludes);
        hidden.unite(keySet(m_preferences.latestItemsExcludes));
        recent.removeIf(
            [&](const QVariant &library) { return hidden.contains(libraryKey(library)); });
    }
    return recent;
}

void ConnectionController::applyPreferences(const UserPreferencesSnapshot *snapshot)
{
    const bool wasSettled = m_preferencesSettled;
    const QVariantList recentBefore = recentLibraries();
    const bool unplayedBefore = hidePlayedInLatest();
    if (snapshot) {
        m_preferences = *snapshot;
        m_hasPreferences = true;
        // A late answer of a session that is gone must not leave its option to the next one
        if (m_state == Authenticated)
            m_client.setDisplayMissingEpisodes(snapshot->displayMissingEpisodes);
    }
    m_preferencesSettled = true;
    Q_EMIT homeProjectionChanged();
    // Only what the rows ask for rebuilds them: a new tile order, say, leaves them alone
    if (m_librariesLoaded
        && (!wasSettled || recentBefore != recentLibraries()
            || unplayedBefore != hidePlayedInLatest()))
        m_home.load(recentLibraries(), hidePlayedInLatest());
}

void ConnectionController::resetHomeProjection()
{
    const bool changed = m_hasPreferences || m_preferencesSettled || m_librariesLoaded;
    m_preferences = {};
    m_hasPreferences = false;
    m_preferencesSettled = false;
    m_librariesLoaded = false;
    if (changed)
        Q_EMIT homeProjectionChanged();
}

void ConnectionController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    if (state != Offline) {
        m_offlineUserName.clear();
        m_downloads.setOfflineOwner({}, {});
    }
    if (state == Idle || state == Connecting)
        m_publicSource.clear();
    if (state == Idle || state == Authenticated)
        setLoginUserName({});
    // The origin first: the avatars bind to the users the moment they change, and a request made before the origin is set is refused
    updateImageOrigin();
    refreshUsers();
    Q_EMIT stateChanged();
}

void ConnectionController::setImageOrigin(std::shared_ptr<ImageOrigin> origin)
{
    m_imageOrigin = std::move(origin);
    if (m_imageOrigin)
        m_imageOrigin->setIgnoreCertificateErrors(m_ignoreCertificateErrors);
    m_downloads.setImageOrigin(m_imageOrigin);
    updateImageOrigin();
}

void ConnectionController::setIgnoreCertificateErrors(bool ignore)
{
    // As in the client: no probe connection accepted with an invalid certificate outlives the option. mpv does not go through here: PlayerSettings follows the same ClientSettings value on its own
    if (m_ignoreCertificateErrors && !ignore)
        m_probeNetwork.clearConnectionCache();
    m_ignoreCertificateErrors = ignore;
    m_client.setIgnoreCertificateErrors(ignore);
    m_downloads.setIgnoreCertificateErrors(ignore);
    if (m_imageOrigin)
        m_imageOrigin->setIgnoreCertificateErrors(ignore);
}

bool ConnectionController::imagesAllowed() const
{
    return m_state == Authenticated || m_state == ChoosingUser || m_state == NeedsLogin;
}

QUrl ConnectionController::avatarUrl(const QString &userId, const QString &tag) const
{
    // Without an origin a request is refused: the pages show the initials until the state allows images
    return tag.isEmpty() || !imagesAllowed() ? QUrl() : m_client.userImageUrl(userId, tag);
}

void ConnectionController::updateImageOrigin()
{
    if (!m_imageOrigin)
        return;
    if (imagesAllowed())
        m_imageOrigin->set(m_client.serverUrl());
    else
        m_imageOrigin->clear();
}

void ConnectionController::setError(const QString &text)
{
    m_libraryError = false;
    if (m_errorText == text)
        return;
    m_errorText = text;
    Q_EMIT errorTextChanged();
}

void ConnectionController::clearQuickConnect()
{
    m_quickConnectTimer.stop();
    if (m_quickConnectCode.isEmpty())
        return;
    m_quickConnectCode.clear();
    Q_EMIT quickConnectCodeChanged();
}

void ConnectionController::refreshServers()
{
    // One entry per server, in order of use: the place of its most recently used user
    QStringList seen;
    QVariantList newestFirst;
    const QList<ServerCredential> credentials = m_sync.credentials();
    for (auto it = credentials.crbegin(); it != credentials.crend(); ++it) {
        if (seen.contains(it->serverId))
            continue;
        seen.append(it->serverId);
        newestFirst.append(QVariantMap{{QStringLiteral("serverId"), it->serverId},
                                       {QStringLiteral("url"), it->url}});
    }
    m_servers.clear();
    for (auto it = newestFirst.crbegin(); it != newestFirst.crend(); ++it)
        m_servers.append(*it);
    Q_EMIT serversChanged();
    refreshOfflineAvailable();
}

void ConnectionController::refreshUsers()
{
    QVariantList users;
    const QList<ServerCredential> saved = m_sync.restorableUsers();
    for (auto it = saved.crbegin(); it != saved.crend(); ++it) {
        users.append(QVariantMap{{QStringLiteral("id"), it->userId},
                                 {QStringLiteral("name"), singleLine(it->username)},
                                 {QStringLiteral("imageUrl"), avatarUrl(it->userId, it->imageTag)},
                                 {QStringLiteral("remembered"), it->remembered},
                                 {QStringLiteral("askPassword"), it->askPassword}});
    }
    if (users != m_users) {
        m_users = users;
        Q_EMIT usersChanged();
    }
    refreshPublicUsers();
}

void ConnectionController::refreshPublicUsers()
{
    QVariantList users;
    for (const PublicUser &user : std::as_const(m_publicSource)) {
        const bool saved =
            std::any_of(m_users.cbegin(), m_users.cend(), [&](const QVariant &entry) {
                const QString id = entry.toMap().value(QStringLiteral("id")).toString();
                return id.compare(user.id, Qt::CaseInsensitive) == 0;
            });
        if (saved)
            continue;
        const QUrl avatar = avatarUrl(user.id, user.imageTag);
        users.append(QVariantMap{{QStringLiteral("id"), user.id},
                                 {QStringLiteral("name"), singleLine(user.name)},
                                 {QStringLiteral("imageUrl"), avatar}});
    }
    if (users != m_publicUsers) {
        m_publicUsers = users;
        Q_EMIT publicUsersChanged();
    }
}

void ConnectionController::refreshCurrentUserId()
{
    const QString userId = m_client.userId();
    if (userId == m_currentUserId)
        return;
    m_currentUserId = userId;
    Q_EMIT currentUserIdChanged();
}

void ConnectionController::refreshCurrentUserIsAdministrator()
{
    const bool administrator = m_client.isAdministrator();
    if (administrator == m_currentUserIsAdministrator)
        return;
    m_currentUserIsAdministrator = administrator;
    Q_EMIT currentUserIsAdministratorChanged();
}

void ConnectionController::setLoginUserName(const QString &name)
{
    if (m_loginUserName == name)
        return;
    m_loginUserName = name;
    Q_EMIT loginUserNameChanged();
}
} // namespace Kinefin
