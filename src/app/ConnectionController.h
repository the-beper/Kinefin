// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <optional>

#include "api/JellyfinClient.h"
#include "imagecache/ImageOrigin.h"
#include "models/FavoritesGroupsModel.h"
#include "models/FavoritesModel.h"
#include "models/GenreRailsModel.h"
#include "models/HomeRowsModel.h"
#include "models/ItemDetailsModel.h"
#include "models/LibraryItemsModel.h"
#include "models/LibrarySubtabController.h"
#include "models/PlayQueue.h"
#include "models/ResumeItemsModel.h"
#include "models/SearchModel.h"
#include "models/UserConfigurationModel.h"
#include "settings/BrowseStateStore.h"
#include "settings/ServerCredentialSync.h"
#include "settings/TokenStore.h"

namespace Kinefin {
// What the Servers, sign-in and Home pages render: one active server at a time, the stored servers of the profile, and the state of the connection to the active one. It owns the JellyfinClient and the ServerCredentialSync of the profile and only reports what they report; the pages never talk to the client. `errorText` is translated and fixed: no URL, body or server text ever reaches it. `libraries` entries (`id`, `name`, `collectionType`, `imageUrl`, `browsable`, `tab`) carry server metadata: render it as plain text
class ConnectionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString quickConnectCode READ quickConnectCode NOTIFY quickConnectCodeChanged)
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    // The saved users of the active server, most recently used first: `id`, `name`, `imageUrl` (a QUrl, empty without an avatar), `remembered` (a token is stored, so choosing the user needs no password) and `askPassword`. `name` is remote text: render it as plain text. Empty while no server is active and while credentials cannot be sent to it
    Q_PROPERTY(QVariantList users READ users NOTIFY usersChanged)
    // What the server lists on its sign-in screen (after loadPublicUsers()), without the users already in `users`: `id`, `name`, `imageUrl` as above
    Q_PROPERTY(QVariantList publicUsers READ publicUsers NOTIFY publicUsersChanged)
    // The name the sign-in page starts with: the saved user that was chosen or whose session ended, empty for a fresh sign-in
    Q_PROPERTY(QString loginUserName READ loginUserName NOTIFY loginUserNameChanged)
    Q_PROPERTY(bool hasSavedUsers READ hasSavedUsers NOTIFY usersChanged)
    // The id of the signed-in user, empty while nobody is (also during a restore that has not landed yet): the page matches it against the `id` of `users`
    Q_PROPERTY(QString currentUserId READ currentUserId NOTIFY currentUserIdChanged)
    Q_PROPERTY(QVariantList libraries READ libraries NOTIFY librariesChanged)
    // The libraries of each tab type (the `libraries` entries whose `tab` is "movies" or "tvshows"); a tab shows only when its list is not empty
    Q_PROPERTY(QVariantList movieLibraries READ movieLibraries NOTIFY librariesChanged)
    Q_PROPERTY(QVariantList showLibraries READ showLibraries NOTIFY librariesChanged)
    Q_PROPERTY(Kinefin::LibraryItemsModel *library READ library CONSTANT)
    Q_PROPERTY(Kinefin::LibraryItemsModel *movies READ movies CONSTANT)
    Q_PROPERTY(Kinefin::LibraryItemsModel *shows READ shows CONSTANT)
    Q_PROPERTY(Kinefin::HomeRowsModel *homeRows READ homeRows CONSTANT)
    Q_PROPERTY(Kinefin::SearchModel *search READ search CONSTANT)
    Q_PROPERTY(Kinefin::GenreRailsModel *genreRails READ genreRails CONSTANT)
    // The sub-tabs of the Movies and TV Shows pages and the data of the one showing; it follows openType (opened for the scope the tab opens, closed with the Home tab and the session) and drives `genreRails` while its Genres sub-tab shows
    Q_PROPERTY(Kinefin::LibrarySubtabController *subtabs READ subtabs CONSTANT)
    Q_PROPERTY(Kinefin::ResumeItemsModel *resumeItems READ resumeItems CONSTANT)
    Q_PROPERTY(Kinefin::PlayQueue *queue READ queue CONSTANT)
    // The editor of the signed-in user's server-side preferences (the user preferences page); it follows the session and is fed the `libraries` ids and names
    Q_PROPERTY(Kinefin::UserConfigurationModel *userConfiguration READ userConfiguration CONSTANT)
    // The Home projection of the user's configuration: `homeLibraries` are the tiles of the Home page (`libraries` ordered by OrderedViews, without MyMediaExcludes), `recentLibraries` the libraries whose Recently Added row shows (without LatestItemsExcludes). Both are empty until the session's configuration has been loaded or has failed to load (then they are `libraries` as they are), so a hidden library never shows for a moment; `libraries` itself is never filtered
    Q_PROPERTY(QVariantList homeLibraries READ homeLibraries NOTIFY homeProjectionChanged)
    Q_PROPERTY(QVariantList recentLibraries READ recentLibraries NOTIFY homeProjectionChanged)
    // The favorites of the signed-in user across `libraries` (the Favorites page), and the same snapshot split into groups; both follow the session and the library list
    Q_PROPERTY(Kinefin::FavoritesModel *favorites READ favorites CONSTANT)
    Q_PROPERTY(Kinefin::FavoritesGroupsModel *favoriteGroups READ favoriteGroups CONSTANT)

  public:
    enum State {
        Idle,            // no active server
        Connecting,      // probing the address, or restoring a stored session
        ConfirmInsecure, // plain HTTP to a non-loopback host: waiting for the user's explicit opt-in
        NeedsLogin,      // the server is known, no session
        SigningIn,       // password login in flight
        QuickConnectPending, // a Quick Connect code is (about to be) shown
        Authenticated,
        ChoosingUser // the server is known and has saved users: waiting for the choice of one (QML compares states by name, so new values go last)
    };
    Q_ENUM(State)

    // `deviceId` is the profile's stable device id (profileDeviceId()). Tokens go to `store`, which must outlive the controller; without one the user's KWallet is used
    ConnectionController(const QString &profileId, const QString &deviceId, const QString &version,
                         TokenStore *store = nullptr, QObject *parent = nullptr);
    ~ConnectionController() override;

    State state() const { return m_state; }
    QString errorText() const { return m_errorText; }
    QString serverUrl() const { return m_serverUrl; }
    QString quickConnectCode() const { return m_quickConnectCode; }
    QVariantList servers() const { return m_servers; }
    QVariantList users() const { return m_users; }
    QVariantList publicUsers() const { return m_publicUsers; }
    QString loginUserName() const { return m_loginUserName; }
    QString currentUserId() const { return m_client.userId(); }
    bool hasSavedUsers() const { return !m_users.isEmpty(); }
    QVariantList libraries() const { return m_libraries; }
    QVariantList movieLibraries() const { return typeLibraries(QStringLiteral("movies")); }
    QVariantList showLibraries() const { return typeLibraries(QStringLiteral("tvshows")); }
    // The `libraries` entries of a tab type ("movies" or "tvshows"); empty for another type
    QVariantList typeLibraries(const QString &type) const;
    LibraryItemsModel *library() { return &m_library; }
    LibraryItemsModel *movies() { return &m_movies; }
    LibraryItemsModel *shows() { return &m_shows; }
    HomeRowsModel *homeRows() { return &m_home; }
    SearchModel *search() { return &m_search; }
    GenreRailsModel *genreRails() { return &m_genreRails; }
    LibrarySubtabController *subtabs() { return &m_subtabs; }
    ResumeItemsModel *resumeItems() { return &m_resumeItems; }
    PlayQueue *queue() { return &m_queue; }
    UserConfigurationModel *userConfiguration() { return &m_userConfiguration; }
    QVariantList homeLibraries() const;
    QVariantList recentLibraries() const;
    FavoritesModel *favorites() { return &m_favorites; }
    FavoritesGroupsModel *favoriteGroups() { return &m_favoriteGroups; }

    // The place images may be fetched from: the server URL while Authenticated, ChoosingUser or NeedsLogin (the avatars), nothing in every other state. Shared with the image fetcher, which runs on QML's network thread
    void setImageOrigin(std::shared_ptr<ImageOrigin> origin);

    // For tests; the default is 5 s
    void setQuickConnectPollInterval(std::chrono::milliseconds interval);

    // The key the browse state store keeps a view under, `libraryId` lower-cased without dashes. Without a `subtab` it is the scope of a tab or a collection library: `movies`, `tvshows:<id>`, `boxsets:<id>` (an empty `libraryId` is all libraries). The sub-tabs are `movies` + `favorites` (`movies/favorites[:<id>]`), `movies` + `collections` (`movies/collections`, `libraryId` ignored: collections belong to no one library), `tvshows` + `episodes` (`tvshows/episodes[:<id>]`) and `tvshows` + `network`, where `libraryId` is the studio id (`tvshows/network:<studioId>`, required). Empty for any other combination
    static QString scopeKey(const QString &type, const QString &subtab, const QString &libraryId);

    // Connects to the most recently used stored server, if there is one
    Q_INVOKABLE void start();
    // `address` is what the user typed; without a scheme it is taken as https. Once the server answers: no saved user goes to the sign-in, a single saved user that is remembered and does not ask for the password is restored, a single other one goes to the sign-in with the name filled in, and two or more go to the picker (ChoosingUser)
    Q_INVOKABLE void connectTo(const QString &address);
    // The user accepted sending credentials over plain HTTP to this server (this connection only)
    Q_INVOKABLE void confirmInsecureHttp();
    Q_INVOKABLE void login(const QString &username, const QString &password);
    Q_INVOKABLE void startQuickConnect();
    Q_INVOKABLE void cancelQuickConnect();
    // Disconnects the active server and returns to Idle, cancelling what is in flight; the stored session is kept (a later connect restores it)
    Q_INVOKABLE void cancel();
    // Removes the signed-in user as removeUser() does; lands on the picker when other users of the server remain, else on Idle. Without a signed-in user it only leaves the server
    Q_INVOKABLE void signOut();
    // Leaves the session and opens the picker of the same server: the token and the saved queue stay, so choosing the user again needs no password. A session that could not be saved leaves no one to pick, so it goes to the sign-in
    Q_INVOKABLE void switchUser();
    // Picks a saved user of the active server (from ChoosingUser or the sign-in page): one that is remembered and does not ask for the password is restored (Connecting), any other goes to the sign-in with its name in `loginUserName`. Ignored for an unknown id and in any other state, Connecting and SigningIn included
    Q_INVOKABLE void chooseUser(const QString &userId);
    // Leaves the session, if any, and goes to the sign-in of the same server for a new user; the saved tokens stay
    Q_INVOKABLE void addUser();
    // Forgets one saved user of the active server: token, record and saved queue (the browse state stays). Removing the signed-in user ends its session. Lands on the picker when other users remain, else on Idle. Ignored for an id that is neither a saved user nor the signed-in one
    Q_INVOKABLE void removeUser(const QString &userId);
    // Turns "Ask for Password" of a saved user on or off: on, no token is kept for the user (a live session stays in memory only) and choosing it always asks for the password
    Q_INVOKABLE void setUserAskPassword(const QString &userId, bool ask);
    // From the sign-in page back to the picker; ignored without saved users
    Q_INVOKABLE void chooseAnotherUser();
    // Asks the server for the users of its sign-in screen: the answer fills `publicUsers`
    Q_INVOKABLE void loadPublicUsers();
    // Loads the items of one of the current `libraries` into `library`; false for an id that is not in the list or whose type cannot be browsed
    Q_INVOKABLE bool openLibrary(const QString &libraryId);
    // The details of one item for the page `owner`: the model is a child of `owner` and dies with it. Null unless signed in and `owner` exists
    Q_INVOKABLE Kinefin::ItemDetailsModel *createItemDetails(const QString &itemId, QObject *owner);
    // Loads the home rows again (the libraries are kept)
    Q_INVOKABLE void reloadHome();
    // Whether a library of this tab type ("movies" or "tvshows") is listed
    Q_INVOKABLE bool hasType(const QString &type) const;
    // Loads a tab type's items into `movies` or `shows`, with the sort order, filters and view options last used for that scope (a saved tag or rating is resolved by the model once its facet list has loaded, see LibraryItemsModel). An empty `libraryId` is the library selected last (kept per server and user); an id that is not a library of the type, "*" say, selects all libraries of it. With fewer than two libraries of the type the scope is always all of them. Nothing is reloaded when the scope is already open. False when signed out, for another type or when no library of the type is listed
    Q_INVOKABLE bool openType(const QString &type, const QString &libraryId = {});
    // The same scope choice as openType, but always reopened as a one-off view: newest first, no filter and no search term, whatever is stored (the view mode, image type and card options stay as stored). Nothing of it is written to the browse state: the stored sort, direction and filters stay, while an edit made meanwhile is saved with them (a filter edit replaces the stored filters). The view lasts until the user picks another sort or the next openType of that type, which reopens with the stored view. Same false cases as openType
    Q_INVOKABLE bool openRecentlyAdded(const QString &type, const QString &libraryId = {});
    // Starts the genre rails of the library open in `model` (`library`, `movies` or `shows`); false when it is none of them or none is open
    Q_INVOKABLE bool openGenreRails(Kinefin::LibraryItemsModel *model);
    // The sub-tabs the settings turn on for `type` (the ids of LibrarySubtabController::subtabId, e.g. "favorites"; unknown ones are ignored): the sub-tab bar follows at once, and a showing sub-tab that went off gives way to the main one. Kept across sessions
    Q_INVOKABLE void setSubtabsEnabled(const QString &type, const QStringList &ids);
    // Leaves the Movies and TV Shows pages (the Home tab): the showing sub-tab's data and channels are released, the sub-tab last used stays remembered
    Q_INVOKABLE void closeSubtabs();
    // Starts loading everything in Continue Watching into `resumeItems` (the page that shows it calls its `close()` when it goes); false when not signed in
    Q_INVOKABLE bool openResumeItems();
    // Starts a new scan of the favorites into `favorites` (the page that shows it calls its `close()` when it goes), so a visit never shows a snapshot of an earlier one; a scan already under way is kept. False when not signed in
    Q_INVOKABLE bool openFavorites();
    // Asks the server for the libraries again (the Home page's retry)
    Q_INVOKABLE void reloadLibraries();
    // Removes a stored server with all its users, saved queues and browse state; disconnects first when it is the active one
    Q_INVOKABLE void removeServer(const QString &serverId);
    // Opens the admin dashboard of the active server's web interface in the host browser (the app's one action for everything server-side). False when there is no server or its address is not an http(s) link
    Q_INVOKABLE bool openAdminDashboard() const;

  Q_SIGNALS:
    void stateChanged();
    void errorTextChanged();
    void serverUrlChanged();
    void quickConnectCodeChanged();
    void serversChanged();
    void usersChanged();
    void publicUsersChanged();
    void loginUserNameChanged();
    void currentUserIdChanged();
    void librariesChanged();
    // `homeLibraries` or `recentLibraries` may have changed (also with every librariesChanged)
    void homeProjectionChanged();

  private:
    void onServerReady();
    void onRestoreFinished();
    // The server is probed and credentials may be sent: sign-in, an automatic restore or the picker, by how many users are saved
    void proceedAfterProbe();
    // Starts restoring a saved user: Connecting, or the sign-in with the name filled in when the restore cannot start
    void restoreUser(const QString &userId);
    // The sign-in page for a saved user (empty id: a fresh sign-in)
    void askPasswordOf(const QString &userId);
    // The saved user of the active server with this id
    std::optional<ServerCredential> savedUser(const QString &userId) const;
    // Removes a user's token, record and saved queue
    void forgetUserData(const QString &userId);
    // The session is gone, the server stays probed: no libraries, nothing pending
    void leaveSession();
    void onLoginSucceeded();
    void onSessionExpired(const QString &userId);
    void onRequestFailed(int status);
    void onUserViews(const QJsonArray &items);
    LibraryItemsModel *typeModel(const QString &type);
    // The scope a tab type can open: `requested` when two or more libraries of the type are listed and it is one of them, else all (empty)
    QString resolveScope(const QString &type, const QString &requested) const;
    // The scope openType and openRecentlyAdded open: `requested` (or the stored selection when empty) resolved, and remembered as the selection when it can be one
    QString chooseScope(const QString &type, const QString &libraryId);
    // Ends a one-off view of the type's model, then opens `scope` unless it is already open and no one-off was showing
    bool openTypeScope(const QString &type, const QString &scope);
    // Opens `scope` with `view`, or with the stored view of the scope when none is given
    bool openScopeView(const QString &type, const QString &scope,
                       const std::optional<LibraryItemsModel::ViewState> &view);
    void keepTypeModelsConsistent();
    // The name of one of the libraries of a tab type, empty for all of them
    QString libraryNameOf(const QString &type, const QString &libraryId) const;
    // The key the browse state store keeps the view of the showing sub-tab's list under, empty when it has none
    QString subtabViewKey() const;
    void connectViewPersistence(LibraryItemsModel *model);
    void setState(State state);
    void updateImageOrigin();
    // Whether the state lets the pages load images: the same states the image origin is set in
    bool imagesAllowed() const;
    QUrl avatarUrl(const QString &userId, const QString &tag) const;
    void setError(const QString &text);
    void clearQuickConnect();
    void clearLibraryError();
    void refreshServers();
    void refreshUsers();
    void refreshPublicUsers();
    void setLoginUserName(const QString &name);
    // Emits currentUserIdChanged when the client's user differs from the last one announced
    void refreshCurrentUserId();
    // Takes the configuration the server confirmed (null: it could not be loaded, Home goes unprojected) as the Home projection and rebuilds the Home rows when what they ask for changed
    void applyPreferences(const UserPreferencesSnapshot *snapshot);
    // Forgets the projection of the session that ended
    void resetHomeProjection();
    bool hidePlayedInLatest() const { return m_hasPreferences && m_preferences.hidePlayedInLatest; }

    std::unique_ptr<TokenStore> m_ownedStore; // KWallet, unless a store was passed in
    // Declared before the client so it outlives the signals the client emits while it goes: the models showing a one-off view and the state it was opened with, which is not what is stored for the scope
    QHash<const LibraryItemsModel *, LibraryItemsModel::ViewState> m_oneOff;
    JellyfinClient m_client;
    ServerCredentialSync m_sync;
    LibraryItemsModel m_library{&m_client, LibraryItemsModel::TileChannel};
    LibraryItemsModel m_movies{&m_client, LibraryItemsModel::MoviesChannel};
    LibraryItemsModel m_shows{&m_client, LibraryItemsModel::ShowsChannel};
    HomeRowsModel m_home{&m_client};
    SearchModel m_search{&m_client};
    GenreRailsModel m_genreRails{&m_client};
    // After the client and the rails it drives (its destructor clears them)
    LibrarySubtabController m_subtabs{&m_client, &m_genreRails};
    ResumeItemsModel m_resumeItems{&m_client};
    // The collection type the rails were opened for: re-scoping another type leaves them alone
    QString m_genreRailsType;
    PlayQueue m_queue{&m_client};
    UserConfigurationModel m_userConfiguration{&m_client};
    FavoritesModel m_favorites{&m_client};
    FavoritesGroupsModel m_favoriteGroups{&m_favorites};
    BrowseStateStore m_browse;
    int m_nextDetailsChannel = ItemDetailsModel::FirstChannel;
    std::shared_ptr<ImageOrigin> m_imageOrigin;
    QTimer m_quickConnectTimer;
    State m_state = Idle;
    QString m_errorText;
    QString m_serverUrl;
    QString m_quickConnectCode;
    QVariantList m_servers;
    QVariantList m_users;
    QList<PublicUser>
        m_publicSource; // what the server listed, before the saved users are taken out
    QVariantList m_publicUsers;
    QString m_loginUserName;
    QString m_currentUserId;   // the last value announced
    QString m_restoringUserId; // the saved user a restore in flight is for
    QVariantList m_libraries;
    bool m_identityValid = false;
    bool m_libraryError = false; // errorText currently shows the libraries' failure
    // The Home projection: the last configuration the server confirmed for this session (`m_hasPreferences`), whether its load has been answered (`m_preferencesSettled`, failure included) and whether the libraries have been listed (`m_librariesLoaded`: the rows load once both are)
    UserPreferencesSnapshot m_preferences;
    bool m_hasPreferences = false;
    bool m_preferencesSettled = false;
    bool m_librariesLoaded = false;
};
} // namespace Kinefin
