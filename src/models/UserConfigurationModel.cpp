// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "UserConfigurationModel.h"

#include "FilterOptionsModel.h"
#include "api/MediaSourceSelection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QVariantMap>

namespace Kinefin {
namespace {
enum class Kind { Bool, Text, Ids };

struct FieldSpec {
    const char *key;
    Kind kind;
    bool defaultBool;
    const char *defaultText;
};

// In the order of UserConfigurationModel::Field; the defaults are what the server assumes for a missing key
constexpr FieldSpec kFields[UserConfigurationModel::FieldCount] = {
    {"AudioLanguagePreference", Kind::Text, false, ""},
    {"SubtitleLanguagePreference", Kind::Text, false, ""},
    {"SubtitleMode", Kind::Text, false, "Default"},
    {"PlayDefaultAudioTrack", Kind::Bool, true, ""},
    {"RememberAudioSelections", Kind::Bool, true, ""},
    {"RememberSubtitleSelections", Kind::Bool, true, ""},
    {"EnableNextEpisodeAutoPlay", Kind::Bool, true, ""},
    {"DisplayMissingEpisodes", Kind::Bool, false, ""},
    {"HidePlayedInLatest", Kind::Bool, true, ""},
    {"OrderedViews", Kind::Ids, false, ""},
    {"MyMediaExcludes", Kind::Ids, false, ""},
    {"LatestItemsExcludes", Kind::Ids, false, ""},
};

constexpr int HomeSectionSlots = 10;

QString keyOf(UserConfigurationModel::Field field)
{
    return QString::fromLatin1(kFields[field].key);
}

bool fitsBodyCap(const QJsonObject &configuration)
{
    return QJsonDocument(configuration).toJson(QJsonDocument::Compact).size()
           <= UserConfigurationModel::MaxConfigurationBytes;
}

int homeSectionOf(const QString &value)
{
    const QString kind = value.toLower();
    if (kind == QLatin1String("smalllibrarytiles") || kind == QLatin1String("librarybuttons"))
        return UserConfigurationModel::Libraries;
    if (kind == QLatin1String("resume"))
        return UserConfigurationModel::ContinueWatching;
    if (kind == QLatin1String("nextup"))
        return UserConfigurationModel::NextUp;
    if (kind == QLatin1String("latestmedia"))
        return UserConfigurationModel::RecentlyAdded;
    return UserConfigurationModel::Unsupported;
}
} // namespace

UserConfigurationModel::UserConfigurationModel(JellyfinClient *client, QObject *parent)
    : QObject(parent), m_client(client), m_cultures(new CultureOptionsModel(this))
{
    for (int i = 0; i < FieldCount; ++i)
        m_draft[size_t(i)] = defaultValue(Field(i));
    if (!m_client)
        return;
    connect(m_client, &JellyfinClient::currentUserConfigurationReady, this,
            &UserConfigurationModel::onUserReady);
    connect(m_client, &JellyfinClient::currentUserConfigurationFailed, this,
            [this] { onUserFailed(); });
    connect(m_client, &JellyfinClient::userConfigurationUpdated, this,
            &UserConfigurationModel::onUpdated);
    connect(m_client, &JellyfinClient::userConfigurationUpdateFailed, this,
            [this] { onUpdateFailed(); });
    connect(m_client, &JellyfinClient::culturesReady, this,
            &UserConfigurationModel::onCulturesReady);
    connect(m_client, &JellyfinClient::culturesFailed, this, [this] { m_culturesPending = false; });
    connect(m_client, &JellyfinClient::displayHomeSectionsReady, this,
            &UserConfigurationModel::onHomeSectionsReady);
    connect(m_client, &JellyfinClient::displayHomeSectionsFailed, this, [this] {
        if (!m_homePending)
            return;
        m_homePending = false;
        setHomeStatus(HomeSectionsUnavailable);
    });
    // A 401, a disconnect, a server switch and a sign-out all end with this signal
    connect(m_client, &JellyfinClient::credentialsCleared, this, [this] { clear(); });
}

UserConfigurationModel::~UserConfigurationModel()
{
    // The client may emit while it is torn down; nothing here must run then
    if (m_client)
        m_client->disconnect(this);
}

void UserConfigurationModel::load()
{
    if (m_step != Step::None)
        return;
    ++m_generation;
    m_failure = NoFailure;
    m_hasBaseline = false;
    m_baselineRaw = {};
    m_latestRaw = {};
    m_pending = {};
    m_postSent = false;
    m_saving.clear();
    m_conflictFields.clear();
    m_malformed.reset();
    for (int i = 0; i < FieldCount; ++i)
        m_draft[size_t(i)] = defaultValue(Field(i));
    m_homeSections.clear();
    m_homePending = false;
    setHomeStatus(HomeSectionsIdle);
    m_userId = m_client ? m_client->userId() : QString();
    if (m_userId.isEmpty()) {
        fail(LoadFailed);
        return;
    }
    setState(Loading, Step::LoadFetch);
    if (!m_client->fetchCurrentUserConfiguration()) {
        fail(LoadFailed);
        return;
    }
    // The catalog and the preview never block the editor: their failure only leaves them unavailable
    if (!m_cultures->available() && !m_culturesPending)
        m_culturesPending = m_client->fetchCultures();
    m_homePending = m_client->fetchDisplayHomeSections();
    setHomeStatus(m_homePending ? HomeSectionsLoading : HomeSectionsUnavailable);
}

void UserConfigurationModel::save()
{
    if (m_state == Ready && dirty())
        startSave();
}

void UserConfigurationModel::retry()
{
    if (m_state != Failed)
        return;
    if (m_failure == SaveFailed && m_hasBaseline)
        startSave();
    else
        load();
}

void UserConfigurationModel::discard()
{
    // The baseline is unreliable after a failure that is not a plain write failure, after a conflict (the server holds other values) and once a write was sent without a confirmed outcome
    if ((m_state == Failed && m_failure != SaveFailed) || m_state == Conflict || m_postSent) {
        load();
        return;
    }
    discardDraft();
}

void UserConfigurationModel::discardDraft()
{
    if (m_postSent || !m_hasBaseline || m_step != Step::None)
        return;
    adoptBaseline(m_baselineRaw);
    m_failure = NoFailure;
    setState(Ready, Step::None);
}

void UserConfigurationModel::keepEditing()
{
    if (!m_hasBaseline || !(m_state == Conflict || (m_state == Failed && m_failure == SaveFailed)))
        return;
    if (m_state == Conflict)
        rebaseOntoLatest();
    m_conflictFields.clear();
    m_failure = NoFailure;
    setState(Ready, Step::None);
}

void UserConfigurationModel::clear()
{
    ++m_generation;
    m_userId.clear();
    m_hasBaseline = false;
    m_baselineRaw = {};
    m_latestRaw = {};
    m_pending = {};
    m_postSent = false;
    m_saving.clear();
    m_conflictFields.clear();
    m_malformed.reset();
    for (int i = 0; i < FieldCount; ++i)
        m_draft[size_t(i)] = defaultValue(Field(i));
    m_cultures->clear();
    m_culturesPending = false;
    m_homePending = false;
    m_homeSections.clear();
    const bool hadLibraries = !m_libraries.isEmpty();
    m_libraries.clear();
    m_libraryRow.clear();
    m_failure = NoFailure;
    setHomeStatus(HomeSectionsIdle);
    setState(Idle, Step::None);
    if (hadLibraries)
        Q_EMIT librariesChanged();
}

void UserConfigurationModel::startSave()
{
    m_conflictFields.clear();
    m_failure = NoFailure;
    m_saving = dirtyFieldList();
    if (m_saving.isEmpty()) {
        setState(Ready, Step::None);
        return;
    }
    setState(Saving, Step::PrefetchFetch);
    if (!m_client || !m_client->fetchCurrentUserConfiguration())
        fail(SaveFailed);
}

void UserConfigurationModel::onUserReady(const QJsonObject &user)
{
    const Step step = m_step;
    if (step != Step::LoadFetch && step != Step::PrefetchFetch && step != Step::VerifyFetch)
        return;
    const QJsonValue configuration = user.value(QStringLiteral("Configuration"));
    const bool valid =
        userMatches(user) && configuration.isObject() && fitsBodyCap(configuration.toObject());
    if (!valid) {
        fail(step == Step::VerifyFetch ? NotVerified : InvalidProfile);
        return;
    }
    const QJsonObject latest = configuration.toObject();
    if (step == Step::LoadFetch) {
        adoptBaseline(latest);
        m_failure = NoFailure;
        setState(Ready, Step::None);
        return;
    }
    if (step == Step::VerifyFetch) {
        m_latestRaw = latest;
        for (const Field field : std::as_const(m_saving)) {
            if (project(latest, field) != m_draft[field]) {
                fail(NotVerified);
                return;
            }
        }
        adoptBaseline(latest);
        m_saving.clear();
        m_pending = {};
        m_failure = NoFailure;
        setState(Ready, Step::None);
        Q_EMIT saved();
        Q_EMIT configurationApplied(snapshot());
        return;
    }

    // The fresh read that precedes the write: a field the user edited and somebody else changed too blocks it
    m_latestRaw = latest;
    QStringList conflicts;
    for (const Field field : std::as_const(m_saving)) {
        const QVariant remote = project(latest, field);
        if (!remote.isValid()) {
            fail(InvalidProfile);
            return;
        }
        if (remote != project(m_baselineRaw, field) && remote != m_draft[field])
            conflicts.append(keyOf(field));
    }
    if (!conflicts.isEmpty()) {
        m_conflictFields = conflicts;
        setState(Conflict, Step::None);
        return;
    }
    m_pending = mergedOver(latest);
    if (!fitsBodyCap(m_pending)) {
        fail(SaveFailed);
        return;
    }
    setState(Saving, Step::Posting);
    if (!m_client->updateCurrentUserConfiguration(m_pending)) {
        fail(SaveFailed);
        return;
    }
    m_postSent = true;
}

void UserConfigurationModel::onUserFailed()
{
    switch (m_step) {
    case Step::LoadFetch:
        fail(LoadFailed);
        break;
    case Step::PrefetchFetch:
        fail(SaveFailed);
        break;
    case Step::VerifyFetch:
        fail(NotVerified);
        break;
    default:
        break;
    }
}

void UserConfigurationModel::onUpdated()
{
    if (m_step != Step::Posting)
        return;
    setState(Verifying, Step::VerifyFetch);
    if (!m_client || !m_client->fetchCurrentUserConfiguration())
        fail(NotVerified);
}

void UserConfigurationModel::onUpdateFailed()
{
    if (m_step == Step::Posting)
        fail(SaveFailed);
}

void UserConfigurationModel::onCulturesReady(const QJsonArray &cultures)
{
    if (!m_culturesPending)
        return;
    m_culturesPending = false;
    m_cultures->setCultures(cultures);
}

void UserConfigurationModel::onHomeSectionsReady(const QJsonObject &customPrefs)
{
    if (!m_homePending)
        return;
    m_homePending = false;
    m_homeSections.clear();
    for (int slot = 0; slot < HomeSectionSlots; ++slot) {
        const QJsonValue value = customPrefs.value(QStringLiteral("homesection%1").arg(slot));
        // An empty slot and the explicit "none" show nothing; any other value this client does not know is shown as unsupported
        if (!value.isString() || value.toString().isEmpty()
            || value.toString().compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
            continue;
        m_homeSections.append(homeSectionOf(value.toString()));
    }
    setHomeStatus(HomeSectionsReady);
}

void UserConfigurationModel::setState(State state, Step step)
{
    m_state = state;
    m_step = step;
    Q_EMIT stateChanged();
    Q_EMIT draftChanged();
}

void UserConfigurationModel::fail(Failure failure)
{
    m_failure = failure;
    setState(Failed, Step::None);
}

void UserConfigurationModel::setHomeStatus(HomeSectionsStatus status)
{
    m_homeStatus = status;
    Q_EMIT homeSectionsChanged();
}

void UserConfigurationModel::adoptBaseline(const QJsonObject &configuration)
{
    m_baselineRaw = configuration;
    m_latestRaw = configuration;
    m_postSent = false;
    m_hasBaseline = true;
    m_conflictFields.clear();
    for (int i = 0; i < FieldCount; ++i) {
        const QVariant value = project(configuration, Field(i));
        m_malformed.set(size_t(i), !value.isValid());
        m_draft[size_t(i)] = value.isValid() ? value : defaultValue(Field(i));
    }
}

// After a conflict: the fresh read becomes the baseline, so the next save knowingly overwrites it. A field the user edited keeps its draft, every other field follows the server
void UserConfigurationModel::rebaseOntoLatest()
{
    const QList<Field> edited = dirtyFieldList();
    m_baselineRaw = m_latestRaw;
    m_postSent = false;
    for (int i = 0; i < FieldCount; ++i) {
        const QVariant value = project(m_baselineRaw, Field(i));
        m_malformed.set(size_t(i), !value.isValid());
        if (!edited.contains(Field(i)))
            m_draft[size_t(i)] = value.isValid() ? value : defaultValue(Field(i));
    }
}

bool UserConfigurationModel::userMatches(const QJsonObject &user) const
{
    const QString id = user.value(QStringLiteral("Id")).toString();
    return !m_userId.isEmpty() && id.compare(m_userId, Qt::CaseInsensitive) == 0 && m_client
           && m_client->userId().compare(m_userId, Qt::CaseInsensitive) == 0;
}

QVariant UserConfigurationModel::defaultValue(Field field) const
{
    const FieldSpec &spec = kFields[field];
    switch (spec.kind) {
    case Kind::Bool:
        return spec.defaultBool;
    case Kind::Text:
        return QString::fromLatin1(spec.defaultText);
    case Kind::Ids:
        return QStringList();
    }
    return {};
}

// The typed value of a field in `configuration`; invalid when the stored value has a shape the editor cannot represent. A missing or null value reads as the field's default, so it only becomes a write when the user changes it
QVariant UserConfigurationModel::project(const QJsonObject &configuration, Field field) const
{
    const FieldSpec &spec = kFields[field];
    const QJsonValue value = configuration.value(keyOf(field));
    const bool absent = value.isUndefined() || value.isNull();
    switch (spec.kind) {
    case Kind::Bool:
        if (absent)
            return spec.defaultBool;
        return value.isBool() ? QVariant(value.toBool()) : QVariant();
    case Kind::Text:
        if (absent)
            return QString::fromLatin1(spec.defaultText);
        return value.isString() ? QVariant(value.toString()) : QVariant();
    case Kind::Ids:
        break;
    }
    if (!absent && !value.isArray())
        return {};
    QSet<QString> stored;
    for (const QJsonValue &entry : value.toArray()) {
        if (entry.isString())
            stored.insert(entry.toString().toLower());
    }
    QStringList ids;
    if (field == OrderedViews) {
        // The stored order of the visible libraries first, then the ones the array does not mention in server order
        QSet<QString> seen;
        for (const QJsonValue &entry : value.toArray()) {
            const QString id = visibleId(entry.toString());
            if (entry.isString() && !id.isEmpty() && !seen.contains(id.toLower())) {
                seen.insert(id.toLower());
                ids.append(id);
            }
        }
        for (const LibraryOption &library : m_libraries) {
            if (!seen.contains(library.id.toLower()))
                ids.append(library.id);
        }
    } else {
        for (const LibraryOption &library : m_libraries) {
            if (stored.contains(library.id.toLower()))
                ids.append(library.id);
        }
    }
    return ids;
}

bool UserConfigurationModel::isDirty(Field field) const
{
    return m_hasBaseline && !m_malformed.test(size_t(field))
           && m_draft[field] != project(m_baselineRaw, field);
}

QList<UserConfigurationModel::Field> UserConfigurationModel::dirtyFieldList() const
{
    QList<Field> fields;
    for (int i = 0; i < FieldCount; ++i) {
        if (isDirty(Field(i)))
            fields.append(Field(i));
    }
    return fields;
}

QStringList UserConfigurationModel::dirtyFields() const
{
    QStringList keys;
    for (const Field field : dirtyFieldList())
        keys.append(keyOf(field));
    return keys;
}

QStringList UserConfigurationModel::malformedFields() const
{
    QStringList keys;
    for (int i = 0; i < FieldCount; ++i) {
        if (m_malformed.test(size_t(i)))
            keys.append(keyOf(Field(i)));
    }
    return keys;
}

QStringList UserConfigurationModel::subtitleModes() const
{
    return {QStringLiteral("Default"), QStringLiteral("Always"), QStringLiteral("OnlyForced"),
            QStringLiteral("None"), QStringLiteral("Smart")};
}

// The draft laid over the fresh object: only the dirty fields change, everything else is the server's own value
QJsonObject UserConfigurationModel::mergedOver(const QJsonObject &latest) const
{
    QJsonObject merged = latest;
    for (const Field field : m_saving) {
        const QString key = keyOf(field);
        switch (kFields[field].kind) {
        case Kind::Bool:
            merged.insert(key, m_draft[field].toBool());
            break;
        case Kind::Text:
            merged.insert(key, m_draft[field].toString());
            break;
        case Kind::Ids:
            merged.insert(
                key, mergedIds(field, latest.value(key).toArray(), m_draft[field].toStringList()));
            break;
        }
    }
    return merged;
}

// The stored array with the visible part replaced by the draft: what is not a visible library id (hidden libraries, odd entries) stays where it is. OrderedViews keeps those as anchors and refills the visible slots in the draft order; a visible id the array lacks goes last, once
QJsonArray UserConfigurationModel::mergedIds(Field field, const QJsonArray &latest,
                                             const QStringList &draft) const
{
    QJsonArray merged;
    if (field == OrderedViews) {
        qsizetype next = 0;
        for (const QJsonValue &entry : latest) {
            if (!entry.isString() || visibleId(entry.toString()).isEmpty())
                merged.append(entry);
            else if (next < draft.size())
                merged.append(draft.at(next++));
        }
        for (; next < draft.size(); ++next)
            merged.append(draft.at(next));
        return merged;
    }
    QSet<QString> wanted;
    for (const QString &id : draft)
        wanted.insert(id.toLower());
    QSet<QString> present;
    for (const QJsonValue &entry : latest) {
        if (!entry.isString() || visibleId(entry.toString()).isEmpty()) {
            merged.append(entry);
            continue;
        }
        present.insert(entry.toString().toLower());
        if (wanted.contains(entry.toString().toLower()))
            merged.append(entry);
    }
    for (const QString &id : draft) {
        if (!present.contains(id.toLower()))
            merged.append(id);
    }
    return merged;
}

QString UserConfigurationModel::visibleId(const QString &id) const
{
    const int row = m_libraryRow.value(id.toLower(), -1);
    return row < 0 ? QString() : m_libraries.at(row).id;
}

// `ids` reduced to visible libraries, without repeats: OrderedViews keeps the given order and completes it with the missing libraries, the exclusion lists follow the server order
QStringList UserConfigurationModel::normalizedIds(Field field, const QStringList &ids) const
{
    QStringList result;
    QSet<QString> seen;
    for (const QString &id : ids) {
        const QString visible = visibleId(id);
        if (!visible.isEmpty() && !seen.contains(visible.toLower())) {
            seen.insert(visible.toLower());
            result.append(visible);
        }
    }
    if (field == OrderedViews) {
        for (const LibraryOption &library : m_libraries) {
            if (!seen.contains(library.id.toLower()))
                result.append(library.id);
        }
        return result;
    }
    QStringList inServerOrder;
    for (const LibraryOption &library : m_libraries) {
        if (seen.contains(library.id.toLower()))
            inServerOrder.append(library.id);
    }
    return inServerOrder;
}

bool UserConfigurationModel::isEditable(Field field) const
{
    return editable() && !m_malformed.test(size_t(field));
}

void UserConfigurationModel::assign(Field field, const QVariant &value)
{
    if (!isEditable(field) || m_draft[field] == value)
        return;
    m_draft[field] = value;
    Q_EMIT draftChanged();
}

void UserConfigurationModel::setText(Field field, const QString &value)
{
    if (!isEditable(field) || value.size() > MaxTextChars)
        return;
    // What the server stored is always a valid choice; anything new must be one the editor offers
    const bool stored = value == project(m_baselineRaw, field).toString();
    bool offered = stored;
    if (field == SubtitleMode)
        offered = offered || subtitleModes().contains(value);
    else
        offered = offered || value.isEmpty() || m_cultures->contains(value);
    if (offered)
        assign(field, value);
}

void UserConfigurationModel::setIds(Field field, const QStringList &ids)
{
    assign(field, normalizedIds(field, ids));
}

void UserConfigurationModel::moveOrderedView(int from, int to)
{
    QStringList order = m_draft[OrderedViews].toStringList();
    if (from < 0 || to < 0 || from >= order.size() || to >= order.size() || from == to)
        return;
    order.move(from, to);
    assign(OrderedViews, order);
}

void UserConfigurationModel::toggleExcluded(Field field, const QString &libraryId, bool excluded)
{
    const QString id = visibleId(libraryId);
    if (id.isEmpty())
        return;
    QStringList ids = m_draft[field].toStringList();
    ids.removeAll(id);
    if (excluded)
        ids.append(id);
    setIds(field, ids);
}

void UserConfigurationModel::setMyMediaExcluded(const QString &libraryId, bool excluded)
{
    toggleExcluded(MyMediaExcludes, libraryId, excluded);
}

void UserConfigurationModel::setLatestExcluded(const QString &libraryId, bool excluded)
{
    toggleExcluded(LatestItemsExcludes, libraryId, excluded);
}

void UserConfigurationModel::setLibraries(const QList<LibraryOption> &libraries)
{
    QList<LibraryOption> accepted;
    QHash<QString, int> rows;
    for (const LibraryOption &library : libraries) {
        if (accepted.size() == MaxLibraries)
            break;
        const QString name = FilterOptionsModel::cappedName(library.name);
        if (!isValidJellyfinId(library.id) || name.isEmpty() || rows.contains(library.id.toLower()))
            continue;
        rows.insert(library.id.toLower(), int(accepted.size()));
        accepted.append({library.id, name});
    }
    // The dirty lists are judged against the libraries they were edited with, before the swap
    QList<Field> editedLists;
    for (const Field field : {OrderedViews, MyMediaExcludes, LatestItemsExcludes}) {
        if (isDirty(field))
            editedLists.append(field);
    }
    m_libraries = accepted;
    m_libraryRow = rows;
    if (m_hasBaseline) {
        for (const Field field : {OrderedViews, MyMediaExcludes, LatestItemsExcludes}) {
            if (m_malformed.test(size_t(field)))
                continue;
            m_draft[field] = editedLists.contains(field)
                                 ? QVariant(normalizedIds(field, m_draft[field].toStringList()))
                                 : project(m_baselineRaw, field);
        }
    }
    Q_EMIT librariesChanged();
    Q_EMIT stateChanged();
    Q_EMIT draftChanged();
}

QVariantList UserConfigurationModel::libraries() const
{
    QVariantList list;
    for (const LibraryOption &library : m_libraries)
        list.append(QVariantMap{{QStringLiteral("id"), library.id},
                                {QStringLiteral("name"), library.name}});
    return list;
}

int UserConfigurationModel::unavailableLibraryCount() const
{
    QSet<QString> unavailable;
    for (const Field field : {OrderedViews, MyMediaExcludes, LatestItemsExcludes}) {
        const QJsonValue stored = m_baselineRaw.value(keyOf(field));
        for (const QJsonValue &entry : stored.toArray()) {
            if (entry.isString() && !m_libraryRow.contains(entry.toString().toLower()))
                unavailable.insert(entry.toString().toLower());
        }
    }
    return int(unavailable.size());
}

QVariantList UserConfigurationModel::homeSections() const
{
    QVariantList sections;
    for (const int section : m_homeSections)
        sections.append(section);
    return sections;
}

UserPreferencesSnapshot UserConfigurationModel::snapshot() const
{
    const auto text = [this](Field field) {
        const QVariant value = project(m_baselineRaw, field);
        return value.isValid() ? value.toString() : defaultValue(field).toString();
    };
    const auto flag = [this](Field field) {
        const QVariant value = project(m_baselineRaw, field);
        return value.isValid() ? value.toBool() : defaultValue(field).toBool();
    };
    const auto ids = [this](Field field) {
        QStringList list;
        for (const QJsonValue &entry : m_baselineRaw.value(keyOf(field)).toArray()) {
            if (entry.isString())
                list.append(entry.toString());
        }
        return list;
    };
    UserPreferencesSnapshot snapshot;
    snapshot.audioLanguage = text(AudioLanguage);
    snapshot.subtitleLanguage = text(SubtitleLanguage);
    snapshot.subtitleMode = text(SubtitleMode);
    snapshot.playDefaultAudioTrack = flag(PlayDefaultAudioTrack);
    snapshot.rememberAudioSelections = flag(RememberAudioSelections);
    snapshot.rememberSubtitleSelections = flag(RememberSubtitleSelections);
    snapshot.enableNextEpisodeAutoPlay = flag(EnableNextEpisodeAutoPlay);
    snapshot.displayMissingEpisodes = flag(DisplayMissingEpisodes);
    snapshot.hidePlayedInLatest = flag(HidePlayedInLatest);
    snapshot.orderedViews = ids(OrderedViews);
    snapshot.myMediaExcludes = ids(MyMediaExcludes);
    snapshot.latestItemsExcludes = ids(LatestItemsExcludes);
    return snapshot;
}
} // namespace Kinefin
