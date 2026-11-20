// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen: a QWindow records the key events, QLocalSocket clients talk to the server in the same event loop
#include "integration/RemoteInputServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest/QtTest>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using Kinefin::parseRemoteFrame;
using Kinefin::RemoteInputServer;
using Kinefin::remoteSocketPath;

namespace {
QtMessageHandler g_previousHandler = nullptr;
QStringList g_warnings;

void recordWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg)
        g_warnings.append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

// Stands in for Qt Quick's popup item, which the server recognises by its class name
class QQuickPopupItem : public QObject {
    Q_OBJECT
};

// A page's plain Flickable and a ListView as plain focus objects: the server looks at items only in a Qt Quick window
class QQuickFlickable : public QObject {
    Q_OBJECT
};
class QQuickItemView : public QQuickFlickable {
    Q_OBJECT
};

// A focused item: a text field when it takes input methods, and its visual parent as Qt Quick's items expose it
class FocusItem : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *parent READ parentItem CONSTANT)
  public:
    bool textField = false;
    QObject *visualParent = nullptr;
    QObject *parentItem() const { return visualParent; }

  protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::InputMethodQuery) {
            static_cast<QInputMethodQueryEvent *>(event)->setValue(Qt::ImEnabled, textField);
            return true;
        }
        return QObject::event(event);
    }
};

class KeyWindow : public QWindow {
  public:
    QList<int> presses;
    QList<int> releases;
    QList<bool> repeats;
    bool acceptEscape = true;
    bool acceptArrows = true;
    bool acceptReturn = true;
    QObject *focus = nullptr;

    QObject *focusObject() const override { return focus ? focus : QWindow::focusObject(); }

  protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        presses.append(event->key());
        repeats.append(event->isAutoRepeat());
        bool accepted = true;
        if (event->key() == Qt::Key_Escape)
            accepted = acceptEscape;
        else if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down
                 || event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)
            accepted = acceptArrows;
        else if (event->key() == Qt::Key_Return)
            accepted = acceptReturn;
        event->setAccepted(accepted);
    }
    void keyReleaseEvent(QKeyEvent *event) override { releases.append(event->key()); }
};

struct TestClient {
    QByteArray received;
    bool closed = false;
    // Last, so it goes first: its closing signals still find the fields above alive
    QLocalSocket socket;

    explicit TestClient(const QString &path)
    {
        QObject::connect(&socket, &QLocalSocket::readyRead, &socket,
                         [this] { received += socket.readAll(); });
        QObject::connect(&socket, &QLocalSocket::disconnected, &socket, [this] { closed = true; });
        socket.connectToServer(path);
    }

    bool waitForWelcome()
    {
        return QTest::qWaitFor([this] { return received.endsWith('\n') || closed; }) && !closed;
    }

    void send(const QByteArray &data) { socket.write(data); }
};

QByteArray frameLine(const QString &keycode)
{
    const QJsonObject object{{QStringLiteral("client"), QStringLiteral("test")},
                             {QStringLiteral("source"), QStringLiteral("unit")},
                             {QStringLiteral("keycode"), keycode}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QByteArray longText()
{
    return QByteArray(65, 'a');
}

// Lines that are no frame
QList<QByteArray> invalidLines()
{
    return {
        QByteArrayLiteral("not json"),
        QByteArrayLiteral(""),
        QByteArrayLiteral("\"home\""),
        QByteArrayLiteral("[{\"keycode\":\"home\"}]"),
        QByteArrayLiteral("{\"keycode\":\"home\""),
        QByteArrayLiteral("{\"client\":\"x\"}"),
        QByteArrayLiteral("{\"keycode\":5}"),
        QByteArrayLiteral("{\"keycode\":null}"),
        QByteArrayLiteral("{\"keycode\":[\"home\"]}"),
        QByteArrayLiteral("{\"keycode\":\"") + longText() + QByteArrayLiteral("\"}"),
        QByteArrayLiteral("{\"keycode\":\"home\",\"client\":\"") + longText()
            + QByteArrayLiteral("\"}"),
        QByteArrayLiteral("{\"keycode\":\"home\",\"source\":\"") + longText()
            + QByteArrayLiteral("\"}"),
        QByteArrayLiteral("{\"keycode\":\"home\",\"client\":5}"),
        QByteArrayLiteral("{\"keycode\":\"\xff\"}"),
    };
}

bool bindStaleSocket(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(path);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, encoded.constData(), static_cast<size_t>(encoded.size()));
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return false;
    const bool bound =
        ::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0;
    ::close(descriptor);
    return bound;
}

// lstat's mode, 0 when the path does not exist
mode_t modeOf(const QString &path)
{
    struct stat info{};
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0 ? info.st_mode : 0;
}
} // namespace

class TestRemoteInput : public QObject {
    Q_OBJECT
  private:
    std::unique_ptr<QTemporaryDir> m_runtime;
    QString m_path;
    QStringList m_actions;

    std::unique_ptr<RemoteInputServer> startServer(KeyWindow *window = nullptr)
    {
        auto server = std::make_unique<RemoteInputServer>(m_path, QStringLiteral("1.2.3"));
        connect(server.get(), &RemoteInputServer::actionRequested, this,
                [this](const QString &action) { m_actions.append(action); });
        server->setWindow(window);
        return server->listen() ? std::move(server) : nullptr;
    }

    std::unique_ptr<TestClient> welcomedClient()
    {
        auto client = std::make_unique<TestClient>(m_path);
        return client->waitForWelcome() ? std::move(client) : nullptr;
    }

  private Q_SLOTS:
    void initTestCase() { g_previousHandler = qInstallMessageHandler(recordWarnings); }
    void cleanupTestCase() { qInstallMessageHandler(g_previousHandler); }

    void networkNavigationKeysStayOutOfTextFieldsAndPopups()
    {
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        QVERIFY(server->dispatchNetworkKeycode(QStringLiteral("down")));
        QCOMPARE(window.presses, QList<int>{Qt::Key_Down});
        // The user types: a remote Select would submit the field
        FocusItem field;
        field.textField = true;
        window.focus = &field;
        QVERIFY(!server->dispatchNetworkKeycode(QStringLiteral("select")));
        QVERIFY(!server->dispatchNetworkKeycode(QStringLiteral("up")));
        QCOMPARE(window.presses, QList<int>{Qt::Key_Down});
        // A button inside a popup: a remote Select would confirm the dialog
        QQuickPopupItem popup;
        FocusItem button;
        button.visualParent = &popup;
        window.focus = &button;
        QVERIFY(!server->dispatchNetworkKeycode(QStringLiteral("select")));
        QCOMPARE(window.presses, QList<int>{Qt::Key_Down});
        // Player commands and Back still pass
        QVERIFY(server->dispatchNetworkKeycode(QStringLiteral("pause")));
        QCOMPARE(m_actions.constLast(), QStringLiteral("pause"));
        QVERIFY(server->dispatchNetworkKeycode(QStringLiteral("back")));
        QCOMPARE(window.presses.constLast(), int(Qt::Key_Escape));
        // Out of the popup the keys work again
        window.focus = nullptr;
        QVERIFY(server->dispatchNetworkKeycode(QStringLiteral("select")));
        QCOMPARE(window.presses.constLast(), int(Qt::Key_Return));
    }

    void networkKeycodesAreRateLimited()
    {
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        int passed = 0;
        for (int i = 0; i < RemoteInputServer::MaxNetworkKeysPerSecond + 5; ++i)
            passed += server->dispatchNetworkKeycode(QStringLiteral("volumeup")) ? 1 : 0;
        QCOMPARE(passed, RemoteInputServer::MaxNetworkKeysPerSecond);
        // The local socket's own path has its own limit, per client
        QVERIFY(server->dispatchKeycode(QStringLiteral("volumeup")));
    }

    // Every test has a runtime directory of its own, so concurrent runs never share a path
    void init()
    {
        m_runtime = std::make_unique<QTemporaryDir>();
        QVERIFY(m_runtime->isValid());
        qputenv("XDG_RUNTIME_DIR", QFile::encodeName(m_runtime->path()));
        m_path = remoteSocketPath(QStringLiteral("test"));
        QVERIFY(!m_path.isEmpty());
        m_actions.clear();
        g_warnings.clear();
    }

    void socketPath()
    {
        const QString suffix = QStringLiteral("/app/io.github.the_beper.Kinefin/kinefin-");
        qputenv("XDG_RUNTIME_DIR", "/run/user/1000");
        QCOMPARE(remoteSocketPath(QStringLiteral("default")),
                 QStringLiteral("/run/user/1000") + suffix + QStringLiteral("default"));

        for (const QString &invalid :
             {QString(), QStringLiteral("../x"), QStringLiteral(".hidden"), QStringLiteral("-x"),
              QStringLiteral("a/b"), QString(65, QLatin1Char('a'))})
            QVERIFY2(remoteSocketPath(invalid).isEmpty(), qPrintable(invalid));

        qputenv("XDG_RUNTIME_DIR", "run/user/1000");
        QVERIFY(remoteSocketPath(QStringLiteral("default")).isEmpty());
        qputenv("XDG_RUNTIME_DIR", "");
        QVERIFY(remoteSocketPath(QStringLiteral("default")).isEmpty());
        qunsetenv("XDG_RUNTIME_DIR");
        QVERIFY(remoteSocketPath(QStringLiteral("default")).isEmpty());

        // sun_path holds 107 bytes: the last one that fits, and the first that does not
        const QString id = QStringLiteral("abcdefghij");
        const qsizetype runtimeLength = 107 - suffix.size() - id.size();
        qputenv("XDG_RUNTIME_DIR", QByteArray("/").append(QByteArray(runtimeLength - 1, 'r')));
        QCOMPARE(remoteSocketPath(id).size(), 107);
        qputenv("XDG_RUNTIME_DIR", QByteArray("/").append(QByteArray(runtimeLength, 'r')));
        QVERIFY(remoteSocketPath(id).isEmpty());
    }

    void listenWithoutPath()
    {
        RemoteInputServer server(QString(), QStringLiteral("1.2.3"));
        QVERIFY(!server.listen());
        QCOMPARE(g_warnings.size(), 1);
    }

    void privateModes()
    {
        auto server = startServer();
        QVERIFY(server);
        // The mode bits themselves: QFileInfo::permissions() adds the bits of the current user
        QCOMPARE(modeOf(QFileInfo(m_path).absolutePath()) & 07777, mode_t(0700));
        QVERIFY(S_ISSOCK(modeOf(m_path)));
        QCOMPARE(modeOf(m_path) & 07777, mode_t(0600));
    }

    void welcomeFirst()
    {
        auto server = startServer();
        QVERIFY(server);
        auto client = welcomedClient();
        QVERIFY(client);
        const QByteArray welcome =
            QByteArrayLiteral("{\"version\":\"1.2.3\",\"builddate\":\"\"}\n");
        QCOMPARE(client->received, welcome);

        // Nothing else is ever sent, whatever the client does
        client->send(QByteArrayLiteral("not json\n") + frameLine(QStringLiteral("home")));
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("home")});
        QCOMPARE(client->received, welcome);
    }

    void parseFrame_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<QString>("client");
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("keycode");
        const QString none;
        QTest::newRow("keycode only") << QByteArrayLiteral("{\"keycode\":\"up\"}") << true << none
                                      << none << QStringLiteral("up");
        QTest::newRow("all fields")
            << QByteArrayLiteral("{\"client\":\"remote\",\"source\":\"ir\",\"keycode\":\"up\"}")
            << true << QStringLiteral("remote") << QStringLiteral("ir") << QStringLiteral("up");
        QTest::newRow("extra keys ignored")
            << QByteArrayLiteral("{\"keycode\":\"up\",\"repeat\":true,\"x\":[1]}") << true << none
            << none << QStringLiteral("up");
        QTest::newRow("surrounding space") << QByteArrayLiteral(" {\"keycode\":\"up\"}\r") << true
                                           << none << none << QStringLiteral("up");
        QTest::newRow("64 characters")
            << QByteArrayLiteral("{\"keycode\":\"") + QByteArray(64, 'k') + QByteArrayLiteral("\"}")
            << true << none << none << QString(64, QLatin1Char('k'));
        const QList<QByteArray> invalid = invalidLines();
        for (qsizetype i = 0; i < invalid.size(); ++i)
            QTest::newRow(qPrintable(QStringLiteral("invalid %1").arg(i)))
                << invalid.at(i) << false << none << none << none;
    }

    void parseFrame()
    {
        QFETCH(QByteArray, line);
        QFETCH(bool, valid);
        QFETCH(QString, client);
        QFETCH(QString, source);
        QFETCH(QString, keycode);
        const auto frame = parseRemoteFrame(line);
        QCOMPARE(frame.has_value(), valid);
        if (!valid)
            return;
        QCOMPARE(frame->client, client);
        QCOMPARE(frame->source, source);
        QCOMPARE(frame->keycode, keycode);
    }

    void keycodeTable_data()
    {
        QTest::addColumn<QString>("keycode");
        QTest::addColumn<int>("key");
        QTest::addColumn<QString>("action");
        const auto key = [](const char *keycode, Qt::Key value) {
            QTest::newRow(keycode) << QString::fromLatin1(keycode) << int(value) << QString();
        };
        const auto action = [](const char *keycode, const char *name) {
            QTest::newRow(keycode)
                << QString::fromLatin1(keycode) << 0 << QString::fromLatin1(name);
        };
        key("up", Qt::Key_Up);
        key("down", Qt::Key_Down);
        key("left", Qt::Key_Left);
        key("right", Qt::Key_Right);
        key("select", Qt::Key_Return);
        key("back", Qt::Key_Escape);
        key("menu", Qt::Key_Menu);
        for (const char *name :
             {"home", "playpause", "play", "pause", "stop", "next", "previous", "fastforward",
              "rewind", "volumeup", "volumedown", "togglemute", "togglefullscreen", "previoustab",
              "nexttab", "previouschapter", "nextchapter", "mainmenu", "expandplayer"})
            action(name, name);
        action("host:fullscreen", "togglefullscreen");
    }

    void keycodeTable()
    {
        QFETCH(QString, keycode);
        QFETCH(int, key);
        QFETCH(QString, action);
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        auto client = welcomedClient();
        QVERIFY(client);

        client->send(frameLine(keycode));
        if (key != 0) {
            QTRY_COMPARE(window.presses, QList<int>{key});
            QTRY_COMPARE(window.releases, QList<int>{key});
            QVERIFY(m_actions.isEmpty());
        } else {
            QTRY_COMPARE(m_actions, QStringList{action});
            QVERIFY(window.presses.isEmpty());
        }
    }

    // The move of the focus by screen position needs a Qt Quick window and is tested in test_spatial_navigation; here an arrow nobody takes stays one press, with no Tab or Backtab behind it
    void anArrowNobodyTakesIsNotRetried()
    {
        KeyWindow window;
        window.acceptArrows = false;
        auto server = startServer(&window);
        QVERIFY(server);
        for (const char *keycode : {"down", "up", "right", "left"})
            QVERIFY2(server->dispatchKeycode(QString::fromLatin1(keycode)), keycode);
        QCOMPARE(window.presses,
                 (QList<int>{Qt::Key_Down, Qt::Key_Up, Qt::Key_Right, Qt::Key_Left}));
        QCOMPARE(window.releases, window.presses);
    }

    void aFocusOutsideQtQuickGetsTheKeysAsTheyAre()
    {
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        // Whatever class the focus object has, there is no Qt Quick item to look at: the keys go to the window unchanged
        QQuickItemView list;
        QQuickFlickable page;
        for (QObject *focus : {static_cast<QObject *>(&list), static_cast<QObject *>(&page)}) {
            window.focus = focus;
            window.presses.clear();
            for (const char *keycode : {"left", "right", "up", "down", "select"})
                QVERIFY2(server->dispatchKeycode(QString::fromLatin1(keycode)), keycode);
            QCOMPARE(window.presses, (QList<int>{Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
                                                 Qt::Key_Down, Qt::Key_Return}));
        }
    }

    void selectFallsBackToSpace()
    {
        KeyWindow window;
        window.acceptReturn = false;
        auto server = startServer(&window);
        QVERIFY(server);
        QVERIFY(server->dispatchKeycode(QStringLiteral("select")));
        QCOMPARE(window.presses, (QList<int>{Qt::Key_Return, Qt::Key_Space}));
    }

    void noFallbackInATextField()
    {
        KeyWindow window;
        window.acceptArrows = false;
        window.acceptReturn = false;
        FocusItem field;
        field.textField = true;
        window.focus = &field;
        auto server = startServer(&window);
        QVERIFY(server);
        QVERIFY(server->dispatchKeycode(QStringLiteral("down")));
        QCOMPARE(window.presses, QList<int>{Qt::Key_Down});
        window.presses.clear();
        QVERIFY(server->dispatchKeycode(QStringLiteral("select")));
        QCOMPARE(window.presses, QList<int>{Qt::Key_Return});
    }

    void theAutoRepeatFlagReachesTheReceiver()
    {
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        QVERIFY(server->dispatchKeycode(QStringLiteral("select"), true));
        QCOMPARE(window.repeats, QList<bool>{true});
    }

    void theNetworkCannotSendTheNewKeycodes()
    {
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        for (const char *keycode : {"menu", "previoustab", "nexttab", "previouschapter",
                                    "nextchapter", "mainmenu", "expandplayer"})
            QVERIFY2(!server->dispatchNetworkKeycode(QString::fromLatin1(keycode)), keycode);
        QVERIFY(window.presses.isEmpty());
        QVERIFY(m_actions.isEmpty());
    }

    void backFallsBackWhenEscapeIsIgnored()
    {
        KeyWindow window;
        window.acceptEscape = false;
        auto server = startServer(&window);
        QVERIFY(server);
        auto client = welcomedClient();
        QVERIFY(client);

        client->send(frameLine(QStringLiteral("back")));
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("back")});
        QCOMPARE(window.presses, QList<int>{Qt::Key_Escape});

        // Without a window nothing takes the key, so back is a command again
        server->setWindow(nullptr);
        client->send(frameLine(QStringLiteral("back")));
        QTRY_COMPARE(m_actions.size(), 2);
        QCOMPARE(window.presses.size(), 1);
    }

    void unknownKeycodeIgnored()
    {
        KeyWindow window;
        auto server = startServer(&window);
        QVERIFY(server);
        auto client = welcomedClient();
        QVERIFY(client);

        for (const char *keycode : {"nonsense", "host:unknown", "UP", ""})
            client->send(frameLine(QString::fromLatin1(keycode)));
        client->send(frameLine(QStringLiteral("home")));
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("home")});
        QVERIFY(window.presses.isEmpty());
        QVERIFY(!client->closed);
        QCOMPARE(g_warnings.size(), 0);
    }

    void invalidFramesDropped()
    {
        auto server = startServer();
        QVERIFY(server);
        auto client = welcomedClient();
        QVERIFY(client);

        for (const QByteArray &line : invalidLines())
            client->send(line + '\n');
        client->send(frameLine(QStringLiteral("home")));
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("home")});
        QVERIFY(!client->closed);
        QCOMPARE(g_warnings.size(), 1);
    }

    void oversizedFrameCloses_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("unterminated") << QByteArray(5000, 'x');
        QTest::newRow("terminated") << QByteArray(5000, 'x') + '\n';
        QTest::newRow("huge") << QByteArray(200000, 'x');
    }

    void oversizedFrameCloses()
    {
        QFETCH(QByteArray, data);
        auto server = startServer();
        QVERIFY(server);
        auto offender = welcomedClient();
        auto bystander = welcomedClient();
        QVERIFY(offender);
        QVERIFY(bystander);

        offender->send(data);
        QTRY_VERIFY_WITH_TIMEOUT(offender->closed, 5000);
        QVERIFY(!bystander->closed);
        bystander->send(frameLine(QStringLiteral("home")));
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("home")});
    }

    void splitAndJoinedFrames()
    {
        auto server = startServer();
        QVERIFY(server);
        auto client = welcomedClient();
        QVERIFY(client);

        const QByteArray line = frameLine(QStringLiteral("home"));
        // Each part is on its way before the next one is written
        for (const QByteArray &part : {line.left(5), line.mid(5, 7), line.mid(12)}) {
            client->send(part);
            client->socket.flush();
            for (int turn = 0; turn < 3; ++turn)
                QCoreApplication::processEvents();
        }
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("home")});

        client->send(frameLine(QStringLiteral("pause")) + frameLine(QStringLiteral("play")));
        const QStringList expected{QStringLiteral("home"), QStringLiteral("pause"),
                                   QStringLiteral("play")};
        QTRY_COMPARE(m_actions, expected);
    }

    void twoConcurrentClients()
    {
        auto server = startServer();
        QVERIFY(server);
        auto first = welcomedClient();
        auto second = welcomedClient();
        QVERIFY(first);
        QVERIFY(second);

        first->send(frameLine(QStringLiteral("play")));
        second->send(frameLine(QStringLiteral("pause")));
        first->send(frameLine(QStringLiteral("stop")));
        second->send(frameLine(QStringLiteral("next")));
        QTRY_COMPARE(m_actions.size(), 4);
        QStringList actions = m_actions;
        actions.sort();
        const QStringList expected{QStringLiteral("next"), QStringLiteral("pause"),
                                   QStringLiteral("play"), QStringLiteral("stop")};
        QCOMPARE(actions, expected);
        QVERIFY(!first->closed);
        QVERIFY(!second->closed);
    }

    void fifthClientClosed()
    {
        auto server = startServer();
        QVERIFY(server);
        std::vector<std::unique_ptr<TestClient>> clients;
        for (int i = 0; i < 4; ++i) {
            clients.push_back(welcomedClient());
            QVERIFY(clients.back());
        }

        TestClient fifth(m_path);
        QTRY_VERIFY_WITH_TIMEOUT(fifth.closed, 5000);
        QVERIFY(fifth.received.isEmpty());

        for (const auto &client : clients)
            QVERIFY(!client->closed);
        clients.front()->send(frameLine(QStringLiteral("home")));
        QTRY_COMPARE(m_actions, QStringList{QStringLiteral("home")});
    }

    void rateLimitCloses()
    {
        auto server = startServer();
        QVERIFY(server);
        auto flooder = welcomedClient();
        auto bystander = welcomedClient();
        QVERIFY(flooder);
        QVERIFY(bystander);

        QByteArray burst;
        for (int i = 0; i < 200; ++i)
            burst += frameLine(QStringLiteral("home"));
        flooder->send(burst);
        QTRY_VERIFY_WITH_TIMEOUT(flooder->closed, 5000);
        QCOMPARE(m_actions.size(), 50);

        QVERIFY(!bystander->closed);
        bystander->send(frameLine(QStringLiteral("play")));
        QTRY_COMPARE(m_actions.size(), 51);
        QCOMPARE(m_actions.last(), QStringLiteral("play"));
    }

    void staleSocketReplaced()
    {
        QVERIFY(QDir().mkpath(QFileInfo(m_path).absolutePath()));
        QVERIFY(bindStaleSocket(m_path));
        QVERIFY(S_ISSOCK(modeOf(m_path)));

        auto server = startServer();
        QVERIFY(server);
        QVERIFY(welcomedClient());
    }

    void liveSocketKept()
    {
        auto first = startServer();
        QVERIFY(first);

        auto second = std::make_unique<RemoteInputServer>(m_path, QStringLiteral("9.9.9"));
        QVERIFY(!second->listen());
        QCOMPARE(g_warnings.size(), 1);
        second.reset();

        auto client = welcomedClient();
        QVERIFY(client);
        QVERIFY(client->received.contains("1.2.3"));
    }

    void regularFileKept_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("file") << int(S_IFREG);
        QTest::newRow("symlink") << int(S_IFLNK);
        QTest::newRow("dangling symlink") << int(S_IFLNK) + 1;
        QTest::newRow("directory") << int(S_IFDIR);
    }

    void regularFileKept()
    {
        QFETCH(int, kind);
        const QString folder = QFileInfo(m_path).absolutePath();
        QVERIFY(QDir().mkpath(folder));
        const QString target = folder + QStringLiteral("/target");
        switch (kind) {
        case S_IFREG: {
            QFile file(m_path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("precious");
            break;
        }
        case S_IFLNK: {
            QFile file(target);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("precious");
            file.close();
            QVERIFY(QFile::link(target, m_path));
            break;
        }
        case S_IFLNK + 1:
            QVERIFY(QFile::link(target, m_path));
            break;
        default:
            QVERIFY(QDir().mkdir(m_path));
        }
        const mode_t before = modeOf(m_path);

        auto server = startServer();
        QVERIFY(!server);
        QCOMPARE(g_warnings.size(), 1);
        QCOMPARE(modeOf(m_path), before);
        if (kind == S_IFREG) {
            QFile file(m_path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArrayLiteral("precious"));
        }
    }

    void removedOnDestruction()
    {
        auto server = startServer();
        QVERIFY(server);
        QVERIFY(S_ISSOCK(modeOf(m_path)));
        server.reset();
        QCOMPARE(modeOf(m_path), mode_t(0));
    }
};

QTEST_MAIN(TestRemoteInput)
#include "test_remote_input.moc"
