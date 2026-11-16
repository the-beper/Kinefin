// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "api/SyncPlayProtocol.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>

#include <cmath>

namespace Kinefin::SyncPlay {
namespace {

constexpr int MaxTokenLetters = 32;
constexpr int MaxCombiningRun = 4;

bool isHex(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
}

// A GUID in the 32-digit or the dashed 8-4-4-4-12 form, returned in the 32-digit lower-case form; the all-zero id becomes an empty string when `allowZero`, nullopt otherwise
std::optional<QString> guid(const QJsonValue &value, bool allowZero)
{
    if (!value.isString())
        return std::nullopt;
    const QString text = value.toString();
    QString digits;
    if (text.size() == 32) {
        digits = text;
    } else if (text.size() == 36) {
        for (int i = 0; i < 36; ++i) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash != (text.at(i) == QLatin1Char('-')))
                return std::nullopt;
        }
        digits = QString(text).remove(QLatin1Char('-'));
    } else {
        return std::nullopt;
    }
    for (const QChar c : std::as_const(digits)) {
        if (!isHex(c))
            return std::nullopt;
    }
    digits = digits.toLower();
    if (digits == QString(32, QLatin1Char('0'))) {
        if (!allowZero)
            return std::nullopt;
        return QString();
    }
    return digits;
}

// Remote text for a plain-text label: at most MaxNameChars, not blank, valid UTF-16, no control, format (bidi marks and controls, zero-width, tags), private-use, unassigned, line or paragraph separator code point, and no run of more than MaxCombiningRun combining marks; empty when it does not qualify. The zero-width joiner and non-joiner are the exception, between two characters that show (Persian and Indic names, emoji sequences)
QString displayName(const QJsonValue &value)
{
    if (!value.isString())
        return {};
    const QString text = value.toString();
    if (text.size() > MaxNameChars || text.trimmed().isEmpty())
        return {};
    int marks = 0;
    bool joinerAllowed = false;
    bool endsWithJoiner = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t code = text.at(i).unicode();
        if (QChar::isHighSurrogate(code) && i + 1 < text.size()
            && QChar::isLowSurrogate(text.at(i + 1).unicode())) {
            code = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
            ++i;
        }
        if (code == 0x200c || code == 0x200d) {
            if (!joinerAllowed)
                return {};
            joinerAllowed = false;
            endsWithJoiner = true;
            marks = 0;
            continue;
        }
        joinerAllowed = true;
        endsWithJoiner = false;
        // A lone surrogate keeps its own value and reads as Other_Surrogate
        switch (QChar::category(code)) {
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Other_Surrogate:
        case QChar::Other_PrivateUse:
        case QChar::Other_NotAssigned:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
            return {};
        case QChar::Mark_NonSpacing:
        case QChar::Mark_Enclosing:
            if (++marks > MaxCombiningRun)
                return {};
            break;
        default:
            marks = 0;
        }
    }
    if (endsWithJoiner)
        return {};
    return text;
}

bool letters(const QJsonValue &value)
{
    if (!value.isString())
        return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > MaxTokenLetters)
        return false;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z')))
            return false;
    }
    return true;
}

std::optional<qint64> integral(const QJsonValue &value, qint64 minimum, qint64 maximum)
{
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < double(minimum)
        || number > double(maximum))
        return std::nullopt;
    return qint64(number);
}

std::optional<qint64> ticks(const QJsonValue &value)
{
    return integral(value, 0, MaxPositionTicks);
}

std::optional<qint64> date(const QJsonValue &value)
{
    if (!value.isString())
        return std::nullopt;
    return parseServerDateMs(value.toString());
}

std::optional<GroupState> groupState(const QJsonValue &value)
{
    const QString text = value.toString();
    if (text == QLatin1String("Idle"))
        return GroupState::Idle;
    if (text == QLatin1String("Waiting"))
        return GroupState::Waiting;
    if (text == QLatin1String("Paused"))
        return GroupState::Paused;
    if (text == QLatin1String("Playing"))
        return GroupState::Playing;
    return std::nullopt;
}

std::optional<PlayQueue> playQueue(const QJsonValue &value)
{
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject data = value.toObject();
    PlayQueue queue;
    const QJsonValue reason = data.value(QStringLiteral("Reason"));
    const auto lastUpdate = date(data.value(QStringLiteral("LastUpdate")));
    const QJsonValue playlist = data.value(QStringLiteral("Playlist"));
    const auto start = ticks(data.value(QStringLiteral("StartPositionTicks")));
    const QJsonValue playing = data.value(QStringLiteral("IsPlaying"));
    const QString shuffle = data.value(QStringLiteral("ShuffleMode")).toString();
    const QString repeat = data.value(QStringLiteral("RepeatMode")).toString();
    if (!letters(reason) || !lastUpdate || !playlist.isArray() || !start || !playing.isBool()
        || (shuffle != QLatin1String("Sorted") && shuffle != QLatin1String("Shuffle"))
        || (repeat != QLatin1String("RepeatNone") && repeat != QLatin1String("RepeatAll")
            && repeat != QLatin1String("RepeatOne")))
        return std::nullopt;
    const QJsonArray entries = playlist.toArray();
    if (entries.size() > MaxQueueEntries)
        return std::nullopt;
    for (const QJsonValue &entryValue : entries) {
        const QJsonObject entry = entryValue.toObject();
        const auto itemId = guid(entry.value(QStringLiteral("ItemId")), false);
        const auto playlistItemId = guid(entry.value(QStringLiteral("PlaylistItemId")), false);
        if (!itemId || !playlistItemId)
            return std::nullopt;
        queue.entries.append({*itemId, *playlistItemId});
    }
    const auto index =
        integral(data.value(QStringLiteral("PlayingItemIndex")), -1, entries.size() - 1);
    if (!index)
        return std::nullopt;
    queue.reason = reason.toString();
    queue.lastUpdateMs = *lastUpdate;
    queue.playingIndex = int(*index);
    queue.startPositionTicks = *start;
    queue.isPlaying = playing.toBool();
    queue.shuffleMode = shuffle;
    queue.repeatMode = repeat;
    return queue;
}

// An array of `minimum` to `maximum` GUIDs, none all-zero
bool guids(const QJsonValue &value, qsizetype minimum, qsizetype maximum)
{
    if (!value.isArray())
        return false;
    const QJsonArray items = value.toArray();
    if (items.size() < minimum || items.size() > maximum)
        return false;
    for (const QJsonValue &item : items) {
        if (!guid(item, false))
            return false;
    }
    return true;
}

bool hasExactly(const QJsonObject &body, QStringList keys)
{
    QStringList present = body.keys();
    present.sort();
    keys.sort();
    return present == keys;
}

} // namespace

std::optional<qint64> parseServerDateMs(QStringView text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})"
                       "(?:\\.([0-9]{1,7}))?Z\\z"));
    const QRegularExpressionMatch match = pattern.matchView(text);
    if (!match.hasMatch())
        return std::nullopt;
    const QDate day(match.capturedView(1).toInt(), match.capturedView(2).toInt(),
                    match.capturedView(3).toInt());
    const QTime time(match.capturedView(4).toInt(), match.capturedView(5).toInt(),
                     match.capturedView(6).toInt());
    if (!day.isValid() || !time.isValid())
        return std::nullopt;
    const QString fraction = QString(match.captured(7) + QStringLiteral("000")).left(3);
    return QDateTime(day, time, QTimeZone::UTC).toMSecsSinceEpoch() + fraction.toInt();
}

QString serverDate(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)
               .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz"))
           + QStringLiteral("0000Z");
}

std::optional<Command> parseCommand(const QJsonValue &data)
{
    if (!data.isObject())
        return std::nullopt;
    const QJsonObject object = data.toObject();
    const auto groupId = guid(object.value(QStringLiteral("GroupId")), false);
    const auto playlistItemId = guid(object.value(QStringLiteral("PlaylistItemId")), true);
    const auto when = date(object.value(QStringLiteral("When")));
    const auto emittedAt = date(object.value(QStringLiteral("EmittedAt")));
    if (!groupId || !playlistItemId || !when || !emittedAt)
        return std::nullopt;
    Command command;
    const QString kind = object.value(QStringLiteral("Command")).toString();
    if (kind == QLatin1String("Unpause"))
        command.kind = Command::Kind::Unpause;
    else if (kind == QLatin1String("Pause"))
        command.kind = Command::Kind::Pause;
    else if (kind == QLatin1String("Seek"))
        command.kind = Command::Kind::Seek;
    else if (kind == QLatin1String("Stop"))
        command.kind = Command::Kind::Stop;
    else
        return std::nullopt;
    const QJsonValue position = object.value(QStringLiteral("PositionTicks"));
    if (!position.isUndefined() && !position.isNull()) {
        const auto value = ticks(position);
        if (!value)
            return std::nullopt;
        command.positionTicks = *value;
    } else if (command.kind == Command::Kind::Seek) {
        return std::nullopt;
    }
    command.groupId = *groupId;
    command.playlistItemId = *playlistItemId;
    command.whenMs = *when;
    command.emittedAtMs = *emittedAt;
    return command;
}

std::optional<GroupInfo> parseGroupInfo(const QJsonValue &value)
{
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    const auto groupId = guid(object.value(QStringLiteral("GroupId")), false);
    const auto state = groupState(object.value(QStringLiteral("State")));
    const auto lastUpdated = date(object.value(QStringLiteral("LastUpdatedAt")));
    if (!groupId || !state || !lastUpdated)
        return std::nullopt;
    // The names are only shown: one that breaks a rule is left out, never the group (a dropped GroupJoined would leave the session in a group it never heard of)
    GroupInfo group{
        *groupId, displayName(object.value(QStringLiteral("GroupName"))), *state, {}, *lastUpdated};
    for (const QJsonValue &participant : object.value(QStringLiteral("Participants")).toArray()) {
        if (group.participants.size() == MaxParticipants)
            break;
        const QString userName = displayName(participant);
        if (!userName.isEmpty())
            group.participants.append(userName);
    }
    return group;
}

std::optional<QList<GroupInfo>> parseGroupList(const QJsonValue &value)
{
    if (!value.isArray())
        return std::nullopt;
    QList<GroupInfo> groups;
    for (const QJsonValue &entry : value.toArray()) {
        if (groups.size() == MaxGroups)
            break;
        if (const auto group = parseGroupInfo(entry))
            groups.append(*group);
    }
    return groups;
}

std::optional<GroupUpdate> parseGroupUpdate(const QJsonValue &data)
{
    if (!data.isObject())
        return std::nullopt;
    const QJsonObject object = data.toObject();
    const auto groupId = guid(object.value(QStringLiteral("GroupId")), true);
    if (!groupId)
        return std::nullopt;
    const QString type = object.value(QStringLiteral("Type")).toString();
    const QJsonValue payload = object.value(QStringLiteral("Data"));
    GroupUpdate update;
    update.groupId = *groupId;
    using Type = GroupUpdate::Type;
    static const QList<std::pair<QLatin1String, Type>> refusals{
        {QLatin1String("NotInGroup"), Type::NotInGroup},
        {QLatin1String("GroupDoesNotExist"), Type::GroupDoesNotExist},
        {QLatin1String("CreateGroupDenied"), Type::CreateGroupDenied},
        {QLatin1String("JoinGroupDenied"), Type::JoinGroupDenied},
        {QLatin1String("LibraryAccessDenied"), Type::LibraryAccessDenied}};
    for (const auto &[label, refusal] : refusals) {
        if (type == label) {
            update.type = refusal;
            return update;
        }
    }
    // Every other update belongs to a group
    if (update.groupId.isEmpty())
        return std::nullopt;
    if (type == QLatin1String("GroupJoined")) {
        const auto group = parseGroupInfo(payload);
        if (!group || group->groupId != update.groupId)
            return std::nullopt;
        update.type = Type::GroupJoined;
        update.group = *group;
    } else if (type == QLatin1String("UserJoined") || type == QLatin1String("UserLeft")) {
        update.type = type == QLatin1String("UserJoined") ? Type::UserJoined : Type::UserLeft;
        update.userName = displayName(payload);
    } else if (type == QLatin1String("GroupLeft")) {
        update.type = Type::GroupLeft;
    } else if (type == QLatin1String("StateUpdate")) {
        const QJsonObject change = payload.toObject();
        const auto state = groupState(change.value(QStringLiteral("State")));
        const QJsonValue reason = change.value(QStringLiteral("Reason"));
        if (!state || !letters(reason))
            return std::nullopt;
        update.type = Type::StateUpdate;
        update.state = *state;
        update.reason = reason.toString();
    } else if (type == QLatin1String("PlayQueue")) {
        const auto queue = playQueue(payload);
        if (!queue)
            return std::nullopt;
        update.type = Type::PlayQueue;
        update.queue = *queue;
    } else {
        return std::nullopt;
    }
    return update;
}

std::optional<UtcTime> parseUtcTime(const QJsonValue &value)
{
    const QJsonObject object = value.toObject();
    const auto reception = date(object.value(QStringLiteral("RequestReceptionTime")));
    const auto transmission = date(object.value(QStringLiteral("ResponseTransmissionTime")));
    if (!reception || !transmission || *transmission < *reception)
        return std::nullopt;
    return UtcTime{*reception, *transmission};
}

QString routeName(Route route)
{
    switch (route) {
    case Route::New:
        return QStringLiteral("New");
    case Route::Join:
        return QStringLiteral("Join");
    case Route::Leave:
        return QStringLiteral("Leave");
    case Route::Pause:
        return QStringLiteral("Pause");
    case Route::Unpause:
        return QStringLiteral("Unpause");
    case Route::Stop:
        return QStringLiteral("Stop");
    case Route::Seek:
        return QStringLiteral("Seek");
    case Route::Buffering:
        return QStringLiteral("Buffering");
    case Route::Ready:
        return QStringLiteral("Ready");
    case Route::Ping:
        return QStringLiteral("Ping");
    case Route::SetIgnoreWait:
        return QStringLiteral("SetIgnoreWait");
    case Route::SetNewQueue:
        return QStringLiteral("SetNewQueue");
    case Route::NextItem:
        return QStringLiteral("NextItem");
    case Route::PreviousItem:
        return QStringLiteral("PreviousItem");
    case Route::SetPlaylistItem:
        return QStringLiteral("SetPlaylistItem");
    case Route::Queue:
        return QStringLiteral("Queue");
    case Route::RemoveFromPlaylist:
        return QStringLiteral("RemoveFromPlaylist");
    case Route::MovePlaylistItem:
        return QStringLiteral("MovePlaylistItem");
    case Route::SetRepeatMode:
        return QStringLiteral("SetRepeatMode");
    case Route::SetShuffleMode:
        return QStringLiteral("SetShuffleMode");
    }
    return {};
}

QJsonObject newGroupBody(const QString &groupName)
{
    return {{QStringLiteral("GroupName"), groupName}};
}

QJsonObject joinBody(const QString &groupId)
{
    return {{QStringLiteral("GroupId"), groupId}};
}

QJsonObject seekBody(qint64 positionTicks)
{
    return {{QStringLiteral("PositionTicks"), positionTicks}};
}

QJsonObject readinessBody(qint64 whenMs, qint64 positionTicks, bool isPlaying,
                          const QString &playlistItemId)
{
    return {{QStringLiteral("When"), serverDate(whenMs)},
            {QStringLiteral("PositionTicks"), positionTicks},
            {QStringLiteral("IsPlaying"), isPlaying},
            {QStringLiteral("PlaylistItemId"), playlistItemId}};
}

QJsonObject pingBody(qint64 pingMs)
{
    return {{QStringLiteral("Ping"), pingMs}};
}

QJsonObject ignoreWaitBody(bool ignoreWait)
{
    return {{QStringLiteral("IgnoreWait"), ignoreWait}};
}

QJsonObject newQueueBody(const QStringList &itemIds, int playingPosition, qint64 startPositionTicks)
{
    return {{QStringLiteral("PlayingQueue"), QJsonArray::fromStringList(itemIds)},
            {QStringLiteral("PlayingItemPosition"), playingPosition},
            {QStringLiteral("StartPositionTicks"), startPositionTicks}};
}

QJsonObject playlistItemBody(const QString &playlistItemId)
{
    return {{QStringLiteral("PlaylistItemId"), playlistItemId}};
}

QJsonObject queueBody(const QStringList &itemIds, bool next)
{
    return {{QStringLiteral("ItemIds"), QJsonArray::fromStringList(itemIds)},
            {QStringLiteral("Mode"), next ? QStringLiteral("QueueNext") : QStringLiteral("Queue")}};
}

QJsonObject removeBody(const QStringList &playlistItemIds, bool clearPlaylist,
                       bool clearPlayingItem)
{
    return {{QStringLiteral("PlaylistItemIds"), QJsonArray::fromStringList(playlistItemIds)},
            {QStringLiteral("ClearPlaylist"), clearPlaylist},
            {QStringLiteral("ClearPlayingItem"), clearPlayingItem}};
}

QJsonObject moveBody(const QString &playlistItemId, int newIndex)
{
    return {{QStringLiteral("PlaylistItemId"), playlistItemId},
            {QStringLiteral("NewIndex"), newIndex}};
}

QJsonObject repeatBody(const QString &mode)
{
    return {{QStringLiteral("Mode"), mode}};
}

QJsonObject shuffleBody(bool shuffle)
{
    return {
        {QStringLiteral("Mode"), shuffle ? QStringLiteral("Shuffle") : QStringLiteral("Sorted")}};
}

bool isValidRequest(Route route, const QJsonObject &body)
{
    switch (route) {
    case Route::New:
        return hasExactly(body, {QStringLiteral("GroupName")})
               && !displayName(body.value(QStringLiteral("GroupName"))).isEmpty();
    case Route::Join:
        return hasExactly(body, {QStringLiteral("GroupId")})
               && guid(body.value(QStringLiteral("GroupId")), false);
    case Route::Leave:
    case Route::Pause:
    case Route::Unpause:
    case Route::Stop:
        return body.isEmpty();
    case Route::Seek:
        return hasExactly(body, {QStringLiteral("PositionTicks")})
               && ticks(body.value(QStringLiteral("PositionTicks")));
    case Route::Buffering:
    case Route::Ready:
        return hasExactly(body, {QStringLiteral("When"), QStringLiteral("PositionTicks"),
                                 QStringLiteral("IsPlaying"), QStringLiteral("PlaylistItemId")})
               && date(body.value(QStringLiteral("When")))
               && ticks(body.value(QStringLiteral("PositionTicks")))
               && body.value(QStringLiteral("IsPlaying")).isBool()
               && guid(body.value(QStringLiteral("PlaylistItemId")), false);
    case Route::Ping:
        return hasExactly(body, {QStringLiteral("Ping")})
               && integral(body.value(QStringLiteral("Ping")), 0, MaxPingMs);
    case Route::SetIgnoreWait:
        return hasExactly(body, {QStringLiteral("IgnoreWait")})
               && body.value(QStringLiteral("IgnoreWait")).isBool();
    case Route::SetNewQueue: {
        if (!hasExactly(body,
                        {QStringLiteral("PlayingQueue"), QStringLiteral("PlayingItemPosition"),
                         QStringLiteral("StartPositionTicks")}))
            return false;
        const QJsonArray items = body.value(QStringLiteral("PlayingQueue")).toArray();
        if (items.isEmpty() || items.size() > MaxNewQueueItems)
            return false;
        for (const QJsonValue &item : items) {
            if (!guid(item, false))
                return false;
        }
        return integral(body.value(QStringLiteral("PlayingItemPosition")), 0, items.size() - 1)
               && ticks(body.value(QStringLiteral("StartPositionTicks")));
    }
    case Route::NextItem:
    case Route::PreviousItem:
    case Route::SetPlaylistItem:
        return hasExactly(body, {QStringLiteral("PlaylistItemId")})
               && guid(body.value(QStringLiteral("PlaylistItemId")), false);
    case Route::Queue: {
        const QString mode = body.value(QStringLiteral("Mode")).toString();
        return hasExactly(body, {QStringLiteral("ItemIds"), QStringLiteral("Mode")})
               && guids(body.value(QStringLiteral("ItemIds")), 1, MaxNewQueueItems)
               && (mode == QLatin1String("Queue") || mode == QLatin1String("QueueNext"));
    }
    case Route::RemoveFromPlaylist: {
        const QJsonValue clear = body.value(QStringLiteral("ClearPlaylist"));
        return hasExactly(body, {QStringLiteral("PlaylistItemIds"), QStringLiteral("ClearPlaylist"),
                                 QStringLiteral("ClearPlayingItem")})
               && clear.isBool() && body.value(QStringLiteral("ClearPlayingItem")).isBool()
               && guids(body.value(QStringLiteral("PlaylistItemIds")), clear.toBool() ? 0 : 1,
                        MaxQueueEntries);
    }
    case Route::MovePlaylistItem:
        return hasExactly(body, {QStringLiteral("PlaylistItemId"), QStringLiteral("NewIndex")})
               && guid(body.value(QStringLiteral("PlaylistItemId")), false)
               && integral(body.value(QStringLiteral("NewIndex")), 0, MaxQueueEntries - 1);
    case Route::SetRepeatMode: {
        const QString mode = body.value(QStringLiteral("Mode")).toString();
        return hasExactly(body, {QStringLiteral("Mode")})
               && (mode == QLatin1String("RepeatNone") || mode == QLatin1String("RepeatAll")
                   || mode == QLatin1String("RepeatOne"));
    }
    case Route::SetShuffleMode: {
        const QString mode = body.value(QStringLiteral("Mode")).toString();
        return hasExactly(body, {QStringLiteral("Mode")})
               && (mode == QLatin1String("Sorted") || mode == QLatin1String("Shuffle"));
    }
    }
    return false;
}

} // namespace Kinefin::SyncPlay
