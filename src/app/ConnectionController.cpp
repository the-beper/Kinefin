// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConnectionController.h"

#include "api/MediaSourceSelection.h"

#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QUrl>
#include <KLocalizedString>

#include "integration/LinkRouter.h"
#include "settings/KWalletTokenStore.h"

namespace Kinefin {
Q_LOGGING_CATEGORY(controllerLog, "kinefin.app.connection")
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
    m_quickConnectTimer.setInterval(5000);
    connect(&m_quickConnectTimer, &QTimer::timeout, &m_client,
            [this] { m_client.pollQuickConnect(); });
    connect(&m_sync, &ServerCredentialSync::credentialsChanged, this, [this] { refreshServers(); });
    connect(&m_sync, &ServerCredentialSync::restoreFinished, this, [this] { onRestoreFinished(); });
    connect(&m_sync, &ServerCredentialSync::saveFailed, this, [this] {
        setError(i18n("Could not save the session in KWallet. You will have to sign in again next "
                      "time"));
    });
    connect(&m_client, &JellyfinClient::serverReady, this, [this] { onServerReady(); });
    connect(&m_client, &JellyfinClient::loginSucceeded, this, [this] { onLoginSucceeded(); });
    connect(&m_client, &JellyfinClient::sessionExpired, this, [this] { onSessionExpired(); });
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
    clearQuickConnect();
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
    // A stored session being read from the wallet keeps this on Connecting until it is known whether it restores
    if (m_client.isAuthenticated() || m_sync.restorePending())
        return;
    proceedAfterProbe();
}

void ConnectionController::onRestoreFinished()
{
    // A restored session already made this Authenticated; any other outcome goes on to sign-in
    if (m_state == Connecting && !m_client.isAuthenticated())
        proceedAfterProbe();
}

void ConnectionController::proceedAfterProbe()
{
    const QUrl base = m_client.serverUrl();
    if (base.scheme() == QLatin1String("http") && !isLoopbackHost(base))
        setState(ConfirmInsecure);
    else
        setState(NeedsLogin);
}

void ConnectionController::confirmInsecureHttp()
{
    if (m_state != ConfirmInsecure)
        return;
    m_client.setAllowInsecureHttp(true);
    setState(NeedsLogin);
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
    m_client.disconnectFromServer(); // keeps the stored session: only a 401 or sign-out forgets
    if (!m_libraries.isEmpty()) {
        m_libraries.clear();
        Q_EMIT librariesChanged();
    }
    setError({});
    setState(Idle);
}

void ConnectionController::signOut()
{
    if (m_state == Idle)
        return;
    clearQuickConnect();
    m_sync.signOut();
    m_libraries.clear();
    Q_EMIT librariesChanged();
    refreshServers();
    setError({});
    setState(Idle);
}

bool ConnectionController::openServerWebInterface() const
{
    const auto link = parseWebLink(m_serverUrl);
    return link && QDesktopServices::openUrl(*link);
}

void ConnectionController::removeServer(const QString &serverId)
{
    if (m_state != Idle && m_client.serverId() == serverId) {
        signOut();
        return;
    }
    m_sync.forgetServer(serverId);
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
    m_client.fetchUserViews();
}

void ConnectionController::onSessionExpired()
{
    clearQuickConnect();
    m_libraries.clear();
    Q_EMIT librariesChanged();
    refreshServers();
    // The client stays on the probed server, so signing in again needs no new address
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

bool ConnectionController::openGenreRails()
{
    if (m_library.libraryId().isEmpty())
        return false;
    return m_genreRails.open(m_library.libraryId(), m_library.collectionType());
}

bool ConnectionController::openLibrary(const QString &libraryId)
{
    // Rails belong to the library they were opened for
    m_genreRails.clear();
    for (const QVariant &entry : std::as_const(m_libraries)) {
        const QVariantMap library = entry.toMap();
        if (library.value(QStringLiteral("id")).toString() != libraryId)
            continue;
        return library.value(QStringLiteral("browsable")).toBool()
               && m_library.open(libraryId,
                                 library.value(QStringLiteral("collectionType")).toString(),
                                 library.value(QStringLiteral("name")).toString());
    }
    return false;
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
    updateImageOrigin();
    Q_EMIT stateChanged();
}

void ConnectionController::setImageOrigin(std::shared_ptr<ImageOrigin> origin)
{
    m_imageOrigin = std::move(origin);
    updateImageOrigin();
}

void ConnectionController::updateImageOrigin()
{
    if (!m_imageOrigin)
        return;
    if (m_state == Authenticated)
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
    m_servers.clear();
    for (const ServerCredential &entry : m_sync.credentials()) {
        m_servers.append(QVariantMap{{QStringLiteral("serverId"), entry.serverId},
                                     {QStringLiteral("url"), entry.url}});
    }
    Q_EMIT serversChanged();
}
} // namespace Kinefin
