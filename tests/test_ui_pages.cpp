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

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlListReference>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
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
const QString kPlayedMovieId = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e01");
const QString kShowCardId = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e02");
const QString kOldMovieId = QStringLiteral("f0e1d2c3b4a5968778695a4b3c2d1e03");
const QString kMovies2 = QStringLiteral("e2a1aa11a8d94b5c999a25cf59abcd02");
const QString kSeries = QStringLiteral("7f1b8f4b3e2148c5a8f2b7c9d0e1f2a3");
const QString kCollections = QStringLiteral("cc11dd22ee33ff4455667788990011aa");
const QString kMusic = QStringLiteral("dd11ee22ff33aa4455667788990011bb");
const QString kAliceId = QStringLiteral("a11ce000a11ce000a11ce000a11ce000");
const QString kBobId = QStringLiteral("b0b00000b0b00000b0b00000b0b00000");
const QString kHiddenId = QStringLiteral("c0ffee00c0ffee00c0ffee00c0ffee00");
const QString kHostileName = QStringLiteral("<b>x</b>");

QJsonObject fixtureObject(const QString &name)
{
    return QJsonDocument::fromJson(fixture(name))
        .object()
        .value(QStringLiteral("Items"))
        .toArray()
        .first()
        .toObject();
}

// The fixture movie under another id, with some of its fields and of its user data replaced
QJsonObject cardItem(const QString &id, const QJsonObject &fields = {},
                     const QJsonObject &userData = {})
{
    QJsonObject item = fixtureObject(QStringLiteral("movie_item"));
    item.insert(QStringLiteral("Id"), id);
    for (auto it = fields.begin(); it != fields.end(); ++it)
        item.insert(it.key(), it.value());
    QJsonObject data = item.value(QStringLiteral("UserData")).toObject();
    for (auto it = userData.begin(); it != userData.end(); ++it)
        data.insert(it.key(), it.value());
    item.insert(QStringLiteral("UserData"), data);
    return item;
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

    // The views of the server in place of the fixture's: the given (id, name, collection type) triples
    void serveViews(const QList<std::array<QString, 3>> &views)
    {
        QJsonArray items;
        for (const auto &view : views)
            items.append(QJsonObject{{QStringLiteral("Id"), view[0]},
                                     {QStringLiteral("Name"), view[1]},
                                     {QStringLiteral("CollectionType"), view[2]}});
        server.viewsRawBody = envelope(items);
    }

    // Two movies libraries (the second one named `second`) and the series one
    void serveTwoMovieLibraries(const QString &second = QStringLiteral("Films"))
    {
        serveViews({{kMovies, QStringLiteral("Movies"), QStringLiteral("movies")},
                    {kMovies2, second, QStringLiteral("movies")},
                    {kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
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

    // The user table of the fake server: Alice with an avatar and a second user (Bob unless named otherwise, without an avatar unless given a tag)
    void serveUsers(const QString &secondName = QStringLiteral("Bob"),
                    const QString &secondTag = {})
    {
        server.users = {
            {kAliceId, "token-alice", QStringLiteral("Alice"), QStringLiteral("aa11bb22"), false},
            {kBobId, "token-bob", secondName, secondTag, false}};
    }

    // Waits for the state, then for the page of the state to show
    bool reach(ConnectionController::State state, const QString &page)
    {
        return QTest::qWaitFor([this, state] { return controller.state() == state; }, 10000)
               && QTest::qWaitFor([this, &page] { return item(page) != nullptr; }, 10000);
    }

    // Signs in as the user of the table, from the sign-in page
    bool signInAs(const QString &name)
    {
        controller.login(name, QStringLiteral("fixture-password"));
        return reach(ConnectionController::Authenticated, QStringLiteral("homePage"));
    }

    // Alice signs in, Bob is added through Add User, and Switch User opens the picker with both. The window must already be loaded
    bool openPickerWithTwoUsers(const QString &secondName = QStringLiteral("Bob"),
                                const QString &secondTag = {})
    {
        serveUsers(secondName, secondTag);
        controller.connectTo(address(server));
        if (!reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage"))
            || !signInAs(QStringLiteral("Alice")))
            return false;
        // The record of a user is written once the wallet has taken the token: Switch User opens the picker only for saved users
        if (!QTest::qWaitFor([this] { return savedUsers().size() == 1; }, 5000))
            return false;
        controller.switchUser();
        if (!reach(ConnectionController::ChoosingUser, QStringLiteral("userPickerPage")))
            return false;
        controller.addUser();
        if (!reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage"))
            || !signInAs(secondName))
            return false;
        if (!QTest::qWaitFor([this] { return savedUsers().size() == 2; }, 5000))
            return false;
        controller.switchUser();
        return reach(ConnectionController::ChoosingUser, QStringLiteral("userPickerPage"))
               && QTest::qWaitFor(
                   [this] {
                       const QQuickItem *grid = item(QStringLiteral("userPickerGrid"));
                       return grid && grid->property("count").toInt() == 3;
                   },
                   5000);
    }

    // The tile of a saved user or the Add User tile of the picker
    QQuickItem *userTile(const QString &userId) const
    {
        return item(userId.isEmpty() ? QStringLiteral("userTileAdd")
                                     : QStringLiteral("userTile_") + userId);
    }

    // Moves the current tile of the picker grid to the one of this user (empty: Add User)
    bool selectTile(const QString &userId)
    {
        QQuickItem *grid = item(QStringLiteral("userPickerGrid"));
        const QQuickItem *tile = userTile(userId);
        if (!grid || !tile)
            return false;
        grid->setProperty("currentIndex", tile->property("index"));
        return QTest::qWaitFor(
            [&] { return grid->property("currentItem").value<QObject *>() == tile; }, 3000);
    }

    QVariantList savedUsers() const { return controller.property("users").toList(); }

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
    // True when the card has this overlay and shows it
    static bool cardShows(QQuickItem *card, const QString &overlay)
    {
        const QQuickItem *chip = card ? itemIn(card, overlay) : nullptr;
        return chip && chip->isVisible();
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
    // The labels (anything with a `text` but a button) that show `needle` in a text format other than plain text, as descriptions; `found` counts the labels that show it at all
    QStringList notPlainText(const QString &needle, int *found = nullptr) const
    {
        QStringList problems;
        int count = 0;
        for (QQuickItem *candidate : itemsWithText(needle, false)) {
            if (candidate->inherits("QQuickAbstractButton"))
                continue;
            ++count;
            if (!candidate->property("textFormat").isValid()
                || candidate->property("textFormat").toInt() != int(Qt::PlainText))
                problems << describe(candidate);
        }
        if (found)
            *found = count;
        return problems;
    }
    // Clicks the visible button with exactly this text; false when there is none
    bool clickButton(const QString &text)
    {
        for (QQuickItem *candidate : itemsWithText(text)) {
            if (candidate->isVisible() && candidate->isEnabled() && candidate->width() > 0
                && candidate->metaObject()->indexOfMethod("click()") >= 0)
                return QMetaObject::invokeMethod(candidate, "click");
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

    // The visible tab buttons of the bar, left to right: a Kirigami private class, matched by its class name
    QList<QQuickItem *> tabButtons() const
    {
        QList<QQuickItem *> found;
        for (QQuickItem *candidate : allItems(shellHeader())) {
            if (candidate->isVisible()
                && QString::fromLatin1(candidate->metaObject()->className())
                       .contains(QLatin1String("NavigationTabButton")))
                found.append(candidate);
        }
        std::sort(found.begin(), found.end(), [](const QQuickItem *a, const QQuickItem *b) {
            return a->mapToScene(QPointF(0, 0)).x() < b->mapToScene(QPointF(0, 0)).x();
        });
        return found;
    }

    // The labels of the visible tabs, left to right
    QStringList tabNames() const
    {
        QStringList names;
        for (const QQuickItem *tab : tabButtons())
            names << tab->property("text").toString();
        return names;
    }

    // Clicks the tab with this label
    bool clickTab(const QString &name)
    {
        // The tabs slide while one appears or goes: the click waits until the bar holds still
        QList<qreal> before;
        const auto positions = [this] {
            QList<qreal> xs;
            for (const QQuickItem *tab : tabButtons())
                xs << tab->mapToScene(QPointF(0, 0)).x();
            return xs;
        };
        QTest::qWaitFor(
            [&] {
                const QList<qreal> now = positions();
                const bool still = now == before;
                before = now;
                QTest::qWait(60);
                return still;
            },
            3000);
        for (QQuickItem *tab : tabButtons()) {
            if (tab->property("text").toString() == name) {
                clickCenter(tab);
                return true;
            }
        }
        return false;
    }

    // The tab the first page of the stack stands for
    QString rootTab() const { return window->property("rootTab").toString(); }

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
        const QList<QQuickItem *> tabs = tabButtons();
        const int wantedDisplay = wide ? 2 /*TextBesideIcon*/ : 0 /*IconOnly*/;
        if (tabs.isEmpty())
            problems << QStringLiteral("the tabs are not there");
        for (const QQuickItem *tab : tabs) {
            if (tab->property("display").toInt() != wantedDisplay)
                problems << QStringLiteral("the display of tab '%1' is %2, the mode wants %3")
                                .arg(tab->property("text").toString())
                                .arg(tab->property("display").toInt())
                                .arg(wantedDisplay);
        }
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

const QString kSignedInUserId = QStringLiteral("d5ed5d4e2e5447b58e9f2b6f7bd5c96a");
const QString kMusicLibrary = QStringLiteral("2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f");

// The configuration of the signed-in user the preferences tests start from: every field the page edits, two the page must carry untouched, and ids of a library the user cannot see
QJsonObject preferencesConfiguration()
{
    return QJsonObject{{QStringLiteral("AudioLanguagePreference"), QStringLiteral("ita")},
                       {QStringLiteral("SubtitleLanguagePreference"), QStringLiteral("eng")},
                       {QStringLiteral("SubtitleMode"), QStringLiteral("Smart")},
                       {QStringLiteral("PlayDefaultAudioTrack"), true},
                       {QStringLiteral("RememberAudioSelections"), true},
                       {QStringLiteral("RememberSubtitleSelections"), false},
                       {QStringLiteral("EnableNextEpisodeAutoPlay"), true},
                       {QStringLiteral("DisplayMissingEpisodes"), false},
                       {QStringLiteral("HidePlayedInLatest"), true},
                       {QStringLiteral("GroupedFolders"), QJsonArray{kMovies}},
                       {QStringLiteral("FutureField"), QJsonObject{{QStringLiteral("Flag"), true}}},
                       {QStringLiteral("OrderedViews"), QJsonArray{kHiddenId, kMovies, kSeries}},
                       {QStringLiteral("MyMediaExcludes"), QJsonArray{}},
                       {QStringLiteral("LatestItemsExcludes"), QJsonArray{kHiddenId}}};
}

QByteArray preferencesUser(const QJsonObject &configuration)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("Id"), kSignedInUserId},
                                     {QStringLiteral("Name"), QStringLiteral("bepi")},
                                     {QStringLiteral("Policy"),
                                      QJsonObject{{QStringLiteral("IsAdministrator"), false}}},
                                     {QStringLiteral("Configuration"), configuration}})
        .toJson(QJsonDocument::Compact);
}

// The server side of the preferences tests: GET /Users/Me serves `configuration`, every POST of it is kept and replaces it (unless `stores` is off), and a test changes it to play another device
struct RemotePreferences {
    explicit RemotePreferences(Ui &ui, const QJsonObject &initial = preferencesConfiguration())
        : ui(ui), configuration(initial)
    {
        ui.server.onUsersMe = [this] {
            this->ui.server.usersMeRawBody = preferencesUser(configuration);
        };
        ui.server.onUserConfigurationPost = [this](const QByteArray &body) {
            posts.append(QJsonDocument::fromJson(body).object());
            if (stores)
                configuration = posts.constLast();
        };
    }
    ~RemotePreferences()
    {
        ui.server.onUsersMe = nullptr;
        ui.server.onUserConfigurationPost = nullptr;
    }

    Ui &ui;
    QJsonObject configuration;
    QList<QJsonObject> posts;
    bool stores = true;
};

// Opens the page from the account menu action and waits until the configuration is loaded; null when it did not come up
QQuickItem *openPreferences(Ui &ui)
{
    if (!QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("userPreferencesAction")),
                                   "trigger"))
        return nullptr;
    if (!QTest::qWaitFor([&] { return ui.item(QStringLiteral("userPreferencesPage")) != nullptr; },
                         10000))
        return nullptr;
    // The page asks for a load when it is created; one the sign-in started for Home may still be under way, and the page then waits for that one, so no new load is to be expected here
    return QTest::qWaitFor(
               [&] {
                   const auto *prefs = ui.controller.userConfiguration();
                   return prefs->state() == Kinefin::UserConfigurationModel::Ready
                          && prefs->loaded();
               },
               10000)
               ? ui.item(QStringLiteral("userPreferencesPage"))
               : nullptr;
}

QQuickItem *comboBoxIn(QQuickItem *root)
{
    for (QQuickItem *candidate : allItems(root)) {
        if (candidate->inherits("QQuickComboBox"))
            return candidate;
    }
    return nullptr;
}

// Clicks the button with this text inside a dialog: the page footer has a Discard button of its own
bool clickInDialog(const Ui &ui, const QString &dialogName, const QString &text)
{
    QObject *dialog = ui.popup(dialogName);
    const auto *content = dialog ? dialog->property("contentItem").value<QQuickItem *>() : nullptr;
    for (QQuickItem *candidate : allItems(content ? content->parentItem() : nullptr)) {
        if (candidate->property("text").toString() == text && candidate->isVisible()
            && candidate->isEnabled() && candidate->metaObject()->indexOfMethod("click()") >= 0)
            return QMetaObject::invokeMethod(candidate, "click");
    }
    return false;
}

// The favorites each library answers with, by the parentId the scan asks for, and the libraries that fail with a status instead
struct FavoritesData {
    QHash<QString, QJsonArray> byLibrary;
    QHash<QString, int> failing;
};

QString favoriteId(int n)
{
    return QString::number(n).rightJustified(32, QLatin1Char('0'));
}

QJsonObject favoriteJson(int n, const QString &type, const QString &name, int year,
                         const QStringList &genres = {})
{
    QJsonObject item{{QStringLiteral("Id"), favoriteId(n)},
                     {QStringLiteral("Name"), name},
                     {QStringLiteral("Type"), type},
                     {QStringLiteral("ProductionYear"), year}};
    if (!genres.isEmpty())
        item.insert(QStringLiteral("Genres"), QJsonArray::fromStringList(genres));
    return item;
}

// Answers the Items requests of the Favorites scan (filters=IsFavorite under a library of `data`) a page at a time; every other request goes to the usual fixtures. Without `data` the fixture favorites are served: three movies in the movies library, two series and an episode in the TV library. The data lives as long as the server
std::shared_ptr<FavoritesData> serveFavorites(Ui &ui, std::shared_ptr<FavoritesData> data = nullptr)
{
    if (!data) {
        data = std::make_shared<FavoritesData>();
        data->byLibrary[kMovies] =
            QJsonArray{favoriteJson(1, QStringLiteral("Movie"), QStringLiteral("Alpha"), 2020,
                                    {QStringLiteral("Action")}),
                       favoriteJson(2, QStringLiteral("Movie"), QStringLiteral("Bravo"), 2019,
                                    {QStringLiteral("Drama")}),
                       favoriteJson(3, QStringLiteral("Movie"), QStringLiteral("Charlie"), 2019)};
        data->byLibrary[kSeries] =
            QJsonArray{favoriteJson(4, QStringLiteral("Series"), QStringLiteral("Delta"), 2015,
                                    {QStringLiteral("Drama")}),
                       favoriteJson(5, QStringLiteral("Series"), QStringLiteral("Echo"), 2012,
                                    {QStringLiteral("Comedy")}),
                       favoriteJson(6, QStringLiteral("Episode"), QStringLiteral("Foxtrot"), 2015,
                                    {QStringLiteral("Drama")})};
    }
    ui.server.itemsResponder = [data](const QByteArray &route) -> std::pair<int, QByteArray> {
        const QUrlQuery query = FakeJellyfin::capturedListingQuery(route);
        const QString parent = query.queryItemValue(QStringLiteral("parentId"));
        if (query.queryItemValue(QStringLiteral("filters")) != QLatin1String("IsFavorite")
            || (!data->byLibrary.contains(parent) && !data->failing.contains(parent)))
            return {};
        if (data->failing.contains(parent))
            return {data->failing.value(parent), {}};
        const QJsonArray all = data->byLibrary.value(parent);
        const int start = query.queryItemValue(QStringLiteral("startIndex")).toInt();
        const int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        QJsonArray page;
        for (int i = start; i < start + limit && i < all.size(); ++i)
            page.append(all.at(i));
        return {200,
                QJsonDocument(QJsonObject{{QStringLiteral("Items"), page},
                                          {QStringLiteral("StartIndex"), start},
                                          {QStringLiteral("TotalRecordCount"), int(all.size())}})
                    .toJson(QJsonDocument::Compact)};
    };
    return data;
}

// Opens the Favorites page the way the star does and waits until the scan ended; null when the page did not come up. The libraries come first: the scan reads the ones the server listed
QQuickItem *openFavorites(Ui &ui)
{
    if (!QTest::qWaitFor([&] { return !ui.controller.libraries().isEmpty(); }, 10000))
        return nullptr;
    if (!QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openFavorites"))
        return nullptr;
    if (!QTest::qWaitFor([&] { return ui.item(QStringLiteral("favoritesPage")) != nullptr; },
                         10000))
        return nullptr;
    return QTest::qWaitFor(
               [&] {
                   const auto status = ui.controller.favorites()->status();
                   return status != Kinefin::FavoritesModel::Loading
                          && status != Kinefin::FavoritesModel::Idle;
               },
               10000)
               ? ui.item(QStringLiteral("favoritesPage"))
               : nullptr;
}

QString favoriteGroupTitle(const Ui &ui, int row)
{
    const QQuickItem *title = ui.item(QStringLiteral("favoritesGroupTitle_%1").arg(row));
    return title ? title->property("text").toString() : QString();
}

// The names of the groups shown, in order
QStringList favoriteGroupTitles(Ui &ui)
{
    QStringList titles;
    for (int row = 0; row < ui.controller.favoriteGroups()->count(); ++row)
        titles << favoriteGroupTitle(ui, row);
    return titles;
}

// An action of the page's Group By menu: "Type", "Library", "Genre", "Year" or "None"
QObject *groupByAction(QQuickItem *page, const QString &grouping)
{
    return page ? page->findChild<QObject *>(QStringLiteral("favoritesGroupBy_") + grouping)
                : nullptr;
}

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
    if (state == QLatin1String("resume"))
        ui.server.generatedResumeTotal = 30;
    if (state == QLatin1String("moviesTwoLibraries") && ui.server.viewsRawBody.isEmpty())
        ui.serveTwoMovieLibraries();
    if (state == QLatin1String("picker"))
        return ui.openPickerWithTwoUsers();
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
    if (state == QLatin1String("preferences"))
        ui.server.usersMeRawBody = preferencesUser(preferencesConfiguration());
    if (state == QLatin1String("favorites"))
        serveFavorites(ui);
    if (!ui.signIn())
        return false;
    if (state == QLatin1String("preferences"))
        return openPreferences(ui) != nullptr;
    if (state == QLatin1String("favorites"))
        return openFavorites(ui) != nullptr && ui.controller.favorites()->count() == 6;
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
    if (state == QLatin1String("resume")) {
        if (!QTest::qWaitFor([&] { return ui.controller.homeRows()->rowCount() > 0; }, 10000))
            return false;
        QMetaObject::invokeMethod(root, "openResumeItems");
        return QTest::qWaitFor(
            [&] {
                const QQuickItem *grid = ui.item(QStringLiteral("resumeItemsGrid"));
                return grid && grid->property("count").toInt() > 0
                       && ui.controller.resumeItems()->status() == Kinefin::ResumeItemsModel::Ready;
            },
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
    if (state == QLatin1String("tvTab")) {
        if (!QTest::qWaitFor(
                [&] { return ui.item(QStringLiteral("libraryCard_") + kSeries) != nullptr; }, 10000)
            || !ui.openLibraryTile(kSeries))
            return false;
        return QTest::qWaitFor(
            [&] {
                return ui.rootTab() == QLatin1String("tvshows")
                       && ui.controller.shows()->status() != Kinefin::LibraryItemsModel::Loading;
            },
            10000);
    }
    if (!QTest::qWaitFor(
            [&] { return ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr; }, 10000)
        || !ui.openLibraryTile(kMovies))
        return false;
    if (state == QLatin1String("moviesTwoLibraries"))
        return QTest::qWaitFor(
            [&] {
                return ui.controller.movies()->status() != Kinefin::LibraryItemsModel::Loading
                       && ui.item(QStringLiteral("librarySelector"))
                       && ui.item(QStringLiteral("librarySelector"))->isVisible();
            },
            10000);
    if (state == QLatin1String("browse")) {
        // The Favorites and Genres sub-tabs on, the list view, a status and a feature filter and a letter: every row of the page showing
        ui.settings.setValue(QStringLiteral("library"), QStringLiteral("tabs.movies.genres"), true);
        auto *movies = ui.controller.movies();
        movies->setViewMode(QStringLiteral("list"));
        movies->setFilterValues(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")},
                        {QStringLiteral("features"), QStringList{QStringLiteral("Subtitles")}}});
        movies->setAlphabet(QStringLiteral("A"));
        return QTest::qWaitFor(
            [&] {
                return movies->status() != Kinefin::LibraryItemsModel::Loading
                       && ui.item(QStringLiteral("subtab_genres")) != nullptr
                       && ui.item(QStringLiteral("filterChip_1")) != nullptr
                       && ui.item(QStringLiteral("libraryList")) != nullptr;
            },
            10000);
    }
    if (state == QLatin1String("library") || state == QLatin1String("libraryFailed"))
        return QTest::qWaitFor(
            [&] { return ui.controller.movies()->status() != Kinefin::LibraryItemsModel::Loading; },
            10000);
    if (state == QLatin1String("rails")) {
        if (!QTest::qWaitFor([&] { return ui.controller.movies()->genreOptions()->available(); },
                             10000))
            return false;
        QMetaObject::invokeMethod(
            root, "openGenres",
            Q_ARG(QVariant, QVariant::fromValue(ui.item(QStringLiteral("libraryPage")))));
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

namespace {
// The Kirigami action or menu entry of the library page with this object name, triggered the way a click does
bool triggerAction(const Ui &ui, const QString &name)
{
    QObject *action = ui.popup(name);
    return action && QMetaObject::invokeMethod(action, "trigger");
}

// The headings of the filter dialog's list, in order
QStringList filterSections(const Ui &ui)
{
    QStringList titles;
    QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
    if (!dialog)
        return titles;
    for (const QVariant &entry : dialog->property("entries").toList()) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("kind")).toString() == QLatin1String("heading"))
            titles << row.value(QStringLiteral("title")).toString();
    }
    return titles;
}

// Opens the filter dialog once it is fully open: its footer buttons only answer then. A dialog still closing from the last Apply finishes first, so that it opens again from what the model has
bool openFilterDialog(const Ui &ui)
{
    QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
    if (!dialog
        || !QTest::qWaitFor([dialog] { return !dialog->property("visible").toBool(); }, 5000))
        return false;
    return QMetaObject::invokeMethod(dialog, "open")
           && QTest::qWaitFor([dialog] { return dialog->property("opened").toBool(); }, 5000);
}

// Flips one option of the open filter dialog (the options below the first rows of its list have no item until scrolled to)
bool toggleOption(const Ui &ui, const QString &facet, const QVariant &value)
{
    QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
    QVariantMap entry;
    entry.insert(QStringLiteral("facet"), facet);
    entry.insert(QStringLiteral("value"), value);
    return dialog && QMetaObject::invokeMethod(dialog, "toggle", Q_ARG(QVariant, entry));
}

// Scrolls the list of the open filter dialog towards one entry (its facet and value, or the state row of a failed source when `facet` is empty): a row has no item before it is near the view, so callers look again until the item is there
bool revealFilterEntry(const Ui &ui, const QString &facet, const QVariant &value = {})
{
    QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
    QQuickItem *list = ui.item(QStringLiteral("filterOptionsList"));
    if (!dialog || !list)
        return false;
    const QVariantList entries = dialog->property("entries").toList();
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const QVariantMap row = entries.at(i).toMap();
        const bool match =
            facet.isEmpty() ? row.value(QStringLiteral("kind")).toString() == QLatin1String("state")
                            : row.value(QStringLiteral("facet")).toString() == facet
                                  && row.value(QStringLiteral("value")) == value;
        if (!match)
            continue;
        const qreal contentHeight = list->property("contentHeight").toReal();
        const qreal top = contentHeight * qreal(qMax(qsizetype(0), i - 2)) / qreal(entries.size());
        list->setProperty("contentY", qBound(0.0, top, qMax(0.0, contentHeight - list->height())));
        return true;
    }
    return false;
}

// The query of the most recent Items request the server saw
QUrlQuery lastItemsQuery(const Ui &ui)
{
    return FakeJellyfin::capturedListingQuery(
        ui.server.lastListingRoute.value(QStringLiteral("Items")));
}

// The visible item with this object name, once it has a size
QQuickItem *shownItem(const Ui &ui, const QString &name)
{
    QQuickItem *found = ui.item(name);
    return found && found->isVisible() && found->width() > 0 && found->height() > 0 ? found
                                                                                    : nullptr;
}

// The sub-tab bar of the library page: the ids of its buttons that exist, in the order of the bar
QStringList subtabIds(const Ui &ui)
{
    QStringList ids;
    for (const QString &id :
         {QStringLiteral("main"), QStringLiteral("suggestions"), QStringLiteral("favorites"),
          QStringLiteral("collections"), QStringLiteral("genres"), QStringLiteral("upcoming"),
          QStringLiteral("networks"), QStringLiteral("episodes")}) {
        const QQuickItem *button = ui.item(QStringLiteral("subtab_") + id);
        if (button && button->isVisible())
            ids << id;
    }
    return ids;
}

void setSubtabSetting(Ui &ui, const QString &id, bool on)
{
    ui.settings.setValue(QStringLiteral("library"), QStringLiteral("tabs.") + id, on);
}

// The toolbar of the library page builds its buttons a moment after its actions change, and a window or a page that goes meanwhile makes Kirigami warn about a delegate destroyed during incubation: waits until the items of the toolbar hold still
void settleToolbar(const Ui &ui)
{
    qsizetype last = -1;
    int still = 0;
    (void)QTest::qWaitFor(
        [&] {
            const QQuickItem *bar = ui.item(QStringLiteral("libraryToolbar"));
            const qsizetype now = bar ? allItems(const_cast<QQuickItem *>(bar)).size() : 0;
            still = now == last ? still + 1 : 0;
            last = now;
            QTest::qWait(60);
            return still >= 4;
        },
        5000);
}

// Picks a sub-tab by clicking its button. A button of a bar that was just created or shown is laid out a moment later, so a click at the spot it has at first could land beside it: the button is clicked by itself
bool selectSubtab(const Ui &ui, const QString &id)
{
    QQuickItem *button = nullptr;
    if (!QTest::qWaitFor(
            [&] { return (button = shownItem(ui, QStringLiteral("subtab_") + id)) != nullptr; },
            5000))
        return false;
    return QMetaObject::invokeMethod(button, "click");
}

// Clicks the real, visible footer button, not the QAction. Kirigami's
// customFooterActions delegates do not always attach their action through
// QQuickAbstractButton.action; the visible button's text is still the text of
// the named action. This verifies the actual clickable UI without depending on
// an ActionToolBar implementation detail.
bool clickActionButton(const Ui &ui, const QString &actionName)
{
    QObject *action = ui.popup(actionName);
    if (!action)
        return false;
    const QString label = action->property("text").toString();
    for (QQuickItem *button : allItems(ui.window->contentItem())) {
        if (!button->inherits("QQuickAbstractButton") || !button->isVisible()
            || !button->isEnabled() || button->width() <= 0 || button->height() <= 0
            || !ui.insideWindow(button) || Ui::clippedAway(button))
            continue;
        const QObject *bound = button->property("action").value<QObject *>();
        if (bound == action || (!label.isEmpty() && button->property("text").toString() == label))
            return QMetaObject::invokeMethod(button, "click");
    }
    return false;
}

// Waits until the button can be clicked, then clicks it once. Not for QTRY_VERIFY: that macro evaluates its expression a second time after it succeeded, which would click again on the page the first click closed
bool clickActionButtonWhenReady(const Ui &ui, const QString &actionName)
{
    return QTest::qWaitFor([&] { return clickActionButton(ui, actionName); }, 5000);
}

// Full diagnostic, including buttons that Kirigami creates without setting
// their QQuickAbstractButton.action (the cause of the former false failure).
QString describeButtons(const Ui &ui)
{
    QStringList parts;
    for (QQuickItem *button : allItems(ui.window->contentItem())) {
        if (!button->inherits("QQuickAbstractButton"))
            continue;
        const QObject *action = button->property("action").value<QObject *>();
        parts << QStringLiteral("%1 -> %2 visible=%3 enabled=%4 width=%5 text='%6'")
                     .arg(action ? action->objectName() : QStringLiteral("(no bound action)"),
                          Ui::describe(button))
                     .arg(button->isVisible())
                     .arg(button->isEnabled())
                     .arg(button->width())
                     .arg(button->property("text").toString());
    }
    return parts.isEmpty() ? QStringLiteral("no abstract button exists")
                           : parts.join(QStringLiteral("; "));
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

    // MyMediaExcludes hides a tile only, LatestItemsExcludes a Recently Added row only, OrderedViews orders the tiles that stay
    void homeTilesAndRowsFollowTheUserConfiguration()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(
            ui, QJsonObject{{QStringLiteral("OrderedViews"), QJsonArray{kSeries, kMovies}},
                            {QStringLiteral("MyMediaExcludes"), QJsonArray{kMovies}},
                            {QStringLiteral("LatestItemsExcludes"), QJsonArray{kSeries}}});
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // Series first, then the library the order does not name; Movies has no tile
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("libraryCard_")).size(), 2);
        QQuickItem *series = ui.item(QStringLiteral("libraryCard_") + kSeries);
        QQuickItem *music = ui.item(QStringLiteral("libraryCard_") + kMusicLibrary);
        QVERIFY(series && music);
        QVERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) == nullptr);
        const QPointF seriesAt = series->mapToScene(QPointF(0, 0));
        const QPointF musicAt = music->mapToScene(QPointF(0, 0));
        QVERIFY(seriesAt.y() < musicAt.y() - 1
                || (qAbs(seriesAt.y() - musicAt.y()) <= 1 && seriesAt.x() < musicAt.x()));
        // The rows: the tile of Series stays, its Recently Added row does not; Continue Watching and Next Up are untouched
        QTRY_VERIFY(!ui.itemsWithText(QStringLiteral("Recently Added in Movies")).isEmpty());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QVERIFY(ui.showsText(QStringLiteral("Next Up")));
        QVERIFY(ui.itemsWithText(QStringLiteral("Recently Added in Series")).isEmpty());
        // The libraries and the Movies tab are not filtered by a presentation preference
        QCOMPARE(ui.controller.libraries().size(), 3);
        QVERIFY(ui.controller.hasType(QStringLiteral("movies")));
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openTab",
                                          Q_ARG(QVariant, QStringLiteral("movies")),
                                          Q_ARG(QVariant, kMovies)));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        // The hidden library is the only movies library: its scope is all of them
        QTRY_VERIFY(ui.controller.movies()->isOpen());
        QCOMPARE(ui.controller.movieLibraries().size(), 1);
        QCOMPARE(ui.controller.movieLibraries().constFirst().toMap().value(QStringLiteral("id")),
                 QVariant(kMovies));
        QVERIFY(ui.controller.movies()->libraryId().isEmpty());
    }

    // The save of the preferences page reaches Home at once: no restart and no refresh
    void aVerifiedSaveFromThePreferencesPageUpdatesHome()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui, QJsonObject());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("libraryCard_")).size(), 3);
        QTRY_VERIFY(!ui.itemsWithText(QStringLiteral("Recently Added in Series")).isEmpty());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        prefs->setMyMediaExcluded(kMovies, true);
        prefs->setLatestExcluded(kSeries, true);
        QTRY_VERIFY(ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("prefsSaveButton")), "click"));
        QTRY_COMPARE(remote.posts.size(), 1);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) == nullptr);
        QCOMPARE(ui.itemsStartingWith(QStringLiteral("libraryCard_")).size(), 2);
        QTRY_VERIFY(ui.itemsWithText(QStringLiteral("Recently Added in Series")).isEmpty());
        QVERIFY(!ui.itemsWithText(QStringLiteral("Recently Added in Movies")).isEmpty());
        QCOMPARE(ui.controller.libraries().size(), 3);
        QVERIFY(ui.controller.hasType(QStringLiteral("movies")));
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
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QCOMPARE(ui.controller.movies()->rowCount(), 450);
        QTRY_COMPARE(grid->property("count").toInt(), 450);
        // Scrolling to the end asks for the pages there and nothing else
        QVERIFY(QMetaObject::invokeMethod(grid, "positionViewAtEnd"));
        QTRY_VERIFY(
            ui.controller.movies()
                ->data(ui.controller.movies()->index(449), Kinefin::LibraryItemsModel::LoadedRole)
                .toBool());
        QVERIFY(ui.controller.movies()->residentPages().size()
                <= Kinefin::LibraryItemsModel::MaxResidentPages);
        // A sort change from the page's own action list restarts the model
        ui.controller.movies()->setSortKey(Kinefin::LibraryItemsModel::Runtime);
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
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
        ui.controller.movies()->setFilter(Kinefin::LibraryItemsModel::Unplayed);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not load this library"), false));
        QVERIFY(!ui.showsText(QStringLiteral("No items")));
        ui.server.listingStatus.clear();
        ui.controller.movies()->retry();
        QTRY_VERIFY(!ui.showsText(QStringLiteral("Could not load this library"), false));
        QTRY_COMPARE(ui.controller.movies()->rowCount(), 30);
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
        auto *library = ui.controller.movies();
        QTRY_VERIFY(library->genreOptions()->available());
        // The footer buttons answer once the dialog is fully open
        QVERIFY(openFilterDialog(ui));
        QQuickItem *option = nullptr;
        QTRY_VERIFY((revealFilterEntry(ui, QStringLiteral("genreIds"), kSciFi),
                     (option = shownItem(ui, QStringLiteral("filterOption_genreIds_") + kSciFi))
                         != nullptr));
        QVERIFY(ui.showsText(QStringLiteral("Science Fiction")));
        ui.clickCenter(option);
        // The choice is a draft until Apply
        QTRY_COMPARE(option->property("selected").toBool(), true);
        QVERIFY(library->genreIds().isEmpty());
        QVERIFY(triggerAction(ui, QStringLiteral("filterApplyAction")));
        QTRY_COMPARE(library->genreIds(), QStringList({kSciFi}));
        QTRY_COMPARE(library->rowCount(), 2);
        // The dialog opens on what the model has: the row clicked again and Apply take the genre off
        QVERIFY(openFilterDialog(ui));
        QTRY_VERIFY((revealFilterEntry(ui, QStringLiteral("genreIds"), kSciFi),
                     (option = shownItem(ui, QStringLiteral("filterOption_genreIds_") + kSciFi))
                         != nullptr));
        QTRY_COMPARE(option->property("selected").toBool(), true);
        ui.clickCenter(option);
        QTRY_COMPARE(option->property("selected").toBool(), false);
        QVERIFY(triggerAction(ui, QStringLiteral("filterApplyAction")));
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
        auto *library = ui.controller.movies();
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
        QTRY_VERIFY(ui.controller.movies()->genreOptions()->available());
        QVERIFY(QMetaObject::invokeMethod(ui.popup(QStringLiteral("filterDialog")), "open"));
        QTRY_VERIFY((revealFilterEntry(ui, QStringLiteral("genreIds"), kSciFi),
                     !ui.itemsWithText(hostile).isEmpty()));
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
        auto *library = ui.controller.movies();
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
        QTRY_VERIFY(ui.controller.movies()->genreOptions()->available());
        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
        QVERIFY(QMetaObject::invokeMethod(
            ui.engine.rootObjects().constFirst(), "openGenres",
            Q_ARG(QVariant, QVariant::fromValue(ui.item(QStringLiteral("libraryPage"))))));
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
        QVERIFY(ui.controller.openGenreRails(ui.controller.movies()));
        QVERIFY(QMetaObject::invokeMethod(
            ui.engine.rootObjects().constFirst(), "openGenres",
            Q_ARG(QVariant, QVariant::fromValue(ui.item(QStringLiteral("libraryPage"))))));
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

    void theFilterDialogOffersTheSectionsOfTheScope()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(movies->filterOptions()->available());
        QTRY_COMPARE(movies->filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Ready);
        QTRY_COMPARE(filterSections(ui),
                     (QStringList{QStringLiteral("Status"), QStringLiteral("Features"),
                                  QStringLiteral("Video Quality"), QStringLiteral("Video Type"),
                                  QStringLiteral("Genres"), QStringLiteral("Tags"),
                                  QStringLiteral("Ratings"), QStringLiteral("Years")}));
    }

    void aSeriesLibraryOffersItsStatusAndNoVideoQuality()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("tvTab")));
        auto *shows = ui.controller.shows();
        QTRY_VERIFY(shows->filterOptions()->available());
        QTRY_COMPARE(shows->filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Ready);
        QTRY_COMPARE(filterSections(ui),
                     (QStringList{QStringLiteral("Status"), QStringLiteral("Features"),
                                  QStringLiteral("Series Status"), QStringLiteral("Genres"),
                                  QStringLiteral("Tags"), QStringLiteral("Ratings"),
                                  QStringLiteral("Years")}));
    }

    void ratingsAndYearsAreHiddenWithoutTheLegacyEndpoint()
    {
        Ui ui(m_home->path());
        ui.server.listingStatus.insert(QStringLiteral("FiltersLegacy"), 404);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_COMPARE(movies->filterOptions()->legacyStatus(),
                     Kinefin::FilterOptionsModel::Unavailable);
        QTRY_VERIFY(movies->filterOptions()->available());
        QTRY_VERIFY(filterSections(ui).contains(QStringLiteral("Genres")));
        QVERIFY(!filterSections(ui).contains(QStringLiteral("Ratings")));
        QVERIFY(!filterSections(ui).contains(QStringLiteral("Years")));
        QVERIFY(!filterSections(ui).contains(QStringLiteral("Ratings and Years")));
    }

    void aFilterChosenInTheDialogReachesTheServer()
    {
        Ui ui(m_home->path());
        ui.server.itemsByGenre.insert(kSciFi, QStringLiteral("items_genre_scifi"));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(movies->filterOptions()->available());
        QVERIFY(openFilterDialog(ui));
        QQuickItem *unplayed = nullptr;
        QQuickItem *played = nullptr;
        QTRY_VERIFY((unplayed = shownItem(ui, QStringLiteral("filterOption_played_Unplayed")))
                    != nullptr);
        QTRY_VERIFY((played = shownItem(ui, QStringLiteral("filterOption_played_Played")))
                    != nullptr);
        ui.clickCenter(unplayed);
        QTRY_VERIFY(unplayed->property("selected").toBool());
        // Played and Unplayed exclude each other
        ui.clickCenter(played);
        QTRY_VERIFY(played->property("selected").toBool());
        QVERIFY(!unplayed->property("selected").toBool());
        ui.clickCenter(unplayed);
        QTRY_VERIFY(unplayed->property("selected").toBool());
        QVERIFY(!played->property("selected").toBool());
        QVERIFY(toggleOption(ui, QStringLiteral("is4K"), true));
        QVERIFY(toggleOption(ui, QStringLiteral("features"), QStringLiteral("Subtitles")));
        QVERIFY(toggleOption(ui, QStringLiteral("genreIds"), kSciFi));
        // The choices are a draft: nothing reaches the model, or the server, before Apply
        QVERIFY(!movies->filtered());
        QVERIFY(triggerAction(ui, QStringLiteral("filterApplyAction")));
        QTRY_VERIFY(movies->filtered());
        QCOMPARE(movies->filterState().played, Kinefin::PlayedState::Unplayed);
        QVERIFY(movies->filterState().is4K);
        QVERIFY(movies->filterState().features.testFlag(Kinefin::LibraryFeature::Subtitles));
        QCOMPARE(movies->genreIds(), QStringList({kSciFi}));
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Ready);
        const QUrlQuery query = lastItemsQuery(ui);
        QVERIFY(query.queryItemValue(QStringLiteral("filters"))
                    .split(QLatin1Char(','))
                    .contains(QStringLiteral("IsUnplayed")));
        QCOMPARE(query.queryItemValue(QStringLiteral("is4K")), QStringLiteral("true"));
        QCOMPARE(query.queryItemValue(QStringLiteral("hasSubtitles")), QStringLiteral("true"));
        QCOMPARE(query.queryItemValue(QStringLiteral("genreIds")), kSciFi);
        // Clear in the dialog empties the draft, and Apply then clears the model
        QVERIFY(openFilterDialog(ui));
        QVERIFY(triggerAction(ui, QStringLiteral("filterClearAction")));
        QVERIFY(triggerAction(ui, QStringLiteral("filterApplyAction")));
        QTRY_VERIFY(!movies->filtered());
    }

    void theFooterButtonsOfTheFilterDialogApplyAndClear()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *movies = ui.controller.movies();
        QVERIFY(openFilterDialog(ui));
        QVERIFY(toggleOption(ui, QStringLiteral("played"), QStringLiteral("Unplayed")));
        // The buttons as the user meets them; the message says what is wrong with them when there is none to click
        QVERIFY2(clickActionButtonWhenReady(ui, QStringLiteral("filterApplyAction")),
                 qPrintable(describeButtons(ui)));
        QTRY_COMPARE(movies->filterState().played, Kinefin::PlayedState::Unplayed);
        QVERIFY(openFilterDialog(ui));
        QVERIFY2(clickActionButtonWhenReady(ui, QStringLiteral("filterClearAction")),
                 qPrintable(describeButtons(ui)));
        QVERIFY2(clickActionButtonWhenReady(ui, QStringLiteral("filterApplyAction")),
                 qPrintable(describeButtons(ui)));
        QTRY_VERIFY(!movies->filtered());
    }

    void chipsRemoveOneFilterAndClearFiltersLeavesTheRest()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *movies = ui.controller.movies();
        movies->setSortBy(QStringLiteral("Runtime"));
        movies->setFilterValues(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")},
                        {QStringLiteral("favorites"), true},
                        {QStringLiteral("features"), QStringList{QStringLiteral("Subtitles")}}});
        movies->setAlphabet(QStringLiteral("B"));
        QTRY_VERIFY(shownItem(ui, QStringLiteral("filterChip_2")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("filterChip_3")));
        // One chip, one remove button: Favorites goes, the other filters stay
        QQuickItem *remove = nullptr;
        QTRY_VERIFY((remove = shownItem(ui, QStringLiteral("filterChipRemove_1"))) != nullptr);
        // The chips are laid out a moment after they exist: a click at the spot of a button that has not moved yet would land on another one (Clear Filters, say), so the button is clicked by itself
        QVERIFY(QMetaObject::invokeMethod(remove, "click"));
        QTRY_VERIFY(!movies->filterState().favorites);
        QCOMPARE(movies->filterState().played, Kinefin::PlayedState::Unplayed);
        QVERIFY(movies->filterState().features.testFlag(Kinefin::LibraryFeature::Subtitles));
        QTRY_VERIFY(!ui.item(QStringLiteral("filterChip_2")));
        // Clear Filters takes the filters and nothing else: not the sort, the letter or the library
        QQuickItem *clear = nullptr;
        QTRY_VERIFY((clear = shownItem(ui, QStringLiteral("clearFiltersButton"))) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(clear, "click"));
        QTRY_VERIFY(movies->filterState().isEmpty());
        QCOMPARE(movies->sortBy(), QStringLiteral("Runtime"));
        QCOMPARE(movies->alphabet(), QStringLiteral("B"));
        QTRY_VERIFY(!ui.item(QStringLiteral("filterChip_0")));
    }

    void theSortMenuOffersTheKeysOfTheScopeAndRandomOffersReshuffle()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QStringList offered;
        for (const QString &token :
             {QStringLiteral("SortName"), QStringLiteral("DateCreated"),
              QStringLiteral("DateLastContentAdded"), QStringLiteral("PremiereDate"),
              QStringLiteral("ProductionYear"), QStringLiteral("AiredEpisodeOrder"),
              QStringLiteral("CommunityRating"), QStringLiteral("CriticRating"),
              QStringLiteral("Runtime"), QStringLiteral("DatePlayed"), QStringLiteral("PlayCount"),
              QStringLiteral("Random")}) {
            QObject *action = ui.popup(QStringLiteral("sortKey_") + token);
            QVERIFY2(action, qPrintable(token));
            if (action->property("visible").toBool())
                offered << token;
        }
        QStringList keys = movies->sortKeys();
        offered.sort();
        keys.sort();
        QCOMPARE(offered.size(), 10);
        QCOMPARE(offered, keys);

        movies->setFilterValues(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")}});
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Ready);
        QVERIFY(triggerAction(ui, QStringLiteral("sortKey_Runtime")));
        QCOMPARE(movies->sortBy(), QStringLiteral("Runtime"));
        QVERIFY(triggerAction(ui, QStringLiteral("sortDescending")));
        QVERIFY(movies->descending());
        QTRY_COMPARE(lastItemsQuery(ui).queryItemValue(QStringLiteral("sortOrder")),
                     QStringLiteral("Descending"));
        QVERIFY(!ui.popup(QStringLiteral("reshuffleAction"))->property("visible").toBool());

        // Random is one page of at most 100 items, with the filters kept, and Reshuffle asks for another
        QVERIFY(triggerAction(ui, QStringLiteral("sortKey_Random")));
        QVERIFY(movies->isRandom());
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Ready);
        QTRY_VERIFY(ui.popup(QStringLiteral("reshuffleAction"))->property("visible").toBool());
        QVERIFY(!ui.popup(QStringLiteral("sortAscending"))->property("enabled").toBool());
        QUrlQuery query = lastItemsQuery(ui);
        QCOMPARE(query.queryItemValue(QStringLiteral("sortBy")), QStringLiteral("Random"));
        QCOMPARE(query.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
        QCOMPARE(query.queryItemValue(QStringLiteral("limit")), QStringLiteral("100"));
        QVERIFY(
            query.queryItemValue(QStringLiteral("filters")).contains(QStringLiteral("IsUnplayed")));
        const qsizetype before = ui.server.itemsRoutes.size();
        QVERIFY(triggerAction(ui, QStringLiteral("reshuffleAction")));
        QTRY_COMPARE(ui.server.itemsRoutes.size(), before + 1);
        query = lastItemsQuery(ui);
        QCOMPARE(query.queryItemValue(QStringLiteral("sortBy")), QStringLiteral("Random"));
        QVERIFY(
            query.queryItemValue(QStringLiteral("filters")).contains(QStringLiteral("IsUnplayed")));
        // The alphabet picker is not offered for a random order
        QVERIFY(!ui.item(QStringLiteral("alphabetPicker"))->isVisible());

        QVERIFY(triggerAction(ui, QStringLiteral("sortKey_SortName")));
        QTRY_VERIFY(!ui.popup(QStringLiteral("reshuffleAction"))->property("visible").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("alphabetPicker"))->isVisible());
    }

    void theAlphabetPickerAsksTheServerForTheLetter()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *movies = ui.controller.movies();
        QQuickItem *letter = nullptr;
        QTRY_VERIFY((letter = shownItem(ui, QStringLiteral("letter_B"))) != nullptr);
        ui.clickCenter(letter);
        QTRY_COMPARE(movies->alphabet(), QStringLiteral("B"));
        QTRY_COMPARE(lastItemsQuery(ui).queryItemValue(QStringLiteral("nameStartsWith")),
                     QStringLiteral("B"));
        // The search is independent of the letter
        movies->setSearchTerm(QStringLiteral("abyss"));
        QCOMPARE(movies->alphabet(), QStringLiteral("B"));
        movies->setSearchTerm(QString());
        // # is the names before A
        QQuickItem *hash = nullptr;
        QTRY_VERIFY((hash = shownItem(ui, QStringLiteral("letter_hash"))) != nullptr);
        ui.clickCenter(hash);
        QTRY_COMPARE(movies->alphabet(), QStringLiteral("#"));
        QTRY_COMPARE(lastItemsQuery(ui).queryItemValue(QStringLiteral("nameLessThan")),
                     QStringLiteral("A"));
        QVERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("nameStartsWith")).isEmpty());
        // The same letter again takes it off
        ui.clickCenter(hash);
        QTRY_VERIFY(movies->alphabet().isEmpty());
        QTRY_VERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("nameLessThan")).isEmpty());
        QVERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("nameStartsWith")).isEmpty());
    }

    void theViewModeIsKeptPerLibraryAndTheListShowsRows()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 12;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("moviesTwoLibraries")));
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        // The scope the Home tile opened: the list mode is kept under it
        const QString first =
            movies->libraryId().isEmpty() ? QStringLiteral("*") : movies->libraryId();
        QVERIFY(triggerAction(ui, QStringLiteral("viewModeAction")));
        QCOMPARE(movies->viewMode(), QStringLiteral("list"));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryList")) != nullptr);
        QTRY_VERIFY(!ui.item(QStringLiteral("libraryGrid")));
        QTRY_VERIFY(!ui.itemsStartingWith(QStringLiteral("listRow_")).isEmpty());
        QTRY_VERIFY(ui.itemsStartingWith(QStringLiteral("mediaCard_")).isEmpty());
        // Another library has its own view, and the first one comes back as it was left
        QVERIFY(ui.controller.openType(QStringLiteral("movies"), kMovies2));
        QCOMPARE(movies->viewMode(), QStringLiteral("grid"));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        QVERIFY(ui.controller.openType(QStringLiteral("movies"), first));
        QCOMPARE(movies->viewMode(), QStringLiteral("list"));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryList")) != nullptr);
        QVERIFY(triggerAction(ui, QStringLiteral("viewModeAction")));
        QCOMPARE(movies->viewMode(), QStringLiteral("grid"));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
    }

    void theViewOptionsReachTheCards()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 6;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        // A new image type of the cards asks the server again: the cards go and come back, so a property of the first card is read when there is one
        const auto firstCard = [&ui](const char *name) {
            const QList<QQuickItem *> cards = ui.itemsStartingWith(QStringLiteral("mediaCard_"));
            return cards.isEmpty() ? QVariant() : cards.constFirst()->property(name);
        };
        QTRY_VERIFY(firstCard("posterAspect").isValid());
        QCOMPARE(firstCard("posterAspect").toReal(), 1.5);
        QVERIFY(triggerAction(ui, QStringLiteral("imageType_Banner")));
        QCOMPARE(movies->imageType(), QStringLiteral("Banner"));
        QTRY_VERIFY(firstCard("posterAspect").isValid()
                    && qAbs(firstCard("posterAspect").toReal() - 0.185) < 0.01);
        QVERIFY(triggerAction(ui, QStringLiteral("showTitleAction")));
        QVERIFY(triggerAction(ui, QStringLiteral("showYearAction")));
        QVERIFY(triggerAction(ui, QStringLiteral("cardLayoutAction")));
        QVERIFY(!movies->showTitle());
        QVERIFY(!movies->showYear());
        QVERIFY(!movies->cardLayout());
        QTRY_VERIFY(firstCard("cardLayout").isValid() && !firstCard("cardLayout").toBool());
        QTRY_VERIFY(firstCard("showTitle").isValid() && !firstCard("showTitle").toBool());
    }

    void theListOpensAnItemWithTheKeyboardAndBackKeepsTheMode()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QVERIFY(triggerAction(ui, QStringLiteral("viewModeAction")));
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("libraryList"))) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("listRow_") + kMovieId) != nullptr);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(list, "forceActiveFocus",
                                          Q_ARG(Qt::FocusReason, Qt::TabFocusReason)));
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
        QTRY_VERIFY(list->hasActiveFocus());
        QCOMPARE(ui.controller.movies()->viewMode(), QStringLiteral("list"));
    }

    void anEmptyLibraryAndAnEmptyFilteredListAreTold()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 0;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryEmpty")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("libraryFilteredEmpty"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("There are no movies yet")));
        // A filter that leaves nothing is not an empty library, and has its own way out
        movies->setFilterValues(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")}});
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryFilteredEmpty")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("libraryEmpty"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("No item matches the search or the selected filters")));
        QQuickItem *clear = nullptr;
        QTRY_VERIFY((clear = [&]() -> QQuickItem * {
                        for (QQuickItem *candidate :
                             allItems(ui.item(QStringLiteral("libraryFilteredEmpty")))) {
                            if (candidate->property("text").toString()
                                    == QStringLiteral("Clear Filters")
                                && candidate->isVisible() && candidate->width() > 0)
                                return candidate;
                        }
                        return nullptr;
                    }())
                    != nullptr);
        QVERIFY(QMetaObject::invokeMethod(clear, "click"));
        QTRY_VERIFY(!movies->filtered());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryEmpty")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("libraryFilteredEmpty"))->isVisible());
    }

    void aFailedFacetShowsRetryAndKeepsTheSelection()
    {
        Ui ui(m_home->path());
        ui.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_COMPARE(movies->filterOptions()->status(), Kinefin::FilterOptionsModel::Failed);
        QTRY_COMPARE(movies->filterOptions()->legacyStatus(), Kinefin::FilterOptionsModel::Ready);
        movies->setFilterValues(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")}});
        QVERIFY(openFilterDialog(ui));
        // The failed source has its own state with Retry, the other one has its sections
        QQuickItem *retry = nullptr;
        QTRY_VERIFY((revealFilterEntry(ui, QString()),
                     (retry = shownItem(ui, QStringLiteral("filterRetry_genres"))) != nullptr));
        QVERIFY(filterSections(ui).contains(QStringLiteral("Genres and Tags")));
        QVERIFY(filterSections(ui).contains(QStringLiteral("Ratings")));
        QVERIFY(!ui.item(QStringLiteral("filterOption_genreIds_") + kSciFi));
        ui.server.listingStatus.remove(QStringLiteral("Filters"));
        ui.clickCenter(retry);
        QTRY_COMPARE(movies->filterOptions()->status(), Kinefin::FilterOptionsModel::Ready);
        QTRY_VERIFY((revealFilterEntry(ui, QStringLiteral("genreIds"), kSciFi),
                     ui.item(QStringLiteral("filterOption_genreIds_") + kSciFi) != nullptr));
        QVERIFY(!filterSections(ui).contains(QStringLiteral("Genres and Tags")));
        // The selection was never touched
        QCOMPARE(movies->filterState().played, Kinefin::PlayedState::Unplayed);
    }

    void savedFiltersStillResolvingShowAsLoading()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *movies = ui.controller.movies();
        // The source of the tags holds its answer: the saved tag cannot be named yet
        ui.server.heldFamilies.insert(QStringLiteral("Filters"));
        Kinefin::LibraryItemsModel::ViewState saved;
        saved.pendingTagHashes = {
            Kinefin::BrowseStateStore::hashText(QStringLiteral("restored-master"))};
        QVERIFY(movies->open(QString(), QStringLiteral("movies"), QStringLiteral("Movies"), saved));
        QVERIFY(movies->resolvingFacets());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("resolvingFiltersLabel")) != nullptr);
        QVERIFY(shownItem(ui, QStringLiteral("libraryLoading")) != nullptr);
        QVERIFY(ui.showsText(QStringLiteral("Applying the saved filters…")));
        // No list is shown as if it were filtered, and Clear Filters ends the wait
        QCOMPARE(movies->rowCount(), 0);
        QQuickItem *clear = nullptr;
        QTRY_VERIFY((clear = shownItem(ui, QStringLiteral("clearFiltersButton"))) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(clear, "click"));
        QTRY_VERIFY(!movies->resolvingFacets());
        QTRY_VERIFY(!ui.item(QStringLiteral("resolvingFiltersLabel"))
                    || !ui.item(QStringLiteral("resolvingFiltersLabel"))->isVisible());
    }

    void tagsRatingsAndTheirChipsStayPlainText()
    {
        Ui ui(m_home->path());
        const QString tag = QStringLiteral("<b>Noir</b> & <img src=\"http://evil.invalid/t.png\">");
        const QString rating = QStringLiteral("<i>R</i><img src=\"http://evil.invalid/r.png\">");
        ui.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(QJsonObject{{QStringLiteral("Genres"),
                                       QJsonArray{QJsonObject{
                                           {QStringLiteral("Id"), kSciFi},
                                           {QStringLiteral("Name"), QStringLiteral("Sci-Fi")}}}},
                                      {QStringLiteral("Tags"), QJsonArray{tag}}})
                .toJson(QJsonDocument::Compact));
        ui.server.listingRawBody.insert(
            QStringLiteral("FiltersLegacy"),
            QJsonDocument(QJsonObject{{QStringLiteral("OfficialRatings"), QJsonArray{rating}},
                                      {QStringLiteral("Years"), QJsonArray{2024}}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(movies->filterOptions()->tagsAvailable());
        QTRY_VERIFY(movies->filterOptions()->ratingsAvailable());
        // The chips of the selected values
        movies->setFilterValues(
            QVariantMap{{QStringLiteral("tags"), QStringList{tag}},
                        {QStringLiteral("officialRatings"), QStringList{rating}}});
        QTRY_VERIFY(!ui.itemsWithText(tag).isEmpty());
        QTRY_VERIFY(!ui.itemsWithText(rating).isEmpty());
        // And the options of the dialog, once its list has scrolled to them
        QVERIFY(openFilterDialog(ui));
        QQuickItem *list = ui.item(QStringLiteral("filterOptionsList"));
        QVERIFY(list);
        QVERIFY(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
        QTRY_VERIFY(ui.itemsWithText(tag).size() >= 2);
        QTRY_VERIFY(ui.itemsWithText(rating).size() >= 2);
        for (const QString &text : {tag, rating}) {
            for (QQuickItem *label : ui.itemsWithText(text)) {
                QVERIFY2(label->inherits("QQuickText"), qPrintable(Ui::describe(label)));
                QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
            }
        }
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void theSubtabBarFollowsTheSettingsWhileTheAppRuns()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *subtabs = ui.controller.subtabs();
        // Favorites and Collections are on at first, and the main one is always there
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("main"), QStringLiteral("favorites"),
                                  QStringLiteral("collections")}));
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("movies.genres"), true);
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("main"), QStringLiteral("favorites"),
                                  QStringLiteral("collections"), QStringLiteral("genres")}));
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("movies.favorites"), false);
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("main"), QStringLiteral("collections"),
                                  QStringLiteral("genres")}));
        settleToolbar(ui);
        // A sub-tab turned off while it shows gives way to the main one
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_genres")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("genres")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("genres"));
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("movies.genres"), false);
        QTRY_COMPARE(subtabs->current(), QStringLiteral("main"));
        settleToolbar(ui);
        // Nothing but the main one: no bar
        setSubtabSetting(ui, QStringLiteral("movies.collections"), false);
        QTRY_VERIFY(subtabIds(ui).isEmpty());
        QVERIFY(!ui.item(QStringLiteral("subtabBar"))->isVisible());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        settleToolbar(ui);
    }

    void aHiddenSubtabAsksTheServerForNothing()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // Favorites and Collections are on and not showing: no request for them, none for the others
        for (const QByteArray &route : std::as_const(ui.server.itemsRoutes)) {
            QVERIFY2(!route.contains("BoxSet") && !route.contains("IsFavorite"), route.constData());
        }
        QVERIFY(ui.server.listingRoutes.value(QStringLiteral("MovieRecommendations")).isEmpty());
        QVERIFY(ui.server.listingRoutes.value(QStringLiteral("Genres")).isEmpty());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_collections")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("collections")));
        QTRY_COMPARE(ui.controller.subtabs()->current(), QStringLiteral("collections"));
        // The collections of the user, over every library
        QTRY_VERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("includeItemTypes"))
                    == QStringLiteral("BoxSet"));
        QVERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("parentId")).isEmpty());
        QVERIFY(ui.server.listingRoutes.value(QStringLiteral("MovieRecommendations")).isEmpty());
        // Suggestions load when they show, as cards in rows with the wording built from the kind
        setSubtabSetting(ui, QStringLiteral("movies.suggestions"), true);
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_suggestions")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("suggestions")));
        QTRY_VERIFY(
            !ui.server.listingRoutes.value(QStringLiteral("MovieRecommendations")).isEmpty());
        QTRY_VERIFY(ui.item(QStringLiteral("recommendationRow_0")) != nullptr);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Because you watched Hollow Interval")));
        // The list of Collections is gone from the page
        QTRY_VERIFY(!ui.item(QStringLiteral("libraryGrid")));
        settleToolbar(ui);
    }

    void theFavoritesSubtabIsALibraryListWithAFixedFavoritesFilter()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *subtabs = ui.controller.subtabs();
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_favorites")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("favorites")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("favorites"));
        QTRY_COMPARE(subtabs->status(), Kinefin::LibrarySubtabController::Ready);
        auto *list = subtabs->activeList();
        QVERIFY(list);
        QVERIFY(list != ui.controller.movies());
        QUrlQuery query = lastItemsQuery(ui);
        QCOMPARE(query.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie"));
        QVERIFY(
            query.queryItemValue(QStringLiteral("filters")).contains(QStringLiteral("IsFavorite")));
        // The fixed filter is neither a chip nor an option of the dialog
        QVERIFY(!ui.item(QStringLiteral("filterChip_0")));
        QVERIFY(openFilterDialog(ui));
        QTRY_VERIFY(ui.item(QStringLiteral("filterOption_played_Unplayed")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("filterOption_favorites_true")));
        QVERIFY(!filterSections(ui).isEmpty());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!ui.popup(QStringLiteral("filterDialog"))->property("visible").toBool());
        // A filter of the user joins it, and clearing the filters leaves it
        list->setFilter(Kinefin::LibraryItemsModel::Unplayed);
        QTRY_VERIFY(lastItemsQuery(ui)
                        .queryItemValue(QStringLiteral("filters"))
                        .contains(QStringLiteral("IsUnplayed")));
        QVERIFY(lastItemsQuery(ui)
                    .queryItemValue(QStringLiteral("filters"))
                    .contains(QStringLiteral("IsFavorite")));
        list->clearFilters();
        QTRY_COMPARE(lastItemsQuery(ui).queryItemValue(QStringLiteral("filters")),
                     QStringLiteral("IsFavorite"));
        // It is a list of the library page, not the global Favorites page
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
        QCOMPARE(ui.rootTab(), QStringLiteral("movies"));
    }

    void typingInTheHeaderSearchReturnsToTheMainSubtab()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *subtabs = ui.controller.subtabs();
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_favorites")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("favorites")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("favorites"));
        // The header box writes the term of the tab's own list, which is the main one
        ui.controller.movies()->setSearchTerm(QStringLiteral("abyss"));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("main"));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
    }

    void selectingANetworkOpensItsSeriesAndBackReturns()
    {
        Ui ui(m_home->path());
        const QString studioId = QStringLiteral("aa00000000000000000000000000ff11");
        const QString hostile = QStringLiteral("<u>Net</u><img src=\"http://evil.invalid/s.png\">");
        ui.server.listingRawBody.insert(
            QStringLiteral("Studios"),
            QJsonDocument(QJsonObject{{QStringLiteral("Items"),
                                       QJsonArray{QJsonObject{
                                           {QStringLiteral("Name"), hostile},
                                           {QStringLiteral("Id"), studioId},
                                           {QStringLiteral("Type"), QStringLiteral("Studio")}}}},
                                      {QStringLiteral("TotalRecordCount"), 1}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("tvTab")));
        setSubtabSetting(ui, QStringLiteral("tv.networks"), true);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_networks")) != nullptr);
        settleToolbar(ui);
        auto *subtabs = ui.controller.subtabs();
        QVERIFY(selectSubtab(ui, QStringLiteral("networks")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("networks"));
        QTRY_COMPARE(subtabs->status(), Kinefin::LibrarySubtabController::Ready);
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = shownItem(ui, QStringLiteral("mediaCard_") + studioId)) != nullptr);
        // The network's name is server text: plain text only, and the studio has no card menu
        QVERIFY(!ui.itemsWithText(hostile).isEmpty());
        for (QQuickItem *label : ui.itemsWithText(hostile)) {
            QVERIFY2(label->inherits("QQuickText"), qPrintable(Ui::describe(label)));
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }
        QVERIFY(!shownItem(ui, QStringLiteral("cardMenuButton")));
        ui.clickCenter(card);
        QTRY_COMPARE(subtabs->networkId(), studioId);
        QTRY_VERIFY(shownItem(ui, QStringLiteral("networkBackButton")) != nullptr);
        QTRY_COMPARE(lastItemsQuery(ui).queryItemValue(QStringLiteral("studioIds")), studioId);
        // Series of every library, and the label says so with the name as plain text
        QQuickItem *cross = nullptr;
        QTRY_VERIFY((cross = shownItem(ui, QStringLiteral("crossLibraryLabel"))) != nullptr);
        QCOMPARE(cross->property("textFormat").toInt(), int(Qt::PlainText));
        QVERIFY(cross->property("text").toString().contains(hostile));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        QTRY_VERIFY(!ui.item(QStringLiteral("networksGrid"))
                    || !ui.item(QStringLiteral("networksGrid"))->isVisible());
        // The way back shows the networks again
        QVERIFY(
            QMetaObject::invokeMethod(shownItem(ui, QStringLiteral("networkBackButton")), "click"));
        QTRY_VERIFY(subtabs->networkId().isEmpty());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("networksGrid")) != nullptr);
        settleToolbar(ui);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void theEpisodesSubtabShowsTheCountAndItsOwnFilters()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 40;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("tvTab")));
        setSubtabSetting(ui, QStringLiteral("tv.episodes"), true);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_episodes")) != nullptr);
        settleToolbar(ui);
        auto *subtabs = ui.controller.subtabs();
        QVERIFY(selectSubtab(ui, QStringLiteral("episodes")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("episodes"));
        // The count is the server's own, from a request of its own
        QTRY_COMPARE(subtabs->episodeCount(), 40);
        QTRY_VERIFY(shownItem(ui, QStringLiteral("episodeCountLabel")) != nullptr);
        QVERIFY(ui.showsText(QStringLiteral("40 episodes")));
        // The filters of an episode list: the kinds of episode and the video quality, no series status
        QTRY_VERIFY(filterSections(ui).contains(QStringLiteral("Episodes")));
        QVERIFY(filterSections(ui).contains(QStringLiteral("Video Quality")));
        QVERIFY(!filterSections(ui).contains(QStringLiteral("Series Status")));
        QCOMPARE(subtabs->activeList()->sortKeys().size(), 7);
        settleToolbar(ui);
    }

    void theTabOrderFollowsTheLayoutOfThePage()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 20;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("subtab_collections")) != nullptr);
        QQuickItem *menuButton = ui.item(QStringLiteral("mainMenuButton"));
        menuButton->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(menuButton->hasActiveFocus());
        const QList<QQuickItem *> visited = ui.tabCycle();
        const auto indexOfItem = [&](const QString &name) {
            const QQuickItem *target = ui.item(name);
            for (qsizetype i = 0; i < visited.size(); ++i) {
                if (visited.at(i) == target || (target && target->isAncestorOf(visited.at(i))))
                    return i;
            }
            return qsizetype(-1);
        };
        // After the tabs of the shell: the sub-tabs, the toolbar, the alphabet, the grid
        const qsizetype mainAt = indexOfItem(QStringLiteral("subtab_main"));
        const qsizetype favoritesAt = indexOfItem(QStringLiteral("subtab_favorites"));
        const qsizetype collectionsAt = indexOfItem(QStringLiteral("subtab_collections"));
        const qsizetype toolbarAt = indexOfItem(QStringLiteral("libraryToolbar"));
        const qsizetype alphabetAt = indexOfItem(QStringLiteral("alphabetPicker"));
        const qsizetype gridAt = indexOfItem(QStringLiteral("libraryGrid"));
        QVERIFY(mainAt > 2);
        QVERIFY(favoritesAt > mainAt);
        QVERIFY(collectionsAt > favoritesAt);
        QVERIFY(toolbarAt > collectionsAt);
        QVERIFY(alphabetAt > toolbarAt);
        QVERIFY(gridAt > alphabetAt);
        for (QQuickItem *item : visited) {
            QVERIFY2(item->isVisible(), qPrintable(Ui::describe(item)));
            QVERIFY2(ui.insideWindow(item), qPrintable(Ui::describe(item)));
        }
    }

    void everyControlIsInsideTheWindow_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<int>("width");
        const QStringList states{QStringLiteral("servers"),    QStringLiteral("login"),
                                 QStringLiteral("home"),       QStringLiteral("homeFailedRow"),
                                 QStringLiteral("library"),    QStringLiteral("libraryFailed"),
                                 QStringLiteral("details"),    QStringLiteral("detailsMissing"),
                                 QStringLiteral("search"),     QStringLiteral("rails"),
                                 QStringLiteral("settings"),   QStringLiteral("settingsSection"),
                                 QStringLiteral("about"),      QStringLiteral("queue"),
                                 QStringLiteral("queueEmpty"), QStringLiteral("resume"),
                                 QStringLiteral("picker")};
        for (const QString &state : states)
            QTest::addRow("%s@1000", qPrintable(state)) << state << 1000;
        for (const QString &state : {QStringLiteral("moviesTwoLibraries"), QStringLiteral("tvTab")})
            QTest::addRow("%s@1000", qPrintable(state)) << state << 1000;
        for (const QString &state :
             {QStringLiteral("home"), QStringLiteral("library"), QStringLiteral("details"),
              QStringLiteral("settings"), QStringLiteral("queue"), QStringLiteral("resume"),
              QStringLiteral("moviesTwoLibraries"), QStringLiteral("tvTab"),
              QStringLiteral("picker")}) {
            QTest::addRow("%s@560", qPrintable(state)) << state << 560;
            QTest::addRow("%s@440", qPrintable(state)) << state << 440;
            QTest::addRow("%s@1600", qPrintable(state)) << state << 1600;
        }
        for (const int width : {440, 560, 1000, 1600})
            QTest::addRow("preferences@%d", width) << QStringLiteral("preferences") << width;
        for (const int width : {560, 1000, 1600})
            QTest::addRow("browse@%d", width) << QStringLiteral("browse") << width;
        for (const int width : {560, 1000, 1600})
            QTest::addRow("favorites@%d", width) << QStringLiteral("favorites") << width;
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
        QTRY_VERIFY(ui.controller.movies()->genreOptions()->available());
        QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
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
        QCOMPARE(ui.tabNames(), QStringList({QStringLiteral("Home"), QStringLiteral("Movies"),
                                             QStringLiteral("TV Shows")}));
        // Home marks itself, and only itself, wherever the front page belongs
        const auto tabChecked = [&](int index, bool expected) {
            return ui.tabButtons().value(index)->property("checked").toBool() == expected;
        };
        const auto marked = [&](bool expected) { return tabChecked(0, expected); };
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
        // The Movies tile opens the Movies tab, which alone is marked
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryPage")) != nullptr);
        QTRY_VERIFY(marked(false));
        QTRY_VERIFY(tabChecked(1, true));
        QVERIFY(tabChecked(2, false));
        QTRY_VERIFY(!ui.item(QStringLiteral("queueButton"))->property("down").toBool());
        // A page above the tab belongs to it: Settings marks the menu button and no tab
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_VERIFY(tabChecked(1, false));
        // The TV Shows tab replaces the Movies one
        QVERIFY(ui.clickTab(QStringLiteral("TV Shows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTRY_VERIFY(tabChecked(2, true));
        QVERIFY(tabChecked(1, false));
        QVERIFY(tabChecked(0, false));
    }

    void theMouseBackButtonGoesBack()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const QPoint middle(ui.window->width() / 2, ui.window->height() / 2);
        // The first page has nowhere to go back to
        QTest::mouseClick(ui.window, Qt::BackButton, Qt::NoModifier, middle);
        QTest::qWait(100);
        QCOMPARE(ui.stackDepth(), 1);
        // A page above Home goes away with the back button, wherever the pointer is over the window
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_COMPARE(ui.stackDepth(), 2);
        QTest::mouseClick(ui.window, Qt::BackButton, Qt::NoModifier, middle);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);
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
        // The order inside the header: the three tabs, then the right cluster
        const QList<QQuickItem *> tabs = ui.tabButtons();
        QCOMPARE(tabs.size(), 3);
        for (QQuickItem *tab : tabs) {
            QTest::keyClick(ui.window, Qt::Key_Tab);
            QTRY_VERIFY(tab->hasActiveFocus());
        }
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("headerSearchField"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesButton"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("queueButton"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("accountButton"))->hasActiveFocus());
        // Behind the account button the page content begins: Back from the menu button wraps, so nothing of the header sits before it
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QQuickItem *next = ui.window->activeFocusItem();
        QVERIFY(next && !tabs.contains(next) && next != menuButton
                && next->objectName() != QStringLiteral("headerSearchField")
                && next->objectName() != QStringLiteral("favoritesButton")
                && next->objectName() != QStringLiteral("queueButton")
                && next->objectName() != QStringLiteral("accountButton"));
    }

    void homeReturnsToTheSameHomePage()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QPointer<QQuickItem> home = ui.item(QStringLiteral("homePage"));
        QVERIFY(home);
        // A page above Home gives way to the same Home page
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_COMPARE(ui.stackDepth(), 2);
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(ui.currentDetails(), home.data());
        // From a type tab Home is a page of its own, and the stack holds nothing else
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryPage")) == nullptr);
        QCOMPARE(ui.stackDepth(), 1);
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

    void accountMenuSwitchUserOpensPicker()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(!ui.store.tokens.isEmpty());
        const int logins = ui.server.successfulLogins;
        // The account menu opens from its button, and Switch User is one of its entries
        ui.clickCenter(ui.item(QStringLiteral("accountButton")));
        QObject *menu = ui.popup(QStringLiteral("accountMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("switchUserAction")),
                                          "trigger"));
        // The picker takes the place of Home, the session stays saved and the server stays chosen
        QTRY_COMPARE(ui.controller.state(), ConnectionController::ChoosingUser);
        QTRY_VERIFY(ui.item(QStringLiteral("userPickerPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) == nullptr);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QVERIFY(!ui.store.tokens.isEmpty());
        QCOMPARE(ui.controller.servers().size(), 1);
        QCOMPARE(ui.savedUsers().size(), 1);
        QQuickItem *grid = ui.item(QStringLiteral("userPickerGrid"));
        QVERIFY(grid);
        // The saved user and Add User
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        QVERIFY(ui.userTile(QString()) != nullptr);
        // The grid has the keys, and Return signs the saved user back in without a password
        QTRY_VERIFY(grid->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QCOMPARE(ui.server.successfulLogins, logins);
    }

    void pickerNamesArePlainText()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers(kHostileName));
        QQuickItem *tile = ui.userTile(kBobId);
        QVERIFY(tile);
        const QQuickItem *label = Ui::itemIn(tile, QStringLiteral("userName"));
        QVERIFY(label);
        QCOMPARE(label->property("text").toString(), kHostileName);
        int found = 0;
        const QStringList notPlain = ui.notPlainText(kHostileName, &found);
        QVERIFY(found >= 1);
        QVERIFY2(notPlain.isEmpty(), qPrintable(notPlain.join(QLatin1Char('\n'))));
    }

    void pickerLoadsTheAvatarsOfTheUsersThatHaveOne()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers());
        // Alice has an avatar tag, Bob has none and so asks for no image
        QTRY_VERIFY(ui.server.userImageRequests > 0);
        QVERIFY(ui.server.lastUserImageRoute.contains(kAliceId.toUtf8()));
        QVERIFY(!ui.server.lastUserImageRoute.contains(kBobId.toUtf8()));
        QVERIFY(ui.server.lastUserImageAuth.isEmpty());
    }

    void theAccountButtonShowsTheAvatarOfTheUser()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        ui.serveUsers();
        ui.controller.connectTo(address(ui.server));
        QVERIFY(ui.reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage")));
        QVERIFY(ui.signInAs(QStringLiteral("Alice")));
        const QQuickItem *avatar = nullptr;
        QTRY_VERIFY((avatar = ui.item(QStringLiteral("accountAvatar"))) != nullptr
                    && avatar->isVisible());
        // The text stays static (it feeds the tooltip): the name is only in the accessible name
        const QQuickItem *button = ui.item(QStringLiteral("accountButton"));
        QCOMPARE(button->property("text").toString(), QStringLiteral("Account"));
        QCOMPARE(button->property("accessibleName").toString(), QStringLiteral("Account: Alice"));
    }

    void pickerChoosesWithEnter_data()
    {
        QTest::addColumn<int>("key");
        QTest::newRow("Return") << int(Qt::Key_Return);
        QTest::newRow("Enter") << int(Qt::Key_Enter);
        QTest::newRow("Space") << int(Qt::Key_Space);
    }

    void pickerChoosesWithEnter()
    {
        QFETCH(int, key);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers());
        QQuickItem *grid = ui.item(QStringLiteral("userPickerGrid"));
        QTRY_VERIFY(grid->hasActiveFocus());
        const int logins = ui.server.successfulLogins;
        // Bob signed in last: the arrow keys reach the other user, and the key chooses it without a password
        QVERIFY(ui.selectTile(kAliceId));
        ui.server.lastUsersMeAuth.clear();
        QTest::keyClick(ui.window, Qt::Key(key));
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QCOMPARE(ui.server.successfulLogins, logins);
        QTRY_VERIFY(ui.server.lastUsersMeAuth.contains("token-alice"));
    }

    void addUserOpensAFreshSignInAndChooseAnotherUserComesBack()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers());
        QTRY_VERIFY(ui.item(QStringLiteral("userPickerGrid"))->hasActiveFocus());
        QVERIFY(ui.selectTile(QString()));
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("loginPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("userPickerPage")) == nullptr);
        QQuickItem *username = ui.item(QStringLiteral("usernameField"));
        QVERIFY(username);
        QVERIFY(username->property("text").toString().isEmpty());
        QTRY_VERIFY(username->hasActiveFocus());
        // Users are saved, so the way back to the picker is offered
        QTRY_VERIFY(ui.showsText(QStringLiteral("Choose Another User")));
        QVERIFY(ui.clickButton(QStringLiteral("Choose Another User")));
        QTRY_COMPARE(ui.controller.state(), ConnectionController::ChoosingUser);
        QTRY_VERIFY(ui.item(QStringLiteral("userPickerPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("loginPage")) == nullptr);
        QCOMPARE(ui.savedUsers().size(), 2);
    }

    void aUserThatAsksForThePasswordLocksItsTileAndPrefillsTheSignIn()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers());
        const auto locked = [&ui](const QString &id) {
            const QQuickItem *lock = Ui::itemIn(ui.userTile(id), QStringLiteral("userLock"));
            return lock && lock->isVisible();
        };
        QVERIFY(!locked(kAliceId));
        QVERIFY(!locked(kBobId));
        ui.controller.setUserAskPassword(kBobId, true);
        QTRY_VERIFY(locked(kBobId));
        QVERIFY(!locked(kAliceId));
        QTRY_VERIFY(ui.item(QStringLiteral("userPickerGrid"))->hasActiveFocus());
        QVERIFY(ui.selectTile(kBobId));
        QTest::keyClick(ui.window, Qt::Key_Return);
        // The sign-in is for that user: the name is filled in and the password has the focus
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("loginPage")) != nullptr);
        QQuickItem *username = ui.item(QStringLiteral("usernameField"));
        QVERIFY(username);
        QCOMPARE(username->property("text").toString(), QStringLiteral("Bob"));
        QTRY_VERIFY(ui.item(QStringLiteral("passwordField"))->hasActiveFocus());
        ui.type(QStringLiteral("pw"));
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
    }

    void theSignInListsTheUsersOfTheServer()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        ui.serveUsers(kHostileName);
        const FakeJellyfin::FakeUser hidden{
            kHiddenId, "token-hidden", QStringLiteral("Hidden"), {}, true};
        ui.server.users.append(hidden);
        ui.controller.connectTo(address(ui.server));
        QVERIFY(ui.reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage")));
        // The server lists Alice and the hostile name, not the hidden user
        QQuickItem *strip = ui.item(QStringLiteral("publicUsersStrip"));
        QVERIFY(strip);
        QTRY_VERIFY(strip->isVisible() && strip->property("count").toInt() == 2);
        QTRY_VERIFY(ui.item(QStringLiteral("publicUser_") + kBobId) != nullptr);
        QVERIFY(ui.item(QStringLiteral("publicUser_") + kHiddenId) == nullptr);
        int found = 0;
        const QStringList notPlain = ui.notPlainText(kHostileName, &found);
        QVERIFY(found >= 1);
        QVERIFY2(notPlain.isEmpty(), qPrintable(notPlain.join(QLatin1Char('\n'))));
        // A tile fills in the username and moves on to the password
        QQuickItem *username = ui.item(QStringLiteral("usernameField"));
        QVERIFY(username);
        QVERIFY(username->property("text").toString().isEmpty());
        QQuickItem *tile = ui.item(QStringLiteral("publicUser_") + kBobId);
        QTRY_VERIFY([&] {
            if (username->property("text").toString() != kHostileName)
                ui.clickCenter(tile);
            return username->property("text").toString() == kHostileName;
        }());
        QTRY_VERIFY(ui.item(QStringLiteral("passwordField"))->hasActiveFocus());
        ui.type(QStringLiteral("pw"));
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
    }

    void theMenuOfATileAsksForThePasswordAndRemovesTheUser()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers(kHostileName));
        QQuickItem *grid = ui.item(QStringLiteral("userPickerGrid"));
        QTRY_VERIFY(grid->hasActiveFocus());
        QObject *menu = ui.popup(QStringLiteral("userMenu"));
        QVERIFY(menu);
        const auto menuOpen = [menu] { return menu->property("opened").toBool(); };
        const auto menuClosed = [menu] { return !menu->property("visible").toBool(); };
        const auto askPassword = [&ui](const QString &id) {
            for (const QVariant &user : ui.savedUsers()) {
                if (user.toMap().value(QStringLiteral("id")).toString() == id)
                    return user.toMap().value(QStringLiteral("askPassword")).toBool();
            }
            return false;
        };

        // The button of the tile, a right click, the Menu key and Shift+F10 open the menu; Escape closes it
        QVERIFY(ui.selectTile(kBobId));
        QQuickItem *tile = ui.userTile(kBobId);
        ui.clickCenter(Ui::itemIn(tile, QStringLiteral("userMenuButton")));
        QTRY_VERIFY(menuOpen());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(menuClosed());
        const QPointF point = tile->mapToScene(QPointF(tile->width() / 2, tile->height() / 3));
        QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier, point.toPoint());
        QTRY_VERIFY(menuOpen());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(menuClosed());
        QTRY_VERIFY(grid->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menuOpen());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(menuClosed());
        QTest::keyClick(ui.window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(menuOpen());

        // Ask for Password is checkable and applies at once, and the current tile stays on the same user
        QQuickItem *ask = ui.item(QStringLiteral("userMenuAskPassword"));
        QVERIFY(ask);
        QVERIFY(ask->property("checkable").toBool());
        QVERIFY(!ask->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(ask, "click"));
        QTRY_VERIFY(menuClosed());
        QTRY_VERIFY(askPassword(kBobId));
        QVERIFY(!askPassword(kAliceId));
        QTRY_VERIFY(grid->property("currentItem").value<QObject *>() == ui.userTile(kBobId));
        QTRY_VERIFY(grid->hasActiveFocus());

        // Remove asks first, with the name as plain text, and Escape cancels
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menuOpen());
        QVERIFY(ui.item(QStringLiteral("userMenuAskPassword"))->property("checked").toBool());
        QQuickItem *remove = ui.item(QStringLiteral("userMenuRemove"));
        QVERIFY(remove);
        QVERIFY(QMetaObject::invokeMethod(remove, "click"));
        QObject *dialog = ui.popup(QStringLiteral("removeUserDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        const QString question =
            QStringLiteral("Remove %1 and the saved queue from this system? The account on "
                           "the server is not changed")
                .arg(kHostileName);
        QTRY_VERIFY(ui.showsText(question));
        for (QQuickItem *label : ui.itemsWithText(question))
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui.savedUsers().size(), 2);

        // Confirming removes the user
        QTRY_VERIFY(grid->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menuOpen());
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("userMenuRemove")), "click"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *confirm = nullptr;
        QTRY_VERIFY([&] {
            for (QQuickItem *candidate : ui.itemsWithText(QStringLiteral("Remove"))) {
                if (candidate->isVisible() && candidate->width() > 0
                    && candidate->metaObject()->indexOfMethod("click()") >= 0)
                    confirm = candidate;
            }
            return confirm != nullptr;
        }());
        QVERIFY(QMetaObject::invokeMethod(confirm, "click"));
        QTRY_COMPARE(ui.savedUsers().size(), 1);
    }

    void pickerWrapsInNarrowWindow()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers());
        const QQuickItem *grid = ui.item(QStringLiteral("userPickerGrid"));
        QVERIFY(grid);
        const int cell = int(grid->property("minCellWidth").toReal());
        const QStringList tiles{kAliceId, kBobId, QString()};
        const auto distinct = [&ui, &tiles](bool rows) {
            QSet<int> positions;
            for (const QString &id : tiles) {
                // A tile the grid has not built yet counts as nowhere, so the checks wait for it
                const QQuickItem *tile = ui.userTile(id);
                if (!tile)
                    return -1;
                const QPointF origin = tile->mapToScene(QPointF(0, 0));
                positions.insert(qRound(rows ? origin.y() : origin.x()));
            }
            return int(positions.size());
        };
        const auto tilesInside = [&ui, &tiles] {
            for (const QString &id : tiles) {
                const QQuickItem *tile = ui.userTile(id);
                if (!tile)
                    return false;
                const QRectF rect =
                    tile->mapRectToScene(QRectF(0, 0, tile->width(), tile->height()));
                if (rect.left() < -1 || rect.right() > ui.window->width() + 1)
                    return false;
            }
            return true;
        };
        const int narrow = qMin(cell * 2 - 20, ui.window->property("compactLimit").toInt() - 40);

        // Three columns hold the three tiles in one row, two wrap the last one, and one stacks them
        QVERIFY(ui.opensWindowOfWidth(cell * 3 + 120));
        QTRY_COMPARE(distinct(true), 1);
        QTRY_VERIFY(tilesInside());
        QVERIFY(ui.opensWindowOfWidth(cell * 2 + 60));
        QTRY_COMPARE(distinct(true), 2);
        QTRY_COMPARE(distinct(false), 2);
        QTRY_VERIFY(tilesInside());
        QVERIFY(ui.opensWindowOfWidth(narrow));
        QTRY_COMPARE(distinct(false), 1);
        QTRY_COMPARE(distinct(true), 3);
        QTRY_VERIFY(tilesInside());
    }

    void signOutForgetsEverythingAndReturnsToTheServers()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        ui.clickCenter(ui.item(QStringLiteral("accountButton")));
        QObject *menu = ui.popup(QStringLiteral("accountMenu"));
        QTRY_VERIFY(menu->property("opened").toBool());
        // A click on the menu item closes the menu first, then the action asks for a confirmation
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("signOutAction")), "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("signOutDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        const QString question =
            QStringLiteral("Sign out %1? The saved session and queue of this user are removed "
                           "from this system. The account on the server is not changed")
                .arg(ui.savedUsers().constFirst().toMap().value(QStringLiteral("name")).toString());
        QTRY_VERIFY(ui.showsText(question));
        for (QQuickItem *label : ui.itemsWithText(question))
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        // Escape cancels: the session stays
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QVERIFY(!ui.store.tokens.isEmpty());

        // Confirming signs out
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("signOutAction")), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(ui.showsText(question));
        QQuickItem *confirm = nullptr;
        QTRY_VERIFY([&] {
            for (QQuickItem *candidate : ui.itemsWithText(QStringLiteral("Sign Out"))) {
                if (candidate->isVisible() && candidate->width() > 0
                    && candidate->metaObject()->indexOfMethod("click()") >= 0)
                    confirm = candidate;
            }
            return confirm != nullptr;
        }());
        QVERIFY(QMetaObject::invokeMethod(confirm, "click"));
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
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
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
        ui.controller.movies()->setFilter(Kinefin::LibraryItemsModel::Unplayed);
        // The tab already in front is not opened again: its page, filter and stack stay
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openTab",
                                          Q_ARG(QVariant, QStringLiteral("movies")),
                                          Q_ARG(QVariant, kMovies)));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTest::qWait(500);
        QCOMPARE(ui.currentDetails(), library.data());
        QCOMPARE(ui.controller.movies()->filter(), Kinefin::LibraryItemsModel::Unplayed);
        QCOMPARE(ui.stackDepth(), 1);
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

    void cardChips()
    {
        Ui ui(m_home->path());
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{cardItem(kMovieId, {}, {{QStringLiteral("PlayedPercentage"), 40}}),
                                cardItem(kPlayedMovieId,
                                         {{QStringLiteral("Name"), QStringLiteral("Seen")},
                                          {QStringLiteral("CommunityRating"), QJsonValue::Null}},
                                         {{QStringLiteral("Played"), true},
                                          {QStringLiteral("PlaybackPositionTicks"), 0}}),
                                cardItem(kShowCardId,
                                         {{QStringLiteral("Name"), QStringLiteral("Shows")},
                                          {QStringLiteral("Type"), QStringLiteral("Series")},
                                          {QStringLiteral("ChildCount"), 3},
                                          {QStringLiteral("RecursiveItemCount"), 20}},
                                         {{QStringLiteral("UnplayedItemCount"), 5},
                                          {QStringLiteral("PlaybackPositionTicks"), 0}})}));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        const auto card = [page](const QString &id) {
            return Ui::itemIn(page, QStringLiteral("mediaCard_") + id);
        };
        // The movie in progress: rating, minutes left (rounded up) and the position bar
        QTRY_VERIFY(card(kMovieId) && card(kPlayedMovieId) && card(kShowCardId));
        QQuickItem *movie = card(kMovieId);
        QTRY_VERIFY(Ui::cardShows(movie, QStringLiteral("cardRating")));
        QCOMPARE(Ui::itemIn(movie, QStringLiteral("cardRating"))->property("text").toString(),
                 QLocale().toString(7.1, 'f', 1));
        QVERIFY(Ui::cardShows(movie, QStringLiteral("cardRemaining")));
        QCOMPARE(Ui::itemIn(movie, QStringLiteral("cardRemaining"))->property("text").toString(),
                 QStringLiteral("15 min left"));
        QVERIFY(Ui::cardShows(movie, QStringLiteral("cardProgress")));
        QVERIFY(!Ui::cardShows(movie, QStringLiteral("cardPlayed")));
        QVERIFY(!Ui::cardShows(movie, QStringLiteral("cardUnplayed")));
        QVERIFY(!Ui::cardShows(movie, QStringLiteral("cardNew")));
        // A played movie without a rating: the check only
        QQuickItem *played = card(kPlayedMovieId);
        QVERIFY(Ui::cardShows(played, QStringLiteral("cardPlayed")));
        QVERIFY(!Ui::cardShows(played, QStringLiteral("cardRating")));
        QVERIFY(!Ui::cardShows(played, QStringLiteral("cardRemaining")));
        QVERIFY(!Ui::cardShows(played, QStringLiteral("cardProgress")));
        // A series: the unplayed count, the counts line and the share of watched episodes
        QQuickItem *show = card(kShowCardId);
        QVERIFY(Ui::cardShows(show, QStringLiteral("cardUnplayed")));
        QVERIFY(!Ui::cardShows(show, QStringLiteral("cardPlayed")));
        QCOMPARE(show->property("secondLine").toString(),
                 QStringLiteral("3 seasons \u00b7 20 episodes"));
        QCOMPARE(show->property("progress").toReal(), 0.75);
        QVERIFY(Ui::cardShows(show, QStringLiteral("cardProgress")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
    }

    void newBadgeFollowsSetting()
    {
        Ui ui(m_home->path());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{
                cardItem(kMovieId,
                         {{QStringLiteral("DateCreated"), now.addDays(-1).toString(Qt::ISODate)}}),
                cardItem(kOldMovieId, {{QStringLiteral("DateCreated"),
                                        now.addDays(-30).toString(Qt::ISODate)}})}));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        const auto card = [page](const QString &id) {
            return Ui::itemIn(page, QStringLiteral("mediaCard_") + id);
        };
        QTRY_VERIFY(card(kMovieId) && card(kOldMovieId));
        // On by default: only the recent item has the chip
        QVERIFY(ui.window->property("newBadgeEnabled").toBool());
        QTRY_VERIFY(Ui::cardShows(card(kMovieId), QStringLiteral("cardNew")));
        QVERIFY(!Ui::cardShows(card(kOldMovieId), QStringLiteral("cardNew")));
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("newBadge"), false));
        QTRY_VERIFY(!Ui::cardShows(card(kMovieId), QStringLiteral("cardNew")));
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("newBadge"), true));
        QTRY_VERIFY(Ui::cardShows(card(kMovieId), QStringLiteral("cardNew")));
        QVERIFY(!Ui::cardShows(card(kOldMovieId), QStringLiteral("cardNew")));
    }

    void cardMenuActions()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.serveShow();
        ui.server.listingRawBody.insert(QStringLiteral("NextUp"), envelope({}));
        const QString episodeId = QStringLiteral("c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7");
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{fixtureObject(QStringLiteral("movie_item")),
                                fixtureObject(QStringLiteral("episode_item"))}));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        const auto card = [page](const QString &id) {
            return Ui::itemIn(page, QStringLiteral("mediaCard_") + id);
        };
        QTRY_VERIFY(card(kMovieId) && card(episodeId));
        const auto menuVisible = [&ui] {
            QObject *menu = ui.popup(QStringLiteral("cardMenu"));
            return menu && menu->property("visible").toBool();
        };
        const auto entryShown = [&ui](const QString &name) {
            (void)QTest::qWaitFor([&] { return ui.item(name) != nullptr; }, 5000);
            const QQuickItem *found = ui.item(name);
            return found && found->isVisible();
        };
        const auto choose = [&](const QString &name) {
            QQuickItem *found = nullptr;
            QTRY_VERIFY((found = ui.item(name)) != nullptr);
            QTRY_VERIFY(found->isEnabled());
            QVERIFY(QMetaObject::invokeMethod(found, "click"));
            QTRY_VERIFY(!menuVisible());
        };
        const auto rightClick = [&ui](QQuickItem *target) {
            const QPointF center =
                target->mapToScene(QPointF(target->width() / 2, target->height() / 3));
            QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier, center.toPoint());
        };

        // The Menu key opens the menu of the current card (the grid has the keys), Escape closes it
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menuVisible());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menuVisible());

        // A movie: no Go to Series, and Add to Queue joins the end
        rightClick(card(kMovieId));
        QTRY_VERIFY(menuVisible());
        QVERIFY(entryShown(QStringLiteral("cardMenuOpen")));
        QVERIFY(entryShown(QStringLiteral("cardMenuPlayNext")));
        QVERIFY(ui.item(QStringLiteral("cardMenuGoToSeries")));
        QVERIFY(!entryShown(QStringLiteral("cardMenuGoToSeries")));
        choose(QStringLiteral("cardMenuAddToQueue"));
        QTRY_COMPARE(ui.controller.queue()->count(), 1);
        QTRY_VERIFY(!ui.controller.queue()->busy());

        // An episode: Go to Series shows, and Play Next lands before the movie
        rightClick(card(episodeId));
        QTRY_VERIFY(menuVisible());
        QVERIFY(entryShown(QStringLiteral("cardMenuGoToSeries")));
        choose(QStringLiteral("cardMenuPlayNext"));
        QTRY_COMPARE(ui.controller.queue()->count(), 2);
        QCOMPARE(ui.controller.queue()
                     ->data(ui.controller.queue()->index(0), Kinefin::PlayQueue::ItemIdRole)
                     .toString(),
                 episodeId);
        QTRY_VERIFY(!ui.controller.queue()->busy());

        // Go to Series opens the series page
        rightClick(card(episodeId));
        QTRY_VERIFY(menuVisible());
        choose(QStringLiteral("cardMenuGoToSeries"));
        QTRY_COMPARE(ui.currentDetails()->property("itemId").toString(), kSeriesId);
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
        // One file per owner in the queues folder: the only one is the signed-in user's
        const QString queuesPath =
            m_home->filePath(QStringLiteral("data/kinefin/profiles/default/queues"));
        const auto queueFiles = [&] {
            return QDir(queuesPath).entryList({QStringLiteral("*.json")}, QDir::Files);
        };
        QTRY_COMPARE(queueFiles().size(), 1);
        QFile stored(queuesPath + QLatin1Char('/') + queueFiles().constFirst());
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
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_VERIFY(focusIsIn(QStringLiteral("settingsPage")));
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(focusIsIn(QStringLiteral("libraryPage")));
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
        // Every label that carries the markup renders it as text, never as rich text. The card's second line carries the markup too, after "S1:E2 - "
        const auto checkPlainText = [&ui, &hostile] {
            const QList<QQuickItem *> labels = ui.itemsWithText(hostile, false);
            QVERIFY(!labels.isEmpty());
            for (QQuickItem *label : labels) {
                QVERIFY(label->property("textFormat").isValid());
                // Text.PlainText is Qt::PlainText, 0: not RichText (1), AutoText (2) or Markdown (5)
                QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
            }
        };
        QTRY_VERIFY(!ui.itemsWithText(hostile).isEmpty());
        checkPlainText();
        // The Movies tab replaces Home: the grid card carries the markup now
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QTRY_VERIFY(!ui.itemsWithText(hostile).isEmpty());
        checkPlainText();
        // No image was requested for the markup's <img>: the only image routes are the server's own
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void typeTabsShowOnlyWithLibraries()
    {
        Ui ui(m_home->path());
        ui.serveViews({{kMovies, QStringLiteral("Movies"), QStringLiteral("movies")}});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_COMPARE(ui.tabNames(),
                     QStringList({QStringLiteral("Home"), QStringLiteral("Movies")}));
        // Only series: the Movies tab goes and TV Shows takes its place
        ui.serveViews({{kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     QStringList({QStringLiteral("Home"), QStringLiteral("TV Shows")}));
        // Neither: the bar holds Home alone
        ui.serveViews({{kSeries, QStringLiteral("Music"), QStringLiteral("music")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(), QStringList({QStringLiteral("Home")}));
        // A type that vanishes while its tab shows sends the user to Home
        ui.serveViews({{kMovies, QStringLiteral("Movies"), QStringLiteral("movies")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     QStringList({QStringLiteral("Home"), QStringLiteral("Movies")}));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        // The page is built before it goes: a page destroyed while its toolbar is incubating warns
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QTest::qWait(300);
        ui.serveViews({{kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryPage")) == nullptr);
        QCOMPARE(ui.stackDepth(), 1);
    }

    void aTabIsTheStackRoot()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 300;
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QCOMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
        QCOMPARE(ui.currentDetails()->property("title").toString(), QStringLiteral("Movies"));
        // One library of the type: no selector
        QQuickItem *selector = ui.item(QStringLiteral("librarySelector"));
        QVERIFY(!selector || !selector->isVisible());
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_COMPARE(grid->property("count").toInt(), 300);
        // The tile and the scroll position the user left behind
        grid->setProperty("currentIndex", 7);
        grid->setProperty("contentY", 600.0);
        QTRY_COMPARE(grid->property("contentY").toReal(), 600.0);
        QVERIFY(ui.clickTab(QStringLiteral("TV Shows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QCOMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(ui.currentDetails()->property("title").toString(), QStringLiteral("TV Shows"));
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.stackDepth(), 1);
        QQuickItem *again = nullptr;
        QTRY_VERIFY((again = ui.item(QStringLiteral("libraryGrid"))) != nullptr);
        QTRY_COMPARE(again->property("count").toInt(), 300);
        QTRY_COMPARE(again->property("currentIndex").toInt(), 7);
        QTRY_VERIFY(qAbs(again->property("contentY").toReal() - 600.0) < 2.0);
        // A page above the tab leaves it in place: Home is a page of its own
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_COMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.rootTab(), QStringLiteral("movies"));
    }

    void selectorFromTwoLibraries()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("moviesTwoLibraries")));
        QQuickItem *selector = ui.item(QStringLiteral("librarySelector"));
        QVERIFY(selector);
        // All libraries and the two names; the tile of the first library chose it
        QCOMPARE(selector->property("count").toInt(), 3);
        QCOMPARE(selector->property("currentIndex").toInt(), 1);
        QCOMPARE(selector->property("displayText").toString(), QStringLiteral("Movies"));
        QCOMPARE(ui.controller.movies()->libraryId(), kMovies);
        QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 2)));
        QTRY_COMPARE(ui.controller.movies()->libraryId(), kMovies2);
        QTRY_COMPARE(selector->property("currentIndex").toInt(), 2);
        QCOMPARE(selector->property("displayText").toString(), QStringLiteral("Films"));
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 0)));
        QTRY_COMPARE(ui.controller.movies()->libraryId(), QString());
        QTRY_COMPARE(selector->property("currentIndex").toInt(), 0);
        // The three tabs come first in the Tab order, the selector and the grid after them
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QQuickItem *menuButton = ui.item(QStringLiteral("mainMenuButton"));
        menuButton->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(menuButton->hasActiveFocus());
        const QList<QQuickItem *> visited = ui.tabCycle();
        const QList<QQuickItem *> tabs = ui.tabButtons();
        QCOMPARE(tabs.size(), 3);
        for (qsizetype i = 0; i < tabs.size(); ++i)
            QCOMPARE(visited.value(i), tabs.at(i));
        const qsizetype selectorAt = visited.indexOf(selector);
        // The grid holds the focus itself or through the tile it made current
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        qsizetype gridAt = -1;
        for (qsizetype i = 0; i < visited.size() && gridAt < 0; ++i) {
            if (visited.at(i) == grid || grid->isAncestorOf(visited.at(i)))
                gridAt = i;
        }
        // Kirigami puts the header of a page after its content in the Tab order, so the selector follows the grid
        QVERIFY(selectorAt > 2);
        QVERIFY(gridAt > 2);
    }

    void selectorNamesArePlainText()
    {
        Ui ui(m_home->path());
        const QString hostile =
            QStringLiteral("<b>Films</b><img src=\"http://evil.invalid/x.png\">");
        ui.serveTwoMovieLibraries(hostile);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("moviesTwoLibraries")));
        QQuickItem *selector = ui.item(QStringLiteral("librarySelector"));
        QVERIFY(selector);
        QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 2)));
        QTRY_COMPARE(selector->property("displayText").toString(), hostile);
        // The popup builds its entries: the name shows in the box and in the list
        auto *popup = selector->property("popup").value<QObject *>();
        QVERIFY(popup);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QTRY_VERIFY(ui.itemsWithText(hostile).size() >= 2);
        for (QQuickItem *label : ui.itemsWithText(hostile)) {
            QVERIFY2(label->inherits("QQuickText"), qPrintable(Ui::describe(label)));
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void homeTileDeepLinks()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        ui.serveTwoMovieLibraries();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies2) != nullptr);
        // The tile of the second movies library opens the Movies tab on that library
        QVERIFY(ui.openLibraryTile(kMovies2));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(ui.controller.movies()->libraryId(), kMovies2);
        QCOMPARE(ui.currentDetails()->property("tabType").toString(), QStringLiteral("movies"));
        // The choice is kept: the tab reopens on it
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.controller.movies()->libraryId(), kMovies2);
        // The series tile opens the TV Shows tab
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kSeries) != nullptr);
        QVERIFY(ui.openLibraryTile(kSeries));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QCOMPARE(ui.stackDepth(), 1);
        // The page is built before the window goes: an item still being created at teardown warns
        QTRY_COMPARE(ui.controller.shows()->status(), Kinefin::LibraryItemsModel::Ready);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        QTest::qWait(300);
    }

    void collectionsTileOpensAboveHome()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 5;
        ui.serveViews({{kMovies, QStringLiteral("Movies"), QStringLiteral("movies")},
                       {kCollections, QStringLiteral("Collections"), QStringLiteral("boxsets")}});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kCollections) != nullptr);
        QVERIFY(ui.openLibraryTile(kCollections));
        QTRY_COMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
        QCOMPARE(ui.currentDetails()->property("tabType").toString(), QString());
        QCOMPARE(ui.currentDetails()->property("title").toString(), QStringLiteral("Collections"));
        QCOMPARE(ui.stackPages().constFirst()->objectName(), QStringLiteral("homePage"));
        QCOMPARE(ui.controller.library()->libraryId(), kCollections);
        QCOMPARE(ui.rootTab(), QStringLiteral("home"));
        // The library belongs to no tab
        QTRY_COMPARE(ui.window->property("currentSection").toString(),
                     QStringLiteral("library:") + kCollections);
        for (const QQuickItem *tab : ui.tabButtons())
            QVERIFY(!tab->property("checked").toBool());
        // Opening the library that is already open keeps its page
        QPointer<QQuickItem> page = ui.currentDetails();
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openLibrary",
                                          Q_ARG(QVariant, kCollections),
                                          Q_ARG(QVariant, QStringLiteral("Collections"))));
        QTest::qWait(300);
        QCOMPARE(ui.currentDetails(), page.data());
        QCOMPARE(ui.stackDepth(), 2);
        // A tab replaces it
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.stackDepth(), 1);
    }

    void headerBoxSearchesLocallyInATab()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 40;
        ui.controller.movies()->setSearchDebounceInterval(30);
        ui.controller.shows()->setSearchDebounceInterval(30);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(field);
        // The placeholder names the tab, never a library of the server
        QCOMPARE(field->property("placeholderText").toString(), QStringLiteral("Search in Movies"));
        // A page above the tab: the first keystroke pops back to the tab
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger");
        QTRY_COMPARE(ui.stackDepth(), 2);
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        ui.type(QStringLiteral("ab"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("libraryPage"));
        QTRY_COMPARE(ui.controller.movies()->searchTerm(), QStringLiteral("ab"));
        // No search page, no global term, and the list itself carries the term
        QVERIFY(ui.item(QStringLiteral("searchPage")) == nullptr);
        QCOMPARE(ui.controller.search()->term(), QString());
        QTRY_VERIFY(!ui.server.itemsRoutes.isEmpty()
                    && ui.server.itemsRoutes.constLast().contains("searchTerm=ab"));
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        // Each tab keeps its own term for the run: the box follows the tab
        QVERIFY(ui.clickTab(QStringLiteral("TV Shows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTRY_COMPARE(field->property("text").toString(), QString());
        QCOMPARE(field->property("placeholderText").toString(),
                 QStringLiteral("Search in TV Shows"));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QTRY_COMPARE(field->property("text").toString(), QStringLiteral("ab"));
        // Home searches the whole server again
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_COMPARE(field->property("text").toString(), QString());
        QCOMPARE(field->property("placeholderText").toString(), QStringLiteral("Search"));
    }

    void ctrlNumberSelectsTabsByPosition()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QTest::keyClick(ui.window, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QTest::keyClick(ui.window, Qt::Key_3, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        // Without a Movies tab TV Shows is the second one
        ui.serveViews({{kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     QStringList({QStringLiteral("Home"), QStringLiteral("TV Shows")}));
        QTest::keyClick(ui.window, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        // The third position is gone
        QTest::keyClick(ui.window, Qt::Key_3, Qt::ControlModifier);
        QTest::qWait(200);
        QCOMPARE(ui.rootTab(), QStringLiteral("tvshows"));
    }

    void browseStateSurvivesRestart()
    {
        {
            Ui ui(m_home->path());
            ui.server.generatedItemTotal = 10;
            ui.serveTwoMovieLibraries();
            QVERIFY(ui.load());
            QVERIFY(buildState(ui, QStringLiteral("moviesTwoLibraries")));
            QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("librarySelector")),
                                              "activated", Q_ARG(int, 2)));
            QTRY_COMPARE(ui.controller.movies()->libraryId(), kMovies2);
            ui.controller.movies()->setSortKey(Kinefin::LibraryItemsModel::DateAdded);
            ui.controller.movies()->setDescending(true);
            ui.controller.movies()->setFilter(Kinefin::LibraryItemsModel::Unplayed);
            QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        }
        // A new run of the application on the same profile: the library and the view come back
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        ui.serveTwoMovieLibraries();
        QVERIFY(ui.load());
        // The startup restore of the stored server fails first, as the first run's port is closed; the new address then signs in with the saved session, or the login is asked
        QTRY_VERIFY(!ui.controller.errorText().isEmpty());
        ui.controller.connectTo(address(ui.server));
        QTRY_VERIFY(ui.controller.state() == ConnectionController::NeedsLogin
                    || ui.controller.state() == ConnectionController::Authenticated);
        if (ui.controller.state() == ConnectionController::NeedsLogin)
            ui.controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        // The tabs of the libraries appear with the views: a click before that lands on a tab that moves
        QTRY_COMPARE(ui.tabNames().size(), 3);
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QTRY_COMPARE(ui.controller.movies()->libraryId(), kMovies2);
        QCOMPARE(ui.controller.movies()->sortKey(), Kinefin::LibraryItemsModel::DateAdded);
        QVERIFY(ui.controller.movies()->descending());
        QCOMPARE(ui.controller.movies()->filter(), Kinefin::LibraryItemsModel::Unplayed);
        // The search term is never kept
        QCOMPARE(ui.controller.movies()->searchTerm(), QString());
    }

    void homeHeroTiles()
    {
        Ui ui(m_home->path());
        const QString hostile =
            QStringLiteral("<b>Films</b><img src=\"http://evil.invalid/x.png\">");
        ui.serveViews({{kMovies, hostile, QStringLiteral("movies")},
                       {kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")},
                       {kCollections, QStringLiteral("Collections"), QStringLiteral("boxsets")},
                       {kMusic, QStringLiteral("Music"), QStringLiteral("music")}});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        const auto tile = [&ui](const QString &id) {
            return ui.item(QStringLiteral("libraryCard_") + id);
        };
        const auto description = [&tile](const QString &id) {
            const QQuickItem *label =
                tile(id) ? Ui::itemIn(tile(id), QStringLiteral("libraryTileDescription")) : nullptr;
            return label && label->isVisible() ? label->property("text").toString() : QString();
        };
        QTRY_VERIFY(tile(kMusic) != nullptr);
        // The tiles of the types the app browses carry no text but the name, so they are the same size
        for (const QString &id : {kMovies, kSeries, kCollections})
            QVERIFY2(description(id).isEmpty(), qPrintable(id));
        QTRY_COMPARE(tile(kMovies)->width(), tile(kSeries)->width());
        QTRY_COMPARE(tile(kMovies)->height(), tile(kSeries)->height());
        // A type the app cannot browse says so, stays disabled and has no Recently Added row
        QCOMPARE(description(kMusic), QStringLiteral("Not supported yet"));
        QVERIFY(!tile(kMusic)->isEnabled());
        for (const QString &id : {kMovies, kSeries, kCollections})
            QVERIFY(tile(id)->isEnabled());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Recently Added in Series")));
        QVERIFY(!ui.showsText(QStringLiteral("Recently Added in Music")));
        QVERIFY(!ui.showsText(QStringLiteral("Recently Added in Collections")));
        // The library name is server text: plain text everywhere it shows
        const QList<QQuickItem *> labels = ui.itemsWithText(hostile, false);
        QVERIFY(!labels.isEmpty());
        for (QQuickItem *label : labels) {
            QVERIFY(label->property("textFormat").isValid());
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void continueWatchingSeeAll()
    {
        Ui ui(m_home->path());
        ui.server.generatedResumeTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // The Next Up row has no See All, Continue Watching has
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = ui.item(QStringLiteral("homeRowSeeAll_0"))) != nullptr
                    && button->isVisible());
        QCOMPARE(button->property("text").toString(), QStringLiteral("See All"));
        QTRY_VERIFY(ui.item(QStringLiteral("homeRowSeeAll_1")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("homeRowSeeAll_1"))->isVisible());
        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("resumeItemsPage"));
        QCOMPARE(ui.stackDepth(), 2);
        QPointer<QQuickItem> page = ui.currentDetails();
        QCOMPARE(page->property("title").toString(), QStringLiteral("Continue Watching"));
        QTRY_COMPARE(ui.controller.resumeItems()->status(), Kinefin::ResumeItemsModel::Ready);
        QCOMPARE(ui.controller.resumeItems()->totalCount(), 30);
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("home"));
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("resumeItemsGrid"))) != nullptr);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // Asking again brings the open page back as it is
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openResumeItems"));
        QTest::qWait(300);
        QCOMPARE(ui.currentDetails(), page.data());
        QCOMPARE(ui.stackDepth(), 2);
        // Back destroys the page, and the model gave its listing channel back
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(!page);
        QTRY_COMPARE(ui.controller.resumeItems()->status(), Kinefin::ResumeItemsModel::Idle);
        QCOMPARE(ui.controller.resumeItems()->rowCount(), 0);
        // The list opens again from the start
        QTRY_VERIFY((button = ui.item(QStringLiteral("homeRowSeeAll_0"))) != nullptr
                    && button->isVisible());
        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("resumeItemsPage"));
        QTRY_COMPARE(ui.controller.resumeItems()->status(), Kinefin::ResumeItemsModel::Ready);
        QTRY_VERIFY(ui.item(QStringLiteral("resumeItemsGrid"))->property("count").toInt() > 0);
    }

    void theResumeItemsPageIsReachedByTabAndEscapeGoesBack()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("resume")));
        QPointer<QQuickItem> page = ui.item(QStringLiteral("resumeItemsPage"));
        QQuickItem *grid = ui.item(QStringLiteral("resumeItemsGrid"));
        QVERIFY(page && grid);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_VERIFY(grid->hasActiveFocus());
        const QList<QQuickItem *> visited = ui.tabCycle();
        QVERIFY(visited.size() >= 2);
        bool gridStop = false;
        for (QQuickItem *item : visited) {
            QVERIFY2(item->isVisible(), qPrintable(Ui::describe(item)));
            QVERIFY2(!ui.inAnotherPage(item), qPrintable(Ui::describe(item)));
            QVERIFY2(ui.insideWindow(item), qPrintable(Ui::describe(item)));
            for (const QQuickItem *parent = item; parent; parent = parent->parentItem())
                gridStop = gridStop || parent == grid;
        }
        QVERIFY(gridStop);
        // A field of the shell may hold the focus after the cycle and would take the key itself
        grid->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(!page);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_COMPARE(ui.controller.resumeItems()->rowCount(), 0);
    }

    void recentlyAddedSeeAll()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // A sort saved for the Movies tab
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        ui.controller.movies()->setSortKey(Kinefin::LibraryItemsModel::Runtime);
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        // The third row of Home is Recently Added of the Movies library
        QCOMPARE(
            ui.controller.homeRows()
                ->data(ui.controller.homeRows()->index(2), Kinefin::HomeRowsModel::LibraryIdRole)
                .toString(),
            kMovies);
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = ui.item(QStringLiteral("homeRowSeeAll_2"))) != nullptr
                    && button->isVisible());
        const qsizetype routesBefore = ui.server.itemsRoutes.size();
        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        // The tab opens on the library, newest first, from the top
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.stackDepth(), 1);
        // One library of the type: its scope is all of them
        QVERIFY(ui.controller.movies()->libraryId().isEmpty());
        QTRY_COMPARE(ui.controller.movies()->sortKey(), Kinefin::LibraryItemsModel::DateAdded);
        QVERIFY(ui.controller.movies()->descending());
        QCOMPARE(ui.controller.movies()->filter(), Kinefin::LibraryItemsModel::All);
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QTRY_VERIFY(
            std::any_of(ui.server.itemsRoutes.cbegin() + routesBefore, ui.server.itemsRoutes.cend(),
                        [](const QByteArray &route) { return route.contains("DateCreated"); }));
        // The sort saved for the tab is untouched: the tab opened from the Home tab is on it again
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QTRY_COMPARE(ui.controller.movies()->sortKey(), Kinefin::LibraryItemsModel::Runtime);
        QVERIFY(!ui.controller.movies()->descending());
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
    }

    void userPreferencesNeedASession()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        QObject *action = ui.shellAction(QStringLiteral("userPreferencesAction"));
        QVERIFY(action);
        QVERIFY(!action->property("visible").toBool());
        QVERIFY(buildState(ui, QStringLiteral("login")));
        QVERIFY(!action->property("visible").toBool());
        QVERIFY(ui.signInAs(QStringLiteral("fixture-user")));
        QVERIFY(action->property("visible").toBool());
    }

    void userPreferencesOpenFromTheAccountMenuAndShowTheStoredValues()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // The account menu lists the entry first, and it is not part of Settings
        ui.clickCenter(ui.item(QStringLiteral("accountButton")));
        QObject *menu = ui.popup(QStringLiteral("accountMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        const QList<QQuickItem *> entries = ui.menuItems(QStringLiteral("accountMenu"));
        QVERIFY(!entries.isEmpty());
        QCOMPARE(entries.constFirst()->objectName(), QStringLiteral("userPreferencesItem"));
        QVERIFY(entries.constFirst()->isVisible());
        QCOMPARE(entries.constFirst()->property("text").toString(),
                 QStringLiteral("User Preferences"));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        QVERIFY(openPreferences(ui) != nullptr);
        // No tab and no menu button marks the page
        QCOMPARE(ui.window->property("currentSection").toString(),
                 QStringLiteral("userPreferences"));
        QVERIFY(!ui.item(QStringLiteral("mainMenuButton"))->property("down").toBool());
        // The stored values: the language names come from the catalog
        QQuickItem *audio = ui.item(QStringLiteral("prefAudioLanguage"));
        QQuickItem *subtitles = ui.item(QStringLiteral("prefSubtitleLanguage"));
        QVERIFY(audio && subtitles);
        QTRY_COMPARE(comboBoxIn(audio)->property("displayText").toString(),
                     QStringLiteral("Italian"));
        QTRY_COMPARE(comboBoxIn(subtitles)->property("displayText").toString(),
                     QStringLiteral("English"));
        QCOMPARE(comboBoxIn(ui.item(QStringLiteral("prefSubtitleMode")))
                     ->property("displayText")
                     .toString(),
                 QStringLiteral("Smart"));
        QVERIFY(ui.item(QStringLiteral("prefPlayDefaultAudioTrack"))->property("checked").toBool());
        QVERIFY(!ui.item(QStringLiteral("prefRememberSubtitleSelections"))
                     ->property("checked")
                     .toBool());
        // The libraries in the stored order, the id of the one the user cannot see kept out of sight
        QTRY_VERIFY(ui.item(QStringLiteral("prefOrderRow_2")) != nullptr);
        QCOMPARE(ui.item(QStringLiteral("prefOrderRow_0"))->property("text").toString(),
                 QStringLiteral("Movies"));
        QCOMPARE(ui.item(QStringLiteral("prefOrderRow_1"))->property("text").toString(),
                 QStringLiteral("Series"));
        QCOMPARE(ui.item(QStringLiteral("prefOrderRow_2"))->property("text").toString(),
                 QStringLiteral("Music"));
        QVERIFY(ui.item(QStringLiteral("prefOrderRow_3")) == nullptr);
        QTRY_VERIFY(ui.showsText(
            QStringLiteral("The preferences of 1 library that is not available are kept")));
        // The Home sections of Jellyfin are only listed
        QTRY_VERIFY(ui.item(QStringLiteral("prefHomeSection_1")) != nullptr);
        QCOMPARE(ui.item(QStringLiteral("prefHomeSection_0"))->property("text").toString(),
                 QStringLiteral("Libraries"));
        QCOMPARE(ui.item(QStringLiteral("prefHomeSection_1"))->property("text").toString(),
                 QStringLiteral("Continue Watching"));
        // Nothing to save yet
        QVERIFY(!ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        QVERIFY(!ui.item(QStringLiteral("prefsDiscardButton"))->isEnabled());
        QVERIFY(remote.posts.isEmpty());
    }

    void savingPostsTheWholeConfigurationAndKeepsWhatThePageDoesNotEdit()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPreferences(ui);
        QVERIFY(page != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        QTRY_VERIFY(ui.item(QStringLiteral("prefMyMediaExclude_") + kMovies) != nullptr);
        // Real clicks: click() on a switch delegate does not toggle it the way a press and release do
        QVERIFY(ui.revealAndClickUntil(page,
                                       ui.item(QStringLiteral("prefRememberSubtitleSelections")),
                                       [&] { return prefs->rememberSubtitleSelections(); }));
        QVERIFY(ui.revealAndClickUntil(
            page, ui.item(QStringLiteral("prefMyMediaExclude_") + kMovies),
            [&] { return prefs->myMediaExcludes() == QStringList{kMovies}; }));
        QVERIFY(prefs->dirty());
        QVERIFY(ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("prefsSaveButton")), "click"));
        QTRY_COMPARE(remote.posts.size(), 1);
        QTRY_VERIFY(!prefs->dirty());
        QTRY_VERIFY(!ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        // The whole object goes: the two edits, and everything else as the server had it
        const QJsonObject posted = remote.posts.constFirst();
        const QJsonObject before = preferencesConfiguration();
        QCOMPARE(posted.keys(), before.keys());
        QVERIFY(posted.value(QStringLiteral("RememberSubtitleSelections")).toBool());
        QCOMPARE(posted.value(QStringLiteral("MyMediaExcludes")).toArray(), QJsonArray{kMovies});
        QCOMPARE(posted.value(QStringLiteral("GroupedFolders")),
                 before.value(QStringLiteral("GroupedFolders")));
        QCOMPARE(posted.value(QStringLiteral("FutureField")),
                 before.value(QStringLiteral("FutureField")));
        QCOMPARE(posted.value(QStringLiteral("LatestItemsExcludes")),
                 before.value(QStringLiteral("LatestItemsExcludes")));
        QCOMPARE(posted.value(QStringLiteral("OrderedViews")),
                 before.value(QStringLiteral("OrderedViews")));
        QCOMPARE(posted.value(QStringLiteral("AudioLanguagePreference")).toString(),
                 QStringLiteral("ita"));
    }

    void aConflictShowsTheBannerAndPostsNothing()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        // Another device changes the same field first
        remote.configuration.insert(QStringLiteral("SubtitleMode"), QStringLiteral("None"));
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("prefsSaveButton")), "click"));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Conflict);
        QTRY_VERIFY(ui.showsText(QStringLiteral("Some preferences were changed on another "
                                                "device, so nothing was saved: keep editing to "
                                                "review your changes, or discard them")));
        QVERIFY(remote.posts.isEmpty());
        QVERIFY(!ui.item(QStringLiteral("prefRememberAudioSelections"))->isEnabled());
        // Keep Editing gives the draft back, a second save meets the same conflict, Discard drops the draft
        QVERIFY(ui.clickButton(QStringLiteral("Keep Editing")));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Ready);
        QVERIFY(prefs->dirty());
        QVERIFY(ui.item(QStringLiteral("prefRememberAudioSelections"))->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("prefsSaveButton")), "click"));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Conflict);
        QVERIFY(ui.clickButton(QStringLiteral("Discard")));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Ready);
        QVERIFY(!prefs->dirty());
        QCOMPARE(prefs->subtitleMode(), QStringLiteral("Smart"));
        QVERIFY(remote.posts.isEmpty());
    }

    void leavingWithUnsavedChangesAsksFirst()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        // Without a draft Escape just goes back
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesPage")) == nullptr);
        QVERIFY(openPreferences(ui) != nullptr);
        dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // With one it asks before the page is popped, and Cancel stays
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(ui.item(QStringLiteral("userPreferencesPage")) != nullptr);
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Cancel")));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(prefs->dirty());
        QCOMPARE(prefs->subtitleMode(), QStringLiteral("Always"));
        // A section asks the same, and Discard opens it
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);
        QVERIFY(ui.item(QStringLiteral("userPreferencesPage")) != nullptr);
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Discard")));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesPage")) == nullptr);
        QVERIFY(!prefs->dirty());
        QVERIFY(remote.posts.isEmpty());
        // A tab asks too, and Save writes the draft and then leaves
        QVERIFY(openPreferences(ui) != nullptr);
        dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setRememberSubtitleSelections(true);
        QTRY_VERIFY(prefs->dirty());
        QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Save")));
        QTRY_COMPARE(remote.posts.size(), 1);
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesPage")) == nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(!prefs->dirty());
    }

    void theMouseBackButtonAndClosingAskBeforeLosingUnsavedPreferences()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        const QPoint middle(ui.window->width() / 2, ui.window->height() / 2);
        QTest::mouseClick(ui.window, Qt::BackButton, Qt::NoModifier, middle);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(ui.stackDepth(), 2);
        // The page is brought back to the front under the dialog
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("userPreferencesPage"));
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Cancel")));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(prefs->dirty());
        // Closing the window asks too, and Cancel keeps it open
        ui.window->close();
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(ui.window->isVisible());
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Cancel")));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(ui.window->isVisible());
        QVERIFY(prefs->dirty());
    }

    void theHeaderSearchIsGlobalOnThePreferencesPage()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        ui.controller.movies()->setSearchDebounceInterval(30);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(field);
        QCOMPARE(field->property("placeholderText").toString(), QStringLiteral("Search in Movies"));
        QTRY_VERIFY(ui.tabButtons().value(1)->property("checked").toBool());
        QVERIFY(openPreferences(ui) != nullptr);
        // The Movies tab stays at the root of the stack but is not marked, and the box searches the server
        QCOMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.window->property("currentSection").toString(),
                 QStringLiteral("userPreferences"));
        QTRY_COMPARE(field->property("placeholderText").toString(), QStringLiteral("Search"));
        for (int index = 0; index < 3; ++index)
            QTRY_VERIFY(!ui.tabButtons().value(index)->property("checked").toBool());
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        ui.type(QStringLiteral("ab"));
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("ab"));
        QCOMPARE(ui.controller.movies()->searchTerm(), QString());
        QCOMPARE(ui.stackDepth(), 3);
        QVERIFY(ui.item(QStringLiteral("userPreferencesPage")) != nullptr);
    }

    void aFailedLoadOffersRetryAndAMissingCatalogKeepsTheStoredLanguage()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        ui.server.usersMeStatus = 500;
        ui.server.culturesStatus = 500;
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("userPreferencesAction")),
                                          "trigger"));
        auto *prefs = ui.controller.userConfiguration();
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Failed);
        QTRY_VERIFY(ui.showsText(QStringLiteral(
            "Could not load the preferences: check the connection to the server and try again")));
        QVERIFY(!ui.item(QStringLiteral("prefAudioLanguage"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("prefsSaveButton"))->isVisible());
        ui.server.usersMeStatus = 0;
        QVERIFY(ui.clickButton(QStringLiteral("Retry")));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Ready);
        // The catalog is still missing: the stored code stays selected, and the list says so
        QTRY_COMPARE(comboBoxIn(ui.item(QStringLiteral("prefAudioLanguage")))
                         ->property("displayText")
                         .toString(),
                     QStringLiteral("ita (not in the catalog)"));
        QTRY_VERIFY(ui.showsText(QStringLiteral("The language list is not available: only "
                                                "Automatic and the current choice can be "
                                                "selected")));
        QVERIFY(!prefs->dirty());
    }

    void userPreferencesKeepServerTextPlain()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        const QString library =
            QStringLiteral("<b>Films</b><img src=\"http://evil.invalid/x.png\">");
        const QString language =
            QStringLiteral("<i>Italiano</i><img src=\"http://evil.invalid/y.png\">");
        ui.serveViews({{kMovies, library, QStringLiteral("movies")},
                       {kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.server.culturesRawBody =
            QJsonDocument(
                QJsonArray{QJsonObject{{QStringLiteral("DisplayName"), language},
                                       {QStringLiteral("ThreeLetterISOLanguageName"),
                                        QStringLiteral("ita")}},
                           QJsonObject{{QStringLiteral("DisplayName"), QStringLiteral("English")},
                                       {QStringLiteral("ThreeLetterISOLanguageName"),
                                        QStringLiteral("eng")}}})
                .toJson(QJsonDocument::Compact);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        QQuickItem *box = comboBoxIn(ui.item(QStringLiteral("prefAudioLanguage")));
        QVERIFY(box);
        QTRY_COMPARE(box->property("displayText").toString(), language);
        // The name of the library shows in the order list and in the two lists of check boxes
        int found = 0;
        QTRY_VERIFY((ui.notPlainText(library, &found), found >= 3));
        const QStringList problems = ui.notPlainText(library);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
        // The popup of the box builds its entries
        auto *popup = box->property("popup").value<QObject *>();
        QVERIFY(popup);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QTRY_VERIFY(ui.itemsWithText(language).size() >= 2);
        for (QQuickItem *label : ui.itemsWithText(language)) {
            if (label->inherits("QQuickAbstractButton"))
                continue;
            QVERIFY2(label->inherits("QQuickText"), qPrintable(Ui::describe(label)));
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void aSessionSwitchDropsTheDraft()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // The session ends without asking: nothing of the draft can reach the next one
        ui.controller.switchUser();
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesPage")) == nullptr);
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Idle);
        QVERIFY(!prefs->dirty());
        QVERIFY(!prefs->loaded());
        QVERIFY(prefs->libraries().isEmpty());
        QVERIFY(remote.posts.isEmpty());
    }

    void theKeyboardReordersTheLibraries()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        QTRY_VERIFY(ui.item(QStringLiteral("prefOrderRow_2")) != nullptr);
        QQuickItem *row = ui.item(QStringLiteral("prefOrderRow_1"));
        row->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(row->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Up, Qt::ControlModifier);
        QTRY_COMPARE(prefs->orderedViews(), QStringList({kSeries, kMovies, kMusicLibrary}));
        // The rows are rebuilt: the keyboard follows the one that moved
        QTRY_VERIFY(ui.item(QStringLiteral("prefOrderRow_0")) != nullptr
                    && ui.item(QStringLiteral("prefOrderRow_0"))->hasActiveFocus());
        QCOMPARE(ui.item(QStringLiteral("prefOrderRow_0"))->property("text").toString(),
                 QStringLiteral("Series"));
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("prefsSaveButton")), "click"));
        QTRY_COMPARE(remote.posts.size(), 1);
        // The id the user cannot see keeps its slot
        QCOMPARE(remote.posts.constFirst().value(QStringLiteral("OrderedViews")).toArray(),
                 QJsonArray({kHiddenId, kSeries, kMovies, kMusicLibrary}));
    }

    void preferencesTabStaysInThePage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("preferences")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        ui.controller.userConfiguration()->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        const QList<QQuickItem *> visited = ui.tabCycle();
        QVERIFY(visited.size() >= 2);
        QStringList seen;
        bool save = false;
        bool rows = false;
        for (QQuickItem *item : visited) {
            seen << Ui::describe(item);
            QVERIFY2(item->isVisible(), qPrintable(Ui::describe(item)));
            QVERIFY2(!ui.inAnotherPage(item), qPrintable(Ui::describe(item)));
            QVERIFY2(ui.insideWindow(item), qPrintable(Ui::describe(item)));
            save = save || item->objectName() == QStringLiteral("prefsSaveButton");
            rows = rows || item->objectName().startsWith(QStringLiteral("prefOrderRow_"));
        }
        QVERIFY2(save && rows, qPrintable(seen.join(QLatin1Char('\n'))));
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
        QCOMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Idle);
        QCOMPARE(ui.controller.homeRows()->rowCount(), 0);
        // The sign-in page shows the avatars of the users of the server, so the origin stays until the connection is dropped
        QVERIFY(!ui.origin->get().isEmpty());
        ui.controller.cancel();
        QTRY_VERIFY(ui.origin->get().isEmpty());
    }

    void theStarOpensTheFavoritesPageAndMarksItself()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *star = ui.item(QStringLiteral("favoritesButton"));
        QVERIFY(star && star->isVisible() && star->isEnabled());
        QVERIFY(!star->property("down").toBool());
        QTRY_VERIFY(!ui.controller.libraries().isEmpty());
        ui.clickCenter(star);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QCOMPARE(ui.stackDepth(), 2);
        // The star marks itself and no tab does
        QTRY_VERIFY(star->property("down").toBool());
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        for (int index = 0; index < 3; ++index)
            QTRY_VERIFY(!ui.tabButtons().value(index)->property("checked").toBool());
        // By type, the default: the movies, the shows and the episode, with their counts
        QCOMPARE(ui.controller.favorites()->count(), 6);
        QTRY_COMPARE(favoriteGroupTitles(ui),
                     QStringList({QStringLiteral("Movies"), QStringLiteral("TV Shows"),
                                  QStringLiteral("Episodes")}));
        QCOMPARE(ui.item(QStringLiteral("favoritesGroupCount_0"))->property("text").toString(),
                 QStringLiteral("3 favorites"));
        QCOMPARE(ui.item(QStringLiteral("favoritesGroupCount_2"))->property("text").toString(),
                 QStringLiteral("1 favorite"));
        // Nothing is marked as loading, empty or partial
        QVERIFY(!ui.item(QStringLiteral("favoritesLoading"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesEmpty"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesCappedBanner"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesPartialBanner"))->isVisible());
        // The first row takes the keys
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesRow_0"))->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // The star again brings the page back as it is: no second page, no second scan
        const int requests = ui.server.itemsRoutes.size();
        ui.clickCenter(star);
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.server.itemsRoutes.size(), requests);
    }

    void theCompactMenuOpensTheFavorites()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // Below the Compact limit, which the shell derives from grid units: a fixed width may lie above it
        QVERIFY(ui.opensWindowOfWidth(ui.window->property("compactLimit").toInt() - 40));
        QTRY_VERIFY(ui.shellHeader()->property("compact").toBool());
        QTRY_VERIFY(!ui.item(QStringLiteral("favoritesButton"))->isVisible());
        QObject *action = ui.shellAction(QStringLiteral("compactFavoritesAction"));
        QVERIFY(action && action->property("enabled").toBool());
        QTRY_VERIFY(!ui.controller.libraries().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
    }

    void groupByRegroupsWithoutARequestAndIsKeptAcrossARestart()
    {
        {
            Ui ui(m_home->path());
            serveFavorites(ui);
            QVERIFY(ui.load());
            QVERIFY(buildState(ui, QStringLiteral("home")));
            QQuickItem *page = openFavorites(ui);
            QVERIFY(page);
            QCOMPARE(ui.controller.favoriteGroups()->grouping(),
                     Kinefin::FavoritesGroupsModel::Type);
            for (const char *name : {"Type", "Library", "Genre", "Year", "None"})
                QVERIFY2(groupByAction(page, QLatin1String(name)), name);
            QTRY_VERIFY(groupByAction(page, QStringLiteral("Type"))->property("checked").toBool());
            QVERIFY(!groupByAction(page, QStringLiteral("Library"))->property("checked").toBool());

            // Each choice regroups at once and sends nothing
            const int requests = ui.server.itemsRoutes.size();
            const auto choose = [&](const QString &grouping, const QStringList &titles) {
                QVERIFY(QMetaObject::invokeMethod(groupByAction(page, grouping), "trigger"));
                QTRY_COMPARE(favoriteGroupTitles(ui), titles);
                QTRY_VERIFY(groupByAction(page, grouping)->property("checked").toBool());
                QCOMPARE(ui.server.itemsRoutes.size(), requests);
            };
            choose(QStringLiteral("Library"), {QStringLiteral("Movies"), QStringLiteral("Series")});
            QVERIFY(!groupByAction(page, QStringLiteral("Type"))->property("checked").toBool());
            choose(QStringLiteral("Genre"), {QStringLiteral("Action"), QStringLiteral("Comedy"),
                                             QStringLiteral("Drama"), QStringLiteral("No genre")});
            choose(QStringLiteral("Year"), {QStringLiteral("2020"), QStringLiteral("2019"),
                                            QStringLiteral("2015"), QStringLiteral("2012")});
            // None is one group, shown as a grid of every favorite and with no title row
            QVERIFY(
                QMetaObject::invokeMethod(groupByAction(page, QStringLiteral("None")), "trigger"));
            QTRY_COMPARE(ui.controller.favoriteGroups()->count(), 1);
            QTRY_VERIFY(groupByAction(page, QStringLiteral("None"))->property("checked").toBool());
            QCOMPARE(ui.server.itemsRoutes.size(), requests);
            QQuickItem *grid = nullptr;
            QTRY_VERIFY((grid = ui.item(QStringLiteral("favoritesGrid"))) != nullptr);
            QTRY_COMPARE(grid->property("count").toInt(), 6);
            QTRY_VERIFY(ui.layoutProblems().isEmpty());
            choose(QStringLiteral("Library"), {QStringLiteral("Movies"), QStringLiteral("Series")});
            QTRY_VERIFY(ui.layoutProblems().isEmpty());
        }
        // A new run of the application on the same profile: the page opens grouped by library
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QTRY_VERIFY(!ui.controller.errorText().isEmpty());
        ui.controller.connectTo(address(ui.server));
        const auto settled = [&] {
            return ui.controller.state() == ConnectionController::NeedsLogin
                   || ui.controller.state() == ConnectionController::Authenticated;
        };
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 15000);
        if (ui.controller.state() == ConnectionController::NeedsLogin)
            ui.controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY_WITH_TIMEOUT(ui.item(QStringLiteral("homePage")) != nullptr, 15000);
        QCOMPARE(ui.controller.favoriteGroups()->grouping(),
                 Kinefin::FavoritesGroupsModel::Library);
        QQuickItem *page = openFavorites(ui);
        QVERIFY(page);
        QTRY_COMPARE(favoriteGroupTitles(ui),
                     QStringList({QStringLiteral("Movies"), QStringLiteral("Series")}));
        QTRY_VERIFY(groupByAction(page, QStringLiteral("Library"))->property("checked").toBool());
    }

    void aCappedScanShowsItsBannerAndShowAllListsWhatWasLoaded()
    {
        Ui ui(m_home->path());
        const auto data = serveFavorites(ui);
        QJsonArray many;
        for (int n = 1; n <= 1001; ++n)
            many.append(favoriteJson(n, QStringLiteral("Movie"),
                                     QStringLiteral("Movie %1").arg(n, 5, 10, QLatin1Char('0')),
                                     2000));
        data->byLibrary[kMovies] = many;
        data->byLibrary[kSeries] = QJsonArray();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openFavorites(ui));
        QCOMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Capped);
        QCOMPARE(ui.controller.favorites()->count(), 1000);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesCappedBanner"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("favorites: the list may be incomplete"), false));
        QVERIFY(!ui.item(QStringLiteral("favoritesPartialBanner"))->isVisible());
        // Show All is cut from the same snapshot and says it may be partial
        QQuickItem *showAll = nullptr;
        QTRY_VERIFY((showAll = ui.item(QStringLiteral("favoritesShowAll_0"))) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(showAll, "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesGroupPage"));
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("favoritesGroupGrid"))) != nullptr);
        QTRY_COMPARE(grid->property("count").toInt(), 1000);
        QVERIFY(ui.showsText(QStringLiteral("the list may be incomplete"), false));
    }

    void aFailedLibraryKeepsTheOthersAndOffersRetry()
    {
        Ui ui(m_home->path());
        const auto data = serveFavorites(ui);
        data->failing[kSeries] = 500;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openFavorites(ui));
        QCOMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::PartialError);
        // The movies show, the failure is no empty state
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesPartialBanner"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("Some libraries could not be read")));
        QVERIFY(!ui.item(QStringLiteral("favoritesCappedBanner"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesEmpty"))->isVisible());
        QTRY_COMPARE(favoriteGroupTitles(ui), QStringList({QStringLiteral("Movies")}));
        // Retry scans again
        data->failing.clear();
        QVERIFY(ui.clickButton(QStringLiteral("Retry")));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QTRY_VERIFY(!ui.item(QStringLiteral("favoritesPartialBanner"))->isVisible());
        QTRY_COMPARE(favoriteGroupTitles(ui).size(), 3);
    }

    void noFavoritesAndNoLibrariesHaveTheirOwnStates()
    {
        Ui ui(m_home->path());
        const auto data = serveFavorites(ui);
        data->byLibrary[kMovies] = QJsonArray();
        data->byLibrary[kSeries] = QJsonArray();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openFavorites(ui));
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesEmpty"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("No favorites yet")));
        QVERIFY(!ui.item(QStringLiteral("favoritesNoLibraries"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesLoading"))->isVisible());
        QCOMPARE(ui.controller.favoriteGroups()->count(), 0);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
    }

    void withoutLibrariesThePageLeadsHome()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        ui.serveViews({{kMusic, QStringLiteral("Music"), QStringLiteral("music")}});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openFavorites(ui));
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesNoLibraries"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesEmpty"))->isVisible());
        QCOMPARE(ui.controller.favorites()->libraryCount(), 0);
        QPointer<QQuickItem> page = ui.item(QStringLiteral("favoritesPage"));
        // The button belongs to the placeholder's action: it is triggered through the placeholder
        QObject *action = ui.item(QStringLiteral("favoritesNoLibraries"))
                              ->property("helpfulAction")
                              .value<QObject *>();
        QVERIFY(action && action->property("text").toString() == QStringLiteral("Go to Home"));
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(!page);
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("home"));
    }

    void showAllOpensTheGroupAndBackReturnsTheFocus()
    {
        Ui ui(m_home->path());
        const auto data = serveFavorites(ui);
        // The first movie behind the details route
        ui.server.itemsById.insert(
            favoriteId(1), QJsonDocument(cardItem(favoriteId(1))).toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openFavorites(ui));
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("favoritesRow_0"))) != nullptr
                    && row->hasActiveFocus());
        // Show All of the second group, the shows
        QQuickItem *showAll = ui.item(QStringLiteral("favoritesShowAll_1"));
        QVERIFY(showAll);
        QVERIFY(QMetaObject::invokeMethod(showAll, "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesGroupPage"));
        QCOMPARE(ui.stackDepth(), 3);
        QCOMPARE(ui.currentDetails()->property("title").toString(), QStringLiteral("TV Shows"));
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("favoritesGroupGrid"))) != nullptr);
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // Back returns to the row the user came from
        QPointer<QQuickItem> groupPage = ui.currentDetails();
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.stackDepth(), 2);
        QTRY_VERIFY(!groupPage);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesRow_0"))->hasActiveFocus());
        // A card opens its item above the page and Escape comes back to the same card
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesRow_0"))->hasActiveFocus());
        // A replaced snapshot takes the group page with it
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("favoritesShowAll_0")), "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesGroupPage"));
        data->byLibrary[kSeries] = QJsonArray();
        ui.controller.favorites()->reload();
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QTRY_COMPARE(ui.stackDepth(), 2);
    }

    void theMenuKeyOpensTheCardMenuAndEscapeClosesItFirst()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesRow_0"))->hasActiveFocus());
        QObject *menu = ui.popup(QStringLiteral("cardMenu"));
        QVERIFY(menu);
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        // The card menu is the one of every page, with no entry that changes a favorite
        QCOMPARE(menu->property("itemId").toString(), favoriteId(1));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTest::keyClick(ui.window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    void backClosesThePageAndOpeningAgainScansAgain()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        QCOMPARE(ui.window->property("favoritesPages").toInt(), 1);
        QPointer<QQuickItem> page = ui.item(QStringLiteral("favoritesPage"));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(!page);
        QTRY_COMPARE(ui.window->property("favoritesPages").toInt(), 0);
        QTRY_COMPARE(ui.window->property("currentSection").toString(), QStringLiteral("home"));
        // The next visit starts from a new scan: the star shows nothing of the old snapshot while it runs
        const int requests = ui.server.itemsRoutes.size();
        ui.clickCenter(ui.item(QStringLiteral("favoritesButton")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_VERIFY(ui.server.itemsRoutes.size() > requests);
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QCOMPARE(ui.window->property("favoritesPages").toInt(), 1);
        QCOMPARE(ui.controller.favorites()->count(), 6);
    }

    void theHeaderSearchIsGlobalOnTheFavoritesPage()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        ui.controller.movies()->setSearchDebounceInterval(30);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(field);
        QCOMPARE(field->property("placeholderText").toString(), QStringLiteral("Search in Movies"));
        QTRY_VERIFY(ui.tabButtons().value(1)->property("checked").toBool());
        QVERIFY(openFavorites(ui));
        // The Movies tab stays at the root of the stack but is not marked, and the box searches the server
        QCOMPARE(ui.rootTab(), QStringLiteral("movies"));
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QTRY_COMPARE(field->property("placeholderText").toString(), QStringLiteral("Search"));
        for (int index = 0; index < 3; ++index)
            QTRY_VERIFY(!ui.tabButtons().value(index)->property("checked").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesButton"))->property("down").toBool());
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        ui.type(QStringLiteral("ab"));
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("ab"));
        QCOMPARE(ui.controller.movies()->searchTerm(), QString());
        QCOMPARE(ui.stackDepth(), 3);
        QVERIFY(ui.item(QStringLiteral("favoritesPage")) != nullptr);
    }

    void switchingUserOrServerClearsTheSnapshot()
    {
        Ui ui(m_home->path());
        const auto data = serveFavorites(ui);
        data->byLibrary[kMovies] = QJsonArray{
            favoriteJson(1, QStringLiteral("Movie"), QStringLiteral("Alice Movie"), 2001)};
        data->byLibrary[kSeries] = QJsonArray();
        QVERIFY(ui.load());
        QVERIFY(ui.openPickerWithTwoUsers());
        ui.controller.chooseUser(kAliceId);
        QVERIFY(ui.reach(ConnectionController::Authenticated, QStringLiteral("homePage")));
        QVERIFY(openFavorites(ui));
        QCOMPARE(ui.controller.favorites()->count(), 1);
        const auto firstName = [&] {
            return ui.controller.favorites()
                ->data(ui.controller.favorites()->index(0, 0), Kinefin::FavoritesModel::NameRole)
                .toString();
        };
        QCOMPARE(firstName(), QStringLiteral("Alice Movie"));
        // Another user of the server: nothing of Alice's is left, not even before the page scans
        ui.controller.switchUser();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::ChoosingUser);
        QCOMPARE(ui.controller.favorites()->count(), 0);
        QCOMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Idle);
        QCOMPARE(ui.controller.favoriteGroups()->count(), 0);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesPage")) == nullptr);
        data->byLibrary[kMovies] =
            QJsonArray{favoriteJson(2, QStringLiteral("Movie"), QStringLiteral("Bob Movie"), 2002)};
        ui.controller.chooseUser(kBobId);
        QVERIFY(ui.reach(ConnectionController::Authenticated, QStringLiteral("homePage")));
        QCOMPARE(ui.controller.favorites()->count(), 0);
        QVERIFY(openFavorites(ui));
        QCOMPARE(ui.controller.favorites()->count(), 1);
        QCOMPARE(firstName(), QStringLiteral("Bob Movie"));
        QTRY_COMPARE(favoriteGroupTitles(ui), QStringList({QStringLiteral("Movies")}));
        // Leaving the server drops the snapshot and its libraries
        ui.controller.cancel();
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesPage")) == nullptr);
        QCOMPARE(ui.controller.favorites()->count(), 0);
        QCOMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Idle);
        QCOMPARE(ui.controller.favorites()->libraryCount(), 0);
    }

    void aLostSessionEmptiesTheFavorites()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        ui.server.denyViews = true;
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::NeedsLogin);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesPage")) == nullptr);
        QCOMPARE(ui.controller.favorites()->count(), 0);
        QCOMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Idle);
        QCOMPARE(ui.controller.favoriteGroups()->count(), 0);
    }

    void favoriteNamesAreShownAsPlainText()
    {
        Ui ui(m_home->path());
        const auto data = serveFavorites(ui);
        ui.serveViews({{kMovies, kHostileName, QStringLiteral("movies")},
                       {kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        data->byLibrary[kMovies] = QJsonArray{favoriteJson(
            1, QStringLiteral("Movie"), QStringLiteral("Alpha"), 2020, {kHostileName})};
        data->byLibrary[kSeries] = QJsonArray();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *page = openFavorites(ui);
        QVERIFY(page);
        for (const QString &grouping : {QStringLiteral("Library"), QStringLiteral("Genre")}) {
            QVERIFY(QMetaObject::invokeMethod(groupByAction(page, grouping), "trigger"));
            QTRY_COMPARE(favoriteGroupTitle(ui, 0), kHostileName);
            int found = 0;
            const QStringList problems = ui.notPlainText(kHostileName, &found);
            QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
            QVERIFY(found > 0);
        }
    }

    void theFavoritesCardsKeepTheirKeys()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // Down walks the rows
        QQuickItem *first = ui.item(QStringLiteral("favoritesRow_0"));
        QTRY_VERIFY(first && first->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesRow_1"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(first->hasActiveFocus());
        // Tab stops stay visible and inside the page in front or the shell
        const QList<QQuickItem *> visited = ui.tabCycle();
        QVERIFY(visited.size() >= 2);
        QStringList seen;
        for (QQuickItem *item : visited) {
            seen << Ui::describe(item);
            QVERIFY2(item->isVisible(), qPrintable(seen.join(QLatin1Char('\n'))));
            QVERIFY2(!ui.inAnotherPage(item), qPrintable(seen.join(QLatin1Char('\n'))));
        }
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

QTEST_MAIN(TestUiPages)
#include "test_ui_pages.moc"
