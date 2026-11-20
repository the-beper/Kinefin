// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The application-wide input filter: mouse input on the full player and in the picture-in-picture window, and keyboard autorepeat while a setting is off (a text field keeps its own)
#include "app/InputFilter.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QtTest/QtTest>

using Kinefin::InputFilter;

namespace {
class TestWindow : public QWindow {
    Q_OBJECT
    Q_PROPERTY(QString playbackMode MEMBER playbackMode)
  public:
    QString playbackMode;
};

class Target : public QObject {
  public:
    int presses = 0;
    int moves = 0;
    int keys = 0;

  protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress)
            ++presses;
        else if (event->type() == QEvent::MouseMove)
            ++moves;
        else if (event->type() == QEvent::KeyPress)
            ++keys;
        return false;
    }
};

void sendPress(Target &target)
{
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), QPointF(1, 1),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &press);
}

void sendMove(Target &target)
{
    QMouseEvent move(QEvent::MouseMove, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2), Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&target, &move);
}
} // namespace

class TestInputFilter : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void mouseIsBlockedOnTheFullPlayerOnly()
    {
        InputFilter filter;
        QCoreApplication::instance()->installEventFilter(&filter);
        TestWindow window;
        Target target;
        target.setParent(&window);
        // Off: every event reaches the item
        sendPress(target);
        sendMove(target);
        QCOMPARE(target.presses, 1);
        QCOMPARE(target.moves, 1);
        // On with the full player: presses and moves are dropped
        window.playbackMode = QStringLiteral("full");
        filter.setMouseBlocked(true);
        sendPress(target);
        sendMove(target);
        QCOMPARE(target.presses, 1);
        QCOMPARE(target.moves, 1);
        // The mini player and browsing are not the full player
        window.playbackMode = QStringLiteral("mini");
        sendPress(target);
        QCOMPARE(target.presses, 2);
        // The picture-in-picture window is its own window
        TestWindow pip;
        pip.setObjectName(QStringLiteral("pictureInPictureWindow"));
        Target pipTarget;
        pipTarget.setParent(&pip);
        sendPress(pipTarget);
        QCOMPARE(pipTarget.presses, 0);
        // Off again
        filter.setMouseBlocked(false);
        sendPress(target);
        QCOMPARE(target.presses, 3);
    }

    void keyboardRepeatFollowsTheSetting()
    {
        InputFilter filter;
        QCoreApplication::instance()->installEventFilter(&filter);
        TestWindow window;
        Target target;
        target.setParent(&window);
        filter.setRepeatEnabled(false);
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier, QString(), true);
        QCoreApplication::sendEvent(&target, &repeat);
        QCOMPARE(target.keys, 0);
        QKeyEvent first(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier, QString(), false);
        QCoreApplication::sendEvent(&target, &first);
        QCOMPARE(target.keys, 1);
        filter.setRepeatEnabled(true);
        QCoreApplication::sendEvent(&target, &repeat);
        QCOMPARE(target.keys, 2);
    }
};

QTEST_MAIN(TestInputFilter)
#include "test_input_filter.moc"
