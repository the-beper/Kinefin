// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "PipAspect.h"

#include <QtTest>

using Kinefin::fitToAspect;

namespace {
constexpr qreal kWide = 16.0 / 9.0;
const QSize kNoBounds;
} // namespace

class TestPipAspect : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void aSideLeadsTheShapeByItsOwnDimension()
    {
        // The width leads on a vertical side of the window (left, right)
        QCOMPARE(fitToAspect({640, 400}, kWide, Qt::RightEdge, 0, kNoBounds), QSize(640, 360));
        QCOMPARE(fitToAspect({640, 400}, kWide, Qt::LeftEdge, 0, kNoBounds), QSize(640, 360));
        // The height leads on a horizontal one (top, bottom)
        QCOMPARE(fitToAspect({640, 300}, kWide, Qt::TopEdge, 0, kNoBounds), QSize(533, 300));
        QCOMPARE(fitToAspect({640, 300}, kWide, Qt::BottomEdge, 0, kNoBounds), QSize(533, 300));
    }

    void aCornerFollowsTheLargerOfTheTwo()
    {
        QCOMPARE(fitToAspect({600, 400}, kWide, Qt::BottomEdge | Qt::RightEdge, 0, kNoBounds),
                 QSize(711, 400));
        QCOMPARE(fitToAspect({900, 400}, kWide, Qt::TopEdge | Qt::LeftEdge, 0, kNoBounds),
                 QSize(900, 506));
    }

    void withoutAnEdgeTheWidthLeads()
    {
        QCOMPARE(fitToAspect({640, 500}, kWide, Qt::Edges(), 0, kNoBounds), QSize(640, 360));
    }

    void aVideoOfAnotherShapeGivesAnotherWindow()
    {
        // 2.39:1 is wide and short, 4:3 is nearly square
        QCOMPARE(fitToAspect({716, 400}, 2.39, Qt::RightEdge, 0, kNoBounds), QSize(716, 300));
        QCOMPARE(fitToAspect({640, 400}, 4.0 / 3.0, Qt::RightEdge, 0, kNoBounds), QSize(640, 480));
    }

    void noShapeLeavesTheSizeAlone()
    {
        QCOMPARE(fitToAspect({640, 400}, 0, Qt::RightEdge, 0, kNoBounds), QSize(640, 400));
        QCOMPARE(fitToAspect({640, 400}, -1, Qt::Edges(), 0, kNoBounds), QSize(640, 400));
    }

    void theMinimumWidthKeepsTheShape()
    {
        QCOMPARE(fitToAspect({200, 100}, 2.0, Qt::RightEdge, 288, kNoBounds), QSize(288, 144));
    }

    void theBoundsShrinkTheWindowKeepingItsShape()
    {
        QCOMPARE(fitToAspect({800, 600}, kWide, Qt::RightEdge, 0, {533, 290}), QSize(515, 290));
        // Without a shape too: the bounds are a limit of the protocol
        QCOMPARE(fitToAspect({1066, 580}, 0, Qt::Edges(), 0, {533, 290}), QSize(533, 290));
    }

    void theBoundsWinOverTheMinimum()
    {
        QCOMPARE(fitToAspect({100, 400}, 0.5, Qt::RightEdge, 288, {200, 300}), QSize(150, 300));
    }

    void aSizeWithoutBothDimensionsIsReturnedAsItIs()
    {
        QCOMPARE(fitToAspect({0, 300}, kWide, Qt::RightEdge, 288, {533, 290}), QSize(0, 300));
        QCOMPARE(fitToAspect({300, -1}, kWide, Qt::RightEdge, 288, {533, 290}), QSize(300, -1));
    }

    void noAskedSizeEverLeavesTheBoundsOrTheShape()
    {
        const QSize bounds(533, 290);
        const QList<Qt::Edges> sides{Qt::Edges(),
                                     Qt::LeftEdge,
                                     Qt::RightEdge,
                                     Qt::TopEdge,
                                     Qt::BottomEdge,
                                     Qt::TopEdge | Qt::LeftEdge,
                                     Qt::BottomEdge | Qt::RightEdge};
        for (const qreal aspect : {1.0, 4.0 / 3.0, kWide, 2.39}) {
            for (int width = 40; width <= 1400; width += 37) {
                for (int height = 40; height <= 900; height += 41) {
                    for (const Qt::Edges edges : sides) {
                        const QSize fitted =
                            fitToAspect({width, height}, aspect, edges, 288, bounds);
                        const QByteArray what = QByteArray::number(width) + "x"
                                                + QByteArray::number(height) + " aspect "
                                                + QByteArray::number(aspect);
                        QVERIFY2(fitted.width() >= 1 && fitted.height() >= 1, what.constData());
                        QVERIFY2(fitted.width() <= bounds.width()
                                     && fitted.height() <= bounds.height(),
                                 what.constData());
                        // A pixel of rounding on each side, and no more
                        QVERIFY2(qAbs(fitted.width() - fitted.height() * aspect) <= 2 + aspect,
                                 what.constData());
                    }
                }
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestPipAspect)
#include "test_pip_aspect.moc"
