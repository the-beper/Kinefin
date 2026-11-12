// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include "player/Trickplay.h"

using namespace Kinefin;

namespace {
QJsonObject layout(int width = 320, int height = 180, int columns = 5, int rows = 4,
                   int images = 41, int interval = 10000)
{
    return {{QStringLiteral("Width"), width},           {QStringLiteral("Height"), height},
            {QStringLiteral("TileWidth"), columns},     {QStringLiteral("TileHeight"), rows},
            {QStringLiteral("ThumbnailCount"), images}, {QStringLiteral("Interval"), interval}};
}
QJsonObject nested(const QJsonObject &widths)
{
    return {{QStringLiteral("Trickplay"), QJsonObject{{QStringLiteral("media-source"), widths}}}};
}
} // namespace

class TestTrickplay : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void readsNestedMetadata()
    {
        const auto result = parseTrickplayLayout(nested({{QStringLiteral("320"), layout()}}),
                                                 QStringLiteral("media-source"));
        QVERIFY(result.has_value());
        QCOMPARE(result->width, 320);
        QCOMPARE(result->height, 180);
        QCOMPARE(result->columns, 5);
        QCOMPARE(result->rows, 4);
        QCOMPARE(result->imageCount, 41);
        QCOMPARE(result->intervalMs, 10000);
    }
    void supportsOlderWidthMaps()
    {
        const QJsonObject widths{{QStringLiteral("320"), layout()}};
        const QJsonObject flat{{QStringLiteral("Trickplay"), widths}};
        QVERIFY(parseTrickplayLayout(flat, QStringLiteral("missing-source")));
        const QJsonObject oneSource{
            {QStringLiteral("Trickplay"), QJsonObject{{QStringLiteral("other-source"), widths}}}};
        QVERIFY(parseTrickplayLayout(oneSource, QStringLiteral("missing-source")));
    }
    void picksClosestWidthTo320()
    {
        const auto result =
            parseTrickplayLayout(nested({{QStringLiteral("160"), layout(160, 90)},
                                         {QStringLiteral("320"), layout(320, 180)},
                                         {QStringLiteral("640"), layout(640, 360, 1, 1)}}),
                                 QStringLiteral("media-source"));
        QVERIFY(result);
        QCOMPARE(result->width, 320);
    }
    void rejectsInvalidMetadata()
    {
        const QList<QJsonObject> bad{
            layout(320, 0),
            layout(0, 180),
            layout(320, 180, 0),
            layout(320, 180, 5, 0),
            layout(320, 180, 5, 4, 0),
            layout(320, 180, 5, 4, 40, 0),
            layout(640, 360, 20, 20),
            layout(320, 180, 5, 4, 40, 100),
        };
        for (const QJsonObject &value : bad) {
            const auto result = parseTrickplayLayout(nested({{QStringLiteral("320"), value}}),
                                                     QStringLiteral("media-source"));
            QVERIFY2(!result, "untrusted thumbnail metadata must be rejected");
        }
    }
    void tileBoundaries()
    {
        const auto value = parseTrickplayLayout(nested({{QStringLiteral("320"), layout()}}),
                                                QStringLiteral("media-source"));
        QVERIFY(value);
        const auto first = trickplayFrameAt(*value, 0);
        QVERIFY(first);
        QCOMPARE(first->tileIndex, 0);
        QCOMPARE(first->column, 0);
        QCOMPARE(first->row, 0);
        const auto lastOnTile = trickplayFrameAt(*value, 19LL * 10 * 10'000'000);
        QVERIFY(lastOnTile);
        QCOMPARE(lastOnTile->tileIndex, 0);
        QCOMPARE(lastOnTile->column, 4);
        QCOMPARE(lastOnTile->row, 3);
        const auto nextTile = trickplayFrameAt(*value, 20LL * 10 * 10'000'000);
        QVERIFY(nextTile);
        QCOMPARE(nextTile->tileIndex, 1);
        QCOMPARE(nextTile->column, 0);
        QCOMPARE(nextTile->row, 0);
        QVERIFY(!trickplayFrameAt(*value, 41LL * 10 * 10'000'000));
        QVERIFY(!trickplayFrameAt(*value, -1));
    }
};
QTEST_APPLESS_MAIN(TestTrickplay)
#include "test_trickplay.moc"
