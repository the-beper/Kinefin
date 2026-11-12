// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include "integration/LinkRouter.h"

class TestLinkRouter : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void validItemLink()
    {
        const auto id = Kinefin::parseItemLink(
            QStringLiteral("jellyfin://item/ABCDEF0123456789ABCDEF0123456789"));
        QVERIFY(id);
        QCOMPARE(*id, QStringLiteral("abcdef0123456789abcdef0123456789"));
    }
    void rejectsUnverifiedShapesAndSecrets()
    {
        for (const auto &url : {"https://example.com/items/abcdef0123456789abcdef0123456789",
                                "jellyfin://item/../admin", "jellyfin://item/xyz",
                                "jellyfin://item/abcdef0123456789abcdef0123456789?api_key=secret",
                                "jellyfin://u:p@item/abcdef0123456789abcdef0123456789",
                                "jellyfin://item/abcdef0123456789abcdef0123456789#fragment",
                                "jellyfin://item/abcdef0123456789abcdef0123456789/",
                                "jellyfin://item//abcdef0123456789abcdef0123456789",
                                "jellyfin://item:80/abcdef0123456789abcdef0123456789",
                                "jellyfin://item/%61bcdef0123456789abcdef0123456789",
                                "jellyfin://item/abcdef0123456789abcdef0123456789%2f",
                                " jellyfin://item/abcdef0123456789abcdef0123456789",
                                "jellyfin://item/abcdef0123456789abcdef0123456789\n"}) {
            QVERIFY2(!Kinefin::parseItemLink(QString::fromLatin1(url)), url);
        }
        // RFC 3986 scheme and hostname are case-insensitive, IDs are normalized.
        const auto mixed = Kinefin::parseItemLink(
            QStringLiteral("JELLYFIN://ITEM/ABCDEF0123456789ABCDEF0123456789"));
        QVERIFY(mixed);
        QCOMPARE(*mixed, QStringLiteral("abcdef0123456789abcdef0123456789"));
    }

    void forwardedActivationAcceptsOnePositionalLink()
    {
        const QString uri = QStringLiteral("jellyfin://item/ABCDEF0123456789ABCDEF0123456789");
        const QString id = QStringLiteral("abcdef0123456789abcdef0123456789");
        const auto forwarded = Kinefin::itemFromActivationArguments(
            {QStringLiteral("/usr/bin/kinefin"), QStringLiteral("--profile"),
             QStringLiteral("home"), QStringLiteral("--fullscreen"), uri});
        QVERIFY(forwarded);
        QCOMPARE(*forwarded, id);
        const auto afterDelimiter = Kinefin::itemFromActivationArguments(
            {QStringLiteral("kinefin"), QStringLiteral("--"), uri});
        QVERIFY(afterDelimiter);
        QCOMPARE(*afterDelimiter, id);
        QVERIFY(!Kinefin::itemFromActivationArguments(
            {QStringLiteral("kinefin"), QStringLiteral("--log-level"), uri}));
        QVERIFY(!Kinefin::itemFromActivationArguments(
            {QStringLiteral("kinefin"), QStringLiteral("--profile"), uri}));
        QVERIFY(!Kinefin::itemFromActivationArguments({QStringLiteral("kinefin"), uri, uri}));
        QVERIFY(!Kinefin::itemFromActivationArguments(
            {QStringLiteral("kinefin"), QStringLiteral("jellyfin://item/../admin")}));
        QVERIFY(!Kinefin::itemFromActivationArguments({QStringLiteral("kinefin")}));
    }
    void webLinksAreHttpOrHttpsWithoutCredentials()
    {
        QVERIFY(Kinefin::parseWebLink(QStringLiteral("https://jellyfin.example.com:8920/web/")));
        QVERIFY(Kinefin::parseWebLink(QStringLiteral("http://192.168.1.10:8096")));
        for (const auto &text :
             {"file:///etc/passwd", "javascript:alert(1)", "ftp://example.com",
              "jellyfin://item/abcdef0123456789abcdef0123456789", "https://user:pass@example.com/",
              "https:///no-host", "example.com", "https://exa mple.com/", ""}) {
            QVERIFY2(!Kinefin::parseWebLink(QString::fromLatin1(text)), text);
        }
    }

    void theAdminDashboardIsTheWebClientsDashboardPage_data()
    {
        QTest::addColumn<QString>("base");
        QTest::addColumn<QString>("expected");
        QTest::newRow("a bare host")
            << "http://192.168.1.10:8096" << "http://192.168.1.10:8096/web/#/dashboard";
        QTest::newRow("a trailing slash")
            << "https://jf.example.com/" << "https://jf.example.com/web/#/dashboard";
        QTest::newRow("a path prefix")
            << "https://example.com/jellyfin" << "https://example.com/jellyfin/web/#/dashboard";
        QTest::newRow("a prefix with a slash")
            << "https://example.com/jellyfin/" << "https://example.com/jellyfin/web/#/dashboard";
        QTest::newRow("the web client already")
            << "https://jf.example.com:8920/web/" << "https://jf.example.com:8920/web/#/dashboard";
        QTest::newRow("the web client without slash")
            << "https://jf.example.com/web" << "https://jf.example.com/web/#/dashboard";
        QTest::newRow("a query and a fragment")
            << "https://jf.example.com/?x=1#old" << "https://jf.example.com/web/#/dashboard";
    }

    void theAdminDashboardIsTheWebClientsDashboardPage()
    {
        QFETCH(QString, base);
        QFETCH(QString, expected);
        const auto url = Kinefin::adminDashboardUrl(base);
        QVERIFY(url);
        QCOMPARE(url->toString(), expected);
    }

    void theAdminDashboardRefusesWhatAWebLinkRefuses()
    {
        for (const auto &text :
             {"file:///etc/passwd", "javascript:alert(1)", "https://user:pass@example.com/",
              "https:///no-host", "example.com", ""})
            QVERIFY2(!Kinefin::adminDashboardUrl(QString::fromLatin1(text)), text);
    }
};
QTEST_APPLESS_MAIN(TestLinkRouter)
#include "test_link_router.moc"
