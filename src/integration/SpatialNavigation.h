// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QRectF>
#include <Qt>

class QObject;
class QQuickItem;

namespace Kinefin {
enum class NavDirection { Up, Down, Left, Right };

// The candidate to move to from `source`, an index into `candidates`, or -1. Lowest score wins: the gap along the direction, plus four times the miss across it, minus a quarter of the span shared with the source. A candidate counts only if its near edge is past the source's far edge minus half the smaller extent. Zero-size candidates and candidates that contain the source are skipped, except when the source has no extent along the direction (the entry edge of a scroll view), where the nearest to the edge wins by gap alone. Ties go to the reading start: the leading side for Up and Down, the top for Left and Right, then the later candidate (higher stacking)
qsizetype chooseSpatialCandidate(const QRectF &source, NavDirection direction,
                                 const QList<QRectF> &candidates, Qt::LayoutDirection reading);

// The tab-focusable item the focus moves to, searched in the topmost popup that has one, else in the window's content item; null when nothing lies that way. The walk skips hidden, disabled and clipped-away items and the delegates of item views, and stops after a fixed number of items
QQuickItem *findSpatialTarget(QQuickItem *focus, NavDirection direction);

// Moves the focus to the target: an item view keeps its current row when that is visible, the nearest visible row becomes current otherwise; the flickables around scroll the target into view. False when there is no target or the focus did not change
bool moveFocusSpatially(QQuickItem *focus, NavDirection direction);

// Scrolls every flickable above the item so the item shows whole with some room around it
void revealItem(QQuickItem *item);

bool isItemView(const QObject *item);
// A scroll view or a plain flickable: the keys would only scroll it
bool isScrollContainer(const QObject *item);
// A single-line text field (a TextField or a TextInput), as opposed to a text area
bool isSingleLineField(const QObject *item);
// The closed combo box or the spin box the focus belongs to (the control itself, or the text input of an editable one); null for anything else, and for a combo box whose popup is open
QQuickItem *valueControlOf(QQuickItem *focus);
// True when the item is a combo box, false for a spin box
bool isComboBox(const QObject *control);
// True when the focus is an item view or sits inside one
bool insideItemView(QQuickItem *focus);
} // namespace Kinefin
