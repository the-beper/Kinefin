// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/RemoteInputServer.h"

#include "integration/Mpris.h"
#include "integration/SpatialNavigation.h"
#include "settings/ProfilePaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
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
    {"select"_L1, Qt::Key_Return, "\r"_L1}, {"menu"_L1, Qt::Key_Menu, ""_L1},
};

std::optional<NavDirection> directionOf(Qt::Key key)
{
    switch (key) {
    case Qt::Key_Up:
        return NavDirection::Up;
    case Qt::Key_Down:
        return NavDirection::Down;
    case Qt::Key_Left:
        return NavDirection::Left;
    case Qt::Key_Right:
        return NavDirection::Right;
    default:
        return std::nullopt;
    }
}

// An ancestor of the focus that would use a key on its own: an item view that does not hold the focus in its current row, a scroll view or a flickable, a page (a scrollable page forwards its keys to its flickable from there)
bool stopsKey(const QQuickItem *ancestor, const QQuickItem *focus)
{
    if (isItemView(ancestor)) {
        const auto *current = ancestor->property("currentItem").value<QQuickItem *>();
        return !(current && (current == focus || current->isAncestorOf(focus)));
    }
    return ancestor->inherits("QQuickFlickable") || ancestor->inherits("QQuickScrollView")
           || ancestor->inherits("QQuickPage");
}

// Stops the press of a key at the first ancestor of the focus that stopsKey() names, so a list or a page behind a control cannot move its own row or scroll for a key the control did not use. While it lives the key is delivered as usual; a press accepted before the stop was used by the control, one that reaches the stop is held back there
class AncestorKeyGuard final : public QObject {
  public:
    AncestorKeyGuard(QQuickItem *focus, int key) : m_key(key)
    {
        for (QQuickItem *item = focus->parentItem(); item; item = item->parentItem()) {
            if (stopsKey(item, focus)) {
                item->installEventFilter(this);
                m_filtered.append(item);
            }
        }
    }
    ~AncestorKeyGuard() override
    {
        for (const QPointer<QQuickItem> &item : std::as_const(m_filtered)) {
            if (item)
                item->removeEventFilter(this);
        }
    }

    QQuickItem *stoppedAt() const { return m_stoppedAt; }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (m_stoppedAt || event->type() != QEvent::KeyPress
            || static_cast<QKeyEvent *>(event)->key() != m_key)
            return false;
        m_stoppedAt = qobject_cast<QQuickItem *>(watched);
        return true;
    }

  private:
    int m_key;
    QList<QPointer<QQuickItem>> m_filtered;
    QPointer<QQuickItem> m_stoppedAt;
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
    "previoustab"_L1,
    "nexttab"_L1,
    "previouschapter"_L1,
    "nextchapter"_L1,
    "mainmenu"_L1,
    "expandplayer"_L1,
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
    if (!dispatchKeycode(frame->keycode) && m_clients.contains(socket)
        && !std::exchange(m_clients[socket].warned, true))
        qDebug("Remote control: unknown keycode '%s' ignored, later ones from this client are not "
               "logged",
               utf8ForLog(frame->keycode).constData());
    return m_clients.contains(socket);
}

bool RemoteInputServer::dispatchKeycode(const QString &code, bool autoRepeat)
{
    const auto *navigation = std::ranges::find_if(
        kNavigationKeys, [&code](const NavigationKey &entry) { return code == entry.code; });
    if (navigation != std::end(kNavigationKeys))
        return dispatchNavigation(navigation->key, QString(navigation->text), autoRepeat);
    if (code == "back"_L1) {
        // Escape first, so a popup or a focused control closes itself; the shell's own back runs when nothing took it
        if (!sendKey(Qt::Key_Escape, QStringLiteral("\x1b"), autoRepeat))
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

bool RemoteInputServer::dispatchNetworkKeycode(const QString &code)
{
    // A gamepad-only set: another Jellyfin session must not open menus, switch tabs or step chapters
    static constexpr QLatin1StringView kGamepadOnly[] = {
        "menu"_L1,        "previoustab"_L1, "nexttab"_L1,     "previouschapter"_L1,
        "nextchapter"_L1, "mainmenu"_L1,    "expandplayer"_L1};
    if (std::ranges::any_of(kGamepadOnly,
                            [&code](QLatin1StringView entry) { return code == entry; }))
        return false;
    if (!m_networkSecond.isValid() || m_networkSecond.elapsed() >= 1000) {
        m_networkSecond.start();
        m_networkKeys = 0;
    }
    if (++m_networkKeys > MaxNetworkKeysPerSecond)
        return false;
    const bool navigation = std::ranges::any_of(
        kNavigationKeys, [&code](const NavigationKey &entry) { return code == entry.code; });
    if (navigation && !networkNavigationAllowed())
        return false;
    return dispatchKeycode(code);
}

bool RemoteInputServer::networkNavigationAllowed() const
{
    QObject *focus = userWindow() ? userWindow()->focusObject() : nullptr;
    if (!focus)
        return true;
    if (focusTakesText())
        return false;
    // Up the visual parents of a Qt Quick item (its "parent" property): one inside a popup sits under the popup's item and the window's overlay
    for (QObject *item = focus; item; item = item->property("parent").value<QObject *>()) {
        if (item->inherits("QQuickPopupItem") || item->inherits("QQuickOverlay"))
            return false;
    }
    return true;
}

bool RemoteInputServer::focusTakesText() const
{
    QWindow *window = userWindow();
    QObject *focus = window ? window->focusObject() : nullptr;
    if (!focus)
        return false;
    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(focus, &query);
    return query.value(Qt::ImEnabled).toBool();
}

bool RemoteInputServer::dispatchNavigation(Qt::Key key, const QString &text, bool autoRepeat)
{
    const QPointer<QWindow> window = userWindow();
    auto *quick = qobject_cast<QQuickWindow *>(window.data());
    const QPointer<QQuickItem> focus = quick ? quick->activeFocusItem() : nullptr;
    // A spin box or a combo box that is not editable is no text field, whatever its text item answers to an input query
    QQuickItem *control = focus ? valueControlOf(focus) : nullptr;
    const bool takesText =
        focusTakesText() && !(control && !control->property("editable").toBool());

    if (!focus) {
        // Nothing of Qt Quick to look at: the key goes to the window as it is, and a Select nobody takes presses Space; a text field gets no stray space
        const bool accepted = sendKey(key, text, autoRepeat);
        if (!accepted && !takesText && key == Qt::Key_Return)
            sendKey(Qt::Key_Space, QStringLiteral(" "), autoRepeat);
        return true;
    }
    if (key == Qt::Key_Menu) {
        sendKey(key, text, autoRepeat);
        return true;
    }

    const std::optional<NavDirection> direction = directionOf(key);
    const bool vertical =
        direction && (*direction == NavDirection::Up || *direction == NavDirection::Down);
    // A text field takes its keys as they are: the cursor keeps Left and Right in a single-line field, a text area keeps every arrow, and Select is for the field's own handler; only Up and Down leave a single-line field
    if (takesText && !(vertical && isSingleLineField(focus))) {
        sendKey(key, text, autoRepeat);
        return true;
    }
    if (direction) {
        // A closed combo box or a spin box would change its value on Up and Down: they move the focus instead, and Left and Right step a spin box
        if (control) {
            if (vertical) {
                moveFocusSpatially(focus, *direction);
                return true;
            }
            if (!isComboBox(control) && !takesText) {
                const bool increase = (key == Qt::Key_Right)
                                      == (QGuiApplication::layoutDirection() == Qt::LeftToRight);
                sendKey(increase ? Qt::Key_Up : Qt::Key_Down, {}, autoRepeat);
                return true;
            }
        }
    }

    // A closed combo box accepts Return and does nothing with it: Select opens it as Space does
    if (!direction && key == Qt::Key_Return && control && isComboBox(control)) {
        const auto *popup = control->property("popup").value<QObject *>();
        if (!popup || !popup->property("visible").toBool()) {
            pressSpace(control, autoRepeat);
            return true;
        }
    }
    // A focused scroll view or flickable would scroll on the arrow: the arrow enters its contents first, and only with nothing that way does it scroll
    if (direction && !autoRepeat && isScrollContainer(focus)
        && moveFocusSpatially(focus, *direction))
        return true;

    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text, autoRepeat);
    // Accepted only by a receiver that took the key
    press.setAccepted(false);
    QPointer<QQuickItem> stopped;
    {
        AncestorKeyGuard guard(focus, key);
        QCoreApplication::sendEvent(window, &press);
        stopped = guard.stoppedAt();
    }
    const auto release = [&] {
        if (userWindow() != window)
            return;
        QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, text, autoRepeat);
        QCoreApplication::sendEvent(window, &up);
    };
    if (press.isAccepted() && !stopped) {
        release();
        return true;
    }

    if (!direction) {
        // A Select nobody used presses the focused control as Space does, and only that control: a list or page behind it must not take the Space; a scroll view enters its page instead
        if (focus) {
            if (isScrollContainer(focus))
                moveFocusSpatially(focus, NavDirection::Down);
            else
                pressSpace(focus, autoRepeat);
        }
        release();
        return true;
    }

    // A held arrow never leaves an item view, so it stops at the end of a grid
    const bool mayLeave = !(autoRepeat && focus && insideItemView(focus));
    if (mayLeave && focus && moveFocusSpatially(focus, *direction)) {
        release();
        return true;
    }
    // Nothing that way: the key goes on from where it was held back, so a page that scrolls or seeks still does
    QPointer<QQuickItem> item = stopped;
    while (item) {
        press.accept();
        QCoreApplication::sendEvent(item, &press);
        if (press.isAccepted() || !item)
            break;
        item = item->parentItem();
    }
    release();
    return true;
}

void RemoteInputServer::pressSpace(QQuickItem *target, bool autoRepeat)
{
    const QPointer<QQuickItem> item = target;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "),
                    autoRepeat);
    press.setAccepted(false);
    QCoreApplication::sendEvent(item, &press);
    if (!item)
        return;
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "),
                      autoRepeat);
    QCoreApplication::sendEvent(item, &release);
}

QWindow *RemoteInputServer::userWindow() const
{
    // Another Kinefin window has the focus (picture in picture): keys go where the user is, not to the main window behind; any other focus window is not ours and must not take them
    if (QWindow *focused = QGuiApplication::focusWindow();
        focused == m_window
        || (focused && focused->objectName() == QLatin1String("pictureInPictureWindow")))
        return focused;
    return m_window;
}

bool RemoteInputServer::sendKey(Qt::Key key, const QString &text, bool autoRepeat)
{
    QWindow *window = userWindow();
    if (!window)
        return false;
    // The window routes the key to its focus item, as a real one is; sending it to the item directly skips the QML key propagation the pages rely on
    QObject *target = window;
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text, autoRepeat);
    // Accepted only by a receiver that took the key
    press.setAccepted(false);
    QCoreApplication::sendEvent(target, &press);
    if (userWindow() == window) {
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text, autoRepeat);
        QCoreApplication::sendEvent(target, &release);
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
