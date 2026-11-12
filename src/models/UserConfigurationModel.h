// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "api/JellyfinClient.h"
#include "models/CultureOptionsModel.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <array>
#include <bitset>

namespace Kinefin {
// One library the user can see (`/UserViews`): the id goes in the preference arrays, the name is server text for display only
struct LibraryOption {
    QString id;
    QString name;
};

// What the saved and verified configuration says, for the consumers that apply it (the Home projection, the future player). Built from the server's data, never from an unsaved draft; the id lists hold every id the server keeps, hidden ones included
struct UserPreferencesSnapshot {
    QString audioLanguage;
    QString subtitleLanguage;
    QString subtitleMode;
    bool playDefaultAudioTrack = true;
    bool rememberAudioSelections = true;
    bool rememberSubtitleSelections = true;
    bool enableNextEpisodeAutoPlay = true;
    bool displayMissingEpisodes = false;
    bool hidePlayedInLatest = true;
    QStringList orderedViews;
    QStringList myMediaExcludes;
    QStringList latestItemsExcludes;
};

// The editor of the signed-in user's `UserConfiguration` (the user preferences page): the whole read-modify-write cycle in QtCore, so QML only sees typed properties and actions, never JSON. The configuration is loaded once (`GET /Users/Me`, `Configuration` kept as `m_baselineRaw`) and edited as a typed draft of the fields the page owns; every other field (GroupedFolders, nulls, nested objects, enum values this client does not know, fields of a newer server) is never parsed into the draft and travels untouched. A field is dirty while its draft differs from the baseline, so setting it back clears it. save() reads the configuration again right before writing: a dirty field that changed on the server since the baseline (and is not already what the draft says) is a conflict and nothing is written; otherwise the draft is merged over that fresh object, the whole object is posted and read once more to verify the written fields. The server offers no compare-and-swap, so two clients saving at the same moment can still overwrite each other. The id lists (OrderedViews, MyMediaExcludes, LatestItemsExcludes) are edited as the ids of the libraries the user can see (setLibraries): ids the server holds that are not visible stay exactly where they are in the merged array. Display preferences are only read (the home sections preview) and never written. Everything the server sends is plain text; a language the catalog does not know and an enum value this client does not know stay selected until the user picks another one. Losing the session (credentialsCleared) empties everything and cancels the cycle: nothing is retried and no later reply reaches another session
class UserConfigurationModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(Failure failure READ failure NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY draftChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY draftChanged)
    Q_PROPERTY(QStringList dirtyFields READ dirtyFields NOTIFY draftChanged)
    Q_PROPERTY(QStringList conflictFields READ conflictFields NOTIFY stateChanged)
    Q_PROPERTY(QStringList malformedFields READ malformedFields NOTIFY stateChanged)
    Q_PROPERTY(QString audioLanguage READ audioLanguage WRITE setAudioLanguage NOTIFY draftChanged)
    Q_PROPERTY(bool playDefaultAudioTrack READ playDefaultAudioTrack WRITE setPlayDefaultAudioTrack
                   NOTIFY draftChanged)
    Q_PROPERTY(bool rememberAudioSelections READ rememberAudioSelections WRITE
                   setRememberAudioSelections NOTIFY draftChanged)
    Q_PROPERTY(QString subtitleLanguage READ subtitleLanguage WRITE setSubtitleLanguage NOTIFY
                   draftChanged)
    Q_PROPERTY(QString subtitleMode READ subtitleMode WRITE setSubtitleMode NOTIFY draftChanged)
    Q_PROPERTY(bool rememberSubtitleSelections READ rememberSubtitleSelections WRITE
                   setRememberSubtitleSelections NOTIFY draftChanged)
    Q_PROPERTY(bool enableNextEpisodeAutoPlay READ enableNextEpisodeAutoPlay WRITE
                   setEnableNextEpisodeAutoPlay NOTIFY draftChanged)
    Q_PROPERTY(bool displayMissingEpisodes READ displayMissingEpisodes WRITE
                   setDisplayMissingEpisodes NOTIFY draftChanged)
    Q_PROPERTY(bool hidePlayedInLatest READ hidePlayedInLatest WRITE setHidePlayedInLatest NOTIFY
                   draftChanged)
    Q_PROPERTY(QStringList orderedViews READ orderedViews WRITE setOrderedViews NOTIFY draftChanged)
    Q_PROPERTY(QStringList myMediaExcludes READ myMediaExcludes WRITE setMyMediaExcludes NOTIFY
                   draftChanged)
    Q_PROPERTY(QStringList latestItemsExcludes READ latestItemsExcludes WRITE setLatestItemsExcludes
                   NOTIFY draftChanged)
    Q_PROPERTY(QStringList subtitleModes READ subtitleModes CONSTANT)
    Q_PROPERTY(QVariantList libraries READ libraries NOTIFY librariesChanged)
    Q_PROPERTY(int unavailableLibraryCount READ unavailableLibraryCount NOTIFY stateChanged)
    Q_PROPERTY(Kinefin::CultureOptionsModel *cultures READ cultures CONSTANT)
    Q_PROPERTY(
        HomeSectionsStatus homeSectionsStatus READ homeSectionsStatus NOTIFY homeSectionsChanged)
    Q_PROPERTY(QVariantList homeSections READ homeSections NOTIFY homeSectionsChanged)

  public:
    enum State { Idle, Loading, Ready, Saving, Verifying, Conflict, Failed };
    Q_ENUM(State)
    // Why the state is Failed: the page maps each to a fixed message (never a server text). NotVerified: the write was sent but could not be confirmed, reload before trusting the draft
    enum Failure { NoFailure, LoadFailed, InvalidProfile, SaveFailed, NotVerified };
    Q_ENUM(Failure)
    enum HomeSectionsStatus {
        HomeSectionsIdle,
        HomeSectionsLoading,
        HomeSectionsReady,
        HomeSectionsUnavailable
    };
    Q_ENUM(HomeSectionsStatus)
    // The home sections the display preferences name, in slot order; anything else is Unsupported and carries no text
    enum HomeSection { Libraries, ContinueWatching, NextUp, RecentlyAdded, Unsupported };
    Q_ENUM(HomeSection)

    // The editable fields, in the order of kFields in the implementation
    enum Field {
        AudioLanguage,
        SubtitleLanguage,
        SubtitleMode,
        PlayDefaultAudioTrack,
        RememberAudioSelections,
        RememberSubtitleSelections,
        EnableNextEpisodeAutoPlay,
        DisplayMissingEpisodes,
        HidePlayedInLatest,
        OrderedViews,
        MyMediaExcludes,
        LatestItemsExcludes,
        FieldCount
    };

    // The most a configuration may weigh serialized, and the most libraries and characters of a language code taken in
    static constexpr int MaxConfigurationBytes = 256 * 1024;
    static constexpr int MaxLibraries = 200;
    static constexpr int MaxTextChars = 64;

    explicit UserConfigurationModel(JellyfinClient *client, QObject *parent = nullptr);
    ~UserConfigurationModel() override;

    // Loads the configuration of the signed-in user (and, once per session, the language catalog; and the home sections preview): drops any draft. Ignored while a request is in flight. Without a session the state is Failed(LoadFailed)
    Q_INVOKABLE void load();
    // Starts the save sequence; only from Ready with a dirty field
    Q_INVOKABLE void save();
    // From Failed: a failed write is sent again (the sequence restarts with the fresh read), anything else reloads
    Q_INVOKABLE void retry();
    // Resets the draft to the baseline. From a failure that left the baseline unreliable, or once a write was sent without a verified outcome, it reloads instead
    Q_INVOKABLE void discard();
    // Drops the draft without a network read, for a page that goes away: nothing happens after a write of unknown outcome, the next visit loads again
    Q_INVOKABLE void discardDraft();
    // Leaves Conflict, or a failed write, and returns to editing the draft: the next save() checks again against the server. From Conflict the server's values become the baseline, so that save overwrites them
    Q_INVOKABLE void keepEditing();
    // Moves a library one place in the draft order of OrderedViews (the keyboard reorder); out-of-range indexes do nothing
    Q_INVOKABLE void moveOrderedView(int from, int to);
    Q_INVOKABLE void setMyMediaExcluded(const QString &libraryId, bool excluded);
    Q_INVOKABLE void setLatestExcluded(const QString &libraryId, bool excluded);
    // Drops everything tied to the session (what credentialsCleared does); for Switch User and Switch Server
    Q_INVOKABLE void clear();

    // The libraries the user can see, from `/UserViews`: invalid ids, empty names, repeats and anything past MaxLibraries are dropped. Safe at any time: the draft id lists are re-derived (a dirty list keeps its choices among the ids still visible)
    void setLibraries(const QList<LibraryOption> &libraries);

    State state() const { return m_state; }
    Failure failure() const { return m_failure; }
    bool loaded() const { return m_hasBaseline; }
    bool editable() const { return m_state == Ready && m_hasBaseline; }
    bool dirty() const { return !dirtyFieldList().isEmpty(); }
    bool canSave() const { return m_state == Ready && dirty(); }
    // The `UserConfiguration` keys that differ from the baseline
    QStringList dirtyFields() const;
    // The keys that blocked the last save: changed on the server and in the draft
    QStringList conflictFields() const { return m_conflictFields; }
    // Fields whose value has a shape the editor cannot represent (a number where a bool belongs): read-only, never written
    QStringList malformedFields() const;
    // Bumped by every load() and clear(): for tests and for guarding deferred work
    quint64 generation() const { return m_generation; }
    // The baseline as a snapshot (empty values before the first load)
    UserPreferencesSnapshot snapshot() const;

    QString audioLanguage() const { return m_draft[AudioLanguage].toString(); }
    void setAudioLanguage(const QString &code) { setText(AudioLanguage, code); }
    QString subtitleLanguage() const { return m_draft[SubtitleLanguage].toString(); }
    void setSubtitleLanguage(const QString &code) { setText(SubtitleLanguage, code); }
    QString subtitleMode() const { return m_draft[SubtitleMode].toString(); }
    void setSubtitleMode(const QString &mode) { setText(SubtitleMode, mode); }
    bool playDefaultAudioTrack() const { return m_draft[PlayDefaultAudioTrack].toBool(); }
    void setPlayDefaultAudioTrack(bool value) { assign(PlayDefaultAudioTrack, value); }
    bool rememberAudioSelections() const { return m_draft[RememberAudioSelections].toBool(); }
    void setRememberAudioSelections(bool value) { assign(RememberAudioSelections, value); }
    bool rememberSubtitleSelections() const { return m_draft[RememberSubtitleSelections].toBool(); }
    void setRememberSubtitleSelections(bool value) { assign(RememberSubtitleSelections, value); }
    bool enableNextEpisodeAutoPlay() const { return m_draft[EnableNextEpisodeAutoPlay].toBool(); }
    void setEnableNextEpisodeAutoPlay(bool value) { assign(EnableNextEpisodeAutoPlay, value); }
    bool displayMissingEpisodes() const { return m_draft[DisplayMissingEpisodes].toBool(); }
    void setDisplayMissingEpisodes(bool value) { assign(DisplayMissingEpisodes, value); }
    bool hidePlayedInLatest() const { return m_draft[HidePlayedInLatest].toBool(); }
    void setHidePlayedInLatest(bool value) { assign(HidePlayedInLatest, value); }
    // Ids of visible libraries only: the full order (libraries missing from the stored array come last, in server order)
    QStringList orderedViews() const { return m_draft[OrderedViews].toStringList(); }
    void setOrderedViews(const QStringList &ids) { setIds(OrderedViews, ids); }
    QStringList myMediaExcludes() const { return m_draft[MyMediaExcludes].toStringList(); }
    void setMyMediaExcludes(const QStringList &ids) { setIds(MyMediaExcludes, ids); }
    QStringList latestItemsExcludes() const { return m_draft[LatestItemsExcludes].toStringList(); }
    void setLatestItemsExcludes(const QStringList &ids) { setIds(LatestItemsExcludes, ids); }
    // The values of the `SubtitleMode` enum the editor offers
    QStringList subtitleModes() const;

    // `{id, name}` of each visible library, in server order
    QVariantList libraries() const;
    // How many distinct library ids the stored preferences name that the user cannot see: kept in every save, never shown
    int unavailableLibraryCount() const;
    CultureOptionsModel *cultures() const { return m_cultures; }
    HomeSectionsStatus homeSectionsStatus() const { return m_homeStatus; }
    QVariantList homeSections() const;

  Q_SIGNALS:
    // State, failure, conflict fields, loaded/editable and the unavailable count changed
    void stateChanged();
    // A draft value, the dirty set or canSave changed
    void draftChanged();
    void librariesChanged();
    void homeSectionsChanged();
    // The write was confirmed by a second read: the page's "saved" notice
    void saved();
    // Same moment, for C++ consumers: the verified configuration, to project onto Home and the player. Never emitted for a draft or a failed or unverified write
    void configurationApplied(const Kinefin::UserPreferencesSnapshot &snapshot);

  private:
    enum class Step { None, LoadFetch, PrefetchFetch, Posting, VerifyFetch };

    void onUserReady(const QJsonObject &user);
    void onUserFailed();
    void onUpdated();
    void onUpdateFailed();
    void onCulturesReady(const QJsonArray &cultures);
    void onHomeSectionsReady(const QJsonObject &customPrefs);
    void startSave();
    void setState(State state, Step step);
    void fail(Failure failure);
    void adoptBaseline(const QJsonObject &configuration);
    void rebaseOntoLatest();
    bool userMatches(const QJsonObject &user) const;
    QVariant project(const QJsonObject &configuration, Field field) const;
    QVariant defaultValue(Field field) const;
    bool isDirty(Field field) const;
    QList<Field> dirtyFieldList() const;
    QJsonObject mergedOver(const QJsonObject &latest) const;
    QJsonArray mergedIds(Field field, const QJsonArray &latest, const QStringList &draft) const;
    QString visibleId(const QString &id) const;
    QStringList normalizedIds(Field field, const QStringList &ids) const;
    bool isEditable(Field field) const;
    void assign(Field field, const QVariant &value);
    void setText(Field field, const QString &value);
    void setIds(Field field, const QStringList &ids);
    void toggleExcluded(Field field, const QString &libraryId, bool excluded);
    void setHomeStatus(HomeSectionsStatus status);

    // The client comes first: handlers connected to its signals may touch everything below it, which is declared after
    QPointer<JellyfinClient> m_client;
    CultureOptionsModel *m_cultures;
    State m_state = Idle;
    Failure m_failure = NoFailure;
    Step m_step = Step::None;
    quint64 m_generation = 0;
    QString m_userId;
    bool m_hasBaseline = false;
    QJsonObject m_baselineRaw;
    QJsonObject m_latestRaw;
    // The write was handed to the client and no verified read followed: the server may or may not have applied it
    bool m_postSent = false;
    std::array<QVariant, FieldCount> m_draft;
    std::bitset<FieldCount> m_malformed;
    QList<Field> m_saving;
    QJsonObject m_pending;
    QStringList m_conflictFields;
    QList<LibraryOption> m_libraries;
    QHash<QString, int> m_libraryRow; // lower-case id -> row of m_libraries
    bool m_culturesPending = false;
    bool m_homePending = false;
    HomeSectionsStatus m_homeStatus = HomeSectionsIdle;
    QList<int> m_homeSections;
};
} // namespace Kinefin

Q_DECLARE_METATYPE(Kinefin::UserPreferencesSnapshot)
