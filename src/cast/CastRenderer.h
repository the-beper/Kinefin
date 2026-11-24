// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <optional>

namespace Kinefin {

// A text track the renderer fetches itself: a WebVTT URL under the server's base, with the language the Cast protocol requires
struct CastTextTrack {
    QUrl url;
    QString language;
    QString name;
};

// What one load asks of a renderer. `url` is an http or https URL under `serverBase`; a direct play seeks to `startMs` by itself when the renderer can, a transcode URL already starts there and its positions count from it. `subtitle` is the second line of the title (an episode line), `imageUrl` and `track` are for renderers that show them
struct CastLoad {
    QUrl url;
    QString mime;
    QString title;
    QString subtitle;
    qint64 startMs = 0;
    qint64 durationMs = 0;
    QUrl serverBase;
    bool direct = true;
    QUrl imageUrl;
    CastTextTrack track;
};

// A device the player hands a video to: the DLNA renderer and the Google Cast sender behind one interface, so that the controller and the player see one set of states, commands and signals
class CastRenderer : public QObject {
    Q_OBJECT

  public:
    enum class Transport {
        Stopped,
        Playing,
        Paused,
        Transitioning,
        NoMedia,
    };
    Q_ENUM(Transport)

    enum class Failure {
        // The device would not play the stream it was given: a fault on the load, no PLAYING in time, a stop before it ever played. Another stream may still play
        Refused,
        // No answer to the load or to the polls in a row
        NotResponding,
        // The server asks the device for a credential: nothing reached the device
        ServerRefused,
        // The device could not prove that it is the kind of device it claims to be: nothing was sent to it
        Untrusted,
    };
    Q_ENUM(Failure)

    explicit CastRenderer(QObject *parent = nullptr) : QObject(parent) {}

    // The id of the target in the picker: the UDN of a DLNA renderer, the id of a Cast device
    virtual QString id() const = 0;

    // Loads and plays; the state comes through stateChanged
    virtual bool load(const CastLoad &request) = 0;
    // The same transcode again from `url`, which starts at `startMs`: how a transcode seeks. `subtitleUrl` is the text track at that start, empty for none or for a renderer that takes no track
    virtual bool reload(const QUrl &url, qint64 startMs, const QUrl &subtitleUrl = {}) = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    // A direct play only: a transcode seeks by loading the same URL at another start
    virtual bool seek(qint64 positionMs) = 0;
    virtual bool stop() = 0;
    virtual bool setVolume(int volume) = 0;
    virtual bool setMuted(bool muted) = 0;
    // The item's runtime once the server named it: the device's own duration of a transcode is not trusted
    virtual void setDurationMs(qint64 durationMs) = 0;

    virtual bool hasVolume() const = 0;
    virtual bool canPause() const = 0;
    virtual bool canSeek() const = 0;
    virtual qint64 positionMs() const = 0;
    virtual qint64 durationMs() const = 0;
    virtual std::optional<Transport> transport() const = 0;
    // -1 until the device told its volume
    virtual int volume() const = 0;
    virtual bool muted() const = 0;

  Q_SIGNALS:
    void stateChanged(Kinefin::CastRenderer::Transport state, qint64 positionMs, qint64 durationMs);
    void positionChanged(qint64 positionMs, qint64 durationMs);
    void volumeChanged(int volume, bool muted);
    // The device reported the end of the item
    void ended();
    // The device stopped on its own, or another sender took it
    void stopped();
    void failed(Kinefin::CastRenderer::Failure failure);
    // The text track a transcode was loaded without can be fetched at once now: a reload brings it in
    void textTrackReady();
};

} // namespace Kinefin
