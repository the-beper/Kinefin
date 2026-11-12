// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/UpdateChecker.h"

#include <QHostAddress>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest/QtTest>

#include <memory>
#include <tuple>

using Kinefin::UpdateChecker;

namespace {
class ReleaseServer final : public QObject {
  public:
    ReleaseServer()
    {
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = listener.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, request] {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n"))
                        return;
                    requests.append(*request);
                    // A held request lets the client abort while its reply is
                    // outstanding, so no signal from an old request may land.
                    if (hold) {
                        pending = socket;
                        return;
                    }
                    send(socket);
                });
            }
        });
    }

    bool listen() { return listener.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/the-beper/Kinefin/releases/latest")
                        .arg(listener.serverPort()));
    }

    void send(QTcpSocket *socket)
    {
        if (!socket)
            return;
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }

    void release()
    {
        QPointer<QTcpSocket> socket = pending;
        pending = nullptr;
        send(socket);
    }

    static QByteArray redirect(const QByteArray &location, int status = 302)
    {
        return "HTTP/1.1 " + QByteArray::number(status) + " Redirect\r\nLocation: " + location
               + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }

    QTcpServer listener;
    QByteArray response = redirect("/the-beper/Kinefin/releases/tag/v1.2.4");
    QList<QByteArray> requests;
    bool hold = false;
    QPointer<QTcpSocket> pending;
};
} // namespace

class TestUpdateChecker final : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void onlyWellFormedVersionsCanBeCompared()
    {
        for (const auto &[current, latest, expected] : {
                 std::tuple{"1.2.3", "1.2.4", 1},
                 std::tuple{"1.2.3", "1.2.3", 0},
                 std::tuple{"1.2.3", "1.2.3.0", 0},
                 std::tuple{"1.10", "1.9.99", -1},
                 std::tuple{"0.0.1", "0.1", 1},
                 std::tuple{"3.2-5", "3.2-4", -1},
             }) {
            const auto result = UpdateChecker::compareVersions(QString::fromLatin1(current),
                                                               QString::fromLatin1(latest));
            QVERIFY(result);
            QCOMPARE(*result, expected);
        }
        for (const QString &invalid : {
                 QStringLiteral("v1.2.3"),
                 QStringLiteral("1.2.3-rc1"),
                 QStringLiteral("1..2"),
                 QStringLiteral("1.2."),
                 QStringLiteral("-1"),
                 QStringLiteral("1.2+1"),
                 QStringLiteral("1.99999999999999"),
                 QString(100, QLatin1Char('1')),
                 QStringLiteral(""),
             }) {
            QVERIFY2(!UpdateChecker::compareVersions(QStringLiteral("1.2.3"), invalid),
                     qPrintable(invalid));
            QVERIFY2(!UpdateChecker::compareVersions(invalid, QStringLiteral("1.2.3")),
                     qPrintable(invalid));
        }
    }

    void followsOnlyTheReleaseTagWithoutFetchingIt()
    {
        ReleaseServer server;
        QVERIFY(server.listen());
        UpdateChecker checker(QStringLiteral("1.2.3"), server.url());
        QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
        QSignalSpy failures(&checker, &UpdateChecker::checkFailed);
        QSignalSpy checking(&checker, &UpdateChecker::checkingChanged);
        QVERIFY(checker.check());
        QVERIFY(checker.checking());
        QVERIFY(!checker.check()); // no duplicate network request
        QTRY_COMPARE(available.size(), 1);
        QVERIFY(!checker.checking());
        QCOMPARE(checking.size(), 2);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(available.first().at(0).toString(), QStringLiteral("1.2.4"));
        QCOMPARE(available.first().at(1).toUrl().toString(),
                 server.url().toString().replace(QStringLiteral("/releases/latest"),
                                                 QStringLiteral("/releases/tag/v1.2.4")));
        QCOMPARE(server.requests.size(), 1);
        QVERIFY(server.requests.first().startsWith(
            "GET /the-beper/Kinefin/releases/latest HTTP/1.1\r\n"));
    }

    void alsoAcceptsOlderBareNumericTags()
    {
        ReleaseServer server;
        QVERIFY(server.listen());
        server.response = ReleaseServer::redirect("/the-beper/Kinefin/releases/tag/1.2.5");
        UpdateChecker checker(QStringLiteral("1.2.3"), server.url());
        QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
        QVERIFY(checker.check());
        QTRY_COMPARE(available.size(), 1);
        QCOMPARE(available.first().at(0).toString(), QStringLiteral("1.2.5"));
    }

    void equalAndOlderVersionsNeverPrompt()
    {
        for (const QByteArray &tag : {QByteArrayLiteral("1.2.3"), QByteArrayLiteral("1.2.2")}) {
            ReleaseServer server;
            QVERIFY(server.listen());
            server.response = ReleaseServer::redirect("/the-beper/Kinefin/releases/tag/v" + tag);
            UpdateChecker checker(QStringLiteral("1.2.3"), server.url());
            QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
            QSignalSpy upToDate(&checker, &UpdateChecker::upToDate);
            QVERIFY(checker.check());
            QTRY_COMPARE(upToDate.size(), 1);
            QCOMPARE(available.size(), 0);
        }
    }

    void rejectsRedirectsOutsideTheExactTrustedReleasePath_data()
    {
        QTest::addColumn<QByteArray>("location");
        QTest::newRow("other origin") << QByteArray("https://updates.example/releases/tag/99.0");
        QTest::newRow("other protocol") << QByteArray("file:///tmp/releases/tag/99.0");
        QTest::newRow("foreign account") << QByteArray("/other/Kinefin/releases/tag/99.0");
        QTest::newRow("other repository") << QByteArray("/the-beper/Other/releases/tag/99.0");
        QTest::newRow("embedded credentials")
            << QByteArray("http://user@127.0.0.1/the-beper/Kinefin/releases/tag/99.0");
        QTest::newRow("not a tag") << QByteArray("/the-beper/Kinefin/releases");
        QTest::newRow("traversal") << QByteArray("/the-beper/Kinefin/releases/tag/../tag/99.0");
        QTest::newRow("extra slash") << QByteArray("/the-beper/Kinefin/releases/tag/99.0/evil");
        QTest::newRow("encoded slash") << QByteArray("/the-beper/Kinefin/releases/tag/99%2F0");
        QTest::newRow("encoded dot traversal")
            << QByteArray("/the-beper/Kinefin/releases/tag/%2e%2e/tag/99.0");
        QTest::newRow("encoded digit") << QByteArray("/the-beper/Kinefin/releases/tag/%39%39.0");
        QTest::newRow("query") << QByteArray("/the-beper/Kinefin/releases/tag/99.0?x=1");
        QTest::newRow("fragment") << QByteArray("/the-beper/Kinefin/releases/tag/99.0#ok");
        QTest::newRow("pre-release") << QByteArray("/the-beper/Kinefin/releases/tag/99.0-rc1");
        QTest::newRow("double v") << QByteArray("/the-beper/Kinefin/releases/tag/vv99.0");
    }

    void rejectsRedirectsOutsideTheExactTrustedReleasePath()
    {
        QFETCH(QByteArray, location);
        ReleaseServer server;
        QVERIFY(server.listen());
        server.response = ReleaseServer::redirect(location);
        UpdateChecker checker(QStringLiteral("1.2.3"), server.url());
        QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
        QSignalSpy failures(&checker, &UpdateChecker::checkFailed);
        QVERIFY(checker.check());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(available.size(), 0);
        QCOMPARE(server.requests.size(), 1);
    }

    void rejectsNonRedirectResponses()
    {
        ReleaseServer server;
        QVERIFY(server.listen());
        server.response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 10"
                          "\r\nConnection: close\r\n\r\nNOT A TAG!";
        UpdateChecker checker(QStringLiteral("1.2.3"), server.url());
        QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
        QSignalSpy failures(&checker, &UpdateChecker::checkFailed);
        QVERIFY(checker.check());
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(available.size(), 0);
    }

    void cancellingAnInFlightRequestDoesNotEmitAResult()
    {
        ReleaseServer server;
        QVERIFY(server.listen());
        server.hold = true;
        UpdateChecker checker(QStringLiteral("1.2.3"), server.url());
        QSignalSpy available(&checker, &UpdateChecker::updateAvailable);
        QSignalSpy failures(&checker, &UpdateChecker::checkFailed);
        QSignalSpy upToDate(&checker, &UpdateChecker::upToDate);
        QVERIFY(checker.check());
        QTRY_COMPARE(server.requests.size(), 1);
        checker.cancel();
        QVERIFY(!checker.checking());
        server.release();
        QTest::qWait(50);
        QCOMPARE(available.size(), 0);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(upToDate.size(), 0);
    }

    void invalidLocalVersionNeverMakesARequest()
    {
        ReleaseServer server;
        QVERIFY(server.listen());
        UpdateChecker checker(QStringLiteral("0.0.1-dev"), server.url());
        QSignalSpy failures(&checker, &UpdateChecker::checkFailed);
        QVERIFY(!checker.check());
        QCOMPARE(failures.size(), 1);
        QVERIFY(!checker.checking());
        QCOMPARE(server.requests.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestUpdateChecker)
#include "test_update_checker.moc"
