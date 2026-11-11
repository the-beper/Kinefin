// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The real Main.qml of the application, loaded from the QML module offscreen against the fake server: signed in, the Home page with its library tiles and rows, the library grid, its empty and failed states, plain-text rendering of server metadata and the return to sign-in. Any QML warning fails a test except Kirigami's own noise (the same list as tests/shell_smoke.py: keep them in step)
#include "FakeJellyfin.h"
#include "MemoryTokenStore.h"

#include "app/ConnectionController.h"
#include "app/ImageNetworkFactory.h"
#include "imagecache/ImageOrigin.h"
#include "settings/ClientSettings.h"

#include <KLocalizedQmlContext>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlListReference>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <functional>

using Kinefin::ConnectionController;

namespace {
QStringList gWarnings;
// The message handler also runs on QML's network thread
QMutex gWarningsMutex;
QtMessageHandler gPreviousHandler = nullptr;

// Kirigami's PageRow warns about every pushed page and its AboutPage reports binding loops: inferred harmless, everything else fails the test.
// The animation driver of a window destroyed while a page slide is settling is stopped once too often (Qt 6.11.2, offscreen): framework noise of the teardown, not of the shell
const QStringList kKnownNoise = {
    QStringLiteral("Created graphical object was not placed in the graphics scene"),
    QStringLiteral("templates/OverlaySheet.qml"),
    QStringLiteral("AboutItem.qml:377:9: QML OverlaySheet: Binding loop"),
    QStringLiteral("AboutItem.qml:380: TypeError: Cannot read property 'height' of null"),
    QStringLiteral("QUnifiedTimer::stopAnimationDriver: driver is not running"),
};

void collectWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        bool noise = false;
        for (const QString &known : kKnownNoise)
            noise = noise || message.contains(known);
        if (!noise) {
            QMutexLocker locker(&gWarningsMutex);
            gWarnings.append(message);
        }
    }
    if (gPreviousHandler)
        gPreviousHandler(type, context, message);
}

QList<QQuickItem *> allItems(QQuickItem *root)
{
    QList<QQuickItem *> items;
    if (!root)
        return items;
    items.append(root);
    for (QQuickItem *child : root->childItems())
        items.append(allItems(child));
    return items;
}

QString address(const FakeJellyfin &server)
{
    return server.url(QStringLiteral("/jellyfin")).toString();
}

const QString kMovies = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd01");
const QString kSciFi = QStringLiteral("aa00000000000000000000000000ff01");
const QString kMovieId = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f");
const QString kSeriesId = QStringLiteral("aa11bb22cc33dd44ee55ff6677889900");
const QString kSeasonId = QStringLiteral("11aa22bb33cc44dd55ee66ff778899aa");
const QString kEpisodeId = QStringLiteral("b1b2c3d4e5f64788990a1b2c3d4e5f60");
const QString kBoxSetId = QStringLiteral("bb11cc22dd33ee44ff5566778899aabb");

QJsonObject fixtureObject(const QString &name)
{
    return QJsonDocument::fromJson(fixture(name))
        .object()
        .value(QStringLiteral("Items"))
        .toArray()
        .first()
        .toObject();
}

QByteArray envelope(const QJsonArray &items)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Items"), items},
                                     {QStringLiteral("TotalRecordCount"), items.size()}})
        .toJson(QJsonDocument::Compact);
}

// Everything a signed-in window needs: the fake server, the controller with its token store, the settings, the image fetcher and the engine with the real Main.qml (declared last, so it is destroyed first)
struct Ui {
    explicit Ui(const QString &root)
        : settings(root + QStringLiteral("/kinefin.conf")),
          controller(QStringLiteral("default"), QStringLiteral("ui-test-device"),
                     QStringLiteral("0.0.1"), &store),
          origin(std::make_shared<Kinefin::ImageOrigin>()),
          imageFactory(origin, root + QStringLiteral("/images"))
    {
        controller.setImageOrigin(origin);
    }

    // A page pushed a moment ago may still be creating its items: let the engine finish before it is torn down, or Qt warns about items in creation
    ~Ui() { QTest::qWait(300); }

    // Loads Main.qml; true when a window came up
    bool load()
    {
        if (!server.listen())
            return false;
        engine.setNetworkAccessManagerFactory(&imageFactory);
        KLocalization::setupLocalizedContext(&engine);
        engine.setInitialProperties(
            {{QStringLiteral("connection"), QVariant::fromValue(&controller)},
             {QStringLiteral("settings"), QVariant::fromValue(&settings)},
             {QStringLiteral("startupWindowMode"), QStringLiteral("windowed")}});
        engine.loadFromModule(QStringLiteral("org.kde.kinefin"), QStringLiteral("Main"));
        if (engine.rootObjects().isEmpty())
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        return window && QTest::qWaitForWindowExposed(window);
    }

    // The fixture movie everywhere: in the library grid, in Continue Watching and behind the details route
    void serveMovie()
    {
        const QJsonObject movie = QJsonDocument::fromJson(fixture(QStringLiteral("movie_item")))
                                      .object()
                                      .value(QStringLiteral("Items"))
                                      .toArray()
                                      .first()
                                      .toObject();
        server.itemsById.insert(kMovieId, QJsonDocument(movie).toJson(QJsonDocument::Compact));
        server.listingRawBody.insert(QStringLiteral("Items"), envelope(QJsonArray{movie}));
        server.listingRawBody.insert(QStringLiteral("Resume"), envelope(QJsonArray{movie}));
    }

    // Search results by type, and the fixture movie behind the details route; the debounce is short
    void serveSearch()
    {
        serveMovie();
        server.itemsByType.insert(QStringLiteral("Movie"), QStringLiteral("items_page"));
        server.itemsByType.insert(QStringLiteral("Series"), QStringLiteral("search_series"));
        server.itemsByType.insert(QStringLiteral("Episode"), QStringLiteral("episodes_page"));
        server.itemsByType.insert(QStringLiteral("BoxSet"), QStringLiteral("boxset_children"));
        controller.search()->setDebounceInterval(30);
    }

    // Types ASCII text into whatever has the focus, one key at a time
    void type(const QString &text)
    {
        for (const QChar c : text)
            QTest::keyClick(window, c.toLatin1());
    }

    // Ctrl+F from the signed-in window, then the search page with the field of its mode in focus: the header box on a wide window, the page's own field in Compact mode
    QQuickItem *openSearchPage()
    {
        QTest::keyClick(window, Qt::Key_F, Qt::ControlModifier);
        if (!QTest::qWaitFor([this] { return item(QStringLiteral("searchPage")) != nullptr; },
                             5000))
            return nullptr;
        QQuickItem *field = nullptr;
        if (!QTest::qWaitFor(
                [&] {
                    field = item(searchFieldName());
                    return field && field->hasActiveFocus();
                },
                5000))
            return nullptr;
        return field;
    }

    // The field the search term is typed into at this width
    QString searchFieldName() const
    {
        return window->property("compactHeader").toBool() ? QStringLiteral("searchField")
                                                          : QStringLiteral("headerSearchField");
    }

    // A series with its season and episodes: the series, the season, the episodes of the fixture page and each episode behind the details route
    void serveShow()
    {
        for (const auto &[id, name] : {std::pair{kSeriesId, QStringLiteral("series_item")},
                                       std::pair{kSeasonId, QStringLiteral("season_item")},
                                       std::pair{kBoxSetId, QStringLiteral("boxset_item")}})
            server.itemsById.insert(
                id, QJsonDocument(fixtureObject(name)).toJson(QJsonDocument::Compact));
        QJsonObject episode = fixtureObject(QStringLiteral("episode_item"));
        server.itemsById.insert(episode.value(QStringLiteral("Id")).toString(),
                                QJsonDocument(episode).toJson(QJsonDocument::Compact));
        episode.insert(QStringLiteral("Id"), kEpisodeId);
        episode.insert(QStringLiteral("Name"), QStringLiteral("The Silent Sea"));
        server.itemsById.insert(kEpisodeId, QJsonDocument(episode).toJson(QJsonDocument::Compact));
        server.listingRawBody.insert(QStringLiteral("Items"),
                                     fixture(QStringLiteral("boxset_children")));
    }

    // Five played episodes of the fixture season, queued in order
    void fillQueue()
    {
        serveShow();
        server.generatedEpisodeTotal = 5;
        server.generatedEpisodesPlayed = 5;
        controller.queue()->add(kSeasonId, Kinefin::PlayQueue::Last);
    }

    // Scrolls the page so the item sits a little under the top of the view, then clicks it
    void revealAndClick(QQuickItem *page, QQuickItem *target)
    {
        auto *flickable = page->property("flickable").value<QQuickItem *>();
        const qreal itemTop = target->mapToScene(QPointF(0, 0)).y();
        const qreal viewTop = flickable->mapToScene(QPointF(0, 0)).y();
        flickable->setProperty("contentY",
                               flickable->property("contentY").toReal() + itemTop - viewTop - 100);
        QTest::qWait(100);
        // Under load the layout may still move after the reveal: a target outside the window gets no
        // click (Qt would log the dropped event), the caller tries again
        const QPointF center =
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
        if (center.x() < 0 || center.y() < 0 || center.x() >= window->width()
            || center.y() >= window->height())
            return;
        clickCenter(target);
    }

    // Reveals and clicks again until `done` holds: under load the layout may still move after the first reveal and the click lands beside its target
    bool revealAndClickUntil(QQuickItem *page, QQuickItem *target,
                             const std::function<bool()> &done, int timeoutMs = 8000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > timeoutMs)
                return false;
            revealAndClick(page, target);
            QTest::qWait(250);
        }
        return true;
    }

    // The details page in front: the last page of the stack
    QQuickItem *currentDetails() const
    {
        auto *stack = window->property("pageStack").value<QObject *>();
        return stack ? stack->property("currentItem").value<QQuickItem *>() : nullptr;
    }
    int stackDepth() const
    {
        auto *stack = window->property("pageStack").value<QObject *>();
        return stack ? stack->property("depth").toInt() : -1;
    }

    bool signIn()
    {
        controller.connectTo(address(server));
        if (!QTest::qWaitFor(
                [this] { return controller.state() == ConnectionController::NeedsLogin; }, 10000))
            return false;
        controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        return QTest::qWaitFor(
                   [this] { return controller.state() == ConnectionController::Authenticated; },
                   10000)
               && QTest::qWaitFor([this] { return item(QStringLiteral("homePage")) != nullptr; },
                                  10000);
    }

    QQuickItem *item(const QString &objectName) const
    {
        for (QQuickItem *candidate : allItems(window->contentItem())) {
            if (candidate->objectName() == objectName)
                return candidate;
        }
        return nullptr;
    }
    // A popup is no item: it is found among the QObject children of the window's root
    QObject *popup(const QString &objectName) const
    {
        return engine.rootObjects().constFirst()->findChild<QObject *>(objectName);
    }
    // The item below `root` with this object name: the Home page lists the same cards as a library page under it
    static QQuickItem *itemIn(QQuickItem *root, const QString &objectName)
    {
        for (QQuickItem *candidate : allItems(root)) {
            if (candidate->objectName() == objectName)
                return candidate;
        }
        return nullptr;
    }
    QList<QQuickItem *> itemsStartingWith(const QString &prefix) const
    {
        QList<QQuickItem *> found;
        for (QQuickItem *candidate : allItems(window->contentItem())) {
            if (candidate->objectName().startsWith(prefix))
                found.append(candidate);
        }
        return found;
    }
    // Every item that carries a `text` property equal to or containing `text`, visible or not
    QList<QQuickItem *> itemsWithText(const QString &text, bool exact = true) const
    {
        QList<QQuickItem *> found;
        for (QQuickItem *candidate : allItems(window->contentItem())) {
            const QVariant value = candidate->property("text");
            if (value.metaType().id() != QMetaType::QString)
                continue;
            if (exact ? value.toString() == text : value.toString().contains(text))
                found.append(candidate);
        }
        return found;
    }
    bool showsText(const QString &text, bool exact = true) const
    {
        for (QQuickItem *candidate : itemsWithText(text, exact)) {
            if (candidate->isVisible())
                return true;
        }
        return false;
    }
    void clickCenter(QQuickItem *target)
    {
        const QPointF center =
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    }
    bool openLibraryTile(const QString &id)
    {
        QQuickItem *tile = item(QStringLiteral("libraryCard_") + id);
        if (!tile)
            return false;
        clickCenter(tile);
        return QTest::qWaitFor([this] { return item(QStringLiteral("libraryPage")) != nullptr; },
                               5000);
    }

    // The pages of the stack, first to last
    QList<QQuickItem *> stackPages() const
    {
        QList<QQuickItem *> pages;
        auto *stack = window->property("pageStack").value<QObject *>();
        if (!stack)
            return pages;
        const QQmlListReference items(stack, "items");
        for (qsizetype i = 0; items.isValid() && i < items.count(); ++i) {
            if (auto *page = qobject_cast<QQuickItem *>(items.at(i)))
                pages.append(page);
        }
        return pages;
    }

    // True for an item inside a page of the stack that is not the one in front
    bool inAnotherPage(const QQuickItem *item) const
    {
        const QList<QQuickItem *> pages = stackPages();
        for (const QQuickItem *parent = item; parent; parent = parent->parentItem()) {
            if (pages.contains(parent))
                return parent != currentDetails();
        }
        return false;
    }

    // Where an item may lie: inside the window, except along an axis a Flickable above it scrolls
    bool insideWindow(const QQuickItem *item) const
    {
        const QRectF bounds(0, 0, window->width(), window->height());
        const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        bool freeX = false;
        bool freeY = false;
        for (const QQuickItem *parent = item->parentItem(); parent; parent = parent->parentItem()) {
            if (!parent->inherits("QQuickFlickable"))
                continue;
            freeX = freeX || parent->property("contentWidth").toReal() > parent->width() + 1;
            freeY = freeY || parent->property("contentHeight").toReal() > parent->height() + 1;
        }
        const bool xOk =
            freeX || (rect.left() >= bounds.left() - 1 && rect.right() <= bounds.right() + 1);
        const bool yOk =
            freeY || (rect.top() >= bounds.top() - 1 && rect.bottom() <= bounds.bottom() + 1);
        return xOk && yOk;
    }

    // True when a clipping ancestor leaves nothing of the item to see: the toolbars of the pages below sit there, outside the page row
    static bool clippedAway(const QQuickItem *item)
    {
        QRectF visible = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        for (const QQuickItem *parent = item->parentItem(); parent; parent = parent->parentItem()) {
            if (!parent->clip())
                continue;
            visible = visible.intersected(
                parent->mapRectToScene(QRectF(0, 0, parent->width(), parent->height())));
            if (visible.width() <= 0 || visible.height() <= 0)
                return true;
        }
        return false;
    }

    static QString describe(const QQuickItem *item)
    {
        return QStringLiteral("%1 '%2' text='%3'")
            .arg(QString::fromLatin1(item->metaObject()->className()), item->objectName(),
                 item->property("text").toString());
    }

    // The window header the shell puts its menus and tab in
    QQuickItem *shellHeader() const { return window->property("header").value<QQuickItem *>(); }

    // The Kirigami.Action of the header with this object name: the menus trigger through it like a click does
    QObject *shellAction(const QString &objectName) const
    {
        return engine.rootObjects().constFirst()->findChild<QObject *>(objectName);
    }

    // The single tab button of the bar: a Kirigami private class, matched by its class name
    QQuickItem *tabButton() const
    {
        for (QQuickItem *candidate : allItems(shellHeader())) {
            if (QString::fromLatin1(candidate->metaObject()->className())
                    .contains(QLatin1String("NavigationTabButton")))
                return candidate;
        }
        return nullptr;
    }

    // The items of a menu, in order: QQuickMenu::itemAt is invokable
    QList<QQuickItem *> menuItems(const QString &menuName) const
    {
        QList<QQuickItem *> found;
        QObject *menu = popup(menuName);
        if (!menu)
            return found;
        const int count = menu->property("count").toInt();
        for (int i = 0; i < count; ++i) {
            QQuickItem *entry = nullptr;
            if (QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, entry),
                                          Q_ARG(int, i))
                && entry)
                found.append(entry);
        }
        return found;
    }

    // The header widgets against the mode this width should give: the box, favorites and the queue
    // fold into the main menu in Compact mode, and only a Wide tab carries its label. Empty when all is well
    QStringList modeProblems(bool wide, bool compact) const
    {
        QStringList problems;
        if (!shellHeader())
            return {QStringLiteral("the header is not there")};
        if (window->property("compactHeader").toBool() != compact)
            problems << QStringLiteral("compactHeader is %1, the mode wants %2")
                            .arg(window->property("compactHeader").toBool())
                            .arg(compact);
        const struct {
            const char *name;
            bool visible;
        } cluster[] = {
            {"headerSearchField", !compact},
            {"favoritesButton", !compact},
            {"queueButton", !compact},
            {"accountButton", true},
        };
        for (const auto &entry : cluster) {
            QQuickItem *control = item(QLatin1String(entry.name));
            if (!control) {
                problems << QStringLiteral("%1 is not there").arg(QLatin1String(entry.name));
                continue;
            }
            if (control->isVisible() != entry.visible)
                problems << QStringLiteral("%1 should be %2 but is %3")
                                .arg(QLatin1String(entry.name),
                                     entry.visible ? QStringLiteral("visible")
                                                   : QStringLiteral("hidden"),
                                     control->isVisible() ? QStringLiteral("visible")
                                                          : QStringLiteral("hidden"));
        }
        QQuickItem *tab = tabButton();
        const int wantedDisplay = wide ? 2 /*TextBesideIcon*/ : 0 /*IconOnly*/;
        if (!tab)
            problems << QStringLiteral("the tab is not there");
        else if (tab->property("display").toInt() != wantedDisplay)
            problems << QStringLiteral("the tab display is %1, the mode wants %2")
                            .arg(tab->property("display").toInt())
                            .arg(wantedDisplay);
        return problems;
    }

    // What is wrong with the layout now: more than one page showing, the page in front not filling the window under the header, a control outside the window. Empty when all is well
    QStringList layoutProblems() const
    {
        QStringList problems;
        const QRectF bounds(0, 0, window->width(), window->height());
        const QQuickItem *current = currentDetails();
        if (!current)
            return {QStringLiteral("no page in front")};
        if (stackPages().size() != stackDepth())
            problems << QStringLiteral("the stack lists %1 pages for a depth of %2")
                            .arg(stackPages().size())
                            .arg(stackDepth());
        QQuickItem *header = shellHeader();
        const qreal headerHeight = header ? header->height() : -1;
        if (headerHeight <= 0)
            problems << QStringLiteral("the header is not there");
        const QRectF front =
            current->mapRectToScene(QRectF(0, 0, current->width(), current->height()));
        if (front.top() < headerHeight - 1 || front.left() < -1
            || front.right() > bounds.right() + 1 || front.bottom() > bounds.bottom() + 1
            || front.width() < bounds.width() - 1)
            problems << QStringLiteral("front page %1 does not fill the window under the header "
                                       "(%2 px): x=%3 y=%4 width=%5 window=%6")
                            .arg(describe(current))
                            .arg(headerHeight)
                            .arg(front.left())
                            .arg(front.top())
                            .arg(front.width())
                            .arg(bounds.width());
        for (const QQuickItem *page : stackPages()) {
            if (page == current || !page->isVisible() || clippedAway(page))
                continue;
            const QRectF rect = page->mapRectToScene(QRectF(0, 0, page->width(), page->height()));
            if (rect.intersected(bounds).width() > 1)
                problems << QStringLiteral("another page shows: %1").arg(describe(page));
        }
        for (QQuickItem *candidate : allItems(window->contentItem())) {
            const bool control = candidate->inherits("QQuickAbstractButton")
                                 || candidate->inherits("QQuickTextInput")
                                 || candidate->inherits("QQuickFlickable");
            if (!control || !candidate->isVisible() || candidate->width() <= 0
                || candidate->height() <= 0 || inAnotherPage(candidate) || clippedAway(candidate))
                continue;
            if (!insideWindow(candidate))
                problems << QStringLiteral("outside the window: %1").arg(describe(candidate));
        }
        return problems;
    }

    // Tab presses until the focus comes back to an item it already visited: the items that took it, in order
    QList<QQuickItem *> tabCycle(int limit = 120)
    {
        QList<QQuickItem *> visited;
        for (int i = 0; i < limit; ++i) {
            QTest::keyClick(window, Qt::Key_Tab);
            QQuickItem *focused = window->activeFocusItem();
            if (!focused || visited.contains(focused))
                break;
            visited.append(focused);
        }
        return visited;
    }

    bool opensWindowOfWidth(int width)
    {
        window->resize(width, 640);
        return QTest::qWaitFor([this, width] { return window->width() == width; }, 3000);
    }

    FakeJellyfin server;
    MemoryTokenStore store;
    Kinefin::ClientSettings settings;
    ConnectionController controller;
    std::shared_ptr<Kinefin::ImageOrigin> origin;
    Kinefin::ImageNetworkFactory imageFactory;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

// Brings the window to one of the screens the layout checks walk through; false when it could not
bool buildState(Ui &ui, const QString &state)
{
    auto *root = ui.engine.rootObjects().constFirst();
    const auto wait = [&](const QString &objectName) {
        return QTest::qWaitFor([&] { return ui.item(objectName) != nullptr; }, 10000);
    };
    if (state == QLatin1String("servers"))
        return wait(QStringLiteral("serversPage"));
    if (state == QLatin1String("login")) {
        ui.controller.connectTo(address(ui.server));
        return wait(QStringLiteral("loginPage"));
    }
    if (state == QLatin1String("settings") || state == QLatin1String("settingsSection")
        || state == QLatin1String("about")) {
        // The shell clears the stack once when it starts: the page goes in after that
        if (!wait(QStringLiteral("serversPage")))
            return false;
        const bool about = state == QLatin1String("about");
        const QString name = about ? QStringLiteral("aboutPage") : QStringLiteral("settingsPage");
        const QString file =
            about ? QStringLiteral("AboutPage.qml") : QStringLiteral("SettingsPage.qml");
        QVariantMap properties;
        if (!about)
            properties.insert(QStringLiteral("settings"), QVariant::fromValue(&ui.settings));
        QMetaObject::invokeMethod(root, "openSection", Q_ARG(QVariant, name), Q_ARG(QVariant, file),
                                  Q_ARG(QVariant, properties));
        if (!wait(name))
            return false;
        if (state != QLatin1String("settingsSection"))
            return true;
        const QVariantMap first = ui.item(QStringLiteral("settingsPage"))
                                      ->property("sections")
                                      .toList()
                                      .constFirst()
                                      .toMap();
        QVariantMap sectionProperties = properties;
        sectionProperties.insert(QStringLiteral("section"), first.value(QStringLiteral("section")));
        sectionProperties.insert(QStringLiteral("title"), first.value(QStringLiteral("title")));
        const QUrl sectionUrl(
            QStringLiteral("qrc:/qt/qml/org/kde/kinefin/SettingsSectionPage.qml"));
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QMetaObject::invokeMethod(stack, "push", Q_ARG(QVariant, sectionUrl),
                                  Q_ARG(QVariant, sectionProperties));
        return QTest::qWaitFor([&] { return ui.stackDepth() == 3; }, 5000);
    }
    if (state == QLatin1String("homeFailedRow"))
        ui.server.listingStatus.insert(QStringLiteral("Resume"), 500);
    if (state == QLatin1String("libraryFailed"))
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
    if (state == QLatin1String("details") || state == QLatin1String("search"))
        state == QLatin1String("search") ? ui.serveSearch() : ui.serveMovie();
    if (state == QLatin1String("detailsMissing")) {
        ui.serveMovie();
        const QJsonObject episode = fixtureObject(QStringLiteral("episode_item"));
        ui.server.listingRawBody.insert(QStringLiteral("Items"), envelope(QJsonArray{episode}));
    }
    if (state == QLatin1String("rails")) {
        ui.server.generatedGenreTotal = 2;
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
    }
    if (!ui.signIn())
        return false;
    if (state == QLatin1String("home") || state == QLatin1String("homeFailedRow"))
        return QTest::qWaitFor([&] { return ui.controller.homeRows()->rowCount() > 0; }, 10000);
    if (state == QLatin1String("search")) {
        if (!ui.openSearchPage())
            return false;
        ui.type(QStringLiteral("abyss"));
        return QTest::qWaitFor(
            [&] { return ui.controller.search()->state() == Kinefin::SearchModel::Results; },
            10000);
    }
    if (state == QLatin1String("queue") || state == QLatin1String("queueEmpty")) {
        if (state == QLatin1String("queue")) {
            ui.fillQueue();
            if (!QTest::qWaitFor([&] { return ui.controller.queue()->count() == 5; }, 10000))
                return false;
        }
        const QVariantMap properties{
            {QStringLiteral("connection"), QVariant::fromValue(&ui.controller)}};
        QMetaObject::invokeMethod(root, "openSection", Q_ARG(QVariant, QStringLiteral("queuePage")),
                                  Q_ARG(QVariant, QStringLiteral("QueuePage.qml")),
                                  Q_ARG(QVariant, properties));
        return QTest::qWaitFor([&] { return ui.item(QStringLiteral("queuePage")) != nullptr; },
                               10000);
    }
    if (!QTest::qWaitFor(
            [&] { return ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr; }, 10000)
        || !ui.openLibraryTile(kMovies))
        return false;
    if (state == QLatin1String("library") || state == QLatin1String("libraryFailed"))
        return QTest::qWaitFor(
            [&] {
                return ui.controller.library()->status() != Kinefin::LibraryItemsModel::Loading;
            },
            10000);
    if (state == QLatin1String("rails")) {
        if (!QTest::qWaitFor([&] { return ui.controller.library()->genreOptions()->available(); },
                             10000))
            return false;
        QMetaObject::invokeMethod(root, "openGenres", Q_ARG(QVariant, QVariant()));
        return QTest::qWaitFor(
            [&] { return ui.showsText(QStringLiteral("Could not load this genre"), false); },
            10000);
    }
    // details and detailsMissing: the first card of the grid
    QQuickItem *card = nullptr;
    const QString cardId = state == QLatin1String("details")
                               ? kMovieId
                               : QStringLiteral("c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7");
    if (!QTest::qWaitFor(
            [&] {
                return (card = Ui::itemIn(ui.item(QStringLiteral("libraryPage")),
                                          QStringLiteral("mediaCard_") + cardId))
                       != nullptr;
            },
            10000))
        return false;
    ui.clickCenter(card);
    return wait(QStringLiteral("itemDetailsPage"))
           && QTest::qWaitFor(
               [&] {
                   return ui.showsText(state == QLatin1String("details")
                                           ? QStringLiteral("Cast and Crew")
                                           : QStringLiteral("This item is not available"));
               },
               10000);
}
} // namespace

class TestUiPages : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void initTestCase() { gPreviousHandler = qInstallMessageHandler(collectWarnings); }
    void cleanupTestCase() { qInstallMessageHandler(gPreviousHandler); }

    void init()
    {
        {
            QMutexLocker locker(&gWarningsMutex);
            gWarnings.clear();
        }
        m_home = std::make_unique<QTemporaryDir>();
        QVERIFY(m_home->isValid());
        qputenv("XDG_DATA_HOME", m_home->filePath(QStringLiteral("data")).toUtf8());
        qputenv("XDG_CACHE_HOME", m_home->filePath(QStringLiteral("cache")).toUtf8());
        qputenv("XDG_CONFIG_HOME", m_home->filePath(QStringLiteral("config")).toUtf8());
    }

    void cleanup()
    {
        // Every test ends with a clean log: a QML warning is a bug
        QMutexLocker locker(&gWarningsMutex);
        QVERIFY2(gWarnings.isEmpty(), qPrintable(gWarnings.join(QLatin1Char('\n'))));
        m_home.reset();
    }

    void homeShowsLibraryTilesAndRows()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // Three libraries: the music one is shown but cannot be opened
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("libraryCard_")).size(), 3);
        int enabled = 0;
        for (QQuickItem *tile : ui.itemsStartingWith(QStringLiteral("libraryCard_")))
            enabled += tile->isEnabled() ? 1 : 0;
        QCOMPARE(enabled, 2);
        // The rows: Continue Watching, Next Up and Recently Added for the two video libraries
        QTRY_VERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QVERIFY(ui.showsText(QStringLiteral("Next Up")));
        QVERIFY(ui.showsText(QStringLiteral("Recently Added in Movies")));
        QVERIFY(ui.showsText(QStringLiteral("Recently Added in Series")));
        // The wide row of the fixture shows its episode under the series with its numbers
        QTRY_VERIFY(ui.showsText(QStringLiteral("S2:E6"), false));
        QQuickItem *list = ui.item(QStringLiteral("homeRowList_0"));
        QVERIFY(list);
        QTRY_COMPARE(list->property("count").toInt(), 2);
    }

    void libraryTilesShowTheirImages()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // The fake serves a PNG for every image route: the tile with an image tag ends up with a ready image and no credentials went out with it
        QTRY_VERIFY(ui.server.imageRequests > 0);
        QVERIFY(ui.server.lastImageAuth.isEmpty());
        QVERIFY(ui.server.lastImageCookie.isEmpty());
        bool ready = false;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (QQuickItem *candidate : allItems(ui.window->contentItem())) {
                    if (candidate->inherits("QQuickImage")
                        && candidate->property("status").toInt() == 1) // Image.Ready
                        ready = true;
                }
                return ready;
            }(),
            10000);
    }

    void libraryGridOpensAndPagesOnDemand()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 450;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_COMPARE(ui.controller.library()->status(), Kinefin::LibraryItemsModel::Ready);
        QCOMPARE(ui.controller.library()->rowCount(), 450);
        QTRY_COMPARE(grid->property("count").toInt(), 450);
        // Scrolling to the end asks for the pages there and nothing else
        QVERIFY(QMetaObject::invokeMethod(grid, "positionViewAtEnd"));
        QTRY_VERIFY(
            ui.controller.library()
                ->data(ui.controller.library()->index(449), Kinefin::LibraryItemsModel::LoadedRole)
                .toBool());
        QVERIFY(ui.controller.library()->residentPages().size()
                <= Kinefin::LibraryItemsModel::MaxResidentPages);
        // A sort change from the page's own action list restarts the model
        ui.controller.library()->setSortKey(Kinefin::LibraryItemsModel::Runtime);
        QTRY_COMPARE(ui.controller.library()->status(), Kinefin::LibraryItemsModel::Ready);
    }

    void emptyAndFailedStatesAreShown()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 0;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_VERIFY(ui.showsText(QStringLiteral("No items")));
        QVERIFY(!ui.showsText(QStringLiteral("Could not load this library"), false));

        // A failing server turns the page into an error with a retry that works
        ui.server.generatedItemTotal = 30;
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
        ui.controller.library()->setFilter(Kinefin::LibraryItemsModel::Unplayed);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not load this library"), false));
        QVERIFY(!ui.showsText(QStringLiteral("No items")));
        ui.server.listingStatus.clear();
        ui.controller.library()->retry();
        QTRY_VERIFY(!ui.showsText(QStringLiteral("Could not load this library"), false));
        QTRY_COMPARE(ui.controller.library()->rowCount(), 30);
    }

    void theWheelOverAHomeRowStillScrollsThePage()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Recently Added in Series")));
        // Measured on a settled page: while the rows still load the content height keeps changing
        QTRY_VERIFY([&ui] {
            auto *rows = ui.controller.homeRows();
            for (int row = 0; row < rows->rowCount(); ++row) {
                if (rows->data(rows->index(row), Kinefin::HomeRowsModel::StatusRole).toInt()
                    == int(Kinefin::HomeRowsModel::Loading))
                    return false;
            }
            return rows->rowCount() > 0;
        }());
        QQuickItem *page = ui.item(QStringLiteral("homePage"));
        QQuickItem *list = ui.item(QStringLiteral("homeRowList_0"));
        QVERIFY(page && list);
        QTRY_VERIFY(list->isVisible());
        QTest::qWait(200);
        auto *flickable = page->property("flickable").value<QQuickItem *>();
        QVERIFY(flickable);
        // The window is 640 px high: the libraries, four rows and their headings do not fit
        const qreal contentY = [&] { return flickable->property("contentY").toReal(); }();
        QVERIFY(flickable->property("contentHeight").toReal() > flickable->height() * 1.5);
        QCOMPARE(contentY, 0.0);
        const auto wheelOver = [&ui](QQuickItem *target) {
            const QPoint over =
                target->mapToScene(QPointF(target->width() / 2, target->height() / 2)).toPoint();
            QWheelEvent wheel(over, ui.window->mapToGlobal(over), QPoint(), QPoint(0, -240),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(ui.window, &wheel);
        };
        // Control: the same wheel over a library tile moves the page, so the event itself is sound
        QQuickItem *tile = ui.item(QStringLiteral("libraryCard_") + kMovies);
        QVERIFY(tile);
        // Sent again until it takes: under load the first one can arrive before the page is ready for it
        QTRY_VERIFY((wheelOver(tile), flickable->property("contentY").toReal() > 0.0));
        // Put the first row 100 px under the top of the viewport, with room left to scroll
        const qreal rowTop = list->mapToScene(QPointF(0, 0)).y();
        const qreal viewTop = flickable->mapToScene(QPointF(0, 0)).y();
        const qreal baseline = flickable->property("contentY").toReal() + rowTop - viewTop - 100;
        flickable->setProperty("contentY", baseline);
        QTRY_COMPARE(flickable->property("contentY").toReal(), baseline);
        QVERIFY(list->mapToScene(QPointF(0, list->height() / 2)).y() < ui.window->height());
        QTRY_VERIFY((wheelOver(list), flickable->property("contentY").toReal() > baseline));
    }

    void aLibraryTileOpensItsDetails()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = Ui::itemIn(ui.item(QStringLiteral("libraryPage")),
                                       QStringLiteral("mediaCard_") + kMovieId))
                    != nullptr);
        QTRY_VERIFY(card->isVisible() && card->isEnabled());
        ui.clickCenter(card);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        // The item and every section its fixture has
        QTRY_VERIFY(ui.showsText(QStringLiteral("Arcturus (2012)")));
        for (const QString &heading :
             {QStringLiteral("Cast and Crew"), QStringLiteral("Chapters"),
              QStringLiteral("Media Info"), QStringLiteral("More Like This")})
            QTRY_VERIFY2(ui.showsText(heading), qPrintable(heading));
        QVERIFY(ui.showsText(QStringLiteral("Mira Vale"), false));
        QVERIFY(ui.showsText(QStringLiteral("The Ship"), false));
        // Playback is not wired yet: the Resume action is there and disabled
        QQmlListReference actions(ui.item(QStringLiteral("itemDetailsPage")), "actions");
        bool foundPlay = false;
        for (qsizetype i = 0; i < actions.count(); ++i) {
            // The fixture movie has a resume point: the action reads Resume
            if (actions.at(i)->property("text").toString() == QStringLiteral("Resume")) {
                foundPlay = true;
                QVERIFY(!actions.at(i)->property("enabled").toBool());
            }
        }
        QVERIFY(foundPlay);
    }

    void returnOnAHomeRowOpensDetailsAndBackReturns()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), 1);
        list->forceActiveFocus();
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Arcturus (2012)")));
        // Back returns to Home
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QVERIFY(stack);
        QVERIFY(QMetaObject::invokeMethod(stack, "pop", Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) == nullptr);
        QVERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
    }

    void detailsRenderServerTextAsPlainText()
    {
        Ui ui(m_home->path());
        const QString hostile =
            QStringLiteral("<b>bold</b><img src=\"http://evil.invalid/x.png\">");
        const auto with = [&hostile](const QString &suffix) { return QString(hostile + suffix); };
        const QJsonObject item{
            {QStringLiteral("Id"), kMovieId},
            {QStringLiteral("Type"), QStringLiteral("Movie")},
            {QStringLiteral("Name"), with(QStringLiteral(" name"))},
            {QStringLiteral("SeriesName"), with(QStringLiteral(" series"))},
            {QStringLiteral("SeriesId"), QStringLiteral("aa11bb22cc33dd44ee55ff6677889900")},
            {QStringLiteral("Overview"), with(QStringLiteral(" overview"))},
            {QStringLiteral("Taglines"), QJsonArray{with(QStringLiteral(" tagline"))}},
            {QStringLiteral("Genres"), QJsonArray{with(QStringLiteral(" genre"))}},
            {QStringLiteral("People"),
             QJsonArray{QJsonObject{{QStringLiteral("Name"), with(QStringLiteral(" person"))},
                                    {QStringLiteral("Role"), with(QStringLiteral(" role"))},
                                    {QStringLiteral("Type"), QStringLiteral("Actor")}}}},
            {QStringLiteral("Chapters"),
             QJsonArray{QJsonObject{{QStringLiteral("Name"), with(QStringLiteral(" chapter"))},
                                    {QStringLiteral("StartPositionTicks"), 0}}}},
            {QStringLiteral("MediaSources"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("Id"), QStringLiteral("ms1")},
                 {QStringLiteral("Container"), with(QStringLiteral(" container"))},
                 {QStringLiteral("MediaStreams"),
                  QJsonArray{QJsonObject{
                      {QStringLiteral("Type"), QStringLiteral("Audio")},
                      {QStringLiteral("DisplayTitle"), with(QStringLiteral(" stream"))}}}}}}}};
        ui.server.itemsById.insert(kMovieId, QJsonDocument(item).toJson(QJsonDocument::Compact));
        ui.server.listingRawBody.insert(QStringLiteral("Items"), envelope(QJsonArray{item}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = Ui::itemIn(ui.item(QStringLiteral("libraryPage")),
                                       QStringLiteral("mediaCard_") + kMovieId))
                    != nullptr);
        ui.clickCenter(card);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        // Every label that carries the markup renders it as text, never as rich text: the title and heading, the series link, and the lines the markup shares with other words
        for (const QString &part :
             {QStringLiteral(" name"), QStringLiteral(" series"), QStringLiteral(" overview"),
              QStringLiteral(" tagline"), QStringLiteral(" genre"), QStringLiteral(" person"),
              QStringLiteral(" role"), QStringLiteral(" chapter"), QStringLiteral(" container"),
              QStringLiteral(" stream")}) {
            QTRY_VERIFY2(!ui.itemsWithText(with(part), false).isEmpty(), qPrintable(part));
            for (QQuickItem *label : ui.itemsWithText(with(part), false)) {
                QVERIFY(label->property("textFormat").isValid());
                QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
            }
        }
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void aMissingItemShowsNotFound()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        // The grid lists an episode the details route does not know
        const QJsonObject episode = QJsonDocument::fromJson(fixture(QStringLiteral("episode_item")))
                                        .object()
                                        .value(QStringLiteral("Items"))
                                        .toArray()
                                        .first()
                                        .toObject();
        ui.server.listingRawBody.insert(QStringLiteral("Items"), envelope(QJsonArray{episode}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QQuickItem *card = nullptr;
        QTRY_VERIFY(
            (card = Ui::itemIn(ui.item(QStringLiteral("libraryPage")),
                               QStringLiteral("mediaCard_c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7")))
            != nullptr);
        ui.clickCenter(card);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_VERIFY(ui.showsText(QStringLiteral("This item is not available")));
    }

    void aLinkOpensItsItemAfterSignIn()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        // The link arrives before the user is signed in and waits
        ui.window->setProperty("requestedItemId", kMovieId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Arcturus (2012)")));
        // It is used once
        QTRY_COMPARE(ui.window->property("requestedItemId").toString(), QString());
    }

    void lossOfTheSessionClosesTheDetails()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kMovieId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        ui.server.denyViews = true;
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) == nullptr);
    }

    void aSeriesPageListsItsSeasonsAndOpensOne()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeriesId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Seasons")));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("seasonsRow"))) != nullptr);
        QTRY_COMPARE(row->property("count").toInt(), 3);
        // The delegates follow the count by a moment
        QTRY_VERIFY(Ui::itemIn(ui.currentDetails(), QStringLiteral("mediaCard_") + kSeasonId)
                    != nullptr);
        // Return on the second season opens it
        row->forceActiveFocus();
        row->setProperty("currentIndex", 1);
        const int depth = ui.stackDepth();
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.stackDepth(), depth + 1);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Episodes")));
        QTRY_VERIFY(Ui::itemIn(ui.currentDetails(), QStringLiteral("episodeRow_") + kEpisodeId)
                    != nullptr);
        // Back to the series
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(stack, "pop", Q_ARG(QVariant, QVariant())));
        QTRY_COMPARE(ui.stackDepth(), depth);
    }

    void nextUpOpensItsEpisode()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeriesId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Next Up")));
        QQuickItem *page = nullptr;
        QTRY_VERIFY((page = ui.item(QStringLiteral("itemDetailsPage"))) != nullptr);
        QQuickItem *entry = nullptr;
        QTRY_VERIFY((entry = Ui::itemIn(
                         page, QStringLiteral("episodeRow_c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7")))
                    != nullptr);
        const int depth = ui.stackDepth();
        QVERIFY(ui.revealAndClickUntil(page, entry, [&] { return ui.stackDepth() == depth + 1; }));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Weightless")));
    }

    void aLongSeasonPagesOnlyWhileScrolling()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        ui.server.generatedEpisodeTotal = 120;
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeasonId);
        QVERIFY(ui.signIn());
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("episodesList"))) != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), 120);
        QTest::qWait(300);
        // Only the first page until the user scrolls
        QCOMPARE(ui.server.listingRoutes.value(QStringLiteral("Episodes")).size(), 1);
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(ui.server.listingRoutes.value(QStringLiteral("Episodes"))
                        .constLast()
                        .contains("startIndex=100"));
    }

    void aCollectionPageListsItsItems()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kBoxSetId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("In This Collection")));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("collectionRow"))) != nullptr);
        QTRY_COMPARE(row->property("count").toInt(), 3);
        // The delegates follow the count by a moment
        QTRY_VERIFY(Ui::itemIn(ui.currentDetails(), QStringLiteral("mediaCard_") + kMovieId)
                    != nullptr);
    }

    void episodeTextIsPlainText()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        const QString hostile =
            QStringLiteral("<b>bold</b><img src=\"http://evil.invalid/x.png\">");
        const QJsonObject episode{
            {QStringLiteral("Id"), kEpisodeId},
            {QStringLiteral("Type"), QStringLiteral("Episode")},
            {QStringLiteral("Name"), QString(hostile + QStringLiteral(" title"))},
            {QStringLiteral("Overview"), QString(hostile + QStringLiteral(" synopsis"))},
            {QStringLiteral("SeriesId"), kSeriesId},
            {QStringLiteral("ParentIndexNumber"), 2},
            {QStringLiteral("IndexNumber"), 5}};
        ui.server.listingRawBody.insert(QStringLiteral("Episodes"), envelope(QJsonArray{episode}));
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeasonId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("episodesList")) != nullptr);
        for (const QString &part : {QStringLiteral(" title"), QStringLiteral(" synopsis")}) {
            QTRY_VERIFY2(!ui.itemsWithText(QString(hostile + part), false).isEmpty(),
                         qPrintable(part));
            for (QQuickItem *label : ui.itemsWithText(QString(hostile + part), false)) {
                QVERIFY(label->property("textFormat").isValid());
                QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
            }
        }
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void aFailedEpisodesListOffersRetry()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        ui.server.listingStatus.insert(QStringLiteral("Episodes"), 500);
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeasonId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not load the episodes"), false));
        ui.server.listingStatus.clear();
        auto *children = ui.currentDetails()->property("childModel").value<QObject *>();
        QVERIFY(children);
        auto *episodes = children->property("episodes").value<QObject *>();
        QVERIFY(episodes);
        QVERIFY(QMetaObject::invokeMethod(episodes, "retry"));
        QTRY_VERIFY(!ui.showsText(QStringLiteral("Could not load the episodes"), false));
        QTRY_VERIFY(Ui::itemIn(ui.currentDetails(), QStringLiteral("episodeRow_") + kEpisodeId)
                    != nullptr);
    }

    void theMetaLineIsVisibleOnAnEpisodePage()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kEpisodeId);
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        // The series link and the metadata are shown, not only present
        QTRY_VERIFY(ui.showsText(QStringLiteral("Abyss Station")));
        QTRY_VERIFY(ui.showsText(QStringLiteral("28 min"), false));
        QTRY_VERIFY(ui.showsText(QStringLiteral("S2:E6"), false));
    }

    void aSeriesLinkOpensTheSeriesWhenItIsNotBelow()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        // A deep link straight to an episode: the series is not in the stack
        ui.window->setProperty("requestedItemId", kEpisodeId);
        QVERIFY(ui.signIn());
        QQuickItem *link = nullptr;
        QTRY_VERIFY((link = Ui::itemIn(ui.currentDetails(), QStringLiteral("seriesLink")))
                    != nullptr);
        QTRY_VERIFY(link->isVisible());
        const int depth = ui.stackDepth();
        QVERIFY(ui.revealAndClickUntil(ui.currentDetails(), link,
                                       [&] { return ui.stackDepth() == depth + 1; }));
        QTRY_COMPARE(ui.currentDetails()->property("itemId").toString(), kSeriesId);
    }

    void theSeriesLinkGoesBackUpTheStack()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeriesId);
        QVERIFY(ui.signIn());
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("seasonsRow"))) != nullptr);
        QTRY_COMPARE(row->property("count").toInt(), 3);
        const int seriesDepth = ui.stackDepth();
        row->forceActiveFocus();
        row->setProperty("currentIndex", 1);
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.stackDepth(), seriesDepth + 1);
        QQuickItem *episodeRow = nullptr;
        QTRY_VERIFY((episodeRow = Ui::itemIn(ui.currentDetails(),
                                             QStringLiteral("episodeRow_") + kEpisodeId))
                    != nullptr);
        QQuickItem *seasonList = ui.currentDetails();
        QVERIFY(ui.revealAndClickUntil(seasonList, episodeRow,
                                       [&] { return ui.stackDepth() == seriesDepth + 2; }));
        QQuickItem *link = nullptr;
        QTRY_VERIFY((link = Ui::itemIn(ui.currentDetails(), QStringLiteral("seriesLink")))
                    != nullptr);
        QTRY_VERIFY(link->isVisible());
        // The series page is already in the stack: the click goes back to it instead of pushing another
        QVERIFY(ui.revealAndClickUntil(ui.currentDetails(), link,
                                       [&] { return ui.stackDepth() == seriesDepth; }));
        QCOMPARE(ui.currentDetails()->property("itemId").toString(), kSeriesId);
    }

    void ctrlFOpensTheSearchPageWithTheFieldInFocusAndOnlyOnce()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.openSearchPage() != nullptr);
        QCOMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Empty);
        QVERIFY(ui.showsText(QStringLiteral("Search your libraries")));
        // A second Ctrl+F does not stack another page and keeps the field in focus (an ambiguous shortcut would be a warning, and a failure)
        const int depth = ui.stackDepth();
        QTest::keyClick(ui.window, Qt::Key_F, Qt::ControlModifier);
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), depth);
        QVERIFY(ui.item(ui.searchFieldName())->hasActiveFocus());
    }

    void typingShowsTheGroupedResults()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *field = ui.openSearchPage();
        QVERIFY(field);
        ui.type(QStringLiteral("a"));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Keep typing")));
        ui.type(QStringLiteral("byss"));
        QTRY_COMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
        for (const QString &heading : {QStringLiteral("Movies"), QStringLiteral("Series"),
                                       QStringLiteral("Episodes"), QStringLiteral("Collections")})
            QTRY_VERIFY2(ui.showsText(heading), qPrintable(heading));
        QTRY_VERIFY(Ui::itemIn(ui.item(QStringLiteral("searchPage")),
                               QStringLiteral("mediaCard_") + kMovieId)
                    != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("abyss"));
    }

    void downThenReturnOpensTheResultAndBackKeepsTheResults()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.openSearchPage() != nullptr);
        ui.type(QStringLiteral("arc"));
        QTRY_COMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("searchRow_0"))) != nullptr);
        QTRY_VERIFY(row->property("count").toInt() > 0);
        const int depth = ui.stackDepth();
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(row->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.stackDepth(), depth + 1);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Arcturus (2012)")));
        // Back to the same results: the term and the groups belong to the controller
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(stack, "pop", Q_ARG(QVariant, QVariant())));
        QTRY_COMPARE(ui.stackDepth(), depth);
        QVERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("arc"));
        QCOMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
    }

    void markupInResultsAndInTheTermIsPlainText()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        const QString hostile =
            QStringLiteral("<b>bold</b><img src=\"http://evil.invalid/x.png\">");
        const QJsonObject movie{
            {QStringLiteral("Id"), kMovieId},
            {QStringLiteral("Type"), QStringLiteral("Movie")},
            {QStringLiteral("Name"), QString(hostile + QStringLiteral(" result"))}};
        ui.server.itemsByType.clear();
        ui.server.listingRawBody.insert(QStringLiteral("Items"), envelope(QJsonArray{movie}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.openSearchPage() != nullptr);
        // The term itself carries the markup
        ui.type(hostile);
        QTRY_COMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
        QTRY_VERIFY(
            !ui.itemsWithText(QString(hostile + QStringLiteral(" result")), false).isEmpty());
        for (QQuickItem *label :
             ui.itemsWithText(QString(hostile + QStringLiteral(" result")), false))
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        // The typed term is never echoed into a label: only the field itself holds it
        for (QQuickItem *holder : ui.itemsWithText(QStringLiteral("evil.invalid"), false)) {
            const bool isTitleOfResult =
                holder->property("text").toString().contains(QStringLiteral(" result"));
            QVERIFY2(isTitleOfResult || holder->inherits("QQuickTextField")
                         || holder->inherits("QQuickTextInput"),
                     holder->metaObject()->className());
        }
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void anEmptyResultShowsNoResults()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        ui.server.itemsByType.clear();
        ui.server.listingRawBody.insert(QStringLiteral("Items"), envelope({}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.openSearchPage() != nullptr);
        ui.type(QStringLiteral("zzzz"));
        QTRY_VERIFY(ui.showsText(QStringLiteral("No results")));
    }

    void lossOfTheSessionDuringASearchReturnsToSignIn()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.openSearchPage() != nullptr);
        ui.type(QStringLiteral("abyss"));
        QTRY_COMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
        ui.server.denyViews = true;
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) == nullptr);
        QCOMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Empty);
        QCOMPARE(ui.controller.search()->term(), QString());
    }

    void theGenreDialogFiltersTheGrid()
    {
        Ui ui(m_home->path());
        ui.server.itemsByGenre.insert(kSciFi, QStringLiteral("items_genre_scifi"));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        auto *library = ui.controller.library();
        QTRY_VERIFY(library->genreOptions()->available());
        QObject *dialog = ui.popup(QStringLiteral("genreFilterDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QQuickItem *option = nullptr;
        QTRY_VERIFY((option = ui.item(QStringLiteral("genreOption_") + kSciFi)) != nullptr);
        QTRY_VERIFY(option->isVisible() && option->width() > 0);
        QVERIFY(ui.showsText(QStringLiteral("Science Fiction")));
        ui.clickCenter(option);
        QTRY_COMPARE(library->genreIds(), QStringList({kSciFi}));
        QTRY_COMPARE(library->rowCount(), 2);
        QCOMPARE(option->property("selected").toBool(), true);
        // The list is the focus: Space on the current row toggles it back
        QQuickItem *list = ui.item(QStringLiteral("genreOptionsList"));
        QVERIFY(list);
        list->forceActiveFocus();
        QTest::keyClick(ui.window, Qt::Key_Space);
        QTRY_VERIFY(library->genreIds().isEmpty());
        QTRY_VERIFY(!library->filtered());
    }

    void genreActionsAreHiddenWhenTheLibraryHasNoGenres()
    {
        Ui ui(m_home->path());
        ui.server.listingRawBody.insert(QStringLiteral("Filters"),
                                        QByteArrayLiteral("{\"Genres\":[]}"));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        auto *library = ui.controller.library();
        QTRY_COMPARE(library->genreOptions()->status(), Kinefin::FilterOptionsModel::Ready);
        QTRY_COMPARE(library->status(), Kinefin::LibraryItemsModel::Ready);
        QVERIFY(!library->genreOptions()->available());
        QVERIFY(!ui.showsText(QStringLiteral("Browse by Genre")));
        QVERIFY(!ui.showsText(QStringLiteral("Filter by Genre…")));
    }

    void genreNamesStayPlainText()
    {
        Ui ui(m_home->path());
        const QString hostile =
            QStringLiteral("<b>Noir</b> & <img src=\"http://evil.invalid/x.png\">");
        const QJsonArray genres{
            QJsonObject{{QStringLiteral("Id"), kSciFi}, {QStringLiteral("Name"), hostile}}};
        ui.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(QJsonObject{{QStringLiteral("Genres"), genres}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_VERIFY(ui.controller.library()->genreOptions()->available());
        QVERIFY(QMetaObject::invokeMethod(ui.popup(QStringLiteral("genreFilterDialog")), "open"));
        QTRY_VERIFY(!ui.itemsWithText(hostile).isEmpty());
        for (QQuickItem *label : ui.itemsWithText(hostile))
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
        // The genre name never lands in a control's own text, where a mnemonic or markup could apply
        for (QQuickItem *holder : ui.itemsWithText(hostile))
            QVERIFY2(holder->inherits("QQuickText"), holder->metaObject()->className());
    }

    void genreRailsLoadAsTheyScrollAndShowAllFiltersTheGrid()
    {
        Ui ui(m_home->path());
        ui.server.generatedGenreTotal = 40;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        auto *library = ui.controller.library();
        QTRY_VERIFY(library->genreOptions()->available());
        const int libraryDepth = ui.stackDepth();
        QQuickItem *browse = nullptr;
        QTRY_VERIFY((browse = [&]() -> QQuickItem * {
                        for (QQuickItem *candidate :
                             ui.itemsWithText(QStringLiteral("Browse by Genre"))) {
                            if (candidate->isVisible() && candidate->width() > 0)
                                return candidate;
                        }
                        return nullptr;
                    }())
                    != nullptr);
        ui.clickCenter(browse);
        QTRY_VERIFY(ui.item(QStringLiteral("genreRailsPage")) != nullptr);
        QCOMPARE(ui.stackDepth(), libraryDepth + 1);
        auto *rails = ui.controller.genreRails();
        QTRY_COMPARE(rails->status(), Kinefin::GenreRailsModel::Ready);
        QTRY_VERIFY(ui.item(QStringLiteral("genreRail_0")) != nullptr);
        // Only the rails in view ask for their items: the first page of genres is 20, the view shows a few
        QTRY_VERIFY(ui.server.itemsRoutes.size() >= 1);
        QTest::qWait(300);
        const int firstRoutes = ui.server.itemsRoutes.size();
        QVERIFY2(firstRoutes < 20, qPrintable(QString::number(firstRoutes)));
        QQuickItem *railList = ui.item(QStringLiteral("genreRailsList"));
        QVERIFY(railList);
        QVERIFY(QMetaObject::invokeMethod(railList, "positionViewAtEnd"));
        QTRY_VERIFY(ui.server.itemsRoutes.size() > firstRoutes);
        // The next page of genres follows the end of the list
        QTRY_VERIFY(rails->rowCount() > 20);
        // Show All on the first rail returns to the grid filtered by that genre
        QVERIFY(QMetaObject::invokeMethod(railList, "positionViewAtBeginning"));
        QQuickItem *showAll = nullptr;
        QTRY_VERIFY((showAll = [&]() -> QQuickItem * {
                        for (QQuickItem *candidate : ui.itemsWithText(QStringLiteral("Show All"))) {
                            if (candidate->isVisible() && candidate->width() > 0
                                && candidate->mapToScene(QPointF(0, 0)).y() > 0)
                                return candidate;
                        }
                        return nullptr;
                    }())
                    != nullptr);
        const QString firstGenre =
            rails->data(rails->index(0), Kinefin::GenreRailsModel::GenreIdRole).toString();
        QVERIFY(ui.revealAndClickUntil(ui.item(QStringLiteral("genreRailsPage")), showAll,
                                       [&] { return ui.stackDepth() == libraryDepth; }));
        QTRY_COMPARE(library->genreIds(), QStringList({firstGenre}));
        QVERIFY(ui.item(QStringLiteral("libraryPage")) != nullptr);
        // The library page is current again and its toolbar builds its items: tearing the window down meanwhile makes Kirigami warn
        QTest::qWait(1000);
    }

    void aFailedRailShowsItsOwnRetry()
    {
        Ui ui(m_home->path());
        ui.server.generatedGenreTotal = 2;
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        ui.server.listingStatus.remove(QStringLiteral("Items"));
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_VERIFY(ui.controller.library()->genreOptions()->available());
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openGenres",
                                          Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not load this genre"), false));
        QVERIFY(!ui.showsText(QStringLiteral("Could not load the genres"), false));
        ui.server.listingStatus.clear();
        // One Retry per failed rail: each click reloads one rail and removes its box
        const auto retryButtons = [&ui] {
            QList<QQuickItem *> found;
            for (QQuickItem *candidate : allItems(ui.item(QStringLiteral("genreRailsPage")))) {
                if (candidate->metaObject()->indexOfMethod("click()") >= 0
                    && candidate->property("text").toString() == QStringLiteral("Retry")
                    && candidate->isVisible() && candidate->width() > 0)
                    found.append(candidate);
            }
            return found;
        };
        // The page slides in first: a click before it holds still lands beside its target
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_COMPARE(retryButtons().size(), 2);
        for (int remaining = 2; remaining > 0; --remaining) {
            QTRY_COMPARE(retryButtons().size(), remaining);
            ui.clickCenter(retryButtons().constFirst());
            QTRY_VERIFY(retryButtons().size() < remaining);
        }
        QTRY_VERIFY(!ui.showsText(QStringLiteral("Could not load this genre"), false));
        QTRY_VERIFY(ui.item(QStringLiteral("genreRailRow_0")) != nullptr);
    }

    void downWalksTheGenreRails()
    {
        Ui ui(m_home->path());
        ui.server.generatedGenreTotal = 6;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QVERIFY(ui.controller.openGenreRails());
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openGenres",
                                          Q_ARG(QVariant, QVariant())));
        QQuickItem *first = nullptr;
        QTRY_VERIFY((first = ui.item(QStringLiteral("genreRailRow_0"))) != nullptr);
        QTRY_COMPARE(ui.controller.genreRails()
                         ->data(ui.controller.genreRails()->index(1),
                                Kinefin::GenreRailsModel::RailStatusRole)
                         .toInt(),
                     int(Kinefin::GenreRailsModel::RailReady));
        QTRY_VERIFY(ui.item(QStringLiteral("genreRailRow_1")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(first, "forceActiveFocus",
                                          Q_ARG(Qt::FocusReason, Qt::TabFocusReason)));
        QTRY_VERIFY(first->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QQuickItem *second = ui.item(QStringLiteral("genreRailRow_1"));
        QVERIFY(second);
        QTRY_VERIFY(second->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(first->hasActiveFocus());
    }

    void everyControlIsInsideTheWindow_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<int>("width");
        const QStringList states{QStringLiteral("servers"),   QStringLiteral("login"),
                                 QStringLiteral("home"),      QStringLiteral("homeFailedRow"),
                                 QStringLiteral("library"),   QStringLiteral("libraryFailed"),
                                 QStringLiteral("details"),   QStringLiteral("detailsMissing"),
                                 QStringLiteral("search"),    QStringLiteral("rails"),
                                 QStringLiteral("settings"),  QStringLiteral("settingsSection"),
                                 QStringLiteral("about"),     QStringLiteral("queue"),
                                 QStringLiteral("queueEmpty")};
        for (const QString &state : states)
            QTest::addRow("%s@1000", qPrintable(state)) << state << 1000;
        for (const QString &state :
             {QStringLiteral("home"), QStringLiteral("library"), QStringLiteral("details"),
              QStringLiteral("settings"), QStringLiteral("queue")}) {
            QTest::addRow("%s@560", qPrintable(state)) << state << 560;
            QTest::addRow("%s@440", qPrintable(state)) << state << 440;
            QTest::addRow("%s@1600", qPrintable(state)) << state << 1600;
        }
    }

    void everyControlIsInsideTheWindow()
    {
        QFETCH(QString, state);
        QFETCH(int, width);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(width));
        QVERIFY2(buildState(ui, state), qPrintable(state));
        // The page slides in: the layout is judged once it holds still
        QStringList problems;
        QTRY_VERIFY2((problems = ui.layoutProblems()).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
    }

    void tabReachesOnlyTheCurrentPageAndTheShell()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        const QList<QQuickItem *> visited = ui.tabCycle();
        QVERIFY(visited.size() >= 2);
        for (QQuickItem *item : visited) {
            QVERIFY2(item->isVisible(), qPrintable(Ui::describe(item)));
            QVERIFY2(!ui.inAnotherPage(item), qPrintable(Ui::describe(item)));
            QVERIFY2(ui.insideWindow(item), qPrintable(Ui::describe(item)));
        }
        // The toolbar actions and a tile of the grid are stops
        bool grid = false;
        for (QQuickItem *item : visited)
            grid = grid || item->objectName().startsWith(QStringLiteral("mediaCard_"));
        QStringList seen;
        for (QQuickItem *item : visited)
            seen << Ui::describe(item);
        QVERIFY2(grid, qPrintable(seen.join(QLatin1Char('\n'))));
    }

    void altLeftAndEscapeGoBackAndDestroyThePage_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("modifiers");
        QTest::newRow("Alt+Left") << int(Qt::Key_Left) << int(Qt::AltModifier);
        QTest::newRow("Escape") << int(Qt::Key_Escape) << int(Qt::NoModifier);
    }

    void altLeftAndEscapeGoBackAndDestroyThePage()
    {
        QFETCH(int, key);
        QFETCH(int, modifiers);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("details")));
        QPointer<QQuickItem> details = ui.item(QStringLiteral("itemDetailsPage"));
        QVERIFY(details);
        const int depth = ui.stackDepth();
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTest::keyClick(ui.window, Qt::Key(key), Qt::KeyboardModifiers(modifiers));
        QTRY_COMPARE(ui.stackDepth(), depth - 1);
        // The page is gone, so its models gave their listing channels back
        QTRY_VERIFY(!details);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryPage")) != nullptr);
    }

    void escapeOnTheFirstPageDoesNothing()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QCOMPARE(ui.stackDepth(), 1);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
    }

    void escapeClosesTheGenreDialogFirst()
    {
        Ui ui(m_home->path());
        ui.server.itemsByGenre.insert(kSciFi, QStringLiteral("items_genre_scifi"));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.controller.library()->genreOptions()->available());
        QObject *dialog = ui.popup(QStringLiteral("genreFilterDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        const int depth = ui.stackDepth();
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), depth);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
    }

    void theSignInFieldsKeepTheirFocus()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        QTest::qWait(300);
        QVERIFY(ui.window->activeFocusItem());
        QVERIFY2(ui.window->activeFocusItem()->inherits("QQuickTextInput"),
                 qPrintable(Ui::describe(ui.window->activeFocusItem())));
        QVERIFY(buildState(ui, QStringLiteral("login")));
        QTest::qWait(300);
        QVERIFY(ui.window->activeFocusItem());
        QVERIFY2(ui.window->activeFocusItem()->inherits("QQuickTextInput"),
                 qPrintable(Ui::describe(ui.window->activeFocusItem())));
    }

    void searchAfterBackFindsNoLeftPage()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.openSearchPage() != nullptr);
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        // Ctrl+F while the page Back left is still sliding out
        QTest::keyClick(ui.window, Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QTRY_VERIFY(ui.window->activeFocusItem()
                    && ui.window->activeFocusItem()->objectName() == ui.searchFieldName());
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("searchPage"));
    }

    void noSidebarRemains()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // The old shell's drawer and its rows are gone for good
        QVERIFY(!ui.window->property("globalDrawer").value<QObject *>());
        for (QQuickItem *candidate : allItems(ui.window->contentItem()))
            QVERIFY2(!QString::fromLatin1(candidate->metaObject()->className())
                          .contains(QLatin1String("GlobalDrawer")),
                     qPrintable(Ui::describe(candidate)));
    }

    void theHeaderModesFollowTheWidth()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // The limits the shell itself derives from grid units, in pixels
        const int compactLimit = ui.window->property("compactLimit").toInt();
        const int wideLimit = ui.window->property("wideLimit").toInt();
        QVERIFY(compactLimit > 0 && wideLimit > compactLimit);
        const struct Mode {
            int width;
            bool wide;
            bool compact;
        } modes[] = {{wideLimit * 2, true, false},
                     {wideLimit + 10, true, false},
                     {compactLimit + 40, false, false},
                     {compactLimit + 1, false, false},
                     {compactLimit - 40, false, true}};
        for (const Mode &mode : modes) {
            QVERIFY2(ui.opensWindowOfWidth(mode.width), qPrintable(QString::number(mode.width)));
            // A resize reaches the QML bindings that read the width on the next event loop pass: QTRY lets them settle
            QTRY_VERIFY2(ui.shellHeader()->property("compact").toBool() == mode.compact,
                         qPrintable(QStringLiteral("%1 px: compact %2, want %3")
                                        .arg(mode.width)
                                        .arg(ui.shellHeader()->property("compact").toBool())
                                        .arg(mode.compact)));
            QTRY_VERIFY2(ui.shellHeader()->property("wide").toBool() == mode.wide,
                         qPrintable(QStringLiteral("%1 px: wide %2, want %3")
                                        .arg(mode.width)
                                        .arg(ui.shellHeader()->property("wide").toBool())
                                        .arg(mode.wide)));
            QStringList problems = ui.modeProblems(mode.wide, mode.compact);
            QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("%1 px: %2")
                                                        .arg(mode.width)
                                                        .arg(problems.join(QLatin1Char('\n')))));
            // The menu carries what the bar hid, and nothing more
            QObject *menu = ui.popup(QStringLiteral("mainMenu"));
            QVERIFY(menu);
            QVERIFY(QMetaObject::invokeMethod(menu, "open"));
            QTRY_VERIFY(menu->property("opened").toBool());
            const QStringList shown = [&ui, &menu] {
                QStringList names;
                for (QQuickItem *entry : ui.menuItems(QStringLiteral("mainMenu"))) {
                    if (entry && entry->isVisible() && entry->property("action").isValid())
                        names << entry->property("action").value<QObject *>()->objectName();
                }
                return names;
            }();
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            if (mode.compact) {
                for (const char *name :
                     {"compactSearchAction", "compactFavoritesAction", "compactQueueAction",
                      "settingsAction", "shortcutsAction", "aboutAction", "adminDashboardAction"})
                    QVERIFY2(shown.contains(QLatin1String(name)),
                             qPrintable(QStringLiteral("%1 px: %2 missing")
                                            .arg(mode.width)
                                            .arg(QLatin1String(name))));
            } else {
                for (const char *name :
                     {"compactSearchAction", "compactFavoritesAction", "compactQueueAction"})
                    QVERIFY2(!shown.contains(QLatin1String(name)),
                             qPrintable(QStringLiteral("%1 px: %2 shown")
                                            .arg(mode.width)
                                            .arg(QLatin1String(name))));
                QVERIFY2(shown.contains(QLatin1String("settingsAction")),
                         qPrintable(shown.join(QLatin1Char('|'))));
            }
            // The page keeps its place under the bar in every mode
            QTRY_VERIFY2((problems = ui.layoutProblems()).isEmpty(),
                         qPrintable(problems.join(QLatin1Char('\n'))));
        }
    }

    void theHeaderTabMarksTheCurrentSection()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *tab = ui.tabButton();
        QVERIFY(tab);
        // Home marks itself, and only itself, wherever the front page belongs
        const auto marked = [&](bool expected) {
            return tab->property("checked").toBool() == expected;
        };
        QTRY_VERIFY(marked(true));
        QVERIFY(ui.openSearchPage() != nullptr);
        QTRY_VERIFY(marked(false));
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_VERIFY(marked(false));
        // The main menu button marks its own pages while one of them shows
        QTRY_VERIFY(ui.item(QStringLiteral("mainMenuButton"))->property("down").toBool());
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger");
        QTRY_VERIFY(ui.item(QStringLiteral("mainMenuButton"))->property("down").toBool());
        // Ctrl+1 back to the same Home: the tab marks it again
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_VERIFY(marked(true));
        QTRY_VERIFY(!ui.item(QStringLiteral("mainMenuButton"))->property("down").toBool());
        // A library opened from a tile marks nothing: the type tabs are not there yet
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryPage")) != nullptr);
        QTRY_VERIFY(marked(false));
        QTRY_VERIFY(!ui.item(QStringLiteral("queueButton"))->property("down").toBool());
    }

    void theHeaderShortcutsOpenTheirPages()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // Ctrl+, opens Settings without a menu
        QTest::keyClick(ui.window, Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) != nullptr);
        QTRY_COMPARE(ui.stackDepth(), 2);
        // Ctrl+Shift+/ shows the dialog of the shortcuts
        QTest::keyClick(ui.window, Qt::Key_Slash, Qt::ControlModifier | Qt::ShiftModifier);
        QObject *dialog = ui.popup(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Go Back")));
        // The rows: every action the shell answers with, and the keys of the row that shows two modifiers (the keys sit in their own boxes, one label per part)
        for (const QString &action :
             {QStringLiteral("Quit Kinefin"), QStringLiteral("Search"), QStringLiteral("Home"),
              QStringLiteral("Settings"), QStringLiteral("Show Keyboard Shortcuts")})
            QTRY_VERIFY2(ui.showsText(action), qPrintable(action));
        for (const QString &key :
             {QStringLiteral("Ctrl"), QStringLiteral("Shift"), QStringLiteral("/")})
            QTRY_VERIFY2(ui.showsText(key), qPrintable(key));
        // Escape closes the dialog first, and nothing else moved
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("settingsPage"));
        // Ctrl+1 goes back to Home
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_COMPARE(ui.stackDepth(), 1);
    }

    void typingInTheHeaderBoxOpensTheSearchPage()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // The first keystroke in the header box opens the page above Home and keeps the term
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(field);
        field->forceActiveFocus(Qt::TabFocusReason);
        ui.type(QStringLiteral("a"));
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("headerSearchField"))->hasActiveFocus());
        // The page keeps its own field hidden on a wide window, where the header box is the field
        QTRY_VERIFY([&] {
            QQuickItem *pageField = ui.item(QStringLiteral("searchField"));
            return pageField && !pageField->isVisible();
        }());
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("a"));
        ui.type(QStringLiteral("byss"));
        QTRY_COMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("abyss"));
        // Leaving the search page takes the term with it: the box empties
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) == nullptr);
        QCOMPARE(ui.controller.search()->term(), QString());
        QTRY_COMPARE(ui.item(QStringLiteral("headerSearchField"))->property("text").toString(),
                     QString());
        // The search page's own field shows only in Compact mode, where the header has none
        QVERIFY(ui.opensWindowOfWidth(ui.window->property("compactLimit").toInt() - 40));
        QVERIFY(ui.openSearchPage() != nullptr);
        ui.type(QStringLiteral("arc"));
        QTRY_COMPARE(ui.controller.search()->state(), Kinefin::SearchModel::Results);
        QQuickItem *pageField = ui.item(QStringLiteral("searchField"));
        QVERIFY(pageField);
        QTRY_VERIFY(pageField->isVisible());
        QCOMPARE(pageField->property("text").toString(), QStringLiteral("arc"));
    }

    void tabWalksTheHeaderBeforeThePage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QQuickItem *menuButton = ui.item(QStringLiteral("mainMenuButton"));
        QVERIFY(menuButton);
        menuButton->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(menuButton->hasActiveFocus());
        // The order inside the header: the tab, then the right cluster (the disabled favorites is skipped)
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.tabButton()->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("headerSearchField"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("queueButton"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("accountButton"))->hasActiveFocus());
        // Behind the account button the page content begins: Back from the menu button wraps, so nothing of the header sits before it
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QQuickItem *next = ui.window->activeFocusItem();
        QVERIFY(next && next != ui.tabButton() && next != menuButton
                && next->objectName() != QStringLiteral("headerSearchField")
                && next->objectName() != QStringLiteral("queueButton")
                && next->objectName() != QStringLiteral("accountButton"));
    }

    void homeReturnsToTheSameHomePage()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QPointer<QQuickItem> home = ui.item(QStringLiteral("homePage"));
        QVERIFY(home);
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(ui.currentDetails(), home.data());
    }

    void theHeaderKeepsThePageWholeWhileTheWidthChanges()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        // The modes flip as the width crosses the grid-unit lines; whatever the mode, the page in
        // front keeps the keyboard focus and fills the window under the bar
        for (const int width : {1000, 440, 1000, 560, 1600, 640, 440, 1200}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            QTRY_VERIFY2(ui.layoutProblems().isEmpty(),
                         qPrintable(ui.layoutProblems().join(QLatin1Char('\n'))));
            QTRY_VERIFY(ui.window->activeFocusItem()
                        && (ui.window->activeFocusItem() == ui.currentDetails()
                            || ui.currentDetails()->isAncestorOf(ui.window->activeFocusItem())));
        }
    }

    void sectionsNeverGrowTheStack()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        for (const char *name :
             {"settingsAction", "aboutAction", "settingsAction", "aboutAction", "settingsAction"}) {
            QMetaObject::invokeMethod(ui.shellAction(QLatin1String(name)), "trigger");
            QTRY_VERIFY(ui.currentDetails()->objectName()
                        == (QLatin1String(name) == QLatin1String("settingsAction")
                                ? QStringLiteral("settingsPage")
                                : QStringLiteral("aboutPage")));
            QTRY_COMPARE(ui.stackDepth(), 2);
        }
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 2);
    }

    void theHeaderDisablesWhatNeedsASession()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        // Sections that need a session are there but dead
        QVERIFY(!ui.shellAction(QStringLiteral("homeTabAction"))->property("enabled").toBool());
        QVERIFY(!ui.item(QStringLiteral("queueButton"))->isEnabled());
        QVERIFY(!ui.item(QStringLiteral("headerSearchField"))->isEnabled());
        QVERIFY(ui.item(QStringLiteral("favoritesButton"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesButton"))->isEnabled());
        // Settings and About work without one
        QVERIFY(ui.shellAction(QStringLiteral("settingsAction"))->property("enabled").toBool());
        QVERIFY(ui.shellAction(QStringLiteral("aboutAction"))->property("enabled").toBool());
        // The server and session commands have nothing to act on yet
        QVERIFY(
            !ui.shellAction(QStringLiteral("adminDashboardAction"))->property("visible").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("signOutAction"))->property("visible").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("switchUserAction"))->property("visible").toBool());
        QVERIFY(
            !ui.shellAction(QStringLiteral("switchServerAction"))->property("visible").toBool());
        QVERIFY(!ui.item(QStringLiteral("accountButton"))->isVisible());

        QVERIFY(buildState(ui, QStringLiteral("login")));
        // A chosen server brings the account menu in, with only Switch Server to act on
        QVERIFY(ui.item(QStringLiteral("accountButton"))->isVisible());
        QVERIFY(ui.shellAction(QStringLiteral("switchServerAction"))->property("visible").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("signOutAction"))->property("visible").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("switchUserAction"))->property("visible").toBool());
    }

    void switchUserForgetsTheTokenAndLandsOnTheSignIn()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(!ui.store.tokens.isEmpty());
        // The account menu opens from its button, and Switch User is one of its entries
        ui.clickCenter(ui.item(QStringLiteral("accountButton")));
        QObject *menu = ui.popup(QStringLiteral("accountMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("switchUserAction")),
                                          "trigger"));
        // The session goes, the server stays chosen: the sign-in of the same server is next
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("loginPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) == nullptr);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Server: "), false));
        QTRY_VERIFY(ui.store.tokens.isEmpty());
        QCOMPARE(ui.controller.servers().size(), 1);
        // The same account signs back in
        ui.controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
    }

    void signOutForgetsEverythingAndReturnsToTheServers()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        ui.clickCenter(ui.item(QStringLiteral("accountButton")));
        QTRY_VERIFY(ui.popup(QStringLiteral("accountMenu"))->property("opened").toBool());
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("signOutAction")), "trigger"));
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QTRY_VERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        QTRY_VERIFY(ui.store.tokens.isEmpty());
        // The server record went with the session, unlike Switch User's
        QVERIFY(ui.controller.servers().isEmpty());
    }

    void switchServerKeepsTheSessionAndReturnsToTheServers()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        ui.clickCenter(ui.item(QStringLiteral("accountButton")));
        QTRY_VERIFY(ui.popup(QStringLiteral("accountMenu"))->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("switchServerAction")),
                                          "trigger"));
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QTRY_VERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        // The session of the server stays: a start finds it again without a password
        QVERIFY(!ui.store.tokens.isEmpty());
        QCOMPARE(ui.controller.servers().size(), 1);
        ui.controller.start();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
    }

    void theGridTakesTheKeysWhenTheLibraryOpens()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid"))->hasActiveFocus());
    }

    void backReturnsTheFocusToTheOpenedTile()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_COMPARE(ui.controller.library()->status(), Kinefin::LibraryItemsModel::Ready);
        QQuickItem *tile = nullptr;
        QTRY_VERIFY([&] {
            for (QQuickItem *candidate : allItems(grid)) {
                if (candidate->objectName().startsWith(QStringLiteral("mediaCard_"))
                    && candidate->property("index").toInt() == 3 && candidate->isVisible()) {
                    tile = candidate;
                    return true;
                }
            }
            return false;
        }());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        ui.clickCenter(tile);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
        QTRY_VERIFY(grid->hasActiveFocus());
        QCOMPARE(grid->property("currentIndex").toInt(), 3);
    }

    void backReturnsTheFocusToTheHomeRow()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
        QTRY_VERIFY(row->isVisible() && row->property("count").toInt() > 0);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(row, "forceActiveFocus",
                                          Q_ARG(Qt::FocusReason, Qt::TabFocusReason)));
        QTRY_VERIFY(row->hasActiveFocus());
        // The key is pressed again until the row has a current card to open
        QTRY_VERIFY([&] {
            if (ui.currentDetails()->objectName() == QStringLiteral("homePage"))
                QTest::keyClick(ui.window, Qt::Key_Return);
            return ui.currentDetails()->objectName() == QStringLiteral("itemDetailsPage");
        }());
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_VERIFY(row->hasActiveFocus());
    }

    void downKeepsTheFocusedRowInView()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        // A low window: the rows do not all fit, so the page has to scroll
        ui.window->resize(1000, 300);
        QTRY_COMPARE(ui.window->height(), 300);
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // The rows with cards, top to bottom
        const auto rows = [&ui] {
            QList<QQuickItem *> found;
            for (QQuickItem *row : ui.itemsStartingWith(QStringLiteral("homeRowList_"))) {
                if (row->isVisible() && row->property("count").toInt() > 0)
                    found.append(row);
            }
            std::sort(found.begin(), found.end(), [](const QQuickItem *a, const QQuickItem *b) {
                return a->mapToScene(QPointF(0, 0)).y() < b->mapToScene(QPointF(0, 0)).y();
            });
            return found;
        };
        QTRY_VERIFY(rows().size() >= 2);
        // Rows still loading would change the list under the walk: it counts once it holds still
        qsizetype seen = -1;
        QTRY_VERIFY([&] {
            const qsizetype now = rows().size();
            const bool still = now == seen;
            seen = now;
            QTest::qWait(300);
            return still;
        }());
        const QList<QQuickItem *> lists = rows();
        QVERIFY(QMetaObject::invokeMethod(lists.constFirst(), "forceActiveFocus",
                                          Q_ARG(Qt::FocusReason, Qt::TabFocusReason)));
        QTRY_VERIFY(lists.constFirst()->hasActiveFocus());
        for (qsizetype row = 1; row < lists.size(); ++row) {
            QTest::keyClick(ui.window, Qt::Key_Down);
            QTRY_VERIFY2(lists.at(row)->hasActiveFocus(), qPrintable(QString::number(row)));
            // The row is whole inside the window once the page has scrolled to it
            QTRY_VERIFY2(
                [&] {
                    const QRectF rect = lists.at(row)->mapRectToScene(
                        QRectF(0, 0, lists.at(row)->width(), lists.at(row)->height()));
                    return rect.top() >= 0 && rect.bottom() <= ui.window->height() + 1;
                }(),
                qPrintable(QString::number(row)));
        }
        // The page scrolled to reach the last row
        QVERIFY(ui.item(QStringLiteral("homePage"))
                    ->property("flickable")
                    .value<QQuickItem *>()
                    ->property("contentY")
                    .toReal()
                > 0);
        // Up goes back, and Down from the last row stays where it is
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(lists.at(lists.size() - 2)->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(lists.constLast()->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTest::qWait(200);
        QVERIFY(lists.constLast()->hasActiveFocus());
    }

    void reopeningTheOpenLibraryKeepsItsPage()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QPointer<QQuickItem> library = ui.item(QStringLiteral("libraryPage"));
        QVERIFY(library);
        ui.controller.library()->setFilter(Kinefin::LibraryItemsModel::Unplayed);
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openLibrary",
                                          Q_ARG(QVariant, kMovies),
                                          Q_ARG(QVariant, QStringLiteral("Movies"))));
        QTest::qWait(500);
        QCOMPARE(ui.currentDetails(), library.data());
        QCOMPARE(ui.controller.library()->filter(), Kinefin::LibraryItemsModel::Unplayed);
        QCOMPARE(ui.stackDepth(), 2);
    }

    // The queue page of a queue holding the five fixture episodes, in front
    QQuickItem *openQueueWithEpisodes(Ui &ui)
    {
        if (!buildState(ui, QStringLiteral("queue")))
            return nullptr;
        return ui.item(QStringLiteral("queueList"));
    }

    void theQueueButtonOpensOnce()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *button = ui.item(QStringLiteral("queueButton"));
        QVERIFY(button);
        for (int i = 0; i < 3; ++i) {
            ui.clickCenter(button);
            QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("queuePage"));
            QTRY_COMPARE(ui.stackDepth(), 2);
        }
        QTRY_VERIFY(button->property("down").toBool());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
    }

    void anEmptyQueueSaysSo()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("queueEmpty")));
        QTRY_VERIFY(ui.showsText(QStringLiteral("The queue is empty")));
        const auto clear = [&ui] {
            QQmlListReference actions(ui.item(QStringLiteral("queuePage")), "actions");
            for (qsizetype i = 0; i < actions.count(); ++i) {
                if (actions.at(i)->property("text").toString() == QStringLiteral("Clear Queue"))
                    return actions.at(i);
            }
            return static_cast<QObject *>(nullptr);
        };
        QVERIFY(clear());
        QVERIFY(!clear()->property("enabled").toBool());
    }

    void theDetailsPageAddsItsItemToTheQueue()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("details")));
        QQmlListReference actions(ui.item(QStringLiteral("itemDetailsPage")), "actions");
        const auto action = [&actions](const QString &text) {
            for (qsizetype i = 0; i < actions.count(); ++i) {
                if (actions.at(i)->property("text").toString() == text)
                    return actions.at(i);
            }
            return static_cast<QObject *>(nullptr);
        };
        QVERIFY(action(QStringLiteral("Add to Queue")));
        QVERIFY(action(QStringLiteral("Play Next")));
        QVERIFY(action(QStringLiteral("Add to Queue"))->property("visible").toBool());
        QMetaObject::invokeMethod(action(QStringLiteral("Add to Queue")), "trigger");
        QTRY_COMPARE(ui.controller.queue()->count(), 1);
        QCOMPARE(ui.controller.queue()
                     ->data(ui.controller.queue()->index(0), Kinefin::PlayQueue::ItemIdRole)
                     .toString(),
                 kMovieId);
        // Adding it again with Play Next keeps one row
        QTRY_VERIFY(action(QStringLiteral("Play Next"))->property("enabled").toBool());
        QMetaObject::invokeMethod(action(QStringLiteral("Play Next")), "trigger");
        QTRY_VERIFY(!ui.controller.queue()->busy());
        QCOMPARE(ui.controller.queue()->count(), 1);
    }

    void aSeriesPageQueuesItsEpisodes()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        ui.server.generatedEpisodeTotal = 4;
        ui.server.listingRawBody.insert(QStringLiteral("NextUp"), envelope({}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openItem",
                                          Q_ARG(QVariant, kSeriesId), Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QQmlListReference actions(ui.item(QStringLiteral("itemDetailsPage")), "actions");
        QObject *playNext = nullptr;
        QTRY_VERIFY([&] {
            for (qsizetype i = 0; i < actions.count(); ++i) {
                if (actions.at(i)->property("text").toString() == QStringLiteral("Play Next")
                    && actions.at(i)->property("visible").toBool())
                    playNext = actions.at(i);
            }
            return playNext != nullptr;
        }());
        QMetaObject::invokeMethod(playNext, "trigger");
        QTRY_COMPARE(ui.controller.queue()->count(), 4);
    }

    void aFailedRestoreOffersRetryOnTheQueuePage()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        auto *queue = ui.controller.queue();
        queue->setSaveDelay(std::chrono::milliseconds(20));
        QVERIFY(queue->add(kMovieId, Kinefin::PlayQueue::Last));
        const QString file =
            m_home->filePath(QStringLiteral("data/kinefin/profiles/default/queue.json"));
        QTRY_VERIFY(QFileInfo::exists(file));
        QFile stored(file);
        QVERIFY(stored.open(QIODevice::ReadOnly));
        const QJsonObject owner = QJsonDocument::fromJson(stored.readAll()).object();
        // The restore after the next sign-in fails
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
        queue->restoreFor(owner.value(QStringLiteral("serverId")).toString(),
                          owner.value(QStringLiteral("userId")).toString());
        QTRY_VERIFY(queue->restoreFailed());
        const QVariantMap properties{
            {QStringLiteral("connection"), QVariant::fromValue(&ui.controller)}};
        QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openSection",
                                  Q_ARG(QVariant, QStringLiteral("queuePage")),
                                  Q_ARG(QVariant, QStringLiteral("QueuePage.qml")),
                                  Q_ARG(QVariant, properties));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not restore the queue"), false));
        QVERIFY(!ui.showsText(QStringLiteral("The queue is empty")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        ui.server.listingStatus.clear();
        QQuickItem *retry = nullptr;
        QTRY_VERIFY([&] {
            for (QQuickItem *candidate : allItems(ui.item(QStringLiteral("queuePage")))) {
                if (candidate->metaObject()->indexOfMethod("click()") >= 0
                    && candidate->property("text").toString() == QStringLiteral("Retry")
                    && candidate->isVisible() && candidate->width() > 0)
                    retry = candidate;
            }
            return retry != nullptr;
        }());
        QMetaObject::invokeMethod(retry, "click");
        QTRY_COMPARE(queue->count(), 1);
        QTRY_VERIFY(!ui.showsText(QStringLiteral("Could not restore the queue"), false));
    }

    void theQueueListIsReachedByTabAndKeepsItsKeys()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QQuickItem *list = openQueueWithEpisodes(ui);
        QVERIFY(list);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // The list has the focus when the page opens and is the one stop of the page
        QTRY_VERIFY(list->hasActiveFocus());
        bool reached = false;
        QStringList stops;
        for (QQuickItem *item : ui.tabCycle(200)) {
            reached = reached || item == list || list->isAncestorOf(item);
            stops << Ui::describe(item);
        }
        QVERIFY2(reached, qPrintable(stops.join(QLatin1Char('\n'))));
    }

    void keyboardReordersAndRemovesRows()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QQuickItem *list = openQueueWithEpisodes(ui);
        QVERIFY(list);
        auto *queue = ui.controller.queue();
        const auto nameAt = [queue](int row) {
            return queue->data(queue->index(row), Kinefin::PlayQueue::NameRole).toString();
        };
        QTRY_VERIFY(list->hasActiveFocus());
        QCOMPARE(nameAt(0), QStringLiteral("Episode 1"));
        QTest::keyClick(ui.window, Qt::Key_Down, Qt::ControlModifier);
        QTRY_COMPARE(nameAt(1), QStringLiteral("Episode 1"));
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(ui.window, Qt::Key_Up, Qt::ControlModifier);
        QTRY_COMPARE(nameAt(0), QStringLiteral("Episode 1"));
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        // Ctrl+Up on the first row does nothing
        QTest::keyClick(ui.window, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(nameAt(0), QStringLiteral("Episode 1"));
        QTest::keyClick(ui.window, Qt::Key_Delete);
        QTRY_COMPARE(queue->count(), 4);
        QCOMPARE(nameAt(0), QStringLiteral("Episode 2"));
        QCOMPARE(list->property("currentIndex").toInt(), 0);
    }

    void aRowOpensItsItemAndBackReturnsToIt()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QQuickItem *list = openQueueWithEpisodes(ui);
        QVERIFY(list);
        QTRY_VERIFY(list->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("queuePage"));
        QTRY_VERIFY(list->hasActiveFocus());
        QCOMPARE(list->property("currentIndex").toInt(), 1);
    }

    void theMenuKeyOpensTheRowMenuAndEscapeClosesItFirst()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QQuickItem *list = openQueueWithEpisodes(ui);
        QVERIFY(list);
        QTRY_VERIFY(list->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Move Down")));
        QVERIFY(ui.showsText(QStringLiteral("Remove")));
        QVERIFY(ui.showsText(QStringLiteral("Go to Item")));
        const int depth = ui.stackDepth();
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!ui.showsText(QStringLiteral("Move Down")));
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), depth);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("queuePage"));
        // Open again and remove the row from the menu with the keyboard
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Remove")));
        for (QQuickItem *item : ui.itemsWithText(QStringLiteral("Remove"))) {
            if (item->isVisible() && item->metaObject()->indexOfMethod("click()") >= 0) {
                QMetaObject::invokeMethod(item, "click");
                break;
            }
        }
        QTRY_COMPARE(ui.controller.queue()->count(), 4);
    }

    void clearAsksFirstAndEscapeCancels()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QQuickItem *list = openQueueWithEpisodes(ui);
        QVERIFY(list);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        const auto clearAction = [&ui] {
            QQmlListReference actions(ui.item(QStringLiteral("queuePage")), "actions");
            for (qsizetype i = 0; i < actions.count(); ++i) {
                if (actions.at(i)->property("text").toString() == QStringLiteral("Clear Queue"))
                    return actions.at(i);
            }
            return static_cast<QObject *>(nullptr);
        };
        QVERIFY(clearAction());
        QTRY_VERIFY(clearAction()->property("enabled").toBool());
        QMetaObject::invokeMethod(clearAction(), "trigger");
        QObject *dialog = ui.popup(QStringLiteral("clearQueueDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTest::qWait(300);
        QCOMPARE(ui.controller.queue()->count(), 5);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("queuePage"));
        QMetaObject::invokeMethod(clearAction(), "trigger");
        QTRY_VERIFY(dialog->property("opened").toBool());
        // The dialog's own Clear Queue button sits below the page's toolbar action of the same name
        QQuickItem *confirm = nullptr;
        QTRY_VERIFY([&] {
            confirm = nullptr;
            for (QQuickItem *item : ui.itemsWithText(QStringLiteral("Clear Queue"))) {
                if (item->isVisible() && item->width() > 0
                    && item->metaObject()->indexOfMethod("click()") >= 0
                    && (!confirm
                        || item->mapToScene(QPointF(0, 0)).y()
                               > confirm->mapToScene(QPointF(0, 0)).y()))
                    confirm = item;
            }
            return confirm != nullptr
                   && confirm->mapToScene(QPointF(0, 0)).y() > ui.window->height() / 4.0;
        }());
        QMetaObject::invokeMethod(confirm, "click");
        QTRY_COMPARE(ui.controller.queue()->count(), 0);
        QTRY_VERIFY(ui.showsText(QStringLiteral("The queue is empty")));
    }

    void queueTextStaysPlainText()
    {
        Ui ui(m_home->path());
        const QString hostile =
            QStringLiteral("<b>Bold</b> <img src=\"http://evil.invalid/x.png\">");
        ui.serveMovie();
        const QJsonObject movie{{QStringLiteral("Id"), kMovieId},
                                {QStringLiteral("Type"), QStringLiteral("Movie")},
                                {QStringLiteral("Name"), hostile}};
        ui.server.itemsById.insert(kMovieId, QJsonDocument(movie).toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.controller.queue()->add(kMovieId, Kinefin::PlayQueue::Last));
        QTRY_COMPARE(ui.controller.queue()->count(), 1);
        const QVariantMap properties{
            {QStringLiteral("connection"), QVariant::fromValue(&ui.controller)}};
        QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openSection",
                                  Q_ARG(QVariant, QStringLiteral("queuePage")),
                                  Q_ARG(QVariant, QStringLiteral("QueuePage.qml")),
                                  Q_ARG(QVariant, properties));
        QTRY_VERIFY(!ui.itemsWithText(hostile, false).isEmpty());
        for (QQuickItem *label : ui.itemsWithText(hostile, false))
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void aLostSessionEmptiesTheQueue()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(openQueueWithEpisodes(ui));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        // The rows have their pictures: signing out while they load makes the image fetcher refuse the late requests
        QTRY_VERIFY(ui.server.imageRequests >= 1);
        QTest::qWait(500);
        ui.controller.signOut();
        QCOMPARE(ui.controller.queue()->count(), 0);
        QTRY_VERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
    }

    void focusFollowsTheCurrentPage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        const auto focusIsIn = [&](const QString &page) {
            QQuickItem *root = ui.item(page);
            for (QQuickItem *focused = ui.window->activeFocusItem(); focused;
                 focused = focused->parentItem()) {
                if (focused == root)
                    return true;
            }
            return false;
        };
        QTRY_VERIFY(focusIsIn(QStringLiteral("libraryPage")));
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(focusIsIn(QStringLiteral("homePage")));
    }

    void aFailedHomeRowOffersItsOwnRetry()
    {
        Ui ui(m_home->path());
        ui.server.listingStatus.insert(QStringLiteral("Resume"), 500);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not load this row"), false));
        // The other rows are unaffected
        QTRY_VERIFY(ui.showsText(QStringLiteral("Next Up")));
        ui.server.listingStatus.clear();
        ui.controller.homeRows()->retry(0);
        QTRY_COMPARE(
            ui.controller.homeRows()
                ->data(ui.controller.homeRows()->index(0), Kinefin::HomeRowsModel::StatusRole)
                .toInt(),
            int(Kinefin::HomeRowsModel::Ready));
        QVERIFY(!ui.showsText(QStringLiteral("Could not load this row"), false));
        QQuickItem *list = ui.item(QStringLiteral("homeRowList_0"));
        QVERIFY(list);
        QTRY_VERIFY(list->isVisible());
        QTRY_COMPARE(list->property("count").toInt(), 2);
    }

    void serverMetadataStaysPlainText()
    {
        Ui ui(m_home->path());
        const QString hostile =
            QStringLiteral("<b>bold</b><img src=\"http://evil.invalid/x.png\">");
        const auto episode =
            QJsonObject{{QStringLiteral("Id"), QStringLiteral("b1b2c3d4e5f64788990a1b2c3d4e5f60")},
                        {QStringLiteral("Type"), QStringLiteral("Episode")},
                        {QStringLiteral("SeriesName"), hostile},
                        {QStringLiteral("Name"), hostile},
                        {QStringLiteral("ParentIndexNumber"), 1},
                        {QStringLiteral("IndexNumber"), 2}};
        ui.server.listingRawBody.insert(
            QStringLiteral("Resume"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"), QJsonArray{episode}},
                                      {QStringLiteral("TotalRecordCount"), 1}})
                .toJson(QJsonDocument::Compact));
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"),
                                       QJsonArray{QJsonObject{
                                           {QStringLiteral("Id"),
                                            QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e0f")},
                                           {QStringLiteral("Name"), hostile}}}},
                                      {QStringLiteral("TotalRecordCount"), 1}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(!ui.itemsWithText(hostile).isEmpty());
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.controller.library()->status(), Kinefin::LibraryItemsModel::Ready);
        QTRY_VERIFY(ui.itemsWithText(hostile).size() >= 2);
        // Every label that carries the markup renders it as text, never as rich text. The card's second line carries the markup too, after "S1:E2 - "
        const QList<QQuickItem *> labels = ui.itemsWithText(hostile, false);
        QVERIFY(!labels.isEmpty());
        for (QQuickItem *label : labels) {
            QVERIFY(label->property("textFormat").isValid());
            // Text.PlainText is Qt::PlainText, 0: not RichText (1), AutoText (2) or Markdown (5)
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }
        // No image was requested for the markup's <img>: the only image routes are the server's own
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void lossOfTheSessionReturnsToSignIn()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 20;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        // The token stops working: the next call ends the session and the stack returns to sign-in
        ui.server.denyViews = true;
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryPage")) == nullptr);
        QVERIFY(ui.item(QStringLiteral("homePage")) == nullptr);
        QCOMPARE(ui.controller.library()->status(), Kinefin::LibraryItemsModel::Idle);
        QCOMPARE(ui.controller.homeRows()->rowCount(), 0);
        QVERIFY(ui.origin->get().isEmpty());
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_MAIN(TestUiPages)
#include "test_ui_pages.moc"
