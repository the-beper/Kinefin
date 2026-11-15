// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

namespace Kinefin {
class PlayerController;

// The MPRIS bus name of a profile (`org.mpris.MediaPlayer2.Kinefin.profile_<id>`, the `.` and `-` of the id as `_`), empty for an id the profiles refuse
QString mprisServiceName(const QString &profileId);
// The MPRIS track id of a Jellyfin item: its hexadecimal id without dashes under the app's own path, NoTrack for none or an id that is not hexadecimal
QDBusObjectPath mprisTrackId(const QString &itemId);
// Server text made fit for the desktop: control characters out, at most 512 characters
QString mprisText(const QString &text);

class MprisRootAdaptor;
class MprisPlayerAdaptor;

// MPRIS (org.mpris.MediaPlayer2 and its Player interface) for the player of this profile: it reads PlayerController and calls its slots, the player knows nothing of it. Changes are sent in one PropertiesChanged per burst, Position never, a seek as Seeked
class Mpris final : public QObject {
    Q_OBJECT
  public:
    static constexpr auto ObjectPath = "/org/mpris/MediaPlayer2";

    Mpris(PlayerController &player, const QString &profileId, const QDBusConnection &bus,
          QObject *parent = nullptr);
    ~Mpris() override;

    // Exports the object and takes the bus name; false, with one warning, when the profile id cannot form a name, there is no bus or the name is taken
    bool registerOnBus();
    QString serviceName() const { return m_service; }
    // The local file of the current item's artwork, shown once it is written; ignored for another item
    void setArtwork(const QString &itemId, const QUrl &file);

    // The values of the Player interface, read by the adaptor and diffed for PropertiesChanged
    QVariantMap playerProperties() const;
    QVariantMap metadata() const;
    PlayerController &player() const { return m_player; }

  Q_SIGNALS:
    void raiseRequested();
    void quitRequested();

  private:
    void scheduleChange();
    void sendChanges();

    PlayerController &m_player;
    QDBusConnection m_bus;
    QString m_service;
    bool m_registered = false;
    MprisRootAdaptor *m_root = nullptr;
    MprisPlayerAdaptor *m_playerAdaptor = nullptr;
    QTimer m_coalesce;
    QVariantMap m_sent;
    QString m_artItem;
    QUrl m_artFile;
};
} // namespace Kinefin
