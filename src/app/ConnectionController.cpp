// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConnectionController.h"

#include "api/MediaSourceSelection.h"

#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMetaEnum>
#include <QStringList>
#include <QUrl>
#include <KLocalizedString>

#include <algorithm>

#include "integration/LinkRouter.h"
#include "settings/KWalletTokenStore.h"
#include "settings/ProfilePaths.h"

namespace Kinefin {
Q_LOGGING_CATEGORY(controllerLog, "kinefin.app.connection")
// The store drops a view with more genre ids than it accepts: it must hold what the model sends
static_assert(BrowseStateStore::MaxGenreIds >= MaxGenreIds);
namespace {
bool isLoopbackHost(const QUrl &url)
{
    if (url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;
    QHostAddress address;
    return address.setAddress(url.host()) && address.isLoopback();
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

// The libraries a tab lists: movies and TV series
QString tabOf(const QString &collectionType)
{
    return collectionType == QLatin1String("movies") || collectionType == QLatin1String("tvshows")
               ? collectionType
               : QString();
}

// The key the browse state store keeps a scope's view under: the type alone for all libraries, else with the library id (lower case, no dashes)
QString scopeKey(const QString &type, const QString &libraryId)
{
    if (libraryId.isEmpty())
        return type;
    QString id = libraryId.toLower();
    id.remove(QLatin1Char('-'));
    return type + QLatin1Char(':') + id;
}

BrowseStateStore::ViewState toStored(const LibraryItemsModel::ViewState &state)
{
    return {QString::fromLatin1(
                QMetaEnum::fromType<LibraryItemsModel::SortKey>().valueToKey(state.sort)),
            state.descending,
            QString::fromLatin1(
                QMetaEnum::fromType<LibraryItemsModel::Filter>().valueToKey(state.filter)),
            state.genreIds};
}

// A key name the enum does not know keeps the default
LibraryItemsModel::ViewState fromStored(const BrowseStateStore::ViewState &stored)
{
    LibraryItemsModel::ViewState state;
    bool ok = false;
    const int sort = QMetaEnum::fromType<LibraryItemsModel::SortKey>().keyToValue(
        stored.sort.toLatin1().constData(), &ok);
    if (ok)
        state.sort = LibraryItemsModel::SortKey(sort);
    const int filter = QMetaEnum::fromType<LibraryItemsModel::Filter>().keyToValue(
        stored.filter.toLatin1().constData(), &ok);
    if (ok)
        state.filter = LibraryItemsModel::Filter(filter);
    state.descending = stored.descending;
    state.genreIds = stored.genreIds;
    return state;
}
} // namespace

ConnectionController::ConnectionController(const QString &profileId, const QString &deviceId,
                                           const QString &version, TokenStore *store,
                                           QObject *parent)
    : QObject(parent), m_ownedStore(store ? nullptr : new KWalletTokenStore), m_client(this),
      m_sync(&m_client, store ? store : m_ownedStore.get(), profileId, this)
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
    // A session that ends keeps its browse state, written now
    connect(&m_client, &JellyfinClient::credentialsCleared, &m_browse, &BrowseStateStore::flush);
    // The models empty themselves on the same signal: nothing is left to be a one-off view
    connect(&m_client, &JellyfinClient::credentialsCleared, this, [this] { m_oneOff.clear(); });
    // Before the handlers that change the state, so a page reading the user in a state binding sees the new one
    connect(&m_client, &JellyfinClient::credentialsCleared, this,
            [this] { refreshCurrentUserId(); });
    connect(&m_client, &JellyfinClient::loginSucceeded, this, [this] { refreshCurrentUserId(); });
    connect(&m_browse, &BrowseStateStore::saveFailed, this,
            [] { qCWarning(controllerLog) << "could not save the browse state"; });
    connectViewPersistence(&m_movies);
    connectViewPersistence(&m_shows);
    connectViewPersistence(&m_library);
    m_quickConnectTimer.setInterval(5000);
    connect(&m_quickConnectTimer, &QTimer::timeout, &m_client,
            [this] { m_client.pollQuickConnect(); });
    connect(&m_sync, &ServerCredentialSync::credentialsChanged, this, [this] {
        refreshServers();
        refreshUsers();
    });
    connect(&m_sync, &ServerCredentialSync::restoreFinished, this, [this] { onRestoreFinished(); });
    connect(&m_sync, &ServerCredentialSync::saveFailed, this, [this] {
        setError(i18n("Could not save the session in KWallet. You will have to sign in again next "
                      "time"));
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
    connect(&m_client, &JellyfinClient::userViewsReady, this,
            [this](const QJsonArray &items) { onUserViews(items); });
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

void ConnectionController::connectTo(const QString &address)
{
    if (m_state != Idle && m_state != NeedsLogin)
        return;
    setError({});
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
    // A wallet read still pending belongs to the connection that went: its answer must not land on this one
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
    const QUrl base = m_client.serverUrl();
    m_serverUrl = base.toString();
    Q_EMIT serverUrlChanged();
    m_publicSource.clear();
    refreshUsers();
    if (base.scheme() == QLatin1String("http") && !isLoopbackHost(base))
        setState(ConfirmInsecure);
    else
        proceedAfterProbe();
}

void ConnectionController::onRestoreFinished()
{
    // A restored session already made this Authenticated; any other outcome (the wallet could not be read) goes to the sign-in of that user
    if (m_state != Connecting || m_client.isAuthenticated())
        return;
    setError(i18n("Could not read the saved session. Enter the password"));
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
    // A stored session being read from the wallet keeps this on Connecting until it is known whether it restores
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
        setError(i18n("Enter a username and a password"));
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
    if (m_state == Idle)
        return;
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

void ConnectionController::leaveSession()
{
    clearQuickConnect();
    m_client.forgetSession();
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
    m_sync.forgetUser(serverId, userId);
}

void ConnectionController::signOut()
{
    if (m_state == Idle)
        return;
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
    forgetUserData(userId);
    // Another user's record goes while this session carries on
    if (m_state == Authenticated && !signedIn)
        return;
    if (signedIn)
        leaveSession();
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
    if (m_state != ChoosingUser && m_state != NeedsLogin)
        return;
    const auto user = savedUser(userId);
    if (!user)
        return;
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
    const bool active = m_state != Idle && m_client.serverId() == serverId;
    // Unlike a sign-out, forgetting a server takes the browse state of all its users with it
    m_browse.forgetServer(serverId);
    m_browse.flush();
    m_queue.forgetStored(serverId);
    m_sync.forgetServer(serverId);
    if (active) {
        clearQuickConnect();
        m_sync.cancelRestore();
        m_client.disconnectFromServer();
        m_libraries.clear();
        Q_EMIT librariesChanged();
        setError({});
        setState(Idle);
    }
    refreshServers();
}

void ConnectionController::reloadLibraries()
{
    if (m_state != Authenticated)
        return;
    clearLibraryError();
    m_client.fetchUserViews();
}

void ConnectionController::onLoginSucceeded()
{
    clearQuickConnect();
    setError({});
    refreshServers();
    setState(Authenticated);
    m_queue.restoreFor(m_client.serverId(), m_client.userId());
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

bool ConnectionController::openLibrary(const QString &libraryId)
{
    // Rails belong to the library they were opened for
    m_genreRails.clear();
    for (const QVariant &entry : std::as_const(m_libraries)) {
        const QVariantMap library = entry.toMap();
        if (library.value(QStringLiteral("id")).toString() != libraryId)
            continue;
        if (!library.value(QStringLiteral("browsable")).toBool())
            return false;
        const QString collectionType = library.value(QStringLiteral("collectionType")).toString();
        const QString name = library.value(QStringLiteral("name")).toString();
        // Only the collections are remembered here: the movie and TV libraries belong to their tabs
        const auto stored = collectionType == QLatin1String("boxsets")
                                ? m_browse.view(m_client.serverId(), m_client.userId(),
                                                scopeKey(collectionType, libraryId))
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
    const LibraryItemsModel::ViewState view{
        LibraryItemsModel::DateAdded, true, LibraryItemsModel::All, {}};
    m_oneOff.remove(model);
    if (!openScopeView(type, scope, view))
        return false;
    m_oneOff.insert(model, view);
    return true;
}

bool ConnectionController::openTypeScope(const QString &type, const QString &scope)
{
    LibraryItemsModel *model = typeModel(type);
    const bool wasOneOff = m_oneOff.remove(model) > 0;
    if (!wasOneOff && model->isOpen() && model->libraryId() == scope)
        return true;
    return openScopeView(type, scope, std::nullopt);
}

bool ConnectionController::openScopeView(const QString &type, const QString &scope,
                                         const std::optional<LibraryItemsModel::ViewState> &view)
{
    LibraryItemsModel *model = typeModel(type);
    // Rails belong to the library they were opened for
    if (m_genreRailsType == type)
        m_genreRails.clear();
    QString name;
    for (const QVariant &entry : typeLibraries(type)) {
        const QVariantMap library = entry.toMap();
        if (library.value(QStringLiteral("id")).toString() == scope)
            name = library.value(QStringLiteral("name")).toString();
    }
    if (view)
        return model->open(scope, type, name, *view);
    const auto stored =
        m_browse.view(m_client.serverId(), m_client.userId(), scopeKey(type, scope));
    return model->open(scope, type, name,
                       stored ? fromStored(*stored) : LibraryItemsModel::ViewState());
}

void ConnectionController::keepTypeModelsConsistent()
{
    for (const QString &type : {QStringLiteral("movies"), QStringLiteral("tvshows")}) {
        LibraryItemsModel *model = typeModel(type);
        if (!hasType(type)) {
            m_oneOff.remove(model);
            model->clear();
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
        const QString key = scopeKey(type, model->libraryId());
        LibraryItemsModel::ViewState state = model->viewState();
        const auto oneOff = m_oneOff.constFind(model);
        if (oneOff != m_oneOff.constEnd()) {
            if (state.sort == oneOff->sort && state.descending == oneOff->descending) {
                // The user did not pick a sort: the one-off order is not theirs to keep, the filter and genres are
                const auto stored = m_browse.view(m_client.serverId(), m_client.userId(), key);
                const LibraryItemsModel::ViewState kept =
                    stored ? fromStored(*stored) : LibraryItemsModel::ViewState();
                state.sort = kept.sort;
                state.descending = kept.descending;
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
                 : m_client
                       .imageUrl(id, QStringLiteral("Primary"), LibraryItemsModel::ImageWidth, tag)
                       .toString(QUrl::FullyEncoded)},
            {QStringLiteral("browsable"),
             LibraryItemsModel::browseQuery(collectionType).has_value()}});
    }
    keepTypeModelsConsistent();
    Q_EMIT librariesChanged();
    m_home.load(m_libraries);
}

ItemDetailsModel *ConnectionController::createItemDetails(const QString &itemId, QObject *owner)
{
    if (m_state != Authenticated || !owner || !isValidJellyfinId(itemId))
        return nullptr;
    const int channel = m_nextDetailsChannel;
    m_nextDetailsChannel += ItemDetailsModel::ChannelsPerModel;
    return new ItemDetailsModel(&m_client, itemId, channel, owner);
}

void ConnectionController::reloadHome()
{
    if (m_state != Authenticated)
        return;
    m_home.reload();
}

void ConnectionController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
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
    updateImageOrigin();
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

void ConnectionController::setLoginUserName(const QString &name)
{
    if (m_loginUserName == name)
        return;
    m_loginUserName = name;
    Q_EMIT loginUserNameChanged();
}
} // namespace Kinefin
