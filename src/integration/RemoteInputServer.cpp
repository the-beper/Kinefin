// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/RemoteInputServer.h"

#include "integration/Mpris.h"
#include "settings/ProfilePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QWindow>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace Kinefin {
using namespace Qt::StringLiterals;

namespace {
// sun_path holds 108 bytes, one of them the terminator
constexpr qsizetype kMaxPathBytes = 107;
constexpr qsizetype kMaxFieldLength = 64;
constexpr qsizetype kMaxFrameBytes = 4 * 1024;
constexpr qint64 kReadBufferBytes = 8 * 1024;
constexpr int kMaxClients = 4;
constexpr int kMaxFramesPerSecond = 50;

struct NavigationKey {
    QLatin1StringView code;
    Qt::Key key;
    QLatin1StringView text;
};

constexpr NavigationKey kNavigationKeys[] = {
    {"up"_L1, Qt::Key_Up, ""_L1},           {"down"_L1, Qt::Key_Down, ""_L1},
    {"left"_L1, Qt::Key_Left, ""_L1},       {"right"_L1, Qt::Key_Right, ""_L1},
    {"select"_L1, Qt::Key_Return, "\r"_L1},
};

constexpr QLatin1StringView kCommands[] = {
    "home"_L1,
    "playpause"_L1,
    "play"_L1,
    "pause"_L1,
    "stop"_L1,
    "next"_L1,
    "previous"_L1,
    "fastforward"_L1,
    "rewind"_L1,
    "volumeup"_L1,
    "volumedown"_L1,
    "togglemute"_L1,
    "togglefullscreen"_L1,
};

struct HostCommand {
    QLatin1StringView code;
    QLatin1StringView action;
};

constexpr HostCommand kHostCommands[] = {
    {"host:fullscreen"_L1, "togglefullscreen"_L1},
};

bool peerIsCurrentUser(const QLocalSocket *socket)
{
    ucred credentials{};
    socklen_t size = sizeof credentials;
    return ::getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED,
                        &credentials, &size)
               == 0
           && credentials.uid == ::getuid();
}

QByteArray utf8ForLog(const QString &text)
{
    return mprisText(text).toUtf8();
}

// A real folder (not a link) of this user with none of `others` mode bits set
bool ownDirectory(const QString &path, mode_t others)
{
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0)
        return false;
    return S_ISDIR(info.st_mode) && info.st_uid == ::getuid() && (info.st_mode & others) == 0;
}
} // namespace

QString remoteSocketPath(const QString &profileId)
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!validProfileId(profileId) || !QDir::isAbsolutePath(runtime))
        return {};
    const QString path = QDir::cleanPath(runtime)
                         + QStringLiteral("/app/io.github.the_beper.Kinefin/kinefin-") + profileId;
    return QFile::encodeName(path).size() > kMaxPathBytes ? QString() : path;
}

std::optional<RemoteFrame> parseRemoteFrame(QByteArrayView line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();

    const QJsonValue keycode = object.value("keycode"_L1);
    if (!keycode.isString() || keycode.toString().size() > kMaxFieldLength)
        return std::nullopt;
    RemoteFrame frame;
    frame.keycode = keycode.toString();

    const auto optionalField = [&object](QLatin1StringView name, QString &target) {
        const QJsonValue value = object.value(name);
        if (value.isUndefined())
            return true;
        if (!value.isString() || value.toString().size() > kMaxFieldLength)
            return false;
        target = value.toString();
        return true;
    };
    if (!optionalField("client"_L1, frame.client) || !optionalField("source"_L1, frame.source))
        return std::nullopt;
    return frame;
}

RemoteInputServer::RemoteInputServer(const QString &path, const QString &version, QObject *parent)
    : QObject(parent), m_path(path)
{
    // A QJsonObject sorts its keys, so the version is written first by hand
    m_welcome = QJsonDocument(QJsonObject{{"version"_L1, version}}).toJson(QJsonDocument::Compact);
    m_welcome.chop(1);
    m_welcome += ",\"builddate\":\"\"}\n";
    connect(&m_server, &QLocalServer::newConnection, this, &RemoteInputServer::acceptConnections);
}

RemoteInputServer::~RemoteInputServer()
{
    // The clients die with the server after `m_clients`: their last signals must not reach this half-destroyed object
    for (QLocalSocket *socket : m_clients.keys())
        QObject::disconnect(socket, nullptr, this, nullptr);
    m_server.close();
    if (!m_ownsPath)
        return;
    const QByteArray path = QFile::encodeName(m_path);
    struct stat info{};
    if (::lstat(path.constData(), &info) == 0 && info.st_dev == m_device && info.st_ino == m_inode)
        ::unlink(path.constData());
}

void RemoteInputServer::setWindow(QWindow *window)
{
    m_window = window;
}

bool RemoteInputServer::listen()
{
    if (m_server.isListening())
        return true;
    if (m_path.isEmpty()) {
        qWarning("Remote control is off: there is no usable socket path");
        return false;
    }
    const QString folder = QFileInfo(m_path).absolutePath();
    // The runtime folder above `app/<app-id>` must be this user's and closed to writes by others, or another user could swap the app folder under the socket
    const QString runtime = QFileInfo(QFileInfo(folder).absolutePath()).absolutePath();
    if (!ownDirectory(runtime, 022)) {
        qWarning("Remote control is off: the runtime folder is not this user's own");
        return false;
    }
    QString error;
    if (!ensurePrivateDirectory(folder, &error)) {
        qWarning("Remote control is off: %s", qUtf8Printable(error));
        return false;
    }
    if (!ownDirectory(folder, 077)) {
        qWarning("Remote control is off: the socket folder is not private to this user");
        return false;
    }
    if (!clearStalePath())
        return false;
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(m_path)) {
        qWarning("Remote control is off: %s", qUtf8Printable(m_server.errorString()));
        return false;
    }
    m_ownsPath = true;
    struct stat created{};
    if (::lstat(QFile::encodeName(m_path).constData(), &created) == 0) {
        m_device = created.st_dev;
        m_inode = created.st_ino;
    }
    if (::chmod(QFile::encodeName(m_path).constData(), S_IRUSR | S_IWUSR) != 0) {
        qWarning("Remote control is off: the socket cannot be made private (%s)",
                 std::strerror(errno));
        m_server.close();
        ::unlink(QFile::encodeName(m_path).constData());
        m_ownsPath = false;
        return false;
    }
    return true;
}

bool RemoteInputServer::clearStalePath()
{
    const QByteArray path = QFile::encodeName(m_path);
    struct stat info{};
    if (::lstat(path.constData(), &info) != 0) {
        if (errno == ENOENT)
            return true;
        qWarning("Remote control is off: the socket path cannot be examined (%s)",
                 std::strerror(errno));
        return false;
    }
    if (!S_ISSOCK(info.st_mode)) {
        qWarning("Remote control is off: the socket path holds something that is not a socket");
        return false;
    }
    if (info.st_uid != ::getuid()) {
        qWarning("Remote control is off: the socket path belongs to another user");
        return false;
    }

    sockaddr_un address{};
    if (static_cast<size_t>(path.size()) >= sizeof address.sun_path) {
        qWarning("Remote control is off: the socket path is too long");
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), static_cast<size_t>(path.size()));
    // Non-blocking: a full backlog answers EAGAIN, which only a live server can cause
    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0) {
        qWarning("Remote control is off: the socket path cannot be probed (%s)",
                 std::strerror(errno));
        return false;
    }
    const int result =
        ::connect(probe, reinterpret_cast<const sockaddr *>(&address), sizeof address);
    const int connectError = errno;
    ::close(probe);
    if (result == 0 || connectError == EAGAIN) {
        qWarning("Remote control is off: another Kinefin owns the socket");
        return false;
    }
    if (connectError == ENOENT)
        return true;
    if (connectError != ECONNREFUSED) {
        qWarning("Remote control is off: the socket path cannot be probed (%s)",
                 std::strerror(connectError));
        return false;
    }
    if (::unlink(path.constData()) != 0 && errno != ENOENT) {
        qWarning("Remote control is off: the stale socket cannot be removed (%s)",
                 std::strerror(errno));
        return false;
    }
    return true;
}

void RemoteInputServer::acceptConnections()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        if (!peerIsCurrentUser(socket)) {
            dropClient(socket, "the peer is another user");
            continue;
        }
        if (m_clients.size() >= kMaxClients) {
            dropClient(socket, "too many clients");
            continue;
        }
        socket->setReadBufferSize(kReadBufferBytes);
        m_clients.insert(socket, Client{});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readFrom(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { dropClient(socket); });
        socket->write(m_welcome);
        if (socket->bytesAvailable() > 0)
            readFrom(socket);
    }
}

void RemoteInputServer::readFrom(QLocalSocket *socket)
{
    auto client = m_clients.find(socket);
    if (client == m_clients.end())
        return;
    client->buffer += socket->readAll();
    // handleLine runs the QML shell, so the client is looked up again on every turn
    for (;;) {
        client = m_clients.find(socket);
        if (client == m_clients.end())
            return;
        const qsizetype end = client->buffer.indexOf('\n');
        if (end < 0) {
            if (client->buffer.size() > kMaxFrameBytes)
                dropClient(socket, "a frame over 4 KiB");
            return;
        }
        if (end > kMaxFrameBytes) {
            dropClient(socket, "a frame over 4 KiB");
            return;
        }
        const QByteArray line = client->buffer.left(end);
        client->buffer.remove(0, end + 1);
        if (!handleLine(socket, line))
            return;
    }
}

bool RemoteInputServer::handleLine(QLocalSocket *socket, QByteArrayView line)
{
    Client &client = m_clients[socket];
    if (!client.windowStart.isValid() || client.windowStart.hasExpired(1000)) {
        client.windowStart.start();
        client.frames = 0;
    }
    if (++client.frames > kMaxFramesPerSecond) {
        dropClient(socket, "more than 50 frames per second");
        return false;
    }

    const std::optional<RemoteFrame> frame = parseRemoteFrame(line);
    if (!frame) {
        if (!std::exchange(client.warned, true))
            qWarning("Remote control: a malformed frame was dropped, later ones from this client "
                     "are not logged");
        return true;
    }
    // Once per client: a chatty or reconnecting one cannot grow a debug log frame by frame
    if (!std::exchange(client.logged, true))
        qDebug("Remote control: client '%s', source '%s' connected",
               utf8ForLog(frame->client).constData(), utf8ForLog(frame->source).constData());
    if (!dispatch(*frame) && m_clients.contains(socket)
        && !std::exchange(m_clients[socket].warned, true))
        qDebug("Remote control: unknown keycode '%s' ignored, later ones from this client are not "
               "logged",
               utf8ForLog(frame->keycode).constData());
    return m_clients.contains(socket);
}

bool RemoteInputServer::dispatch(const RemoteFrame &frame)
{
    const QString &code = frame.keycode;

    const auto *navigation = std::ranges::find_if(
        kNavigationKeys, [&code](const NavigationKey &entry) { return code == entry.code; });
    if (navigation != std::end(kNavigationKeys)) {
        sendKey(navigation->key, QString(navigation->text));
        return true;
    }
    if (code == "back"_L1) {
        // Escape first, so a popup or a focused control closes itself; the shell's own back runs when nothing took it
        if (!sendKey(Qt::Key_Escape, QStringLiteral("\x1b")))
            Q_EMIT actionRequested(QStringLiteral("back"));
        return true;
    }
    if (std::ranges::any_of(kCommands,
                            [&code](QLatin1StringView command) { return code == command; })) {
        Q_EMIT actionRequested(code);
        return true;
    }
    const auto *host = std::ranges::find_if(
        kHostCommands, [&code](const HostCommand &entry) { return code == entry.code; });
    if (host != std::end(kHostCommands)) {
        Q_EMIT actionRequested(QString(host->action));
        return true;
    }
    return false;
}

bool RemoteInputServer::sendKey(Qt::Key key, const QString &text)
{
    if (!m_window)
        return false;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    // Accepted only by a receiver that took the key
    press.setAccepted(false);
    QCoreApplication::sendEvent(m_window, &press);
    if (m_window) {
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
        QCoreApplication::sendEvent(m_window, &release);
    }
    return press.isAccepted();
}

void RemoteInputServer::dropClient(QLocalSocket *socket, const char *reason)
{
    if (reason)
        qDebug("Remote control: a client was closed: %s", reason);
    m_clients.remove(socket);
    // Only the connections made here: the socket's own internals stay wired
    QObject::disconnect(socket, nullptr, this, nullptr);
    socket->abort();
    socket->deleteLater();
}
} // namespace Kinefin
