// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integration/SpatialNavigation.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Kinefin {
namespace {
// A miss across the direction costs this many times a gap along it: a button off to the side loses to the row straight below
constexpr double kOrthogonalWeight = 4.0;
// The share of the span a candidate shares with the source that is taken off its score
constexpr double kOverlapWeight = 0.25;
// Scores and starts closer than this are a tie (device pixels are fractional in a scaled layout)
constexpr double kTieTolerance = 0.5;
// The walk of the item tree stops after this many items: a remote must not be able to make a flood of keys expensive
constexpr int kMaxWalkedItems = 5000;

// Rotates and mirrors a rectangle so that every direction becomes Down: y runs along the move, x across it
QRectF downFrame(const QRectF &rect, NavDirection direction)
{
    switch (direction) {
    case NavDirection::Up:
        return {rect.x(), -rect.bottom(), rect.width(), rect.height()};
    case NavDirection::Right:
        return {rect.y(), rect.x(), rect.height(), rect.width()};
    case NavDirection::Left:
        return {rect.y(), -rect.right(), rect.height(), rect.width()};
    case NavDirection::Down:
        break;
    }
    return rect;
}

QRectF sceneRect(QQuickItem *item)
{
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

QQuickItem *itemProperty(const QObject *object, const char *name)
{
    return object->property(name).value<QQuickItem *>();
}

// The viewport edge a key enters a scroll view from: the top for Down, the bottom for Up, the left for Right, the right for Left
QRectF entryEdge(const QRectF &viewport, NavDirection direction)
{
    switch (direction) {
    case NavDirection::Down:
        return {viewport.left(), viewport.top(), viewport.width(), 0};
    case NavDirection::Up:
        return {viewport.left(), viewport.bottom(), viewport.width(), 0};
    case NavDirection::Right:
        return {viewport.left(), viewport.top(), 0, viewport.height()};
    case NavDirection::Left:
        return {viewport.right(), viewport.top(), 0, viewport.height()};
    }
    return viewport;
}

QRectF sourceRect(QQuickItem *focus, NavDirection direction)
{
    const QRectF scene = sceneRect(focus);
    if (isScrollContainer(focus))
        return entryEdge(scene, direction);
    if (isItemView(focus)) {
        if (QQuickItem *current = itemProperty(focus, "currentItem");
            current && current->isVisible()) {
            const QRectF row = sceneRect(current).intersected(scene);
            if (!row.isEmpty())
                return row;
        }
    }
    return scene;
}

// The rows of an item view that show, as one rectangle: what a move from the view's header or footer enters
QRectF visibleRowsRect(QQuickItem *view)
{
    QQuickItem *content = itemProperty(view, "contentItem");
    if (!content)
        return {};
    QQuickItem *header = itemProperty(view, "headerItem");
    QQuickItem *footer = itemProperty(view, "footerItem");
    QQuickItem *highlight = itemProperty(view, "highlightItem");
    const QRectF viewport = sceneRect(view);
    QRectF rows;
    for (QQuickItem *child : content->childItems()) {
        if (child == header || child == footer || child == highlight || !child->isVisible()
            || child->width() < 1 || child->height() < 1)
            continue;
        const QRectF rect = sceneRect(child).intersected(viewport);
        if (!rect.isEmpty())
            rows = rows.isNull() ? rect : rows.united(rect);
    }
    return rows;
}

struct Walk {
    QQuickItem *root = nullptr;
    QSet<const QQuickItem *> focusChain;
    int remaining = kMaxWalkedItems;
    QList<QQuickItem *> items;
    QList<QRectF> rects;
    // Plain scroll containers that hold a candidate of their own: the contents are the better target
    QList<bool> dropped;
    // The mini player floats over the page and has no space of its own: what lies fully under it is out of reach
    QList<QQuickItem *> occluders;
};

void collect(Walk &walk, QQuickItem *item, QRectF clip, bool inFlickable)
{
    if (walk.remaining-- <= 0 || !item->isVisible())
        return;
    // A popup is searched as a root of its own, never as part of the page under it
    if (item != walk.root && item->inherits("QQuickPopupItem"))
        return;
    const bool flickable = item->inherits("QQuickFlickable");
    bool inFlickableBelow = inFlickable;
    QRectF clipBelow = clip;
    // The viewport of a flickable does not clip a target away, since the target can be revealed; any other clip does
    if (flickable) {
        if (!inFlickable && !clip.intersects(sceneRect(item)))
            return;
        inFlickableBelow = true;
    } else if (item->clip() && !inFlickable) {
        clipBelow = clip.intersected(sceneRect(item));
        if (clipBelow.isEmpty())
            return;
    }
    // The page row shows one page at a time (SingleColumn): a page that is not the current one is sliding out or kept under it, never a target, unless it holds the focus
    if (item != walk.root && !walk.focusChain.contains(item)) {
        if (const QVariant current = item->property("isCurrentPage");
            current.isValid() && !current.toBool())
            return;
    }
    if (item != walk.root && item->objectName() == QLatin1String("miniPlayer"))
        walk.occluders.append(item);

    qsizetype slot = -1;
    if (item != walk.root && item->isEnabled() && item->activeFocusOnTab() && item->width() >= 1
        && item->height() >= 1) {
        QRectF rect = sceneRect(item);
        bool eligible = inFlickable || clip.intersects(rect);
        if (eligible && walk.focusChain.contains(item)) {
            // An item view with the focus in its header or footer offers its rows; any other ancestor of the focus is no target
            QQuickItem *header = isItemView(item) ? itemProperty(item, "headerItem") : nullptr;
            QQuickItem *footer = isItemView(item) ? itemProperty(item, "footerItem") : nullptr;
            const bool inEdge = (header && walk.focusChain.contains(header))
                                || (footer && walk.focusChain.contains(footer));
            if (inEdge) {
                rect = visibleRowsRect(item);
                // A header as tall as the page (a season's) leaves no row in view: the current row stands for them, since the view reveals it
                if (rect.isEmpty()) {
                    if (QQuickItem *current = itemProperty(item, "currentItem"))
                        rect = sceneRect(current);
                }
                eligible = !rect.isEmpty();
            } else {
                eligible = false;
            }
        }
        if (eligible) {
            slot = walk.items.size();
            walk.items.append(item);
            walk.rects.append(rect);
            walk.dropped.append(false);
        }
    }

    QQuickItem *content = isItemView(item) ? itemProperty(item, "contentItem") : nullptr;
    for (QQuickItem *child : item->childItems()) {
        if (child != content) {
            collect(walk, child, clipBelow, inFlickableBelow);
            continue;
        }
        // The rows of an item view are not entered: the view is the target and keeps its own current row
        for (const char *edge : {"headerItem", "footerItem"}) {
            if (QQuickItem *part = itemProperty(item, edge))
                collect(walk, part, clipBelow, inFlickableBelow);
        }
    }
    if (slot >= 0 && isScrollContainer(item) && walk.items.size() > slot + 1)
        walk.dropped[slot] = true;
}

// The visible popup items under the root, topmost first: by z, then by order in the tree
void findPopups(QQuickItem *item, QList<QQuickItem *> &popups, int &remaining)
{
    if (remaining-- <= 0 || !item->isVisible())
        return;
    if (item->inherits("QQuickPopupItem")) {
        popups.append(item);
        return;
    }
    for (QQuickItem *child : item->childItems())
        findPopups(child, popups, remaining);
}

struct Found {
    QList<QQuickItem *> items;
    QList<QRectF> rects;
};

Found candidatesUnder(QQuickItem *root, QQuickItem *focus, QQuickWindow *window)
{
    Walk walk;
    walk.root = root;
    for (QQuickItem *item = focus; item; item = item->parentItem())
        walk.focusChain.insert(item);
    collect(walk, root, QRectF(0, 0, window->width(), window->height()), false);

    Found found;
    for (qsizetype i = 0; i < walk.items.size(); ++i) {
        if (walk.dropped.at(i))
            continue;
        const auto under = [&](QQuickItem *occluder) {
            return !occluder->isAncestorOf(walk.items.at(i))
                   && sceneRect(occluder).contains(walk.rects.at(i));
        };
        if (std::ranges::any_of(walk.occluders, under))
            continue;
        found.items.append(walk.items.at(i));
        found.rects.append(walk.rects.at(i));
    }
    return found;
}

double revealMargin()
{
    // Kirigami.Units.largeSpacing: twice a quarter of the height of an M
    const int gridUnit =
        QFontMetrics(QGuiApplication::font()).boundingRect(QLatin1Char('M')).height();
    return std::max(2, gridUnit / 4) * 2.0;
}

void revealAxis(QQuickItem *flickable, const char *position, const char *origin, const char *size,
                double view, double start, double end, double margin)
{
    if (view <= 0)
        return;
    const double current = flickable->property(position).toDouble();
    const double low = flickable->property(origin).toDouble();
    const double high = std::max(low, low + flickable->property(size).toDouble() - view);
    double wanted = current;
    if (end - start + 2 * margin > view || start - margin < current)
        wanted = start - margin;
    else if (end + margin > current + view)
        wanted = end + margin - view;
    wanted = std::clamp(wanted, low, high);
    if (std::abs(wanted - current) > 0.5)
        flickable->setProperty(position, wanted);
}

// The current row of an item view when it shows, else the row nearest the source
void enterItemView(QQuickItem *view, const QRectF &source)
{
    const QRectF viewport = sceneRect(view);
    QQuickItem *current = itemProperty(view, "currentItem");
    if (current && current->isVisible() && !sceneRect(current).intersected(viewport).isEmpty())
        return;
    QQuickItem *content = itemProperty(view, "contentItem");
    if (!content)
        return;
    QQuickItem *header = itemProperty(view, "headerItem");
    QQuickItem *footer = itemProperty(view, "footerItem");
    QQuickItem *highlight = itemProperty(view, "highlightItem");
    QQuickItem *nearest = nullptr;
    double nearestDistance = std::numeric_limits<double>::max();
    const QPointF from = source.center();
    for (QQuickItem *child : content->childItems()) {
        if (child == header || child == footer || child == highlight || !child->isVisible()
            || child->width() < 1 || child->height() < 1)
            continue;
        const QRectF rect = sceneRect(child);
        if (rect.intersected(viewport).isEmpty())
            continue;
        const double dx = std::max({rect.left() - from.x(), 0.0, from.x() - rect.right()});
        const double dy = std::max({rect.top() - from.y(), 0.0, from.y() - rect.bottom()});
        if (const double distance = dx * dx + dy * dy; distance < nearestDistance) {
            nearestDistance = distance;
            nearest = child;
        }
    }
    if (!nearest)
        return;
    const QPointF inContent = content->mapFromScene(sceneRect(nearest).center());
    int index = -1;
    if (QMetaObject::invokeMethod(view, "indexAt", Q_RETURN_ARG(int, index),
                                  Q_ARG(double, inContent.x()), Q_ARG(double, inContent.y()))
        && index >= 0)
        view->setProperty("currentIndex", index);
}
} // namespace

qsizetype chooseSpatialCandidate(const QRectF &source, NavDirection direction,
                                 const QList<QRectF> &candidates, Qt::LayoutDirection reading)
{
    const QRectF from = downFrame(source, direction);
    const bool entry = from.height() <= 0;
    const bool vertical = direction == NavDirection::Up || direction == NavDirection::Down;
    const bool mirrored = vertical && reading == Qt::RightToLeft;
    qsizetype best = -1;
    double bestScore = 0;
    double bestStart = 0;
    for (qsizetype i = 0; i < candidates.size(); ++i) {
        const QRectF to = downFrame(candidates.at(i), direction);
        if (to.width() < 1 || to.height() < 1)
            continue;
        if (entry) {
            if (to.bottom() <= from.bottom())
                continue;
        } else {
            if (to.contains(from)
                || to.top() < from.bottom() - 0.5 * std::min(from.height(), to.height()))
                continue;
        }
        const double gap = std::max(0.0, to.top() - from.bottom());
        const double overlap =
            std::max(0.0, std::min(to.right(), from.right()) - std::max(to.left(), from.left()));
        const double miss =
            overlap > 0 ? 0.0 : std::max(to.left() - from.right(), from.left() - to.right());
        const double score =
            entry ? gap : gap + kOrthogonalWeight * miss - kOverlapWeight * overlap;
        const double start = mirrored ? -to.right() : to.left();
        // Within the tolerance the reading start decides, and an exact tie goes to the later candidate
        const bool wins =
            best < 0 || score < bestScore - kTieTolerance
            || (score <= bestScore + kTieTolerance && start <= bestStart + kTieTolerance);
        if (wins) {
            best = i;
            bestScore = score;
            bestStart = start;
        }
    }
    return best;
}

QQuickItem *findSpatialTarget(QQuickItem *focus, NavDirection direction)
{
    QQuickWindow *window = focus ? focus->window() : nullptr;
    if (!window)
        return nullptr;
    const QRectF source = sourceRect(focus, direction);
    const Qt::LayoutDirection reading = QGuiApplication::layoutDirection();

    // Each walk has its own allowance
    QList<QQuickItem *> popups;
    int popupWalk = kMaxWalkedItems;
    findPopups(window->contentItem(), popups, popupWalk);
    // Topmost first; a popup with nothing to focus (a tooltip, a busy message) does not trap the keys unless the focus is in it
    std::stable_sort(popups.begin(), popups.end(),
                     [](const QQuickItem *a, const QQuickItem *b) { return a->z() < b->z(); });
    std::ranges::reverse(popups);
    for (QQuickItem *popup : std::as_const(popups)) {
        const Found found = candidatesUnder(popup, focus, window);
        if (found.items.isEmpty() && !popup->isAncestorOf(focus))
            continue;
        const qsizetype index = chooseSpatialCandidate(source, direction, found.rects, reading);
        return index >= 0 ? found.items.at(index) : nullptr;
    }
    const Found found = candidatesUnder(window->contentItem(), focus, window);
    const qsizetype index = chooseSpatialCandidate(source, direction, found.rects, reading);
    return index >= 0 ? found.items.at(index) : nullptr;
}

bool moveFocusSpatially(QQuickItem *focus, NavDirection direction)
{
    QPointer<QQuickWindow> window = focus ? focus->window() : nullptr;
    QQuickItem *target = window ? findSpatialTarget(focus, direction) : nullptr;
    if (!target)
        return false;
    QQuickItem *receiver = target;
    if (isItemView(target)) {
        enterItemView(target, sourceRect(focus, direction));
        // An item view is a focus scope that would hand the focus back to the header control that had it last: its current row takes it
        if (QQuickItem *current = itemProperty(target, "currentItem"))
            receiver = current;
    }
    const bool forward = direction == NavDirection::Down || direction == NavDirection::Right;
    receiver->forceActiveFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    QQuickItem *now = window ? window->activeFocusItem() : nullptr;
    if (!now)
        return false;
    revealItem(now);
    return now != focus;
}

void revealItem(QQuickItem *item)
{
    if (!item)
        return;
    const double margin = revealMargin();
    for (QQuickItem *parent = item->parentItem(); parent; parent = parent->parentItem()) {
        if (!parent->inherits("QQuickFlickable"))
            continue;
        QQuickItem *content = itemProperty(parent, "contentItem");
        // An item beside the content (a header that stays put, a scroll bar) does not scroll with it
        if (!content || !content->isAncestorOf(item))
            continue;
        const QRectF rect =
            item->mapRectToItem(content, QRectF(0, 0, item->width(), item->height()));
        revealAxis(parent, "contentY", "originY", "contentHeight", parent->height(), rect.top(),
                   rect.bottom(), margin);
        if (parent->property("contentWidth").toDouble() > parent->width() + 1)
            revealAxis(parent, "contentX", "originX", "contentWidth", parent->width(), rect.left(),
                       rect.right(), margin);
    }
}

bool isItemView(const QObject *item)
{
    return item && item->inherits("QQuickItemView");
}

bool isScrollContainer(const QObject *item)
{
    // A ListView or GridView is a Flickable too, but it moves its own current item with the arrows
    return item
           && (item->inherits("QQuickScrollView")
               || (item->inherits("QQuickFlickable") && !isItemView(item)));
}

bool isSingleLineField(const QObject *item)
{
    return item && item->inherits("QQuickTextInput");
}

QQuickItem *valueControlOf(QQuickItem *focus)
{
    // The control itself, or its text input when it is editable: a few steps up is enough
    int steps = 0;
    for (QQuickItem *item = focus; item && steps < 3; item = item->parentItem(), ++steps) {
        if (item->inherits("QQuickSpinBox") || item->inherits("QQuickDoubleSpinBox"))
            return item;
        if (item->inherits("QQuickComboBox")) {
            const auto *popup = item->property("popup").value<QObject *>();
            return popup && popup->property("visible").toBool() ? nullptr : item;
        }
    }
    return nullptr;
}

bool isComboBox(const QObject *control)
{
    return control && control->inherits("QQuickComboBox");
}

bool insideItemView(QQuickItem *focus)
{
    for (QQuickItem *item = focus; item; item = item->parentItem()) {
        if (isItemView(item))
            return true;
    }
    return false;
}
} // namespace Kinefin
