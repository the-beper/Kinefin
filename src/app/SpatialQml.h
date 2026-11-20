// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "integration/SpatialNavigation.h"

#include <QObject>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

namespace Kinefin {
// The spatial navigation engine for QML: the keys of a row or a tile that leave it move by screen position, as the gamepad's arrows do
class SpatialQml : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Spatial)
    QML_SINGLETON

  public:
    explicit SpatialQml(QObject *parent = nullptr) : QObject(parent) {}

    // Moves the focus from `from` in the direction of an arrow key (Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right); false when nothing lies that way or the key is no arrow
    Q_INVOKABLE bool move(QQuickItem *from, int key) const
    {
        switch (key) {
        case Qt::Key_Up:
            return moveFocusSpatially(from, NavDirection::Up);
        case Qt::Key_Down:
            return moveFocusSpatially(from, NavDirection::Down);
        case Qt::Key_Left:
            return moveFocusSpatially(from, NavDirection::Left);
        case Qt::Key_Right:
            return moveFocusSpatially(from, NavDirection::Right);
        default:
            return false;
        }
    }

    // Scrolls the flickables above the item so it shows whole
    Q_INVOKABLE void reveal(QQuickItem *item) const { revealItem(item); }
};
} // namespace Kinefin
