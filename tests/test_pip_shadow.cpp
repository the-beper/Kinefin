// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PipShadow.h"

#include <QtTest>

using Kinefin::PipShadow;
using Kinefin::pipShadow;

namespace {
constexpr qreal kSize = 18;
constexpr qreal kOffset = 4;
constexpr qreal kOpacity = 0.45;

int alphaAt(const QImage &image, int x, int y)
{
    return qAlpha(image.pixel(x, y));
}

QList<QImage> tilesOf(const PipShadow &shadow)
{
    return {shadow.topLeft,     shadow.top,    shadow.topRight,   shadow.right,
            shadow.bottomRight, shadow.bottom, shadow.bottomLeft, shadow.left};
}

// The ratios of the screens: whole, fractional and large
void addRatios()
{
    QTest::addColumn<qreal>("ratio");
    QTest::newRow("1") << 1.0;
    QTest::newRow("1.25") << 1.25;
    QTest::newRow("1.5") << 1.5;
    QTest::newRow("2") << 2.0;
}

// Whether the alpha never rises along (dx, dy), which points away from the window
bool fallsAway(const QImage &tile, int dx, int dy)
{
    for (int y = 0; y < tile.height(); ++y) {
        for (int x = 0; x < tile.width(); ++x) {
            const int nextX = x + dx;
            const int nextY = y + dy;
            if (nextX >= 0 && nextX < tile.width() && nextY >= 0 && nextY < tile.height()
                && alphaAt(tile, nextX, nextY) > alphaAt(tile, x, y))
                return false;
        }
    }
    return true;
}

// Whether every pixel of a corner is the product of the edge tile of its column (left or right) and the one of its row (top or bottom), in units of the densest alpha, within the rounding of three 8-bit alphas
bool isProductOfEdges(const QImage &corner, const QImage &columnEdge, const QImage &rowEdge,
                      qreal peak)
{
    for (int y = 0; y < corner.height(); ++y) {
        for (int x = 0; x < corner.width(); ++x) {
            const qreal product = alphaAt(columnEdge, x, 0) * alphaAt(rowEdge, 0, y) / peak;
            if (qAbs(alphaAt(corner, x, y) - product) > 2)
                return false;
        }
    }
    return true;
}
} // namespace

class TestPipShadow : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void paddingIsRoundedUpToWholePixels_data()
    {
        QTest::addColumn<qreal>("size");
        QTest::addColumn<qreal>("offset");
        QTest::addColumn<QMarginsF>("padding");
        QTest::newRow("whole") << 18.0 << 4.0 << QMarginsF(18, 14, 18, 22);
        QTest::newRow("fractional") << 17.2 << 4.3 << QMarginsF(18, 13, 18, 22);
        QTest::newRow("no offset") << 18.0 << 0.0 << QMarginsF(18, 18, 18, 18);
        QTest::newRow("upward") << 18.0 << -4.0 << QMarginsF(18, 22, 18, 14);
    }
    void paddingIsRoundedUpToWholePixels()
    {
        QFETCH(qreal, size);
        QFETCH(qreal, offset);
        QFETCH(QMarginsF, padding);
        QCOMPARE(pipShadow(size, offset, kOpacity, 1).padding, padding);
    }

    void tilesHaveTheSizesThePaddingImplies_data() { addRatios(); }
    void tilesHaveTheSizesThePaddingImplies()
    {
        QFETCH(qreal, ratio);
        const PipShadow shadow = pipShadow(kSize, kOffset, kOpacity, ratio);
        const QMarginsF padding = shadow.padding;
        const auto pixels = [ratio](qreal logical) { return qRound(logical * ratio); };
        const int left = pixels(padding.left());
        const int top = pixels(padding.top());
        const int right = pixels(padding.right());
        const int bottom = pixels(padding.bottom());
        QCOMPARE(shadow.topLeft.size(), QSize(left, top));
        QCOMPARE(shadow.top.size(), QSize(1, top));
        QCOMPARE(shadow.topRight.size(), QSize(right, top));
        QCOMPARE(shadow.right.size(), QSize(right, 1));
        QCOMPARE(shadow.bottomRight.size(), QSize(right, bottom));
        QCOMPARE(shadow.bottom.size(), QSize(1, bottom));
        QCOMPARE(shadow.bottomLeft.size(), QSize(left, bottom));
        QCOMPARE(shadow.left.size(), QSize(left, 1));
        for (const QImage &tile : tilesOf(shadow))
            QCOMPARE(tile.format(), QImage::Format_ARGB32_Premultiplied);
    }

    void edgeTilesAreOnePixelThick_data() { addRatios(); }
    void edgeTilesAreOnePixelThick()
    {
        QFETCH(qreal, ratio);
        const PipShadow shadow = pipShadow(kSize, kOffset, kOpacity, ratio);
        QCOMPARE(shadow.top.width(), 1);
        QCOMPARE(shadow.bottom.width(), 1);
        QCOMPARE(shadow.left.height(), 1);
        QCOMPARE(shadow.right.height(), 1);
    }

    void alphaFallsAwayFromTheWindow_data() { addRatios(); }
    void alphaFallsAwayFromTheWindow()
    {
        QFETCH(qreal, ratio);
        const PipShadow shadow = pipShadow(kSize, kOffset, kOpacity, ratio);
        QVERIFY(fallsAway(shadow.left, -1, 0));
        QVERIFY(fallsAway(shadow.top, 0, -1));
        QVERIFY(fallsAway(shadow.right, 1, 0));
        QVERIFY(fallsAway(shadow.bottom, 0, 1));
        QVERIFY(fallsAway(shadow.topLeft, -1, 0) && fallsAway(shadow.topLeft, 0, -1));
        QVERIFY(fallsAway(shadow.topRight, 1, 0) && fallsAway(shadow.topRight, 0, -1));
        QVERIFY(fallsAway(shadow.bottomRight, 1, 0) && fallsAway(shadow.bottomRight, 0, 1));
        QVERIFY(fallsAway(shadow.bottomLeft, -1, 0) && fallsAway(shadow.bottomLeft, 0, 1));
        // The shadow is there next to the window and gone at its outer border
        QVERIFY(alphaAt(shadow.left, shadow.left.width() - 1, 0) > 10);
        QVERIFY(alphaAt(shadow.left, 0, 0) <= 1);
        QVERIFY(alphaAt(shadow.top, 0, 0) <= 1);
        QVERIFY(alphaAt(shadow.right, shadow.right.width() - 1, 0) <= 1);
        QVERIFY(alphaAt(shadow.bottom, 0, shadow.bottom.height() - 1) <= 1);
        QVERIFY(alphaAt(shadow.topLeft, 0, 0) <= 1);
        QVERIFY(alphaAt(shadow.bottomRight, shadow.bottomRight.width() - 1,
                        shadow.bottomRight.height() - 1)
                <= 1);
    }

    void aCornerIsTheProductOfItsTwoEdges_data() { addRatios(); }
    void aCornerIsTheProductOfItsTwoEdges()
    {
        QFETCH(qreal, ratio);
        const PipShadow shadow = pipShadow(kSize, kOffset, kOpacity, ratio);
        const qreal peak = 255 * kOpacity;
        QVERIFY(isProductOfEdges(shadow.topLeft, shadow.left, shadow.top, peak));
        QVERIFY(isProductOfEdges(shadow.topRight, shadow.right, shadow.top, peak));
        QVERIFY(isProductOfEdges(shadow.bottomRight, shadow.right, shadow.bottom, peak));
        QVERIFY(isProductOfEdges(shadow.bottomLeft, shadow.left, shadow.bottom, peak));
    }

    void noAlphaExceedsTheOpacity_data()
    {
        QTest::addColumn<qreal>("opacity");
        QTest::addColumn<int>("limit");
        QTest::newRow("0.45") << 0.45 << 115;
        QTest::newRow("1") << 1.0 << 255;
        QTest::newRow("0.1") << 0.1 << 26;
        QTest::newRow("above 1") << 2.0 << 255;
    }
    void noAlphaExceedsTheOpacity()
    {
        QFETCH(qreal, opacity);
        QFETCH(int, limit);
        int densest = 0;
        for (const QImage &tile : tilesOf(pipShadow(kSize, kOffset, opacity, 1))) {
            for (int y = 0; y < tile.height(); ++y) {
                for (int x = 0; x < tile.width(); ++x)
                    densest = qMax(densest, alphaAt(tile, x, y));
            }
        }
        QVERIFY(densest > 0);
        QVERIFY2(densest <= limit, qPrintable(QString::number(densest)));
    }

    void theOffsetMakesTheBottomLargerThanTheTop()
    {
        const PipShadow shadow = pipShadow(kSize, kOffset, kOpacity, 1);
        QCOMPARE(shadow.padding.top(), kSize - kOffset);
        QCOMPARE(shadow.padding.bottom(), kSize + kOffset);
        QCOMPARE(shadow.padding.left(), kSize);
        QCOMPARE(shadow.padding.right(), kSize);
        QVERIFY(shadow.padding.bottom() > shadow.padding.top());
        // The shadow is denser at the foot of the window than at its head
        QVERIFY(alphaAt(shadow.bottom, 0, 0) > alphaAt(shadow.top, 0, shadow.top.height() - 1));
    }

    void withoutAnOffsetTheShadowIsSymmetric()
    {
        const PipShadow shadow = pipShadow(kSize, 0, kOpacity, 1);
        QCOMPARE(shadow.padding, QMarginsF(kSize, kSize, kSize, kSize));
        const int length = shadow.top.height();
        QCOMPARE(shadow.bottom.height(), length);
        for (int i = 0; i < length; ++i) {
            QCOMPARE(alphaAt(shadow.top, 0, i), alphaAt(shadow.bottom, 0, length - 1 - i));
            QCOMPARE(alphaAt(shadow.left, i, 0), alphaAt(shadow.right, length - 1 - i, 0));
        }
    }

    void theDevicePixelRatioScalesTheImagesNotThePadding()
    {
        const PipShadow one = pipShadow(kSize, kOffset, kOpacity, 1);
        const PipShadow two = pipShadow(kSize, kOffset, kOpacity, 2);
        QCOMPARE(two.padding, one.padding);
        QCOMPARE(two.topLeft.size(), one.topLeft.size() * 2);
        QCOMPARE(two.top.size(), QSize(1, one.top.height() * 2));
        QCOMPARE(two.right.size(), QSize(one.right.width() * 2, 1));
        QCOMPARE(one.left.devicePixelRatio(), 1.0);
        QCOMPARE(two.left.devicePixelRatio(), 2.0);
        // The same profile sampled twice as often: two pixels of the large tile add up to about two of the small one
        for (int x = 0; x < one.left.width(); ++x) {
            const int pair = alphaAt(two.left, 2 * x, 0) + alphaAt(two.left, 2 * x + 1, 0);
            QVERIFY2(qAbs(pair - 2 * alphaAt(one.left, x, 0)) <= 3, qPrintable(QString::number(x)));
        }
    }

    void nothingShowsWithoutASizeOrAnOpacity_data()
    {
        QTest::addColumn<qreal>("size");
        QTest::addColumn<qreal>("offset");
        QTest::addColumn<qreal>("opacity");
        QTest::newRow("no size") << 0.0 << 4.0 << 0.45;
        QTest::newRow("negative size") << -18.0 << 0.0 << 0.45;
        QTest::newRow("no opacity") << 18.0 << 4.0 << 0.0;
        QTest::newRow("offset as large as the size") << 18.0 << 18.0 << 0.45;
        QTest::newRow("offset larger upward") << 18.0 << -20.0 << 0.45;
        QTest::newRow("size not a number") << qQNaN() << 4.0 << 0.45;
        QTest::newRow("offset not a number") << 18.0 << qQNaN() << 0.45;
    }
    void nothingShowsWithoutASizeOrAnOpacity()
    {
        QFETCH(qreal, size);
        QFETCH(qreal, offset);
        QFETCH(qreal, opacity);
        const PipShadow shadow = pipShadow(size, offset, opacity, 1);
        for (const QImage &tile : tilesOf(shadow))
            QVERIFY(tile.isNull());
        QVERIFY(shadow.padding.isNull());
    }
};

QTEST_GUILESS_MAIN(TestPipShadow)
#include "test_pip_shadow.moc"
