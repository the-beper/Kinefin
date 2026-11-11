// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/GenreRailsModel.h"
#include "models/ItemRowModel.h"
#include "models/LibraryItemsModel.h"
#include "models/RecommendationRowsModel.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace Kinefin {
// The sub-tabs of the Movies and TV Shows pages (Movies: main, Suggestions, Favorites, Collections, Genres; TV Shows: main, Suggestions, Upcoming, Genres, Networks, Episodes) for the open type (`movies` or `tvshows`) and library (empty: every library), and the data of the one that is showing. The enabled sub-tabs are an input (the main one is always available), the controller reads no settings. Lazy: only `open()` and `select()` ask the server, and only for the showing sub-tab; leaving one clears its data and releases its channels, so an answer that was in flight never reaches the next view. The main sub-tab's list is not owned here (the Movies and TV Shows tabs keep theirs). The listing channels 3072 to 3078 are used here and only here (never 3082, the global Favorites page's): the movie list 3072 (Favorites, Collections), the show list 3073 (Episodes, a network's series), recommendations 3074, TV suggestions 3075, upcoming 3076, networks 3077, the episode counter 3078. The Genres sub-tab drives the `GenreRailsModel` given at construction (the rails' own channels) and clears it when it is left; the owner must not use that model for anything else meanwhile. The controller must be declared after the client in its owner and is emptied, with the remembered sub-tabs, when the client loses its credentials
class LibrarySubtabController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString type READ type NOTIFY scopeChanged)
    Q_PROPERTY(QString libraryId READ libraryId NOTIFY scopeChanged)
    // The ids of the sub-tabs on offer for the open type, in order: main, suggestions, favorites, collections, genres, upcoming, networks, episodes
    Q_PROPERTY(QStringList available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)
    // The state of what the showing sub-tab lists: `status` and `empty` (loaded, nothing to list: a filtered-empty list is the list's own `filtered`)
    Q_PROPERTY(Status status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool empty READ isEmpty NOTIFY stateChanged)
    // The showing data ignores the chosen library (Collections, TV suggestions and the series of a network): the view says so
    Q_PROPERTY(bool crossLibrary READ crossLibrary NOTIFY currentChanged)
    // The key the showing list's view state is kept under (see BrowseStateStore), empty when the sub-tab has no such list
    Q_PROPERTY(QString scopeKey READ scopeKey NOTIFY currentChanged)
    // The list model of the showing sub-tab (Favorites, Collections, Episodes, a network's series), or null
    Q_PROPERTY(Kinefin::LibraryItemsModel *list READ activeList NOTIFY currentChanged)
    Q_PROPERTY(Kinefin::RecommendationRowsModel *recommendations READ recommendations CONSTANT)
    Q_PROPERTY(Kinefin::ItemRowModel *suggestions READ suggestions CONSTANT)
    Q_PROPERTY(Kinefin::ItemRowModel *upcoming READ upcoming CONSTANT)
    Q_PROPERTY(Kinefin::ItemRowModel *networks READ networks CONSTANT)
    // The Networks sub-tab shows the networks (empty `networkId`) or the series of the chosen one
    Q_PROPERTY(QString networkId READ networkId NOTIFY currentChanged)
    Q_PROPERTY(QString networkName READ networkName NOTIFY currentChanged)
    // More networks than the list holds: it is cut
    Q_PROPERTY(bool networksCapped READ networksCapped NOTIFY stateChanged)
    // The episodes of the library as the server counted them (a separate request); -1 while unknown or when that request failed, never a guess
    Q_PROPERTY(int episodeCount READ episodeCount NOTIFY episodeCountChanged)

  public:
    enum Subtab { Main, Suggestions, Favorites, Collections, Genres, Upcoming, Networks, Episodes };
    Q_ENUM(Subtab)
    enum Status { Idle, Loading, Ready, Failed };
    Q_ENUM(Status)

    static constexpr int MovieListChannel = 3072;
    static constexpr int ShowListChannel = 3073;
    static constexpr int RecommendationsChannel = 3074;
    static constexpr int SuggestionsChannel = 3075;
    static constexpr int UpcomingChannel = 3076;
    static constexpr int NetworksChannel = 3077;
    static constexpr int EpisodeCountChannel = 3078;

    // The recommendation rows asked for, and the items of each
    static constexpr int CategoryLimit = 6;
    static constexpr int RecommendationItemLimit = 12;
    // The TV suggestions asked for
    static constexpr int SuggestionLimit = 24;
    // The most upcoming episodes and networks listed
    static constexpr int MaxListed = ItemRowModel::MaxItems;

    using ViewStateProvider = std::function<LibraryItemsModel::ViewState(const QString &scopeKey)>;

    // `genres` is the rails model the Genres sub-tab uses: not owned, may be null (that sub-tab then fails)
    LibrarySubtabController(JellyfinClient *client, GenreRailsModel *genres,
                            QObject *parent = nullptr);
    ~LibrarySubtabController() override;

    // The id of a sub-tab ("main", "suggestions", ...) and back
    static QString subtabId(Subtab subtab);
    static std::optional<Subtab> subtabFromId(const QString &id);
    // The sub-tabs a type has, in order, enabled or not; empty for another type
    static QList<Subtab> subtabsOf(const QString &type);

    // The sub-tabs the settings turn on for `type` (the main one needs no entry, a sub-tab the type does not have is ignored). Takes effect at once: a showing sub-tab that is no longer on is left for the main one
    void setEnabled(const QString &type, const QList<Subtab> &enabled);
    // Opens the page of `type` on `libraryId` (empty: every library; `libraryName` is for the list titles) and shows the sub-tab last used for the type in this session, or the main one. The same type and library again changes nothing, another library shows the same sub-tab for it. False, with everything closed, for an unknown type or an invalid library id
    bool open(const QString &type, const QString &libraryId, const QString &libraryName = {});
    // Leaves the page: the showing sub-tab is torn down and every channel released; the remembered sub-tabs stay
    void close();
    bool isOpen() const { return m_open; }

    // Shows another sub-tab of the open type, tearing the previous one down; false when it is not on offer. Selecting the showing one changes nothing
    bool select(Subtab subtab);
    Q_INVOKABLE bool select(const QString &id);
    // Retry of the showing sub-tab's failed request, keeping the selection (the library, the sub-tab, the chosen network)
    Q_INVOKABLE void retry();
    // Opens the series of a network of the `networks` list (an id it does not list is refused); only in the Networks sub-tab
    Q_INVOKABLE bool selectNetwork(const QString &studioId);
    // Back from a network's series to the list of networks, which stays loaded
    Q_INVOKABLE void closeNetwork();

    // Where the view state of a list-like sub-tab was kept before: asked once when the list opens, so it opens straight into it
    void setViewStateProvider(ViewStateProvider provider) { m_provider = std::move(provider); }

    QString type() const { return m_type; }
    QString libraryId() const { return m_libraryId; }
    QStringList available() const;
    QString current() const { return subtabId(m_current); }
    Subtab currentSubtab() const { return m_current; }
    Status status() const;
    bool isEmpty() const;
    bool crossLibrary() const;
    QString scopeKey() const;
    LibraryItemsModel *activeList();
    LibraryItemsModel *movieList() { return &m_movieList; }
    LibraryItemsModel *showList() { return &m_showList; }
    RecommendationRowsModel *recommendations() { return &m_recommendations; }
    ItemRowModel *suggestions() { return &m_suggestions; }
    ItemRowModel *upcoming() { return &m_upcoming; }
    ItemRowModel *networks() { return &m_networks; }
    QString networkId() const { return m_networkId; }
    QString networkName() const { return m_networkName; }
    bool networksCapped() const { return m_networksCapped; }
    int episodeCount() const { return m_episodeCount; }

  Q_SIGNALS:
    void scopeChanged();
    void availableChanged();
    // The showing sub-tab, its list, scope key or network changed
    void currentChanged();
    // The status or emptiness of the showing sub-tab may have changed
    void stateChanged();
    void episodeCountChanged();
    // The user edited the view of the showing list: keep its `viewState()` under `scopeKey()`
    void listViewStateEdited();

  private:
    // The requests that are not a list model's own
    enum Source { Recommendations, TvSuggestions, UpcomingEpisodes, NetworkList, EpisodeCounter };

    bool isAvailable(Subtab subtab) const;
    void show(Subtab subtab);
    void start(Subtab subtab);
    void teardown();
    void clearAll();
    void openList(LibraryItemsModel *list, const QString &libraryId, const QString &collectionType,
                  const QString &libraryName, const LibraryItemsModel::Constraint &constraint);
    void request(Source source);
    void setSource(Source source, Status status);
    void clearShowList();
    void onCategoriesReady(const QJsonArray &categories, int channel);
    void onFlatReady(ItemRowModel *model, Source source, int expectedChannel,
                     const QJsonArray &items, int total, int channel);
    void releaseChannels();
    void onItemsReady(const QJsonArray &items, int total, int channel);
    void onListingFailed(const QString &family, int status, const QString &reason, int channel);
    void forwardEdit(LibraryItemsModel *list);
    void retryList(LibraryItemsModel *list);

    QPointer<JellyfinClient> m_client;
    QPointer<GenreRailsModel> m_genres;
    bool m_open = false;
    QString m_type;
    QString m_libraryId;
    QString m_libraryName;
    Subtab m_current = Main;
    // The showing sub-tab has been started, its data is requested or loaded
    bool m_started = false;
    bool m_genresOpened = false;
    QHash<QString, QList<Subtab>> m_enabled;
    // The sub-tab last used per type in this session
    QHash<QString, Subtab> m_remembered;
    QString m_networkId;
    QString m_networkName;
    bool m_networksCapped = false;
    int m_episodeCount = -1;
    Status m_sourceStatus[EpisodeCounter + 1] = {};
    ViewStateProvider m_provider;
    LibraryItemsModel m_movieList;
    LibraryItemsModel m_showList;
    RecommendationRowsModel m_recommendations;
    ItemRowModel m_suggestions;
    ItemRowModel m_upcoming;
    ItemRowModel m_networks;
};
} // namespace Kinefin
