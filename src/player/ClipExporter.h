// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>

#include <utility>

struct mpv_handle;

namespace Kinefin {
// One export: a profile file (a download) or a direct play's static URL, the marks and the tracks
// as the player has them, and the subtitle style it shows
struct ClipRequest {
    enum class Format { Video, Gif };
    Format format = Format::Video;
    QUrl source;
    // The Authorization header value, never in the URL: empty for a local file
    QByteArray authorization;
    qint64 startTicks = 0;
    qint64 endTicks = 0;
    // mpv track ids as the player has them, 0 for none
    int audioId = 0;
    int subtitleId = 0;
    int secondarySubtitleId = 0;
    QStringList subtitleFiles;
    QList<std::pair<QString, QVariant>> subtitleStyle;
    bool tlsVerify = true;
    QString tlsCaFile;
    // A cache dump, which starts on a keyframe: it is decoded from its first frame up to the start, since a seek in a transport stream can land on the keyframe after the start and lose a GOP
    bool decodeFromFileStart = false;
};

// A clip or GIF encoded in its own libmpv instance, so playback goes on untouched: every encoding option is set before `mpv_initialize`, the source is read once between the marks, the result is written to `<staging>.part` and renamed on success. A second instance because encoding options are init-only and mpvqt initializes the playback instance at once; never the system ffmpeg, since the access token would appear in its argv
class ClipExporter final : public QObject {
    Q_OBJECT
  public:
    enum class Error { NoEncoder, NoSpace, Source, Encoder, TooLarge };
    Q_ENUM(Error)

    explicit ClipExporter(QObject *parent = nullptr);
    ~ClipExporter() override;

    bool start(const ClipRequest &request, const QString &stagingPath);
    // Stops the instance and removes the part file; nothing is signalled, the caller started it
    void cancel();
    [[nodiscard]] bool running() const { return m_running; }

    // Whether libmpv lists an encoder (read once from a throwaway idle instance)
    static bool hasEncoder(const QString &name);

  Q_SIGNALS:
    void progress(double fraction);
    void finished(const QString &stagingPath);
    void failed(ClipExporter::Error error);

  private:
    static void wakeUp(void *context);
    void processEvents();
    void finish(bool ok, Error error = Error::Encoder);

    mpv_handle *m_handle = nullptr;
    bool m_running = false;
    QString m_staging;
    QString m_part;
    double m_start = 0;
    double m_end = 0;
    bool m_tlsVerify = true;
    QString m_tlsCaFile;
    QByteArray m_authorization;
};
} // namespace Kinefin
