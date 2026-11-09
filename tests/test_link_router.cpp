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
                                "jellyfin://item/abcdef0123456789abcdef0123456789#fragment"}) {
            QVERIFY2(!Kinefin::parseItemLink(QString::fromLatin1(url)), url);
        }
    }
};
QTEST_APPLESS_MAIN(TestLinkRouter)
#include "test_link_router.moc"
