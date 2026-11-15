// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QElapsedTimer>
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QPointer>

#include <QString>
#include <QWindow>
#include <Qt>
#include <optional>
#include <sys/types.h>

class QLocalSocket;

namespace Kinefin {
// `$XDG_RUNTIME_DIR/app/io.github.the_beper.Kinefin/kinefin-<id>`; empty when XDG_RUNTIME_DIR is unset or relative, the id is invalid (validProfileId) or the path does not fit sun_path (107 bytes)
QString remoteSocketPath(const QString &profileId);

struct RemoteFrame {
    QString client;
    QString source;
    QString keycode;
};

// One line without its '\n': a JSON object with a string "keycode" (required) and optional string "client" and "source", each at most 64 characters; anything else is nullopt
std::optional<RemoteFrame> parseRemoteFrame(QByteArrayView line);

// The remote-control socket of one profile: external programs (a remote, a script) send JSON lines with a keycode, the server turns it into a key event for the window or into a command for the QML shell. Everything it receives is untrusted
class RemoteInputServer final : public QObject {
    Q_OBJECT
  public:
    RemoteInputServer(const QString &path, const QString &version, QObject *parent = nullptr);
    ~RemoteInputServer() override;

    // Where navigation keys go (synthesised key events)
    void setWindow(QWindow *window);

    // False, with one warning, when there is no path, the folder cannot be made private, a live server owns the path, something that is not a socket sits there, or listening fails
    bool listen();

  Q_SIGNALS:
    // A command for the QML shell: "back", "home", "playpause", "play", "pause", "stop", "next", "previous", "fastforward", "rewind", "volumeup", "volumedown", "togglemute", "togglefullscreen"
    void actionRequested(const QString &action);

  private:
    struct Client {
        QByteArray buffer;
        QElapsedTimer windowStart;
        int frames = 0;
        bool warned = false;
        bool logged = false;
    };

    bool clearStalePath();
    void acceptConnections();
    void readFrom(QLocalSocket *socket);
    bool handleLine(QLocalSocket *socket, QByteArrayView line);
    // False for a keycode outside the table
    bool dispatch(const RemoteFrame &frame);
    bool sendKey(Qt::Key key, const QString &text = {});
    void dropClient(QLocalSocket *socket, const char *reason = nullptr);

    QString m_path;
    QByteArray m_welcome;
    QLocalServer m_server;
    QPointer<QWindow> m_window;
    QHash<QLocalSocket *, Client> m_clients;
    bool m_ownsPath = false;
    // The socket file this instance made: only that one is removed at exit
    dev_t m_device = 0;
    ino_t m_inode = 0;
};
} // namespace Kinefin
