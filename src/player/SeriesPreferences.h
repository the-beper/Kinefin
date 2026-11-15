// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <chrono>
#include <optional>

namespace Kinefin {
// A track the user chose, as the next episode can find it again: mpv's language code (empty when the track had none), whether it was forced, and where it stood among the tracks of its kind
struct TrackChoice {
    QString lang;
    bool forced = false;
    int ordinal = -1;
    bool operator==(const TrackChoice &) const = default;
};

// What the user chose for one series; a field never chosen is empty
struct SeriesChoice {
    std::optional<TrackChoice> audio;
    // Subtitles turned off are a choice of their own
    bool subtitleOff = false;
    std::optional<TrackChoice> subtitle;
    std::optional<double> rate;
    QString aspect;
};

// A track of the playing file as mpv lists it, in mpv's order, for matching a remembered choice
struct TrackFact {
    int id = 0;
    QString lang;
    bool forced = false;
};

// mpv's language code as it is kept: lower case, `[a-z]{2,3}(-[a-z0-9]{1,8})?`, else (`und` included) empty
QString trackLanguage(const QString &code);

// The mpv id of the track that answers a remembered choice: the language with the forced flag, then the language alone, then (no language stored) the same position; 0 when none fits. Languages compare through QLocale (`en` is `eng`)
int pickTrack(const QList<TrackFact> &tracks, const TrackChoice &choice);

// The choices of each server user, one file per owner, `<directory>/<serverId>_<userId>.json` (as the queue files): `{version 1, entries: {seriesId: {audio, subtitle, rate, aspect, seq}}}`, ids, language codes and numbers only. At most 500 series, fewer when their entries are large (the file stays within 64 KiB), the least recently chosen going first; a file over 64 KiB is deleted unread, a newer version is renamed `.broken`, a wrongly typed field is dropped alone and an entry with a bad id entirely. Writes wait 1 s and are flushed on an owner change and on destruction; without a directory the choices live in memory only
class SeriesPreferences final : public QObject {
    Q_OBJECT
  public:
    static constexpr int MaxSeries = 500;
    static constexpr qint64 MaxFileBytes = 64 * 1024;
    static constexpr int FileVersion = 1;

    explicit SeriesPreferences(QObject *parent = nullptr);
    ~SeriesPreferences() override;

    void setDirectory(const QString &directory);
    void setSaveDelay(std::chrono::milliseconds delay);
    // The signed-in user: their file is read on the change, after the previous owner's pending write; invalid ids keep nothing
    void setOwner(const QString &serverId, const QString &userId);

    SeriesChoice choice(const QString &seriesId) const;
    bool contains(const QString &seriesId) const;
    // Each replaces one field, an empty value removing it, and makes the series the most recent
    void setAudio(const QString &seriesId, const std::optional<TrackChoice> &audio);
    void setSubtitle(const QString &seriesId, bool off, const std::optional<TrackChoice> &subtitle);
    void setRate(const QString &seriesId, std::optional<double> rate);
    void setAspect(const QString &seriesId, const QString &aspect);
    void forget(const QString &seriesId);
    void flush();
    // The file of one user, or those of every user of a server, go (from memory too when the owner is among them)
    void forgetUser(const QString &serverId, const QString &userId);
    void forgetServer(const QString &serverId);

  Q_SIGNALS:
    // The choices of the owner changed
    void changed();

  private:
    QString ownerFile(const QString &serverId, const QString &userId) const;
    void load();
    void update(const QString &seriesId, const QString &field, const QJsonValue &value);
    void save();

    QString m_directory;
    QString m_serverId;
    QString m_userId;
    // Series id (lower case) to its fields and `seq`, the order of the last choice
    QJsonObject m_entries;
    qint64 m_seq = 0;
    QTimer m_saveTimer;
};
} // namespace Kinefin
