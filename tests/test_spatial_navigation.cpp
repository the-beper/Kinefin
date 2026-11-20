// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The spatial engine: the pure scorer on rectangles, then the walk and the key dispatch on small offscreen Qt Quick scenes (Basic style)
#include "integration/RemoteInputServer.h"
#include "integration/SpatialNavigation.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest/QtTest>
#include <memory>

using Kinefin::findSpatialTarget;
using Kinefin::moveFocusSpatially;
using Kinefin::NavDirection;
using Kinefin::RemoteInputServer;

namespace {
// One QML scene in an application window, with the remote input server pointed at it
struct Scene {
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    RemoteInputServer input{QString(), QStringLiteral("test")};

    bool load(const char *source)
    {
        QQmlComponent component(&engine);
        component.setData(QByteArray("import QtQuick\nimport QtQuick.Controls\n") + source, QUrl());
        root.reset(component.create());
        if (!root) {
            qWarning("%s", qPrintable(component.errorString()));
            return false;
        }
        window = qobject_cast<QQuickWindow *>(root.get());
        if (!window)
            return false;
        window->show();
        input.setWindow(window);
        return QTest::qWaitForWindowActive(window);
    }

    QQuickItem *item(const char *name) const
    {
        return root->findChild<QQuickItem *>(QString::fromLatin1(name));
    }

    bool focus(const char *name) const
    {
        QQuickItem *target = item(name);
        if (!target)
            return false;
        target->forceActiveFocus(Qt::TabFocusReason);
        return target->hasActiveFocus();
    }

    // The named item that holds the focus: a list is a focus scope, so the active focus sits on its current delegate, which has no name
    QString focused() const
    {
        for (const QQuickItem *current = window->activeFocusItem(); current;
             current = current->parentItem()) {
            if (!current->objectName().isEmpty())
                return current->objectName();
        }
        return {};
    }

    bool press(const char *keycode, bool autoRepeat = false)
    {
        return input.dispatchKeycode(QString::fromLatin1(keycode), autoRepeat);
    }
};

const QList<QRectF> kNoCandidates;

// The index as an int, which QCOMPARE takes against a literal
int pick(const QRectF &source, NavDirection direction, const QList<QRectF> &candidates,
         Qt::LayoutDirection reading)
{
    return int(Kinefin::chooseSpatialCandidate(source, direction, candidates, reading));
}
} // namespace

class TestSpatialNavigation : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void downPrefersTheOverlappingRow()
    {
        const QRectF card(20, 100, 160, 100);
        // A button nearer in height but far to the side loses to the row straight below
        const QList<QRectF> candidates{QRectF(900, 210, 100, 30), QRectF(0, 220, 1100, 100)};
        QCOMPARE(pick(card, NavDirection::Down, candidates, Qt::LeftToRight), 1);
    }

    void upFromTheRightEndReachesTheRowButton()
    {
        // The card at the right end of a row, the button of the row's header above it, the row above that
        const QRectF card(900, 300, 160, 200);
        const QList<QRectF> candidates{QRectF(0, 100, 1100, 140), QRectF(1000, 250, 100, 40)};
        QCOMPARE(pick(card, NavDirection::Up, candidates, Qt::LeftToRight), 1);
        // From the first card the row above wins: the button is off to the side
        const QRectF first(20, 300, 160, 200);
        QCOMPARE(pick(first, NavDirection::Up, candidates, Qt::LeftToRight), 0);
    }

    void nothingThatWayChoosesNothing()
    {
        const QRectF source(100, 300, 100, 40);
        const QList<QRectF> above{QRectF(100, 100, 100, 40), QRectF(300, 0, 100, 40)};
        QCOMPARE(pick(source, NavDirection::Down, above, Qt::LeftToRight), -1);
        QCOMPARE(pick(source, NavDirection::Down, kNoCandidates, Qt::LeftToRight), -1);
        // Beside the source is not below it
        const QList<QRectF> beside{QRectF(300, 300, 100, 40), QRectF(-200, 300, 100, 40)};
        QCOMPARE(pick(source, NavDirection::Down, beside, Qt::LeftToRight), -1);
        QCOMPARE(pick(source, NavDirection::Up, beside, Qt::LeftToRight), -1);
    }

    void partialOverlapCountsAsThatWay()
    {
        const QRectF source(0, 0, 100, 40);
        // Past the middle of the source along the move: below it, although it starts above the source's bottom edge
        QCOMPARE(pick(source, NavDirection::Down, {QRectF(0, 30, 100, 40)}, Qt::LeftToRight), 0);
        // Less than halfway: beside it
        QCOMPARE(pick(source, NavDirection::Down, {QRectF(120, 10, 100, 40)}, Qt::LeftToRight), -1);
        // The same on the horizontal axis
        QCOMPARE(pick(source, NavDirection::Right, {QRectF(60, 0, 100, 40)}, Qt::LeftToRight), 0);
        QCOMPARE(pick(source, NavDirection::Right, {QRectF(20, 60, 100, 40)}, Qt::LeftToRight), -1);
    }

    void tiesGoToTheReadingStartInBothDirections()
    {
        // Two candidates the same distance below, one each side of the source: the leading side wins, whatever the order of the list
        const QRectF source(100, 0, 100, 40);
        const QRectF left(0, 100, 100, 40);
        const QRectF right(200, 100, 100, 40);
        QCOMPARE(pick(source, NavDirection::Down, {right, left}, Qt::LeftToRight), 1);
        QCOMPARE(pick(source, NavDirection::Down, {left, right}, Qt::LeftToRight), 0);
        QCOMPARE(pick(source, NavDirection::Down, {right, left}, Qt::RightToLeft), 0);
        QCOMPARE(pick(source, NavDirection::Down, {left, right}, Qt::RightToLeft), 1);
        // Left and Right: the top wins in either reading direction
        const QRectF side(0, 100, 100, 40);
        const QRectF above(200, 40, 100, 40);
        const QRectF below(200, 160, 100, 40);
        for (const Qt::LayoutDirection reading : {Qt::LeftToRight, Qt::RightToLeft}) {
            QCOMPARE(pick(side, NavDirection::Right, {below, above}, reading), 1);
            QCOMPARE(pick(side, NavDirection::Right, {above, below}, reading), 0);
        }
        // An exact tie of everything goes to the later candidate, the one stacked above
        QCOMPARE(pick(source, NavDirection::Down, {left, left}, Qt::LeftToRight), 1);
    }

    void zeroSizeAndContainingRectsAreSkipped()
    {
        const QRectF source(100, 100, 50, 50);
        const QRectF valid(100, 200, 50, 50);
        QCOMPARE(
            pick(source, NavDirection::Down,
                 {QRectF(100, 160, 0, 40), QRectF(100, 160, 40, 0), QRectF(0, 0, 500, 500), valid},
                 Qt::LeftToRight),
            3);
        QCOMPARE(pick(source, NavDirection::Down,
                      {QRectF(100, 160, 0, 40), QRectF(100, 160, 40, 0), QRectF(0, 0, 500, 500)},
                      Qt::LeftToRight),
                 -1);
    }

    void containerEntryPicksTheFirstBlock()
    {
        // The top edge of a scroll view: no extent along the move, so a candidate that holds the edge counts and the nearest block wins by distance alone
        const QRectF top(0, 0, 800, 0);
        const QRectF tile(0, 20, 200, 50);
        const QRectF second(220, 20, 200, 50);
        const QRectF row(0, 300, 800, 100);
        QCOMPARE(pick(top, NavDirection::Down, {row, second, tile}, Qt::LeftToRight), 2);
        QCOMPARE(pick(top, NavDirection::Down, {row, tile, second}, Qt::RightToLeft), 2);
        QCOMPARE(pick(top, NavDirection::Down, {row}, Qt::LeftToRight), 0);
        // A block cut by the edge is the first block
        QCOMPARE(pick(top, NavDirection::Down, {tile, QRectF(0, -30, 800, 60)}, Qt::LeftToRight),
                 1);
        // Everything above the edge is out
        QCOMPARE(pick(top, NavDirection::Down, {QRectF(0, -100, 800, 60)}, Qt::LeftToRight), -1);
        // From the bottom edge Up takes the block nearest to it; one that fills the whole view still counts
        const QRectF bottom(0, 600, 800, 0);
        QCOMPARE(pick(bottom, NavDirection::Up, {tile, row}, Qt::LeftToRight), 1);
        QCOMPARE(pick(bottom, NavDirection::Up, {QRectF(0, 0, 800, 600)}, Qt::LeftToRight), 0);
        // And from the side edges
        const QRectF left(0, 0, 0, 600);
        QCOMPARE(pick(left, NavDirection::Right,
                      {QRectF(400, 0, 100, 50), QRectF(50, 300, 100, 50)}, Qt::LeftToRight),
                 1);
        const QRectF right(800, 0, 0, 600);
        QCOMPARE(pick(right, NavDirection::Left,
                      {QRectF(400, 0, 100, 50), QRectF(650, 300, 100, 50)}, Qt::LeftToRight),
                 1);
    }

    void onlyTheTopmostPopupIsSearched()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 640; height: 480; visible: true
    Page {
        anchors.fill: parent
        Button { objectName: "page"; x: 20; y: 420; width: 120; height: 40 }
    }
    Popup {
        parent: Overlay.overlay
        x: 20; y: 20; width: 300; height: 300; z: 0
        modal: true; closePolicy: Popup.NoAutoClose; visible: true
        contentItem: Item { Button { objectName: "lower"; x: 10; y: 230; width: 100; height: 40 } }
    }
    Popup {
        parent: Overlay.overlay
        x: 20; y: 20; width: 300; height: 150; z: 1
        modal: true; closePolicy: Popup.NoAutoClose; visible: true
        contentItem: Item {
            Button { objectName: "upper1"; x: 10; y: 10; width: 100; height: 40 }
            Button { objectName: "upper2"; x: 10; y: 70; width: 100; height: 40 }
        }
    }
})"));
        QTRY_VERIFY(scene.item("upper2")->isVisible() && scene.item("lower")->isVisible());
        QVERIFY(scene.focus("upper1"));
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("upper2"));
        // Below the popup lie a button of the popup under it and one of the page: neither is reached
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("upper2"));
        QVERIFY(scene.press("up"));
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("upper1"));
    }

    void aPopupWithNothingToFocusDoesNotTrapTheKeys()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 640; height: 480; visible: true
    Page {
        anchors.fill: parent
        Button { objectName: "first"; x: 20; y: 20; width: 120; height: 40 }
        Button { objectName: "second"; x: 20; y: 100; width: 120; height: 40 }
    }
    Popup {
        parent: Overlay.overlay
        x: 300; y: 300; width: 100; height: 40
        modal: false; closePolicy: Popup.NoAutoClose; visible: true
        contentItem: Label { text: "Saved" }
    }
})"));
        QVERIFY(scene.focus("first"));
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("second"));
    }

    void hiddenDisabledAndClippedItemsAreSkipped()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 640; height: 480; visible: true
    Page {
        anchors.fill: parent
        Button { objectName: "top"; x: 10; y: 10; width: 100; height: 30 }
        Button { objectName: "hidden"; x: 10; y: 60; width: 100; height: 30; visible: false }
        Button { objectName: "disabled"; x: 10; y: 100; width: 100; height: 30; enabled: false }
        Item {
            x: 10; y: 140; width: 100; height: 10; clip: true
            Button { objectName: "clipped"; y: 20; width: 100; height: 30 }
        }
        Button { objectName: "far"; x: 10; y: 300; width: 100; height: 30 }
        Button { objectName: "offWindow"; x: 900; y: 300; width: 100; height: 30 }
    }
})"));
        QQuickItem *top = scene.item("top");
        QQuickItem *far = scene.item("far");
        QVERIFY(top && far);
        QCOMPARE(findSpatialTarget(top, NavDirection::Down), far);
        QCOMPARE(findSpatialTarget(far, NavDirection::Up), top);
        // The only item to the right lies outside the window and outside any flickable
        QCOMPARE(findSpatialTarget(far, NavDirection::Right), nullptr);
    }

    void aTargetInsideAFlickableIsRevealed()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 400; height: 300; visible: true
    Button { objectName: "above"; x: 10; y: 10; width: 100; height: 30 }
    Flickable {
        objectName: "flick"
        x: 0; y: 60; width: 400; height: 120
        contentWidth: 400; contentHeight: 600; clip: true
        Button { objectName: "inside"; x: 10; y: 400; width: 100; height: 30 }
    }
})"));
        QVERIFY(scene.focus("above"));
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("inside"));
        QQuickItem *inside = scene.item("inside");
        QQuickItem *flick = scene.item("flick");
        QVERIFY(flick->property("contentY").toDouble() > 0);
        const QRectF shown =
            inside->mapRectToScene(QRectF(0, 0, inside->width(), inside->height()));
        QVERIFY(shown.top() >= 60 && shown.bottom() <= 180);
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("above"));
    }

    void aListHeaderButtonMovesAndTheListKeepsItsRow()
    {
        Scene scene;
        QVERIFY(scene.load(kHeaderList));
        QQuickItem *list = scene.item("list");
        QVERIFY(list);
        QCOMPARE(list->property("currentIndex").toInt(), 2);
        QVERIFY(scene.focus("first"));
        // The list would take the Down for its own row: the engine moves the focus to the next control instead
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("second"));
        QCOMPARE(list->property("currentIndex").toInt(), 2);
        // From the last control of the header the rows are next, with the row the list had
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("list"));
        QCOMPARE(list->property("currentIndex").toInt(), 2);
        // Inside the list the arrows are its own
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("list"));
        QCOMPARE(list->property("currentIndex").toInt(), 3);
        // The guard is gone after the key: a real Down on a header button reaches the list, which moves its row
        QVERIFY(scene.focus("first"));
        QTest::keyClick(scene.window, Qt::Key_Down);
        QCOMPARE(list->property("currentIndex").toInt(), 4);
        // Up inside the list moves its row, and from the first row the list gives the key up and the header takes the focus
        QVERIFY(scene.focus("list"));
        for (int i = 0; i < 4; ++i)
            QVERIFY(scene.press("up"));
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("second"));
        QCOMPARE(list->property("currentIndex").toInt(), 0);
    }

    void selectPressesAListHeaderButton()
    {
        Scene scene;
        QVERIFY(scene.load(kHeaderList));
        QQuickItem *list = scene.item("list");
        QVERIFY(list);
        QVERIFY(scene.focus("first"));
        // The list opens its current row on Return: the button, not the row, gets the Select
        QVERIFY(scene.press("select"));
        QTRY_COMPARE(scene.root->property("clicks").toInt(), 1);
        QCOMPARE(scene.root->property("opened").toInt(), 0);
        // With the focus in the list (on its current row: the list is a focus scope that would give it back to the button), Select opens the current row
        auto *row = list->property("currentItem").value<QQuickItem *>();
        QVERIFY(row);
        row->forceActiveFocus(Qt::TabFocusReason);
        QVERIFY(row->hasActiveFocus());
        QVERIFY(scene.press("select"));
        QCOMPARE(scene.root->property("opened").toInt(), 1);
        QCOMPARE(scene.root->property("clicks").toInt(), 1);
    }

    void aSeekingPageStillGetsAnArrowWithNoTarget()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 400; height: 300; visible: true
    property int seeks: 0
    Page {
        anchors.fill: parent
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Right) {
                seeks++;
                event.accepted = true;
            }
        }
        Button { objectName: "only"; x: 10; y: 10; width: 100; height: 30 }
        Button { objectName: "next"; x: 200; y: 10; width: 100; height: 30 }
    }
})"));
        QVERIFY(scene.focus("only"));
        // A target lies that way: the page does not get the key
        QVERIFY(scene.press("right"));
        QCOMPARE(scene.focused(), QStringLiteral("next"));
        QCOMPARE(scene.root->property("seeks").toInt(), 0);
        // None does: the key goes on to the page, as it would have without the engine
        QVERIFY(scene.press("right"));
        QCOMPARE(scene.focused(), QStringLiteral("next"));
        QCOMPARE(scene.root->property("seeks").toInt(), 1);
    }

    void autoRepeatStaysInAView()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 400; height: 300; visible: true
    Page {
        anchors.fill: parent
        ListView {
            objectName: "list"
            x: 10; y: 10; width: 200; height: 100; clip: true
            model: 3; currentIndex: 2
            activeFocusOnTab: true; keyNavigationEnabled: true
            delegate: ItemDelegate { width: 200; height: 30; text: "row " + index; focusPolicy: Qt.NoFocus }
        }
        Button { objectName: "below"; x: 10; y: 150; width: 100; height: 30 }
    }
})"));
        QVERIFY(scene.focus("list"));
        // A held Down ends at the last row
        QVERIFY(scene.press("down", true));
        QCOMPARE(scene.focused(), QStringLiteral("list"));
        QVERIFY(scene.press("down", true));
        QCOMPARE(scene.focused(), QStringLiteral("list"));
        QCOMPARE(scene.item("list")->property("currentIndex").toInt(), 2);
        // A fresh press leaves it
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("below"));
    }

    void aRowWithoutACurrentCardLeavesFromItsStart()
    {
        // The geometry of Home: a library tile above the first row's start, the row's See All above its far end
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 1000; height: 600; visible: true
    Page {
        anchors.fill: parent
        Button { objectName: "tile"; x: 18; y: 86; width: 288; height: 169 }
        Button { objectName: "seeAll"; x: 888; y: 284; width: 80; height: 25 }
        ListView {
            objectName: "row"
            x: 14; y: 313; width: 958; height: 200; clip: true
            orientation: ListView.Horizontal
            model: 5; currentIndex: -1
            activeFocusOnTab: true; keyNavigationEnabled: true
            delegate: ItemDelegate { width: 252; height: 200; focusPolicy: Qt.NoFocus }
        }
    }
})"));
        QQuickItem *row = scene.item("row");
        QVERIFY(row);
        // The cards of a Home row arrive a few per frame: the focus can reach the row before its current card does
        QVERIFY(scene.focus("row"));
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("tile"));
        // With no card built at all, the start of the row stands for it
        row->setProperty("model", 0);
        QVERIFY(scene.focus("row"));
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("tile"));
    }

    void aClosedComboBoxGivesUpDownToNavigation()
    {
        Scene scene;
        QVERIFY(scene.load(kValueControls));
        QVERIFY(scene.focus("combo"));
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("spin"));
        QCOMPARE(scene.item("combo")->property("currentIndex").toInt(), 0);
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("combo"));
        QCOMPARE(scene.item("combo")->property("currentIndex").toInt(), 0);
        // Left and Right are not the combo box's: nothing is beside it
        QVERIFY(scene.press("right"));
        QCOMPARE(scene.focused(), QStringLiteral("combo"));
        QCOMPARE(scene.item("combo")->property("currentIndex").toInt(), 0);
    }

    void selectOpensAClosedComboBox()
    {
        Scene scene;
        QVERIFY(scene.load(kValueControls));
        QVERIFY(scene.focus("combo"));
        QVERIFY(scene.press("select"));
        const auto *popup = scene.item("combo")->property("popup").value<QObject *>();
        QVERIFY(popup);
        QTRY_VERIFY(popup->property("visible").toBool());
        // The open list takes the arrows for itself: Down moves its highlight and the control below stays out of reach
        QVERIFY(scene.press("down"));
        QVERIFY(scene.item("spin")->property("activeFocus").toBool() == false);
    }

    void aSpinBoxStepsWithLeftAndRight()
    {
        Scene scene;
        QVERIFY(scene.load(kValueControls));
        QVERIFY(scene.focus("spin"));
        QCOMPARE(scene.item("spin")->property("value").toInt(), 5);
        QVERIFY(scene.press("right"));
        QCOMPARE(scene.item("spin")->property("value").toInt(), 6);
        QVERIFY(scene.press("left"));
        QVERIFY(scene.press("left"));
        QCOMPARE(scene.item("spin")->property("value").toInt(), 4);
        // Up and Down move the focus and leave the value
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("below"));
        QCOMPARE(scene.item("spin")->property("value").toInt(), 4);
        QVERIFY(scene.press("up"));
        QCOMPARE(scene.focused(), QStringLiteral("spin"));
        QCOMPARE(scene.item("spin")->property("value").toInt(), 4);
    }

    void aSingleLineFieldLeavesWithUpAndDownOnly()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    width: 400; height: 300; visible: true
    Page {
        anchors.fill: parent
        TextField { objectName: "field"; x: 10; y: 10; width: 200; height: 32; text: "abc" }
        TextArea { objectName: "area"; x: 10; y: 60; width: 200; height: 60; text: "abc" }
        Button { objectName: "below"; x: 10; y: 150; width: 100; height: 30 }
    }
})"));
        QVERIFY(scene.focus("field"));
        QVERIFY(scene.press("left"));
        QVERIFY(scene.press("right"));
        QCOMPARE(scene.focused(), QStringLiteral("field"));
        QVERIFY(scene.press("down"));
        QCOMPARE(scene.focused(), QStringLiteral("area"));
        // A text area keeps every arrow
        for (const char *keycode : {"down", "up", "left", "right"}) {
            QVERIFY(scene.press(keycode));
            QCOMPARE(scene.focused(), QStringLiteral("area"));
        }
        // Select in a field is the field's own: no stray space
        QVERIFY(scene.press("select"));
        QVERIFY(!scene.item("area")->property("text").toString().contains(QLatin1Char(' ')));
    }

    void aFocusedScrollViewEntersItsFirstBlock()
    {
        // A scroll view is a focus scope that hands the focus back to the child that had it: each case starts from a fresh page, whose scroll view has it first, as a page's does
        static constexpr const char *kScroll = R"(
ApplicationWindow {
    width: 400; height: 300; visible: true
    Page {
        anchors.fill: parent
        ScrollView {
            objectName: "scroll"
            x: 0; y: 0; width: 400; height: 200
            activeFocusOnTab: true
            Column {
                Button { objectName: "a"; width: 120; height: 30 }
                Button { objectName: "b"; width: 120; height: 30 }
            }
        }
    }
})";
        const auto entersWith = [](const char *keycode) {
            Scene scene;
            if (!scene.load(kScroll) || !scene.focus("scroll"))
                return QStringLiteral("(no scene)");
            scene.press(keycode);
            return scene.focused();
        };
        QCOMPARE(entersWith("down"), QStringLiteral("a"));
        QCOMPARE(entersWith("up"), QStringLiteral("b"));
        // Select on the scroll view enters its page too
        QCOMPARE(entersWith("select"), QStringLiteral("a"));
    }

    void theWalkStopsAfterTheItemCap()
    {
        Scene scene;
        QVERIFY(scene.load(R"(
ApplicationWindow {
    id: win
    width: 400; height: 300; visible: true
    property int filler: 6000
    Button { objectName: "start"; x: 10; y: 10; width: 100; height: 30 }
    Item { Repeater { model: win.filler; Item { width: 1; height: 1 } } }
    Button { objectName: "late"; x: 10; y: 200; width: 100; height: 30 }
})"));
        QQuickItem *start = scene.item("start");
        QQuickItem *late = scene.item("late");
        QVERIFY(start && late);
        QCOMPARE(findSpatialTarget(start, NavDirection::Down), nullptr);
        QVERIFY(scene.root->setProperty("filler", 50));
        QTRY_COMPARE(findSpatialTarget(start, NavDirection::Down), late);
    }

    void aFocusedItemOutsideAQuickWindowMovesNothing()
    {
        QCOMPARE(findSpatialTarget(nullptr, NavDirection::Down), nullptr);
        QVERIFY(!moveFocusSpatially(nullptr, NavDirection::Down));
        QQuickItem loose;
        QCOMPARE(findSpatialTarget(&loose, NavDirection::Down), nullptr);
    }

  private:
    // A list whose whole page is its header, as the season page is: two buttons, then the rows
    static constexpr const char *kHeaderList = R"(
ApplicationWindow {
    id: win
    width: 600; height: 500; visible: true
    property int clicks: 0
    property int opened: 0
    Page {
        anchors.fill: parent
        ListView {
            id: list
            objectName: "list"
            anchors.fill: parent
            model: 8; currentIndex: 2
            activeFocusOnTab: true; keyNavigationEnabled: true
            Keys.onReturnPressed: win.opened++
            header: Item {
                width: list.width; height: 160
                Button { objectName: "first"; x: 10; y: 10; width: 120; height: 40; onClicked: win.clicks++ }
                Button { objectName: "second"; x: 10; y: 70; width: 120; height: 40 }
            }
            delegate: ItemDelegate { width: list.width; height: 50; text: "row " + index; focusPolicy: Qt.NoFocus }
        }
    }
})";

    static constexpr const char *kValueControls = R"(
ApplicationWindow {
    width: 400; height: 300; visible: true
    Page {
        anchors.fill: parent
        ComboBox { objectName: "combo"; x: 10; y: 10; width: 160; height: 32; model: ["a", "b", "c"]; currentIndex: 0 }
        SpinBox { objectName: "spin"; x: 10; y: 60; width: 160; height: 32; from: 0; to: 10; value: 5 }
        Button { objectName: "below"; x: 10; y: 120; width: 100; height: 30 }
    }
})";
};

int main(int argc, char **argv)
{
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QGuiApplication app(argc, argv);
    TestSpatialNavigation test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_spatial_navigation.moc"
