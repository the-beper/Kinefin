// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The real Main.qml of the application, loaded from the QML module offscreen against the fake server: signed in, the Home page with its library tiles and rows, the library grid, its empty and failed states, plain-text rendering of server metadata and the return to sign-in. Any QML warning fails a test except Kirigami's own noise (the same list as tests/shell_smoke.py: keep them in step)
#include "FakeJellyfin.h"
#include "MemoryTokenStore.h"

#include "app/ConnectionController.h"
#include "app/ImageNetworkFactory.h"
#include "app/PipSurface.h"
#include "imagecache/ImageOrigin.h"
#include "integration/RemoteInputServer.h"
#include "models/SearchModel.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"
#include "syncplay/SyncPlayController.h"

#include <KLocalizedQmlContext>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonArray>
#include <QKeySequence>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlIncubator>
#include <QQmlListReference>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

using Kinefin::ConnectionController;

// Qt's QTRY_* wait 5000 ms; these wait as long, scaled (kTimeoutScale below)
#undef QTRY_VERIFY
#undef QTRY_VERIFY2
#undef QTRY_COMPARE
#define QTRY_VERIFY(expr) QTRY_VERIFY_WITH_TIMEOUT((expr), scaled(5000))
#define QTRY_VERIFY2(expr, messageExpression)                                                      \
    QTRY_VERIFY2_WITH_TIMEOUT((expr), (messageExpression), scaled(5000))
#define QTRY_COMPARE(actual, expected) QTRY_COMPARE_WITH_TIMEOUT((actual), (expected), scaled(5000))

namespace {
// Every wait of this file scales with KINEFIN_TEST_TIMEOUT_SCALE (integer 1 to 10, default 1): a CI runner is several times slower than a development machine
const int kTimeoutScale =
    std::clamp(qEnvironmentVariableIntValue("KINEFIN_TEST_TIMEOUT_SCALE"), 1, 10);

int scaled(int ms)
{
    return ms * kTimeoutScale;
}

template <typename Predicate> bool qWaitForScaled(Predicate &&predicate, int timeoutMs = 5000)
{
    return QTest::qWaitFor(std::forward<Predicate>(predicate), scaled(timeoutMs));
}

QStringList gWarnings;
// The message handler also runs on QML's network thread
QMutex gWarningsMutex;
QtMessageHandler gPreviousHandler = nullptr;

const QStringList kKnownNoise = {
    // Kirigami's PageRow warns about pushed pages in the offscreen test environment
    QStringLiteral("Created graphical object was not placed in the graphics scene"),
    // The animation driver of a window destroyed while a page slide is settling is stopped once too often (Qt 6.11.2, offscreen)
    QStringLiteral("QUnifiedTimer::stopAnimationDriver: driver is not running"),
    // The offscreen platform has no window stacking: the return from picture in picture raises the main window
    QStringLiteral("This plugin does not support raise()"),
    // KIconTheme looks for the compiled breeze-icons resource the first time the desktop style draws an icon; inferred: the Fedora build image has no such resource file and the icons come from /usr/share/icons/breeze
    QStringLiteral("Icon theme \"breeze-internal\" not found"),
    // A list view resets while one of its delegates is still being created asynchronously (seen under the desktop style in the Favorites tests); the two indices depend on timing, and the page is empty or closed by then
    QStringLiteral("DelegateModel::cancel: index out range"),
    // Qt's accounting of an incubation cancelled at teardown, reported after the assertions of the test: it does not reflect the behaviour of the app; deliberately not in tests/shell_smoke.py, which runs the real app
    QStringLiteral("items in the process of being created at engine destruction."),
};

// True when the item cannot show markup: Qt Quick Text in plain format, or what the desktop style puts in a ComboBox instead (QStyle paints the text as it is, and the text field it keeps for editing never renders markup; neither has a text format)
bool showsPlainText(const QQuickItem *label)
{
    if (label->inherits("KQuickStyleItem") || label->inherits("QQuickTextInput"))
        return true;
    return label->inherits("QQuickText")
           && label->property("textFormat").toInt() == int(Qt::PlainText);
}

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
          groupPlayer(controller.playbackClient(), *controller.queue(), settings, QString()),
          syncPlay(controller.playbackClient(), *controller.socket(), groupPlayer),
          origin(std::make_shared<Kinefin::ImageOrigin>()),
          imageFactory(origin, root + QStringLiteral("/images"))
    {
        controller.setImageOrigin(origin);
    }

    // Lets the engine finish creating the items of a page pushed a moment ago before it is torn down (Qt warns otherwise); the fixed wait is for a window that is already gone, where settle() returns at once
    ~Ui()
    {
        QTest::qWait(300);
        settle();
    }

    // Waits until no item is being created for a few polls in a row: a page replaced or a window closed while it was still being built leaves Qt with a creation it never finishes, and it warns about it when the engine goes
    void settle()
    {
        const QQmlIncubationController *incubation = engine.incubationController();
        QElapsedTimer timer;
        timer.start();
        int quiet = 0;
        while (quiet < 5 && timer.elapsed() < scaled(10000)) {
            QTest::qWait(20);
            quiet = incubation && incubation->incubatingObjectCount() > 0 ? 0 : quiet + 1;
        }
    }

    // Loads Main.qml; true when a window came up
    bool load()
    {
        if (!server.listen())
            return false;
        // The Favorites row is off here so that the rows and the Items requests stay as the other tests count them; the tests of the row switch it on
        settings.setValue(QStringLiteral("library"), QStringLiteral("home.favorites"), false);
        engine.setNetworkAccessManagerFactory(&imageFactory);
        KLocalization::setupLocalizedContext(&engine);
        engine.setInitialProperties(
            {{QStringLiteral("connection"), QVariant::fromValue(&controller)},
             {QStringLiteral("settings"), QVariant::fromValue(&settings)},
             {QStringLiteral("syncPlay"), QVariant::fromValue(&syncPlay)},
             {QStringLiteral("startupWindowMode"), QStringLiteral("windowed")}});
        engine.loadFromModule(QStringLiteral("org.kde.kinefin"), QStringLiteral("Main"));
        if (engine.rootObjects().isEmpty())
            return false;
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
        // The startup cover takes every click until the shell is ready: the tests click as a user would, once it is gone
        return window && QTest::qWaitForWindowExposed(window)
               && qWaitForScaled([this] { return !window->property("startupVisible").toBool(); },
                                 5000);
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
        if (!qWaitForScaled([this] { return item(QStringLiteral("searchPage")) != nullptr; }, 5000))
            return nullptr;
        QQuickItem *field = nullptr;
        if (!qWaitForScaled(
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
        // The Settings page scrolls its form in a plain scroll view and has no such property
        if (!flickable)
            flickable = enclosingFlickable(target);
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
            if (timer.elapsed() > scaled(timeoutMs))
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
        if (!qWaitForScaled(
                [this] { return controller.state() == ConnectionController::NeedsLogin; }, 10000))
            return false;
        controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        return qWaitForScaled(
                   [this] { return controller.state() == ConnectionController::Authenticated; },
                   10000)
               && qWaitForScaled([this] { return item(QStringLiteral("homePage")) != nullptr; },
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
        return qWaitForScaled([this, state] { return controller.state() == state; }, 10000)
               && qWaitForScaled([this, &page] { return item(page) != nullptr; }, 10000);
    }

    // Signs in as the user of the table, from the sign-in page
    bool signInAs(const QString &name)
    {
        controller.login(name, QStringLiteral("fixture-password"));
        return reach(ConnectionController::Authenticated, QStringLiteral("homePage"));
    }

    // Signs in as a user of the table (Alice has an avatar, Bob has none) through the sign-in page. The window must already be loaded
    bool signInAsUser(const QString &name)
    {
        serveUsers();
        controller.connectTo(address(server));
        return reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage"))
               && signInAs(name);
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
        if (!qWaitForScaled([this] { return savedUsers().size() == 1; }, 5000))
            return false;
        controller.switchUser();
        if (!reach(ConnectionController::ChoosingUser, QStringLiteral("userPickerPage")))
            return false;
        controller.addUser();
        if (!reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage"))
            || !signInAs(secondName))
            return false;
        if (!qWaitForScaled([this] { return savedUsers().size() == 2; }, 5000))
            return false;
        controller.switchUser();
        return reach(ConnectionController::ChoosingUser, QStringLiteral("userPickerPage"))
               && qWaitForScaled(
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
        return qWaitForScaled(
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
    // The Undo action of the queue page, null when it is not there
    QObject *undoQueueAction() const
    {
        QQmlListReference actions(item(QStringLiteral("queuePage")), "actions");
        for (qsizetype i = 0; i < actions.count(); ++i) {
            if (actions.at(i)->objectName() == QStringLiteral("undoQueueAction"))
                return actions.at(i);
        }
        return nullptr;
    }
    static QQuickItem *enclosingFlickable(QQuickItem *item)
    {
        for (QQuickItem *parent = item; parent; parent = parent->parentItem()) {
            if (parent->inherits("QQuickFlickable"))
                return parent;
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
    // Every item below `root` with this object name
    static QList<QQuickItem *> allItemsNamed(QQuickItem *root, const QString &objectName)
    {
        QList<QQuickItem *> found;
        for (QQuickItem *candidate : allItems(root)) {
            if (candidate->objectName() == objectName)
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
    // Waits for the button, then clicks it once: a message box shows its buttons a moment after its text, and the click must not sit in a QTRY_* (they evaluate the expression once more after the wait)
    bool clickButton(const QString &text)
    {
        QQuickItem *button = nullptr;
        if (!qWaitForScaled(
                [&] {
                    for (QQuickItem *candidate : itemsWithText(text)) {
                        if (candidate->isVisible() && candidate->isEnabled()
                            && candidate->width() > 0
                            && candidate->metaObject()->indexOfMethod("click()") >= 0)
                            button = candidate;
                    }
                    return button != nullptr;
                },
                5000))
            return false;
        return QMetaObject::invokeMethod(button, "click");
    }
    // A page that is still sliding in or out sits where it was when the click is
    // measured, and the press lands on the page below it: the row then makes that
    // page current and the page the test aimed at is taken for a leftover. So a
    // click waits for the row to stop moving first, and clicks anyway after a
    // second if it never does
    bool columnSettled() const
    {
        auto *stack = window->property("pageStack").value<QObject *>();
        QObject *column = stack ? stack->property("columnView").value<QObject *>() : nullptr;
        return !column || (!column->property("moving").toBool() && !tabSwitching());
    }
    // True from a tab switch until its fade and slide are over
    bool tabSwitching() const { return window->property("tabSwitching").toBool(); }
    void clickCenter(QQuickItem *target)
    {
        qWaitForScaled([this] { return columnSettled(); }, 1000);
        const QPointF center =
            target->mapToScene(QPointF(target->width() / 2, target->height() / 2));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    }
    bool openLibraryTile(const QString &id)
    {
        const QString name = QStringLiteral("libraryCard_") + id;
        // The tiles shift while the rows around them load: a click on a tile that still moves lands on its neighbour
        QPointF before(-1, -1);
        if (!qWaitForScaled(
                [&] {
                    const QQuickItem *tile = item(name);
                    const QPointF now = tile ? tile->mapToScene(QPointF(0, 0)) : QPointF(-2, -2);
                    const bool still = tile && now == before;
                    before = now;
                    QTest::qWait(50);
                    return still;
                },
                3000))
            return false;
        clickCenter(item(name));
        return qWaitForScaled([this] { return item(QStringLiteral("libraryPage")) != nullptr; },
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
        qWaitForScaled(
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

    // The object names of the entries a menu shows, in order: separators and entries without a name are left out
    QStringList shownMenuNames(const QString &menuName) const
    {
        QStringList names;
        for (const QQuickItem *entry : menuItems(menuName)) {
            if (entry->isVisible() && !entry->objectName().isEmpty())
                names << entry->objectName();
        }
        return names;
    }

    // Opens the main menu with a click on its button and waits until it is open
    QObject *openMainMenuByClick()
    {
        clickCenter(item(QStringLiteral("mainMenuButton")));
        QObject *menu = popup(QStringLiteral("mainMenu"));
        if (!menu || !qWaitForScaled([menu] { return menu->property("opened").toBool(); }, 5000))
            return nullptr;
        return menu;
    }

    // Closes a menu and waits for its exit transition: a menu still fading out blocks the next open
    static bool closeMenu(QObject *menu)
    {
        return QMetaObject::invokeMethod(menu, "close")
               && qWaitForScaled([menu] { return !menu->property("visible").toBool(); }, 5000);
    }

    // The header widgets against the mode this width should give: the box and the queue fold into the main menu in Compact mode, and a tab carries its label only when `labelledTabs` is true. Empty when all is well
    QStringList modeProblems(bool labelled, bool compact) const
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
            {"headerSearchButton", compact},
            {"queueButton", !compact},
            {"mainMenuButton", true},
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
        const int wantedDisplay = labelled ? 2 /*TextBesideIcon*/ : 0 /*IconOnly*/;
        if (window->property("compactHeader").toBool() == false
            && shellHeader()->property("labelledTabs").toBool() != labelled)
            problems << QStringLiteral("labelledTabs is %1, the mode wants %2")
                            .arg(shellHeader()->property("labelledTabs").toBool())
                            .arg(labelled);
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
        return qWaitForScaled([this, width] { return window->width() == width; }, 3000);
    }

    FakeJellyfin server;
    MemoryTokenStore store;
    Kinefin::ClientSettings settings;
    ConnectionController controller;
    // The group's own player, never shown (the shell's `player` is a stub where a test needs one): the controller only needs one to hand items to
    Kinefin::PlayerController groupPlayer;
    Kinefin::SyncPlayController syncPlay;
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

// Opens Settings from its menu action unless it is open; the page, or null when it did not come up
QQuickItem *openSettings(Ui &ui)
{
    if (!ui.item(QStringLiteral("settingsPage"))) {
        if (!QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger")
            || !qWaitForScaled([&] { return ui.item(QStringLiteral("settingsPage")) != nullptr; },
                               10000))
            return nullptr;
    }
    return ui.item(QStringLiteral("settingsPage"));
}

// Opens Settings, selects the User entry and waits until the configuration is loaded; the form, or null when it did not come up
QQuickItem *openPreferences(Ui &ui)
{
    QQuickItem *page = openSettings(ui);
    if (!page
        || !QMetaObject::invokeMethod(page, "selectSection",
                                      Q_ARG(QVariant, QStringLiteral("user"))))
        return nullptr;
    if (!qWaitForScaled([&] { return ui.item(QStringLiteral("userPreferencesForm")) != nullptr; },
                        10000))
        return nullptr;
    // The form refreshes when it is created (a load when nothing is loaded yet); one the sign-in started for Home may still be under way, and the form then waits for that one
    return qWaitForScaled(
               [&] {
                   const auto *prefs = ui.controller.userConfiguration();
                   return prefs->state() == Kinefin::UserConfigurationModel::Ready
                          && prefs->loaded();
               },
               10000)
               ? ui.item(QStringLiteral("userPreferencesForm"))
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

// Clicks the button with this text inside a dialog: the Save bar has a Discard button of its own
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

// Opens the Favorites page the way its tab does and waits until the scan ended; null when the page did not come up. The libraries come first: the scan reads the ones the server listed
QQuickItem *openFavorites(Ui &ui)
{
    if (!qWaitForScaled([&] { return !ui.controller.libraries().isEmpty(); }, 10000))
        return nullptr;
    if (!QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openFavorites"))
        return nullptr;
    if (!qWaitForScaled([&] { return ui.item(QStringLiteral("favoritesPage")) != nullptr; }, 10000))
        return nullptr;
    return qWaitForScaled(
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

// The button the library toolbar builds for the action with this object name: a visible one with a size, none while the action is off or folded into the overflow menu
QQuickItem *toolbarButton(const Ui &ui, const QString &actionName)
{
    const QObject *action = ui.popup(actionName);
    const QQuickItem *bar = ui.item(QStringLiteral("libraryToolbar"));
    if (!action || !bar)
        return nullptr;
    for (QQuickItem *candidate : allItems(const_cast<QQuickItem *>(bar))) {
        if (candidate->inherits("QQuickAbstractButton") && candidate->isVisible()
            && candidate->width() > 0 && candidate->height() > 0
            && candidate->property("action").value<QObject *>() == action)
            return candidate;
    }
    return nullptr;
}

// The toolbar button of a sub-tab ("favorites", "collections", ...): the main list has none
QQuickItem *subtabButton(const Ui &ui, const QString &id)
{
    return toolbarButton(ui, QStringLiteral("subtab_") + id);
}

// A row of cards has built its cards: the list is in the scene while it is still being built, its count set before them
bool listBuilt(const QQuickItem *list)
{
    return list && list->property("currentItem").value<QObject *>() != nullptr;
}

// The item exists and holds the keyboard: a row of cards builds its list asynchronously, so the list may not be there yet
bool itemHasFocus(const Ui &ui, const QString &name)
{
    const QQuickItem *item = ui.item(name);
    return item && item->hasActiveFocus();
}

// The library page's grid or list is built: it sits in the scene while it is still being built, cards last, and the content takes it once it is whole
bool libraryViewBuilt(const Ui &ui)
{
    const QQuickItem *content = ui.item(QStringLiteral("libraryContent"));
    return content && content->property("view").value<QObject *>() != nullptr;
}

// Brings the window to one of the screens the layout checks walk through; false when it could not
bool buildState(Ui &ui, const QString &state)
{
    auto *root = ui.engine.rootObjects().constFirst();
    const auto wait = [&](const QString &objectName) {
        return qWaitForScaled([&] { return ui.item(objectName) != nullptr; }, 10000);
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
        if (state == QLatin1String("about")) {
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger");
            QObject *dialog = ui.popup(QStringLiteral("aboutDialog"));
            return dialog
                   && qWaitForScaled([dialog] { return dialog->property("opened").toBool(); },
                                     5000);
        }
        const QString name = QStringLiteral("settingsPage");
        const QString file = QStringLiteral("SettingsPage.qml");
        QVariantMap properties;
        properties.insert(QStringLiteral("settings"), QVariant::fromValue(&ui.settings));
        properties.insert(QStringLiteral("connection"), QVariant::fromValue(&ui.controller));
        QMetaObject::invokeMethod(root, "openSection", Q_ARG(QVariant, name), Q_ARG(QVariant, file),
                                  Q_ARG(QVariant, properties));
        if (!wait(name))
            return false;
        if (state != QLatin1String("settingsSection"))
            return true;
        // A category with two cards: Audio
        QQuickItem *page = ui.item(QStringLiteral("settingsPage"));
        return QMetaObject::invokeMethod(page, "selectSection",
                                         Q_ARG(QVariant, QStringLiteral("audio")))
               && qWaitForScaled(
                   [&] {
                       return page->property("activeSection").toString() == QLatin1String("audio");
                   },
                   5000);
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
        return qWaitForScaled([&] { return ui.controller.homeRows()->rowCount() > 0; }, 10000);
    if (state == QLatin1String("search")) {
        if (!ui.openSearchPage())
            return false;
        ui.type(QStringLiteral("abyss"));
        return qWaitForScaled(
            [&] { return ui.controller.search()->state() == Kinefin::SearchModel::Results; },
            10000);
    }
    if (state == QLatin1String("resume")) {
        if (!qWaitForScaled([&] { return ui.controller.homeRows()->rowCount() > 0; }, 10000))
            return false;
        QMetaObject::invokeMethod(root, "openResumeItems");
        return qWaitForScaled(
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
            if (!qWaitForScaled([&] { return ui.controller.queue()->count() == 5; }, 10000))
                return false;
        }
        const QVariantMap properties{
            {QStringLiteral("connection"), QVariant::fromValue(&ui.controller)}};
        QMetaObject::invokeMethod(root, "openSection", Q_ARG(QVariant, QStringLiteral("queuePage")),
                                  Q_ARG(QVariant, QStringLiteral("QueuePage.qml")),
                                  Q_ARG(QVariant, properties));
        return qWaitForScaled([&] { return ui.item(QStringLiteral("queuePage")) != nullptr; },
                              10000);
    }
    if (state == QLatin1String("tvTab")) {
        if (!qWaitForScaled(
                [&] { return ui.item(QStringLiteral("libraryCard_") + kSeries) != nullptr; }, 10000)
            || !ui.openLibraryTile(kSeries))
            return false;
        return qWaitForScaled(
            [&] {
                return ui.rootTab() == QLatin1String("tvshows")
                       && ui.controller.shows()->status() != Kinefin::LibraryItemsModel::Loading;
            },
            10000);
    }
    if (!qWaitForScaled(
            [&] { return ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr; }, 10000)
        || !ui.openLibraryTile(kMovies))
        return false;
    if (state == QLatin1String("moviesTwoLibraries"))
        return qWaitForScaled(
            [&] {
                return ui.controller.movies()->status() != Kinefin::LibraryItemsModel::Loading
                       && ui.item(QStringLiteral("librarySelector"))
                       && ui.item(QStringLiteral("librarySelector"))->isVisible();
            },
            10000);
    if (state == QLatin1String("browse")) {
        // The Favorites and Genres sub-tabs on, the list view, a status and a feature filter: every row of the page showing
        ui.settings.setValue(QStringLiteral("library"), QStringLiteral("tabs.movies.genres"), true);
        auto *movies = ui.controller.movies();
        movies->setViewMode(QStringLiteral("list"));
        movies->setFilterValues(
            QVariantMap{{QStringLiteral("played"), QStringLiteral("Unplayed")},
                        {QStringLiteral("features"), QStringList{QStringLiteral("Subtitles")}}});
        return qWaitForScaled(
            [&] {
                return movies->status() != Kinefin::LibraryItemsModel::Loading
                       && subtabButton(ui, QStringLiteral("genres")) != nullptr
                       && ui.item(QStringLiteral("filterChip_1")) != nullptr
                       && ui.item(QStringLiteral("libraryList")) != nullptr;
            },
            10000);
    }
    // The grid is built asynchronously after the first page, failed or not
    if (state == QLatin1String("library") || state == QLatin1String("libraryFailed"))
        return qWaitForScaled(
            [&] {
                return ui.controller.movies()->status() != Kinefin::LibraryItemsModel::Loading
                       && libraryViewBuilt(ui);
            },
            10000);
    if (state == QLatin1String("rails")) {
        if (!qWaitForScaled([&] { return ui.controller.movies()->genreOptions()->available(); },
                            10000))
            return false;
        QMetaObject::invokeMethod(
            root, "openGenres",
            Q_ARG(QVariant, QVariant::fromValue(ui.item(QStringLiteral("libraryPage")))));
        return qWaitForScaled(
            [&] { return ui.showsText(QStringLiteral("Could not load this genre"), false); },
            10000);
    }
    // details and detailsMissing: the first card of the grid
    QQuickItem *card = nullptr;
    const QString cardId = state == QLatin1String("details")
                               ? kMovieId
                               : QStringLiteral("c2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7");
    if (!qWaitForScaled(
            [&] {
                return (card = Ui::itemIn(ui.item(QStringLiteral("libraryPage")),
                                          QStringLiteral("mediaCard_") + cardId))
                       != nullptr;
            },
            10000))
        return false;
    ui.clickCenter(card);
    return wait(QStringLiteral("itemDetailsPage"))
           && qWaitForScaled(
               [&] {
                   return ui.showsText(state == QLatin1String("details")
                                           ? QStringLiteral("Cast and Crew")
                                           : QStringLiteral("This item is not available"));
               },
               10000);
}

// The button with this label inside `root` (an entry of the saved servers, the add-server card)
QQuickItem *buttonLabelled(QQuickItem *root, const QString &label)
{
    for (QQuickItem *candidate : allItems(root)) {
        if (candidate->inherits("QQuickAbstractButton")
            && candidate->property("text").toString() == label)
            return candidate;
    }
    return nullptr;
}

// Saves the fixture server with a session (the list holds the servers of saved users), then switches back to the Servers page with it listed
bool saveServerAndShowServers(Ui &ui)
{
    if (!ui.signIn())
        return false;
    ui.controller.cancel();
    return ui.reach(ConnectionController::Idle, QStringLiteral("serversPage"))
           && qWaitForScaled([&] { return ui.item(QStringLiteral("savedServerEntry")) != nullptr; },
                             5000)
           // The first cover of the connection is gone before the test starts clicking
           && qWaitForScaled([&] { return !ui.window->property("startupVisible").toBool(); }, 5000);
}

// The port of a server that is not listening any more
quint16 closedPort()
{
    QTcpServer closed;
    if (!closed.listen(QHostAddress::LocalHost))
        return 0;
    const quint16 port = closed.serverPort();
    closed.close();
    return port;
}
} // namespace

namespace {
// The pixels of a grid unit, read from the shell: its wide limit is 36 whole grid units
qreal gridUnitOf(const Ui &ui)
{
    return ui.window->property("wideLimit").toReal() / 36;
}

QRectF sceneRect(const QQuickItem *item)
{
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// The FormCards showing under root (a FormHeader has a maximum width too, but no delegates); the item spans the space its parent gives it, the card surface is inside
QList<QQuickItem *> visibleFormCards(QQuickItem *root)
{
    QList<QQuickItem *> cards;
    for (QQuickItem *candidate : allItems(root)) {
        if (candidate->isVisible() && candidate->width() > 0
            && candidate->property("delegates").isValid()
            && candidate->property("cardWidthRestricted").isValid())
            cards << candidate;
    }
    return cards;
}

// The surface of a FormCard is its shadowed rectangle (the only child with a border width), centred in the item up to the card's maximum width
QRectF cardSurface(const QQuickItem *card)
{
    for (const QQuickItem *child : card->childItems()) {
        if (child->property("borderWidth").isValid())
            return sceneRect(child);
    }
    return {};
}

// What is wrong with the form cards under root: a surface wider than the card's maximum width, or not centred in what its parent gives it. Empty when all is well
QStringList formCardProblems(QQuickItem *root)
{
    QStringList problems;
    for (const QQuickItem *card : visibleFormCards(root)) {
        const QRectF surface = cardSurface(card);
        const QRectF space = sceneRect(card->parentItem());
        if (surface.isEmpty())
            problems << QStringLiteral("no surface in %1").arg(Ui::describe(card));
        else if (surface.width() > card->property("maximumWidth").toReal() + 2)
            problems << QStringLiteral("card %1 is %2 px wide, the maximum is %3")
                            .arg(Ui::describe(card))
                            .arg(surface.width())
                            .arg(card->property("maximumWidth").toReal());
        else if (qAbs((surface.left() - space.left()) - (space.right() - surface.right())) > 1)
            problems << QStringLiteral("card %1 is off-centre: gaps %2 and %3")
                            .arg(Ui::describe(card))
                            .arg(surface.left() - space.left())
                            .arg(space.right() - surface.right());
    }
    return problems;
}

// The Settings page at this window width: the list of entries sits at 13 grid units beside the content in a wide window, and shows alone below the wide limit until an entry opens in place of it; the heading and the cards sit on one centred column at the FormCard default width. Empty when all is well
QStringList settingsFormProblems(const Ui &ui, int width)
{
    QStringList problems;
    const bool wide = width >= ui.window->property("wideLimit").toInt();
    const auto shown = [&](const QString &name) {
        const QQuickItem *entry = ui.item(name);
        return entry && entry->isVisible();
    };
    const QQuickItem *page = ui.item(QStringLiteral("settingsPage"));
    const bool entryOpen = page && page->property("entryOpen").toBool();
    const bool listShown = shown(QStringLiteral("settingsCategory_main"));
    const bool paneShown = shown(QStringLiteral("settingsContentView"));
    if (listShown != (wide || !entryOpen))
        problems << QStringLiteral(
                        "the list of entries is %1 at %2 px (wide mode from %3, entry open: %4)")
                        .arg(listShown ? QStringLiteral("showing") : QStringLiteral("hidden"))
                        .arg(width)
                        .arg(ui.window->property("wideLimit").toInt())
                        .arg(entryOpen);
    if (paneShown != (wide || entryOpen))
        problems << QStringLiteral("the content is %1 at %2 px (entry open: %3)")
                        .arg(paneShown ? QStringLiteral("showing") : QStringLiteral("hidden"))
                        .arg(width)
                        .arg(entryOpen);
    const QQuickItem *list = ui.item(QStringLiteral("settingsEntryList"));
    if (wide && list && qAbs(sceneRect(list).width() - 13 * gridUnitOf(ui)) > 1)
        problems << QStringLiteral("the list is %1 px wide, the sidebar width is 13 grid units")
                        .arg(sceneRect(list).width());
    if (!paneShown)
        return problems;
    QQuickItem *view = ui.item(QStringLiteral("settingsContentView"));
    const QQuickItem *heading = ui.item(QStringLiteral("settingsContentHeading"));
    const QList<QQuickItem *> cards = visibleFormCards(view);
    if (!heading || cards.isEmpty())
        return problems << QStringLiteral("the form is not showing");
    for (const QQuickItem *card : cards) {
        if (card->property("maximumWidth").toReal() > 30 * gridUnitOf(ui) + 0.5)
            problems << QStringLiteral("a card maximum is %1 px, over the default of 30 grid units")
                            .arg(card->property("maximumWidth").toReal());
    }
    problems << formCardProblems(view);
    const QRectF surface = cardSurface(cards.constFirst());
    if (qAbs(sceneRect(heading).left() - surface.left()) > 1)
        problems << QStringLiteral("the heading starts at %1 px, the card at %2")
                        .arg(sceneRect(heading).left())
                        .arg(surface.left());
    return problems;
}

// The columns of a grid view: at least one, and the cells fill the grid's usable width (without the page margin and the scroll bar, where the grid reserves them) up to a pixel per column
QStringList gridProblems(const QQuickItem *grid, qreal tileWidth)
{
    QStringList problems;
    const int columns = grid->property("columns").toInt();
    const qreal cells = columns * grid->property("cellWidth").toReal();
    const QVariant reserved = grid->property("usableWidth");
    const qreal usable = reserved.isValid() ? reserved.toReal() : grid->width();
    if (columns < 1)
        problems << QStringLiteral("%1 has %2 columns").arg(Ui::describe(grid)).arg(columns);
    if (tileWidth > 0 && columns != qMax(1, qFloor(usable / tileWidth)))
        problems << QStringLiteral("%1 has %2 columns for %3 px")
                        .arg(Ui::describe(grid))
                        .arg(columns)
                        .arg(usable);
    if (cells > usable || cells <= usable - columns - 1)
        problems << QStringLiteral("the cells of %1 cover %2 px of %3")
                        .arg(Ui::describe(grid))
                        .arg(cells)
                        .arg(usable);
    if (usable > grid->width())
        problems << QStringLiteral("%1 has %2 usable px of %3")
                        .arg(Ui::describe(grid))
                        .arg(usable)
                        .arg(grid->width());
    return problems;
}

// A block of reading content: capped at the shell's reading width and centred in its container
QStringList readingProblems(const Ui &ui, const QQuickItem *block, const QQuickItem *container)
{
    QStringList problems;
    const qreal reading = ui.window->property("readingWidth").toReal();
    // A delegate that spans its container keeps its side margins as insets: the visible part is what counts
    const QRectF rect = sceneRect(block).adjusted(block->property("leftInset").toReal(), 0,
                                                  -block->property("rightInset").toReal(), 0);
    const QRectF space = sceneRect(container);
    if (rect.width() > reading + 1)
        problems << QStringLiteral("%1 is %2 px wide, the reading width is %3")
                        .arg(Ui::describe(block))
                        .arg(rect.width())
                        .arg(reading);
    if (qAbs((rect.left() - space.left()) - (space.right() - rect.right())) > 1)
        problems << QStringLiteral("%1 is off-centre: gaps %2 and %3")
                        .arg(Ui::describe(block))
                        .arg(rect.left() - space.left())
                        .arg(space.right() - rect.right());
    return problems;
}

// What is wrong with the width rules of the page `state` shows at this window width, besides the layout problems every page is checked for. Empty when all is well
QStringList pageWidthProblems(const Ui &ui, const QString &state, int width)
{
    QStringList problems = ui.layoutProblems();
    const qreal unit = gridUnitOf(ui);
    if (state == QLatin1String("settings")) {
        problems << settingsFormProblems(ui, width);
    } else if (state == QLatin1String("preferences")) {
        // The User entry of Settings: the form in the content pane, the list folded as for any entry
        QQuickItem *form = ui.item(QStringLiteral("userPreferencesForm"));
        if (!form || visibleFormCards(form).isEmpty()) {
            problems << QStringLiteral("no form card showing");
        } else
            problems << formCardProblems(form) << settingsFormProblems(ui, width);
    } else if (state == QLatin1String("details")) {
        const QQuickItem *header = ui.item(QStringLiteral("detailsHeaderContent"));
        if (header)
            problems << readingProblems(ui, header, header->parentItem());
        else
            problems << QStringLiteral("the item header is not there");
    } else if (state == QLatin1String("queue")) {
        const QQuickItem *list = ui.item(QStringLiteral("queueList"));
        for (int row = 0; list; ++row) {
            const QQuickItem *entry = ui.item(QStringLiteral("queueRow_%1").arg(row));
            if (!entry)
                break;
            problems << readingProblems(ui, entry, list);
        }
        if (!ui.item(QStringLiteral("queueRow_0")))
            problems << QStringLiteral("no queue row showing");
    } else if (state == QLatin1String("favorites")) {
        for (const QString &name :
             {QStringLiteral("favoritesGrid"), QStringLiteral("favoritesGroupGrid")}) {
            if (const QQuickItem *grid = ui.item(name))
                problems << gridProblems(grid, 10 * unit);
        }
    } else if (state == QLatin1String("library")) {
        if (const QQuickItem *grid = ui.item(QStringLiteral("libraryGrid")))
            problems << gridProblems(grid, 0);
        else
            problems << QStringLiteral("the library grid is not there");
    }
    return problems;
}

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
        || !qWaitForScaled([dialog] { return !dialog->property("visible").toBool(); }, 5000))
        return false;
    return QMetaObject::invokeMethod(dialog, "open")
           && qWaitForScaled([dialog] { return dialog->property("opened").toBool(); }, 5000);
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

// The tags that leave no room in the query of `model` for another tag: long ones first, then ever shorter ones. The state fits (the filter editor would have taken it), a tag of any usual length does not
Kinefin::LibraryFilterState tagsFillingTheQuery(const Kinefin::LibraryItemsModel &model)
{
    Kinefin::LibraryFilterState state;
    const auto fits = [&](const Kinefin::LibraryFilterState &candidate) {
        return candidate.tags.size() <= Kinefin::MaxFilterValues
               && model.filtersFit(Kinefin::LibraryItemsModel::filtersToVariant(candidate));
    };
    int index = 0;
    for (const int length : {125, 100, 32, 4}) {
        while (true) {
            Kinefin::LibraryFilterState next = state;
            const QString padding(length,
                                  length == 125 ? QChar(0x00e9) : QChar(u'a' + length % 20));
            next.tags.append(padding
                             + QString::number(index++).rightJustified(3, QLatin1Char('0')));
            if (!fits(next))
                break;
            state = next;
        }
    }
    return state;
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
        const QString kind = row.value(QStringLiteral("kind")).toString();
        // The tag options sit in the list of the tags row, which is the one row to scroll to
        const bool match = facet.isEmpty() ? kind == QLatin1String("state")
                           : facet == QLatin1String("tags")
                               ? kind == QLatin1String("tags")
                               : row.value(QStringLiteral("facet")).toString() == facet
                                     && row.value(QStringLiteral("value")) == value;
        if (!match)
            continue;
        // The list places the row itself: the rows differ in height (the tags section is a list of its own), so a share of the content height can leave the row under the dialog's edge, where a click lands outside and closes it
        constexpr int ListViewCenter = 1;
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, int(i)),
                                  Q_ARG(int, ListViewCenter));
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

// The sub-tab entries of the library toolbar: the ids of its buttons that show, in the order of the toolbar (the main list has none)
QStringList subtabIds(const Ui &ui)
{
    QStringList ids;
    for (const QString &id :
         {QStringLiteral("suggestions"), QStringLiteral("favorites"), QStringLiteral("collections"),
          QStringLiteral("upcoming"), QStringLiteral("genres"), QStringLiteral("networks"),
          QStringLiteral("episodes")}) {
        if (subtabButton(ui, id))
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
    (void)qWaitForScaled(
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

// Clicks the toolbar button of a sub-tab (a checked one leads back to the main list). A button of a toolbar that was just created or shown is laid out a moment later, so a click at the spot it has at first could land beside it: the button is clicked by itself
bool selectSubtab(const Ui &ui, const QString &id)
{
    QQuickItem *button = nullptr;
    if (!qWaitForScaled([&] { return (button = subtabButton(ui, id)) != nullptr; }, 5000))
        return false;
    return QMetaObject::invokeMethod(button, "click");
}

// A Qt Quick Controls Popup may put its visual content in a separate popup window;
// walking the application window's contentItem can then see the header tabs but
// none of the dialog's buttons. The footer property points to the actual item
// regardless of the popup's rendering mode.
QList<QQuickItem *> filterFooterItems(const Ui &ui)
{
    const QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
    return dialog ? allItems(dialog->property("footer").value<QQuickItem *>())
                  : QList<QQuickItem *>{};
}

// Click a visible, enabled button in the dialog footer, not the action itself.
// Matching the label as well as the action handles Kirigami versions that do
// not forward customFooterActions to QQuickAbstractButton.action.
bool clickActionButton(const Ui &ui, const QString &actionName)
{
    QObject *action = ui.popup(actionName);
    if (!action)
        return false;
    const QString label = action->property("text").toString();
    for (QQuickItem *button : filterFooterItems(ui)) {
        if (!button->inherits("QQuickAbstractButton") || !button->isVisible()
            || !button->isEnabled() || button->width() <= 0 || button->height() <= 0
            || Ui::clippedAway(button))
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
    return qWaitForScaled([&] { return clickActionButton(ui, actionName); }, 5000);
}

// Gives the focus to the first control of the window header, away from the page
bool focusOutsideThePage(const Ui &ui)
{
    for (QQuickItem *item : allItems(ui.shellHeader())) {
        if (item->activeFocusOnTab() && item->isVisible() && item->isEnabled()) {
            item->forceActiveFocus(Qt::OtherFocusReason);
            return item->hasActiveFocus();
        }
    }
    return false;
}

// Report the buttons in the actual footer, including buttons without a bound
// action, rather than unrelated toolbar buttons in the application window.
QString describeButtons(const Ui &ui)
{
    QStringList parts;
    for (QQuickItem *button : filterFooterItems(ui)) {
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
    return parts.isEmpty() ? QStringLiteral("no abstract button exists in the filter dialog footer")
                           : parts.join(QStringLiteral("; "));
}

// The first row of cards of the library grid, left to right (scene coordinates), with the grid, its scroll bar and its gutter
struct CardGridGeometry {
    QList<QRectF> firstRow;
    QRectF scrollBar;
    QRectF grid;
    qreal gutter = 0;
};

CardGridGeometry cardGridGeometry(Ui &ui)
{
    CardGridGeometry geometry;
    QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
    QQuickItem *bar = ui.item(QStringLiteral("libraryGridScrollBar"));
    if (!grid || !bar)
        return geometry;
    geometry.gutter = grid->property("gutter").toReal();
    geometry.grid = QRectF(grid->mapToScene(QPointF(0, 0)), QSizeF(grid->width(), grid->height()));
    geometry.scrollBar =
        QRectF(bar->mapToScene(QPointF(0, 0)), QSizeF(bar->width(), bar->height()));
    QList<QRectF> cards;
    const QList<QQuickItem *> items = ui.itemsStartingWith(QStringLiteral("mediaCard_"));
    for (QQuickItem *card : items) {
        if (card->isVisible() && card->isEnabled())
            cards.append(
                QRectF(card->mapToScene(QPointF(0, 0)), QSizeF(card->width(), card->height())));
    }
    std::sort(cards.begin(), cards.end(), [](const QRectF &a, const QRectF &b) {
        return a.y() < b.y() || (qFuzzyCompare(a.y() + 1, b.y() + 1) && a.x() < b.x());
    });
    for (const QRectF &card : std::as_const(cards)) {
        if (!qFuzzyCompare(card.y() + 1, cards.constFirst().y() + 1))
            break;
        geometry.firstRow.append(card);
    }
    return geometry;
}
} // namespace

// A stand-in for the player of the window: it records what the shell asked it to play and refuses, so no player page opens
class QuickPlayStub : public QObject {
    Q_OBJECT
  public:
    struct Call {
        QString id;
        qint64 resumeTicks;
    };
    Q_INVOKABLE bool playItem(const QString &id, qint64 resumeTicks)
    {
        calls.push_back({id, resumeTicks});
        return false;
    }
    QList<Call> calls;
};

// What the player page reads and calls, without mpv (the video item takes only the real controller and stays empty): the calls are recorded by name and argument
// The SDL3 gamepads of the shell, as far as the shortcuts dialog and the commands read them
class GamepadStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(int connectedCount MEMBER connectedCount NOTIFY changed)
    Q_PROPERTY(QStringList faceLabels MEMBER faceLabels NOTIFY changed)
  public:
    int connectedCount = 0;
    QStringList faceLabels = {QStringLiteral("Cross"), QStringLiteral("Circle"),
                              QStringLiteral("Square"), QStringLiteral("Triangle")};

  Q_SIGNALS:
    void changed();
};

class PlayerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active MEMBER active NOTIFY changed)
    Q_PROPERTY(bool loading MEMBER loading NOTIFY changed)
    Q_PROPERTY(bool paused MEMBER paused NOTIFY changed)
    Q_PROPERTY(QString title MEMBER title NOTIFY changed)
    Q_PROPERTY(QString subtitle MEMBER subtitle NOTIFY changed)
    Q_PROPERTY(QString errorText MEMBER errorText NOTIFY changed)
    Q_PROPERTY(bool canRetry MEMBER canRetry NOTIFY changed)
    Q_PROPERTY(qint64 positionTicks MEMBER positionTicks NOTIFY changed)
    Q_PROPERTY(qint64 durationTicks MEMBER durationTicks NOTIFY changed)
    Q_PROPERTY(bool canSeek MEMBER canSeek NOTIFY changed)
    Q_PROPERTY(double rate MEMBER rate NOTIFY changed)
    Q_PROPERTY(int volume MEMBER volume NOTIFY changed)
    Q_PROPERTY(bool muted MEMBER muted NOTIFY changed)
    Q_PROPERTY(double audioDelay MEMBER audioDelay NOTIFY changed)
    Q_PROPERTY(double subtitleDelay MEMBER subtitleDelay NOTIFY changed)
    Q_PROPERTY(bool shuffle MEMBER shuffle NOTIFY changed)
    Q_PROPERTY(int repeatMode MEMBER repeatMode NOTIFY changed)
    Q_PROPERTY(int currentIndex MEMBER currentIndex NOTIFY changed)
    Q_PROPERTY(int queueCount MEMBER queueCount NOTIFY changed)
    Q_PROPERTY(bool hasNext MEMBER hasNext NOTIFY changed)
    Q_PROPERTY(bool hasPrevious MEMBER hasPrevious NOTIFY changed)
    Q_PROPERTY(QVariantList audioTracks MEMBER audioTracks NOTIFY changed)
    Q_PROPERTY(QVariantList subtitleTracks MEMBER subtitleTracks NOTIFY changed)
    Q_PROPERTY(QVariantList externalSubtitles MEMBER externalSubtitles NOTIFY changed)
    Q_PROPERTY(QString previewImage MEMBER previewImage NOTIFY changed)
    Q_PROPERTY(QString screenshotPath MEMBER screenshotPath NOTIFY changed)
    Q_PROPERTY(QString skipLabel MEMBER skipLabel NOTIFY changed)
    Q_PROPERTY(int sleepRemainingMinutes MEMBER sleepRemainingMinutes NOTIFY changed)
    Q_PROPERTY(bool sleepAtEndOfItem MEMBER sleepAtEndOfItem NOTIFY changed)
    Q_PROPERTY(bool sleepAtEndOfQueue MEMBER sleepAtEndOfQueue NOTIFY changed)
    Q_PROPERTY(bool casting MEMBER casting NOTIFY changed)
    Q_PROPERTY(QString castTargetName MEMBER castTargetName NOTIFY changed)
    Q_PROPERTY(bool castCanPause MEMBER castCanPause NOTIFY changed)
    Q_PROPERTY(QString castTargetId MEMBER castTargetId NOTIFY changed)
    Q_PROPERTY(bool castHasVolume MEMBER castHasVolume NOTIFY changed)
    Q_PROPERTY(bool canContinueHere MEMBER canContinueHere NOTIFY changed)
    Q_PROPERTY(bool localPlayback MEMBER localPlayback NOTIFY changed)
    Q_PROPERTY(QObject *cast MEMBER cast NOTIFY changed)
    Q_PROPERTY(QString aspectMode MEMBER aspectMode NOTIFY changed)
    Q_PROPERTY(bool nightMode MEMBER nightMode NOTIFY changed)
    Q_PROPERTY(QString clipLine MEMBER clipLine NOTIFY changed)
    Q_PROPERTY(QString clipReason MEMBER clipReason NOTIFY changed)
    Q_PROPERTY(QString clipLengthReason MEMBER clipLengthReason NOTIFY changed)
    Q_PROPERTY(bool clipVideoEnabled MEMBER clipVideoEnabled NOTIFY changed)
    Q_PROPERTY(bool clipGifEnabled MEMBER clipGifEnabled NOTIFY changed)
    Q_PROPERTY(bool clipSaving MEMBER clipSaving NOTIFY changed)
    Q_PROPERTY(bool clipSavingGif MEMBER clipSavingGif NOTIFY changed)
    Q_PROPERTY(double clipProgress MEMBER clipProgress NOTIFY changed)
    Q_PROPERTY(qint64 clipStartTicks MEMBER clipStartTicks NOTIFY changed)
    Q_PROPERTY(qint64 clipEndTicks MEMBER clipEndTicks NOTIFY changed)
    Q_PROPERTY(qint64 manualQualityCap MEMBER manualQualityCap NOTIFY changed)
    Q_PROPERTY(bool qualityAvailable MEMBER qualityAvailable NOTIFY changed)
    Q_PROPERTY(bool qualitySwitching MEMBER qualitySwitching NOTIFY changed)
    Q_PROPERTY(QVariantList qualitySteps MEMBER qualitySteps NOTIFY changed)
    Q_PROPERTY(QVariantList chapters MEMBER chapters NOTIFY chaptersChanged)
    Q_PROPERTY(int currentChapter MEMBER currentChapter NOTIFY changed)
    Q_PROPERTY(bool upNextShown MEMBER upNextShown NOTIFY changed)
    Q_PROPERTY(QVariantMap upNext MEMBER upNext NOTIFY changed)
    Q_PROPERTY(int upNextSeconds MEMBER upNextSeconds NOTIFY changed)
    Q_PROPERTY(bool hasSeriesChoices MEMBER hasSeriesChoices NOTIFY changed)
    Q_PROPERTY(bool canManageSubtitles MEMBER canManageSubtitles NOTIFY changed)
    Q_PROPERTY(QVariantList remoteSubtitles MEMBER remoteSubtitles NOTIFY remoteSubtitlesChanged)
    Q_PROPERTY(
        bool remoteSubtitleSearching MEMBER remoteSubtitleSearching NOTIFY remoteSubtitlesChanged)
    Q_PROPERTY(QString remoteSubtitleError MEMBER remoteSubtitleError NOTIFY remoteSubtitlesChanged)
  public:
    bool canManageSubtitles = false;
    QVariantList remoteSubtitles;
    bool remoteSubtitleSearching = false;
    QString remoteSubtitleError;
    QStringList remoteSearches;
    QStringList remoteDownloads;
    Q_INVOKABLE bool searchRemoteSubtitles(const QString &language)
    {
        remoteSearches << language;
        return true;
    }
    Q_INVOKABLE bool downloadRemoteSubtitle(const QString &subtitleId)
    {
        remoteDownloads << subtitleId;
        return true;
    }
    Q_INVOKABLE bool playItem(const QString &id, qint64)
    {
        record(QStringLiteral("playItem"), id);
        if (playRefused)
            return false;
        active = true;
        Q_EMIT changed();
        return true;
    }
    Q_INVOKABLE bool playQueue(int index, qint64 startTicks = 0)
    {
        lastStartTicks = startTicks;
        return record(QStringLiteral("playQueue"), index);
    }
    Q_INVOKABLE bool selectAudioStreamIndex(int index)
    {
        return record(QStringLiteral("selectAudioStreamIndex"), index);
    }
    Q_INVOKABLE bool selectSubtitleStreamIndex(int index)
    {
        return record(QStringLiteral("selectSubtitleStreamIndex"), index);
    }
    Q_INVOKABLE void togglePause()
    {
        record(QStringLiteral("togglePause"));
        if (pauseChangesNothing)
            return;
        paused = !paused;
        Q_EMIT changed();
    }
    Q_INVOKABLE void pause()
    {
        record(QStringLiteral("pause"));
        paused = true;
        Q_EMIT changed();
    }
    Q_INVOKABLE void resume()
    {
        record(QStringLiteral("resume"));
        paused = false;
        Q_EMIT changed();
    }
    Q_INVOKABLE void toggleMute() { record(QStringLiteral("toggleMute")); }
    Q_INVOKABLE bool retryPlayback() { return record(QStringLiteral("retryPlayback")); }
    Q_INVOKABLE void stop()
    {
        record(QStringLiteral("stop"));
        active = false;
        Q_EMIT changed();
    }
    Q_INVOKABLE bool next() { return record(QStringLiteral("next")); }
    Q_INVOKABLE bool previous() { return record(QStringLiteral("previous")); }
    Q_INVOKABLE void seek(qint64 ticks) { record(QStringLiteral("seek"), ticks); }
    Q_INVOKABLE void seekRelative(int seconds) { record(QStringLiteral("seekRelative"), seconds); }
    Q_INVOKABLE void setRate(double value) { record(QStringLiteral("setRate"), value); }
    Q_INVOKABLE void setVolume(int level)
    {
        record(QStringLiteral("setVolume"), level);
        volume = level;
        Q_EMIT changed();
    }
    Q_INVOKABLE void setShuffle(bool on) { record(QStringLiteral("setShuffle"), on); }
    Q_INVOKABLE void setRepeatMode(int mode) { record(QStringLiteral("setRepeatMode"), mode); }
    Q_INVOKABLE void setAspectMode(const QString &mode)
    {
        record(QStringLiteral("setAspectMode"), mode);
        aspectMode = mode;
        Q_EMIT changed();
    }
    Q_INVOKABLE void setNightMode(bool enabled)
    {
        record(QStringLiteral("setNightMode"), enabled);
        nightMode = enabled;
        Q_EMIT changed();
    }
    Q_INVOKABLE void markClipStart()
    {
        record(QStringLiteral("markClipStart"));
        clipStartTicks = positionTicks;
        Q_EMIT changed();
    }
    Q_INVOKABLE void markClipEnd()
    {
        record(QStringLiteral("markClipEnd"));
        clipEndTicks = positionTicks;
        Q_EMIT changed();
    }
    Q_INVOKABLE void clearClip()
    {
        record(QStringLiteral("clearClip"));
        clipStartTicks = -1;
        clipEndTicks = -1;
        Q_EMIT changed();
    }
    Q_INVOKABLE void cancelClip() { record(QStringLiteral("cancelClip")); }
    Q_INVOKABLE void setQuality(qint64 cap)
    {
        record(QStringLiteral("setQuality"), cap);
        manualQualityCap = cap;
        Q_EMIT changed();
    }
    Q_INVOKABLE void setQualityBitrate(qint64 bitsPerSecond)
    {
        record(QStringLiteral("setQualityBitrate"), bitsPerSecond);
    }
    Q_INVOKABLE void saveClip(bool gif, const QUrl &destination)
    {
        record(QStringLiteral("saveClip"), gif ? QStringLiteral("gif") : QStringLiteral("video"));
        Q_UNUSED(destination);
    }
    Q_INVOKABLE QUrl suggestedClipUrl(bool gif) const
    {
        return QUrl::fromLocalFile(
            QStringLiteral("/tmp/")
            + (gif ? QStringLiteral("clip.gif") : QStringLiteral("clip.webm")));
    }
    Q_INVOKABLE QVariantMap playbackInfo() const { return info; }
    Q_INVOKABLE int chapterAt(qint64 ticks) const
    {
        int index = -1;
        for (int i = 0; i < chapters.size(); ++i) {
            if (chapters.at(i).toMap().value(QStringLiteral("startTicks")).toLongLong() <= ticks)
                index = i;
        }
        return index;
    }
    Q_INVOKABLE void jumpToChapter(int index) { record(QStringLiteral("jumpToChapter"), index); }
    Q_INVOKABLE void playUpNext() { record(QStringLiteral("playUpNext")); }
    Q_INVOKABLE void forgetSeriesChoices() { record(QStringLiteral("forgetSeriesChoices")); }
    Q_INVOKABLE void cancelUpNext()
    {
        record(QStringLiteral("cancelUpNext"));
        upNextShown = false;
        Q_EMIT changed();
    }
    Q_INVOKABLE void stepChapter(int direction)
    {
        record(QStringLiteral("stepChapter"), direction);
    }
    Q_INVOKABLE void selectAudioTrack(int id) { record(QStringLiteral("selectAudioTrack"), id); }
    Q_INVOKABLE void selectSubtitleTrack(int id)
    {
        record(QStringLiteral("selectSubtitleTrack"), id);
    }
    Q_INVOKABLE void selectSecondarySubtitleTrack(int id)
    {
        record(QStringLiteral("selectSecondarySubtitleTrack"), id);
    }
    Q_INVOKABLE bool downloadSecondarySubtitle(int index)
    {
        return record(QStringLiteral("downloadSecondarySubtitle"), index);
    }
    Q_INVOKABLE void adjustAudioDelay(double s) { record(QStringLiteral("adjustAudioDelay"), s); }
    Q_INVOKABLE void adjustSubtitleDelay(double s)
    {
        record(QStringLiteral("adjustSubtitleDelay"), s);
    }
    Q_INVOKABLE void setSleepTimer(int minutes)
    {
        record(QStringLiteral("setSleepTimer"), minutes);
        if (minutes == 0) {
            sleepAtEndOfItem = false;
            sleepAtEndOfQueue = false;
        }
        Q_EMIT changed();
    }
    Q_INVOKABLE void setSleepAtEndOfItem(bool enabled)
    {
        record(QStringLiteral("setSleepAtEndOfItem"), enabled);
        sleepAtEndOfItem = enabled;
        if (enabled)
            sleepAtEndOfQueue = false;
        Q_EMIT changed();
    }
    Q_INVOKABLE void setSleepAtEndOfQueue(bool enabled)
    {
        record(QStringLiteral("setSleepAtEndOfQueue"), enabled);
        sleepAtEndOfQueue = enabled;
        if (enabled)
            sleepAtEndOfItem = false;
        Q_EMIT changed();
    }
    Q_INVOKABLE void stopCasting()
    {
        record(QStringLiteral("stopCasting"));
        casting = false;
        Q_EMIT changed();
    }
    Q_INVOKABLE bool castTo(const QString &udn)
    {
        record(QStringLiteral("castTo"), udn);
        return !castRefused;
    }
    Q_INVOKABLE bool playItemOn(const QString &udn, const QString &id, qint64 resumeTicks)
    {
        const QString call =
            udn + QLatin1Char(' ') + id + QLatin1Char(' ') + QString::number(resumeTicks);
        record(QStringLiteral("playItemOn"), call);
        return !castRefused;
    }
    Q_INVOKABLE void screenshot() { record(QStringLiteral("screenshot")); }
    Q_INVOKABLE void skipSegment() { record(QStringLiteral("skipSegment")); }
    Q_INVOKABLE bool downloadSubtitle(int index)
    {
        return record(QStringLiteral("downloadSubtitle"), index);
    }
    Q_INVOKABLE void previewAt(qint64 ticks) { record(QStringLiteral("previewAt"), ticks); }
    Q_INVOKABLE void clearPreview() {}
    Q_INVOKABLE QString timeText(qint64 ticks) const { return QString::number(ticks / 10000000); }

    bool active = false;
    bool loading = false;
    bool paused = false;
    // playItem() refuses, as it does for an item the server no longer offers
    bool playRefused = false;
    // A toggle the player cannot honour, as before anything plays
    bool pauseChangesNothing = false;
    QString title = QStringLiteral("Abyss Station");
    QString subtitle = QStringLiteral("S2:E6 — Weightless");
    QString errorText;
    bool canRetry = false;
    qint64 positionTicks = 0;
    qint64 durationTicks = 72000000000;
    bool canSeek = true;
    double rate = 1;
    int volume = 100;
    bool muted = false;
    double audioDelay = 0;
    double subtitleDelay = 0;
    bool shuffle = false;
    int repeatMode = 0;
    int currentIndex = -1;
    int queueCount = 0;
    bool hasNext = false;
    bool hasPrevious = false;
    QVariantList audioTracks;
    QVariantList subtitleTracks;
    QVariantList externalSubtitles;
    QString previewImage;
    QString screenshotPath;
    QString skipLabel;
    int sleepRemainingMinutes = 0;
    bool sleepAtEndOfItem = false;
    bool sleepAtEndOfQueue = false;
    bool casting = false;
    QString castTargetName;
    bool castCanPause = false;
    QString castTargetId;
    bool castHasVolume = false;
    bool canContinueHere = false;
    bool localPlayback = false;
    // castTo() and playItemOn() refuse, as they do while following a group or for an unknown device
    bool castRefused = false;
    QObject *cast = nullptr;
    QString aspectMode = QStringLiteral("normal");
    bool nightMode = false;
    QString clipLine;
    QString clipReason;
    QString clipLengthReason;
    bool clipVideoEnabled = false;
    bool clipGifEnabled = false;
    bool clipSaving = false;
    bool clipSavingGif = false;
    double clipProgress = 0;
    qint64 clipStartTicks = -1;
    qint64 clipEndTicks = -1;
    qint64 manualQualityCap = 0;
    bool qualityAvailable = false;
    bool qualitySwitching = false;
    QVariantList qualitySteps;
    QVariantMap info;
    QVariantList chapters;
    int currentChapter = -1;
    bool upNextShown = false;
    QVariantMap upNext;
    int upNextSeconds = 0;
    bool hasSeriesChoices = false;
    QStringList calls;
    // The start position of the last playQueue
    qint64 lastStartTicks = 0;

  Q_SIGNALS:
    void changed();
    void chaptersChanged();
    void remoteSubtitlesChanged();
    void remoteSubtitleDownloaded(const QString &subtitleId);
    void remoteSubtitleDownloadFailed(const QString &subtitleId, int httpStatus);
    void remoteSubtitleNotSaved(const QString &subtitleId);
    void qualityLowered(qint64 bitsPerSecond);
    // The clip notifications of Main.qml; the stub never emits them, but the shell's Connections must match
    void clipSaved(const QString &fileName, bool gif);
    void clipSaveFailed(bool noSpace);
    void qualityChangeFailed();
    void clipMarkChanged(const QString &text);
    void qualityUnavailable();
    // The device stopped on its own or another controller took it
    void castStoppedElsewhere(const QString &deviceName);
    void finished();

  private:
    bool record(const QString &name, const QVariant &argument = {})
    {
        calls.append(argument.isValid() ? name + QLatin1Char(' ') + argument.toString() : name);
        return true;
    }
};

// The devices of the cast dialog, with the roles of the real targets model (udn, name, detail, kind)
class CastTargetsStub : public QAbstractListModel {
    Q_OBJECT
  public:
    struct Target {
        QString udn;
        QString name;
        QString detail;
    };
    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(targets.size());
    }
    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= targets.size())
            return {};
        const Target &target = targets.at(index.row());
        switch (role) {
        case Qt::UserRole + 1:
            return target.udn;
        case Qt::UserRole + 2:
            return target.name;
        case Qt::UserRole + 3:
            return target.detail;
        case Qt::UserRole + 4:
            return QStringLiteral("renderer");
        default:
            return {};
        }
    }
    QHash<int, QByteArray> roleNames() const override
    {
        return {{Qt::UserRole + 1, "udn"},
                {Qt::UserRole + 2, "name"},
                {Qt::UserRole + 3, "detail"},
                {Qt::UserRole + 4, "kind"}};
    }
    void setTargets(const QList<Target> &list)
    {
        beginResetModel();
        targets = list;
        endResetModel();
    }
    Q_INVOKABLE int indexOf(const QString &udn) const
    {
        for (int row = 0; row < targets.size(); ++row) {
            if (targets.at(row).udn == udn)
                return row;
        }
        return -1;
    }
    QList<Target> targets;
};

// The cast controller of the player as far as the dialog reads it: the devices, the search state and the error
class CastStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *targets MEMBER targets CONSTANT)
    Q_PROPERTY(bool searching MEMBER searching NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
  public:
    // A search runs until the test ends it, as the real one does for its window
    Q_INVOKABLE void search()
    {
        ++searches;
        searching = searchStarts;
        Q_EMIT changed();
    }
    Q_INVOKABLE void stopSearch()
    {
        ++stops;
        searching = false;
        Q_EMIT changed();
    }

    CastTargetsStub model;
    QAbstractItemModel *targets = &model;
    bool searching = false;
    // False: the search cannot start, as on a host without a usable interface
    bool searchStarts = true;
    QString error;
    int searches = 0;
    int stops = 0;

  Q_SIGNALS:
    void changed();
};

// The focus ring of a card or a row is the border of its background, drawn for the highlighted one and for a visually focused one
static bool cardRingShown(QQuickItem *card)
{
    auto *background = card ? card->property("background").value<QQuickItem *>() : nullptr;
    return background && background->property("ring").toBool()
           && QQmlProperty(background, QStringLiteral("border.width")).read().toInt() == 2;
}

// The rows of the cast dialog's device list: the delegates that carry a device's `udn`
static QList<QQuickItem *> castDeviceRows(QQuickItem *list)
{
    QList<QQuickItem *> rows;
    for (QQuickItem *candidate : allItems(list)) {
        if (candidate->inherits("QQuickAbstractButton") && candidate->property("udn").isValid())
            rows.append(candidate);
    }
    return rows;
}

// The remote-control object the executable gives the shell
class RemoteStub : public QObject {
    Q_OBJECT
  Q_SIGNALS:
    void actionRequested(const QString &action);
};

// Every value a QML property takes, taken from its notify signal: a poll misses a motion that a stalled thread finishes in one tick, and the animators of the render thread write only their end value back
class PropertyRecorder : public QObject {
    Q_OBJECT
  public:
    PropertyRecorder(QObject *source, const char *name)
        : m_property(source, QString::fromLatin1(name))
    {
        m_property.connectNotifySignal(this, SLOT(record()));
    }
    QList<qreal> values;

  public Q_SLOTS:
    void record() { values.push_back(m_property.read().toReal()); }

  private:
    QQmlProperty m_property;
};

// Runs a callback each time a QML property notifies a change, for a state that must hold at that very moment
class NotifyHook : public QObject {
    Q_OBJECT
  public:
    NotifyHook(QObject *source, const char *name, std::function<void()> callback)
        : m_callback(std::move(callback))
    {
        QQmlProperty(source, QString::fromLatin1(name)).connectNotifySignal(this, SLOT(fire()));
    }

  public Q_SLOTS:
    void fire() { m_callback(); }

  private:
    std::function<void()> m_callback;
};

// The card of the fixture movie in a Home row (0: Continue Watching, 1: Next Up)
QQuickItem *homeRowMovieCard(const Ui &ui, int row)
{
    QQuickItem *list = ui.item(QStringLiteral("homeRowList_") + QString::number(row));
    return list ? Ui::itemIn(list, QStringLiteral("mediaCard_") + kMovieId) : nullptr;
}

// A point over the poster of a card, in window coordinates
QPoint cardPoint(const QQuickItem *card)
{
    return card->mapToScene(QPointF(card->width() / 2, card->height() / 4)).toPoint();
}

// Scrolls the page in front until the poster of the card is inside the window: the Home rows below the tiles start out of view
bool revealCard(const Ui &ui, QQuickItem *card)
{
    QQuickItem *page = ui.currentDetails();
    auto *flickable = page ? page->property("flickable").value<QQuickItem *>() : nullptr;
    if (!flickable)
        return false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < scaled(8000)) {
        // Inside the page's own view, not only the window: a card under the header gets no pointer events
        const QRectF view = flickable->mapRectToScene(flickable->boundingRect());
        if (view.contains(cardPoint(card))
            && view.contains(card->mapToScene(QPointF(card->width() / 2, card->height() / 2))))
            return true;
        const qreal itemTop = card->mapToScene(QPointF(0, 0)).y();
        const qreal viewTop = flickable->mapToScene(QPointF(0, 0)).y();
        flickable->setProperty("contentY",
                               flickable->property("contentY").toReal() + itemTop - viewTop - 50);
        QTest::qWait(100);
    }
    return false;
}

namespace {
// Plays the fixture movie through the shell on a stub player; the player page once it is in front and its focus is set, or null. The stub must outlive the window, so it is declared before the Ui
QQuickItem *openPlayer(Ui &ui, PlayerStub &stub)
{
    QObject *root = ui.engine.rootObjects().constFirst();
    QVariant shown;
    if (!root->setProperty("player", QVariant::fromValue<QObject *>(&stub))
        || !QMetaObject::invokeMethod(root, "playItem", Q_RETURN_ARG(QVariant, shown),
                                      Q_ARG(QVariant, kMovieId), Q_ARG(QVariant, 0))
        || !shown.toBool())
        return nullptr;
    QQuickItem *page = nullptr;
    const bool ready = qWaitForScaled([&] {
        page = ui.currentDetails();
        return page && page->objectName() == QStringLiteral("playerPage")
               && page->property("isShowing").toBool() && ui.window->activeFocusItem() == page;
    });
    return ready ? page : nullptr;
}

const QString kSyncPlayGroup = QStringLiteral("5f1e2d3c4b5a69788796a5b4c3d2e1f0");

// A SyncPlay group update of the fixture group, as the server's socket sends it
QString syncPlayUpdate(const QString &type, const QJsonValue &payload,
                       const QString &group = kSyncPlayGroup)
{
    return QString::fromUtf8(
        QJsonDocument(
            QJsonObject{{QStringLiteral("MessageType"), QStringLiteral("SyncPlayGroupUpdate")},
                        {QStringLiteral("Data"), QJsonObject{{QStringLiteral("GroupId"), group},
                                                             {QStringLiteral("Type"), type},
                                                             {QStringLiteral("Data"), payload}}}})
            .toJson(QJsonDocument::Compact));
}

// The shell's session in the fixture group: the request, then the server's GroupJoined on the socket
bool syncPlayJoined(Ui &ui)
{
    if (!qWaitForScaled([&] { return ui.syncPlay.available() && ui.server.lastConnection(); }, 5000)
        || !ui.syncPlay.joinGroup(kSyncPlayGroup))
        return false;
    ui.server.lastConnection()->sendText(
        QString::fromUtf8(fixture(QStringLiteral("ws_syncplay_group_joined"))));
    return qWaitForScaled(
        [&] { return ui.syncPlay.status() == Kinefin::SyncPlayController::Status::Joined; });
}

// The SyncPlay dialog through its menu entry, once the socket is up; null when it does not open
QObject *openSyncPlayDialog(Ui &ui)
{
    QObject *action = ui.shellAction(QStringLiteral("syncPlayAction"));
    if (!action || !qWaitForScaled([&] { return action->property("visible").toBool(); }, 5000)
        || !QMetaObject::invokeMethod(action, "trigger"))
        return nullptr;
    QObject *dialog = ui.popup(QStringLiteral("syncPlayDialog"));
    if (!dialog || !qWaitForScaled([&] { return dialog->property("opened").toBool(); }))
        return nullptr;
    return dialog;
}

// The body of the last request of `route` the fake saw (a clock Ping may come after it)
QJsonObject lastSyncPlayBody(const Ui &ui, const QString &route)
{
    for (qsizetype i = ui.server.syncPlayRequests.size() - 1; i >= 0; --i) {
        if (ui.server.syncPlayRequests.at(i).route == route)
            return ui.server.syncPlayRequests.at(i).body;
    }
    return {};
}

int syncPlayRequests(const Ui &ui, const QString &route)
{
    return int(std::count_if(ui.server.syncPlayRequests.cbegin(), ui.server.syncPlayRequests.cend(),
                             [&route](const auto &request) { return request.route == route; }));
}

// A chapter as the player lists it
QVariantMap playerChapter(const QString &name, qint64 seconds, const QString &time)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("startTicks"), seconds * 10'000'000},
            {QStringLiteral("time"), time}};
}

// A mouse click whose events carry `timestamp` (ms): QTest moves the time on after every click so that two clicks never make a double one, and a slow double click needs its own times
void timedClick(QWindow *window, const QPoint &pos, quint64 timestamp)
{
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, pos, window->mapToGlobal(pos), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        event.setTimestamp(timestamp);
        QCoreApplication::sendEvent(window, &event);
    }
}

// The mini player once it shows, or null
QQuickItem *shownMiniPlayer(const Ui &ui)
{
    QQuickItem *mini = ui.item(QStringLiteral("miniPlayer"));
    return mini && mini->isVisible() ? mini : nullptr;
}

// Puts the pointer over the middle of the mini player: two moves, as in restPointerOnVideo
void hoverMiniPlayer(const Ui &ui, const QQuickItem *mini)
{
    const QPoint middle =
        mini->mapToScene(QPointF(mini->width() / 2, mini->height() / 2)).toPoint();
    QTest::mouseMove(ui.window, middle + QPoint(1, 1));
    QTest::mouseMove(ui.window, middle);
}

// Leaves the full player for the mini player through the first entry of More Actions, as a user does; false when the menu or the entry is missing
bool chooseShowMiniPlayer(Ui &ui)
{
    // The page in front only: a player page of an earlier visit may still be on its way out
    QQuickItem *page = ui.currentDetails();
    if (!page || page->objectName() != QLatin1String("playerPage"))
        return false;
    QObject *menu = page->findChild<QObject *>(QStringLiteral("playerMoreMenu"));
    QQuickItem *more = Ui::itemIn(page, QStringLiteral("playerMoreButton"));
    if (!menu || !more)
        return false;
    ui.clickCenter(more);
    QQuickItem *entry = nullptr;
    if (!qWaitForScaled([&] {
            entry = menu->findChild<QQuickItem *>(QStringLiteral("playerMoreMiniItem"));
            return menu->property("opened").toBool() && entry && entry->isVisible()
                   && entry->width() > 0;
        }))
        return false;
    ui.clickCenter(entry);
    return true;
}

// Plays the fixture movie, leaves the player by the Show Mini Player entry of More Actions and waits for the mini player; it, or null
QQuickItem *openMiniPlayer(Ui &ui, PlayerStub &stub)
{
    QQuickItem *page = openPlayer(ui, stub);
    if (!page || !chooseShowMiniPlayer(ui))
        return nullptr;
    QQuickItem *mini = nullptr;
    // Shown and at rest: it fades in rising a little, and its buttons are where a click lands only then
    const bool shown = qWaitForScaled(
        [&] { return (mini = shownMiniPlayer(ui)) != nullptr && mini->opacity() == 1.0; });
    return shown ? mini : nullptr;
}

// The picture-in-picture window while the shell has one, or null
QQuickWindow *pictureInPictureWindow(const Ui &ui)
{
    return qobject_cast<QQuickWindow *>(
        ui.window->property("pictureInPictureWindow").value<QObject *>());
}

// An item of the picture-in-picture window, or null
QQuickItem *pictureInPictureItem(QQuickWindow *pip, const QString &objectName)
{
    for (QQuickItem *candidate : allItems(pip->contentItem())) {
        if (candidate->objectName() == objectName)
            return candidate;
    }
    return nullptr;
}

// Puts the pointer over the picture-in-picture window and clicks the centre of one of its controls
bool clickPictureInPictureControl(QQuickWindow *pip, const QString &objectName)
{
    QTest::mouseMove(pip, QPoint(pip->width() / 2 + 1, pip->height() / 4 + 1));
    QTest::mouseMove(pip, QPoint(pip->width() / 2, pip->height() / 4));
    QQuickItem *button = nullptr;
    if (!qWaitForScaled([&] {
            button = pictureInPictureItem(pip, objectName);
            return button && button->isVisible() && button->isEnabled();
        }))
        return false;
    const QQuickItem *scrim = pictureInPictureItem(pip, QStringLiteral("pictureInPictureScrim"));
    if (!scrim || !qWaitForScaled([&] { return scrim->opacity() == 1.0; }))
        return false;
    QTest::mouseClick(
        pip, Qt::LeftButton, Qt::NoModifier,
        button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
    return true;
}

// Plays the fixture movie, enters picture in picture by the full player's button and waits for the window; it, or null
QQuickWindow *openPictureInPicture(Ui &ui, PlayerStub &stub)
{
    QQuickItem *page = openPlayer(ui, stub);
    QQuickItem *button =
        page ? Ui::itemIn(page, QStringLiteral("playerPictureInPictureButton")) : nullptr;
    if (!button)
        return nullptr;
    ui.clickCenter(button);
    QQuickWindow *pip = nullptr;
    const bool shown = qWaitForScaled([&] {
        pip = pictureInPictureWindow(ui);
        return pip && pip->isVisible()
               && ui.window->property("playbackMode").toString() == QStringLiteral("pip");
    });
    return shown ? pip : nullptr;
}

// The item with the keyboard focus once it has stopped changing for a few polls
QQuickItem *focusAtRest(const Ui &ui)
{
    QQuickItem *last = nullptr;
    int quiet = 0;
    QElapsedTimer timer;
    timer.start();
    while (quiet < 5 && timer.elapsed() < scaled(5000)) {
        QTest::qWait(20);
        QQuickItem *now = ui.window->activeFocusItem();
        quiet = now == last ? quiet + 1 : 0;
        last = now;
    }
    return last;
}

// Puts the pointer at rest in the middle of the window, on the video: two moves, because Qt drops a move to the position it already has for the cursor, and an earlier test may have left it there
void restPointerOnVideo(const Ui &ui)
{
    const QPoint middle(ui.window->width() / 2, ui.window->height() / 2);
    QTest::mouseMove(ui.window, middle + QPoint(1, 1));
    QTest::mouseMove(ui.window, middle);
}

// Opens the details of an item through the shell; the page in front once it holds the item, or null
QQuickItem *openDetails(Ui &ui, const QString &id)
{
    if (!QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openItem",
                                   Q_ARG(QVariant, id), Q_ARG(QVariant, QVariant())))
        return nullptr;
    QQuickItem *page = nullptr;
    const bool ready = qWaitForScaled(
        [&] {
            page = ui.currentDetails();
            return page && page->objectName() == QStringLiteral("itemDetailsPage")
                   && page->property("itemId").toString() == id
                   && page->property("showContent").toBool();
        },
        10000);
    return ready ? page : nullptr;
}

// True when the item is, or sits inside, a popup (a dialog, a menu)
bool inPopupItem(const QQuickItem *item)
{
    for (const QQuickItem *parent = item; parent; parent = parent->parentItem()) {
        if (parent->inherits("QQuickPopupItem"))
            return true;
    }
    return false;
}

// The names of the items, the class name for one without
QStringList namesOf(const QList<QQuickItem *> &items)
{
    QStringList names;
    for (const QQuickItem *item : items)
        names << (item->objectName().isEmpty()
                      ? QString::fromLatin1(item->metaObject()->className())
                      : item->objectName());
    return names;
}

// The focus as a pad sees it: a row of a list or grid stands for its view, whose delegates come and go as it scrolls (a header or footer control is itself)
QQuickItem *padFocus(QQuickWindow *window)
{
    QQuickItem *focus = window->activeFocusItem();
    for (QQuickItem *item = focus; item; item = item->parentItem()) {
        if (item == focus || !item->inherits("QQuickItemView"))
            continue;
        const auto *header = item->property("headerItem").value<QQuickItem *>();
        const auto *footer = item->property("footerItem").value<QQuickItem *>();
        if ((header && header->isAncestorOf(focus)) || (footer && footer->isAncestorOf(focus)))
            return focus;
        return item;
    }
    return focus;
}

// What a pad reaches from the focus by the four directions alone: every newly reached target is tried in turn and the focus goes back to it each time. A list is tried with its current row at the start and at the end, and its row is put back after; a spin box is left out of Left and Right, which step it. Targets are guarded: a page can drop an item the walk reached
QList<QQuickItem *> padReach(Ui &ui, Kinefin::RemoteInputServer &input, int limit = 60)
{
    QList<QPointer<QQuickItem>> reached;
    QQuickItem *start = padFocus(ui.window);
    if (!start)
        return {};
    reached << start;
    const auto known = [&reached](const QQuickItem *item) {
        return std::any_of(reached.cbegin(), reached.cend(),
                           [item](const QPointer<QQuickItem> &entry) { return entry == item; });
    };
    for (qsizetype next = 0; next < reached.size() && reached.size() < limit; ++next) {
        const QPointer<QQuickItem> item = reached.at(next);
        if (!item)
            continue;
        const bool view = item->inherits("QQuickItemView");
        const int count = view ? item->property("count").toInt() : 0;
        const int original = view ? item->property("currentIndex").toInt() : -1;
        QList<int> rows{original};
        if (view && count > 1)
            rows << count - 1;
        for (const int row : std::as_const(rows)) {
            for (const char *keycode : {"up", "down", "left", "right"}) {
                if (!item)
                    break;
                const bool sideways =
                    qstrcmp(keycode, "left") == 0 || qstrcmp(keycode, "right") == 0;
                if (sideways && item->inherits("QQuickSpinBox"))
                    continue;
                if (view && row != original) {
                    QMetaObject::invokeMethod(item, "positionViewAtEnd");
                    item->setProperty("currentIndex", row);
                }
                // A view takes the focus on its current row, as the pad gives it
                auto *current =
                    view ? item->property("currentItem").value<QQuickItem *>() : nullptr;
                (current ? current : item.data())->forceActiveFocus(Qt::TabFocusReason);
                input.dispatchKeycode(QString::fromLatin1(keycode));
                QCoreApplication::processEvents();
                if (QQuickItem *now = padFocus(ui.window); now && !known(now))
                    reached << now;
            }
        }
        if (view && item) {
            item->setProperty("currentIndex", original);
            QMetaObject::invokeMethod(item, "positionViewAtBeginning");
        }
    }
    QList<QQuickItem *> alive;
    for (const QPointer<QQuickItem> &entry : std::as_const(reached)) {
        if (entry)
            alive << entry;
    }
    return alive;
}

// Triggers the page's Media Info action and returns the dialog once it is open, or null
QObject *openMediaInfo(Ui &ui, QQuickItem *page)
{
    QObject *action = page->findChild<QObject *>(QStringLiteral("mediaInfoAction"));
    if (!action || !QMetaObject::invokeMethod(action, "trigger"))
        return nullptr;
    QObject *dialog = nullptr;
    const bool opened = qWaitForScaled(
        [&] {
            dialog = ui.popup(QStringLiteral("mediaInfoDialog"));
            return dialog && dialog->property("opened").toBool();
        },
        5000);
    return opened ? dialog : nullptr;
}

// The `text` of every visible item the dialog shows: its items are visual children of its popup item, not QObject children of the dialog (a Repeater's rows are not)
QStringList dialogTexts(const QObject *dialog)
{
    QStringList texts;
    auto *root = dialog->property("contentItem").value<QQuickItem *>();
    while (root && root->parentItem() && !root->parentItem()->inherits("QQuickOverlay"))
        root = root->parentItem();
    for (const QQuickItem *item : root ? allItems(root) : QList<QQuickItem *>{}) {
        if (item->isVisible() && item->property("text").isValid())
            texts.append(item->property("text").toString());
    }
    return texts;
}

// The page's end time for the given clock, as the page words it
QString endsAtText(QQuickItem *page, const QDateTime &now)
{
    QVariant text;
    const QVariant clock(now);
    QMetaObject::invokeMethod(page, "endsAtText", Q_RETURN_ARG(QVariant, text),
                              Q_ARG(QVariant, clock));
    return text.toString();
}

// The name an item gives assistive technology: `Accessible.name` is an attached property, not one of the item's own
QString accessibleNameOf(QObject *object)
{
    return QQmlProperty(object, QStringLiteral("Accessible.name"), qmlContext(object))
        .read()
        .toString();
}

// Hands the shell its remote-control object (the window's `remoteInput`); the object must outlive the window
bool attach(Ui &ui, const char *property, QObject *object)
{
    return ui.engine.rootObjects().constFirst()->setProperty(
        property, QVariant::fromValue<QObject *>(object));
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
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), 2);
    }

    void homeOnlyReportsNoLibrariesAfterTheListingSucceeds()
    {
        Ui ui(m_home->path());
        ui.server.holdViews = true;
        ui.serveViews({});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(!ui.controller.librariesLoaded());
        QVERIFY(!ui.showsText(QStringLiteral("No libraries to show")));

        // The empty response is not a request failure: only now is the
        // placeholder correct. The first held request must not settle it.
        ui.server.holdViews = false;
        ui.controller.reloadLibraries();
        QTRY_VERIFY(ui.controller.librariesLoaded());
        QTRY_VERIFY(ui.showsText(QStringLiteral("No libraries to show")));
        QVERIFY(ui.controller.errorText().isEmpty());
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
        // The layout may still move right after the cards appear
        QTRY_VERIFY([&] {
            const QPointF seriesAt = series->mapToScene(QPointF(0, 0));
            const QPointF musicAt = music->mapToScene(QPointF(0, 0));
            return seriesAt.y() < musicAt.y() - 1
                   || (qAbs(seriesAt.y() - musicAt.y()) <= 1 && seriesAt.x() < musicAt.x());
        }());
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
            scaled(10000));
    }

    void libraryGridOpensAndPagesOnDemand()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 450;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("libraryGrid"))) != nullptr);
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

    void theLibraryViewComesWithItsFirstPage()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 60;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(!ui.window->property("compactHeader").toBool());
        // While the first page is out the page shows its spinner and builds no view
        ui.server.heldFamilies.insert(QStringLiteral("Items"));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryLoading")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("libraryGrid")));
        // One library of the type in a wide window: the row of the tab's search field and library selector is not built
        QVERIFY(!ui.item(QStringLiteral("librarySearchField")));
        QVERIFY(!ui.item(QStringLiteral("librarySelector")));
        // A position restored before the view comes is what the tab keeps if it is left meanwhile
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        QVERIFY(page);
        const QVariantMap restored{{QStringLiteral("currentIndex"), 7},
                                   {QStringLiteral("contentY"), 0.0}};
        QVERIFY(QMetaObject::invokeMethod(page, "restoreViewPosition", Q_ARG(QVariant, restored)));
        QVariant kept;
        QVERIFY(QMetaObject::invokeMethod(page, "viewPosition", Q_RETURN_ARG(QVariant, kept)));
        QCOMPARE(kept.toMap().value(QStringLiteral("currentIndex")).toInt(), 7);

        // The tab gave the keys to a view that did not exist yet: the view takes them when it comes
        ui.server.heldFamilies.clear();
        ui.controller.movies()->reload();
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("libraryGrid"))) != nullptr);
        QTRY_VERIFY(grid->property("count").toInt() > 0);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTRY_COMPARE(grid->property("currentIndex").toInt(), 7);
        QVERIFY(!shownItem(ui, QStringLiteral("libraryLoading")));

        // A reload keeps the view
        ui.controller.movies()->reload();
        QCOMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Loading);
        QCOMPARE(ui.item(QStringLiteral("libraryGrid")), grid);
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QCOMPARE(ui.item(QStringLiteral("libraryGrid")), grid);
    }

    void theLibraryViewLeavesTheKeysWhereTheUserMovedThem()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 60;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        ui.server.heldFamilies.insert(QStringLiteral("Items"));
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryLoading")) != nullptr);
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(field);
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());

        ui.server.heldFamilies.clear();
        ui.controller.movies()->reload();
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("libraryGrid"))) != nullptr);
        QTRY_VERIFY(grid->property("count").toInt() > 0);
        QVERIFY(field->hasActiveFocus());
        QVERIFY(!grid->hasActiveFocus());
    }

    void aCardRowBuildsItsListWithItsFirstCards()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        ui.settle();
        QQmlComponent parts(&ui.engine);
        parts.setData("import QtQuick\nItem {\n"
                      "    property Component delegate: Rectangle { width: 40; height: 40 }\n"
                      "    property Flickable flickable: Flickable {}\n"
                      "}",
                      QUrl());
        std::unique_ptr<QObject> holder(parts.create());
        QVERIFY2(holder, qPrintable(parts.errorString()));
        QQmlComponent component(&ui.engine);
        component.loadFromModule(QStringLiteral("org.kde.kinefin"), QStringLiteral("CardRow"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> created(component.createWithInitialProperties(
            {{QStringLiteral("model"), QVariantList{}},
             {QStringLiteral("delegate"), holder->property("delegate")},
             {QStringLiteral("cardHeight"), 60},
             {QStringLiteral("accessibleName"), QStringLiteral("Row")},
             {QStringLiteral("pageFlickable"), holder->property("flickable")}}));
        auto *row = qobject_cast<QQuickItem *>(created.get());
        QVERIFY(row);
        row->setParentItem(ui.window->contentItem());
        row->setWidth(400);
        const qreal height = row->implicitHeight();
        QVERIFY(height >= 60);

        // No cards, no list, and nothing waits for one
        QVERIFY(!row->property("list").value<QQuickItem *>());
        QVERIFY(
            QMetaObject::invokeMethod(row, "focusList", Q_ARG(QVariant, int(Qt::TabFocusReason))));
        QVERIFY(!row->property("focusPending").toBool());
        // Cards coming: the list is built asynchronously, and the keys asked for meanwhile wait for it
        row->setProperty("model", QVariantList{1, 2, 3});
        QVERIFY(!row->property("list").value<QQuickItem *>());
        QVERIFY(
            QMetaObject::invokeMethod(row, "focusList", Q_ARG(QVariant, int(Qt::TabFocusReason))));
        QVERIFY(row->property("focusPending").toBool());
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = row->property("list").value<QQuickItem *>()) != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), 3);
        QTRY_VERIFY(list->hasActiveFocus());
        // The row kept its height while the list came
        QCOMPARE(row->implicitHeight(), height);

        // A model that empties keeps the list
        row->setProperty("model", QVariantList{});
        QCOMPARE(row->property("list").value<QQuickItem *>(), list);
    }

    void aCardRowScrollsWithItsArrows()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        ui.settle();
        QQmlComponent parts(&ui.engine);
        parts.setData("import QtQuick\nItem {\n"
                      "    property Component delegate: Rectangle { width: 60; height: 40 }\n"
                      "    property Flickable flickable: Flickable {}\n"
                      "}",
                      QUrl());
        std::unique_ptr<QObject> holder(parts.create());
        QVERIFY2(holder, qPrintable(parts.errorString()));
        QQmlComponent component(&ui.engine);
        component.loadFromModule(QStringLiteral("org.kde.kinefin"), QStringLiteral("CardRow"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QVariantList cards;
        for (int i = 0; i < 30; ++i)
            cards << i;
        std::unique_ptr<QObject> created(component.createWithInitialProperties(
            {{QStringLiteral("model"), cards},
             {QStringLiteral("delegate"), holder->property("delegate")},
             {QStringLiteral("cardHeight"), 60},
             {QStringLiteral("accessibleName"), QStringLiteral("Row")},
             {QStringLiteral("pageFlickable"), holder->property("flickable")}}));
        auto *row = qobject_cast<QQuickItem *>(created.get());
        QVERIFY(row);
        row->setParentItem(ui.window->contentItem());
        // Below the window's header, which would take the clicks
        row->setY(ui.window->height() / 2);
        row->setZ(100);
        row->setWidth(400);
        row->setHeight(row->implicitHeight());
        // No scroll bar: the row is as high as its cards
        QCOMPARE(row->implicitHeight(), 60.0);
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = row->property("list").value<QQuickItem *>()) != nullptr);
        QQuickItem *previous = Ui::itemIn(row, QStringLiteral("cardRowPrevious"));
        QQuickItem *next = Ui::itemIn(row, QStringLiteral("cardRowNext"));
        QVERIFY(previous && next);
        // A card's focus frame reaches a little past it: the list clips that much farther out than the row, and the first card still starts at the row's edge
        QQuickItem *first = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(list, "itemAtIndex",
                                              Q_RETURN_ARG(QQuickItem *, first), Q_ARG(int, 0))
                    && first != nullptr);
        QVERIFY(list->property("clip").toBool());
        QVERIFY(list->mapToScene(QPointF(0, 0)).x() <= row->mapToScene(QPointF(0, 0)).x() - 3);
        QVERIFY(qAbs(first->mapToScene(QPointF(0, 0)).x() - row->mapToScene(QPointF(0, 0)).x())
                < 0.5);
        // The arrows show only while the pointer is over the row
        QVERIFY(!next->isVisible());
        const QPoint over = row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint();
        // Qt drops a move to where the cursor already is, which a test before may have left there
        QTest::mouseMove(ui.window, over + QPoint(1, 1));
        QTest::mouseMove(ui.window, over);
        // At the start only the way forward shows; each arrow is a strip as high as the row
        QTRY_VERIFY(next->isVisible());
        QCOMPARE(next->height(), row->height());
        QVERIFY(qAbs(next->width() - 2.5 * gridUnitOf(ui)) < 1);
        QVERIFY(!previous->isVisible());
        const qreal start = list->property("contentX").toReal();
        ui.clickCenter(next);
        QTRY_VERIFY(list->property("contentX").toReal() > start + 100);
        QTRY_VERIFY(previous->isVisible());
        // At the end only the way back shows
        for (int step = 0; step < 20 && list->property("atXEnd").toBool() == false; ++step) {
            QVERIFY(QMetaObject::invokeMethod(row, "scrollBy", Q_ARG(QVariant, 1)));
            QTest::qWait(50);
        }
        QTRY_VERIFY(list->property("atXEnd").toBool());
        QTRY_VERIFY(!next->isVisible());
        QVERIFY(previous->isVisible());
        // The pointer leaving the row takes them away
        QTest::mouseMove(ui.window, QPoint(2, 2));
        QTRY_VERIFY(!previous->isVisible());
    }

    void aMessageBoxIsBuiltWhenItFirstShowsAndKept()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        QQuickItem *box = ui.item(QStringLiteral("libraryFailedBox"));
        QVERIFY(box);
        const auto built = [box] {
            for (const QQuickItem *child : box->childItems()) {
                if (child->inherits("QQuickLoader"))
                    return child->property("item").value<QObject *>() != nullptr;
            }
            return false;
        };
        // A page holds several hidden boxes: none of them builds its Kirigami message
        QVERIFY(!box->isVisible());
        QVERIFY(!built());

        ui.server.listingStatus.insert(QStringLiteral("Items"), 500);
        ui.controller.movies()->reload();
        QTRY_VERIFY(box->isVisible());
        QVERIFY(built());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not load this library"), false));

        // Hidden again, the box keeps what it built
        ui.server.listingStatus.clear();
        QVERIFY(ui.clickButton(QStringLiteral("Retry")));
        QTRY_VERIFY(!box->isVisible());
        QVERIFY(built());
    }

    void cardGridKeepsTheCardsOutOfTheScrollBar()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 120;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        // Cards still being created sit unplaced (and hidden by the view) until they are done
        QTRY_VERIFY(libraryViewBuilt(ui));
        ui.settle();
        QTRY_VERIFY(cardGridGeometry(ui).firstRow.size() > 1);
        const CardGridGeometry geometry = cardGridGeometry(ui);
        QVERIFY(geometry.scrollBar.width() > 0);
        // The right-most card ends left of the bar
        const QRectF last = geometry.firstRow.constLast();
        QVERIFY2(last.right() <= geometry.scrollBar.x(),
                 qPrintable(QStringLiteral("card right %1, scroll bar x %2")
                                .arg(last.right())
                                .arg(geometry.scrollBar.x())));
    }

    void cardGridSeparatesTheCardsAndKeepsAPageMargin()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 120;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + kMovies) != nullptr);
        QVERIFY(ui.openLibraryTile(kMovies));
        QTRY_COMPARE(ui.controller.movies()->status(), Kinefin::LibraryItemsModel::Ready);
        // Cards still being created sit unplaced (and hidden by the view) until they are done
        QTRY_VERIFY(libraryViewBuilt(ui));
        ui.settle();
        QTRY_VERIFY(cardGridGeometry(ui).firstRow.size() > 1);
        const CardGridGeometry geometry = cardGridGeometry(ui);
        QVERIFY(geometry.gutter > 0);
        // The first card starts at least the page margin (the gutter) from the window's left edge
        QVERIFY(geometry.firstRow.constFirst().x() >= geometry.gutter - 0.5);
        // Two neighbours are at least the gutter apart
        for (qsizetype i = 1; i < geometry.firstRow.size(); ++i) {
            const qreal gap = geometry.firstRow.at(i).x() - geometry.firstRow.at(i - 1).right();
            QVERIFY2(gap >= geometry.gutter - 0.5, qPrintable(QStringLiteral("gap %1").arg(gap)));
        }
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
        QVERIFY(page);
        // A row builds its list asynchronously: the list is in the scene before its cards
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr
                    && list->property("currentItem").value<QObject *>() != nullptr);
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
        QTRY_VERIFY(list->mapToScene(QPointF(0, list->height() / 2)).y() < ui.window->height());
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
        for (const QString &heading : {QStringLiteral("Cast and Crew"), QStringLiteral("Chapters"),
                                       QStringLiteral("More Like This")})
            QTRY_VERIFY2(ui.showsText(heading), qPrintable(heading));
        // The cards of a row come a few frames after its heading
        QTRY_VERIFY(ui.showsText(QStringLiteral("Mira Vale"), false));
        QTRY_VERIFY(ui.showsText(QStringLiteral("The Ship"), false));
        // Playback is not wired yet: the Resume action is there and disabled; the fixture movie has a resume point
        const QObject *play = ui.item(QStringLiteral("itemDetailsPage"))
                                  ->findChild<QObject *>(QStringLiteral("playAction"));
        QVERIFY(play);
        QCOMPARE(play->property("text").toString(), QStringLiteral("Resume"));
        QVERIFY(!play->property("enabled").toBool());
        // Media Info is an action of the page that opens a dialog with the rows of the source
        QQuickItem *page = ui.item(QStringLiteral("itemDetailsPage"));
        const QObject *info = page->findChild<QObject *>(QStringLiteral("mediaInfoAction"));
        QVERIFY(info);
        QCOMPARE(info->property("text").toString(), QStringLiteral("Media Info"));
        QTRY_VERIFY(info->property("visible").toBool());
        QVERIFY(!ui.popup(QStringLiteral("mediaInfoDialog")));
        QObject *dialog = openMediaInfo(ui, page);
        QVERIFY(dialog);
        QCOMPARE(dialog->property("title").toString(), QStringLiteral("Media Info"));
        const QStringList texts = dialogTexts(dialog);
        for (const QString &expected :
             {QStringLiteral("Arcturus 1080p"), QStringLiteral("Container"), QStringLiteral("mkv"),
              QStringLiteral("Video"), QStringLiteral("Audio"), QStringLiteral("Subtitles")})
            QVERIFY2(texts.contains(expected), qPrintable(expected));
        // A stream line is the server's own description, a video's with its resolution, and nothing it already says twice
        QVERIFY2(texts.contains(QStringLiteral("1080p HEVC \u00b7 1920\u00d71080")),
                 qPrintable(texts.join(QLatin1Char('|'))));
        QVERIFY(texts.contains(QStringLiteral("English EAC3 5.1")));
        QVERIFY(texts.contains(QStringLiteral("Italian SRT")));
    }

    void copyPutsTheMediaInfoInTheClipboard()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        QGuiApplication::clipboard()->clear();
        QObject *dialog = openMediaInfo(ui, page);
        QVERIFY(dialog);
        QObject *copy = dialog->findChild<QObject *>(QStringLiteral("mediaInfoCopyAction"));
        QVERIFY(copy);
        QVERIFY(QMetaObject::invokeMethod(copy, "trigger"));
        QTRY_VERIFY(
            QGuiApplication::clipboard()->text().contains(QStringLiteral("Container: mkv")));
        const QString copied = QGuiApplication::clipboard()->text();
        QVERIFY(copied.startsWith(QStringLiteral("Arcturus 1080p\n")));
        QVERIFY(copied.contains(QStringLiteral("Video: 1080p HEVC \u00b7 1920\u00d71080")));
        QVERIFY(copied.contains(QStringLiteral("Audio: English EAC3 5.1")));
    }

    void creditsShowOneRowPerKind()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.serveShow();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        const auto check = [](QQuickItem *page, const QString &name, const QString &label,
                              const QString &value) {
            QQuickItem *labelItem = Ui::itemIn(page, name + QStringLiteral("Label"));
            QQuickItem *valueItem = Ui::itemIn(page, name);
            QVERIFY(labelItem && valueItem);
            QTRY_VERIFY(valueItem->isVisible() && labelItem->isVisible());
            QCOMPARE(labelItem->property("text").toString(), label);
            QCOMPARE(valueItem->property("text").toString(), value);
        };
        QQuickItem *movie = openDetails(ui, kMovieId);
        QVERIFY(movie);
        QVERIFY(Ui::itemIn(movie, QStringLiteral("detailsCredits")));
        check(movie, QStringLiteral("creditsGenres"), QStringLiteral("Genres"),
              QStringLiteral("Science Fiction, Mystery"));
        check(movie, QStringLiteral("creditsStudios"), QStringLiteral("Studio"),
              QStringLiteral("Vega Pictures"));
        check(movie, QStringLiteral("creditsDirectors"), QStringLiteral("Director"),
              QStringLiteral("Ines Okafor"));
        check(movie, QStringLiteral("creditsWriters"), QStringLiteral("Writer"),
              QStringLiteral("Pavel Lund"));
        // A series names its studio a network, and its fixture has no crew: those rows are gone
        QQuickItem *series = openDetails(ui, kSeriesId);
        QVERIFY(series);
        check(series, QStringLiteral("creditsGenres"), QStringLiteral("Genres"),
              QStringLiteral("Drama, Science Fiction"));
        check(series, QStringLiteral("creditsStudios"), QStringLiteral("Network"),
              QStringLiteral("Abyss Network"));
        QVERIFY(!Ui::itemIn(series, QStringLiteral("creditsDirectors"))->isVisible());
        QVERIFY(!Ui::itemIn(series, QStringLiteral("creditsWriters"))->isVisible());
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
        // The list is in the scene while it is still being built: its card comes last
        QTRY_VERIFY(list->property("currentItem").value<QObject *>() != nullptr);
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
            {QStringLiteral("Studios"),
             QJsonArray{QJsonObject{{QStringLiteral("Name"), with(QStringLiteral(" studio"))}}}},
            {QStringLiteral("OfficialRating"), with(QStringLiteral(" rating"))},
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
                                 {QStringLiteral("DisplayTitle"), with(QStringLiteral(" stream"))}},
                             QJsonObject{{QStringLiteral("Type"), QStringLiteral("Video")},
                                         {QStringLiteral("Codec"), hostile}}}}}}}};
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
        QQuickItem *hostilePage = ui.item(QStringLiteral("itemDetailsPage"));
        // The media info lives in the dialog: it is open while the labels are checked
        const QObject *hostileInfo =
            hostilePage->findChild<QObject *>(QStringLiteral("mediaInfoAction"));
        QVERIFY(hostileInfo);
        QTRY_VERIFY(hostileInfo->property("visible").toBool());
        QVERIFY(openMediaInfo(ui, hostilePage));
        // Every label that carries the markup renders it as text, never as rich text: the title and heading, the series link, and the lines the markup shares with other words
        for (const QString &part :
             {QStringLiteral(" name"), QStringLiteral(" series"), QStringLiteral(" overview"),
              QStringLiteral(" tagline"), QStringLiteral(" genre"), QStringLiteral(" person"),
              QStringLiteral(" role"), QStringLiteral(" chapter"), QStringLiteral(" container"),
              QStringLiteral(" studio"), QStringLiteral(" rating"), QStringLiteral(" stream")}) {
            QTRY_VERIFY2(!ui.itemsWithText(with(part), false).isEmpty(), qPrintable(part));
            for (QQuickItem *label : ui.itemsWithText(with(part), false)) {
                // A wrapper that hands its text to a child (Kirigami's page title) is judged by the child that renders it
                if (!label->property("textFormat").isValid()) {
                    const QString text = label->property("text").toString();
                    const QList<QQuickItem *> inside = allItems(label);
                    QVERIFY(
                        std::any_of(inside.cbegin(), inside.cend(), [&](const QQuickItem *child) {
                            return child != label && child->property("text").toString() == text
                                   && child->property("textFormat").toInt() == int(Qt::PlainText);
                        }));
                    continue;
                }
                QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
            }
        }
        // An unknown codec becomes a badge, upper-cased and capped: still plain text
        QTRY_VERIFY(!ui.itemsWithText(QStringLiteral("<B>BOLD</B>"), false).isEmpty());
        for (QQuickItem *label : ui.itemsWithText(QStringLiteral("<B>BOLD</B>"), false))
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void detailsFactsDifferByType_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QStringList>("shown");
        QTest::addColumn<QStringList>("hidden");
        QTest::addColumn<QStringList>("texts");
        const auto words = [](const QString &list, QChar separator = QLatin1Char(' ')) {
            return list.split(separator);
        };
        QTest::newRow("movie")
            << kMovieId
            << words(QStringLiteral(
                   "factYear certificateBadge factRating factCritic factRuntime factEndsAt"))
            << words(QStringLiteral("factEpisode factStatus factSeasons factEpisodes factUnplayed "
                                    "factPlayed factFavorite"))
            << words(QStringLiteral("factYear=2012;factCritic=88%;factRuntime=18 min"),
                     QLatin1Char(';'));
        QTest::newRow("series")
            << kSeriesId
            << words(
                   QStringLiteral("factYear certificateBadge factRating factSeasons factUnplayed"))
            << words(QStringLiteral("factEpisode factStatus factEpisodes factCritic factRuntime "
                                    "factEndsAt factPlayed"))
            << words(QStringLiteral("factYear=2021–present;factSeasons=3 seasons;"
                                    "factUnplayed=8 unplayed"),
                     QLatin1Char(';'));
        QTest::newRow("season")
            << kSeasonId << words(QStringLiteral("factEpisodes factUnplayed"))
            << words(QStringLiteral("factYear factEpisode factSeasons factRuntime factEndsAt "
                                    "certificateBadge factRating"))
            << words(QStringLiteral("factEpisodes=4 episodes;factUnplayed=6 unplayed"),
                     QLatin1Char(';'));
        QTest::newRow("episode")
            << kEpisodeId << words(QStringLiteral("factEpisode factYear factRuntime factEndsAt"))
            << words(QStringLiteral("factSeasons factEpisodes factUnplayed factCritic factStatus "
                                    "certificateBadge factRating"))
            << words(QStringLiteral("factEpisode=S2:E6;factYear=%1;factRuntime=28 min")
                         .arg(QLocale().toString(QDate(2026, 2, 13), QLocale::ShortFormat)),
                     QLatin1Char(';'));
    }

    void detailsFactsDifferByType()
    {
        QFETCH(QString, id);
        QFETCH(QStringList, shown);
        QFETCH(QStringList, hidden);
        QFETCH(QStringList, texts);
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.serveShow();
        ui.server.generatedEpisodeTotal = 4;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openDetails(ui, id);
        QVERIFY(page);
        const auto fact = [page](const QString &name) {
            const QQuickItem *item = Ui::itemIn(page, name);
            return item && item->isVisible() ? item : nullptr;
        };
        for (const QString &name : shown)
            QTRY_VERIFY2(fact(name), qPrintable(name));
        for (const QString &name : hidden)
            QVERIFY2(!fact(name), qPrintable(name));
        for (const QString &pair : texts) {
            const QString name = pair.section(QLatin1Char('='), 0, 0);
            // A badge carries its words as `caption`, a label as `text`
            const auto words = [&fact, &name] {
                const QQuickItem *item = fact(name);
                if (!item)
                    return QString();
                const QVariant caption = item->property("caption");
                return (caption.isValid() ? caption : item->property("text")).toString();
            };
            QTRY_COMPARE(words(), pair.section(QLatin1Char('='), 1));
        }
    }

    void detailsCertificateIsABadgeWithAnAccessibleName()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        QQuickItem *badge = nullptr;
        QTRY_VERIFY((badge = Ui::itemIn(page, QStringLiteral("certificateBadge"))) != nullptr
                    && badge->isVisible());
        QCOMPARE(badge->property("accessibleName").toString(), QStringLiteral("Rated PG-13"));
        // The certificate is one plain-text label inside the badge, and nowhere else on the page
        int labels = 0;
        for (QQuickItem *label : ui.itemsWithText(QStringLiteral("PG-13"), false)) {
            if (!page->isAncestorOf(label))
                continue;
            ++labels;
            QVERIFY2(badge->isAncestorOf(label), qPrintable(Ui::describe(label)));
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }
        QCOMPARE(labels, 1);
    }

    void detailsOverviewCollapsesAndExpands()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.serveShow();
        QJsonObject movie = fixtureObject(QStringLiteral("movie_item"));
        QString overview;
        for (int i = 0; i < 60; ++i)
            overview += QStringLiteral("The survey ship drifts on. ");
        movie.insert(QStringLiteral("Overview"), overview);
        ui.server.itemsById.insert(kMovieId, QJsonDocument(movie).toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        QQuickItem *label = Ui::itemIn(page, QStringLiteral("overviewLabel"));
        QQuickItem *toggle = Ui::itemIn(page, QStringLiteral("overviewToggle"));
        QVERIFY(label && toggle);
        // Long text: four lines, elided, and a button to open it
        QCOMPARE(label->property("maximumLineCount").toInt(), 4);
        QTRY_VERIFY(label->property("truncated").toBool());
        QTRY_VERIFY(toggle->isVisible());
        QCOMPARE(toggle->property("text").toString(), QStringLiteral("Show More"));
        QVERIFY(QMetaObject::invokeMethod(toggle, "click"));
        QTRY_COMPARE(toggle->property("text").toString(), QStringLiteral("Show Less"));
        QVERIFY(label->property("maximumLineCount").toInt() > 4);
        QTRY_VERIFY(!label->property("truncated").toBool());
        QVERIFY(toggle->isVisible());
        QVERIFY(QMetaObject::invokeMethod(toggle, "click"));
        QTRY_COMPARE(toggle->property("text").toString(), QStringLiteral("Show More"));
        QCOMPARE(label->property("maximumLineCount").toInt(), 4);
        QTRY_VERIFY(label->property("truncated").toBool());
        // Folded, the last line fades out
        QVERIFY(Ui::itemIn(page, QStringLiteral("overviewFade"))->isVisible());

        // The page keeps its place when the text opens and folds again (a short window: the page stays longer than it, folded too)
        ui.window->resize(ui.window->width(), 360);
        QQuickItem *list = Ui::itemIn(page, QStringLiteral("episodesList"));
        QVERIFY(list);
        QVERIFY(QMetaObject::invokeMethod(toggle, "click"));
        QTRY_COMPARE(toggle->property("text").toString(), QStringLiteral("Show Less"));
        QTRY_VERIFY(list->property("contentHeight").toDouble() > list->height() + 60);
        // The place a click keeps is held only for a moment: a scroll after it is the user's
        QTest::qWait(400);
        const auto offset = [list] {
            return list->property("contentY").toDouble() - list->property("originY").toDouble();
        };
        QVERIFY(list->setProperty("contentY", list->property("originY").toDouble() + 60));
        QTRY_COMPARE(offset(), 60.0);
        QVERIFY(QMetaObject::invokeMethod(toggle, "click"));
        QTRY_COMPARE(toggle->property("text").toString(), QStringLiteral("Show More"));
        QTest::qWait(100);
        QVERIFY(list->property("contentHeight").toDouble() > list->height() + 60);
        QCOMPARE(offset(), 60.0);
        QVERIFY(QMetaObject::invokeMethod(toggle, "click"));
        QTRY_COMPARE(toggle->property("text").toString(), QStringLiteral("Show Less"));
        QTest::qWait(100);
        QCOMPARE(offset(), 60.0);
        QVERIFY(QMetaObject::invokeMethod(toggle, "click"));
        QTRY_COMPARE(toggle->property("text").toString(), QStringLiteral("Show More"));

        // A text that folding would cut by two lines only shows whole, with no button; three more lines fold
        const auto withOverview = [&](int lines) {
            QStringList text;
            for (int i = 0; i < lines; ++i)
                text << QStringLiteral("Line %1 of the overview").arg(i + 1);
            QJsonObject item = fixtureObject(QStringLiteral("movie_item"));
            item.insert(QStringLiteral("Overview"), text.join(QLatin1Char('\n')));
            ui.server.itemsById.insert(kMovieId,
                                       QJsonDocument(item).toJson(QJsonDocument::Compact));
        };
        // The same item already in front is not pushed again: Home first
        QObject *root = ui.engine.rootObjects().constFirst();
        const auto reopen = [&] {
            QVERIFY(QMetaObject::invokeMethod(root, "goHome"));
            QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        };
        withOverview(6);
        reopen();
        QQuickItem *six = openDetails(ui, kMovieId);
        QVERIFY(six);
        QTRY_VERIFY(Ui::itemIn(six, QStringLiteral("overviewLabel"))->property("lineCount").toInt()
                    == 6);
        QVERIFY(!Ui::itemIn(six, QStringLiteral("overviewToggle"))->isVisible());
        QVERIFY(!Ui::itemIn(six, QStringLiteral("overviewLabel"))->property("truncated").toBool());
        withOverview(7);
        reopen();
        QQuickItem *seven = openDetails(ui, kMovieId);
        QVERIFY(seven);
        QTRY_VERIFY(Ui::itemIn(seven, QStringLiteral("overviewToggle"))->isVisible());
        QCOMPARE(Ui::itemIn(seven, QStringLiteral("overviewLabel"))
                     ->property("maximumLineCount")
                     .toInt(),
                 4);

        // Short text fits in four lines: no button
        QQuickItem *series = openDetails(ui, kSeriesId);
        QVERIFY(series);
        QQuickItem *shortLabel = Ui::itemIn(series, QStringLiteral("overviewLabel"));
        QVERIFY(shortLabel && shortLabel->isVisible());
        QTRY_VERIFY(!shortLabel->property("truncated").toBool());
        QVERIFY(!Ui::itemIn(series, QStringLiteral("overviewToggle"))->isVisible());
    }

    void detailsEndsAtFollowsRuntimeAndResume()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.serveShow();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        const QDateTime now(QDate(2026, 11, 13), QTime(20, 0));
        const auto endsAt = [](int hour, int minute) {
            return QStringLiteral("Ends at %1")
                .arg(QLocale().toString(QTime(hour, minute), QLocale::ShortFormat));
        };
        // 18 minutes of runtime with 3.9 watched: 14.1 left
        QCOMPARE(endsAtText(page, now), endsAt(20, 14));
        QQuickItem *label = Ui::itemIn(page, QStringLiteral("factEndsAt"));
        QTRY_VERIFY(label && label->isVisible()
                    && label->property("text").toString().startsWith(QStringLiteral("Ends at ")));
        // The clock moves once a minute, only while the page is in view
        QObject *timer = page->findChild<QObject *>(QStringLiteral("endsAtTimer"));
        QVERIFY(timer);
        QCOMPARE(timer->property("interval").toInt(), 60000);
        QTRY_VERIFY(timer->property("running").toBool());
        // Marking it watched drops the resume point: the whole runtime counts
        QObject *mark = page->findChild<QObject *>(QStringLiteral("togglePlayedAction"));
        QVERIFY(mark);
        QVERIFY(QMetaObject::invokeMethod(mark, "trigger"));
        QTRY_COMPARE(endsAtText(page, now), endsAt(20, 18));
        QCOMPARE(endsAtText(page, now.addSecs(3600)), endsAt(21, 18));
        // A series has no runtime, and the page below it is out of view
        QQuickItem *series = openDetails(ui, kSeriesId);
        QVERIFY(series);
        QCOMPARE(endsAtText(series, now), QString());
        QVERIFY(!series->findChild<QObject *>(QStringLiteral("endsAtTimer"))
                     ->property("running")
                     .toBool());
        QTRY_VERIFY(!timer->property("running").toBool());
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

    void aLinkArrivingWhileSignedInOpensAndReplacesDetails()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        ui.window->setProperty("requestedItemId", kMovieId);
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_VERIFY(ui.currentDetails()
                    && ui.currentDetails()->property("itemId").toString() == kMovieId);
        QTRY_COMPARE(ui.window->property("requestedItemId").toString(), QString());
        QCOMPARE(ui.stackDepth(), 2);
        ui.settle();
        // A warm launch of the same link leaves the stack unchanged.
        ui.window->setProperty("requestedItemId", kMovieId);
        QTRY_COMPARE(ui.window->property("requestedItemId").toString(), QString());
        QCOMPARE(ui.stackDepth(), 2);
        ui.settle();
        // A different link replaces the old detail page rather than nesting it.
        ui.window->setProperty("requestedItemId", kSeriesId);
        QTRY_VERIFY(ui.currentDetails()
                    && ui.currentDetails()->property("itemId").toString() == kSeriesId);
        QCOMPARE(ui.stackDepth(), 2);
        ui.settle();
        // The same external link also works when a type tab is the root.
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        ui.window->setProperty("requestedItemId", kMovieId);
        QTRY_VERIFY(ui.currentDetails()
                    && ui.currentDetails()->property("itemId").toString() == kMovieId);
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.rootTab(), QStringLiteral("movies"));
        ui.settle();
        // And when Favorites or Playlists is: the link opens above it at once
        for (const QString &tab : {QStringLiteral("Favorites"), QStringLiteral("Playlists")}) {
            QVERIFY(ui.clickTab(tab));
            QTRY_COMPARE(ui.rootTab(), tab.toLower());
            QTRY_VERIFY(!ui.tabSwitching());
            ui.window->setProperty("requestedItemId", kSeriesId);
            QTRY_VERIFY(ui.currentDetails()
                        && ui.currentDetails()->property("itemId").toString() == kSeriesId);
            QTRY_COMPARE(ui.window->property("requestedItemId").toString(), QString());
            QCOMPARE(ui.stackDepth(), 2);
            QCOMPARE(ui.rootTab(), tab.toLower());
            ui.settle();
        }
    }

    void aLinkDoesNotDiscardUnsavedPreferences()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        ui.window->setProperty("requestedItemId", kMovieId);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(ui.item(QStringLiteral("itemDetailsPage")) == nullptr);
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Cancel")));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(prefs->dirty());
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        // The same URL can be delivered again after Cancel.
        ui.window->setProperty("requestedItemId", kMovieId);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Discard")));
        QTRY_VERIFY(ui.item(QStringLiteral("itemDetailsPage")) != nullptr);
        QTRY_VERIFY(ui.currentDetails()
                    && ui.currentDetails()->property("itemId").toString() == kMovieId);
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(!prefs->dirty());
        QVERIFY(remote.posts.isEmpty());
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
        // A card outside the visible part is built asynchronously: the row adopts it as current a moment later
        QTRY_VERIFY([&] {
            const QObject *current = row->property("currentItem").value<QObject *>();
            return current && current->property("itemId").toString() == kSeasonId;
        }());
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
        QTRY_VERIFY(listBuilt(row));
        const int seriesDepth = ui.stackDepth();
        // The page the link opened hands the keys out once it settles, which can come after this focus under load: the key goes again until the season opens (the push is synchronous, so never twice)
        QVERIFY(qWaitForScaled(
            [&] {
                if (ui.stackDepth() > seriesDepth)
                    return true;
                row->forceActiveFocus();
                row->setProperty("currentIndex", 1);
                QTest::keyClick(ui.window, Qt::Key_Return);
                return ui.stackDepth() > seriesDepth;
            },
            5000));
        QCOMPARE(ui.stackDepth(), seriesDepth + 1);
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
        QTRY_VERIFY(listBuilt(row));
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
        QTRY_VERIFY(listBuilt(first));
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

    void theFilterDialogBuildsItsRowsOnlyWhileItShows()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_COMPARE(ui.controller.movies()->filterOptions()->legacyStatus(),
                     Kinefin::FilterOptionsModel::Ready);
        // The page creates the dialog with it; a closed popup is out of the scene, so the list is looked up as an object
        QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
        QObject *list = ui.popup(QStringLiteral("filterOptionsList"));
        QVERIFY(dialog && list);
        QCOMPARE(list->property("count").toInt(), 0);
        // The rows come with the opening, and the dialog has its full height from its first frame
        QList<qreal> heights;
        const QMetaObject::Connection frames =
            QObject::connect(ui.window, &QQuickWindow::frameSwapped, ui.window, [&] {
                if (dialog->property("visible").toBool())
                    heights << dialog->property("height").toReal();
            });
        QVERIFY(openFilterDialog(ui));
        QObject::disconnect(frames);
        QVERIFY(list->property("count").toInt() > 0);
        QVERIFY(!heights.isEmpty());
        for (const qreal height : std::as_const(heights))
            QCOMPARE(height, dialog->property("height").toReal());
        // Closed, the rows go
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(list->property("count").toInt(), 0);
        // Opened again while it is still closing, the rows stay
        QVERIFY(openFilterDialog(ui));
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::qWait(500);
        QVERIFY(dialog->property("visible").toBool());
        QVERIFY(list->property("count").toInt() > 0);
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

    void aFilterValueThatWouldMakeTheQueryTooLongIsBlocked()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(movies->filterOptions()->available());
        QVERIFY(openFilterDialog(ui));
        QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
        QVERIFY(dialog);
        const QString tag = QStringLiteral("restored-master");
        // A tag row is named by its place in the list, not by its text
        QString rowName;
        QTRY_VERIFY([&] {
            for (const QVariant &entry : dialog->property("tagRows").toList()) {
                const QVariantMap row = entry.toMap();
                if (row.value(QStringLiteral("facet")).toString() == QLatin1String("tags")
                    && row.value(QStringLiteral("value")).toString() == tag)
                    rowName = QStringLiteral("filterOption_")
                              + row.value(QStringLiteral("key")).toString();
            }
            return !rowName.isEmpty();
        }());
        const QString hint = QStringLiteral("Too many filters selected: remove one to add this");
        // The row is looked up on every wait: a changed draft may rebuild it
        QQuickItem *option = nullptr;
        const auto rowShown = [&] {
            revealFilterEntry(ui, QStringLiteral("tags"), tag);
            return (option = shownItem(ui, rowName)) != nullptr;
        };
        QTRY_VERIFY(rowShown());
        QVERIFY(!option->property("blocked").toBool());
        QVERIFY(option->isEnabled());
        QVERIFY(!ui.showsText(hint));
        // A draft that leaves no room for the tag
        Kinefin::LibraryFilterState full = tagsFillingTheQuery(*movies);
        QVERIFY(!full.tags.isEmpty());
        Kinefin::LibraryFilterState withTag = full;
        withTag.tags.append(tag);
        QVERIFY(!movies->filtersFit(Kinefin::LibraryItemsModel::filtersToVariant(withTag)));
        QVERIFY(dialog->setProperty("draft", Kinefin::LibraryItemsModel::filtersToVariant(full)));
        QTRY_VERIFY(rowShown() && option->property("blocked").toBool());
        QVERIFY(!option->isEnabled());
        QCOMPARE(option->property("hint").toString(), hint);
        QTRY_VERIFY(ui.showsText(hint));
        // Neither a click nor the toggle of the dialog adds it
        const auto draftTags = [&] {
            return dialog->property("draft").toMap().value(QStringLiteral("tags")).toList();
        };
        QCOMPARE(draftTags().size(), full.tags.size());
        ui.clickCenter(option);
        QVERIFY(toggleOption(ui, QStringLiteral("tags"), tag));
        QCOMPARE(draftTags().size(), full.tags.size());
        QVERIFY(!draftTags().contains(tag));
        QVERIFY(!option->property("selected").toBool());
        // Taking one tag off makes room again
        Kinefin::LibraryFilterState fewer = full;
        // As many tags off as it takes for this one to fit
        const auto fitsWithTag = [&] {
            Kinefin::LibraryFilterState probe = fewer;
            probe.tags.append(tag);
            return movies->filtersFit(Kinefin::LibraryItemsModel::filtersToVariant(probe));
        };
        while (!fewer.tags.isEmpty() && !fitsWithTag())
            fewer.tags.removeFirst();
        QVERIFY(fitsWithTag());
        QVERIFY(dialog->setProperty("draft", Kinefin::LibraryItemsModel::filtersToVariant(fewer)));
        QTRY_VERIFY(rowShown() && !option->property("blocked").toBool());
        QVERIFY(option->isEnabled());
        QTRY_VERIFY(!ui.showsText(hint));
        QVERIFY(toggleOption(ui, QStringLiteral("tags"), tag));
        QTRY_VERIFY(rowShown() && option->property("selected").toBool());
    }

    void theTagsSectionListsTheFirstTagsAndItsSearchFindsTheOthers()
    {
        Ui ui(m_home->path());
        QJsonArray tags;
        for (int i = 0; i < 350; ++i)
            tags.append(QStringLiteral("tag %1").arg(i, 3, 10, QLatin1Char('0')));
        ui.server.listingRawBody.insert(
            QStringLiteral("Filters"),
            QJsonDocument(QJsonObject{{QStringLiteral("Genres"),
                                       QJsonArray{QJsonObject{
                                           {QStringLiteral("Id"), kSciFi},
                                           {QStringLiteral("Name"), QStringLiteral("Sci-Fi")}}}},
                                      {QStringLiteral("Tags"), tags}})
                .toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_VERIFY(movies->filterOptions()->tagsAvailable());
        // The model keeps every tag, the dialog lists the first 300 and says how many it leaves out
        QCOMPARE(movies->filterOptions()->tagCount(), 350);
        QVERIFY(openFilterDialog(ui));
        QObject *dialog = ui.popup(QStringLiteral("filterDialog"));
        QVERIFY(dialog);
        const auto rows = [&] { return dialog->property("tagRows").toList(); };
        const auto note = [&] { return dialog->property("tagsNote").toString(); };
        const auto draftTags = [&] {
            return dialog->property("draft").toMap().value(QStringLiteral("tags")).toStringList();
        };
        QCOMPARE(rows().size(), 300);
        QCOMPARE(note(), QStringLiteral("50 more tags: use the search to find them"));

        // The field has a name for assistive technology; the first tag rows and the note are plain text
        QQuickItem *field = nullptr;
        const auto fieldShown = [&] {
            revealFilterEntry(ui, QStringLiteral("tags"));
            return (field = shownItem(ui, QStringLiteral("filterTagSearch"))) != nullptr;
        };
        QTRY_VERIFY(fieldShown());
        QCOMPARE(accessibleNameOf(field), QStringLiteral("Search tags"));
        QTRY_VERIFY(shownItem(ui, QStringLiteral("filterOption_tags_0")) != nullptr);
        const QQuickItem *noteLabel = shownItem(ui, QStringLiteral("filterTagsNote"));
        QVERIFY(noteLabel);
        QCOMPARE(noteLabel->property("textFormat").toInt(), int(Qt::PlainText));
        for (QQuickItem *label : ui.itemsWithText(QStringLiteral("tag 000"))) {
            QVERIFY2(label->inherits("QQuickText"), qPrintable(Ui::describe(label)));
            QCOMPARE(label->property("textFormat").toInt(), int(Qt::PlainText));
        }

        // The search is case-insensitive and reaches the tags beyond the first 300
        QVERIFY(field->setProperty("text", QStringLiteral("TAG 33")));
        QTRY_COMPARE(rows().size(), 10);
        QCOMPARE(rows().constFirst().toMap().value(QStringLiteral("value")).toString(),
                 QStringLiteral("tag 330"));
        QCOMPARE(rows().constFirst().toMap().value(QStringLiteral("key")).toString(),
                 QStringLiteral("tags_330"));
        QVERIFY(note().isEmpty());
        QTRY_VERIFY(fieldShown() && shownItem(ui, QStringLiteral("filterOption_tags_330")));
        QTRY_VERIFY(!ui.item(QStringLiteral("filterOption_tags_0")));

        // A checked tag stays listed and checked when a search no longer matches it
        QVERIFY(toggleOption(ui, QStringLiteral("tags"), QStringLiteral("tag 335")));
        QCOMPARE(draftTags(), QStringList{QStringLiteral("tag 335")});
        QVERIFY(field->setProperty("text", QStringLiteral("nothing like it")));
        QTRY_COMPARE(rows().size(), 1);
        QCOMPARE(rows().constFirst().toMap().value(QStringLiteral("value")).toString(),
                 QStringLiteral("tag 335"));
        QCOMPARE(draftTags(), QStringList{QStringLiteral("tag 335")});
        QVERIFY(note().isEmpty());

        // Unchecked and searched again, nothing is left and the note says so
        QVERIFY(toggleOption(ui, QStringLiteral("tags"), QStringLiteral("tag 335")));
        QVERIFY(draftTags().isEmpty());
        QVERIFY(field->setProperty("text", QStringLiteral("still nothing")));
        QTRY_COMPARE(rows().size(), 0);
        QCOMPARE(note(), QStringLiteral("No tags match the search"));

        // Without a search the first 300 are back
        QVERIFY(field->setProperty("text", QString()));
        QTRY_COMPARE(rows().size(), 300);
        QCOMPARE(note(), QStringLiteral("50 more tags: use the search to find them"));
    }

    void aQueryTooLongForTheServerOffersClearFiltersInTheFailureBox()
    {
        Ui ui(m_home->path());
        // A failed facet list prunes nothing, so the tags below stay
        ui.server.listingStatus.insert(QStringLiteral("Filters"), 500);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        QTRY_COMPARE(movies->filterOptions()->status(), Kinefin::FilterOptionsModel::Failed);
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Ready);
        const QString message = QStringLiteral("The filters and search are too long for the "
                                               "server: clear the filters or shorten the search");
        QVERIFY(!ui.showsText(message));
        // The editor fills the query with tags; the longest search term then no longer fits
        const Kinefin::LibraryFilterState full = tagsFillingTheQuery(*movies);
        movies->setFilterState(full);
        QVERIFY(movies->filterState() == full);
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Ready);
        movies->setSearchDebounceInterval(0);
        movies->setSearchTerm(QString(Kinefin::SearchModel::MaxTermLength, QChar(0x20ac)));
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Failed);
        QVERIFY(movies->queryTooLong());
        QTRY_VERIFY(ui.showsText(message));
        QObject *clear = ui.popup(QStringLiteral("libraryFailedClearFilters"));
        QVERIFY(clear);
        QTRY_VERIFY(clear->property("visible").toBool());
        // Clear Filters takes the filters and leaves the search, which fits alone
        QVERIFY(triggerAction(ui, QStringLiteral("libraryFailedClearFilters")));
        QTRY_VERIFY(movies->filterState().isEmpty());
        QTRY_COMPARE(movies->status(), Kinefin::LibraryItemsModel::Ready);
        QVERIFY(!movies->queryTooLong());
        QCOMPARE(movies->searchTerm().size(), int(Kinefin::SearchModel::MaxTermLength));
        QTRY_VERIFY(!ui.showsText(message));
        QTRY_VERIFY(!clear->property("visible").toBool());
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
        // Clear Filters takes the filters and nothing else: not the sort or the library
        QQuickItem *clear = nullptr;
        QTRY_VERIFY((clear = shownItem(ui, QStringLiteral("clearFiltersButton"))) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(clear, "click"));
        QTRY_VERIFY(movies->filterState().isEmpty());
        QCOMPARE(movies->sortBy(), QStringLiteral("Runtime"));
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
              QStringLiteral("OfficialRating"), QStringLiteral("AiredEpisodeOrder"),
              QStringLiteral("CommunityRating"), QStringLiteral("CriticRating"),
              QStringLiteral("Runtime"), QStringLiteral("DatePlayed"),
              QStringLiteral("SeriesDatePlayed"), QStringLiteral("PlayCount"),
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

        QVERIFY(triggerAction(ui, QStringLiteral("sortKey_SortName")));
        QTRY_VERIFY(!ui.popup(QStringLiteral("reshuffleAction"))->property("visible").toBool());
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

    void anEmptyLocalSearchCanBeClearedFromThePlaceholder()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 0;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        auto *movies = ui.controller.movies();
        movies->setSearchTerm(QStringLiteral("no matching film"));
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryFilteredEmpty")) != nullptr);
        QQuickItem *clear = nullptr;
        QTRY_VERIFY((clear = [&]() -> QQuickItem * {
                        for (QQuickItem *candidate :
                             allItems(ui.item(QStringLiteral("libraryFilteredEmpty")))) {
                            if (candidate->property("text").toString()
                                    == QStringLiteral("Clear Filters")
                                && candidate->isVisible() && candidate->isEnabled()
                                && candidate->width() > 0)
                                return candidate;
                        }
                        return nullptr;
                    }())
                    != nullptr);
        QVERIFY(QMetaObject::invokeMethod(clear, "click"));
        QTRY_COMPARE(movies->searchTerm(), QString());
        QTRY_VERIFY(shownItem(ui, QStringLiteral("libraryEmpty")) != nullptr);
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

    void theSubtabEntriesFollowTheSettingsWhileTheAppRuns()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *subtabs = ui.controller.subtabs();
        // Favorites and Collections are on at first, and the main list is no entry
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("favorites"), QStringLiteral("collections")}));
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("movies.genres"), true);
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("favorites"), QStringLiteral("collections"),
                                  QStringLiteral("genres")}));
        // The Genres entry takes the place of the Browse by Genre button
        QTRY_VERIFY(toolbarButton(ui, QStringLiteral("browseGenresAction")) == nullptr);
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("movies.favorites"), false);
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("collections"), QStringLiteral("genres")}));
        settleToolbar(ui);
        // A sub-tab turned off while it shows gives way to the main one
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("genres")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("genres")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("genres"));
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("movies.genres"), false);
        QTRY_COMPARE(subtabs->current(), QStringLiteral("main"));
        settleToolbar(ui);
        // No sub-tab on: no entry, the toolbar keeps the list actions
        setSubtabSetting(ui, QStringLiteral("movies.collections"), false);
        QTRY_VERIFY(subtabIds(ui).isEmpty());
        QTRY_VERIFY(toolbarButton(ui, QStringLiteral("filtersAction")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        settleToolbar(ui);
    }

    void aSubtabEntryTogglesBackToTheMainList()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *subtabs = ui.controller.subtabs();
        const auto marked = [&](const QString &id) {
            const QQuickItem *button = subtabButton(ui, id);
            return button && button->property("checked").toBool();
        };
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("favorites")) != nullptr);
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("collections")) != nullptr);
        settleToolbar(ui);
        QCOMPARE(subtabs->current(), QStringLiteral("main"));
        QVERIFY(!marked(QStringLiteral("favorites")));
        QVERIFY(selectSubtab(ui, QStringLiteral("favorites")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("favorites"));
        QTRY_VERIFY(marked(QStringLiteral("favorites")));
        QVERIFY(!marked(QStringLiteral("collections")));
        settleToolbar(ui);
        // Another entry takes the mark from it
        QVERIFY(selectSubtab(ui, QStringLiteral("collections")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("collections"));
        QTRY_VERIFY(marked(QStringLiteral("collections")));
        QTRY_VERIFY(!marked(QStringLiteral("favorites")));
        settleToolbar(ui);
        // The entry that is checked leads back to the main list, and no entry is marked
        QVERIFY(selectSubtab(ui, QStringLiteral("collections")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("main"));
        QTRY_VERIFY(!marked(QStringLiteral("collections")));
        QVERIFY(!marked(QStringLiteral("favorites")));
        QTRY_VERIFY(ui.item(QStringLiteral("libraryGrid")) != nullptr);
        settleToolbar(ui);
    }

    void theRootTabPageHasNoTitleRow_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("title");
        QTest::newRow("movies") << QStringLiteral("library") << QStringLiteral("Movies");
        QTest::newRow("shows") << QStringLiteral("tvTab") << QStringLiteral("Shows");
    }

    void theRootTabPageHasNoTitleRow()
    {
        QFETCH(QString, state);
        QFETCH(QString, title);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, state));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        const QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        QVERIFY(page);
        // The title stays: the page property, the window title and the accessible name of the list
        QCOMPARE(page->property("title").toString(), title);
        QCOMPARE(ui.window->title(), QStringLiteral("Kinefin"));
        const QQuickItem *content = ui.item(QStringLiteral("listContent"));
        QVERIFY(content);
        QTRY_VERIFY(content->property("item").value<QObject *>() != nullptr);
        QCOMPARE(
            content->property("item").value<QObject *>()->property("accessibleName").toString(),
            title);
        // No toolbar row above the page, and no label of the page that repeats the tab's name
        QTRY_VERIFY(page->mapToScene(QPointF(0, 0)).y() <= ui.shellHeader()->height() + 1);
        for (const QQuickItem *label : ui.itemsWithText(title)) {
            if (label->isVisible() && !ui.shellHeader()->isAncestorOf(label))
                QFAIL(
                    qPrintable(QStringLiteral("a title label shows: %1").arg(Ui::describe(label))));
        }
        // Back and the header keep working from here: the page is the first of the stack
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(ui.tabButtons().size() >= 2);
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
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("collections")) != nullptr);
        QVERIFY(selectSubtab(ui, QStringLiteral("collections")));
        QTRY_COMPARE(ui.controller.subtabs()->current(), QStringLiteral("collections"));
        // The collections of the user, over every library
        QTRY_VERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("includeItemTypes"))
                    == QStringLiteral("BoxSet"));
        QVERIFY(lastItemsQuery(ui).queryItemValue(QStringLiteral("parentId")).isEmpty());
        QVERIFY(ui.server.listingRoutes.value(QStringLiteral("MovieRecommendations")).isEmpty());
        // Suggestions load when they show, as cards in rows with the wording built from the kind
        setSubtabSetting(ui, QStringLiteral("movies.suggestions"), true);
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("suggestions")) != nullptr);
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
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("favorites")) != nullptr);
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
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("favorites")) != nullptr);
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
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("networks")) != nullptr);
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
        // The toolbar stays the top row: the way back sits in it, and no caption row comes above
        QQuickItem *back = shownItem(ui, QStringLiteral("networkBackButton"));
        QQuickItem *toolbar = shownItem(ui, QStringLiteral("libraryToolbar"));
        QVERIFY(toolbar);
        QVERIFY(qAbs(back->mapToScene(QPointF(0, back->height() / 2)).y()
                     - toolbar->mapToScene(QPointF(0, toolbar->height() / 2)).y())
                < 1);
        QVERIFY(!shownItem(ui, QStringLiteral("crossLibraryLabel")));
        // The network open is named beside the way back, as plain text
        QQuickItem *networkName = shownItem(ui, QStringLiteral("networkNameLabel"));
        QVERIFY(networkName);
        QCOMPARE(networkName->property("textFormat").toInt(), int(Qt::PlainText));
        QVERIFY(networkName->property("text").toString().contains(hostile));
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

    void tvShowsSubtabsMatchMovies()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("tvTab")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        auto *subtabs = ui.controller.subtabs();
        // Favorites and Collections are on at first, as in Movies, and the main list is no entry
        QTRY_COMPARE(subtabIds(ui),
                     (QStringList{QStringLiteral("favorites"), QStringLiteral("collections")}));
        settleToolbar(ui);
        QVERIFY(selectSubtab(ui, QStringLiteral("favorites")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("favorites"));
        QTRY_COMPARE(subtabs->status(), Kinefin::LibrarySubtabController::Ready);
        QCOMPARE(subtabs->activeList(), subtabs->showList());
        const QUrlQuery query = lastItemsQuery(ui);
        QCOMPARE(query.queryItemValue(QStringLiteral("includeItemTypes")),
                 QStringLiteral("Series"));
        QVERIFY(
            query.queryItemValue(QStringLiteral("filters")).contains(QStringLiteral("IsFavorite")));
        QCOMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        settleToolbar(ui);
        // The switches turn them off, and the showing one gives way to the main list
        setSubtabSetting(ui, QStringLiteral("tv.favorites"), false);
        QTRY_COMPARE(subtabs->current(), QStringLiteral("main"));
        QTRY_COMPARE(subtabIds(ui), QStringList{QStringLiteral("collections")});
        settleToolbar(ui);
        setSubtabSetting(ui, QStringLiteral("tv.collections"), false);
        QTRY_VERIFY(subtabIds(ui).isEmpty());
        settleToolbar(ui);
    }

    void theEpisodesSubtabShowsTheCountAndItsOwnFilters()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 40;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("tvTab")));
        setSubtabSetting(ui, QStringLiteral("tv.episodes"), true);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("episodes")) != nullptr);
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
        QTRY_VERIFY(subtabButton(ui, QStringLiteral("collections")) != nullptr);
        settleToolbar(ui);
        // The header comes first: the tabs, the search box, the queue and the menu button, which sits at the right end
        QQuickItem *firstTab = ui.tabButtons().value(0);
        QVERIFY(firstTab);
        firstTab->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(firstTab->hasActiveFocus());
        const QList<QQuickItem *> visited = ui.tabCycle();
        const auto indexOfItem = [&](const QString &name) {
            const QQuickItem *target = ui.item(name);
            for (qsizetype i = 0; i < visited.size(); ++i) {
                if (visited.at(i) == target || (target && target->isAncestorOf(visited.at(i))))
                    return i;
            }
            return qsizetype(-1);
        };
        // After the tabs of the shell: the toolbar, the grid. Kirigami stacks the buttons of an ActionToolBar last to first, and the Tab chain follows the stacking, so the order inside the toolbar is not asserted: its buttons (the sub-tab entries and the list actions) are one block of stops
        const auto indexOfButton = [&](const QString &actionName) {
            const QQuickItem *button = toolbarButton(ui, actionName);
            return button ? visited.indexOf(button) : qsizetype(-1);
        };
        const QQuickItem *toolbar = ui.item(QStringLiteral("libraryToolbar"));
        QVERIFY(toolbar);
        qsizetype first = visited.size();
        qsizetype last = -1;
        qsizetype inToolbar = 0;
        for (qsizetype i = 0; i < visited.size(); ++i) {
            if (!toolbar->isAncestorOf(visited.at(i)))
                continue;
            first = qMin(first, i);
            last = qMax(last, i);
            ++inToolbar;
        }
        QVERIFY(inToolbar >= 3);
        QCOMPARE(last - first + 1, inToolbar);
        QVERIFY(first > 3);
        for (const QString &name :
             {QStringLiteral("subtab_favorites"), QStringLiteral("subtab_collections"),
              QStringLiteral("filtersAction")}) {
            const qsizetype at = indexOfButton(name);
            QVERIFY2(at >= first && at <= last, qPrintable(name));
        }
        const qsizetype gridAt = indexOfItem(QStringLiteral("libraryGrid"));
        QVERIFY(gridAt > last);
        for (QQuickItem *item : visited) {
            QVERIFY2(item->isVisible(), qPrintable(Ui::describe(item)));
            QVERIFY2(ui.insideWindow(item), qPrintable(Ui::describe(item)));
        }
    }

    void theUiIsQuietWhenNothingChanges_data()
    {
        QTest::addColumn<QString>("state");
        for (const QString &state :
             {QStringLiteral("home"), QStringLiteral("library"), QStringLiteral("tvTab"),
              QStringLiteral("favorites"), QStringLiteral("queue"), QStringLiteral("details"),
              QStringLiteral("settings"), QStringLiteral("preferences"), QStringLiteral("servers"),
              QStringLiteral("login")})
            QTest::newRow(qPrintable(state)) << state;
    }

    // The offscreen platform has no vsync: a scene that animates or asks for an update all the time swaps frames back to back (an endless animation of a root item measured 36 frames in 600 ms), one that is idle swaps none; a few are allowed for a fade that ends late
    void theUiIsQuietWhenNothingChanges()
    {
        QFETCH(QString, state);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY2(buildState(ui, state), qPrintable(state));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QTRY_VERIFY(!ui.tabSwitching());
        if (ui.item(QStringLiteral("libraryToolbar")))
            settleToolbar(ui);
        ui.settle();
        QTest::qWait(500);
        int frames = 0;
        const QMetaObject::Connection counter = QObject::connect(
            ui.window, &QQuickWindow::frameSwapped, ui.window, [&frames] { ++frames; });
        QTest::qWait(600);
        QObject::disconnect(counter);
        QVERIFY2(frames <= 3,
                 qPrintable(QStringLiteral("%1: %2 frames in 600 ms").arg(state).arg(frames)));
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
        for (const QQuickItem *item : visited)
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
        // `labelled` is 1 or 0 where the width settles it, and -1 where the labels' own measure decides: the checks below read the property then
        const struct Mode {
            int width;
            bool wide;
            bool compact;
            int labelled;
        } modes[] = {{wideLimit * 2, true, false, 1},
                     {wideLimit + 10, true, false, -1},
                     {compactLimit + 40, false, false, 0},
                     {compactLimit + 1, false, false, 0},
                     {compactLimit - 40, false, true, 0}};
        for (const Mode &mode : modes) {
            QVERIFY2(ui.opensWindowOfWidth(mode.width), qPrintable(QString::number(mode.width)));
            // A resize reaches the QML bindings that read the width on the next event loop pass: QTRY lets them settle
            QTRY_VERIFY2(ui.shellHeader()->property("compact").toBool() == mode.compact,
                         qPrintable(QStringLiteral("%1 px: compact %2, want %3")
                                        .arg(mode.width)
                                        .arg(ui.shellHeader()->property("compact").toBool())
                                        .arg(mode.compact)));
            QTRY_VERIFY2(ui.window->property("wideHeader").toBool() == mode.wide,
                         qPrintable(QStringLiteral("%1 px: wide %2, want %3")
                                        .arg(mode.width)
                                        .arg(ui.window->property("wideHeader").toBool())
                                        .arg(mode.wide)));
            if (mode.labelled >= 0)
                QTRY_VERIFY2(ui.shellHeader()->property("labelledTabs").toBool()
                                 == (mode.labelled == 1),
                             qPrintable(QStringLiteral("%1 px: labelledTabs is not %2")
                                            .arg(mode.width)
                                            .arg(mode.labelled)));
            QStringList problems =
                ui.modeProblems(ui.shellHeader()->property("labelledTabs").toBool(), mode.compact);
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
                    // A separator has no action to name
                    QObject *action =
                        entry ? entry->property("action").value<QObject *>() : nullptr;
                    if (entry && entry->isVisible() && action)
                        names << action->objectName();
                }
                return names;
            }();
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            // The desktop style fades a menu out: opening it again before that ends leaves it closed
            QTRY_VERIFY(!menu->property("visible").toBool());
            if (mode.compact) {
                for (const char *name : {"compactQueueAction", "settingsAction", "shortcutsAction",
                                         "aboutAction", "adminDashboardAction"})
                    QVERIFY2(shown.contains(QLatin1String(name)),
                             qPrintable(QStringLiteral("%1 px: %2 missing")
                                            .arg(mode.width)
                                            .arg(QLatin1String(name))));
            } else {
                for (const char *name : {"compactQueueAction"})
                    QVERIFY2(!shown.contains(QLatin1String(name)),
                             qPrintable(QStringLiteral("%1 px: %2 shown")
                                            .arg(mode.width)
                                            .arg(QLatin1String(name))));
                for (const char *name : {"settingsAction", "shortcutsAction", "aboutAction"})
                    QVERIFY2(shown.contains(QLatin1String(name)),
                             qPrintable(shown.join(QLatin1Char('|'))));
            }
            // Favorites and Playlists are tabs of the bar in every mode: their old header buttons and menu entries are gone
            for (const char *name : {"compactFavoritesAction", "playlistsAction"})
                QVERIFY2(!ui.shellAction(QLatin1String(name)), name);
            for (const char *name : {"favoritesButton", "playlistsButton", "accountButton"})
                QVERIFY2(!ui.item(QLatin1String(name)), name);
            // The page keeps its place under the bar in every mode
            QTRY_VERIFY2((problems = ui.layoutProblems()).isEmpty(),
                         qPrintable(problems.join(QLatin1Char('\n'))));
        }
    }

    void favoritesAndPlaylistsAreStackRoots()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1200));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QTRY_VERIFY(!ui.controller.libraries().isEmpty());
        const struct {
            const char *label;
            Qt::Key key;
            const char *root;
            const char *page;
            int index;
        } tabs[] = {{"Favorites", Qt::Key_5, "favorites", "favoritesPage", 4},
                    {"Playlists", Qt::Key_6, "playlists", "playlistsPage", 5}};
        for (const auto &tab : tabs) {
            for (const bool byKey : {false, true}) {
                const QString root = QString::fromLatin1(tab.root);
                const QString page = QString::fromLatin1(tab.page);
                if (byKey)
                    QTest::keyClick(ui.window, tab.key, Qt::ControlModifier);
                else
                    QVERIFY(ui.clickTab(QString::fromLatin1(tab.label)));
                QTRY_COMPARE(ui.rootTab(), root);
                QTRY_COMPARE(ui.currentDetails()->objectName(), page);
                QTRY_VERIFY(!ui.tabSwitching());
                QCOMPARE(ui.stackDepth(), 1);
                // The type tabs come with the libraries: all six must be there before one is read by its place
                QTRY_COMPARE(ui.tabButtons().size(), 6);
                for (int index = 0; index < 6; ++index)
                    QTRY_COMPARE(ui.tabButtons().value(index)->property("checked").toBool(),
                                 index == tab.index);
                // Back at the root of a tab does nothing, as on Home
                QTest::keyClick(ui.window, Qt::Key_Escape);
                QTest::qWait(200);
                QCOMPARE(ui.stackDepth(), 1);
                QCOMPARE(ui.currentDetails()->objectName(), page);
                // Home between the two ways in, so that each one opens the tab afresh
                QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
                QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
                QTRY_VERIFY(!ui.tabSwitching());
            }
        }
        // A page above the root is popped by a click on the tab of the root
        QVERIFY(ui.clickTab(QStringLiteral("Favorites")));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QQuickItem *showAll = nullptr;
        QTRY_VERIFY((showAll = ui.item(QStringLiteral("favoritesShowAll_1"))) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(showAll, "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesGroupPage"));
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QTRY_VERIFY(ui.tabButtons().value(4)->property("checked").toBool());
        QVERIFY(ui.clickTab(QStringLiteral("Favorites")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.rootTab(), QStringLiteral("favorites"));
        // The same for the details of an item opened above Playlists
        QVERIFY(ui.clickTab(QStringLiteral("Playlists")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openItem",
                                          Q_ARG(QVariant, kMovieId), Q_ARG(QVariant, QVariant())));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("playlists"));
        QVERIFY(ui.clickTab(QStringLiteral("Playlists")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QTRY_COMPARE(ui.stackDepth(), 1);
    }

    void theOneMenuHoldsTheAccountAndTheApp()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signInAsUser(QStringLiteral("Alice")));
        QVERIFY(ui.opensWindowOfWidth(1200));
        QTRY_VERIFY(!ui.shellHeader()->property("compact").toBool());
        QObject *menu = ui.openMainMenuByClick();
        QVERIFY(menu);
        QQuickItem *header = qobject_cast<QQuickItem *>(ui.popup(QStringLiteral("mainMenuHeader")));
        QVERIFY(header && header->isVisible());
        // A plain item: no action, and no entry for the pointer or the keys to stop on
        QVERIFY(!header->property("action").isValid());
        // The user name and the server address without its scheme, both as plain text
        QString serverAddress = ui.controller.serverUrl();
        serverAddress.remove(QRegularExpression(QStringLiteral("^https?://"),
                                                QRegularExpression::CaseInsensitiveOption));
        QVERIFY(!serverAddress.isEmpty());
        int names = 0;
        int addresses = 0;
        for (QQuickItem *candidate : allItems(header)) {
            if (!candidate->property("text").isValid())
                continue;
            const QString text = candidate->property("text").toString();
            if (text != QLatin1String("Alice") && text != serverAddress)
                continue;
            QCOMPARE(candidate->property("textFormat").toInt(), int(Qt::PlainText));
            (text == serverAddress ? addresses : names)++;
        }
        QCOMPARE(names, 1);
        QCOMPARE(addresses, 1);
        // The entries in the order of the contract, those the session and the width show: a subsequence of the full list
        const QStringList contract =
            QStringLiteral(
                "mainMenuHeader switchUserItem signOutItem switchServerItem "
                "menuQueueItem menuRemoteItem menuSyncPlayItem menuAdminItem menuRefreshItem "
                "menuSettingsItem menuShortcutsItem menuAboutItem")
                .split(QLatin1Char(' '));
        const QStringList shown = ui.shownMenuNames(QStringLiteral("mainMenu"));
        qsizetype at = -1;
        for (const QString &name : shown) {
            const qsizetype position = contract.indexOf(name);
            QVERIFY2(position > at, qPrintable(shown.join(QLatin1Char(' '))));
            at = position;
        }
        for (const QString &name :
             {QStringLiteral("mainMenuHeader"), QStringLiteral("switchUserItem"),
              QStringLiteral("signOutItem"), QStringLiteral("switchServerItem"),
              QStringLiteral("menuRefreshItem"), QStringLiteral("menuSettingsItem"),
              QStringLiteral("menuShortcutsItem"), QStringLiteral("menuAboutItem")})
            QVERIFY2(shown.contains(name), qPrintable(name));
        // The search and the queue are in the bar at this width
        QVERIFY(!shown.contains(QStringLiteral("menuQueueItem")));
        // Under the button, right-aligned with it, inside the window
        const QQuickItem *button = ui.item(QStringLiteral("mainMenuButton"));
        const QPointF origin =
            button->mapToScene(QPointF(menu->property("x").toReal(), menu->property("y").toReal()));
        const QRectF rect(
            origin, QSizeF(menu->property("width").toReal(), menu->property("height").toReal()));
        const QRectF buttonRect = button->mapRectToScene(button->boundingRect());
        QVERIFY2(rect.left() >= -1 && rect.right() <= ui.window->width() + 1
                     && rect.bottom() <= ui.window->height() + 1,
                 qPrintable(QStringLiteral("menu outside the window: %1,%2 %3x%4")
                                .arg(rect.x())
                                .arg(rect.y())
                                .arg(rect.width())
                                .arg(rect.height())));
        QVERIFY(qAbs(rect.right() - buttonRect.right()) <= 2);
        QVERIFY(rect.top() >= buttonRect.bottom() - 2);
        QVERIFY(Ui::closeMenu(menu));
    }

    void theSignedOutMenuOffersOnlyTheAppEntries()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1200));
        // No server chosen yet: the server picker
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        QQuickItem *button = ui.item(QStringLiteral("mainMenuButton"));
        QVERIFY(button && button->isVisible() && button->isEnabled());
        const QQuickItem *avatar = ui.item(QStringLiteral("accountAvatar"));
        QVERIFY(!avatar || !avatar->isVisible());
        QCOMPARE(accessibleNameOf(button), QStringLiteral("Main Menu"));
        QObject *menu = ui.openMainMenuByClick();
        QVERIFY(menu);
        QStringList shown = ui.shownMenuNames(QStringLiteral("mainMenu"));
        for (const char *name : {"mainMenuHeader", "switchUserItem", "signOutItem",
                                 "switchServerItem", "menuQueueItem"})
            QVERIFY2(!shown.contains(QLatin1String(name)), name);
        for (const char *name : {"menuSettingsItem", "menuShortcutsItem", "menuAboutItem"})
            QVERIFY2(shown.contains(QLatin1String(name)), name);
        // The hidden header and separators take no room: the first shown entry sits at the top of the list
        const QList<QQuickItem *> entries = ui.menuItems(QStringLiteral("mainMenu"));
        const auto first =
            std::find_if(entries.cbegin(), entries.cend(),
                         [](const QQuickItem *entry) { return entry && entry->isVisible(); });
        QVERIFY(first != entries.cend());
        QVERIFY2((*first)->y() <= 1, qPrintable(QString::number((*first)->y())));
        QVERIFY(Ui::closeMenu(menu));
        // A server is chosen and the sign-in shows: Switch Server joins, the account entries stay out
        QVERIFY(buildState(ui, QStringLiteral("login")));
        QTRY_VERIFY(ui.item(QStringLiteral("mainMenuButton"))->isVisible());
        avatar = ui.item(QStringLiteral("accountAvatar"));
        QVERIFY(!avatar || !avatar->isVisible());
        menu = ui.openMainMenuByClick();
        QVERIFY(menu);
        shown = ui.shownMenuNames(QStringLiteral("mainMenu"));
        for (const char *name : {"mainMenuHeader", "switchUserItem", "signOutItem"})
            QVERIFY2(!shown.contains(QLatin1String(name)), name);
        for (const char *name :
             {"switchServerItem", "menuSettingsItem", "menuShortcutsItem", "menuAboutItem"})
            QVERIFY2(shown.contains(QLatin1String(name)), name);
        QVERIFY(Ui::closeMenu(menu));
    }

    void theSmallestWindowKeepsTheCompactBarWholeWithASearchButton()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const int minimumWidth = ui.window->minimumWidth();
        const int minimumHeight = ui.window->minimumHeight();
        QVERIFY(minimumWidth > 0 && minimumWidth < ui.window->property("compactLimit").toInt());
        QVERIFY(minimumHeight > 0 && minimumHeight < 640);
        ui.window->resize(minimumWidth, minimumHeight);
        QTRY_COMPARE(ui.window->size(), QSize(minimumWidth, minimumHeight));
        QTRY_VERIFY(ui.shellHeader()->property("compact").toBool());
        QTRY_COMPARE(ui.tabButtons().size(), 6);
        // The box becomes a search button, and the controls of the bar stay clear of each other and inside the window
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QQuickItem *button = ui.item(QStringLiteral("headerSearchButton"));
        QQuickItem *menuButton = ui.item(QStringLiteral("mainMenuButton"));
        QQuickItem *tabs = ui.item(QStringLiteral("headerTabs"));
        QVERIFY(field && button && menuButton && tabs);
        QTRY_VERIFY(!field->isVisible() && button->isVisible() && menuButton->isVisible());
        const auto left = [](const QQuickItem *item) {
            return item->mapToScene(QPointF(0, 0)).x();
        };
        const auto right = [](const QQuickItem *item) {
            return item->mapToScene(QPointF(item->width(), 0)).x();
        };
        QTRY_VERIFY2(right(tabs) <= left(button) && right(button) <= left(menuButton)
                         && right(menuButton) <= ui.window->width(),
                     qPrintable(QStringLiteral("tabs end %1, search %2-%3, menu %4-%5, window %6")
                                    .arg(right(tabs))
                                    .arg(left(button))
                                    .arg(right(button))
                                    .arg(left(menuButton))
                                    .arg(right(menuButton))
                                    .arg(ui.window->width())));
        QStringList problems;
        QTRY_VERIFY2((problems = ui.layoutProblems()).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
        // The button does what Ctrl+F does in Compact mode: the search page with its own field, which takes the keyboard
        ui.clickCenter(button);
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("searchField"))
                    && ui.item(QStringLiteral("searchField"))->hasActiveFocus());
        QTRY_VERIFY(button->property("down").toBool());
        // The menu no longer offers a search entry
        QVERIFY(!ui.shellAction(QStringLiteral("compactSearchAction")));
    }

    void theTabLabelsFoldWhenTheyDoNotFit()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const int compactLimit = ui.window->property("compactLimit").toInt();
        // The tooltip of a tab: attached to the button, so it is read through the QML context
        const auto tooltipOf = [](QQuickItem *tab) {
            return QQmlProperty(tab, QStringLiteral("QQC2.ToolTip.text"), qmlContext(tab))
                .read()
                .toString();
        };
        const auto labelsShow = [&ui](bool labelled) {
            const QList<QQuickItem *> tabs = ui.tabButtons();
            if (tabs.size() != 6)
                return false;
            for (const QQuickItem *tab : tabs) {
                if (tab->property("display").toInt() != (labelled ? 2 : 0))
                    return false;
            }
            return true;
        };
        // Room for every label
        QVERIFY(ui.opensWindowOfWidth(2400));
        QTRY_VERIFY(ui.shellHeader()->property("labelledTabs").toBool());
        QTRY_VERIFY(labelsShow(true));
        QStringList problems;
        QTRY_VERIFY2((problems = ui.modeProblems(true, false)).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
        // Just above the limit of the compact mode the labels do not fit: icons, each with its name as the tooltip
        QVERIFY(ui.opensWindowOfWidth(compactLimit + 1));
        QTRY_VERIFY(!ui.shellHeader()->property("compact").toBool());
        QTRY_VERIFY(!ui.shellHeader()->property("labelledTabs").toBool());
        QTRY_VERIFY(labelsShow(false));
        for (QQuickItem *tab : ui.tabButtons())
            QCOMPARE(tooltipOf(tab), tab->property("text").toString());
        // The narrowest bar still keeps the tabs clear of the box, which keeps the queue clear of itself
        const QQuickItem *tabsRow = ui.item(QStringLiteral("headerTabs"));
        const QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(tabsRow && field);
        QTRY_VERIFY2(tabsRow->mapToScene(QPointF(tabsRow->width(), 0)).x()
                         <= field->mapToScene(QPointF(0, 0)).x(),
                     qPrintable(QStringLiteral("tabs end at %1, the box starts at %2")
                                    .arg(tabsRow->mapToScene(QPointF(tabsRow->width(), 0)).x())
                                    .arg(field->mapToScene(QPointF(0, 0)).x())));
        QTRY_VERIFY2((problems = ui.modeProblems(false, false)).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
        QTRY_VERIFY2((problems = ui.layoutProblems()).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
        // Back and forth over the edge: no binding loop (a warning fails the harness) and the same answer each time
        for (const int width : {2400, compactLimit + 1, 2400, compactLimit + 1}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            const bool wide = width == 2400;
            QTRY_COMPARE(ui.shellHeader()->property("labelledTabs").toBool(), wide);
            QTRY_VERIFY(labelsShow(wide));
        }
        // Compact: never labelled
        QVERIFY(ui.opensWindowOfWidth(compactLimit - 40));
        QTRY_VERIFY(ui.shellHeader()->property("compact").toBool());
        QTRY_VERIFY(!ui.shellHeader()->property("labelledTabs").toBool());
        QTRY_VERIFY2((problems = ui.modeProblems(false, true)).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
    }

    void theShortcutsDialogListsAllTabs()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QObject *dialog = ui.popup(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        const QVariantList rows = dialog->property("rows").value<QJSValue>().toVariant().toList();
        QStringList keys;
        QHash<QString, QString> labels;
        for (const QVariant &row : rows) {
            const QVariantList cells = row.toList();
            keys << cells.value(0).toString();
            labels.insert(cells.value(0).toString(), cells.value(1).toString());
        }
        const qsizetype home = keys.indexOf(QStringLiteral("Ctrl+1"));
        QVERIFY(home >= 0);
        const QStringList wanted{QStringLiteral("Home"),      QStringLiteral("Movies"),
                                 QStringLiteral("Shows"),     QStringLiteral("Downloads"),
                                 QStringLiteral("Favorites"), QStringLiteral("Playlists")};
        for (qsizetype i = 0; i < wanted.size(); ++i) {
            QCOMPARE(keys.value(home + i), QStringLiteral("Ctrl+%1").arg(i + 1));
            QCOMPARE(labels.value(keys.value(home + i)), wanted.at(i));
        }
    }

    void theHeaderTabMarksTheCurrentSection()
    {
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QCOMPARE(ui.tabNames(),
                 QStringList({QStringLiteral("Home"), QStringLiteral("Movies"),
                              QStringLiteral("Shows"), QStringLiteral("Downloads"),
                              QStringLiteral("Favorites"), QStringLiteral("Playlists")}));
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
        QObject *about = ui.popup(QStringLiteral("aboutDialog"));
        QVERIFY(about);
        QTRY_VERIFY(about->property("opened").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("mainMenuButton"))->property("down").toBool());
        QVERIFY(QMetaObject::invokeMethod(about, "close"));
        // A popup still fading out blocks every shortcut outside it: wait for the end of the exit transition
        QTRY_VERIFY(!about->property("visible").toBool());
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
        // The Shows tab replaces the Movies one
        QVERIFY(ui.clickTab(QStringLiteral("Shows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTRY_VERIFY(tabChecked(2, true));
        QVERIFY(tabChecked(1, false));
        QVERIFY(tabChecked(0, false));
        // Favorites and Playlists are roots like the others: each one alone is marked
        const struct {
            int index;
            const char *name;
            const char *root;
        } roots[] = {{4, "Favorites", "favorites"}, {5, "Playlists", "playlists"}};
        for (const auto &entry : roots) {
            QVERIFY(ui.clickTab(QLatin1String(entry.name)));
            QTRY_COMPARE(ui.rootTab(), QString::fromLatin1(entry.root));
            for (int other = 0; other < 6; ++other)
                QTRY_VERIFY(tabChecked(other, other == entry.index));
        }
    }

    void theTabsKeepTheirGeometryWhateverTabIsCurrent_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("aboveCompactLimit");
        // Wide enough for the Refresh button of Home (the bar loses its room there) and a narrow window without it: the width is then a margin over the limit of the compact mode
        QTest::newRow("wide") << 1200 << false;
        QTest::newRow("narrow") << 60 << true;
    }

    void theTabsKeepTheirGeometryWhateverTabIsCurrent()
    {
        QFETCH(int, width);
        QFETCH(bool, aboveCompactLimit);
        Ui ui(m_home->path());
        ui.serveSearch();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        if (aboveCompactLimit)
            width += ui.window->property("compactLimit").toInt();
        QVERIFY(ui.opensWindowOfWidth(width));
        QTRY_VERIFY(!ui.shellHeader()->property("compact").toBool());
        const auto geometries = [&ui] {
            QList<QRectF> rects;
            for (QQuickItem *tab : ui.tabButtons())
                rects << QRectF(tab->mapToScene(QPointF(0, 0)),
                                QSizeF(tab->width(), tab->height()));
            return rects;
        };
        // The underline of the current tab sits in the background of the button, and shows on the checked one only
        const auto indicatorShows = [](QQuickItem *tab) {
            for (QQuickItem *candidate : allItems(tab)) {
                if (candidate->objectName() == QLatin1String("tabIndicator"))
                    return candidate->isVisible();
            }
            return false;
        };
        const auto checkState = [&](int current, const QList<QRectF> &reference) {
            QTest::mouseMove(ui.window, QPoint(width / 2, 400));
            QTRY_COMPARE(geometries(), reference);
            const QList<QQuickItem *> tabs = ui.tabButtons();
            QCOMPARE(tabs.size(), 6);
            for (int i = 0; i < tabs.size(); ++i) {
                QTRY_COMPARE(tabs[i]->property("checked").toBool(), i == current);
                QTRY_COMPARE(indicatorShows(tabs[i]), i == current);
            }
            // The tabs are sized by their content, so only their row and height are shared
            for (const QRectF &rect : reference) {
                QCOMPARE(rect.y(), reference[0].y());
                QCOMPARE(rect.height(), reference[0].height());
            }
        };
        QTRY_VERIFY(geometries().size() == 6);
        QTest::qWait(300);
        const QList<QRectF> reference = geometries();
        checkState(0, reference);
        const struct {
            const char *name;
            const char *root;
            int index;
        } steps[] = {{"Movies", "movies", 1},       {"Shows", "tvshows", 2},
                     {"Downloads", "downloads", 3}, {"Favorites", "favorites", 4},
                     {"Playlists", "playlists", 5}, {"Home", "home", 0}};
        for (const auto &step : steps) {
            QVERIFY(ui.clickTab(QLatin1String(step.name)));
            QTRY_COMPARE(ui.rootTab(), QString::fromLatin1(step.root));
            checkState(step.index, reference);
        }
        // The hover and the keyboard focus frame sit inside the button too
        const QQuickItem *second = ui.tabButtons().value(1);
        QTest::mouseMove(ui.window, second->mapToScene(QPointF(second->width() / 2, 2)).toPoint());
        QTRY_COMPARE(geometries(), reference);
        ui.tabButtons().value(0)->forceActiveFocus(Qt::TabFocusReason);
        QTRY_COMPARE(geometries(), reference);
    }

    void theNextAndPreviousTabShortcutsSkipMissingTypes()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // All six tabs, wrapping: Home, Movies, Shows, Downloads, Favorites, Playlists
        for (const char *root :
             {"movies", "tvshows", "downloads", "favorites", "playlists", "home"}) {
            QTest::keyClick(ui.window, Qt::Key_PageDown, Qt::ControlModifier);
            QTRY_COMPARE(ui.rootTab(), QString::fromLatin1(root));
        }
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("playlists"));
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("favorites"));
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("downloads"));
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));

        // The selected root still counts when another page is in front
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) != nullptr);
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);

        // A library type the server does not have leaves the cycle
        ui.serveViews({{kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     QStringList({QStringLiteral("Home"), QStringLiteral("Shows"),
                                  QStringLiteral("Downloads"), QStringLiteral("Favorites"),
                                  QStringLiteral("Playlists")}));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTest::keyClick(ui.window, Qt::Key_PageDown, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTest::keyClick(ui.window, Qt::Key_PageUp, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("playlists"));

        // The other sequences go through the same switch, wrapping included
        QTest::keyClick(ui.window, Qt::Key_Tab, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTest::keyClick(ui.window, Qt::Key_BracketRight, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTest::keyClick(ui.window, Qt::Key_BracketLeft, Qt::ControlModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        // Ctrl+Shift+Tab reaches the window as Backtab
        QTest::keyClick(ui.window, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("playlists"));
    }

    void theAboutDialogOffersTheUpdateCheckOnlyWhenTheProfileSettingIsOn()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), false));
        QVERIFY(ui.load());
        // The check left the main menu for the About dialog
        QVERIFY(!ui.shellAction(QStringLiteral("updateCheckAction")));
        QVERIFY(!ui.popup(QStringLiteral("menuUpdateItem")));
        QObject *checker = ui.popup(QStringLiteral("updateChecker"));
        QObject *dialog = ui.popup(QStringLiteral("aboutDialog"));
        QVERIFY(checker);
        QVERIFY(dialog);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *button = ui.item(QStringLiteral("aboutCheckUpdatesButton"));
        QVERIFY(button);
        // The dialog stays open while the setting changes
        QVERIFY(!button->isVisible());
        QVERIFY(!ui.item(QStringLiteral("aboutUpdateStatus"))->isVisible());
        QVERIFY(!checker->property("checking").toBool());

        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QTRY_VERIFY(button->isVisible());
        QVERIFY(button->isEnabled());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), false));
        QTRY_VERIFY(!button->isVisible());
        QVERIFY(!checker->property("checking").toBool());
    }

    void theAboutDialogShowsTheResultOfTheUpdateCheckInline()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QVERIFY(ui.load());
        QObject *dialog = ui.popup(QStringLiteral("aboutDialog"));
        QVERIFY(dialog);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *button = ui.item(QStringLiteral("aboutCheckUpdatesButton"));
        QQuickItem *status = ui.item(QStringLiteral("aboutUpdateStatus"));
        QVERIFY(button);
        QVERIFY(status);
        QTRY_VERIFY(button->isVisible());
        QVERIFY(!status->isVisible());

        // The test binary has no application version, so the check fails at once and offline: the result shows under the button and the dialog stays
        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        QTRY_VERIFY(status->isVisible());
        QCOMPARE(status->property("text").toString(),
                 QStringLiteral("Could not check for updates. Check the internet connection"));
        QVERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(button->isEnabled());

        // Turning the check off hides both, and the old result is gone when it comes back
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), false));
        QTRY_VERIFY(!button->isVisible());
        QVERIFY(!status->isVisible());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QTRY_VERIFY(button->isVisible());
        QVERIFY(!status->isVisible());
    }

    void theAboutDialogHasMarginsAndLinks()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QObject *dialog = ui.popup(QStringLiteral("aboutDialog"));
        QVERIFY(dialog);
        // The test binary has no application data: an https address shows its entry, any other scheme hides it
        QVERIFY(dialog->setProperty("homepage", QStringLiteral("https://example.org/kinefin")));
        QVERIFY(dialog->setProperty("bugAddress", QStringLiteral("mailto:bugs@example.org")));
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *column = ui.item(QStringLiteral("aboutDialogColumn"));
        QVERIFY(column);
        QTRY_VERIFY(ui.item(QStringLiteral("aboutHomepageEntry"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("aboutBugEntry"))->isVisible());
        QVERIFY(ui.item(QStringLiteral("aboutLicenseEntry"))->isVisible());

        const qreal margin = dialog->property("contentMargin").toReal();
        const QRectF box(dialog->property("x").toReal(), dialog->property("y").toReal(),
                         dialog->property("width").toReal(), dialog->property("height").toReal());
        QTRY_VERIFY(column->width() > 0);
        const QRectF text = column->mapRectToScene(QRectF(0, 0, column->width(), column->height()));
        QVERIFY2(
            text.left() - box.left() >= margin - 1,
            qPrintable(
                QStringLiteral("left inset %1 < %2").arg(text.left() - box.left()).arg(margin)));
        QVERIFY2(
            box.right() - text.right() >= margin - 1,
            qPrintable(
                QStringLiteral("right inset %1 < %2").arg(box.right() - text.right()).arg(margin)));
        for (QQuickItem *candidate : allItems(column)) {
            if (!candidate->isVisible() || candidate->width() <= 0 || candidate->height() <= 0)
                continue;
            const QRectF rect =
                candidate->mapRectToScene(QRectF(0, 0, candidate->width(), candidate->height()));
            QVERIFY2(rect.left() >= text.left() - 1 && rect.right() <= text.right() + 1
                         && rect.bottom() <= box.bottom() + 1,
                     qPrintable(QStringLiteral("clipped: %1").arg(Ui::describe(candidate))));
            const QVariant contentHeight = candidate->property("contentHeight");
            if (contentHeight.isValid() && candidate->inherits("QQuickText"))
                QVERIFY2(contentHeight.toReal() <= candidate->height() + 1,
                         qPrintable(QStringLiteral("text cut: %1").arg(Ui::describe(candidate))));
        }
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
    }

    void theMainMenuOffersRefreshOnce()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
        QObject *entry = ui.popup(QStringLiteral("menuRefreshItem"));
        QVERIFY(menu);
        QVERIFY(entry);
        // The entry of a closed menu is never visible: the menu is open while the width changes
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());

        // The menu entry is the one Refresh at every width: the bar has no button of its own
        for (const int width : {2400, 600}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            QTRY_VERIFY(entry->property("visible").toBool());
            QVERIFY(!ui.item(QStringLiteral("headerRefreshButton")));
        }
        QCOMPARE(entry->property("action").value<QObject *>(),
                 ui.shellAction(QStringLiteral("refreshAction")));
        QVERIFY(entry->property("enabled").toBool());

        // Triggering it reloads the libraries: the views route is asked again
        int before = ui.server.viewRequests;
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(ui.server.viewRequests > before);
        QTRY_VERIFY(!menu->property("visible").toBool());

        // F5 is the entry's key, with no page action left to make it ambiguous
        before = ui.server.viewRequests;
        QTest::keyClick(ui.window, Qt::Key_F5);
        QTRY_VERIFY(ui.server.viewRequests > before);
    }

    void mainMenuIsAsWideAsItsWidestEntry()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
        QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());
        // Wide, Medium and Compact (where the menu is longest)
        for (const int width : {2400, 900, 500}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            const qreal paddings =
                menu->property("leftPadding").toReal() + menu->property("rightPadding").toReal();
            qreal widest = 0;
            for (QQuickItem *entry : ui.menuItems(QStringLiteral("mainMenu"))) {
                if (entry->isVisible())
                    widest = qMax(widest, entry->implicitWidth());
            }
            QVERIFY(widest > 0);
            const QString label = QStringLiteral("%1 px").arg(width);
            // No wider than the widest shown entry needs, and inside the window
            QTRY_VERIFY2(menu->property("width").toReal() <= widest + paddings + 2,
                         qPrintable(label));
            QVERIFY2(menu->property("width").toReal() <= width, qPrintable(label));
            // A window narrower than the menu needs caps it: the entries are squeezed by design then
            if (menu->property("width").toReal() + 1 < widest + paddings)
                continue;
            // Every entry gets what it asks for: no label of it (the shortcut included) is squeezed
            for (QQuickItem *entry : ui.menuItems(QStringLiteral("mainMenu"))) {
                if (!entry->isVisible())
                    continue;
                QVERIFY2(entry->width() + 0.5 >= entry->implicitWidth(), qPrintable(label));
                auto *content = entry->property("contentItem").value<QQuickItem *>();
                if (!content)
                    continue;
                for (QQuickItem *part : content->childItems()) {
                    if (part->isVisible() && part->inherits("QQuickText"))
                        QVERIFY2(part->width() + 0.5 >= part->implicitWidth(),
                                 qPrintable(label + QStringLiteral(": ")
                                            + part->property("text").toString()));
                }
            }
        }
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
    }

    void adminDashboardEntryIsForAdministratorsOnly()
    {
        const auto shown = [this](bool administrator) {
            Ui ui(m_home->path());
            ui.server.administrator = administrator;
            if (!ui.load() || !buildState(ui, QStringLiteral("home")))
                return QStringLiteral("no session");
            QObject *action = ui.shellAction(QStringLiteral("adminDashboardAction"));
            QObject *menu = ui.popup(QStringLiteral("mainMenu"));
            QObject *entry = ui.popup(QStringLiteral("menuAdminItem"));
            if (!action || !menu || !entry || !QMetaObject::invokeMethod(menu, "open")
                || !QTest::qWaitFor([menu] { return menu->property("opened").toBool(); }, 5000))
                return QStringLiteral("no menu");
            const bool actionVisible = action->property("visible").toBool();
            // The menu entry follows its action
            return entry->property("visible").toBool() == actionVisible
                       ? (actionVisible ? QStringLiteral("shown") : QStringLiteral("hidden"))
                       : QStringLiteral("entry and action disagree");
        };
        QCOMPARE(shown(false), QStringLiteral("hidden"));
        QCOMPARE(shown(true), QStringLiteral("shown"));
    }

    void theMainMenuRefreshReloadsTheFavoritesPage()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openFavorites(ui));
        QVERIFY(ui.shellAction(QStringLiteral("refreshAction"))->property("visible").toBool());

        // The page in front refreshes itself: the favorites scan runs again and the libraries are left alone
        const int scans = ui.server.itemsRoutes.size();
        const int views = ui.server.viewRequests;
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("refreshAction")), "trigger"));
        QTRY_VERIFY(ui.server.itemsRoutes.size() > scans);
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QCOMPARE(ui.server.viewRequests, views);
        // And so does F5, the entry's key
        const int menuScans = ui.server.itemsRoutes.size();
        QTest::keyClick(ui.window, Qt::Key_F5);
        QTRY_VERIFY(ui.server.itemsRoutes.size() > menuScans);
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
    }

    void theMainMenuRefreshReloadsTheLibraryAndIsOffWhereThereIsNothingToRefresh()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QObject *refresh = ui.shellAction(QStringLiteral("refreshAction"));
        QVERIFY(refresh);
        QTRY_VERIFY(refresh->property("enabled").toBool());

        // The main list asks for its first page again and the libraries are left alone
        const int views = ui.server.viewRequests;
        int items = ui.server.itemsRoutes.size();
        QVERIFY(QMetaObject::invokeMethod(refresh, "trigger"));
        QTRY_VERIFY(ui.server.itemsRoutes.size() > items);
        QTRY_VERIFY(ui.controller.movies()->status() != Kinefin::LibraryItemsModel::Loading);
        QCOMPARE(ui.server.viewRequests, views);

        // A sub-tab list is asked for again too
        auto *subtabs = ui.controller.subtabs();
        QVERIFY(subtabs->select(QStringLiteral("favorites")));
        QTRY_COMPARE(subtabs->current(), QStringLiteral("favorites"));
        QTRY_VERIFY(subtabs->activeList()->status() != Kinefin::LibraryItemsModel::Loading);
        items = ui.server.itemsRoutes.size();
        QVERIFY(QMetaObject::invokeMethod(refresh, "trigger"));
        QTRY_VERIFY(ui.server.itemsRoutes.size() > items);
        QCOMPARE(ui.server.viewRequests, views);

        // The queue and Settings have nothing to refresh: the entry is there but off
        const QVariantMap queueProperties{
            {QStringLiteral("connection"), QVariant::fromValue(&ui.controller)}};
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openSection",
                                          Q_ARG(QVariant, QStringLiteral("queuePage")),
                                          Q_ARG(QVariant, QStringLiteral("QueuePage.qml")),
                                          Q_ARG(QVariant, queueProperties)));
        QTRY_VERIFY(ui.item(QStringLiteral("queuePage")) != nullptr);
        QTRY_VERIFY(!refresh->property("enabled").toBool());
        QVERIFY(refresh->property("visible").toBool());
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) != nullptr);
        QTRY_VERIFY(!refresh->property("enabled").toBool());
    }

    void theMainMenuRefreshReloadsTheDetailsPage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("details")));
        QQuickItem *page = ui.item(QStringLiteral("itemDetailsPage"));
        QVERIFY(page);
        const auto detailRequests = [&ui] {
            return std::count_if(
                ui.server.itemsRoutes.cbegin(), ui.server.itemsRoutes.cend(),
                [](const QByteArray &route) { return route.contains("Chapters"); });
        };
        const auto before = detailRequests();
        QTRY_VERIFY(ui.shellAction(QStringLiteral("refreshAction"))->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(page, "refreshPage"));
        QTRY_VERIFY(detailRequests() > before);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("refreshAction")), "trigger"));
        QTRY_VERIFY(detailRequests() > before + 1);
        // F5 is the main menu's: the page has no Refresh of its own to make it ambiguous
        const auto afterMenu = detailRequests();
        QTest::keyClick(ui.window, Qt::Key_F5);
        QTRY_VERIFY(detailRequests() > afterMenu);
    }

    void theMenuButtonUsesTheStyleOfTheHeaderButtonsAndNamesItself()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signInAsUser(QStringLiteral("Alice")));
        QVERIFY(ui.opensWindowOfWidth(1200));
        QQuickItem *button = ui.item(QStringLiteral("mainMenuButton"));
        QQuickItem *queue = ui.item(QStringLiteral("queueButton"));
        QVERIFY(button && queue);
        QTRY_VERIFY(button->isVisible() && queue->isVisible());

        // The right end of the bar, as tall as its neighbour and no narrower than tall
        QTRY_COMPARE(button->height(), queue->height());
        QTRY_VERIFY(button->width() >= button->height());
        const QRectF buttonRect =
            button->mapRectToScene(QRectF(0, 0, button->width(), button->height()));
        QVERIFY(buttonRect.right() > ui.window->width() / 2);
        QVERIFY(buttonRect.left() > queue->mapToScene(QPointF(0, 0)).x());

        // No text for the style to draw beside the avatar and no tooltip over it; the label is static and the name says who is signed in
        QVERIFY(button->property("text").toString().isEmpty());
        QVERIFY(QQmlProperty(button, QStringLiteral("QQC2.ToolTip.text"), qmlContext(button))
                    .read()
                    .toString()
                    .isEmpty());
        QCOMPARE(button->property("label").toString(), QStringLiteral("Main Menu"));
        // The user list reaches the header just after the session does
        QTRY_COMPARE(accessibleNameOf(button), QStringLiteral("Main Menu, signed in as Alice"));

        // No ring of its own: the background is the style's, as on the queue button, so hover, press and keyboard focus look the same (the build image has no Breeze QStyle to draw them offscreen)
        const auto *background = button->property("background").value<QQuickItem *>();
        const auto *queueBackground = queue->property("background").value<QQuickItem *>();
        QVERIFY(background && queueBackground);
        QCOMPARE(QString::fromLatin1(background->metaObject()->className()),
                 QString::fromLatin1(queueBackground->metaObject()->className()));
        QVERIFY(!button->property("visualFocus").toBool());
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(button->property("visualFocus").toBool());
    }

    // The signals of the update checker, as its replies would raise them
    static void announceUpdate(QObject *checker)
    {
        QVERIFY(QMetaObject::invokeMethod(
            checker, "updateAvailable", Q_ARG(QString, QStringLiteral("9.9.9")),
            Q_ARG(QUrl, QUrl(QStringLiteral("https://example.org/kinefin/9.9.9")))));
    }

    void aManualUpdateCheckFromTheOpenAboutDialogLeavesItOpenAndOffersTheUpdate()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QVERIFY(ui.load());
        QObject *about = ui.popup(QStringLiteral("aboutDialog"));
        QObject *update = ui.popup(QStringLiteral("updateAvailableDialog"));
        QObject *checker = ui.popup(QStringLiteral("updateChecker"));
        QVERIFY(about && update && checker);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QTRY_VERIFY(about->property("opened").toBool());

        QVERIFY(ui.shellHeader()->setProperty("manualUpdateCheck", true));
        announceUpdate(checker);
        QQuickItem *view = ui.item(QStringLiteral("aboutViewUpdateButton"));
        QVERIFY(view);
        QTRY_VERIFY(view->isVisible());
        QCOMPARE(ui.item(QStringLiteral("aboutUpdateStatus"))->property("text").toString(),
                 QStringLiteral("Kinefin 9.9.9 is available"));
        QVERIFY(!ui.shellHeader()->property("manualUpdateCheck").toBool());

        // Nothing opens under the user's mouse
        QTest::qWait(500);
        QVERIFY(about->property("opened").toBool());
        QVERIFY(!update->property("opened").toBool());

        // Only the click on the button goes to the update dialog, About first
        QVERIFY(QMetaObject::invokeMethod(view, "click"));
        QTRY_VERIFY(update->property("opened").toBool());
        QTRY_VERIFY(!about->property("opened").toBool());
        QVERIFY(ui.showsText(
            QStringLiteral("Kinefin 9.9.9 is available. Open its release page in your browser?")));
    }

    void aManualUpdateCheckWithTheAboutDialogClosedOpensTheUpdateDialogAtOnce()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QVERIFY(ui.load());
        QObject *about = ui.popup(QStringLiteral("aboutDialog"));
        QObject *update = ui.popup(QStringLiteral("updateAvailableDialog"));
        QObject *checker = ui.popup(QStringLiteral("updateChecker"));
        QVERIFY(about && update && checker);
        QVERIFY(ui.shellHeader()->setProperty("manualUpdateCheck", true));
        announceUpdate(checker);
        QTRY_VERIFY_WITH_TIMEOUT(update->property("opened").toBool(), scaled(2000));
        QVERIFY(!about->property("opened").toBool());
    }

    void anAutomaticUpdateCheckNeverOpensADialog()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QVERIFY(ui.load());
        QObject *about = ui.popup(QStringLiteral("aboutDialog"));
        QObject *update = ui.popup(QStringLiteral("updateAvailableDialog"));
        QObject *checker = ui.popup(QStringLiteral("updateChecker"));
        QVERIFY(about && update && checker);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QTRY_VERIFY(about->property("opened").toBool());
        announceUpdate(checker);
        QVERIFY(!ui.item(QStringLiteral("aboutViewUpdateButton"))->isVisible());
        // The notice is passive: neither the update dialog opens nor About closes
        QTest::qWait(500);
        QVERIFY(!update->property("opened").toBool());
        QVERIFY(about->property("opened").toBool());
    }

    void theResultOfAManualUpdateCheckIsInlineWhileAboutIsOpenAndANotificationOtherwise_data()
    {
        QTest::addColumn<QByteArray>("signal");
        QTest::addColumn<QString>("text");
        QTest::newRow("upToDate") << QByteArray("upToDate")
                                  << QStringLiteral("Kinefin is up to date");
        QTest::newRow("failed")
            << QByteArray("checkFailed")
            << QStringLiteral("Could not check for updates. Check the internet connection");
    }

    void theResultOfAManualUpdateCheckIsInlineWhileAboutIsOpenAndANotificationOtherwise()
    {
        QFETCH(QByteArray, signal);
        QFETCH(QString, text);
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("checkForUpdates"), true));
        QVERIFY(ui.load());
        QObject *about = ui.popup(QStringLiteral("aboutDialog"));
        QObject *checker = ui.popup(QStringLiteral("updateChecker"));
        QVERIFY(about && checker);
        // The notification is no item of the About dialog: whatever else shows the text
        const auto notifications = [&ui, &text] {
            const QList<QQuickItem *> found = ui.itemsWithText(text);
            return std::count_if(found.cbegin(), found.cend(), [](const QQuickItem *item) {
                return item->isVisible()
                       && item->objectName() != QLatin1String("aboutUpdateStatus");
            });
        };

        // Open: the status line under the button says it, and no notification repeats it
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QTRY_VERIFY(about->property("opened").toBool());
        QVERIFY(ui.shellHeader()->setProperty("manualUpdateCheck", true));
        QVERIFY(QMetaObject::invokeMethod(checker, signal.constData()));
        QTRY_COMPARE(ui.item(QStringLiteral("aboutUpdateStatus"))->property("text").toString(),
                     text);
        QTest::qWait(800);
        QCOMPARE(notifications(), 0);

        // Closed: a notification carries the result
        QVERIFY(QMetaObject::invokeMethod(about, "close"));
        QTRY_VERIFY(!about->property("opened").toBool());
        QVERIFY(ui.shellHeader()->setProperty("manualUpdateCheck", true));
        QVERIFY(QMetaObject::invokeMethod(checker, signal.constData()));
        QTRY_VERIFY(notifications() > 0);

        // The flag of the manual check is spent by its result
        QVERIFY(!ui.shellHeader()->property("manualUpdateCheck").toBool());
    }

    void theSyncPlayDialogListsTheGroupsAsPlainText()
    {
        Ui ui(m_home->path());
        // Markup in a group's or a member's name must show as typed
        ui.server.syncPlayListBody = QByteArrayLiteral(
            "[{\"GroupId\":\"5f1e2d3c4b5a69788796a5b4c3d2e1f0\",\"GroupName\":\"<b>Night</b> "
            "&amp; day\",\"LastUpdatedAt\":\"2026-11-16T16:37:44.5Z\",\"Participants\":[\"alice\","
            "\"<i>bob</i>\"],\"State\":\"Playing\"}]");
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openSyncPlayDialog(ui));
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayGroupRow")).size(), 1);
        QQuickItem *name = ui.item(QStringLiteral("syncPlayRowName"));
        QQuickItem *members = ui.item(QStringLiteral("syncPlayRowMembers"));
        QVERIFY(name && members);
        QCOMPARE(name->property("text").toString(), QStringLiteral("<b>Night</b> &amp; day"));
        QCOMPARE(name->property("textFormat").toInt(), int(Qt::PlainText));
        QCOMPARE(members->property("text").toString(), QStringLiteral("alice, <i>bob</i>"));
        QCOMPARE(members->property("textFormat").toInt(), int(Qt::PlainText));
        // The policy is unknown here, so a new group is offered, with a name already filled in
        QVERIFY(ui.item(QStringLiteral("syncPlayNewGroup"))->isVisible());
        QVERIFY(
            !ui.item(QStringLiteral("syncPlayNameField"))->property("text").toString().isEmpty());
        QVERIFY(!ui.item(QStringLiteral("syncPlayNoGroups"))->isVisible());
    }

    void joiningAndLeavingFromTheSyncPlayDialog()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openSyncPlayDialog(ui));
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayGroupRow")).size(), 1);
        QQuickItem *join = ui.item(QStringLiteral("syncPlayJoinButton"));
        QVERIFY(join);
        QVERIFY(QMetaObject::invokeMethod(join, "clicked"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("Join")), 1);
        QCOMPARE(lastSyncPlayBody(ui, QStringLiteral("Join"))
                     .value(QStringLiteral("GroupId"))
                     .toString(),
                 kSyncPlayGroup);
        QTRY_VERIFY(ui.item(QStringLiteral("syncPlayJoining"))->isVisible());
        ui.server.lastConnection()->sendText(
            QString::fromUtf8(fixture(QStringLiteral("ws_syncplay_group_joined"))));
        QTRY_VERIFY(ui.item(QStringLiteral("syncPlayGroup"))->isVisible());
        QCOMPARE(ui.item(QStringLiteral("syncPlayGroupName"))->property("text").toString(),
                 QStringLiteral("Movie night"));
        QCOMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayMember")).size(), 1);
        QVERIFY(ui.itemsStartingWith(QStringLiteral("syncPlayGroupRow")).isEmpty());
        QObject *leave = ui.popup(QStringLiteral("syncPlayLeaveAction"));
        QVERIFY(leave && leave->property("visible").toBool());
        QVERIFY(!ui.popup(QStringLiteral("syncPlayRefreshAction"))->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(leave, "trigger"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("Leave")), 1);
        QTRY_VERIFY(!ui.item(QStringLiteral("syncPlayGroup"))->isVisible());
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayGroupRow")).size(), 1);
    }

    void creatingASyncPlayGroupSendsItsNameAndARefusalShows()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openSyncPlayDialog(ui));
        QQuickItem *field = ui.item(QStringLiteral("syncPlayNameField"));
        QVERIFY(field);
        field->setProperty("text", QStringLiteral("Movie night"));
        QVERIFY(
            QMetaObject::invokeMethod(ui.item(QStringLiteral("syncPlayCreateButton")), "clicked"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("New")), 1);
        QCOMPARE(lastSyncPlayBody(ui, QStringLiteral("New")),
                 (QJsonObject{{QStringLiteral("GroupName"), QStringLiteral("Movie night")}}));
        ui.server.lastConnection()->sendText(syncPlayUpdate(
            QStringLiteral("CreateGroupDenied"), QString(), QString(32, QLatin1Char('0'))));
        QQuickItem *failure = ui.item(QStringLiteral("syncPlayFailure"));
        QVERIFY(failure);
        QTRY_VERIFY(failure->isVisible());
        QVERIFY(!ui.item(QStringLiteral("syncPlayJoining"))->isVisible());
    }

    void theSyncPlayEntryFollowsTheUsersPolicy()
    {
        Ui ui(m_home->path());
        ui.server.syncPlayAccess = QStringLiteral("JoinGroups");
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // Joining only: no new group, and the empty list says whom to ask
        ui.server.syncPlayListBody = QByteArrayLiteral("[]");
        QVERIFY(openSyncPlayDialog(ui));
        QQuickItem *empty = ui.item(QStringLiteral("syncPlayNoGroups"));
        QVERIFY(empty);
        QTRY_VERIFY(empty->isVisible());
        QVERIFY(!ui.item(QStringLiteral("syncPlayNewGroup"))->isVisible());
        QVERIFY(!ui.syncPlay.canCreate());
        QVERIFY(QMetaObject::invokeMethod(ui.popup(QStringLiteral("syncPlayDialog")), "close"));
        // No SyncPlay at all: the entry goes, at the next answer of /Users/Me
        ui.server.syncPlayAccess = QStringLiteral("None");
        QVERIFY(ui.controller.playbackClient().verifySession());
        QObject *action = ui.shellAction(QStringLiteral("syncPlayAction"));
        QTRY_VERIFY(!action->property("visible").toBool());
        QVERIFY(!ui.syncPlay.available());
    }

    void theSyncPlayGroupsThatFailToLoadOfferRetry()
    {
        Ui ui(m_home->path());
        ui.server.syncPlayListStatus = 500;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openSyncPlayDialog(ui));
        QQuickItem *failed = ui.item(QStringLiteral("syncPlayGroupsFailed"));
        QVERIFY(failed);
        QTRY_VERIFY(failed->isVisible());
        QVERIFY(!ui.item(QStringLiteral("syncPlayGroupsBusy"))->isVisible());
        ui.server.syncPlayListStatus = 200;
        const int asked = ui.server.syncPlayListRequests;
        QVERIFY(QMetaObject::invokeMethod(ui.popup(QStringLiteral("syncPlayRefreshAction")),
                                          "trigger"));
        QTRY_VERIFY(ui.server.syncPlayListRequests > asked);
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayGroupRow")).size(), 1);
        QVERIFY(!failed->isVisible());
    }

    void theSyncPlayDialogShowsAndEditsTheGroupQueue()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(syncPlayJoined(ui));
        const QString first = QStringLiteral("9a8b7c6d5e4f40312a1b2c3d4e5f6071");
        const QString second = QStringLiteral("9a8b7c6d5e4f40312a1b2c3d4e5f6072");
        ui.server.lastConnection()->sendText(syncPlayUpdate(
            QStringLiteral("PlayQueue"),
            QJsonObject{{QStringLiteral("Reason"), QStringLiteral("NewPlaylist")},
                        {QStringLiteral("LastUpdate"),
                         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                        {QStringLiteral("Playlist"),
                         QJsonArray{QJsonObject{{QStringLiteral("ItemId"), kMovieId},
                                                {QStringLiteral("PlaylistItemId"), first}},
                                    QJsonObject{{QStringLiteral("ItemId"), kMovieId},
                                                {QStringLiteral("PlaylistItemId"), second}}}},
                        {QStringLiteral("PlayingItemIndex"), 0},
                        {QStringLiteral("StartPositionTicks"), 0},
                        {QStringLiteral("IsPlaying"), false},
                        {QStringLiteral("ShuffleMode"), QStringLiteral("Sorted")},
                        {QStringLiteral("RepeatMode"), QStringLiteral("RepeatNone")}}));
        QVERIFY(openSyncPlayDialog(ui));
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayQueueRow")).size(), 2);
        // The names come from the server, as plain text
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayQueueTitle"))
                         .first()
                         ->property("text")
                         .toString(),
                     QStringLiteral("Arcturus (2012)"));
        QCOMPARE(ui.itemsStartingWith(QStringLiteral("syncPlayQueueTitle"))
                     .first()
                     ->property("textFormat")
                     .toInt(),
                 int(Qt::PlainText));
        QVERIFY(ui.itemsStartingWith(QStringLiteral("syncPlayQueueRow"))
                    .first()
                    ->property("highlighted")
                    .toBool());
        // The second entry: up, play, out
        QVERIFY(QMetaObject::invokeMethod(
            ui.itemsStartingWith(QStringLiteral("syncPlayQueueUp")).at(1), "clicked"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("MovePlaylistItem")), 1);
        QCOMPARE(lastSyncPlayBody(ui, QStringLiteral("MovePlaylistItem"))
                     .value(QStringLiteral("NewIndex"))
                     .toInt(),
                 0);
        QVERIFY(QMetaObject::invokeMethod(
            ui.itemsStartingWith(QStringLiteral("syncPlayQueuePlay")).at(1), "clicked"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("SetPlaylistItem")), 1);
        QCOMPARE(lastSyncPlayBody(ui, QStringLiteral("SetPlaylistItem"))
                     .value(QStringLiteral("PlaylistItemId"))
                     .toString(),
                 second);
        QVERIFY(QMetaObject::invokeMethod(
            ui.itemsStartingWith(QStringLiteral("syncPlayQueueRemove")).at(1), "clicked"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("RemoveFromPlaylist")), 1);
        // Not following, then following again
        QObject *follow = ui.popup(QStringLiteral("syncPlayFollowAction"));
        QVERIFY(follow && follow->property("visible").toBool());
        QCOMPARE(follow->property("text").toString(), QStringLiteral("Stop Following"));
        QVERIFY(QMetaObject::invokeMethod(follow, "trigger"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("SetIgnoreWait")), 1);
        QTRY_VERIFY(ui.item(QStringLiteral("syncPlayNotFollowing"))->isVisible());
        QVERIFY(QMetaObject::invokeMethod(follow, "trigger"));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("SetIgnoreWait")), 2);
        QTRY_VERIFY(!ui.item(QStringLiteral("syncPlayNotFollowing"))->isVisible());
    }

    void playNextGoesToTheSyncPlayGroupWhileItFollows()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(syncPlayJoined(ui));
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(QMetaObject::invokeMethod(root, "queueItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, true)));
        QTRY_COMPARE(syncPlayRequests(ui, QStringLiteral("Queue")), 1);
        QCOMPARE(
            lastSyncPlayBody(ui, QStringLiteral("Queue")).value(QStringLiteral("Mode")).toString(),
            QStringLiteral("QueueNext"));
        QCOMPARE(ui.controller.queue()->count(), 0);
        // Not following, the user's own queue takes it
        ui.syncPlay.stopFollowing();
        QVERIFY(QMetaObject::invokeMethod(root, "queueItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, false)));
        QTRY_COMPARE(ui.controller.queue()->count(), 1);
        QCOMPARE(syncPlayRequests(ui, QStringLiteral("Queue")), 1);
    }

    void thePlayerShowsTheSyncPlayGroup()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QVERIFY(!page->property("grouped").toBool());
        QVERIFY(syncPlayJoined(ui));
        QTRY_VERIFY(page->property("grouped").toBool());
        QObject *button = ui.item(QStringLiteral("playerSyncPlayButton"));
        QVERIFY(button);
        // The group's dialog opens over the player, whose page hides the shell's header
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QObject *dialog = ui.popup(QStringLiteral("syncPlayDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("syncPlayGroup"))->isVisible());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        // The group plays at its own pace; its repeat is the group's, and there is no queue to shuffle yet
        QVERIFY(!ui.popup(QStringLiteral("playerSpeedMenu"))->property("enabled").toBool());
        QVERIFY(ui.popup(QStringLiteral("playerRepeatMenu"))->property("enabled").toBool());
        QVERIFY(!ui.popup(QStringLiteral("playerShuffleItem"))->property("enabled").toBool());
        ui.server.lastConnection()->sendText(
            syncPlayUpdate(QStringLiteral("StateUpdate"),
                           QJsonObject{{QStringLiteral("State"), QStringLiteral("Waiting")},
                                       {QStringLiteral("Reason"), QStringLiteral("Buffer")}}));
        QTRY_VERIFY(ui.item(QStringLiteral("playerGroupWaiting"))->isVisible());
        ui.server.lastConnection()->sendText(
            syncPlayUpdate(QStringLiteral("UserJoined"), QStringLiteral("<b>bob</b>")));
        QQuickItem *notice = ui.item(QStringLiteral("playerGroupNotice"));
        QVERIFY(notice);
        QTRY_VERIFY(notice->isVisible());
        QVERIFY(notice->property("text").toString().contains(QStringLiteral("<b>bob</b>")));
        QCOMPARE(notice->property("textFormat").toInt(), int(Qt::PlainText));
        // Left, the menus are the user's again
        ui.syncPlay.leaveGroup();
        QTRY_VERIFY(ui.popup(QStringLiteral("playerSpeedMenu"))->property("enabled").toBool());
        QVERIFY(!ui.item(QStringLiteral("playerGroupWaiting"))->isVisible());
    }

    void theSyncPlayGroupsItemOpensThePlayer()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(ui.engine.rootObjects().constFirst()->setProperty(
            "player", QVariant::fromValue<QObject *>(&stub)));
        QVERIFY(syncPlayJoined(ui));
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        ui.server.lastConnection()->sendText(syncPlayUpdate(
            QStringLiteral("PlayQueue"),
            QJsonObject{
                {QStringLiteral("Reason"), QStringLiteral("NewPlaylist")},
                {QStringLiteral("LastUpdate"), now},
                {QStringLiteral("Playlist"),
                 QJsonArray{QJsonObject{{QStringLiteral("ItemId"), kMovieId},
                                        {QStringLiteral("PlaylistItemId"),
                                         QStringLiteral("9a8b7c6d5e4f40312a1b2c3d4e5f6071")}}}},
                {QStringLiteral("PlayingItemIndex"), 0},
                {QStringLiteral("StartPositionTicks"), 0},
                {QStringLiteral("IsPlaying"), false},
                {QStringLiteral("ShuffleMode"), QStringLiteral("Sorted")},
                {QStringLiteral("RepeatMode"), QStringLiteral("RepeatNone")}}));
        QTRY_VERIFY(ui.currentDetails()
                    && ui.currentDetails()->objectName() == QStringLiteral("playerPage"));
    }

    void theRemoteSessionsDialogExplainsAnEmptyListAndOffersOneRefresh()
    {
        Ui ui(m_home->path());
        ui.server.listingRawBody.insert(QStringLiteral("Sessions"), QByteArrayLiteral("[]"));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("remoteSessionsAction")),
                                          "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("remoteSessionsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *empty = ui.item(QStringLiteral("remoteSessionsEmpty"));
        QVERIFY(empty);
        QTRY_VERIFY(empty->isVisible());
        QVERIFY(!ui.item(QStringLiteral("remoteSessionsBusy"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("remoteSessionsFailed"))->isVisible());
        QVERIFY(ui.itemsStartingWith(QStringLiteral("remoteSessionRow")).isEmpty());

        // The footer keeps its Refresh under the placeholder, so the dialog keeps its size
        QObject *footerRefresh = ui.popup(QStringLiteral("remoteRefreshAction"));
        QVERIFY(footerRefresh);
        QVERIFY(footerRefresh->property("visible").toBool());
        const int asked = ui.server.listingRoutes.value(QStringLiteral("Sessions")).size();
        QVERIFY(QMetaObject::invokeMethod(footerRefresh, "trigger"));
        QTRY_VERIFY(ui.server.listingRoutes.value(QStringLiteral("Sessions")).size() > asked);
        QTRY_COMPARE(ui.controller.remoteSessions()->status(), Kinefin::RemoteSessionsModel::Ready);
        QVERIFY(empty->isVisible());
    }

    void theRemoteSessionsDialogSpinsUntilTheFirstAnswerAndOnRetryOnly()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        Kinefin::RemoteSessionsModel *remote = ui.controller.remoteSessions();
        // The held replies are answered with the report status: a failing one
        ui.server.reportStatus = 500;
        ui.server.heldFamilies.insert(QStringLiteral("Sessions"));
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("remoteSessionsAction")),
                                          "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("remoteSessionsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *busy = ui.item(QStringLiteral("remoteSessionsBusy"));
        QQuickItem *failed = ui.item(QStringLiteral("remoteSessionsFailed"));
        QVERIFY(busy && failed);
        QTRY_VERIFY(busy->isVisible());
        QVERIFY(!failed->isVisible());

        // The first answer stops the spinner, and a failure shows the box
        QTRY_VERIFY(ui.server.heldCount() >= 1);
        ui.server.heldFamilies.clear();
        ui.server.releaseHeld();
        QTRY_VERIFY(failed->isVisible());
        QVERIFY(!busy->isVisible());
        QVERIFY(!dialog->property("retrying").toBool());

        // Retry: the box gives way to the spinner while the request is out
        ui.server.listingStatus.insert(QStringLiteral("Sessions"), 500);
        ui.server.heldFamilies.insert(QStringLiteral("Sessions"));
        QVERIFY(ui.clickButton(QStringLiteral("Retry")));
        QTRY_VERIFY(busy->isVisible());
        QVERIFY(!failed->isVisible());
        QVERIFY(dialog->property("retrying").toBool());

        // A retry that fails again brings the box back and stops the spinner
        QTRY_VERIFY(ui.server.heldCount() >= 1);
        ui.server.heldFamilies.clear();
        ui.server.releaseHeld();
        QTRY_VERIFY(failed->isVisible());
        QVERIFY(!busy->isVisible());
        QVERIFY(!dialog->property("retrying").toBool());

        // One that works shows the devices
        ui.server.listingStatus.remove(QStringLiteral("Sessions"));
        QVERIFY(ui.clickButton(QStringLiteral("Retry")));
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("remoteSessionRow")).size(), 2);
        QVERIFY(!failed->isVisible());
        QVERIFY(!busy->isVisible());
        // Each row shows the kind of its device: a desktop client and a browser
        QStringList icons;
        for (QQuickItem *icon : ui.itemsStartingWith(QStringLiteral("remoteDeviceIcon")))
            icons << icon->property("source").toString();
        icons.sort();
        QCOMPARE(icons,
                 (QStringList{QStringLiteral("computer"), QStringLiteral("internet-web-browser")}));
        QTRY_COMPARE(remote->status(), Kinefin::RemoteSessionsModel::Ready);

        // A poll after an answer changes nothing on screen: no spinner, the rows stay
        ui.server.heldFamilies.insert(QStringLiteral("Sessions"));
        remote->reload();
        QTRY_COMPARE(remote->status(), Kinefin::RemoteSessionsModel::Loading);
        QTest::qWait(100);
        QVERIFY(!busy->isVisible());
        QCOMPARE(ui.itemsStartingWith(QStringLiteral("remoteSessionRow")).size(), 2);
    }

    void theRemoteSessionsDialogReportsAFailedCommandUntilTheNextOne()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const QString message =
            QStringLiteral("The remote command failed. Refresh the devices and try again");
        ui.server.remoteCommandStatus = 500;
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("remoteSessionsAction")),
                                          "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("remoteSessionsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QQuickItem *pause = nullptr;
        QTRY_VERIFY((pause = ui.item(QStringLiteral("remotePauseButton")))
                    && pause->hasActiveFocus());
        QVERIFY(!ui.showsText(message));
        QTest::keyClick(ui.window, Qt::Key_Space);
        QTRY_VERIFY(ui.controller.remoteSessions()->commandFailed());
        QTRY_VERIFY(ui.showsText(message));

        // The next command that goes out clears the box
        ui.server.remoteCommandStatus = 204;
        QTest::keyClick(ui.window, Qt::Key_Space);
        QTRY_VERIFY(!ui.showsText(message));
        QTRY_COMPARE(ui.server.remoteCommandRoutes.size(), 2);
    }

    void remoteSessionsDialogControlsASession()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const int depth = ui.stackDepth();
        Kinefin::RemoteSessionsModel *remote = ui.controller.remoteSessions();
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("remoteSessionsAction")),
                                          "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("remoteSessionsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_COMPARE(remote->status(), Kinefin::RemoteSessionsModel::Ready);
        // A dialog, not a page: the stack keeps its depth
        QCOMPARE(ui.stackDepth(), depth);
        QTRY_COMPARE(ui.itemsStartingWith(QStringLiteral("remoteSessionRow")).size(), 2);

        // Only the playing session offers its buttons, and the keyboard starts on the first one
        QQuickItem *pause = ui.item(QStringLiteral("remotePauseButton"));
        QVERIFY(pause);
        QVERIFY(pause->isVisible());
        QTRY_VERIFY(pause->hasActiveFocus());

        const int polls = ui.server.listingRoutes.value(QStringLiteral("Sessions")).size();
        QTest::keyClick(ui.window, Qt::Key_Space);
        QTRY_COMPARE(ui.server.remoteCommandRoutes.size(), 1);
        QVERIFY(ui.server.remoteCommandRoutes.constFirst().endsWith("/Playing/Pause"));
        // The command reloads the sessions: the rows stay, and so does the focus
        QTRY_VERIFY(ui.server.listingRoutes.value(QStringLiteral("Sessions")).size() > polls);
        QTRY_COMPARE(remote->status(), Kinefin::RemoteSessionsModel::Ready);
        QTRY_VERIFY(!remote->commandPending());
        QCOMPARE(ui.item(QStringLiteral("remotePauseButton")), pause);
        QVERIFY(pause->hasActiveFocus());
        remote->reload();
        QTRY_COMPARE(remote->status(), Kinefin::RemoteSessionsModel::Ready);
        QCOMPARE(ui.item(QStringLiteral("remotePauseButton")), pause);
        QVERIFY(pause->hasActiveFocus());

        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QTRY_COMPARE(remote->status(), Kinefin::RemoteSessionsModel::Idle);
        QCOMPARE(ui.stackDepth(), depth);
    }

    void remoteSessionsDialogClosesWithTheSession()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("remoteSessionsAction")),
                                          "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("remoteSessionsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_COMPARE(ui.controller.remoteSessions()->status(), Kinefin::RemoteSessionsModel::Ready);
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("switchServerAction")),
                                          "trigger"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QTRY_COMPARE(ui.controller.remoteSessions()->status(), Kinefin::RemoteSessionsModel::Idle);
    }

    void theMenuButtonAtTheRightEndTakesTheClickAndOpensTheMenuUnderIt()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *button = ui.item(QStringLiteral("mainMenuButton"));
        QVERIFY(button);
        QVERIFY(ui.insideWindow(button));
        // A real click at its center must reach it
        ui.clickCenter(button);
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
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
              QStringLiteral("Settings"), QStringLiteral("Show Keyboard Shortcuts"),
              QStringLiteral("Player"), QStringLiteral("Play or Pause"),
              QStringLiteral("Mute or Unmute"),
              QStringLiteral("Cancel Up Next, Leave Full Screen, or Go Back"),
              QStringLiteral("Skip Forward 1 Minute"), QStringLiteral("Previous Chapter"),
              QStringLiteral("Next Chapter")})
            QTRY_VERIFY2(ui.showsText(action), qPrintable(action));
        for (const QString &key :
             {QStringLiteral("Ctrl"), QStringLiteral("Shift"), QStringLiteral("/")})
            QTRY_VERIFY2(ui.showsText(key), qPrintable(key));
        // The "+" of Volume Up is a key in its own box, not the separator of a combination
        bool plusKey = false;
        for (const QQuickItem *part : ui.itemsStartingWith(QStringLiteral("shortcutsKeyPart"))) {
            if (part->property("plus").toBool())
                continue;
            for (const QQuickItem *label : part->childItems())
                plusKey = plusKey || label->property("text").toString() == QLatin1String("+");
        }
        QVERIFY(plusKey);
        // Every sequence the header answers with is listed, read from the actions and the tab shortcuts rather than written down here
        QSet<QKeySequence> listed;
        for (const QVariant &row : dialog->property("rows").value<QJSValue>().toVariant().toList())
            listed.insert(
                QKeySequence(row.toList().constFirst().toString(), QKeySequence::PortableText));
        const auto sequenceOf = [](const QVariant &value) {
            return value.metaType().id() == QMetaType::QString
                       ? QKeySequence(value.toString(), QKeySequence::PortableText)
                       : value.value<QKeySequence>();
        };
        QList<QKeySequence> answered;
        for (const QString &name :
             {QStringLiteral("homeTabAction"), QStringLiteral("moviesTabAction"),
              QStringLiteral("tvShowsTabAction"), QStringLiteral("favoritesTabAction"),
              QStringLiteral("playlistsTabAction"), QStringLiteral("settingsAction"),
              QStringLiteral("shortcutsAction")}) {
            QObject *action = ui.shellAction(name);
            QVERIFY2(action, qPrintable(name));
            answered.append(sequenceOf(action->property("shortcut")));
        }
        for (const QString &name :
             {QStringLiteral("previousTabShortcut"), QStringLiteral("nextTabShortcut")}) {
            QObject *shortcut = ui.shellAction(name);
            QVERIFY2(shortcut, qPrintable(name));
            for (const QVariant &sequence : shortcut->property("sequences").toList())
                answered.append(sequenceOf(sequence));
        }
        QVERIFY(answered.size() >= 11);
        for (const QKeySequence &sequence : std::as_const(answered)) {
            if (!sequence.isEmpty())
                QVERIFY2(listed.contains(sequence), qPrintable(sequence.toString()));
        }
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

    void theShortcutsDialogScrollsToItsLastRowInAShortWindow_data()
    {
        QTest::addColumn<int>("height");
        QTest::newRow("short") << 360;
        QTest::newRow("normal") << 640;
        QTest::newRow("tall") << 1000;
    }

    void theShortcutsDialogScrollsToItsLastRowInAShortWindow()
    {
        QFETCH(int, height);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        ui.window->resize(1000, height);
        QTRY_COMPARE(ui.window->height(), height);
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("shortcutsAction")), "trigger");
        QObject *dialog = ui.popup(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        // The opening animation moves the dialog; a binding loop shows as a warning (the harness fails the test on any)
        QTest::qWait(500);
        const QString lastRow = QStringLiteral("Next Chapter");
        QQuickItem *label = nullptr;
        for (QQuickItem *candidate : ui.itemsWithText(lastRow)) {
            if (candidate->isVisible() && candidate->inherits("QQuickText"))
                label = candidate;
        }
        QVERIFY(label);
        QQuickItem *flickable = Ui::enclosingFlickable(label);
        QVERIFY(flickable);
        const qreal contentHeight = flickable->property("contentHeight").toReal();
        const qreal viewHeight = flickable->height();
        // The dialog stays inside the window
        QVERIFY(dialog->property("height").toReal() <= height);
        if (height < 700)
            QVERIFY2(contentHeight > viewHeight, "a short window has to scroll");
        flickable->setProperty("contentY", qMax<qreal>(0, contentHeight - viewHeight));
        QTRY_VERIFY(flickable->mapFromItem(label, QPointF(0, label->height())).y()
                    <= viewHeight + 1);
        QVERIFY(flickable->mapFromItem(label, QPointF(0, 0)).y() >= -1);
        QTest::qWait(200);
    }

    void remoteDialogKeepsItsSizeFromOpeningToTheAnswer_data()
    {
        QTest::addColumn<bool>("empty");
        QTest::newRow("no sessions") << true;
        QTest::newRow("sessions") << false;
    }

    void remoteDialogKeepsItsSizeFromOpeningToTheAnswer()
    {
        QFETCH(bool, empty);
        Ui ui(m_home->path());
        if (empty)
            ui.server.listingRawBody.insert(QStringLiteral("Sessions"), QByteArrayLiteral("[]"));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // The answer is held, so the size is taken while the dialog still only spins
        ui.server.heldFamilies.insert(QStringLiteral("Sessions"));
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("remoteSessionsAction")),
                                          "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("remoteSessionsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(ui.server.heldCount() >= 1);
        // The baseline is taken once the popup's size pass has settled: two reads apart agree
        const auto settledSize = [dialog] {
            QSizeF last(-1, -1);
            for (int i = 0; i < 50; ++i) {
                const QSizeF now(dialog->property("width").toReal(),
                                 dialog->property("height").toReal());
                if (now == last)
                    return now;
                last = now;
                QTest::qWait(50);
            }
            return QSizeF();
        };
        const QSizeF opened = settledSize();
        const qreal width = opened.width();
        const qreal height = opened.height();
        QVERIFY(height > 0);

        // The fake answers a held request with an empty body, which fails it: the footer's Refresh then asks again at once instead of the next poll
        ui.server.heldFamilies.clear();
        ui.server.releaseHeld();
        QTRY_COMPARE(ui.controller.remoteSessions()->status(),
                     Kinefin::RemoteSessionsModel::Failed);
        QVERIFY(
            QMetaObject::invokeMethod(ui.popup(QStringLiteral("remoteRefreshAction")), "trigger"));
        QTRY_COMPARE(ui.controller.remoteSessions()->status(), Kinefin::RemoteSessionsModel::Ready);
        if (empty)
            QTRY_VERIFY(ui.item(QStringLiteral("remoteSessionsEmpty"))->isVisible());
        else
            QTRY_VERIFY(!ui.itemsStartingWith(QStringLiteral("remoteSessionRow")).isEmpty());
        QVERIFY(settledSize().isValid());
        QVERIFY(qAbs(dialog->property("width").toReal() - width) <= 1);
        QVERIFY2(
            qAbs(dialog->property("height").toReal() - height) <= 1,
            qPrintable(
                QStringLiteral("%1 -> %2").arg(height).arg(dialog->property("height").toReal())));
    }

    void shortcutsDialogAlignsTheKeysAndTheDescriptions()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("shortcutsAction")), "trigger");
        QObject *dialog = ui.popup(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTest::qWait(500);
        const QList<QQuickItem *> parts = ui.itemsStartingWith(QStringLiteral("shortcutsKeyPart"));
        QVERIFY(!parts.isEmpty());
        QMap<QQuickItem *, QList<QQuickItem *>> rows;
        for (QQuickItem *part : parts)
            rows[part->parentItem()].append(part);
        const auto left = [](const QQuickItem *item) {
            return item->mapToScene(QPointF(0, 0)).x();
        };
        qreal gap = -1;
        int combinations = 0;
        for (QList<QQuickItem *> &row : rows) {
            std::sort(row.begin(), row.end(),
                      [&](const QQuickItem *a, const QQuickItem *b) { return left(a) < left(b); });
            if (row.size() > 1)
                ++combinations;
            for (qsizetype i = 1; i < row.size(); ++i) {
                const qreal between =
                    left(row.at(i)) - (left(row.at(i - 1)) + row.at(i - 1)->width());
                if (gap < 0)
                    gap = between;
                QVERIFY2(qAbs(between - gap) <= 1, qPrintable(QString::number(between)));
            }
        }
        QVERIFY(combinations > 1);
        const QList<QQuickItem *> descriptions =
            ui.itemsStartingWith(QStringLiteral("shortcutsDescription"));
        QCOMPARE(descriptions.size(), rows.size());
        for (const QQuickItem *description : descriptions)
            QVERIFY(qAbs(left(description) - left(descriptions.constFirst())) <= 1);
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
        // The order inside the header: the six tabs, then the right cluster, the menu button last (Home has no Back button to come before them)
        const QList<QQuickItem *> tabs = ui.tabButtons();
        QCOMPARE(tabs.size(), 6);
        tabs.first()->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(tabs.first()->hasActiveFocus());
        for (const QQuickItem *tab : tabs.mid(1)) {
            QTest::keyClick(ui.window, Qt::Key_Tab);
            QTRY_VERIFY(tab->hasActiveFocus());
        }
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("headerSearchField"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(ui.item(QStringLiteral("queueButton"))->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(menuButton->hasActiveFocus());
        // Behind the menu button the page content begins
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QQuickItem *next = ui.window->activeFocusItem();
        QVERIFY(next && !tabs.contains(next) && next != menuButton
                && next->objectName() != QStringLiteral("headerSearchField")
                && next->objectName() != QStringLiteral("queueButton"));
        QVERIFY(ui.currentDetails()->isAncestorOf(next));
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

    void theGeneralSettingsWarnWhileCertificateErrorsAreIgnored()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors"), false));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settings")));
        auto *page = ui.item(QStringLiteral("settingsPage"));
        QVERIFY(page);
        QCOMPARE(page->property("activeSection").toString(), QStringLiteral("main"));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Ignore Certificate Errors")));
        QQuickItem *warning = ui.item(QStringLiteral("certificateErrorsWarning"));
        QVERIFY(warning);
        QVERIFY(!warning->isVisible());

        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors"), true));
        QTRY_VERIFY(warning->isVisible());
        QCOMPARE(warning->property("text").toString(),
                 QStringLiteral("Certificate errors are ignored: anyone on the network could read "
                                "the password and the session"));

        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors"), false));
        QTRY_VERIFY(!warning->isVisible());
    }

    // Forced by --ignore-certificate-errors: the box says so even with the switch off, which cannot undo it
    void theGeneralSettingsSayWhenTheCommandLineIgnoresCertificateErrors()
    {
        Ui ui(m_home->path());
        QVERIFY(
            ui.settings.setValue(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors"), false));
        ui.settings.setCertificateErrorsIgnoredByCommandLine(true);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settings")));
        QQuickItem *warning = ui.item(QStringLiteral("certificateErrorsWarning"));
        QVERIFY(warning);
        QTRY_VERIFY(warning->isVisible());
        QVERIFY(warning->property("text").toString().contains(
            QStringLiteral("for this run (--ignore-certificate-errors)")));
        // The switch says it too: on, and out of reach for the run
        QQuickItem *toggle = nullptr;
        for (QQuickItem *candidate : allItems(ui.item(QStringLiteral("settingsPage")))) {
            if (QString::fromLatin1(candidate->metaObject()->className())
                    .contains(QLatin1String("FormSwitchDelegate"))
                && candidate->property("text").toString()
                       == QStringLiteral("Ignore Certificate Errors"))
                toggle = candidate;
        }
        QVERIFY(toggle);
        QVERIFY(toggle->property("checked").toBool());
        QVERIFY(!toggle->isEnabled());
        QVERIFY(
            !ui.settings.value(QStringLiteral("main"), QStringLiteral("ignoreSSLErrors")).toBool());
    }

    void theRedesignedSettingsAreResponsive()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settings")));
        QVERIFY(ui.opensWindowOfWidth(1000));
        auto *page = ui.item(QStringLiteral("settingsPage"));
        QVERIFY(page);
        QTRY_VERIFY(!page->property("narrow").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
        // The list names itself, as System Settings does
        QQuickItem *listHeader = ui.item(QStringLiteral("settingsListHeader"));
        QVERIFY(listHeader && listHeader->isVisible());
        QCOMPARE(listHeader->property("text").toString(), QStringLiteral("Settings"));
        auto *audio = ui.item(QStringLiteral("settingsCategory_audio"));
        QVERIFY(audio);
        ui.clickCenter(audio);
        QTRY_COMPARE(page->property("activeSection").toString(), QStringLiteral("audio"));
        QCOMPARE(ui.stackDepth(), 2);
        // The groups of the category are cards of their own, each under its heading
        QTRY_VERIFY(ui.item(QStringLiteral("settingsGroupCard_1")) != nullptr);
        QVERIFY(ui.showsText(QStringLiteral("Output")));
        QVERIFY(ui.showsText(QStringLiteral("Passthrough")));
        // Narrow: the entry that was open stays open in place of the list, and Back brings the list back
        QVERIFY(ui.opensWindowOfWidth(440));
        QTRY_VERIFY(page->property("narrow").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QTRY_VERIFY(!ui.item(QStringLiteral("settingsCategory_main"))->isVisible());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsCategory_main"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QCOMPARE(ui.stackDepth(), 2);
        // An entry of the list opens in place, and the stack stays as deep
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_video")));
        QTRY_COMPARE(page->property("activeSection").toString(), QStringLiteral("video"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QCOMPARE(ui.stackDepth(), 2);
    }

    void theSettingsFormIsCenteredAtEveryWidth_data()
    {
        QTest::addColumn<int>("width");
        for (const int width : {440, 560, 800, 1000, 1600, 2560})
            QTest::addRow("%d", width) << width;
    }

    void theSettingsFormIsCenteredAtEveryWidth()
    {
        QFETCH(int, width);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settings")));
        QVERIFY(ui.opensWindowOfWidth(width));
        QStringList problems;
        QTRY_VERIFY2((problems = settingsFormProblems(ui, width)).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
        QTRY_VERIFY2((problems = ui.layoutProblems()).isEmpty(),
                     qPrintable(problems.join(QLatin1Char('\n'))));
    }

    void everyPageKeepsItsWidthRules_data()
    {
        QTest::addColumn<QString>("state");
        for (const QString &state :
             {QStringLiteral("home"), QStringLiteral("library"), QStringLiteral("details"),
              QStringLiteral("favorites"), QStringLiteral("preferences"), QStringLiteral("queue"),
              QStringLiteral("servers"), QStringLiteral("login"), QStringLiteral("settings")})
            QTest::addRow("%s", qPrintable(state)) << state;
    }

    void everyPageKeepsItsWidthRules()
    {
        QFETCH(QString, state);
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY2(buildState(ui, state), qPrintable(state));
        // One page built once and walked through the widths; Home, Servers and Sign In are judged for their layout only
        for (const int width : {440, 560, 800, 1000, 1600, 2560}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            QStringList problems;
            QTRY_VERIFY2(
                (problems = pageWidthProblems(ui, state, width)).isEmpty(),
                qPrintable(
                    QStringLiteral("%1 px: %2").arg(width).arg(problems.join(QLatin1Char('\n')))));
        }
    }

    void aboutIsAPopupAndDoesNotReplaceTheCurrentPage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("aboutAction")), "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("aboutDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
    }

    void serverFormAndMainHeaderWorkAtCompactAndWideWidths()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        auto *page = ui.item(QStringLiteral("serversPage"));
        QVERIFY(page);
        QVERIFY(ui.item(QStringLiteral("serverHostField")));
        const QQuickItem *port = ui.item(QStringLiteral("serverPortField"));
        QVERIFY(port);
        QVERIFY(port->property("text").toString().isEmpty());
        QVERIFY(!port->property("placeholderText").toString().isEmpty());
        QVERIFY(!ui.item(QStringLiteral("serverProtocolSelector")));
        QVERIFY(ui.item(QStringLiteral("testNewServerButton")));
        QVERIFY(ui.item(QStringLiteral("connectNewServerButton")));
        for (int width : {440, 1000, 1600}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            QTRY_COMPARE(page->property("splitLayout").toBool(), width >= 720);
            QTRY_VERIFY2(ui.layoutProblems().isEmpty(),
                         qPrintable(ui.layoutProblems().join(QLatin1Char('\n'))));
        }
    }

    void theServerFormBuildsAnAddressWithoutAScheme()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        auto *page = ui.item(QStringLiteral("serversPage"));
        QObject *host = ui.item(QStringLiteral("serverHostField"));
        QObject *port = ui.item(QStringLiteral("serverPortField"));
        QVERIFY(page && host && port);
        const auto address = [&](const QString &hostText, const QString &portText) {
            host->setProperty("text", hostText);
            port->setProperty("text", portText);
            return page->property("formAddress").toString();
        };
        QCOMPARE(address(QString(), QString()), QString());
        QCOMPARE(address(QStringLiteral("jellyfin.example.org"), QString()),
                 QStringLiteral("jellyfin.example.org"));
        QCOMPARE(address(QStringLiteral(" 192.168.1.5 "), QStringLiteral(" 8096 ")),
                 QStringLiteral("192.168.1.5:8096"));
        QCOMPARE(address(QStringLiteral("::1"), QStringLiteral("8096")),
                 QStringLiteral("[::1]:8096"));
        QCOMPARE(address(QStringLiteral("[::1]"), QStringLiteral("8920")),
                 QStringLiteral("[::1]:8920"));
        QCOMPARE(address(QStringLiteral("[::1]"), QString()), QStringLiteral("[::1]"));
        // One colon is a host with its port, not an IPv6 host
        QCOMPARE(address(QStringLiteral("jf.lan:8096"), QString()), QStringLiteral("jf.lan:8096"));
        QQuickItem *problem = ui.item(QStringLiteral("serverAddressProblem"));
        QVERIFY(problem);
        QVERIFY(!problem->isVisible());
        // A port that is not a number in 1..65535 leaves no address, and so no enabled Connect or Test, and the box says why
        for (const QString &bad : {QStringLiteral("0"), QStringLiteral("65536"),
                                   QStringLiteral("80a"), QStringLiteral("-1")}) {
            QCOMPARE(address(QStringLiteral("jellyfin.example.org"), bad), QString());
            QVERIFY(!ui.item(QStringLiteral("connectNewServerButton"))->isEnabled());
            QVERIFY(!ui.item(QStringLiteral("testNewServerButton"))->isEnabled());
            QTRY_VERIFY(problem->isVisible());
            QVERIFY(problem->property("text").toString().contains(QStringLiteral("port")));
        }
        // Hosts that are not hosts, as before
        for (const QString &bad :
             {QStringLiteral("http://example.org"), QStringLiteral("example.org/jellyfin"),
              QStringLiteral("user@example.org"), QStringLiteral("exa mple.org")}) {
            QCOMPARE(address(bad, QString()), QString());
            QTRY_VERIFY(problem->isVisible());
            QVERIFY(problem->property("text").toString().contains(QStringLiteral("scheme")));
        }
        QCOMPARE(address(QStringLiteral("example.org"), QStringLiteral("8096")),
                 QStringLiteral("example.org:8096"));
        QVERIFY(ui.item(QStringLiteral("connectNewServerButton"))->isEnabled());
        QTRY_VERIFY(!problem->isVisible());
        // An empty form is no mistake
        QCOMPARE(address(QString(), QString()), QString());
        QVERIFY(!problem->isVisible());
    }

    void savedServersAreOneCardEachInTheirOwnColumn()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(saveServerAndShowServers(ui));
        auto *page = ui.item(QStringLiteral("serversPage"));
        for (int width : {1000, 440}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            QTRY_COMPARE(page->property("splitLayout").toBool(), width >= 720);
            const bool split = page->property("splitLayout").toBool();
            QQuickItem *column = ui.item(QStringLiteral("savedServersColumn"));
            QQuickItem *add = ui.item(QStringLiteral("addServerCard"));
            QQuickItem *separator = ui.item(QStringLiteral("serversColumnSeparator"));
            QVERIFY(column && add && separator);
            QVERIFY(!ui.item(QStringLiteral("savedServerCard")));
            const QList<QQuickItem *> entries =
                Ui::allItemsNamed(column, QStringLiteral("savedServerEntry"));
            QCOMPARE(entries.size(), ui.controller.servers().size());
            for (QQuickItem *entry : entries)
                QCOMPARE(entry->parentItem(), column);
            QTRY_VERIFY(separator->isVisible());
            if (split) {
                QTRY_VERIFY(separator->height() > separator->width());
                const QRectF a = add->mapRectToScene(add->boundingRect());
                const QRectF c = column->mapRectToScene(column->boundingRect());
                const QRectF s = separator->mapRectToScene(separator->boundingRect());
                QVERIFY(a.right() <= s.left() && s.right() <= c.left());
            } else {
                QTRY_VERIFY(separator->width() > separator->height());
                const QRectF a = add->mapRectToScene(add->boundingRect());
                const QRectF c = column->mapRectToScene(column->boundingRect());
                const QRectF s = separator->mapRectToScene(separator->boundingRect());
                QVERIFY(a.bottom() <= s.top() && s.bottom() <= c.top());
            }
            QTRY_VERIFY2(ui.layoutProblems().isEmpty(),
                         qPrintable(ui.layoutProblems().join(QLatin1Char('\n'))));
        }
    }

    void connectingASavedServerWithASessionEndsOnHome()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        ui.controller.cancel();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QTRY_VERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        QQuickItem *connect = nullptr;
        QTRY_VERIFY((connect = buttonLabelled(ui.item(QStringLiteral("savedServerEntry")),
                                              QStringLiteral("Connect")))
                    != nullptr);
        ui.clickCenter(connect);
        QVERIFY(ui.window->property("startupVisible").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QTRY_VERIFY(!ui.window->property("startupVisible").toBool());
        QTRY_VERIFY(!ui.item(QStringLiteral("startupCover"))->isVisible());
        QVERIFY(ui.controller.librariesLoaded());
    }

    // Needs the scheme-less attempts of the core: HTTPS first, then HTTP on the given port
    void connectingFromTheFormShowsTheStartupCover()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        QObject *host = ui.item(QStringLiteral("serverHostField"));
        QObject *port = ui.item(QStringLiteral("serverPortField"));
        host->setProperty("text", QStringLiteral("127.0.0.1"));
        port->setProperty("text", QString::number(ui.server.url().port()));
        QQuickItem *connect = ui.item(QStringLiteral("connectNewServerButton"));
        QTRY_VERIFY(connect->isEnabled());
        ui.clickCenter(connect);
        QVERIFY(ui.window->property("startupVisible").toBool());
        // It fades in from nothing: visible from the first frame of the fade
        QTRY_VERIFY(ui.item(QStringLiteral("startupCover"))->isVisible());
        QTRY_VERIFY(ui.item(QStringLiteral("loginPage")) != nullptr);
        QTRY_VERIFY(!ui.window->property("startupVisible").toBool());
        QTRY_VERIFY(!ui.item(QStringLiteral("startupCover"))->isVisible());
    }

    // Needs the scheme-less attempts of the core: the tests of the form reach the server through them
    void testingFromTheFormShowsTheAddressThatAnswered()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        ui.item(QStringLiteral("serverHostField"))
            ->setProperty("text", QStringLiteral("127.0.0.1"));
        ui.item(QStringLiteral("serverPortField"))
            ->setProperty("text", QString::number(ui.server.url().port()));
        QQuickItem *test = ui.item(QStringLiteral("testNewServerButton"));
        QTRY_VERIFY(test->isEnabled());
        ui.clickCenter(test);
        // The message names the address that answered, once, as plain text
        QQuickItem *message = ui.item(QStringLiteral("newServerStatusMessage"));
        QVERIFY(message);
        const QString answered = QStringLiteral("127.0.0.1:%1").arg(ui.server.url().port());
        QTRY_VERIFY(message->isVisible()
                    && message->property("text").toString().contains(answered));
        QCOMPARE(message->property("text").toString().count(answered), 1);
        QCOMPARE(message->property("textFormat").toInt(), int(Qt::PlainText));
    }

    void anUnreachableServerEndsOnTheServersPageWithTheErrorAndNoCover()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        const quint16 closed = closedPort();
        QVERIFY(closed != 0);
        ui.item(QStringLiteral("serverHostField"))
            ->setProperty("text", QStringLiteral("127.0.0.1"));
        ui.item(QStringLiteral("serverPortField"))->setProperty("text", QString::number(closed));
        QQuickItem *connect = ui.item(QStringLiteral("connectNewServerButton"));
        QTRY_VERIFY(connect->isEnabled());
        ui.clickCenter(connect);
        QVERIFY(ui.window->property("startupVisible").toBool());
        QTRY_VERIFY(!ui.controller.errorText().isEmpty());
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QTRY_VERIFY(!ui.window->property("startupVisible").toBool());
        QTRY_VERIFY(!ui.item(QStringLiteral("startupCover"))->isVisible());
        QVERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        QVERIFY(!ui.controller.errorText().isEmpty());
    }

    void theConnectCoverCancelsASilentServer()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        // A host that takes the connection and never answers keeps the attempts going
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        ui.item(QStringLiteral("serverHostField"))
            ->setProperty("text", QStringLiteral("127.0.0.1"));
        ui.item(QStringLiteral("serverPortField"))
            ->setProperty("text", QString::number(silent.serverPort()));
        QQuickItem *connect = ui.item(QStringLiteral("connectNewServerButton"));
        QTRY_VERIFY(connect->isEnabled());
        ui.clickCenter(connect);
        QQuickItem *cancel = ui.item(QStringLiteral("startupCoverCancel"));
        QVERIFY(cancel);
        QTRY_VERIFY(ui.item(QStringLiteral("startupCover"))->isVisible() && cancel->isVisible());
        // The click waits for the end of the fade: before it the column may not have placed the button yet
        QTRY_COMPARE(ui.item(QStringLiteral("startupCover"))->opacity(), 1.0);
        QTRY_VERIFY(cancel->width() > 0 && ui.insideWindow(cancel));
        QCOMPARE(ui.controller.state(), ConnectionController::Connecting);
        ui.clickCenter(cancel);
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QTRY_VERIFY(!ui.window->property("startupVisible").toBool());
        QTRY_VERIFY(!ui.item(QStringLiteral("startupCover"))->isVisible());
        QVERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        // The cover of the app's start has no Cancel
        QVERIFY(!ui.window->property("connectCover").toBool());
    }

    void anInvalidAddressNeverShowsTheCover()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("servers")));
        // The cover of the start has faded out before the test begins
        QTRY_VERIFY(!ui.item(QStringLiteral("startupCover"))->isVisible());
        auto *page = ui.item(QStringLiteral("serversPage"));
        // The form refuses it: Connect stays disabled
        ui.item(QStringLiteral("serverHostField"))->setProperty("text", QStringLiteral("exa mple"));
        QVERIFY(!ui.item(QStringLiteral("connectNewServerButton"))->isEnabled());
        // And an address that gets past the page is refused by the core without a connection
        QVERIFY(QMetaObject::invokeMethod(page, "connectAddress",
                                          Q_ARG(QVariant, QStringLiteral("exa mple")),
                                          Q_ARG(QVariant, true)));
        QVERIFY(ui.controller.state() != ConnectionController::Connecting);
        QVERIFY(!ui.window->property("startupVisible").toBool());
        QTest::qWait(100);
        QVERIFY(!ui.window->property("startupVisible").toBool());
        QVERIFY(!ui.item(QStringLiteral("startupCover"))->isVisible());
    }

    void sectionsNeverGrowTheStack()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        for (const char *name :
             {"settingsAction", "aboutAction", "settingsAction", "aboutAction", "settingsAction"}) {
            QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QLatin1String(name)), "trigger"));
            if (QLatin1String(name) == QLatin1String("aboutAction")) {
                QObject *dialog = ui.popup(QStringLiteral("aboutDialog"));
                QVERIFY(dialog);
                QTRY_VERIFY(dialog->property("opened").toBool());
                QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("settingsPage"));
                QTRY_COMPARE(ui.stackDepth(), 2);
                QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            } else {
                QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("settingsPage"));
                QTRY_COMPARE(ui.stackDepth(), 2);
            }
        }
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 2);
        // On a type tab Ctrl+F searches the tab's own list: the search page opens from Home
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("homeTabAction")), "trigger"));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_VERIFY(!ui.tabSwitching());
        // The queue and the search replace each other and themselves above the root
        QVERIFY(ui.openSearchPage() != nullptr);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("searchPage"));
        QCOMPARE(ui.stackDepth(), 2);
        for (int round = 0; round < 2; ++round) {
            ui.clickCenter(ui.item(QStringLiteral("queueButton")));
            QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("queuePage"));
            QCOMPARE(ui.stackDepth(), 2);
            QVERIFY(ui.openSearchPage() != nullptr);
            QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("searchPage"));
            QCOMPARE(ui.stackDepth(), 2);
        }
        // Favorites and Playlists are roots: opening them again and again keeps one page
        for (const char *name :
             {"favoritesTabAction", "playlistsTabAction", "favoritesTabAction"}) {
            QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QLatin1String(name)), "trigger"));
            QTRY_VERIFY(!ui.tabSwitching());
            QCOMPARE(ui.stackDepth(), 1);
        }
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("favorites"));
    }

    void theDownloadsTabDownloadsAnItem()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        ui.serveMovie();
        // The fake sends 64 KiB from its Download route: the record the page reads must expect that file
        QJsonObject movie = QJsonDocument::fromJson(ui.server.itemsById.value(kMovieId)).object();
        QJsonArray sources = movie.value(QStringLiteral("MediaSources")).toArray();
        QJsonObject source = sources.first().toObject();
        source.insert(QStringLiteral("Size"), 64 * 1024);
        source.insert(QStringLiteral("ETag"), QStringLiteral("etag-1"));
        sources.replace(0, source);
        movie.insert(QStringLiteral("MediaSources"), sources);
        ui.server.itemsById.insert(kMovieId, QJsonDocument(movie).toJson(QJsonDocument::Compact));

        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("downloadsTabAction")),
                                          "trigger"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("downloadsPage"));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("downloads"));
        QVERIFY(ui.showsText(QStringLiteral("No downloads yet")));
        // The footer label keeps the same distance from the left edge as from the top and bottom
        QQuickItem *footer = ui.item(QStringLiteral("downloadsFooter"));
        QVERIFY(footer);
        QQuickItem *count = ui.item(QStringLiteral("downloadsFooterCount"));
        QVERIFY(count);
        const QPointF at = count->mapToItem(footer, QPointF(0, 0));
        // The same distance from the left edge as from the top
        QCOMPARE(at.x(), at.y());

        QVERIFY(ui.controller.downloads()->downloadItem(kMovieId));
        QTRY_VERIFY_WITH_TIMEOUT(ui.showsText(QStringLiteral("Arcturus (2012)")), scaled(10000));
        QTRY_VERIFY_WITH_TIMEOUT(ui.showsText(QStringLiteral("Available on This Device")),
                                 scaled(10000));
        QCOMPARE(ui.controller.downloads()->downloadState(kMovieId), QStringLiteral("completed"));
        // The completed row offers Play; the shell test has no player, so only its presence is checked
        QQuickItem *play = ui.item(QStringLiteral("downloadPlay_") + kMovieId);
        QVERIFY(play && play->isVisible());
        QQuickItem *poster = ui.item(QStringLiteral("downloadPoster_") + kMovieId);
        QVERIFY(poster);
        QVERIFY(poster->property("source").toUrl().isValid());
        // A local completion queues one watched-state event, which the sign-in reconciliation sends
        ui.controller.downloads()->updateLocalProgress(ui.controller.playbackClient().serverId(),
                                                       ui.controller.playbackClient().userId(),
                                                       kMovieId, 600000000, true);
        QCOMPARE(ui.controller.downloads()->pendingSyncCount(), 1);
        QVERIFY(ui.showsText(QStringLiteral("1 item to sync")));
        ui.controller.downloads()->reconcileOutbox();
        QTRY_COMPARE_WITH_TIMEOUT(ui.controller.downloads()->pendingSyncCount(), 0, scaled(5000));
        // Export keeps the sandbox copy and writes the host layout below the chosen folder
        QTemporaryDir host;
        QVERIFY(host.isValid());
        ui.controller.downloads()->setKodiSidecars(true);
        ui.controller.downloads()->exportDownload(ui.controller.playbackClient().serverId(),
                                                  ui.controller.playbackClient().userId(), kMovieId,
                                                  QUrl::fromLocalFile(host.path()));
        QTRY_COMPARE_WITH_TIMEOUT(ui.controller.downloads()->downloadState(kMovieId),
                                  QStringLiteral("exported"), scaled(5000));
        QVERIFY(QFile::exists(host.path() + QStringLiteral("/Movies/Arcturus (2012) (2012).nfo")));
        QVERIFY2(QDir(host.path() + QStringLiteral("/Movies")).exists(),
                 qPrintable(QDir(host.path())
                                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                                .join(QLatin1Char(','))));
        QVERIFY(QFile::exists(host.path() + QStringLiteral("/Movies/Arcturus (2012) (2012).mkv")));
        // The completed row has Play and one More menu; Delete Download asks first, then drops the row and the record
        QQuickItem *more = ui.item(QStringLiteral("downloadMore_") + kMovieId);
        QVERIFY(more && more->isVisible());
        // At most two trailing controls: the primary action and the menu, each with a distinct name
        QCOMPARE(ui.itemsStartingWith(QStringLiteral("downloadPlay_")).size(), 1);
        QVERIFY(ui.itemsStartingWith(QStringLiteral("downloadRemove_")).isEmpty());
        ui.clickCenter(more);
        QQuickItem *remove = nullptr;
        QTRY_VERIFY((remove = ui.item(QStringLiteral("downloadDelete_") + kMovieId)) != nullptr
                    && remove->isVisible());
        QVERIFY(remove->isEnabled());
        QObject *dialog =
            ui.currentDetails()->findChild<QObject *>(QStringLiteral("downloadsPageRemoveDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(remove, "click"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(ui.controller.downloads()->downloadState(kMovieId), QStringLiteral("exported"));
        QVERIFY(QMetaObject::invokeMethod(remove, "click"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QObject *confirm = dialog->findChild<QObject *>(QStringLiteral("removeDownloadConfirm"));
        QVERIFY(confirm);
        QVERIFY(QMetaObject::invokeMethod(confirm, "trigger"));
        QTRY_VERIFY_WITH_TIMEOUT(ui.controller.downloads()->downloadState(kMovieId).isEmpty(),
                                 scaled(5000));
        QTRY_VERIFY(ui.showsText(QStringLiteral("No downloads yet")));
    }

    void aSeriesRuleFollowsTheSeriesPage()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *series = openDetails(ui, kSeriesId);
        QVERIFY(series);
        QObject *action =
            ui.currentDetails()->findChild<QObject *>(QStringLiteral("automaticDownloadsAction"));
        QVERIFY(action);
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
        QObject *dialog = ui.currentDetails()->findChild<QObject *>(QStringLiteral("ruleDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QObject *save = dialog->findChild<QObject *>(QStringLiteral("ruleDialogSave"));
        QVERIFY(save);
        QVERIFY(QMetaObject::invokeMethod(save, "trigger"));
        QTRY_COMPARE(ui.controller.downloads()
                         ->seriesRule(kSeriesId)
                         .value(QStringLiteral("kind"))
                         .toString(),
                     QStringLiteral("nextN"));
        QObject *stop =
            ui.currentDetails()->findChild<QObject *>(QStringLiteral("stopDownloadsAction"));
        QVERIFY(stop);
        QVERIFY(QMetaObject::invokeMethod(stop, "trigger"));
        QTRY_VERIFY(ui.controller.downloads()->seriesRule(kSeriesId).isEmpty());
    }

    void reopeningTheRuleDialogShowsTheStoredKind()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QVERIFY(ui.controller.downloads()->setSeriesRule(kSeriesId, QStringLiteral("Series"),
                                                         QStringLiteral("newEpisodes")));
        QQuickItem *series = openDetails(ui, kSeriesId);
        QVERIFY(series);
        QObject *action =
            ui.currentDetails()->findChild<QObject *>(QStringLiteral("automaticDownloadsAction"));
        QVERIFY(action);
        QObject *dialog = ui.currentDetails()->findChild<QObject *>(QStringLiteral("ruleDialog"));
        QVERIFY(dialog);
        for (int round = 0; round < 2; ++round) {
            QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
            QTRY_VERIFY(dialog->property("opened").toBool());
            QCOMPARE(dialog->property("selectedKind").toString(), QStringLiteral("newEpisodes"));
            QObject *radio = dialog->findChild<QObject *>(QStringLiteral("ruleKindNewEpisodes"));
            QVERIFY(radio);
            QVERIFY(radio->property("checked").toBool());
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            QTRY_VERIFY(!dialog->property("opened").toBool());
        }
    }

    void anItemThatCannotBeDownloadedHasNoAction()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QJsonObject movie = QJsonDocument::fromJson(ui.server.itemsById.value(kMovieId)).object();
        movie.insert(QStringLiteral("CanDownload"), false);
        ui.server.itemsById.insert(kMovieId, QJsonDocument(movie).toJson(QJsonDocument::Compact));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));

        QSignalSpy failed(ui.controller.downloads(), &Kinefin::DownloadsController::downloadFailed);
        QVERIFY(ui.controller.downloads()->downloadItem(kMovieId));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("unsupported"));
        QVERIFY(ui.controller.downloads()->downloadState(kMovieId).isEmpty());

        // The details page hides the action for the same item
        QVERIFY(QMetaObject::invokeMethod(ui.window, "openItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, QVariant())));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QTRY_VERIFY(!ui.currentDetails()->property("downloadable").toBool());
    }

    void workOfflineShowsTheDownloadsWithoutASession()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        ui.serveMovie();
        QJsonObject movie = QJsonDocument::fromJson(ui.server.itemsById.value(kMovieId)).object();
        QJsonArray sources = movie.value(QStringLiteral("MediaSources")).toArray();
        QJsonObject source = sources.first().toObject();
        source.insert(QStringLiteral("Size"), 64 * 1024);
        source.insert(QStringLiteral("ETag"), QStringLiteral("etag-1"));
        sources.replace(0, source);
        movie.insert(QStringLiteral("MediaSources"), sources);
        ui.server.itemsById.insert(kMovieId, QJsonDocument(movie).toJson(QJsonDocument::Compact));
        QVERIFY(ui.controller.downloads()->downloadItem(kMovieId));
        QTRY_COMPARE_WITH_TIMEOUT(ui.controller.downloads()->downloadState(kMovieId),
                                  QStringLiteral("completed"), scaled(10000));

        // Work Offline belongs to a connection that failed or ended, not to a live session
        ui.controller.cancel();
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QVERIFY(ui.controller.canWorkOffline());
        QVERIFY(ui.controller.workOffline());
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Offline);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("downloadsPage"));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Arcturus (2012)")));
        // Only the Downloads tab is offered, and the menu leaves Offline
        QVERIFY(!ui.shellAction(QStringLiteral("homeTabAction"))->property("visible").toBool());
        QVERIFY(ui.shellAction(QStringLiteral("downloadsTabAction"))->property("visible").toBool());
        QVERIFY(ui.shellAction(QStringLiteral("leaveOfflineAction"))->property("visible").toBool());
        QVERIFY(
            !ui.shellAction(QStringLiteral("switchServerAction"))->property("visible").toBool());
        // A downloaded item plays without a session: the player page opens in Offline and leaves with the playback
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        QVariant shown;
        QVERIFY(QMetaObject::invokeMethod(root, "showPlayerPage", Q_RETURN_ARG(QVariant, shown)));
        QVERIFY(shown.toBool());
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        QVERIFY(QMetaObject::invokeMethod(root, "endPlayback"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("downloadsPage"));
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("leaveOfflineAction")),
                                          "trigger"));
        QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("serversPage"));
    }

    void theDownloadsPageFollowsTheServerPolicy()
    {
        Ui ui(m_home->path());
        ui.server.canDownload = false;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QCOMPARE(ui.controller.downloads()->canDownload(), false);

        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("downloadsTabAction")),
                                          "trigger"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("downloadsPage"));
        QTRY_VERIFY(ui.item(QStringLiteral("downloadsPermissionNote"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("This server does not allow downloads for this user"),
                             false));

        // The Settings entry disappears with the permission
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) != nullptr);
        QVERIFY(!ui.item(QStringLiteral("settingsCategory_downloads")));
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("downloadsTabAction")),
                                          "trigger"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("downloadsPage"));
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
        for (const char *name : {"downloadsTabAction", "favoritesTabAction", "playlistsTabAction"})
            QVERIFY2(!ui.shellAction(QLatin1String(name))->property("enabled").toBool(), name);
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
        // The menu button is always there, with the app entries and no avatar
        QVERIFY(ui.item(QStringLiteral("mainMenuButton"))->isVisible());
        const QQuickItem *avatar = ui.item(QStringLiteral("accountAvatar"));
        QVERIFY(!avatar || !avatar->isVisible());

        QVERIFY(buildState(ui, QStringLiteral("login")));
        // A chosen server brings Switch Server in, and only that of the account entries
        QVERIFY(ui.item(QStringLiteral("mainMenuButton"))->isVisible());
        QVERIFY(ui.shellAction(QStringLiteral("switchServerAction"))->property("visible").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("signOutAction"))->property("visible").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("switchUserAction"))->property("visible").toBool());
    }

    void theMainMenuSwitchUserOpensPicker()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(!ui.store.tokens.isEmpty());
        const int logins = ui.server.successfulLogins;
        // The main menu opens from its button, and Switch User is one of its entries
        ui.clickCenter(ui.item(QStringLiteral("mainMenuButton")));
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(ui.popup(QStringLiteral("switchUserItem")), "click"));
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

    void theMenuButtonShowsTheAvatarOfTheUser()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        ui.serveUsers();
        ui.controller.connectTo(address(ui.server));
        QVERIFY(ui.reach(ConnectionController::NeedsLogin, QStringLiteral("loginPage")));
        // Not signed in yet: the plain menu icon and no avatar
        QQuickItem *button = ui.item(QStringLiteral("mainMenuButton"));
        QVERIFY(button && button->isVisible());
        QVERIFY(!ui.item(QStringLiteral("accountAvatar"))
                || !ui.item(QStringLiteral("accountAvatar"))->isVisible());
        QCOMPARE(accessibleNameOf(button), QStringLiteral("Main Menu"));
        QVERIFY(ui.signInAs(QStringLiteral("Alice")));
        const QQuickItem *avatar = nullptr;
        QTRY_VERIFY((avatar = ui.item(QStringLiteral("accountAvatar"))) != nullptr
                    && avatar->isVisible());
        // Alice has an image, the avatar asks for it; the user list reaches the header just after the session does
        QTRY_COMPARE(avatar->property("name").toString(), QStringLiteral("Alice"));
        QTRY_VERIFY(!avatar->property("source").toUrl().isEmpty());
        // The label stays static: the name is only in the accessible name
        QVERIFY(button->property("text").toString().isEmpty());
        QCOMPARE(button->property("label").toString(), QStringLiteral("Main Menu"));
        QCOMPARE(accessibleNameOf(button), QStringLiteral("Main Menu, signed in as Alice"));
    }

    void theMenuButtonShowsTheInitialsOfAUserWithoutAnImage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signInAsUser(QStringLiteral("Bob")));
        const QQuickItem *avatar = nullptr;
        QTRY_VERIFY((avatar = ui.item(QStringLiteral("accountAvatar"))) != nullptr
                    && avatar->isVisible());
        // No image: the source stays empty and the avatar draws the initials of the name, once the user list reached the header
        QTRY_COMPARE(avatar->property("name").toString(), QStringLiteral("Bob"));
        QVERIFY(avatar->property("source").toUrl().isEmpty());
        QTRY_COMPARE(accessibleNameOf(ui.item(QStringLiteral("mainMenuButton"))),
                     QStringLiteral("Main Menu, signed in as Bob"));
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
        // Three columns hold the three tiles in one row, two wrap the last one, and the narrowest window the shell allows keeps as many columns as whole cells fit in the grid
        QVERIFY(ui.opensWindowOfWidth(cell * 3 + 120));
        QTRY_COMPARE(distinct(true), 1);
        QTRY_VERIFY(tilesInside());
        QVERIFY(ui.opensWindowOfWidth(cell * 2 + 60));
        QTRY_COMPARE(distinct(true), 2);
        QTRY_COMPARE(distinct(false), 2);
        QTRY_VERIFY(tilesInside());
        QVERIFY(ui.opensWindowOfWidth(ui.window->minimumWidth()));
        QTRY_COMPARE(grid->width() <= ui.window->width(), true);
        const int columns = qBound(1, int(grid->width()) / cell, 3);
        QTRY_COMPARE(distinct(false), columns);
        QTRY_COMPARE(distinct(true), (3 + columns - 1) / columns);
        QTRY_VERIFY(tilesInside());
    }

    void signOutForgetsEverythingAndReturnsToTheServers()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        ui.clickCenter(ui.item(QStringLiteral("mainMenuButton")));
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
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
            QStringLiteral("Sign out %1? Saved credentials and the local queue will be removed. "
                           "The server account stays unchanged.")
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
        ui.clickCenter(ui.item(QStringLiteral("mainMenuButton")));
        QTRY_VERIFY(ui.popup(QStringLiteral("mainMenu"))->property("opened").toBool());
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
        QTRY_VERIFY(listBuilt(row));
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

    void aPadArrowLeavesTheScrollViewForTheFirstBlock()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // Rows load asynchronously: the first arrow comes once the blocks are there
        QTRY_VERIFY(ui.item(QStringLiteral("homeRowList_0")));
        QQuickItem *focus = ui.window->activeFocusItem();
        QVERIFY(focus);
        QVERIFY2(focus->inherits("QQuickScrollView"), focus->metaObject()->className());
        // The socket and the pad share the dispatcher: a Down on the page's scroll view moves into the content
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const auto focusedRow = [&ui]() -> QString {
            for (QQuickItem *item = ui.window->activeFocusItem(); item; item = item->parentItem()) {
                if (item->objectName().startsWith(QLatin1String("homeRowList_")))
                    return item->objectName();
            }
            return {};
        };
        // Where the focus went instead, for a failure on a slow runner
        const auto focusPath = [&ui] {
            QStringList path;
            for (QQuickItem *item = ui.window->activeFocusItem(); item && path.size() < 4;
                 item = item->parentItem()) {
                const QRectF rect =
                    item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                path << QStringLiteral("%1 at %2,%3")
                            .arg(Ui::describe(item))
                            .arg(rect.x())
                            .arg(rect.y());
            }
            return path.join(QLatin1String(" < "));
        };
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        // The first block is the library tiles, which come above the rows
        QTRY_VERIFY(ui.window->activeFocusItem()
                    && ui.window->activeFocusItem()->objectName().startsWith(
                        QLatin1String("libraryCard_")));
        // Down from a tile enters the rows below: a card of the row takes the focus
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QTRY_VERIFY(!focusedRow().isEmpty());
        const QString firstRow = focusedRow();
        // Inside a row the list keeps the arrows: Right moves its current card, the focus stays in the row
        QQuickItem *list = ui.item(firstRow);
        QVERIFY(list);
        if (list->property("count").toInt() > 1) {
            QCOMPARE(list->property("currentIndex").toInt(), 0);
            QVERIFY(input.dispatchKeycode(QStringLiteral("right")));
            QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
            QCOMPARE(focusedRow(), firstRow);
            QVERIFY(input.dispatchKeycode(QStringLiteral("left")));
            QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
            QCOMPARE(focusedRow(), firstRow);
        }
        // Up from the first row goes back to the tiles above it
        QVERIFY(input.dispatchKeycode(QStringLiteral("up")));
        QTRY_VERIFY2(ui.window->activeFocusItem()
                         && ui.window->activeFocusItem()->objectName().startsWith(
                             QLatin1String("libraryCard_")),
                     qPrintable(firstRow + QLatin1String(": ") + focusPath()));
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QTRY_COMPARE(focusedRow(), firstRow);
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QTRY_VERIFY(!focusedRow().isEmpty() && focusedRow() != firstRow);
    }

    void downKeepsTheFocusedRowInView()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        // A low window: the rows do not all fit, so the page has to scroll
        const int low = ui.window->minimumHeight();
        QVERIFY(low > 0 && low < 640);
        ui.window->resize(1000, low);
        QTRY_COMPARE(ui.window->height(), low);
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
        // Up goes back, and Down from the last row stays where it is: the library tiles are above the rows
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(lists.at(lists.size() - 2)->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(lists.constLast()->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTest::qWait(200);
        QVERIFY(lists.constLast()->hasActiveFocus());
    }

    // A season page has the episodes list as its flickable and the whole page as the list's header: a pad's Down on a header control used to move the list's current row and leave the focus where it was
    void aPadArrowInTheSeasonHeaderMovesTheFocusNotTheEpisodeRow()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        ui.server.generatedEpisodeTotal = 12;
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeasonId);
        QVERIFY(ui.signIn());
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("episodesList"))) != nullptr);
        QTRY_VERIFY(list->property("count").toInt() > 1);
        QQuickItem *page = ui.item(QStringLiteral("itemDetailsPage"));
        QVERIFY(page);
        QQuickItem *control = nullptr;
        for (const char *name :
             {"detailsPlay", "detailsPlayNarrow", "seriesLink", "seasonLink", "overviewToggle"}) {
            QQuickItem *candidate = Ui::itemIn(page, QString::fromLatin1(name));
            if (candidate && candidate->isVisible() && candidate->isEnabled()
                && candidate->activeFocusOnTab()) {
                control = candidate;
                break;
            }
        }
        QVERIFY(control);
        control->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(control->hasActiveFocus());
        const int row = list->property("currentIndex").toInt();
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QCOMPARE(list->property("currentIndex").toInt(), row);
        QVERIFY(ui.window->activeFocusItem() != control);
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        if (list->hasActiveFocus()) {
            // The presses reached the rows, on the row the list had: there the arrows are the list's own
            QCOMPARE(list->property("currentIndex").toInt(),
                     qMin(row + 1, list->property("count").toInt() - 1));
        } else {
            // Still in the header: the second press does not move the rows either
            QCOMPARE(list->property("currentIndex").toInt(), row);
        }
    }

    // The Settings combo boxes apply a value at once: a pad's Down on a closed one used to step it
    void aPadDownPassesAClosedSettingsComboBox()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settingsSection")));
        QQuickItem *view = ui.item(QStringLiteral("settingsContentView"));
        QVERIFY(view);
        // The page slides in disabled and unsized: its controls take the focus once it settled
        QTRY_VERIFY(view->isEnabled() && view->width() > 0);
        QQuickItem *combo = nullptr;
        for (QQuickItem *candidate : allItems(view)) {
            if (candidate->inherits("QQuickComboBox") && candidate->isVisible()
                && (!combo
                    || candidate->mapToScene(QPointF(0, 0)).y()
                           < combo->mapToScene(QPointF(0, 0)).y()))
                combo = candidate;
        }
        QVERIFY(combo);
        const QVariant device =
            ui.settings.value(QStringLiteral("audio"), QStringLiteral("devicetype"));
        const QVariant channels =
            ui.settings.value(QStringLiteral("audio"), QStringLiteral("channels"));
        const int index = combo->property("currentIndex").toInt();
        combo->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(combo->hasActiveFocus());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QVERIFY(ui.window->activeFocusItem() != combo);
        QCOMPARE(combo->property("currentIndex").toInt(), index);
        QCOMPARE(ui.settings.value(QStringLiteral("audio"), QStringLiteral("devicetype")), device);
        QCOMPARE(ui.settings.value(QStringLiteral("audio"), QStringLiteral("channels")), channels);
    }

    void padReachesEveryHomeControl()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        // Enough Continue Watching cards for the last one to sit under the row's See All
        ui.server.generatedResumeTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QTRY_VERIFY(ui.item(QStringLiteral("homeRowList_0")) != nullptr);
        QTRY_VERIFY(listBuilt(ui.item(QStringLiteral("homeRowList_0"))));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const QList<QQuickItem *> reached = padReach(ui, input);
        const QStringList names = namesOf(reached);
        const auto reaches = [&](const QString &name) { return names.contains(name); };
        QVERIFY2(std::any_of(names.cbegin(), names.cend(),
                             [](const QString &name) {
                                 return name.startsWith(QLatin1String("libraryCard_"));
                             }),
                 qPrintable(names.join(QLatin1Char(' '))));
        QVERIFY2(reaches(QStringLiteral("homeRowList_0")),
                 qPrintable(names.join(QLatin1Char(' '))));
        QVERIFY2(reaches(QStringLiteral("homeRowSeeAll_0")),
                 qPrintable(names.join(QLatin1Char(' '))));
        // The header's tabs, above the page
        QVERIFY2(std::any_of(reached.cbegin(), reached.cend(),
                             [&ui](const QQuickItem *item) {
                                 return ui.shellHeader()->isAncestorOf(item);
                             }),
                 qPrintable(names.join(QLatin1Char(' '))));
    }

    void upUnderSeeAllReachesSeeAll()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.server.generatedResumeTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
        QTRY_VERIFY(listBuilt(list));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QQuickItem *seeAll = ui.item(QStringLiteral("homeRowSeeAll_0"));
        QVERIFY(seeAll && seeAll->isVisible());
        // The card under the middle of the button becomes the row's current card; the middle may fall in the gap between two cards, and under load the cards at the edge come a little later
        const auto cardUnderSeeAll = [list, seeAll] {
            const QPointF middle =
                list->mapFromScene(seeAll->mapToScene(QPointF(seeAll->width() / 2, 0)));
            for (const double aside : {0.0, -8.0, 8.0, -16.0, 16.0, -32.0, 32.0}) {
                int index = -1;
                QMetaObject::invokeMethod(
                    list, "indexAt", Q_RETURN_ARG(int, index),
                    Q_ARG(double, list->property("contentX").toDouble() + middle.x() + aside),
                    Q_ARG(double, 10.0));
                if (index >= 0)
                    return index;
            }
            return -1;
        };
        int under = -1;
        QTRY_VERIFY((under = cardUnderSeeAll()) >= 0);
        QVERIFY(list->setProperty("currentIndex", under));
        QVERIFY(QMetaObject::invokeMethod(list, "forceActiveFocus",
                                          Q_ARG(Qt::FocusReason, Qt::TabFocusReason)));
        QTRY_VERIFY(list->hasActiveFocus());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        QVERIFY(input.dispatchKeycode(QStringLiteral("up")));
        QCOMPARE(ui.window->activeFocusItem(), seeAll);
        // And Down returns to the row: the focus sits on its current card, inside the list
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QVERIFY(list->hasActiveFocus());
        QCOMPARE(list->property("currentIndex").toInt(), under);
    }

    void downFromAHeaderTabEntersThePage()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *home = ui.item(QStringLiteral("homePage"));
        QVERIFY(home);
        const QList<QQuickItem *> tabs = ui.tabButtons();
        QVERIFY(!tabs.isEmpty());
        tabs.constFirst()->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(tabs.constFirst()->hasActiveFocus());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QTRY_VERIFY(ui.window->activeFocusItem()
                    && home->isAncestorOf(ui.window->activeFocusItem()));
    }

    void enteringALibrarySelectsNothing()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_VERIFY(grid->hasActiveFocus());
        // The first card is current for Return but not highlighted until the user moves
        const auto firstCard = [&]() -> QQuickItem * {
            for (QQuickItem *card : allItems(grid)) {
                if (card->objectName().startsWith(QStringLiteral("mediaCard_"))
                    && card->isVisible())
                    return card;
            }
            return nullptr;
        };
        QQuickItem *first = nullptr;
        QTRY_VERIFY((first = firstCard()) != nullptr);
        QVERIFY(!first->property("highlighted").toBool());
        QVERIFY(!first->property("focusVisible").toBool());
        const auto anyHighlighted = [&] {
            for (QQuickItem *card : allItems(grid)) {
                if (card->objectName().startsWith(QStringLiteral("mediaCard_")) && card->isVisible()
                    && card->property("highlighted").toBool())
                    return true;
            }
            return false;
        };
        QVERIFY(!anyHighlighted());
        QTest::keyClick(ui.window, Qt::Key_Right);
        QTRY_VERIFY(anyHighlighted());
        QTRY_VERIFY(first->property("focusVisible").toBool());
    }

    void theCurrentHomeCardShowsTheFocusRingAfterKeyboardEntry()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.server.listingRawBody.insert(
            QStringLiteral("NextUp"),
            envelope(QJsonArray{fixtureObject(QStringLiteral("movie_item"))}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = list->property("currentItem").value<QQuickItem *>()) != nullptr);
        // The cards take no focus of their own: the row drives the current one
        list->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(list->hasActiveFocus());
        QTRY_VERIFY(card->property("highlighted").toBool());
        QVERIFY(cardRingShown(card));
        // Focus elsewhere: the ring goes
        QVERIFY(focusOutsideThePage(ui));
        QTRY_VERIFY(!list->hasActiveFocus());
        QTRY_VERIFY(!cardRingShown(card));
    }

    void tabbingBackIntoTheLibraryGridShowsTheCurrentCard()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_VERIFY(grid->hasActiveFocus());
        const auto currentCard = [&]() -> QQuickItem * {
            return grid->property("currentItem").value<QQuickItem *>();
        };
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = currentCard()) != nullptr);
        // The page's own focus selects nothing
        QVERIFY(!card->property("highlighted").toBool());
        QVERIFY(!cardRingShown(card));
        // The user's Backtab into the grid shows where the current card is
        QVERIFY(focusOutsideThePage(ui));
        QTRY_VERIFY(!grid->hasActiveFocus());
        grid->forceActiveFocus(Qt::BacktabFocusReason);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTRY_VERIFY(card->property("highlighted").toBool());
        QVERIFY(cardRingShown(card));
    }

    void theCardMenuPlayOnIsDisabledWhileFollowingAGroup()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.server.listingRawBody.insert(QStringLiteral("NextUp"), envelope({}));
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{fixtureObject(QStringLiteral("movie_item"))}));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = Ui::itemIn(page, QStringLiteral("mediaCard_") + kMovieId)) != nullptr);
        const auto menuVisible = [&ui] {
            QObject *menu = ui.popup(QStringLiteral("cardMenu"));
            return menu && menu->property("visible").toBool();
        };
        // The visible Play On entry of a menu opened on the card, null when it does not show
        const auto openMenu = [&]() -> QQuickItem * {
            const QPointF center = card->mapToScene(QPointF(card->width() / 2, card->height() / 3));
            QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier, center.toPoint());
            QQuickItem *entry = nullptr;
            const bool shown = QTest::qWaitFor([&] {
                entry = menuVisible() ? ui.item(QStringLiteral("cardMenuPlayOn")) : nullptr;
                return entry && entry->isVisible();
            });
            return shown ? entry : nullptr;
        };
        QQuickItem *entry = openMenu();
        QVERIFY(entry);
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Play On…"));
        QVERIFY(entry->isEnabled());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menuVisible());
        QVERIFY(syncPlayJoined(ui));
        QTRY_VERIFY(root->property("groupRouting").toBool());
        entry = openMenu();
        QVERIFY(entry);
        QTRY_VERIFY(!entry->isEnabled());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menuVisible());
        QVERIFY(stub.calls.isEmpty());
    }

    void theOsdOffersPlayOnForAnythingButALocalFile()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = Ui::itemIn(page, QStringLiteral("osdPlayOnButton"))) != nullptr);
        QTRY_VERIFY(button->isVisible());
        QCOMPARE(button->property("text").toString(), QStringLiteral("Play On…"));
        // Before Picture in Picture
        QQuickItem *pictureInPicture =
            Ui::itemIn(page, QStringLiteral("playerPictureInPictureButton"));
        QVERIFY(pictureInPicture);
        QTRY_VERIFY(sceneRect(button).right() <= sceneRect(pictureInPicture).left() + 1);
        // A downloaded file plays here only
        stub.localPlayback = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!button->isVisible());
        stub.localPlayback = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(button->isVisible());
        // A device without a volume has no speaker button; one with a volume keeps it
        QQuickItem *mute = Ui::itemIn(page, QStringLiteral("playerMuteButton"));
        QVERIFY(mute);
        QVERIFY(mute->isVisible());
        stub.casting = true;
        stub.castTargetName = QStringLiteral("Living room");
        Q_EMIT stub.changed();
        QTRY_VERIFY(!mute->isVisible());
        QVERIFY(button->isVisible());
        stub.castHasVolume = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(mute->isVisible());
        // What the bar disables the context menu disables too
        QObject *clipMenu = ui.popup(QStringLiteral("playerContextMoreClipMenu"));
        QObject *subtitlesMenu = ui.popup(QStringLiteral("playerContextSubtitlesMenu"));
        QVERIFY(clipMenu && subtitlesMenu);
        QVERIFY(!clipMenu->property("enabled").toBool());
        QVERIFY(!subtitlesMenu->property("enabled").toBool());
        stub.casting = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(clipMenu->property("enabled").toBool());
        QTRY_VERIFY(subtitlesMenu->property("enabled").toBool());
        // In a SyncPlay group the group decides what plays: the button stays, disabled
        QVERIFY(syncPlayJoined(ui));
        QTRY_VERIFY(!button->isEnabled());
        QVERIFY(button->isVisible());
        // Left, it is the user's again, and a click opens the dialog
        ui.syncPlay.leaveGroup();
        QTRY_VERIFY(button->isEnabled());
        ui.clickCenter(button);
        QObject *dialog = ui.popup(QStringLiteral("castDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
    }

    void theErrorOverlayOffersContinueHereAfterACastFailure()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *button = Ui::itemIn(page, QStringLiteral("playerContinueHereButton"));
        QVERIFY(button);
        QVERIFY(!button->isVisible());
        stub.active = false;
        stub.errorText = QStringLiteral("The device stopped responding.");
        stub.canContinueHere = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(button->isVisible() && button->width() > 0);
        QCOMPARE(button->property("text").toString(), QStringLiteral("Continue Here"));
        QVERIFY(button->property("highlighted").toBool());
        stub.calls.clear();
        // The row lays its buttons out a frame after they show
        QQuickItem *close = Ui::itemIn(page, QStringLiteral("playerCloseAfterErrorButton"));
        QVERIFY(close);
        QTRY_VERIFY(close->isVisible() && !sceneRect(button).intersects(sceneRect(close)));
        ui.clickCenter(button);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("castTo local")));
        stub.canContinueHere = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!button->isVisible());
    }

    void theCastDialogListsThisComputerFirstAndShowsNamesLiterally()
    {
        PlayerStub stub;
        CastStub cast;
        cast.model.setTargets(
            {{QStringLiteral("uuid:tv"), QStringLiteral("<b>Living</b> room"),
              QStringLiteral("<i>Acme</i> TV")},
             {QStringLiteral("uuid:bar"), QStringLiteral("Bar & <u>Grill</u>"), QString()}});
        stub.cast = &cast;
        stub.casting = true;
        stub.castTargetId = QStringLiteral("uuid:bar");
        stub.castTargetName = QStringLiteral("Bar & <u>Grill</u>");
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        QVERIFY(QMetaObject::invokeMethod(root, "castCurrent"));
        QObject *dialog = ui.popup(QStringLiteral("castDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_COMPARE(cast.searches, 1);
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("castTargets"))) != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), 2);
        // This Computer stays above the devices while one plays: it is the way back
        QQuickItem *thisComputer = ui.item(QStringLiteral("castThisComputer"));
        QVERIFY(thisComputer && thisComputer->isVisible());
        QCOMPARE(thisComputer->property("text").toString(), QStringLiteral("This Computer"));
        QVERIFY(!thisComputer->property("current").toBool());
        // The devices, their names as the markup characters they are
        QTRY_COMPARE(castDeviceRows(list).size(), 2);
        const QList<QQuickItem *> rows = castDeviceRows(list);
        QVERIFY(sceneRect(thisComputer).bottom() <= sceneRect(rows.at(0)).top() + 1);
        QCOMPARE(rows.at(0)->property("text").toString(), QStringLiteral("<b>Living</b> room"));
        QCOMPARE(rows.at(1)->property("text").toString(), QStringLiteral("Bar & <u>Grill</u>"));
        int found = 0;
        QVERIFY(ui.notPlainText(QStringLiteral("<b>Living</b>"), &found).isEmpty());
        QVERIFY(found >= 1);
        found = 0;
        QVERIFY(ui.notPlainText(QStringLiteral("<i>Acme</i> TV"), &found).isEmpty());
        QVERIFY(found >= 1);
        // The one that plays now is marked, and the keys start on it
        QVERIFY(!rows.at(0)->property("current").toBool());
        QVERIFY(rows.at(1)->property("current").toBool());
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        stub.castTargetId.clear();
        Q_EMIT stub.changed();
        QTRY_VERIFY(thisComputer->property("current").toBool());
        QVERIFY(!rows.at(1)->property("current").toBool());
        // The error of the search shows in a box
        QQuickItem *error = ui.item(QStringLiteral("castError"));
        QVERIFY(error);
        QVERIFY(!error->isVisible());
        cast.error = QStringLiteral("The network search failed");
        Q_EMIT cast.changed();
        QTRY_VERIFY(error->isVisible());
        // Without an item (the OSD) a pick moves the playing video
        stub.calls.clear();
        QVERIFY(QMetaObject::invokeMethod(rows.at(0), "clicked"));
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(stub.calls, QStringList{QStringLiteral("castTo uuid:tv")});
        QTRY_VERIFY(cast.stops >= 1);
    }

    void theCastDialogOpensOnTheSearchAndKeepsItsSize()
    {
        PlayerStub stub;
        CastStub cast;
        stub.cast = &cast;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        // Before it opens the dialog is outside the window's items: its parts are found under it
        QObject *dialog = ui.popup(QStringLiteral("castDialog"));
        QVERIFY(dialog);
        auto *searching = dialog->findChild<QQuickItem *>(QStringLiteral("castSearching"));
        auto *noDevices = dialog->findChild<QQuickItem *>(QStringLiteral("castNoDevices"));
        QVERIFY(searching && noDevices);
        // The empty state never shows before a search ended, not even for a frame of the opening
        int emptyShown = 0;
        NotifyHook hook(noDevices, "visible", [&] {
            if (noDevices->isVisible() && cast.searching)
                ++emptyShown;
        });
        QVERIFY(QMetaObject::invokeMethod(root, "castCurrent"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(cast.searches, 1);
        QVERIFY(searching->isVisible());
        QVERIFY(!noDevices->isVisible());
        QCOMPARE(emptyShown, 0);
        // The search shows in the room under This Computer, not over it
        QQuickItem *thisComputer = ui.item(QStringLiteral("castThisComputer"));
        QVERIFY(thisComputer);
        QVERIFY(sceneRect(searching).top() >= sceneRect(thisComputer).bottom());
        auto *content = dialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const QSizeF size(content->width(), content->height());
        // The search ends with nothing: the same room shows the empty state, and the window keeps its size
        cast.searching = false;
        Q_EMIT cast.changed();
        QTRY_VERIFY(noDevices->isVisible());
        QVERIFY(!searching->isVisible());
        QVERIFY(sceneRect(noDevices).top() >= sceneRect(thisComputer).bottom());
        QCOMPARE(QSizeF(content->width(), content->height()), size);
        // Search Again goes back to the search in place
        QObject *again = nullptr;
        QQmlListReference footer(dialog, "customFooterActions");
        for (qsizetype i = 0; i < footer.count(); ++i) {
            if (footer.at(i)->objectName() == QStringLiteral("castSearchAgain"))
                again = footer.at(i);
        }
        QVERIFY(again);
        QVERIFY(again->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(again, "trigger"));
        QCOMPARE(cast.searches, 2);
        QTRY_VERIFY(searching->isVisible());
        QVERIFY(!noDevices->isVisible());
        QVERIFY(!again->property("enabled").toBool());
        QCOMPARE(QSizeF(content->width(), content->height()), size);
        // A search that cannot start shows its error, not a search that never ends
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->property("opened").toBool());
        cast.searchStarts = false;
        cast.error = QStringLiteral("Could not search the network");
        QVERIFY(QMetaObject::invokeMethod(root, "castCurrent"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("castError"))->isVisible());
        QVERIFY(!searching->isVisible());
        QVERIFY(!noDevices->isVisible());
    }

    void theCastDialogPicksWithTheKeys()
    {
        PlayerStub stub;
        CastStub cast;
        cast.model.setTargets({{QStringLiteral("uuid:tv"), QStringLiteral("TV"), QString()},
                               {QStringLiteral("uuid:kodi"), QStringLiteral("Kodi"), QString()}});
        stub.cast = &cast;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        QObject *dialog = ui.popup(QStringLiteral("castDialog"));
        QVERIFY(dialog);
        const auto open = [&] {
            QVERIFY(QMetaObject::invokeMethod(root, "castCurrent"));
            QTRY_VERIFY(dialog->property("opened").toBool());
        };
        open();
        QQuickItem *list = ui.item(QStringLiteral("castTargets"));
        QQuickItem *thisComputer = ui.item(QStringLiteral("castThisComputer"));
        QVERIFY(list && thisComputer);
        QTRY_VERIFY(list->hasActiveFocus());
        // Nothing plays elsewhere: the keys start on This Computer, shown as the current row
        QCOMPARE(list->property("currentIndex").toInt(), -1);
        QVERIFY(thisComputer->property("highlighted").toBool());
        // Down reaches the devices, Up from the first goes back to This Computer
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 0);
        QVERIFY(!thisComputer->property("highlighted").toBool());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_COMPARE(list->property("currentIndex").toInt(), -1);
        QTRY_VERIFY(thisComputer->property("highlighted").toBool());
        // Return on This Computer picks it
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(stub.calls, QStringList{QStringLiteral("castTo local")});
        // Space on a device picks the device
        open();
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_COMPARE(list->property("currentIndex").toInt(), 1);
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_Space);
        QTRY_VERIFY(!dialog->property("opened").toBool());
        QCOMPARE(stub.calls, QStringList{QStringLiteral("castTo uuid:kodi")});
    }

    void playOnWithAnItemPlaysItThereAndNotesARefusal()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        const auto chooseThisComputer = [&] {
            QObject *dialog = ui.popup(QStringLiteral("castDialog"));
            QVERIFY(dialog);
            QTRY_VERIFY(dialog->property("opened").toBool());
            QQuickItem *row = nullptr;
            QTRY_VERIFY((row = ui.item(QStringLiteral("castThisComputer"))) != nullptr
                        && row->isVisible());
            QVERIFY(QMetaObject::invokeMethod(row, "clicked"));
            QTRY_VERIFY(!dialog->property("opened").toBool());
        };
        // With an item, This Computer plays that item here
        QVERIFY(QMetaObject::invokeMethod(root, "playOn", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, 12345)));
        chooseThisComputer();
        QVERIFY(stub.calls.contains(QStringLiteral("playItemOn local %1 12345").arg(kMovieId)));
        QTRY_VERIFY(ui.item(QStringLiteral("playerPage")) != nullptr);
        // A refusal says so
        stub.castRefused = true;
        QVERIFY(QMetaObject::invokeMethod(root, "castCurrent"));
        chooseThisComputer();
        QVERIFY(stub.calls.contains(QStringLiteral("castTo local")));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not start playback"), true));
        // A device that stops on its own: named in a notification, and its markup is not read
        Q_EMIT stub.castStoppedElsewhere(QStringLiteral("<b>TV</b>"));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Playback was stopped on ‹b>TV‹/b>"), true));
    }

    void theMiniPlayerSaysWhereACastPlays()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        QQuickItem *view = Ui::itemIn(mini, QStringLiteral("miniPlayerCastView"));
        QVERIFY(view);
        QVERIFY(!view->isVisible());
        stub.casting = true;
        stub.castTargetName = QStringLiteral("<b>Living</b> room");
        Q_EMIT stub.changed();
        QTRY_VERIFY(view->isVisible());
        int found = 0;
        QVERIFY(ui.notPlainText(QStringLiteral("Playing on <b>Living</b> room"), &found).isEmpty());
        QCOMPARE(found, 1);
    }

    void upFromTheGridFirstRowReachesTheControlAbove()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_VERIFY(grid->hasActiveFocus());
        QCOMPARE(grid->property("currentIndex").toInt(), 0);
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        QVERIFY(input.dispatchKeycode(QStringLiteral("up")));
        QQuickItem *now = ui.window->activeFocusItem();
        QVERIFY(now && now != grid);
        const QRectF rect = now->mapRectToScene(QRectF(0, 0, now->width(), now->height()));
        QVERIFY2(rect.center().y() < grid->mapToScene(QPointF(0, 0)).y(),
                 qPrintable(namesOf({now}).join(QLatin1Char(' '))));
        // Down returns to the grid, with its row
        QVERIFY(input.dispatchKeycode(QStringLiteral("down")));
        QTRY_VERIFY(grid->hasActiveFocus());
        QCOMPARE(grid->property("currentIndex").toInt(), 0);
    }

    void padReachesEveryLibraryControl()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 30;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const QList<QQuickItem *> reached = padReach(ui, input);
        const QStringList names = namesOf(reached);
        QVERIFY2(reached.contains(grid), qPrintable(names.join(QLatin1Char(' '))));
        // The page's named controls that Tab reaches: the arrows reach them too
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        QVERIFY(page);
        for (QQuickItem *item : allItems(page)) {
            const QString name = item->objectName();
            if (!item->isVisible() || !item->isEnabled() || !item->activeFocusOnTab()
                || !(name == QLatin1String("librarySelector")
                     || name == QLatin1String("librarySearchField")
                     || name == QLatin1String("clearFiltersButton")
                     || name == QLatin1String("returnToTopButton")
                     || name.startsWith(QLatin1String("filterChipRemove_"))
                     || name.startsWith(QLatin1String("subtab_"))))
                continue;
            QVERIFY2(reached.contains(item),
                     qPrintable(name + QLatin1String(" not reached, reached: ")
                                + names.join(QLatin1Char(' '))));
        }
    }

    void padReachesEverySeasonPageControl()
    {
        Ui ui(m_home->path());
        ui.serveShow();
        ui.server.generatedEpisodeTotal = 12;
        QVERIFY(ui.load());
        ui.window->setProperty("requestedItemId", kSeasonId);
        QVERIFY(ui.signIn());
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("episodesList"))) != nullptr);
        QTRY_VERIFY(list->property("count").toInt() > 1);
        QQuickItem *page = ui.item(QStringLiteral("itemDetailsPage"));
        QVERIFY(page);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        list->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(list->hasActiveFocus());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const QList<QQuickItem *> reached = padReach(ui, input);
        const QStringList names = namesOf(reached);
        // The page's own controls: the links, Play and Show More, whichever of them this window shows
        for (const char *name :
             {"seriesLink", "seasonLink", "detailsPlay", "detailsPlayNarrow", "overviewToggle"}) {
            QQuickItem *control = Ui::itemIn(page, QString::fromLatin1(name));
            if (!control || !control->isVisible() || !control->isEnabled()
                || !control->activeFocusOnTab())
                continue;
            QVERIFY2(reached.contains(control),
                     qPrintable(QString::fromLatin1(name) + QLatin1String(" not reached, reached: ")
                                + names.join(QLatin1Char(' '))));
        }
        QVERIFY(reached.contains(list));
        // The walk puts the list's row back where it was
        QCOMPARE(list->property("currentIndex").toInt(), 0);
    }

    void padReachesEverySettingsControl()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settingsSection")));
        QQuickItem *page = ui.item(QStringLiteral("settingsPage"));
        QVERIFY(page);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsGroupCard_1")) != nullptr);
        // The values before: every combo box, spin box and switch of the content
        QQuickItem *view = ui.item(QStringLiteral("settingsContentView"));
        QVERIFY(view);
        // The page slides in disabled and unsized: its controls take the focus once it settled
        QTRY_VERIFY(view->isEnabled() && view->width() > 0);
        const auto snapshot = [&] {
            QStringList values;
            for (QQuickItem *item : allItems(view)) {
                if (item->inherits("QQuickComboBox") || item->inherits("QQuickSpinBox"))
                    values << item->property("currentIndex").toString() + QLatin1Char('/')
                                  + item->property("value").toString();
                else if (QString::fromLatin1(item->metaObject()->className())
                             .contains(QLatin1String("FormSwitchDelegate")))
                    values << item->property("checked").toString();
            }
            return values;
        };
        const QStringList before = snapshot();
        QVERIFY(!before.isEmpty());
        QQuickItem *first = nullptr;
        for (QQuickItem *candidate : allItems(view)) {
            if (candidate->inherits("QQuickComboBox") && candidate->isVisible()
                && (!first
                    || candidate->mapToScene(QPointF(0, 0)).y()
                           < first->mapToScene(QPointF(0, 0)).y()))
                first = candidate;
        }
        QVERIFY(first);
        first->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasActiveFocus());
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const QList<QQuickItem *> reached = padReach(ui, input);
        const QStringList names = namesOf(reached);
        // Every combo box of the section is reached, and no value changed on the way
        for (QQuickItem *item : allItems(view)) {
            if (item->inherits("QQuickComboBox") && item->isVisible() && item->isEnabled()
                && item->activeFocusOnTab())
                QVERIFY2(reached.contains(item), qPrintable(names.join(QLatin1Char(' '))));
        }
        QCOMPARE(snapshot(), before);
    }

    void padStaysInsideADialog()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("signOutAction")), "trigger"));
        QObject *dialog = ui.popup(QStringLiteral("signOutDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(ui.window->activeFocusItem() && inPopupItem(ui.window->activeFocusItem()));
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const QList<QQuickItem *> reached = padReach(ui, input);
        for (const QQuickItem *item : reached)
            QVERIFY2(inPopupItem(item),
                     qPrintable(namesOf({const_cast<QQuickItem *>(item)}).join(QLatin1Char(' '))));
        // The page behind stayed where it was
        QVERIFY(dialog->property("opened").toBool());
    }

    void noPadFocusOnAHiddenPage()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const QPointer<QQuickItem> home = ui.item(QStringLiteral("homePage"));
        QVERIFY(home);
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        // The Home page stays in the stack under the details page
        QVERIFY(home);
        QTRY_VERIFY(ui.window->activeFocusItem() != nullptr);
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        const QList<QQuickItem *> reached = padReach(ui, input);
        QVERIFY(reached.size() > 1);
        for (QQuickItem *item : reached)
            QVERIFY2(item != home && !home->isAncestorOf(item),
                     qPrintable(namesOf({item}).join(QLatin1Char(' '))));
    }

    void padInThePlayer()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        // The page keeps the arrows for the seeks, and the focus
        for (const auto &[keycode, call] :
             {std::pair{"right", "seekRelative 10"}, std::pair{"left", "seekRelative -10"},
              std::pair{"up", "seekRelative 60"}, std::pair{"down", "seekRelative -60"}}) {
            stub.calls.clear();
            QVERIFY(input.dispatchKeycode(QString::fromLatin1(keycode)));
            QTRY_VERIFY2(stub.calls.contains(QString::fromLatin1(call)), keycode);
            QCOMPARE(ui.window->activeFocusItem(), page);
        }
        // Select plays or pauses: Return is the Up Next card's only
        stub.calls.clear();
        QVERIFY(input.dispatchKeycode(QStringLiteral("select")));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("togglePause")));
        QCOMPARE(ui.window->activeFocusItem(), page);
        // Menu opens the context menu
        QObject *menu = ui.popup(QStringLiteral("playerContextMenu"));
        QVERIFY(menu);
        QVERIFY(input.dispatchKeycode(QStringLiteral("menu")));
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(Ui::closeMenu(menu));
    }

    // The buttons the page shows over the video are Tab-only: the context menu, which Menu and a pad's West button open, lists them while they show
    void playerContextMenuListsTheButtonsOverTheVideo()
    {
        PlayerStub stub;
        stub.skipLabel = QStringLiteral("Skip Intro");
        stub.clipSaving = true;
        stub.errorText = QStringLiteral("The video could not be opened");
        stub.canRetry = true;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *menu = ui.popup(QStringLiteral("playerContextMenu"));
        QVERIFY(menu);
        Kinefin::RemoteInputServer input(QString(), QStringLiteral("test"));
        input.setWindow(ui.window);
        QVERIFY(input.dispatchKeycode(QStringLiteral("menu")));
        QTRY_VERIFY(menu->property("opened").toBool());
        const auto entry = [menu](const char *name) {
            return menu->findChild<QQuickItem *>(QString::fromLatin1(name));
        };
        for (const char *name : {"playerContextSkipItem", "playerContextRetryItem",
                                 "playerContextCloseItem", "playerContextCancelClipItem"}) {
            QVERIFY2(entry(name), name);
            QVERIFY2(entry(name)->isVisible(), name);
        }
        // Not in a SyncPlay group: no entry for it
        QVERIFY(entry("playerContextSyncPlayItem"));
        QVERIFY(!entry("playerContextSyncPlayItem")->isVisible());
        QCOMPARE(entry("playerContextSkipItem")->property("text").toString(),
                 QStringLiteral("Skip Intro"));
        QCOMPARE(entry("playerContextRetryItem")->property("text").toString(),
                 QStringLiteral("Try Again"));
        QCOMPARE(entry("playerContextCloseItem")->property("text").toString(),
                 QStringLiteral("Close Player"));
        QCOMPARE(entry("playerContextCancelClipItem")->property("text").toString(),
                 QStringLiteral("Cancel Saving the Clip"));
        stub.clipSavingGif = true;
        Q_EMIT stub.changed();
        QTRY_COMPARE(entry("playerContextCancelClipItem")->property("text").toString(),
                     QStringLiteral("Cancel Saving the GIF"));
        // Each does what its button does
        for (const auto &[name, call] : {std::pair{"playerContextSkipItem", "skipSegment"},
                                         std::pair{"playerContextRetryItem", "retryPlayback"},
                                         std::pair{"playerContextCancelClipItem", "cancelClip"}}) {
            QVERIFY(QMetaObject::invokeMethod(menu, "open"));
            QTRY_VERIFY(menu->property("opened").toBool());
            QVERIFY(QMetaObject::invokeMethod(entry(name), "click"));
            QTRY_VERIFY2(stub.calls.contains(QString::fromLatin1(call)), call);
            QTRY_VERIFY(!menu->property("visible").toBool());
        }
        // Nothing to show, no entry
        stub.skipLabel.clear();
        stub.errorText.clear();
        stub.clipSaving = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!entry("playerContextSkipItem")->isVisible());
        QTRY_VERIFY(!entry("playerContextRetryItem")->isVisible());
        QTRY_VERIFY(!entry("playerContextCloseItem")->isVisible());
        QTRY_VERIFY(!entry("playerContextCancelClipItem")->isVisible());
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
        // No title bar, as on Home: the actions are a toolbar on top of the list
        QCOMPARE(ui.currentDetails()->property("globalToolBarStyle"),
                 ui.item(QStringLiteral("homePage"))->property("globalToolBarStyle"));
        QTRY_VERIFY(ui.item(QStringLiteral("queueToolbar"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("queueHeading"))->isVisible());
        QQuickItem *fade = ui.item(QStringLiteral("queueTopFade"));
        QVERIFY(fade);
        QCOMPARE(fade->parentItem(), ui.item(QStringLiteral("queueList")));
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

    void theDetailsPageCanMarkAMovieWatchedAndUnwatched()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("details")));
        QQmlListReference actions(ui.item(QStringLiteral("itemDetailsPage")), "actions");
        const auto action = [&actions](const QString &name) -> QObject * {
            for (qsizetype i = 0; i < actions.count(); ++i) {
                if (actions.at(i)->objectName() == name)
                    return actions.at(i);
            }
            return nullptr;
        };
        QObject *mark = action(QStringLiteral("togglePlayedAction"));
        QVERIFY(mark);
        QTRY_COMPARE(mark->property("text").toString(), QStringLiteral("Mark as Watched"));
        QVERIFY(mark->property("visible").toBool());
        QVERIFY(mark->property("enabled").toBool());
        QMetaObject::invokeMethod(mark, "trigger");
        QTRY_COMPARE(ui.server.playedWrites.size(), 1);
        QTRY_COMPARE(mark->property("text").toString(), QStringLiteral("Mark as Unwatched"));
        QTRY_VERIFY(mark->property("enabled").toBool());
        QMetaObject::invokeMethod(mark, "trigger");
        QTRY_COMPARE(ui.server.playedWrites.size(), 2);
        QTRY_COMPARE(mark->property("text").toString(), QStringLiteral("Mark as Watched"));
    }

    void theDetailsPageAddsItsItemToTheQueue()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("details")));
        // The hero shows these actions inline: the page keeps them out of its toolbar
        QQuickItem *page = ui.item(QStringLiteral("itemDetailsPage"));
        const auto action = [page](const QString &text) {
            const QString name = text == QStringLiteral("Add to Queue")
                                     ? QStringLiteral("addToQueueAction")
                                     : QStringLiteral("playNextAction");
            QObject *found = page->findChild<QObject *>(name);
            return found && found->property("text").toString() == text ? found : nullptr;
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
        QObject *playNext = nullptr;
        QTRY_VERIFY([&] {
            playNext = ui.item(QStringLiteral("itemDetailsPage"))
                           ->findChild<QObject *>(QStringLiteral("playNextAction"));
            return playNext && playNext->property("visible").toBool();
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
        // A hidden overlay is not built at all: a grid fills with dozens of cards at once
        for (const QString &overlay :
             {QStringLiteral("cardRating"), QStringLiteral("cardRemaining"),
              QStringLiteral("cardProgress"), QStringLiteral("cardNew"),
              QStringLiteral("cardFavorite"), QStringLiteral("cardUnplayed"),
              QStringLiteral("cardQuickPlay")})
            QVERIFY2(!Ui::itemIn(played, overlay), qPrintable(overlay));
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
            (void)qWaitForScaled([&] { return ui.item(name) != nullptr; }, 5000);
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

    void cardMenuFavoriteTogglesTheFlagOfTheItem()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        const QString favoredId = QStringLiteral("a1a2a3a4b5b6c7c8d9d0e1e2f3f4a5a6");
        const QString folderId = QStringLiteral("b1b2b3b4c5c6d7d8e9e0f1f2a3a4b5b6");
        const auto withFlag = [](const QString &id, const QString &type, bool favorite) {
            QJsonObject item = fixtureObject(QStringLiteral("movie_item"));
            item.insert(QStringLiteral("Id"), id);
            item.insert(QStringLiteral("Type"), type);
            QJsonObject userData = item.value(QStringLiteral("UserData")).toObject();
            userData.insert(QStringLiteral("IsFavorite"), favorite);
            item.insert(QStringLiteral("UserData"), userData);
            return item;
        };
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{withFlag(kMovieId, QStringLiteral("Movie"), false),
                                withFlag(favoredId, QStringLiteral("Movie"), true),
                                withFlag(folderId, QStringLiteral("Folder"), false)}));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        const auto card = [page](const QString &id) {
            return Ui::itemIn(page, QStringLiteral("mediaCard_") + id);
        };
        QTRY_VERIFY(card(kMovieId) && card(favoredId) && card(folderId));
        const auto menuVisible = [&ui] {
            QObject *menu = ui.popup(QStringLiteral("cardMenu"));
            return menu && menu->property("visible").toBool();
        };
        const auto openMenuOf = [&](const QString &id) {
            QQuickItem *target = card(id);
            const QPointF center =
                target->mapToScene(QPointF(target->width() / 2, target->height() / 3));
            QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier, center.toPoint());
            QTRY_VERIFY(menuVisible());
        };
        // Null when the entry never appears: each caller verifies it
        const auto favoriteEntry = [&ui] {
            QQuickItem *entry = nullptr;
            qWaitForScaled(
                [&] { return (entry = ui.item(QStringLiteral("cardMenuFavorite"))) != nullptr; });
            return entry;
        };
        const auto closeMenu = [&] {
            QTest::keyClick(ui.window, Qt::Key_Escape);
            QTRY_VERIFY(!menuVisible());
        };

        // An unfavored movie offers Add; choosing it sends a POST, and the card and the next menu follow the answer
        openMenuOf(kMovieId);
        QQuickItem *entry = favoriteEntry();
        QVERIFY(entry);
        QTRY_VERIFY(entry->isVisible());
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Add to Favorites"));
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(!menuVisible());
        QTRY_COMPARE(ui.server.favoriteWrites.size(), 1);
        QCOMPARE(ui.server.favoriteWrites.constLast().method, QByteArrayLiteral("POST"));
        QCOMPARE(ui.server.favoriteWrites.constLast().route,
                 QByteArrayLiteral("/jellyfin/UserFavoriteItems/") + kMovieId.toLatin1());
        QTRY_VERIFY(card(kMovieId)->property("favorite").toBool());
        openMenuOf(kMovieId);
        entry = favoriteEntry();
        QVERIFY(entry);
        QTRY_COMPARE(entry->property("text").toString(), QStringLiteral("Remove from Favorites"));

        // Remove sends a DELETE and flips the card back
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(!menuVisible());
        QTRY_COMPARE(ui.server.favoriteWrites.size(), 2);
        QCOMPARE(ui.server.favoriteWrites.constLast().method, QByteArrayLiteral("DELETE"));
        QTRY_VERIFY(!card(kMovieId)->property("favorite").toBool());

        // A favored movie starts with Remove
        openMenuOf(favoredId);
        entry = favoriteEntry();
        QVERIFY(entry);
        QTRY_COMPARE(entry->property("text").toString(), QStringLiteral("Remove from Favorites"));
        closeMenu();

        // A kind that cannot be a favorite has no entry
        openMenuOf(folderId);
        QQuickItem *open = nullptr;
        QTRY_VERIFY((open = ui.item(QStringLiteral("cardMenuOpen"))) != nullptr
                    && open->isVisible());
        entry = ui.item(QStringLiteral("cardMenuFavorite"));
        QVERIFY(!entry || !entry->isVisible());
        closeMenu();

        // A write the server refuses leaves the card as it was and is reported
        ui.server.favoriteWriteStatus = 500;
        QSignalSpy failed(&ui.controller, &ConnectionController::favoriteUpdateFailed);
        openMenuOf(kMovieId);
        entry = favoriteEntry();
        QVERIFY(entry);
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!card(kMovieId)->property("favorite").toBool());
    }

    void cardMenuMarksTheItemWatchedOrUnwatched()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        const QString watchedId = QStringLiteral("a1a2a3a4b5b6c7c8d9d0e1e2f3f4a5a6");
        const QString seriesId = QStringLiteral("b1b2b3b4c5c6d7d8e9e0f1f2a3a4b5b6");
        const auto withState = [](const QString &id, const QString &type, bool played) {
            QJsonObject item = fixtureObject(QStringLiteral("movie_item"));
            item.insert(QStringLiteral("Id"), id);
            item.insert(QStringLiteral("Type"), type);
            QJsonObject userData = item.value(QStringLiteral("UserData")).toObject();
            userData.insert(QStringLiteral("Played"), played);
            item.insert(QStringLiteral("UserData"), userData);
            return item;
        };
        ui.server.listingRawBody.insert(
            QStringLiteral("Items"),
            envelope(QJsonArray{withState(kMovieId, QStringLiteral("Movie"), false),
                                withState(watchedId, QStringLiteral("Movie"), true),
                                withState(seriesId, QStringLiteral("Series"), false)}));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *page = ui.item(QStringLiteral("libraryPage"));
        const auto card = [page](const QString &id) {
            return Ui::itemIn(page, QStringLiteral("mediaCard_") + id);
        };
        QTRY_VERIFY(card(kMovieId) && card(watchedId) && card(seriesId));
        const auto menuVisible = [&ui] {
            QObject *menu = ui.popup(QStringLiteral("cardMenu"));
            return menu && menu->property("visible").toBool();
        };
        // The entry of a menu opened on the card, null when it does not show
        const auto openEntryOf = [&](const QString &id) -> QQuickItem * {
            QQuickItem *target = card(id);
            const QPointF center =
                target->mapToScene(QPointF(target->width() / 2, target->height() / 3));
            QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier, center.toPoint());
            QQuickItem *entry = nullptr;
            const bool shown = qWaitForScaled([&] {
                entry = menuVisible() ? ui.item(QStringLiteral("cardMenuPlayed")) : nullptr;
                return entry && entry->isVisible();
            });
            return shown ? entry : nullptr;
        };
        const auto closeMenu = [&] {
            QTest::keyClick(ui.window, Qt::Key_Escape);
            QTRY_VERIFY(!menuVisible());
        };

        // An unwatched movie offers Mark as Watched: a POST, and the card and the next menu follow the answer
        QQuickItem *entry = openEntryOf(kMovieId);
        QVERIFY(entry);
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Mark as Watched"));
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(!menuVisible());
        QTRY_COMPARE(ui.server.playedWrites.size(), 1);
        QCOMPARE(ui.server.playedWrites.constLast().method, QByteArrayLiteral("POST"));
        QVERIFY(ui.server.playedWrites.constLast().route.contains(kMovieId.toLatin1()));
        QTRY_VERIFY(card(kMovieId)->property("played").toBool());
        entry = openEntryOf(kMovieId);
        QVERIFY(entry);
        QTRY_COMPARE(entry->property("text").toString(), QStringLiteral("Mark as Unwatched"));
        // Unwatched sends a DELETE and flips the card back
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(!menuVisible());
        QTRY_COMPARE(ui.server.playedWrites.size(), 2);
        QCOMPARE(ui.server.playedWrites.constLast().method, QByteArrayLiteral("DELETE"));
        QTRY_VERIFY(!card(kMovieId)->property("played").toBool());

        // A watched movie starts with Unwatched
        entry = openEntryOf(watchedId);
        QVERIFY(entry);
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Mark as Unwatched"));
        closeMenu();

        // A series has the state of its episodes: no entry
        QVERIFY(!openEntryOf(seriesId));
        closeMenu();

        // A write the server refuses leaves the card as it was and is reported
        ui.server.playedWriteStatus = 500;
        QSignalSpy failed(&ui.controller, &ConnectionController::playedUpdateFailed);
        entry = openEntryOf(kMovieId);
        QVERIFY(entry);
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_COMPARE(failed.size(), 1);
        QVERIFY(!card(kMovieId)->property("played").toBool());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Could not change the watched state"), false));
    }

    void continueWatchingCardMarksTheItemWatched()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = homeRowMovieCard(ui, 0)) != nullptr);
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = Ui::itemIn(card, QStringLiteral("cardMarkWatched"))) != nullptr);
        QVERIFY(!button->isVisible());
        // The card's menu button has its three dots in its middle, on whole pixels
        QQuickItem *menuButton = Ui::itemIn(card, QStringLiteral("cardMenuButton"));
        QQuickItem *dots = Ui::itemIn(card, QStringLiteral("cardMenuGlyph"));
        QVERIFY(menuButton && dots);
        const QPointF dotsCenter =
            dots->mapToItem(menuButton, QPointF(dots->width() / 2, dots->height() / 2));
        QVERIFY2(qAbs(dotsCenter.x() - menuButton->width() / 2) <= 0.5
                     && qAbs(dotsCenter.y() - menuButton->height() / 2) <= 0.5,
                 qPrintable(QStringLiteral("%1,%2 in %3x%4")
                                .arg(dotsCenter.x())
                                .arg(dotsCenter.y())
                                .arg(menuButton->width())
                                .arg(menuButton->height())));
        QCOMPARE(dots->x(), std::round(dots->x()));
        // The cards of the other rows have no such button
        if (QQuickItem *nextUp = homeRowMovieCard(ui, 1))
            QVERIFY(!Ui::itemIn(nextUp, QStringLiteral("cardMarkWatched")));
        QVERIFY(revealCard(ui, card));
        // With the Play button, the mouse over the card shows it, in the poster's top corner
        QTest::mouseMove(
            ui.window,
            card->mapToScene(QPointF(card->width() * 0.15, card->height() * 0.1)).toPoint());
        QTRY_VERIFY(button->isVisible());
        QTRY_COMPARE(button->opacity(), 1.0);
        const QRectF area = button->mapRectToItem(card, QRectF(QPointF(0, 0), button->size()));
        QVERIFY(card->boundingRect().contains(area));
        QVERIFY(area.center().x() > card->width() / 2 && area.center().y() < card->height() / 3);
        QCOMPARE(button->property("text").toString(), QStringLiteral("Mark as Watched"));
        // A click marks the item watched: a POST for it, and Home asks for its rows again
        const qint64 resumeTicks = card->property("resumeTicks").toLongLong();
        QVERIFY(resumeTicks > 0);
        const int rowsBefore = ui.server.listingRoutes.value(QStringLiteral("Resume")).size();
        ui.clickCenter(button);
        QTRY_COMPARE(ui.server.playedWrites.size(), 1);
        QCOMPARE(ui.server.playedWrites.constLast().method, QByteArrayLiteral("POST"));
        QVERIFY(ui.server.playedWrites.constLast().route.contains(kMovieId.toLatin1()));
        QTRY_VERIFY(ui.server.listingRoutes.value(QStringLiteral("Resume")).size() > rowsBefore);
        // The notification offers it back: Undo makes it unwatched again at its resume point
        QTRY_VERIFY(ui.showsText(QStringLiteral("Marked as watched")));
        QQuickItem *undo = nullptr;
        QTRY_VERIFY([&] {
            for (QQuickItem *candidate : ui.itemsWithText(QStringLiteral("Undo"), true)) {
                if (candidate->inherits("QQuickAbstractButton") && candidate->isVisible())
                    undo = candidate;
            }
            return undo != nullptr;
        }());
        const int rowsAfterMark = ui.server.listingRoutes.value(QStringLiteral("Resume")).size();
        QVERIFY(QMetaObject::invokeMethod(undo, "click"));
        QTRY_COMPARE(ui.server.userDataWrites.size(), 1);
        QCOMPARE(ui.server.userDataWrites.constFirst().body,
                 (QJsonObject{{QStringLiteral("Played"), false},
                              {QStringLiteral("PlaybackPositionTicks"), double(resumeTicks)}}));
        QTRY_VERIFY(ui.server.listingRoutes.value(QStringLiteral("Resume")).size() > rowsAfterMark);
        // The Undo itself brings no notification of its own
        QTest::qWait(200);
        QVERIFY(!ui.showsText(QStringLiteral("Could not change the watched state"), false));
    }

    void theDetailsHeroKeepsPlayAndTheToolbarTheRest()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        QTRY_VERIFY(Ui::itemIn(page, QStringLiteral("detailsPlay")) != nullptr);
        QVERIFY(!Ui::itemIn(page, QStringLiteral("detailsActions")));
        // Wide: Play ends the facts row, on its line and to the right of the facts; narrow: the whole width below
        QVERIFY(ui.opensWindowOfWidth(1400));
        QQuickItem *play = Ui::itemIn(page, QStringLiteral("detailsPlay"));
        QQuickItem *narrowPlay = Ui::itemIn(page, QStringLiteral("detailsPlayNarrow"));
        QQuickItem *facts = Ui::itemIn(page, QStringLiteral("detailsFacts"));
        QVERIFY(play && narrowPlay && facts);
        QTRY_VERIFY(play->isVisible() && !narrowPlay->isVisible());
        const auto centreY = [](QQuickItem *item) {
            return item->mapToScene(QPointF(0, item->height() / 2)).y();
        };
        QTRY_VERIFY(qAbs(centreY(play) - centreY(facts)) < facts->height() / 2 + 1);
        QVERIFY(play->mapToScene(QPointF(0, 0)).x()
                >= facts->mapToScene(QPointF(facts->width(), 0)).x() - 1);
        // At the end of the hero's text, on its margin
        QQuickItem *heroText = Ui::itemIn(page, QStringLiteral("detailsHeroText"));
        QVERIFY(heroText);
        QVERIFY(qAbs(play->mapToScene(QPointF(play->width(), 0)).x()
                     - heroText->mapToScene(QPointF(heroText->width(), 0)).x())
                < 1);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // A Tab stop like the button it replaces
        QVERIFY(play->activeFocusOnTab());
        QVERIFY(ui.opensWindowOfWidth(500));
        QTRY_VERIFY(!play->isVisible() && narrowPlay->isVisible());
        QTRY_VERIFY(narrowPlay->width() > facts->width() / 2);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        QVERIFY(narrowPlay->activeFocusOnTab());
        QStringList names;
        QStringList texts;
        QQmlListReference actions(page, "actions");
        for (qsizetype i = 0; i < actions.count(); ++i) {
            names << actions.at(i)->objectName();
            texts << actions.at(i)->property("text").toString();
        }
        QCOMPARE(
            names.mid(0, 8),
            (QStringList{
                QStringLiteral("playOnAction"), QStringLiteral("playNextAction"),
                QStringLiteral("addToQueueAction"), QStringLiteral("addToPlaylistAction"),
                QStringLiteral("downloadAction"), QStringLiteral("automaticDownloadsAction"),
                QStringLiteral("stopDownloadsAction"), QStringLiteral("toggleFavoriteAction")}));
        // Refresh is the main menu's, for every page
        QVERIFY(!texts.contains(QStringLiteral("Refresh")));
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

    void theQueueUndoesTheLastRemovalWithCtrlZ()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QQuickItem *list = openQueueWithEpisodes(ui);
        QVERIFY(list);
        auto *queue = ui.controller.queue();
        QTRY_COMPARE(queue->count(), 5);
        QTRY_VERIFY(list->hasActiveFocus());
        QObject *undo = ui.undoQueueAction();
        QVERIFY(undo);
        QVERIFY(undo->property("enabled").toBool());
        QTest::keyClick(ui.window, Qt::Key_Delete);
        QTRY_COMPARE(queue->count(), 4);
        QTest::keyClick(ui.window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(queue->count(), 5);
        QCOMPARE(queue->data(queue->index(0), Kinefin::PlayQueue::NameRole).toString(),
                 QStringLiteral("Episode 1"));
        QTRY_VERIFY(!undo->property("enabled").toBool());
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
        QObject *undo = ui.undoQueueAction();
        QVERIFY(undo);
        QVERIFY(undo->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(undo, "trigger"));
        QTRY_COMPARE(ui.controller.queue()->count(), 5);
        QTRY_VERIFY(!undo->property("enabled").toBool());
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
            const QQuickItem *root = ui.item(page);
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
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
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
        const QStringList always{QStringLiteral("Downloads"), QStringLiteral("Favorites"),
                                 QStringLiteral("Playlists")};
        QTRY_COMPARE(ui.tabNames(),
                     (QStringList{QStringLiteral("Home"), QStringLiteral("Movies")} + always));
        // Only series: the Movies tab goes and Shows takes its place
        ui.serveViews({{kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     (QStringList{QStringLiteral("Home"), QStringLiteral("Shows")} + always));
        // Neither: the bar holds Home, Favorites and Playlists, which no library type decides
        ui.serveViews({{kSeries, QStringLiteral("Music"), QStringLiteral("music")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(), (QStringList{QStringLiteral("Home")} + always));
        // A type that vanishes while its tab shows sends the user to Home
        ui.serveViews({{kMovies, QStringLiteral("Movies"), QStringLiteral("movies")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     (QStringList{QStringLiteral("Home"), QStringLiteral("Movies")} + always));
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
        QVERIFY(ui.clickTab(QStringLiteral("Shows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QCOMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(ui.currentDetails()->property("title").toString(), QStringLiteral("Shows"));
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

    void returnToTopSitsAtTheRightAndScrollsBack()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 300;
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        QVERIFY(grid);
        QTRY_COMPARE(grid->property("count").toInt(), 300);
        QObject *action = ui.item(QStringLiteral("libraryPage"))
                              ->findChild<QObject *>(QStringLiteral("returnToTopAction"));
        QQuickItem *button = ui.item(QStringLiteral("returnToTopButton"));
        QQuickItem *toolbar = ui.item(QStringLiteral("libraryToolbar"));
        QVERIFY(action && button && toolbar);
        // Right of the toolbar, outside it, and with nothing to do at the first row
        QQmlListReference actions(toolbar, "actions");
        for (qsizetype i = 0; i < actions.count(); ++i)
            QVERIFY(actions.at(i) != action);
        QTRY_VERIFY(button->isVisible() && button->width() > 0);
        QVERIFY(button->mapToScene(QPointF(0, 0)).x()
                >= toolbar->mapToScene(QPointF(toolbar->width(), 0)).x() - 0.5);
        QVERIFY(!button->isEnabled());
        grid->setProperty("currentIndex", 40);
        grid->setProperty("contentY", 6000.0);
        QTRY_VERIFY(button->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
        // An animation brings it to the very start, the top margin included, then the first card is the current one
        QTRY_VERIFY(grid->property("atYBeginning").toBool());
        QTRY_COMPARE(grid->property("contentY").toReal(), grid->property("originY").toReal());
        QTRY_COMPARE(grid->property("currentIndex").toInt(), 0);
        QTRY_VERIFY(!button->isEnabled());
        // The keys go to the grid, not away with the button that turned off
        QTRY_VERIFY(grid->hasActiveFocus());
    }

    void aTabFadesInWithItsToolbarAndGridAtOnce()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 12;
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_VERIFY(!ui.tabSwitching());
        // The wait has a cap for a page that never gets ready: a loaded machine must not reach it here
        QObject *wait = ui.window->findChild<QObject *>(QStringLiteral("tabReadyWait"));
        QVERIFY(wait);
        wait->setProperty("limit", 30000);
        // What the new tab holds the moment its motion starts: the toolbar builds its buttons and the grid its cards over a few frames
        int starts = 0;
        bool toolbarShown = false;
        bool gridShown = false;
        NotifyHook hook(ui.window, "tabMotionQueued", [&] {
            if (ui.window->property("tabMotionQueued").toBool())
                return;
            ++starts;
            QQuickItem *toolbar = ui.item(QStringLiteral("libraryToolbar"));
            for (QQuickItem *candidate : toolbar ? allItems(toolbar) : QList<QQuickItem *>{}) {
                if (candidate->inherits("QQuickAbstractButton") && candidate->isVisible()
                    && candidate->width() > 0
                    && candidate->property("text").toString() == QStringLiteral("Filters"))
                    toolbarShown = true;
            }
            QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
            gridShown = grid && grid->property("currentItem").value<QObject *>() != nullptr;
        });
        QVERIFY(ui.clickTab(QStringLiteral("Movies")));
        QTRY_COMPARE(starts, 1);
        QVERIFY(toolbarShown);
        QVERIFY(gridShown);
    }

    // The animators of the render thread write only their end value back: the row is dark from the switch (before the push) and whole again at the end
    void aTabSwitchFadesTheNewPageIn()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(!ui.tabSwitching());
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QVERIFY(stack);
        PropertyRecorder opacity(stack, "opacity");
        PropertyRecorder switching(ui.window, "tabSwitching");
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_VERIFY(!ui.tabSwitching());
        QVERIFY(!opacity.values.isEmpty());
        QCOMPARE(opacity.values.first(), 0.0);
        QTRY_COMPARE(stack->property("opacity").toReal(), 1.0);
        QVERIFY(switching.values.contains(1.0));
        QCOMPARE(switching.values.last(), 0.0);
        QCOMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
    }

    void aTabSwitchSlidesFromTheTabSide_data()
    {
        QTest::addColumn<QString>("fromLabel");
        QTest::addColumn<QString>("fromTab");
        QTest::addColumn<QString>("toLabel");
        QTest::addColumn<QString>("toTab");
        QTest::addColumn<bool>("rightToLeft");
        QTest::addColumn<bool>("entersFromRight");
        const QString home = QStringLiteral("home"), movies = QStringLiteral("movies"),
                      shows = QStringLiteral("tvshows"), favorites = QStringLiteral("favorites"),
                      playlists = QStringLiteral("playlists");
        QTest::newRow("home to movies") << "Home" << home << "Movies" << movies << false << true;
        QTest::newRow("movies to home") << "Movies" << movies << "Home" << home << false << false;
        QTest::newRow("movies to shows") << "Movies" << movies << "Shows" << shows << false << true;
        QTest::newRow("shows to home") << "Shows" << shows << "Home" << home << false << false;
        QTest::newRow("movies to favorites")
            << "Movies" << movies << "Favorites" << favorites << false << true;
        QTest::newRow("favorites to playlists")
            << "Favorites" << favorites << "Playlists" << playlists << false << true;
        QTest::newRow("playlists to shows")
            << "Playlists" << playlists << "Shows" << shows << false << false;
        QTest::newRow("favorites to home")
            << "Favorites" << favorites << "Home" << home << false << false;
        QTest::newRow("home to movies, right to left")
            << "Home" << home << "Movies" << movies << true << false;
        QTest::newRow("shows to movies, right to left")
            << "Shows" << shows << "Movies" << movies << true << true;
        QTest::newRow("movies to favorites, right to left")
            << "Movies" << movies << "Favorites" << favorites << true << false;
        QTest::newRow("playlists to favorites, right to left")
            << "Playlists" << playlists << "Favorites" << favorites << true << true;
    }

    // The new tab comes in from the side it has in the tab bar (mirrored in right-to-left): the sign of every offset the slide takes
    void aTabSwitchSlidesFromTheTabSide()
    {
        QFETCH(QString, fromLabel);
        QFETCH(QString, fromTab);
        QFETCH(QString, toLabel);
        QFETCH(QString, toTab);
        QFETCH(bool, rightToLeft);
        QFETCH(bool, entersFromRight);
        const Qt::LayoutDirection direction = QGuiApplication::layoutDirection();
        QGuiApplication::setLayoutDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
        const auto restoreDirection =
            qScopeGuard([direction] { QGuiApplication::setLayoutDirection(direction); });
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        // A click on a mirrored tab bar does not switch tabs offscreen: the right-to-left rows switch through the shell, as the tab actions do
        const auto switchTo = [&ui, rightToLeft](const QString &label, const QString &tab) {
            if (!rightToLeft)
                return ui.clickTab(label);
            return QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openTab",
                                             Q_ARG(QVariant, tab), Q_ARG(QVariant, QString()));
        };
        if (fromTab != QLatin1String("movies")) {
            QVERIFY(switchTo(fromLabel, fromTab));
            QTRY_COMPARE(ui.rootTab(), fromTab);
        }
        QTRY_VERIFY(!ui.tabSwitching());
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QVERIFY(stack);
        const QQmlListReference transforms(stack, "transform");
        QVERIFY(transforms.isValid() && transforms.count() == 1);
        QObject *shift = transforms.at(0);
        PropertyRecorder offsets(shift, "x");
        QVERIFY(switchTo(toLabel, toTab));
        QTRY_COMPARE(ui.rootTab(), toTab);
        QTRY_VERIFY(!ui.tabSwitching());
        QVERIFY(!offsets.values.isEmpty());
        QVERIFY(std::any_of(offsets.values.cbegin(), offsets.values.cend(),
                            [](qreal x) { return qAbs(x) > 1; }));
        const bool onTheRightSide =
            std::all_of(offsets.values.cbegin(), offsets.values.cend(),
                        [entersFromRight](qreal x) { return entersFromRight ? x >= 0 : x <= 0; });
        QVERIFY(onTheRightSide);
        QCOMPARE(shift->property("x").toReal(), 0.0);
    }

    // A switch that begins while the first one still runs ends the first at once: one page at the root, the row whole and in place
    void aSecondTabClickDuringTheSwitchEndsOnTheSecondTab()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("library")));
        QTRY_VERIFY(!ui.tabSwitching());
        auto *stack = ui.window->property("pageStack").value<QObject *>();
        QVERIFY(stack);
        const QQmlListReference transforms(stack, "transform");
        QVERIFY(transforms.isValid() && transforms.count() == 1);
        QObject *shift = transforms.at(0);
        QObject *root = ui.engine.rootObjects().constFirst();
        const auto openTab = [root](const QString &name) {
            return QMetaObject::invokeMethod(root, "openTab", Q_ARG(QVariant, name),
                                             Q_ARG(QVariant, QString()));
        };
        // Back to back, then with the first motion under way
        QVERIFY(openTab(QStringLiteral("tvshows")));
        QVERIFY(openTab(QStringLiteral("home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_VERIFY(!ui.tabSwitching());
        QVERIFY(openTab(QStringLiteral("movies")));
        QTest::qWait(60);
        QVERIFY(openTab(QStringLiteral("tvshows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTRY_VERIFY(!ui.tabSwitching());
        QCOMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(stack->property("opacity").toReal(), 1.0);
        QTRY_COMPARE(shift->property("x").toReal(), 0.0);
    }

    void selectorFromTwoLibraries()
    {
        Ui ui(m_home->path());
        ui.server.generatedItemTotal = 10;
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("moviesTwoLibraries")));
        QQuickItem *selector = ui.item(QStringLiteral("librarySelector"));
        QVERIFY(selector);
        // The selector sits in the toolbar's row: the filter bar stays the page's top row
        QQuickItem *toolbar = ui.item(QStringLiteral("libraryToolbar"));
        QVERIFY(toolbar);
        QTRY_VERIFY(qAbs(selector->mapToScene(QPointF(0, selector->height() / 2)).y()
                         - toolbar->mapToScene(QPointF(0, toolbar->height() / 2)).y())
                    < 1);
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
        // The six tabs come first in the Tab order, then the rest of the header, the selector and the grid after them
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        const QList<QQuickItem *> tabs = ui.tabButtons();
        QCOMPARE(tabs.size(), 6);
        tabs.first()->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(tabs.first()->hasActiveFocus());
        const QList<QQuickItem *> visited = ui.tabCycle();
        for (qsizetype i = 1; i < tabs.size(); ++i)
            QCOMPARE(visited.value(i - 1), tabs.at(i));
        const qsizetype selectorAt = visited.indexOf(selector);
        // The grid holds the focus itself or through the tile it made current
        QQuickItem *grid = ui.item(QStringLiteral("libraryGrid"));
        qsizetype gridAt = -1;
        for (qsizetype i = 0; i < visited.size() && gridAt < 0; ++i) {
            if (visited.at(i) == grid || grid->isAncestorOf(visited.at(i)))
                gridAt = i;
        }
        // Kirigami puts the header of a page after its content in the Tab order, so the selector follows the grid
        QVERIFY(selectorAt > 6);
        QVERIFY(gridAt > 6);
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
        for (const QQuickItem *label : ui.itemsWithText(hostile)) {
            QVERIFY2(showsPlainText(label), qPrintable(Ui::describe(label)));
        }
        QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void aVeryLongLibraryNameKeepsTheSelectorWithinItsCap()
    {
        Ui ui(m_home->path());
        ui.serveTwoMovieLibraries(QString(200, QLatin1Char('W')));
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("moviesTwoLibraries")));
        QQuickItem *selector = ui.item(QStringLiteral("librarySelector"));
        QVERIFY(selector);
        QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 2)));
        QTRY_VERIFY(selector->property("displayText").toString().size() >= 200);
        QTest::qWait(200);
        const qreal cap = gridUnitOf(ui) * 16;
        QTRY_VERIFY2(selector->width() <= cap + 1,
                     qPrintable(QStringLiteral("selector is %1 wide").arg(selector->width())));
        QTRY_VERIFY(selector->width() < ui.window->width() / 2);
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
        // The series tile opens the Shows tab
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

    void queueButtonShowsTheQueueCount()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QQuickItem *button = ui.item(QStringLiteral("queueButton"));
        QQuickItem *badge = ui.item(QStringLiteral("queueCountBadge"));
        QVERIFY(button && badge);
        QVERIFY(!badge->isVisible());
        ui.fillQueue();
        QTRY_COMPARE(ui.controller.queue()->count(), 5);
        QTRY_VERIFY(badge->isVisible());
        QVERIFY(!ui.itemsWithText(QStringLiteral("5")).isEmpty());
        bool inBadge = false;
        for (QQuickItem *label : ui.itemsWithText(QStringLiteral("5")))
            inBadge = inBadge || badge->isAncestorOf(label);
        QVERIFY(inBadge);
        // The badge sits on the button, inside it
        QVERIFY(button->mapRectToScene(button->boundingRect())
                    .contains(badge->mapRectToScene(badge->boundingRect())));
        ui.controller.queue()->clear();
        QTRY_VERIFY(!badge->isVisible());
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
        // A page above the tab: the first keystroke pops back to the tab (not over Settings or Favorites, where the box is global)
        QVariantMap queueProperties;
        queueProperties.insert(QStringLiteral("connection"), QVariant::fromValue(&ui.controller));
        QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openSection",
                                  Q_ARG(QVariant, QStringLiteral("queuePage")),
                                  Q_ARG(QVariant, QStringLiteral("QueuePage.qml")),
                                  Q_ARG(QVariant, queueProperties));
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
        QVERIFY(ui.clickTab(QStringLiteral("Shows")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("tvshows"));
        QTRY_COMPARE(field->property("text").toString(), QString());
        QCOMPARE(field->property("placeholderText").toString(), QStringLiteral("Search in Shows"));
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
        const struct {
            Qt::Key key;
            const char *root;
        } all[] = {{Qt::Key_2, "movies"},    {Qt::Key_3, "tvshows"},   {Qt::Key_4, "downloads"},
                   {Qt::Key_5, "favorites"}, {Qt::Key_6, "playlists"}, {Qt::Key_1, "home"}};
        for (const auto &step : all) {
            QTest::keyClick(ui.window, step.key, Qt::ControlModifier);
            QTRY_COMPARE(ui.rootTab(), QString::fromLatin1(step.root));
        }
        // Without a Movies tab every later tab moves one place down
        ui.serveViews({{kSeries, QStringLiteral("Series"), QStringLiteral("tvshows")}});
        ui.controller.reloadLibraries();
        QTRY_COMPARE(ui.tabNames(),
                     QStringList({QStringLiteral("Home"), QStringLiteral("Shows"),
                                  QStringLiteral("Downloads"), QStringLiteral("Favorites"),
                                  QStringLiteral("Playlists")}));
        const struct {
            Qt::Key key;
            const char *root;
        } fewer[] = {{Qt::Key_2, "tvshows"},
                     {Qt::Key_3, "downloads"},
                     {Qt::Key_4, "favorites"},
                     {Qt::Key_5, "playlists"}};
        for (const auto &step : fewer) {
            QTest::keyClick(ui.window, step.key, Qt::ControlModifier);
            QTRY_COMPARE(ui.rootTab(), QString::fromLatin1(step.root));
        }
        // The fifth position is gone
        QTest::keyClick(ui.window, Qt::Key_5, Qt::ControlModifier);
        QTest::qWait(200);
        QCOMPARE(ui.rootTab(), QStringLiteral("playlists"));
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
        QTRY_COMPARE(ui.tabNames().size(), 6);
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
        // The list opens again from the start: the click waits for the button to be
        // clickable, which the row only is once the page it sits on has slid back in
        QTRY_VERIFY((button = ui.item(QStringLiteral("homeRowSeeAll_0"))) != nullptr
                    && button->isVisible() && button->isEnabled());
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

    void aClickedSettingsEntryTakesTheFocusFrameAlong()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QVERIFY(openSettings(ui) != nullptr);
        // The page gives the active entry the keys when it opens
        QQuickItem *main = ui.item(QStringLiteral("settingsCategory_main"));
        QTRY_VERIFY(main && main->hasActiveFocus());
        QTRY_VERIFY(ui.item(QStringLiteral("settingsCategory_user")) != nullptr);
        QQuickItem *user = ui.item(QStringLiteral("settingsCategory_user"));
        QQuickItem *audio = ui.item(QStringLiteral("settingsCategory_audio"));
        QVERIFY(user && audio);
        // A click moves the keys along without a focus frame: no other entry keeps one and looks chosen
        ui.clickCenter(user);
        QTRY_VERIFY(user->hasActiveFocus());
        QVERIFY(!main->property("visualFocus").toBool());
        QVERIFY(!user->property("visualFocus").toBool());
        // The form comes a moment after the click, the spinner in its place meanwhile
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        QTRY_VERIFY(!ui.item(QStringLiteral("userPreferencesBuilding"))->isVisible());
        ui.clickCenter(audio);
        QTRY_VERIFY(audio->hasActiveFocus());
        QVERIFY(!user->property("checked").toBool());
        QVERIFY(!user->property("visualFocus").toBool());
        QVERIFY(audio->property("checked").toBool());
    }

    void theUserFormComesOnceAfterQuickSectionChanges()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QQuickItem *page = openSettings(ui);
        QVERIFY(page);
        // In and out and in again faster than the form starts: one form, for the entry chosen last
        for (const QString &section :
             {QStringLiteral("user"), QStringLiteral("audio"), QStringLiteral("user")})
            QVERIFY(QMetaObject::invokeMethod(page, "selectSection", Q_ARG(QVariant, section)));
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        QCOMPARE(page->property("activeSection").toString(), QStringLiteral("user"));
        QCOMPARE(ui.itemsStartingWith(QStringLiteral("userPreferencesForm")).size(), 1);
        // Out again before it starts: no form comes
        QVERIFY(QMetaObject::invokeMethod(page, "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("video"))));
        QVERIFY(QMetaObject::invokeMethod(page, "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("user"))));
        QVERIFY(QMetaObject::invokeMethod(page, "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("video"))));
        QTest::qWait(300);
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
        QVERIFY(!ui.item(QStringLiteral("userPreferencesBuilding"))->isVisible());
    }

    void aSecondVisitToUserShowsItsValuesAtOnce()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        QQuickItem *page = ui.item(QStringLiteral("settingsPage"));
        QVERIFY(QMetaObject::invokeMethod(page, "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("audio"))));
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
        // Another device changes a value meanwhile
        auto *prefs = ui.controller.userConfiguration();
        const bool played = prefs->playDefaultAudioTrack();
        remote.configuration[QStringLiteral("PlayDefaultAudioTrack")] = !played;
        // Back on User: the values loaded before show while the server is read again, no Loading on the way
        PropertyRecorder states(prefs, "state");
        QVERIFY(QMetaObject::invokeMethod(page, "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("user"))));
        QQuickItem *check = nullptr;
        QTRY_VERIFY((check = ui.item(QStringLiteral("prefPlayDefaultAudioTrack"))) != nullptr);
        QVERIFY(prefs->loaded());
        // The user is already in the form when the answer comes: the new value lands and the keys stay where they are
        check->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(check->hasActiveFocus());
        QTRY_COMPARE(prefs->playDefaultAudioTrack(), !played);
        QTest::qWait(100);
        QVERIFY(check->hasActiveFocus());
        QVERIFY(!states.values.contains(qreal(Kinefin::UserConfigurationModel::Loading)));
    }

    void aNarrowUserEntryGivesItsFormTheKeys()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(440));
        QVERIFY(ui.signIn());
        QQuickItem *page = openSettings(ui);
        QVERIFY(page);
        QTRY_VERIFY(page->property("narrow").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("settingsCategory_user")) != nullptr);
        // The entry opens in place of the list while its form is still to come: the form takes the keys once it is there
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_user")));
        QQuickItem *form = nullptr;
        QTRY_VERIFY((form = ui.item(QStringLiteral("userPreferencesForm"))) != nullptr);
        QTRY_VERIFY([&] {
            for (QQuickItem *item = ui.window->activeFocusItem(); item; item = item->parentItem()) {
                if (item == form)
                    return true;
            }
            return false;
        }());
    }

    void settingsShowsTheUserEntryOnlyWithASession()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("settings")));
        QQuickItem *page = ui.item(QStringLiteral("settingsPage"));
        QVERIFY(page);
        // A wide window leaves Settings through the tabs: no Back button in the header
        QVERIFY(!ui.item(QStringLiteral("headerBackButton"))->isVisible());
        // Signed out: the entries of this device only, and no link to the server preferences
        QVERIFY(ui.item(QStringLiteral("settingsCategory_main"))->isVisible());
        QVERIFY(ui.item(QStringLiteral("settingsCategory_user")) == nullptr);
        QVERIFY(QMetaObject::invokeMethod(page, "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("audio"))));
        QTRY_COMPARE(page->property("activeSection").toString(), QStringLiteral("audio"));
        QVERIFY(!ui.item(QStringLiteral("settingsTwinCard"))->isVisible());
        // Signed in: the entry comes after the entries of this device, in the same list
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsCategory_user")) != nullptr);
        QQuickItem *user = ui.item(QStringLiteral("settingsCategory_user"));
        QVERIFY(user->isVisible());
        QCOMPARE(user->property("text").toString(), QStringLiteral("User"));
        QCOMPARE(user->parentItem(),
                 ui.item(QStringLiteral("settingsCategory_other"))->parentItem());
        QVERIFY(ui.item(QStringLiteral("settingsCategory_other"))->mapToScene(QPointF(0, 0)).y()
                < user->mapToScene(QPointF(0, 0)).y());
        QCOMPARE(ui.item(QStringLiteral("settingsCategory_other"))->property("text").toString(),
                 QStringLiteral("mpv Configuration"));
        // The form is not alive until the entry is chosen
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
        ui.clickCenter(user);
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        QCOMPARE(ui.item(QStringLiteral("settingsContentHeading"))->property("text").toString(),
                 QStringLiteral("User"));
        QCOMPARE(ui.stackDepth(), 2);
    }

    void theMainMenuOffersNoUserPreferences()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(ui.shellAction(QStringLiteral("userPreferencesAction")) == nullptr);
        ui.clickCenter(ui.item(QStringLiteral("mainMenuButton")));
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QStringList names;
        for (const QQuickItem *entry : ui.menuItems(QStringLiteral("mainMenu")))
            names << entry->objectName();
        // The account entries are there, and none of them is about preferences: those live in Settings
        for (const char *name : {"switchUserItem", "signOutItem", "switchServerItem"})
            QVERIFY2(names.contains(QLatin1String(name)), name);
        for (const QString &name : std::as_const(names))
            QVERIFY2(!name.contains(QLatin1String("Preferences"), Qt::CaseInsensitive),
                     qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
    }

    void userPreferencesOpenFromSettingsAndShowTheStoredValues()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        // Settings marks the menu button, no tab is marked, and the form is a page of its own no more
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("settings"));
        QVERIFY(ui.item(QStringLiteral("mainMenuButton"))->property("down").toBool());
        QCOMPARE(ui.stackDepth(), 2);
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
        // Real clicks: click() on a form delegate does not toggle it the way a press and release do
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
                                                "system, so nothing was saved: keep editing to "
                                                "review your changes, or discard them")));
        QVERIFY(remote.posts.isEmpty());
        QVERIFY(!ui.item(QStringLiteral("prefRememberAudioSelections"))->isEnabled());
        // Keep Editing gives the draft back; a change on the server since then is a new conflict, and Discard drops the draft
        QVERIFY(ui.clickButton(QStringLiteral("Keep Editing")));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Ready);
        QVERIFY(prefs->dirty());
        QVERIFY(ui.item(QStringLiteral("prefRememberAudioSelections"))->isEnabled());
        remote.configuration.insert(QStringLiteral("SubtitleMode"), QStringLiteral("OnlyForced"));
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("prefsSaveButton")), "click"));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Conflict);
        QVERIFY(ui.clickButton(QStringLiteral("Discard")));
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Ready);
        QVERIFY(!prefs->dirty());
        // Discard shows what the server holds now
        QTRY_COMPARE(prefs->subtitleMode(), QStringLiteral("OnlyForced"));
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
        // Without a draft Escape just leaves Settings
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);
        QVERIFY(openPreferences(ui) != nullptr);
        dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // With one it asks before Settings is popped, and Cancel stays
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Cancel")));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(prefs->dirty());
        QCOMPARE(prefs->subtitleMode(), QStringLiteral("Always"));
        // Another entry asks the same, and Discard opens it
        QQuickItem *settingsPage = ui.item(QStringLiteral("settingsPage"));
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_audio")));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(settingsPage->property("activeSection").toString(), QStringLiteral("user"));
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Discard")));
        QTRY_COMPARE(settingsPage->property("activeSection").toString(), QStringLiteral("audio"));
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
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
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(!prefs->dirty());
    }

    void accountActionsAskBeforeDroppingUnsavedPreferences_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("switch user") << QStringLiteral("switchUserAction");
        QTest::newRow("sign out") << QStringLiteral("signOutAction");
        QTest::newRow("switch server") << QStringLiteral("switchServerAction");
    }

    void accountActionsAskBeforeDroppingUnsavedPreferences()
    {
        QFETCH(QString, action);
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QObject *signOut = ui.popup(QStringLiteral("signOutDialog"));
        QVERIFY(dialog && signOut);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // The unsaved changes are asked about first: the session and the draft are untouched, and Cancel keeps them
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(action), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(!signOut->property("visible").toBool());
        QCOMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QVERIFY(prefs->dirty());
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Cancel")));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui.controller.state(), ConnectionController::Authenticated);
        QVERIFY(prefs->dirty());
        QCOMPARE(prefs->subtitleMode(), QStringLiteral("Always"));
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        // Discard settles the draft, then the action goes on
        QVERIFY(QMetaObject::invokeMethod(ui.shellAction(action), "trigger"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(
            clickInDialog(ui, QStringLiteral("unsavedChangesDialog"), QStringLiteral("Discard")));
        QVERIFY(!prefs->dirty());
        if (action == QStringLiteral("switchUserAction")) {
            QTRY_COMPARE(ui.controller.state(), ConnectionController::ChoosingUser);
        } else if (action == QStringLiteral("switchServerAction")) {
            QTRY_COMPARE(ui.controller.state(), ConnectionController::Idle);
        } else {
            // Sign Out still asks its own question
            QTRY_VERIFY(signOut->property("opened").toBool());
            QCOMPARE(ui.controller.state(), ConnectionController::Authenticated);
        }
        QVERIFY(remote.posts.isEmpty());
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
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("settingsPage"));
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

    void theHeaderSearchIsGlobalOnTheSettingsPage()
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
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("settings"));
        QTRY_COMPARE(field->property("placeholderText").toString(), QStringLiteral("Search"));
        // The type tabs come with the libraries: all six must be there before one is read by its place
        QTRY_COMPARE(ui.tabButtons().size(), 6);
        for (int index = 0; index < 6; ++index)
            QTRY_VERIFY(!ui.tabButtons().value(index)->property("checked").toBool());
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        ui.type(QStringLiteral("ab"));
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("ab"));
        QCOMPARE(ui.controller.movies()->searchTerm(), QString());
        QCOMPARE(ui.stackDepth(), 3);
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
    }

    void aFailedLoadOffersRetryAndAMissingCatalogKeepsTheStoredLanguage()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        // The catalog and the configuration are down from the start: the read the sign-in starts for Home fails, so the page has no values to show and loads
        ui.server.culturesStatus = 500;
        ui.server.usersMeStatus = 500;
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        auto *prefs = ui.controller.userConfiguration();
        QTRY_COMPARE(prefs->state(), Kinefin::UserConfigurationModel::Failed);
        QVERIFY(!prefs->loaded());
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("settingsPage")), "selectSection",
                                          Q_ARG(QVariant, QStringLiteral("user"))));
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
            QVERIFY2(showsPlainText(label), qPrintable(Ui::describe(label)));
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
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
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

    void leavingTheUserEntryAsksAboutTheDraft_data()
    {
        QTest::addColumn<QString>("exit");
        QTest::addColumn<QString>("outcome");
        for (const QString &exit :
             {QStringLiteral("another entry"), QStringLiteral("escape"),
              QStringLiteral("header back"), QStringLiteral("mouse back"),
              QStringLiteral("tab shortcut"), QStringLiteral("narrow back")}) {
            for (const QString &outcome :
                 {QStringLiteral("cancel"), QStringLiteral("discard"), QStringLiteral("save")})
                QTest::addRow("%s, %s", qPrintable(exit), qPrintable(outcome)) << exit << outcome;
        }
        // Closing ends the process of the test window: only the answer that keeps it open is run
        QTest::addRow("close window, cancel")
            << QStringLiteral("close window") << QStringLiteral("cancel");
    }

    void leavingTheUserEntryAsksAboutTheDraft()
    {
        QFETCH(QString, exit);
        QFETCH(QString, outcome);
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // The header Back button is there only while a narrow window shows an entry instead of the list
        if (exit == QLatin1String("narrow back") || exit == QLatin1String("header back"))
            QVERIFY(ui.opensWindowOfWidth(440));
        QQuickItem *page = openPreferences(ui);
        QVERIFY(page != nullptr);
        QQuickItem *settingsPage = ui.item(QStringLiteral("settingsPage"));
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // The ways out, each as the user would take it
        if (exit == QLatin1String("another entry")) {
            ui.clickCenter(ui.item(QStringLiteral("settingsCategory_audio")));
        } else if (exit == QLatin1String("escape") || exit == QLatin1String("narrow back")) {
            QTest::keyClick(ui.window, Qt::Key_Escape);
        } else if (exit == QLatin1String("header back")) {
            ui.clickCenter(ui.item(QStringLiteral("headerBackButton")));
        } else if (exit == QLatin1String("mouse back")) {
            QTest::mouseClick(ui.window, Qt::BackButton, Qt::NoModifier,
                              QPoint(ui.window->width() / 2, ui.window->height() / 2));
        } else if (exit == QLatin1String("tab shortcut")) {
            QTest::keyClick(ui.window, Qt::Key_1, Qt::ControlModifier);
        } else {
            ui.window->close();
        }
        QTRY_VERIFY(dialog->property("opened").toBool());
        // Nothing has moved under the question
        QVERIFY(ui.window->isVisible());
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(settingsPage->property("activeSection").toString(), QStringLiteral("user"));
        QVERIFY(ui.item(QStringLiteral("userPreferencesForm")) != nullptr);
        QVERIFY(prefs->dirty());
        QVERIFY(remote.posts.isEmpty());
        if (outcome == QLatin1String("cancel")) {
            QVERIFY(clickInDialog(ui, QStringLiteral("unsavedChangesDialog"),
                                  QStringLiteral("Cancel")));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QVERIFY(prefs->dirty());
            QCOMPARE(prefs->subtitleMode(), QStringLiteral("Always"));
            QCOMPARE(ui.stackDepth(), 2);
            QCOMPARE(settingsPage->property("activeSection").toString(), QStringLiteral("user"));
            QVERIFY(ui.window->isVisible());
            QVERIFY(remote.posts.isEmpty());
            return;
        }
        QVERIFY(clickInDialog(ui, QStringLiteral("unsavedChangesDialog"),
                              outcome == QLatin1String("save") ? QStringLiteral("Save")
                                                               : QStringLiteral("Discard")));
        if (outcome == QLatin1String("save"))
            QTRY_COMPARE(remote.posts.size(), 1);
        // The way out goes on, after the draft was saved once or dropped without a request
        const auto left = [&]() -> bool {
            const bool gone = ui.item(QStringLiteral("settingsPage")) == nullptr;
            if (exit == QLatin1String("another entry"))
                return settingsPage->property("activeSection").toString() == QLatin1String("audio")
                       && ui.item(QStringLiteral("userPreferencesForm")) == nullptr;
            if (exit == QLatin1String("narrow back") || exit == QLatin1String("header back"))
                return !gone && !settingsPage->property("entryOpen").toBool();
            if (exit == QLatin1String("tab shortcut"))
                return gone && ui.item(QStringLiteral("homePage")) != nullptr;
            return gone;
        };
        QVERIFY(qWaitForScaled(left, 10000));
        QVERIFY(!prefs->dirty());
        QCOMPARE(remote.posts.size(), outcome == QLatin1String("save") ? 1 : 0);
    }

    void theUserFormSurvivesAWidthChange()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *form = openPreferences(ui);
        QVERIFY(form != nullptr);
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // A form created again would read the server and drop the draft
        for (const int width : {440, 1000, 560, 1600}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
            QCOMPARE(ui.item(QStringLiteral("userPreferencesForm")), form);
            QCOMPARE(ui.itemsStartingWith(QStringLiteral("userPreferencesForm")).size(), 1);
            QVERIFY(prefs->dirty());
            QCOMPARE(prefs->subtitleMode(), QStringLiteral("Always"));
            QVERIFY(remote.posts.isEmpty());
        }
        QVERIFY(!dialog->property("visible").toBool());
    }

    void narrowSettingsOpenEntriesInPlace()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(440));
        QVERIFY(ui.signIn());
        QQuickItem *page = openSettings(ui);
        QVERIFY(page != nullptr);
        QTRY_VERIFY(page->property("narrow").toBool());
        // The list shows alone, the account entry in it
        QTRY_VERIFY(ui.item(QStringLiteral("settingsCategory_user")) != nullptr);
        QVERIFY(ui.item(QStringLiteral("settingsCategory_user"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(!ui.item(QStringLiteral("headerBackButton"))->isVisible());
        // An entry opens in place of the list and the stack stays as deep; the header Back button leads back to the list
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_audio")));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QVERIFY(ui.item(QStringLiteral("headerBackButton"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("settingsCategory_audio"))->isVisible());
        QCOMPARE(ui.item(QStringLiteral("settingsContentHeading"))->property("text").toString(),
                 QStringLiteral("Audio"));
        QCOMPARE(ui.stackDepth(), 2);
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // The first Escape closes the entry, the second leaves Settings
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsCategory_audio"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QCOMPARE(ui.stackDepth(), 2);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsPage")) == nullptr);
        QCOMPARE(ui.stackDepth(), 1);
        // The header Back button and the mouse back button do the same
        QVERIFY(openSettings(ui) != nullptr);
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_video")));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
        ui.clickCenter(ui.item(QStringLiteral("headerBackButton")));
        QTRY_VERIFY(!ui.item(QStringLiteral("settingsContentView"))->isVisible());
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_subtitles")));
        QTRY_VERIFY(ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QTest::mouseClick(ui.window, Qt::BackButton, Qt::NoModifier,
                          QPoint(ui.window->width() / 2, ui.window->height() / 2));
        QTRY_VERIFY(!ui.item(QStringLiteral("settingsContentView"))->isVisible());
        QCOMPARE(ui.stackDepth(), 2);
    }

    void reopeningSettingsKeepsTheDraft()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        QQuickItem *page = ui.item(QStringLiteral("settingsPage"));
        QQuickItem *form = ui.item(QStringLiteral("userPreferencesForm"));
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        // The menu entry of the page that is open brings it back as it is: nothing is asked and nothing is read again
        QVERIFY(
            QMetaObject::invokeMethod(ui.shellAction(QStringLiteral("settingsAction")), "trigger"));
        QVERIFY(!dialog->property("visible").toBool());
        QCOMPARE(ui.item(QStringLiteral("settingsPage")), page);
        QCOMPARE(ui.item(QStringLiteral("userPreferencesForm")), form);
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(prefs->dirty());
        QCOMPARE(prefs->subtitleMode(), QStringLiteral("Always"));
        QVERIFY(remote.posts.isEmpty());
    }

    void typingInTheHeaderOverSettingsKeepsTheDraft()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        QQuickItem *form = ui.item(QStringLiteral("userPreferencesForm"));
        auto *prefs = ui.controller.userConfiguration();
        QObject *dialog = ui.popup(QStringLiteral("unsavedChangesDialog"));
        QVERIFY(dialog);
        prefs->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(prefs->dirty());
        QQuickItem *field = ui.item(QStringLiteral("headerSearchField"));
        QVERIFY(field);
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        ui.type(QStringLiteral("ab"));
        // The results open above Settings: the page below keeps its form and its draft, and nothing asks
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("ab"));
        QCOMPARE(ui.stackDepth(), 3);
        QCOMPARE(ui.item(QStringLiteral("userPreferencesForm")), form);
        QVERIFY(prefs->dirty());
        QVERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(field->hasActiveFocus());
        QVERIFY(remote.posts.isEmpty());
    }

    void settingsTabOrderIsListThenFormThenSaveBar()
    {
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        // A draft turns the buttons of the bar on, so they are tab stops
        ui.controller.userConfiguration()->setSubtitleMode(QStringLiteral("Always"));
        QTRY_VERIFY(ui.item(QStringLiteral("prefsSaveButton"))->isEnabled());
        QQuickItem *first = ui.item(QStringLiteral("settingsCategory_main"));
        first->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(first->hasActiveFocus());
        // 0 the list, 1 the form, 2 the Save bar, -1 the rest of the window
        const auto areaOf = [](const QQuickItem *item) {
            for (const QQuickItem *parent = item; parent; parent = parent->parentItem()) {
                const QString name = parent->objectName();
                if (name == QLatin1String("settingsEntryList"))
                    return 0;
                if (name == QLatin1String("userPreferencesForm"))
                    return 1;
                if (name == QLatin1String("settingsSaveBar"))
                    return 2;
            }
            return -1;
        };
        QStringList trail;
        int area = 0;
        bool sawForm = false;
        bool sawBar = false;
        for (int step = 0; step < 100; ++step) {
            QTest::keyClick(ui.window, Qt::Key_Tab);
            const QQuickItem *focused = ui.window->activeFocusItem();
            QVERIFY(focused);
            const int now = areaOf(focused);
            trail << QStringLiteral("%1: %2").arg(now).arg(Ui::describe(focused));
            // A stop outside the three areas (a scroll view) is neutral; once the bar is behind, the Tab goes on to the shell
            if (now < 0) {
                if (sawBar)
                    break;
                continue;
            }
            QVERIFY2(now >= area, qPrintable(trail.join(QLatin1Char('\n'))));
            sawForm = sawForm || now == 1;
            sawBar = sawBar || now == 2;
            area = now;
        }
        QVERIFY2(sawForm && sawBar, qPrintable(trail.join(QLatin1Char('\n'))));
    }

    void twinEntriesLinkToEachOther_data()
    {
        QTest::addColumn<QString>("section");
        for (const QString &section :
             {QStringLiteral("audio"), QStringLiteral("subtitles"), QStringLiteral("library")})
            QTest::newRow(qPrintable(section)) << section;
    }

    void twinEntriesLinkToEachOther()
    {
        QFETCH(QString, section);
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openSettings(ui);
        QVERIFY(page != nullptr);
        // No twin on an entry that has none
        QVERIFY(!ui.item(QStringLiteral("settingsTwinCard"))->isVisible());
        ui.clickCenter(ui.item(QStringLiteral("settingsCategory_") + section));
        QTRY_COMPARE(page->property("activeSection").toString(), section);
        QQuickItem *link = ui.item(QStringLiteral("settingsLinkToUser_") + section);
        QVERIFY(link);
        QTRY_VERIFY(ui.item(QStringLiteral("settingsTwinCard"))->isVisible());
        // To the User entry, and back to the entry the link came from
        QVERIFY(ui.revealAndClickUntil(page, link, [&] {
            return page->property("activeSection").toString() == QLatin1String("user");
        }));
        QTRY_VERIFY(ui.item(QStringLiteral("userLinkToDevice_") + section) != nullptr);
        QQuickItem *back = ui.item(QStringLiteral("userLinkToDevice_") + section);
        QTRY_VERIFY(ui.controller.userConfiguration()->state()
                        == Kinefin::UserConfigurationModel::Ready
                    && ui.controller.userConfiguration()->loaded());
        QVERIFY(ui.revealAndClickUntil(
            page, back, [&] { return page->property("activeSection").toString() == section; }));
        QTRY_VERIFY(ui.item(QStringLiteral("userPreferencesForm")) == nullptr);
        QVERIFY(remote.posts.isEmpty());
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

    void theFavoritesTabOpensTheFavoritesPageAsTheRootAndMarksItself()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("home")));
        const QVariant homeToolBarStyle =
            ui.item(QStringLiteral("homePage"))->property("globalToolBarStyle");
        QCOMPARE(ui.tabNames().value(4), QStringLiteral("Favorites"));
        QQuickItem *tab = ui.tabButtons().value(4);
        QVERIFY(tab && tab->isVisible() && tab->isEnabled());
        QVERIFY(!tab->property("checked").toBool());
        QTRY_VERIFY(!ui.controller.libraries().isEmpty());
        QVERIFY(ui.clickTab(QStringLiteral("Favorites")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QCOMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.rootTab(), QStringLiteral("favorites"));
        // No title bar, as on Home: the actions are a toolbar on top of the rows, with no Reload of their own
        QCOMPARE(ui.currentDetails()->property("globalToolBarStyle"), homeToolBarStyle);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesToolbar"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesHeading"))->isVisible());
        QVERIFY(!ui.currentDetails()->findChild<QObject *>(QStringLiteral("favoritesReload")));
        // The fade belongs to the view, not to its scrolling content
        QQuickItem *fade = ui.item(QStringLiteral("favoritesTopFade"));
        QVERIFY(fade);
        QCOMPARE(fade->parentItem(), ui.item(QStringLiteral("favoritesGroups")));
        QCOMPARE(fade->y(), 0.0);
        // The tab marks itself and no other does
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        // The type tabs come with the libraries: all six must be there before one is read by its place
        QTRY_COMPARE(ui.tabButtons().size(), 6);
        for (int index = 0; index < 6; ++index)
            QTRY_COMPARE(ui.tabButtons().value(index)->property("checked").toBool(), index == 4);
        // By type, the default: the movies, the shows and the episode, with their counts
        QCOMPARE(ui.controller.favorites()->count(), 6);
        QTRY_COMPARE(favoriteGroupTitles(ui),
                     QStringList({QStringLiteral("Movies"), QStringLiteral("Shows"),
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
        QTRY_VERIFY(itemHasFocus(ui, QStringLiteral("favoritesRow_0")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // The tab again keeps the page as it is: no second page, no second scan
        const int requests = ui.server.itemsRoutes.size();
        QVERIFY(ui.clickTab(QStringLiteral("Favorites")));
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QCOMPARE(ui.server.itemsRoutes.size(), requests);
    }

    void theFavoritesPageHasNoTitleInCompactModeAndTheQueueKeepsIt()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // Below the Compact limit, which the shell derives from grid units: a fixed width may lie above it
        QVERIFY(ui.opensWindowOfWidth(ui.window->property("compactLimit").toInt() - 40));
        QTRY_VERIFY(ui.shellHeader()->property("compact").toBool());
        // The tab is in the bar at every width, and the menu has no entry for it
        QVERIFY(!ui.shellAction(QStringLiteral("compactFavoritesAction")));
        QTRY_VERIFY(ui.tabNames().contains(QStringLiteral("Favorites")));
        QTRY_VERIFY(!ui.controller.libraries().isEmpty());
        QVERIFY(ui.clickTab(QStringLiteral("Favorites")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        // The tab names the page already: no title beside its toolbar
        QVERIFY(!ui.item(QStringLiteral("favoritesHeading"))->isVisible());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // The queue has no tab, its button is hidden, and so the page names itself
        QObject *queue = ui.shellAction(QStringLiteral("compactQueueAction"));
        QVERIFY(queue && QMetaObject::invokeMethod(queue, "trigger"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("queuePage"));
        QTRY_VERIFY(ui.item(QStringLiteral("queueHeading"))->isVisible());
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
                                             QStringLiteral("Drama"), QStringLiteral("No Genre")});
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
        QTRY_VERIFY_WITH_TIMEOUT(settled(), scaled(15000));
        if (ui.controller.state() == ConnectionController::NeedsLogin)
            ui.controller.login(QStringLiteral("fixture-user"), QStringLiteral("fixture-password"));
        QTRY_VERIFY_WITH_TIMEOUT(ui.item(QStringLiteral("homePage")) != nullptr, scaled(15000));
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
        QCOMPARE(ui.stackDepth(), 2);
        QCOMPARE(ui.currentDetails()->property("title").toString(), QStringLiteral("Shows"));
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QQuickItem *grid = nullptr;
        QTRY_VERIFY((grid = ui.item(QStringLiteral("favoritesGroupGrid"))) != nullptr);
        QTRY_COMPARE(grid->property("count").toInt(), 2);
        QTRY_VERIFY(grid->hasActiveFocus());
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // Back returns to the row the user came from
        QPointer<QQuickItem> groupPage = ui.currentDetails();
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(!groupPage);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_VERIFY(itemHasFocus(ui, QStringLiteral("favoritesRow_0")));
        // A card opens its item above the page and Escape comes back to the same card
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_VERIFY(itemHasFocus(ui, QStringLiteral("favoritesRow_0")));
        // A replaced snapshot takes the group page with it
        QVERIFY(QMetaObject::invokeMethod(ui.item(QStringLiteral("favoritesShowAll_0")), "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesGroupPage"));
        data->byLibrary[kSeries] = QJsonArray();
        ui.controller.favorites()->reload();
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QTRY_COMPARE(ui.stackDepth(), 1);
    }

    void theMenuKeyOpensTheCardMenuAndEscapeClosesItFirst()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        QTRY_VERIFY(itemHasFocus(ui, QStringLiteral("favoritesRow_0")));
        QObject *menu = ui.popup(QStringLiteral("cardMenu"));
        QVERIFY(menu);
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        // The card menu is the one of every page, with no entry that changes a favorite
        QCOMPARE(menu->property("itemId").toString(), favoriteId(1));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        // The desktop style fades a menu out: opening it again before that ends leaves it closed
        QTRY_VERIFY(!menu->property("visible").toBool());
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QTest::keyClick(ui.window, Qt::Key_F10, Qt::ShiftModifier);
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("opened").toBool());
    }

    void leavingTheTabClosesThePageAndOpeningAgainScansAgain()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        QCOMPARE(ui.window->property("favoritesPages").toInt(), 1);
        QPointer<QQuickItem> page = ui.item(QStringLiteral("favoritesPage"));
        // Back at the root of a tab does nothing, as on Home
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTest::qWait(300);
        QCOMPARE(ui.stackDepth(), 1);
        QVERIFY(page);
        // Another tab takes the page away
        QVERIFY(ui.clickTab(QStringLiteral("Home")));
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("home"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        QTRY_VERIFY(!page);
        QTRY_COMPARE(ui.window->property("favoritesPages").toInt(), 0);
        QTRY_COMPARE(ui.window->property("currentSection").toString(), QStringLiteral("home"));
        // The next visit starts from a new scan: the tab shows nothing of the old snapshot while it runs
        const int requests = ui.server.itemsRoutes.size();
        QVERIFY(ui.clickTab(QStringLiteral("Favorites")));
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
        // Favorites is the root of the stack and the only tab marked, and the box searches the server
        QCOMPARE(ui.rootTab(), QStringLiteral("favorites"));
        QCOMPARE(ui.window->property("currentSection").toString(), QStringLiteral("favorites"));
        QTRY_COMPARE(field->property("placeholderText").toString(), QStringLiteral("Search"));
        // The type tabs come with the libraries: all six must be there before one is read by its place
        QTRY_COMPARE(ui.tabButtons().size(), 6);
        for (int index = 0; index < 6; ++index)
            QTRY_COMPARE(ui.tabButtons().value(index)->property("checked").toBool(), index == 4);
        field->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(field->hasActiveFocus());
        ui.type(QStringLiteral("ab"));
        QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        QCOMPARE(ui.controller.search()->term(), QStringLiteral("ab"));
        QCOMPARE(ui.controller.movies()->searchTerm(), QString());
        QCOMPARE(ui.stackDepth(), 2);
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

    void theFavoritesPlaceholderForAScanThatDidNotRunShowsOnlyThen()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        // A scan that is under way is kept Loading, then dropped: the page is left with no snapshot and no scan
        ui.server.holdListings = true;
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "openFavorites"));
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesPage")) != nullptr);
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Loading);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesLoading"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesNotLoaded"))->isVisible());
        ui.controller.favorites()->close();
        QCOMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Idle);
        QTRY_VERIFY(ui.item(QStringLiteral("favoritesNotLoaded"))->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("Favorites could not be loaded")));
        QVERIFY(!ui.item(QStringLiteral("favoritesLoading"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesEmpty"))->isVisible());
        QVERIFY(!ui.item(QStringLiteral("favoritesNoLibraries"))->isVisible());
        // Retry scans again and the placeholder goes with the answer
        ui.server.holdListings = false;
        QObject *action = ui.item(QStringLiteral("favoritesNotLoaded"))
                              ->property("helpfulAction")
                              .value<QObject *>();
        QVERIFY(action && action->property("text").toString() == QStringLiteral("Retry"));
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
        QTRY_COMPARE(ui.controller.favorites()->status(), Kinefin::FavoritesModel::Complete);
        QTRY_VERIFY(!ui.item(QStringLiteral("favoritesNotLoaded"))->isVisible());
        QTRY_COMPARE(favoriteGroupTitles(ui).size(), 3);
    }

    void upAndDownMoveTheKeysBetweenTheFavoritesGroups()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("home")));
        QVERIFY(openFavorites(ui));
        // The rows build their lists asynchronously: the keys go once the first cards of both are there
        QQuickItem *first = nullptr;
        QQuickItem *second = nullptr;
        QTRY_VERIFY((first = ui.item(QStringLiteral("favoritesRow_0"))) != nullptr
                    && first->property("currentItem").value<QObject *>() != nullptr
                    && first->hasActiveFocus());
        QTRY_VERIFY((second = ui.item(QStringLiteral("favoritesRow_1"))) != nullptr
                    && second->property("currentItem").value<QObject *>() != nullptr);
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(second->hasActiveFocus());
        QVERIFY(!first->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(first->hasActiveFocus());
        QVERIFY(!second->hasActiveFocus());
        // Up from the first group has nowhere to go
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(first->hasActiveFocus());
    }

    void theFavoritesCardsKeepTheirKeys()
    {
        Ui ui(m_home->path());
        serveFavorites(ui);
        QVERIFY(ui.load());
        QVERIFY(buildState(ui, QStringLiteral("favorites")));
        QTRY_VERIFY(ui.layoutProblems().isEmpty());
        // Down walks the rows
        QQuickItem *first = nullptr;
        QTRY_VERIFY((first = ui.item(QStringLiteral("favoritesRow_0"))) && first->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(itemHasFocus(ui, QStringLiteral("favoritesRow_1")));
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

    void continueWatchingCardShowsPlayOnMouseHoverOnly()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = homeRowMovieCard(ui, 0)) != nullptr);
        // The card is in the scene while its row is still being built: the button comes with it
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = Ui::itemIn(card, QStringLiteral("cardQuickPlay"))) != nullptr);
        QVERIFY(!button->isVisible());
        // Keyboard focus on the row does not show it
        QQuickItem *list = ui.item(QStringLiteral("homeRowList_0"));
        list->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(list->hasActiveFocus());
        QVERIFY(!button->isVisible());
        QVERIFY(revealCard(ui, card));
        const auto fades = [](const PropertyRecorder &recorder) {
            return std::any_of(recorder.values.cbegin(), recorder.values.cend(),
                               [](qreal value) { return value > 0 && value < 1; });
        };
        // The pointer on the card, away from the button in its middle, fades it in
        PropertyRecorder fadeIn(button, "opacity");
        QTest::mouseMove(
            ui.window,
            card->mapToScene(QPointF(card->width() * 0.15, card->height() * 0.1)).toPoint());
        QTRY_VERIFY(button->isVisible());
        QTRY_COMPARE(button->opacity(), 1.0);
        QVERIFY(fades(fadeIn));
        auto *disc = button->property("background").value<QQuickItem *>();
        QVERIFY(disc);
        QVERIFY(!button->property("hovered").toBool());
        const QColor resting = disc->property("color").value<QColor>();
        // The pointer moving onto the button keeps it shown and lights it up
        QTest::mouseMove(
            ui.window,
            button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
        QTRY_VERIFY(button->property("hovered").toBool());
        QTRY_VERIFY(disc->property("color").value<QColor>() != resting);
        QTRY_VERIFY(button->scale() > 1.0);
        QVERIFY(button->isVisible());
        // Over the button the pointer is a hand
        QTRY_COMPARE(ui.window->cursor().shape(), Qt::PointingHandCursor);
        // The glyph is centred vertically and a little right of the middle, where the triangle looks centred
        auto *content = button->property("contentItem").value<QQuickItem *>();
        QVERIFY(content && !content->childItems().isEmpty());
        QQuickItem *glyph = content->childItems().constFirst();
        const QPointF glyphCenter =
            glyph->mapToItem(button, QPointF(glyph->width() / 2, glyph->height() / 2));
        QVERIFY(qAbs(glyphCenter.y() - button->height() / 2) < 1);
        QVERIFY(glyphCenter.x() > button->width() / 2);
        QVERIFY(glyphCenter.x() < button->width() / 2 + glyph->width() * 0.1);
        // The button stays inside the card
        const QRectF area = button->mapRectToItem(card, QRectF(QPointF(0, 0), button->size()));
        QVERIFY(card->boundingRect().contains(area));
        // The pointer leaving the card fades it out
        PropertyRecorder fadeOut(button, "opacity");
        QTest::mouseMove(ui.window, QPoint(ui.window->width() - 2, ui.window->height() - 2));
        QTRY_VERIFY(!button->isVisible());
        QVERIFY(fades(fadeOut));
    }

    void continueWatchingCardPlayButtonResumesTheItem()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QuickPlayStub stub;
        QVERIFY(ui.engine.rootObjects().constFirst()->setProperty(
            "player", QVariant::fromValue<QObject *>(&stub)));
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = homeRowMovieCard(ui, 0)) != nullptr);
        // The card is in the scene while its row is still being built: the button comes with it
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = Ui::itemIn(card, QStringLiteral("cardQuickPlay"))) != nullptr);
        QVERIFY(revealCard(ui, card));
        QTest::mouseMove(ui.window, cardPoint(card));
        QTRY_VERIFY(button->isVisible());
        const QPoint center =
            button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, center);
        QTRY_COMPARE(stub.calls.size(), 1);
        QCOMPARE(stub.calls.constFirst().id, kMovieId);
        QCOMPARE(stub.calls.constFirst().resumeTicks, qint64(2340000000));
        // The click does not open the details page as well
        QVERIFY(!ui.item(QStringLiteral("itemDetailsPage")));
        QCOMPARE(ui.stackDepth(), 1);
    }

    void continueWatchingCardIsTheOnlyOneWithTheQuickPlay()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.server.listingRawBody.insert(
            QStringLiteral("NextUp"),
            envelope(QJsonArray{fixtureObject(QStringLiteral("movie_item"))}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *resumeCard = nullptr;
        QQuickItem *nextUpCard = nullptr;
        QTRY_VERIFY((resumeCard = homeRowMovieCard(ui, 0)) != nullptr);
        QTRY_VERIFY((nextUpCard = homeRowMovieCard(ui, 1)) != nullptr);
        QVERIFY(resumeCard->property("quickPlay").toBool());
        QVERIFY(!nextUpCard->property("quickPlay").toBool());
        QVERIFY(revealCard(ui, nextUpCard));
        QTest::mouseMove(ui.window, cardPoint(nextUpCard));
        QTRY_VERIFY(nextUpCard->property("hovered").toBool());
        QVERIFY(!Ui::cardShows(nextUpCard, QStringLiteral("cardQuickPlay")));
    }

    void continueWatchingCardMenuHasResumeOnlyThere()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        ui.server.listingRawBody.insert(
            QStringLiteral("NextUp"),
            envelope(QJsonArray{fixtureObject(QStringLiteral("movie_item"))}));
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QuickPlayStub stub;
        QVERIFY(ui.engine.rootObjects().constFirst()->setProperty(
            "player", QVariant::fromValue<QObject *>(&stub)));
        const auto menuVisible = [&ui] {
            QObject *menu = ui.popup(QStringLiteral("cardMenu"));
            return menu && menu->property("visible").toBool();
        };
        // The Menu key opens the menu of the current card of the focused row: no pointer involved
        QQuickItem *list = nullptr;
        QTRY_VERIFY((list = ui.item(QStringLiteral("homeRowList_0"))) != nullptr);
        // The list is in the scene while it is still being built: its cards come last
        QTRY_VERIFY(list->property("currentItem").value<QObject *>() != nullptr);
        list->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(list->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menuVisible());
        QQuickItem *entry = nullptr;
        QTRY_VERIFY((entry = ui.item(QStringLiteral("cardMenuResume"))) != nullptr);
        QTRY_VERIFY(entry->isVisible() && entry->isEnabled());
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Resume"));
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_COMPARE(stub.calls.size(), 1);
        QCOMPARE(stub.calls.constFirst().id, kMovieId);
        QCOMPARE(stub.calls.constFirst().resumeTicks, qint64(2340000000));
        QTRY_VERIFY(!menuVisible());
        // The Next Up row has no such entry
        QQuickItem *nextUp = nullptr;
        QTRY_VERIFY((nextUp = ui.item(QStringLiteral("homeRowList_1"))) != nullptr);
        QTRY_VERIFY(nextUp->property("currentItem").value<QObject *>() != nullptr);
        nextUp->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(nextUp->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Menu);
        QTRY_VERIFY(menuVisible());
        QQuickItem *playNow = nullptr;
        QTRY_VERIFY((playNow = ui.item(QStringLiteral("cardMenuPlayNow"))) != nullptr);
        QTRY_VERIFY(playNow->isVisible());
        QQuickItem *resume = ui.item(QStringLiteral("cardMenuResume"));
        QVERIFY(!resume || !resume->isVisible());
        QCOMPARE(stub.calls.size(), 1);
    }

    void playerVideoFillsTheWindowUnderFloatingBars()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        // No toolbar strip and no shell header: the video takes the whole window, the bars lie on it at the top and the bottom
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QVERIFY(video);
        const QRectF window(QPointF(0, 0), QSizeF(ui.window->size()));
        QTRY_COMPARE(video->mapRectToScene(video->boundingRect()), window);
        QQuickItem *topBar = ui.item(QStringLiteral("playerTopBar"));
        QQuickItem *bottomBar = ui.item(QStringLiteral("playerBottomBar"));
        QVERIFY(topBar && topBar->isVisible());
        QVERIFY(bottomBar && bottomBar->isVisible());
        QCOMPARE(topBar->mapRectToScene(topBar->boundingRect()).top(), 0.0);
        QCOMPARE(bottomBar->mapRectToScene(bottomBar->boundingRect()).bottom(), window.bottom());
        // The title once, in the top bar, with the numbered episode under it, both plain text
        QQuickItem *title = Ui::itemIn(topBar, QStringLiteral("playerTitle"));
        QQuickItem *subtitle = Ui::itemIn(topBar, QStringLiteral("playerSubtitle"));
        QVERIFY(title && subtitle);
        QCOMPARE(title->property("text").toString(), stub.title);
        QCOMPARE(subtitle->property("text").toString(), stub.subtitle);
        QVERIFY(showsPlainText(title) && showsPlainText(subtitle));
        int titles = 0;
        for (const QQuickItem *item : allItems(page))
            titles += item->isVisible() && item->property("text").toString() == stub.title;
        QCOMPARE(titles, 1);
    }

    void playerBackLeavesItsFullScreenFirst_data()
    {
        QTest::addColumn<QString>("way");
        QTest::newRow("escape") << QStringLiteral("escape");
        QTest::newRow("alt-left") << QStringLiteral("alt-left");
        QTest::newRow("mouse back") << QStringLiteral("mouse back");
    }

    void playerBackLeavesItsFullScreenFirst()
    {
        QFETCH(QString, way);
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QCOMPARE(ui.window->visibility(), QWindow::Windowed);
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        const auto back = [&] {
            if (way == QLatin1String("escape"))
                QTest::keyClick(ui.window, Qt::Key_Escape);
            else if (way == QLatin1String("alt-left"))
                QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
            else
                QTest::mouseClick(ui.window, Qt::BackButton, Qt::NoModifier,
                                  QPoint(ui.window->width() / 2, ui.window->height() / 2));
        };
        back();
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);
        QTest::qWait(100);
        QCOMPARE(ui.currentDetails(), page);
        back();
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.window->visibility(), QWindow::Windowed);
    }

    void playerBackButtonLeavesAndGivesTheWindowBack()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QQuickItem *back = ui.item(QStringLiteral("playerBackButton"));
        QVERIFY(back && back->isVisible());
        // AbstractButton.IconOnly
        QCOMPARE(back->property("display").toInt(), 0);
        QCOMPARE(back->property("text").toString(), QStringLiteral("Back"));
        QTest::mouseClick(
            ui.window, Qt::LeftButton, Qt::NoModifier,
            back->mapToScene(QPointF(back->width() / 2, back->height() / 2)).toPoint());
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        // Back never stops: the playback goes on in the mini player
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
        QTRY_VERIFY(shownMiniPlayer(ui));
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);
    }

    void playerLeavesAFullScreenItDidNotEnterAlone()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        // Full screen from the setting, the command line or the window manager: Esc leaves the player and the window stays full screen
        ui.window->setVisibility(QWindow::FullScreen);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QVERIFY(openPlayer(ui, stub));
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        QCOMPARE(ui.window->visibility(), QWindow::FullScreen);
        // F in the player leaves it for a window, and leaving the player gives the full screen back
        QVERIFY(openPlayer(ui, stub));
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
    }

    void playerFullScreenGivesBackAMaximizedWindow()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        ui.window->showMaximized();
        QTRY_COMPARE(ui.window->visibility(), QWindow::Maximized);
        QVERIFY(openPlayer(ui, stub));
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::Maximized);
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.window->visibility(), QWindow::Maximized);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QCOMPARE(ui.window->visibility(), QWindow::Maximized);
    }

    void closingInThePlayersFullScreenKeepsTheWindowSize()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        ui.window->resize(700, 500);
        QTRY_COMPARE(ui.window->size(), QSize(700, 500));
        QVERIFY(openPlayer(ui, stub));
        QTest::keyClick(ui.window, Qt::Key_F);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QGuiApplication::setQuitOnLastWindowClosed(false);
        const auto quitAgain =
            qScopeGuard([] { QGuiApplication::setQuitOnLastWindowClosed(true); });
        ui.window->close();
        QTRY_VERIFY(!ui.window->isVisible());
        QCOMPARE(ui.settings.windowGeometry().size(), QSize(700, 500));
    }

    void playerKeysWorkAfterAClickAndWithTheBarsHidden()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QVERIFY(timer);
        QQuickItem *osd = ui.item(QStringLiteral("playerOsd"));
        QVERIFY(osd && osd->isVisible());
        // The bars stay up while paused: the click below must not pause the stub
        stub.pauseChangesNothing = true;
        // A click on a control of the bars leaves the keyboard with the page
        QQuickItem *pause = ui.item(QStringLiteral("playerPauseButton"));
        QVERIFY(pause);
        QTest::mouseClick(
            ui.window, Qt::LeftButton, Qt::NoModifier,
            pause->mapToScene(QPointF(pause->width() / 2, pause->height() / 2)).toPoint());
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        QCOMPARE(ui.window->activeFocusItem(), page);
        // A pointer at rest on the video, off the bars, lets them go (sooner than the five seconds of the app; the change restarts the timer)
        restPointerOnVideo(ui);
        timer->setProperty("interval", 1000);
        QTRY_VERIFY(!osd->isVisible());
        QTest::qWait(500);
        QVERIFY(!osd->isVisible());
        stub.calls.clear();
        for (const Qt::Key key : {Qt::Key_Space, Qt::Key_M, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
                                  Qt::Key_Down, Qt::Key_Minus, Qt::Key_Plus})
            QTest::keyClick(ui.window, key);
        // The stub starts at full volume: minus takes five off, plus puts them back
        QCOMPARE(stub.calls,
                 QStringList({QStringLiteral("togglePause"), QStringLiteral("toggleMute"),
                              QStringLiteral("seekRelative -10"), QStringLiteral("seekRelative 10"),
                              QStringLiteral("seekRelative 60"), QStringLiteral("seekRelative -60"),
                              QStringLiteral("setVolume 95"), QStringLiteral("setVolume 100")}));
        // A key shows the bars again, and so does a move of the pointer once they are gone
        QTRY_VERIFY(osd->isVisible());
        QTRY_VERIFY(!osd->isVisible());
        QTest::mouseMove(ui.window, QPoint(ui.window->width() / 3, ui.window->height() / 3));
        QTRY_VERIFY(osd->isVisible());
        // A combination belongs to the window: Ctrl+M does not mute
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_M, Qt::ControlModifier);
        QTest::qWait(100);
        QVERIFY(stub.calls.isEmpty());
        // Tab with the bars gone shows them and goes into them; once they hide again the page takes the keys back
        QTRY_VERIFY(!osd->isVisible());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(osd->isVisible());
        QQuickItem *focused = ui.window->activeFocusItem();
        QVERIFY(focused && osd->isAncestorOf(focused));
        QTRY_VERIFY(!osd->isVisible());
        QCOMPARE(ui.window->activeFocusItem(), page);
        // A key shows the bars again with the keys still on the page: Space pauses, it does not press the control Tab had reached
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_Space);
        QTRY_VERIFY(osd->isVisible());
        QCOMPARE(ui.window->activeFocusItem(), page);
        QTest::keyClick(ui.window, Qt::Key_Space);
        QCOMPARE(stub.calls,
                 QStringList({QStringLiteral("togglePause"), QStringLiteral("togglePause")}));
    }

    void playerLeavesToTheMiniPlayerByEveryRoute_data()
    {
        QTest::addColumn<QString>("route");
        for (const char *route :
             {"osd back", "escape", "alt-left", "more actions entry", "tab", "find", "remote home"})
            QTest::newRow(route) << QString::fromLatin1(route);
    }

    void playerLeavesToTheMiniPlayerByEveryRoute()
    {
        QFETCH(QString, route);
        PlayerStub stub;
        RemoteStub remote;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(attach(ui, "remoteInput", &remote));
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QQuickItem *mini = ui.item(QStringLiteral("miniPlayer"));
        QVERIFY(video && mini);
        QVERIFY(page->isAncestorOf(video));
        QVERIFY(!mini->isVisible());
        // The video item never leaves its window on the way: not while the page goes, not afterwards
        QSignalSpy windowChanged(video, &QQuickItem::windowChanged);
        QVERIFY(windowChanged.isValid());
        QPointer<QQuickItem> leavingPage(page);
        if (route == QLatin1String("osd back"))
            ui.clickCenter(ui.item(QStringLiteral("playerBackButton")));
        else if (route == QLatin1String("escape"))
            QTest::keyClick(ui.window, Qt::Key_Escape);
        else if (route == QLatin1String("alt-left"))
            QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        else if (route == QLatin1String("more actions entry"))
            QVERIFY(chooseShowMiniPlayer(ui));
        else if (route == QLatin1String("tab")) {
            // The header and its tabs are hidden in the full player: a tab opened by the shell (a link, a remote) clears the stack under it, once the Movies tab exists
            const auto hasMovies = [&ui] {
                QVariant has;
                QMetaObject::invokeMethod(ui.window, "hasTypeTab", Q_RETURN_ARG(QVariant, has),
                                          Q_ARG(QVariant, QStringLiteral("movies")));
                return has.toBool();
            };
            QTRY_VERIFY(hasMovies());
            QVERIFY(QMetaObject::invokeMethod(ui.window, "openTab",
                                              Q_ARG(QVariant, QStringLiteral("movies")),
                                              Q_ARG(QVariant, QVariant())));
        } else if (route == QLatin1String("find"))
            QTest::keySequence(ui.window, QKeySequence::Find);
        else
            Q_EMIT remote.actionRequested(QStringLiteral("home"));
        // The mini player shows, holding the very item the page held, and nothing was stopped
        QTRY_VERIFY(mini->isVisible());
        QTRY_VERIFY(mini->isAncestorOf(video));
        QCOMPARE(ui.item(QStringLiteral("videoRenderer")), video);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("mini"));
        QTRY_VERIFY(ui.currentDetails()->objectName() != QLatin1String("playerPage"));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
        QVERIFY(stub.active);
        if (route == QLatin1String("tab"))
            QTRY_COMPARE(ui.rootTab(), QStringLiteral("movies"));
        else if (route == QLatin1String("find"))
            QTRY_VERIFY(ui.item(QStringLiteral("searchPage")) != nullptr);
        // The page is deleted by now or soon: the item stays in the mini player, in the window
        QTRY_VERIFY(leavingPage.isNull());
        QCOMPARE(ui.item(QStringLiteral("videoRenderer")), video);
        QVERIFY(mini->isAncestorOf(video));
        QCOMPARE(video->window(), ui.window);
        QCOMPARE(windowChanged.count(), 0);
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    void miniPlayerExpandsBackToTheFullPlayer_data()
    {
        QTest::addColumn<QString>("way");
        for (const char *way : {"click", "expand button", "shortcut", "new play"})
            QTest::newRow(way) << QString::fromLatin1(way);
    }

    void miniPlayerExpandsBackToTheFullPlayer()
    {
        QFETCH(QString, way);
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QVERIFY(video && mini->isAncestorOf(video));
        stub.calls.clear();
        if (way == QLatin1String("click")) {
            hoverMiniPlayer(ui, mini);
            QTest::mouseClick(
                ui.window, Qt::LeftButton, Qt::NoModifier,
                mini->mapToScene(QPointF(mini->width() / 2, mini->height() / 2)).toPoint());
        } else if (way == QLatin1String("expand button")) {
            hoverMiniPlayer(ui, mini);
            QQuickItem *expand = ui.item(QStringLiteral("miniPlayerExpandButton"));
            QVERIFY(expand);
            ui.clickCenter(expand);
        } else if (way == QLatin1String("shortcut")) {
            QTest::keyClick(ui.window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        } else {
            QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(), "playItem",
                                              Q_ARG(QVariant, kMovieId), Q_ARG(QVariant, 0)));
        }
        // The same item is inside the new page, and the mini player is gone
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        QTRY_VERIFY(!mini->isVisible());
        QTRY_VERIFY(ui.currentDetails()->isAncestorOf(video));
        QCOMPARE(ui.item(QStringLiteral("videoRenderer")), video);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("full"));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
        QCOMPARE(stub.calls.contains(QStringLiteral("playItem %1").arg(kMovieId)),
                 way == QLatin1String("new play"));
    }

    void miniPlayerCloseStopsAndDestroysTheVideo()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        QVERIFY(ui.item(QStringLiteral("videoRenderer")));
        hoverMiniPlayer(ui, mini);
        QQuickItem *close = ui.item(QStringLiteral("miniPlayerCloseButton"));
        QVERIFY(close);
        QCOMPARE(close->property("text").toString(), QStringLiteral("Close Mini Player"));
        ui.clickCenter(close);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("stop")));
        QTRY_VERIFY(!mini->isVisible());
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("none"));
        // A new play builds a new item inside the full player
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QVERIFY(video && page->isAncestorOf(video));
    }

    void miniPlayerButtonsShowNoTooltip()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        hoverMiniPlayer(ui, mini);
        // The pointer rests on each button past the tooltip delay: no popup names it (the button's text stays its accessible name)
        for (const QString &name :
             {QStringLiteral("miniPlayerCloseButton"), QStringLiteral("miniPlayerExpandButton"),
              QStringLiteral("miniPlayerPauseButton")}) {
            QQuickItem *button = ui.item(name);
            QVERIFY2(button, qPrintable(name));
            QTRY_VERIFY(button->isVisible() && mini->property("controlsShown").toBool());
            const QPoint centre =
                button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
            QTest::mouseMove(ui.window, centre + QPoint(1, 1));
            QTest::mouseMove(ui.window, centre);
            QTRY_VERIFY(button->property("hovered").toBool());
            QTest::qWait(1200);
            const QString text = button->property("text").toString();
            QVERIFY(!text.isEmpty());
            for (QQuickItem *label : ui.itemsWithText(text)) {
                QVERIFY2(!(label->inherits("QQuickText") && label->isVisible()), qPrintable(name));
            }
        }
    }

    void miniPlayerGoesWhenPlaybackEnds()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        stub.active = false;
        Q_EMIT stub.changed();
        Q_EMIT stub.finished();
        QTRY_VERIFY(!mini->isVisible());
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("none"));
        // The end in the full player leaves its page the same way
        QVERIFY(openPlayer(ui, stub));
        stub.active = false;
        Q_EMIT stub.changed();
        Q_EMIT stub.finished();
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
        QVERIFY(!shownMiniPlayer(ui));
    }

    void miniPlayerStaysForTheNextEpisode()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QQuickItem *title = ui.item(QStringLiteral("miniPlayerTitle"));
        QVERIFY(video && title);
        QVERIFY(showsPlainText(title));
        QCOMPARE(title->property("text").toString(), stub.title);
        // The player moves on to the next item: it is still active when the end of the last one is announced
        stub.title = QStringLiteral("The Silent Sea");
        Q_EMIT stub.changed();
        Q_EMIT stub.finished();
        QTRY_COMPARE(title->property("text").toString(), QStringLiteral("The Silent Sea"));
        QTest::qWait(100);
        QVERIFY(mini->isVisible());
        QCOMPARE(ui.item(QStringLiteral("videoRenderer")), video);
        QVERIFY(mini->isAncestorOf(video));
    }

    void miniPlayerGoesOnSignOut()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        ui.controller.signOut();
        QTRY_VERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        QTRY_VERIFY(!mini->isVisible());
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("none"));
    }

    void miniPlayerKeepsThePageFocus()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        // The page takes the keyboard a turn after it comes to the front
        QQuickItem *home = ui.currentDetails();
        QTRY_VERIFY(ui.window->activeFocusItem()
                    && (ui.window->activeFocusItem() == home
                        || home->isAncestorOf(ui.window->activeFocusItem())));
        QQuickItem *before = focusAtRest(ui);
        QVERIFY(before);
        // Hovering shows the controls, and a click on one acts without taking the keyboard
        QQuickItem *scrim = ui.item(QStringLiteral("miniPlayerScrim"));
        QVERIFY(scrim);
        hoverMiniPlayer(ui, mini);
        QTRY_COMPARE(scrim->opacity(), 1.0);
        QQuickItem *pause = ui.item(QStringLiteral("miniPlayerPauseButton"));
        QVERIFY(pause);
        stub.calls.clear();
        ui.clickCenter(pause);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        QCOMPARE(focusAtRest(ui), before);
        // The click acted on the control: it did not expand the player
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("mini"));
        QCOMPARE(ui.currentDetails(), home);
    }

    void miniPlayerTouchShowsTheControlsBeforeActing()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        QQuickItem *scrim = ui.item(QStringLiteral("miniPlayerScrim"));
        QQuickItem *pause = ui.item(QStringLiteral("miniPlayerPauseButton"));
        QObject *timer = mini->findChild<QObject *>(QStringLiteral("miniPlayerRevealTimer"));
        QVERIFY(scrim && pause && timer);
        // The pointer rests away from the dock, so only the touch can show the controls
        const QPoint away(ui.window->width() / 4, ui.window->height() / 2);
        QTest::mouseMove(ui.window, away + QPoint(1, 1));
        QTest::mouseMove(ui.window, away);
        QTRY_COMPARE(scrim->opacity(), 0.0);
        QPointingDevice *touch = QTest::createTouchDevice();
        const auto tap = [&](const QQuickItem *item) {
            const QPoint at =
                item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
            QTest::touchEvent(ui.window, touch).press(0, at);
            QTest::touchEvent(ui.window, touch).release(0, at);
        };
        const auto mode = [&ui] { return ui.window->property("playbackMode").toString(); };
        stub.calls.clear();
        // A tap on the video shows the controls and does nothing else
        timer->setProperty("interval", 60000);
        tap(mini);
        QTRY_COMPARE(scrim->opacity(), 1.0);
        QCOMPARE(mode(), QStringLiteral("mini"));
        QVERIFY(stub.calls.isEmpty());
        // They hide again after a few seconds
        timer->setProperty("interval", 50);
        QTRY_COMPARE(scrim->opacity(), 0.0);
        timer->setProperty("interval", 60000);
        // A tap on a control that is hidden shows the controls and does not press it
        tap(pause);
        QTRY_COMPARE(scrim->opacity(), 1.0);
        QVERIFY(stub.calls.isEmpty());
        // Shown, the same tap presses it, and the player stays a mini player
        tap(pause);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        QCOMPARE(mode(), QStringLiteral("mini"));
        // A second tap on the video expands the player
        tap(mini);
        QTRY_COMPARE(mode(), QStringLiteral("full"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    void miniPlayerGivesWayToAPlaybackThatFails()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        // The item is still opening when the player is left
        stub.loading = true;
        Q_EMIT stub.changed();
        QVERIFY(chooseShowMiniPlayer(ui));
        QTRY_VERIFY(shownMiniPlayer(ui));
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("mini"));
        // It fails: the full player comes back with the error and the way to close it
        stub.loading = false;
        stub.active = false;
        stub.errorText = QStringLiteral("The video could not be opened. Try again.");
        Q_EMIT stub.changed();
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("full"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        QTRY_VERIFY(!shownMiniPlayer(ui));
        QTRY_VERIFY(ui.showsText(stub.errorText));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Close Player")));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    void miniPlayerDocksAboveTheSettingsSaveBar()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        RemotePreferences remote(ui);
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPreferences(ui) != nullptr);
        QQuickItem *bar = nullptr;
        QTRY_VERIFY((bar = ui.item(QStringLiteral("settingsSaveBar"))) != nullptr
                    && bar->isVisible());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QVERIFY(chooseShowMiniPlayer(ui));
        QQuickItem *mini = nullptr;
        QTRY_VERIFY((mini = shownMiniPlayer(ui)) != nullptr);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("settingsPage"));
        // The dock lies above the bar, not over its Discard and Save buttons
        QTRY_VERIFY(sceneRect(mini).bottom() <= sceneRect(bar).top() + 1);
    }

    void miniPlayerDocksInTheTrailingCorner_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("rightToLeft");
        QTest::newRow("wide") << 1000 << false;
        QTest::newRow("compact") << 400 << false;
        QTest::newRow("right to left") << 1000 << true;
    }

    void miniPlayerDocksInTheTrailingCorner()
    {
        QFETCH(int, width);
        QFETCH(bool, rightToLeft);
        const Qt::LayoutDirection direction = QGuiApplication::layoutDirection();
        QGuiApplication::setLayoutDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
        const auto restoreDirection =
            qScopeGuard([direction] { QGuiApplication::setLayoutDirection(direction); });
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(width));
        QVERIFY(ui.signIn());
        QQuickItem *mini = openMiniPlayer(ui, stub);
        QVERIFY(mini);
        const qreal unit = gridUnitOf(ui);
        const qreal margin = mini->property("margin").toReal();
        QVERIFY(margin > 0);
        // 35 % of the window width, between 12 and 20 grid units, 16:9
        const qreal wanted = std::round(std::min(std::max(width * 0.35, unit * 12), unit * 20));
        const auto docked = [&] {
            const QRectF rect = sceneRect(mini);
            const bool corner = rightToLeft ? qAbs(rect.left() - margin) <= 1
                                            : qAbs(rect.right() - (width - margin)) <= 1;
            return corner && qAbs(rect.width() - wanted) <= 1
                   && qAbs(rect.height() - wanted * 9 / 16) <= 1
                   && qAbs(rect.bottom() - (ui.window->height() - margin)) <= 1;
        };
        QTRY_VERIFY2(docked(), qPrintable(QStringLiteral("%1 %2 %3 %4")
                                              .arg(sceneRect(mini).x())
                                              .arg(sceneRect(mini).y())
                                              .arg(sceneRect(mini).width())
                                              .arg(sceneRect(mini).height())));
        // It floats above the page stack and below the startup cover, outside the row the tab motion moves, and under the header
        const auto *stack =
            qobject_cast<QQuickItem *>(ui.window->property("pageStack").value<QObject *>());
        QVERIFY(stack);
        QVERIFY(!stack->isAncestorOf(mini));
        QVERIFY(mini->z() > stack->z());
        QVERIFY(mini->z() < ui.item(QStringLiteral("startupCover"))->z());
        QVERIFY(sceneRect(mini).top() >= ui.shellHeader()->height());
    }

    void remotePlayerCommandsWorkInEveryMode()
    {
        PlayerStub stub;
        RemoteStub remote;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(attach(ui, "remoteInput", &remote));
        QVERIFY(openPlayer(ui, stub));
        const QStringList sequence =
            QStringLiteral("playpause pause pause play play next previous fastforward rewind "
                           "volumedown volumeup togglemute bogus")
                .split(QLatin1Char(' '));
        // Pause and play act only on the state they change; the stub starts at full volume
        const QStringList calls({QStringLiteral("togglePause"), QStringLiteral("resume"),
                                 QStringLiteral("next"), QStringLiteral("previous"),
                                 QStringLiteral("seekRelative 10"),
                                 QStringLiteral("seekRelative -10"), QStringLiteral("setVolume 95"),
                                 QStringLiteral("setVolume 100"), QStringLiteral("toggleMute")});
        stub.calls.clear();
        for (const QString &command : sequence)
            Q_EMIT remote.actionRequested(command);
        QCOMPARE(stub.calls, calls);

        // The same commands reach the player under another page
        QVERIFY(chooseShowMiniPlayer(ui));
        QTRY_VERIFY(shownMiniPlayer(ui));
        stub.calls.clear();
        for (const QString &command : sequence)
            Q_EMIT remote.actionRequested(command);
        QCOMPARE(stub.calls, calls);

        // Full screen needs the full player: the command brings it back
        Q_EMIT remote.actionRequested(QStringLiteral("togglefullscreen"));
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        Q_EMIT remote.actionRequested(QStringLiteral("togglefullscreen"));
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);

        // Stop ends the playback from the mini player
        QVERIFY(chooseShowMiniPlayer(ui));
        QTRY_VERIFY(shownMiniPlayer(ui));
        stub.calls.clear();
        Q_EMIT remote.actionRequested(QStringLiteral("stop"));
        QCOMPARE(stub.calls, QStringList({QStringLiteral("stop")}));
        QTRY_VERIFY(!shownMiniPlayer(ui));
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
    }

    // The stream kept starving: the notification names the bit rate it plays at now, in every mode; a refusal says playback goes on
    void aLoweredQualityIsAnnounced()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        Q_EMIT stub.qualityLowered(10'000'000);
        QTRY_VERIFY(ui.showsText(
            QStringLiteral("Slow connection: playing at a lower quality (10 Mbps)"), false));
        QVERIFY(chooseShowMiniPlayer(ui));
        QTRY_VERIFY(shownMiniPlayer(ui));
        Q_EMIT stub.qualityLowered(1'500'000);
        QTRY_VERIFY(ui.showsText(
            QStringLiteral("Slow connection: playing at a lower quality (1.5 Mbps)"), false));
        Q_EMIT stub.qualityUnavailable();
        QTRY_VERIFY(ui.showsText(
            QStringLiteral("No lower quality available. Playback continues at the current quality"),
            false));
    }

    // A lowered stream that did not open offers Try Again beside Close Player; another error offers Close Player alone
    void theErrorOverlayOffersTryAgainForAFailedFallback()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        stub.active = false;
        stub.errorText = QStringLiteral(
            "The lower quality stream could not be opened. Try again at the previous quality.");
        stub.canRetry = true;
        Q_EMIT stub.changed();
        QQuickItem *retry = Ui::itemIn(page, QStringLiteral("playerRetryButton"));
        QQuickItem *close = Ui::itemIn(page, QStringLiteral("playerCloseAfterErrorButton"));
        QVERIFY(retry && close);
        QTRY_VERIFY(retry->isVisible() && close->isVisible() && retry->width() > 0);
        QCOMPARE(retry->property("text").toString(), QStringLiteral("Try Again"));
        stub.calls.clear();
        // The row lays the two buttons out a frame after they show
        QTRY_VERIFY(!sceneRect(retry).intersects(sceneRect(close)));
        ui.clickCenter(retry);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("retryPlayback")));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
        stub.canRetry = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!retry->isVisible());
        QVERIFY(close->isVisible());
    }

    // One mode button in the row, Picture in Picture before Full Screen; the mini player is the first entry of More Actions, so two look-alike icons never sit side by side
    void playerModeButtonsFitTheNarrowRow()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QVERIFY(ui.item(QStringLiteral("playerMiniButton")) == nullptr);
        QQuickItem *fullScreen = ui.item(QStringLiteral("playerFullScreenButton"));
        QQuickItem *pictureInPicture = ui.item(QStringLiteral("playerPictureInPictureButton"));
        QVERIFY(fullScreen && pictureInPicture);
        QCOMPARE(pictureInPicture->property("text").toString(),
                 QStringLiteral("Picture in Picture"));
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        QQuickItem *first = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, first), Q_ARG(int, 0));
        QVERIFY(first);
        QCOMPARE(first->objectName(), QStringLiteral("playerMoreMiniItem"));
        QCOMPARE(first->property("text").toString(), QStringLiteral("Show Mini Player"));
        QQuickItem *page = ui.currentDetails();
        for (const int width : {1000, 440}) {
            QVERIFY(ui.opensWindowOfWidth(width));
            // The page lays its row out for the new width a turn after the resize: the checks wait for that layout
            QTRY_COMPARE(page->property("narrow").toBool(),
                         width < ui.window->property("wideLimit").toInt());
            // Icon only at every width (AbstractButton.IconOnly), right before Full Screen, and inside the window
            QTRY_VERIFY(pictureInPicture->isVisible() && pictureInPicture->width() > 0);
            QCOMPARE(pictureInPicture->property("display").toInt(), 0);
            const auto fits = [&] {
                const QRectF rect = sceneRect(pictureInPicture);
                return rect.left() >= 0 && rect.right() <= sceneRect(fullScreen).left() + 1
                       && sceneRect(fullScreen).right() <= ui.window->width();
            };
            QTRY_VERIFY(fits());
            QCOMPARE(pictureInPicture->width(), fullScreen->width());
        }
    }

    void pictureInPictureMovesTheVideoToItsWindow()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QVERIFY(video && page->isAncestorOf(video));
        QVERIFY(pictureInPictureWindow(ui) == nullptr);
        QPointer<QQuickItem> leavingPage(page);
        ui.clickCenter(ui.item(QStringLiteral("playerPictureInPictureButton")));
        QQuickWindow *pip = nullptr;
        QTRY_VERIFY((pip = pictureInPictureWindow(ui)) != nullptr);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("pip"));
        // The video is the one item, now in the window of its own, which is not transient
        QTRY_COMPARE(video->window(), pip);
        QQuickItem *slot = pictureInPictureItem(pip, QStringLiteral("pictureInPictureVideoSlot"));
        QVERIFY(slot && slot->isAncestorOf(video));
        QVERIFY(pip != ui.window);
        QVERIFY(pip->transientParent() == nullptr);
        QVERIFY(pip->flags().testFlag(Qt::WindowStaysOnTopHint));
        // No frame: the window has its own X and moves and resizes by itself
        QVERIFY(pip->flags().testFlag(Qt::FramelessWindowHint));
        QTRY_VERIFY(leavingPage.isNull());
        QVERIFY(ui.currentDetails()->objectName() != QLatin1String("playerPage"));
        QVERIFY(ui.item(QStringLiteral("playerPage")) == nullptr);
        QVERIFY(stub.active);
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
        // A click on the video pauses
        stub.calls.clear();
        QTest::mouseClick(pip, Qt::LeftButton, Qt::NoModifier,
                          QPoint(pip->width() / 2, pip->height() / 2));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("togglePause")));
        // Space pauses too
        stub.calls.clear();
        QTest::keyClick(pip, Qt::Key_Space);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("togglePause")));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    void pictureInPictureReturnsToTheFullPlayer_data()
    {
        QTest::addColumn<QString>("way");
        for (const char *way : {"close button", "window close", "double click", "shortcut"})
            QTest::newRow(way) << QString::fromLatin1(way);
    }

    // Closing the window, from its own X or the window manager, gives the video back to the full player, as a double click and the shortcut do: the playback goes on
    void pictureInPictureReturnsToTheFullPlayer()
    {
        QFETCH(QString, way);
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QPointer<QQuickWindow> pipGuard(pip);
        stub.calls.clear();
        QCOMPARE(pictureInPictureItem(pip, QStringLiteral("pictureInPictureCloseButton"))
                     ->property("text")
                     .toString(),
                 QStringLiteral("Close Picture in Picture"));
        if (way == QLatin1String("close button")) {
            QVERIFY(
                clickPictureInPictureControl(pip, QStringLiteral("pictureInPictureCloseButton")));
        } else if (way == QLatin1String("window close")) {
            pip->close();
        } else if (way == QLatin1String("double click")) {
            QTest::mouseDClick(pip, Qt::LeftButton, Qt::NoModifier,
                               QPoint(pip->width() / 2, pip->height() / 4));
        } else {
            pip->requestActivate();
            QTRY_VERIFY(pip->isActive());
            // The press is enough, and the window it returns from may be gone before a release could reach it
            QTest::keyPress(pip, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        }
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("full"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        // The item is back inside the page of the main window, and the picture-in-picture window is gone
        QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QVERIFY(video);
        QTRY_VERIFY(ui.currentDetails()->isAncestorOf(video));
        QCOMPARE(video->window(), ui.window);
        QTRY_VERIFY(pipGuard.isNull());
        QVERIFY(pictureInPictureWindow(ui) == nullptr);
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
        QVERIFY(stub.active);
        // A single click on the video does not return: the double click is the only way
        if (way == QLatin1String("double click"))
            QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));
    }

    // On a touch screen the first tap only shows the controls, on the video and on a hidden control alike, as on the mini player
    void pictureInPictureTouchShowsTheControlsFirst()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QQuickItem *scrim = pictureInPictureItem(pip, QStringLiteral("pictureInPictureScrim"));
        QQuickItem *close =
            pictureInPictureItem(pip, QStringLiteral("pictureInPictureCloseButton"));
        QObject *timer = pip->findChild<QObject *>(QStringLiteral("pictureInPictureRevealTimer"));
        QVERIFY(scrim && close && timer);
        QTRY_COMPARE(scrim->opacity(), 0.0);
        QPointingDevice *touch = QTest::createTouchDevice();
        const auto tap = [&](const QPoint &at) {
            QTest::touchEvent(pip, touch).press(0, at);
            QTest::touchEvent(pip, touch).release(0, at);
        };
        const QPoint onVideo(pip->width() / 2, pip->height() / 4);
        const QPoint onClose =
            close->mapToScene(QPointF(close->width() / 2, close->height() / 2)).toPoint();
        stub.calls.clear();
        timer->setProperty("interval", 60000);
        // A tap on the video shows the controls and neither pauses nor returns
        tap(onVideo);
        QTRY_COMPARE(scrim->opacity(), 1.0);
        QTest::qWait(400);
        QVERIFY(stub.calls.isEmpty());
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("pip"));
        // Hidden again, a tap on the X only shows the controls: the window stays
        timer->setProperty("interval", 50);
        QTRY_COMPARE(scrim->opacity(), 0.0);
        timer->setProperty("interval", 60000);
        tap(onClose);
        QTRY_COMPARE(scrim->opacity(), 1.0);
        QTest::qWait(100);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("pip"));
        // Shown, the same tap closes the window and the full player takes the video back
        tap(onClose);
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("full"));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    // The timeline seeks where a drag lets go, and the time shows where the video is
    void pictureInPictureHasATimeline()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        stub.durationTicks = 600 * 10000000LL;
        stub.positionTicks = 60 * 10000000LL;
        stub.canSeek = true;
        Q_EMIT stub.changed();
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QQuickItem *timeline =
            pictureInPictureItem(pip, QStringLiteral("pictureInPictureTimeline"));
        QQuickItem *time = pictureInPictureItem(pip, QStringLiteral("pictureInPictureTime"));
        QVERIFY(timeline && time);
        QVERIFY(timeline->isEnabled());
        QCOMPARE(timeline->property("value").toDouble(), 60.0);
        QCOMPARE(time->property("text").toString(), QStringLiteral("60 / 600"));
        stub.calls.clear();
        // Over the window, so the controls show, then a click at three quarters of the groove
        QTest::mouseMove(pip, QPoint(pip->width() / 2, pip->height() / 2));
        QQuickItem *scrim = pictureInPictureItem(pip, QStringLiteral("pictureInPictureScrim"));
        QTRY_COMPARE(scrim->opacity(), 1.0);
        const QPoint at =
            timeline->mapToScene(QPointF(timeline->width() * 0.75, timeline->height() / 2))
                .toPoint();
        QTest::mouseClick(pip, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_VERIFY(std::any_of(stub.calls.cbegin(), stub.calls.cend(), [](const QString &call) {
            return call.startsWith(QLatin1String("seek "));
        }));
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("pip"));
    }

    // A playback that fails in its own window comes back to the full player with its error, as from the mini player
    void pictureInPictureFailureReturnsToTheFullPlayer()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QPointer<QQuickWindow> pipGuard(pip);
        stub.active = false;
        stub.errorText = QStringLiteral("The video could not be opened. Try again.");
        Q_EMIT stub.changed();
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("full"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playerPage"));
        QTRY_VERIFY(ui.showsText(stub.errorText));
        QTRY_VERIFY(pipGuard.isNull());
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    // The window that could not create a render context gives the video back to the full player, playing on
    void pictureInPictureRendererFailureReturnsToTheFullPlayer()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QQuickItem *video = pictureInPictureItem(pip, QStringLiteral("videoRenderer"));
        QVERIFY(video);
        QVERIFY(QMetaObject::invokeMethod(video, "rendererFailed"));
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("full"));
        QTRY_COMPARE(video->window(), ui.window);
        QVERIFY(stub.active);
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    void pictureInPictureGoesOnSignOut()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QPointer<QQuickWindow> pipGuard(pip);
        ui.controller.signOut();
        QTRY_VERIFY(ui.item(QStringLiteral("serversPage")) != nullptr);
        QTRY_VERIFY(pipGuard.isNull());
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
        QCOMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("none"));
    }

    // Back to the full player and straight into picture in picture again, before the window of the first visit is gone: the video ends in a window that stays
    void pictureInPictureCanBeEnteredAgainAtOnce()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QVERIFY(QMetaObject::invokeMethod(ui.window, "expandPlayer"));
        QVERIFY(QMetaObject::invokeMethod(ui.window, "enterPictureInPicture"));
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("pip"));
        // A few turns later the window is still there and holds the video
        QTest::qWait(200);
        QQuickWindow *now = pictureInPictureWindow(ui);
        QVERIFY(now && now->isVisible());
        QVERIFY(pictureInPictureItem(now, QStringLiteral("videoRenderer")));
        QVERIFY(!stub.calls.contains(QStringLiteral("stop")));
    }

    // Offscreen there is no Wayland compositor: the picture-in-picture surface is not offered and the window stays an ordinary frameless one
    void thePictureInPictureSurfaceIsUnavailableOffscreen()
    {
        Kinefin::PipSurface surface;
        QVERIFY(!surface.available());
        QVERIFY(!surface.assign(nullptr));
        QWindow window;
        QVERIFY(!surface.assign(&window));
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip && pip->isVisible());
        QVERIFY(pip->width() >= pip->minimumWidth());
        QCOMPARE(pip->height(), qRound(pip->width() * 9.0 / 16.0));
    }

    // A stop from elsewhere (the remote, MPRIS) ends the playback and takes the window away
    void pictureInPictureGoesWithARemoteStop()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QPointer<QQuickWindow> pipGuard(pip);
        QVERIFY(QMetaObject::invokeMethod(ui.window, "playerAction",
                                          Q_ARG(QVariant, QStringLiteral("stop"))));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("stop")));
        QTRY_COMPARE(ui.window->property("playbackMode").toString(), QStringLiteral("none"));
        QTRY_VERIFY(pipGuard.isNull());
        QTRY_VERIFY(ui.item(QStringLiteral("videoRenderer")) == nullptr);
    }

    void closingTheMainWindowClosesPictureInPicture()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickWindow *pip = openPictureInPicture(ui, stub);
        QVERIFY(pip);
        QPointer<QQuickWindow> pipGuard(pip);
        ui.window->close();
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("stop")));
        QTRY_VERIFY(pipGuard.isNull());
    }

    void homeLibrariesFollowTheSetting()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *heading = nullptr;
        QTRY_VERIFY((heading = ui.item(QStringLiteral("homeLibrariesHeading"))) != nullptr
                    && heading->isVisible());
        QQuickItem *tiles = ui.item(QStringLiteral("homeLibraries"));
        QVERIFY(tiles && tiles->isVisible());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        // Off hides the heading and the tiles at once, the rows stay; on brings them back
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("home.libraries"),
                                     false));
        QTRY_VERIFY(!tiles->isVisible());
        QVERIFY(!heading->isVisible());
        QVERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("home.libraries"),
                                     true));
        QTRY_VERIFY(tiles->isVisible() && heading->isVisible());
    }

    void libraryTilesShowTheNameOnlyWithoutAnImage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        const auto name = [&ui](const QString &id) {
            QQuickItem *tile = ui.item(QStringLiteral("libraryCard_") + id);
            return tile ? Ui::itemIn(tile, QStringLiteral("libraryTileName")) : nullptr;
        };
        // Movies has an image, which carries its name: once it is there the tile shows no text of its own
        QTRY_VERIFY(name(kMovies) && name(kSeries));
        QTRY_VERIFY(!name(kMovies)->isVisible());
        QVERIFY(name(kSeries)->isVisible());
        QCOMPARE(name(kSeries)->property("text").toString(), QStringLiteral("Series"));
        QVERIFY(showsPlainText(name(kSeries)));
        // The image is asked wide enough for a tile on a HiDPI screen
        QString moviesImage;
        for (const QVariant &library : ui.controller.property("libraries").toList()) {
            if (library.toMap().value(QStringLiteral("id")).toString() == kMovies)
                moviesImage = library.toMap().value(QStringLiteral("imageUrl")).toString();
        }
        QVERIFY2(moviesImage.contains(QStringLiteral("maxWidth=960")), qPrintable(moviesImage));
        // A fixed width, the same for every tile, from the left edge of the heading
        QQuickItem *movies = ui.item(QStringLiteral("libraryCard_") + kMovies);
        QQuickItem *series = ui.item(QStringLiteral("libraryCard_") + kSeries);
        QQuickItem *heading = ui.item(QStringLiteral("homeLibrariesHeading"));
        QCOMPARE(movies->width(), gridUnitOf(ui) * 16);
        QCOMPARE(series->width(), movies->width());
        QCOMPARE(movies->mapToScene(QPointF(0, 0)).x(), heading->mapToScene(QPointF(0, 0)).x());
        // A narrow window wraps the tiles inside the page, and the first row starts below them
        QVERIFY(ui.opensWindowOfWidth(440));
        QQuickItem *flow = ui.item(QStringLiteral("homeLibraries"));
        QQuickItem *firstRow = nullptr;
        QTRY_VERIFY((firstRow = ui.item(QStringLiteral("homeRow_0"))) != nullptr);
        QTRY_VERIFY(series->mapToScene(QPointF(0, 0)).y() > movies->mapToScene(QPointF(0, 0)).y());
        const QRectF tiles = flow->mapRectToScene(QRectF(0, 0, flow->width(), flow->height()));
        for (const QQuickItem *tile : {movies, series}) {
            const QRectF rect = tile->mapRectToScene(QRectF(0, 0, tile->width(), tile->height()));
            QVERIFY(rect.right() <= ui.window->width());
            QVERIFY(tiles.contains(rect));
        }
        QTRY_VERIFY(firstRow->mapToScene(QPointF(0, 0)).y() >= tiles.bottom());
    }

    void settingsContentKeepsItsPlaceWhenTheScrollBarShows_data()
    {
        QTest::addColumn<bool>("rightToLeft");
        QTest::newRow("left to right") << false;
        QTest::newRow("right to left") << true;
    }

    void settingsContentKeepsItsPlaceWhenTheScrollBarShows()
    {
        QFETCH(bool, rightToLeft);
        const Qt::LayoutDirection direction = QGuiApplication::layoutDirection();
        QGuiApplication::setLayoutDirection(rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
        const auto restoreDirection =
            qScopeGuard([direction] { QGuiApplication::setLayoutDirection(direction); });
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        ui.window->resize(1000, 800);
        QTRY_COMPARE(ui.window->height(), 800);
        QQuickItem *page = openSettings(ui);
        QVERIFY(page);
        QQuickItem *view = ui.item(QStringLiteral("settingsContentView"));
        QVERIFY(view);
        auto *flickable = view->property("contentItem").value<QQuickItem *>();
        QVERIFY(flickable);
        const auto scrolls = [flickable] {
            return flickable->property("contentHeight").toReal() > flickable->height() + 1;
        };
        const auto cardEdges = [&ui] {
            QQuickItem *card = ui.item(QStringLiteral("settingsGroupCard_0"));
            return card ? card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                        : QRectF();
        };
        const auto select = [page](const QString &section) {
            return QMetaObject::invokeMethod(page, "selectSection", Q_ARG(QVariant, section));
        };
        QVERIFY(select(QStringLiteral("main")));
        QTRY_VERIFY(cardEdges().width() > 0);
        QTest::qWait(300);
        QVERIFY(!scrolls());
        const QRectF without = cardEdges();
        QVERIFY(select(QStringLiteral("video")));
        QTRY_VERIFY(scrolls());
        // The bar shows a frame after the content grows; the card keeps its edges all the same
        QTest::qWait(300);
        QTRY_COMPARE(qRound(cardEdges().left()), qRound(without.left()));
        QCOMPARE(qRound(cardEdges().right()), qRound(without.right()));
    }

    void theHomeFavoritesRowOpensTheFavoritesPage()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("home.favorites"),
                                     true));
        // Right after Next Up, before Recently Added
        QQuickItem *row = nullptr;
        QTRY_VERIFY((row = ui.item(QStringLiteral("homeRow_2"))) != nullptr && row->isVisible());
        QCOMPARE(row->property("title").toString(), QStringLiteral("Favorites"));
        auto *rows = ui.controller.homeRows();
        QCOMPARE(rows->data(rows->index(1), Kinefin::HomeRowsModel::KindRole).toInt(),
                 int(Kinefin::HomeRowsModel::NextUp));
        QCOMPARE(rows->data(rows->index(2), Kinefin::HomeRowsModel::KindRole).toInt(),
                 int(Kinefin::HomeRowsModel::Favorites));
        QCOMPARE(rows->data(rows->index(3), Kinefin::HomeRowsModel::KindRole).toInt(),
                 int(Kinefin::HomeRowsModel::RecentlyAdded));
        QQuickItem *button = nullptr;
        QTRY_VERIFY((button = ui.item(QStringLiteral("homeRowSeeAll_2"))) != nullptr
                    && button->isVisible());
        QCOMPARE(button->property("text").toString(), QStringLiteral("See All"));
        QVERIFY(QMetaObject::invokeMethod(button, "click"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        // The Favorites tab, not a page above Home
        QCOMPARE(ui.stackDepth(), 1);
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("favorites"));
    }

    void theHomeFavoritesSettingAddsAndRemovesTheRowAtOnce()
    {
        Ui ui(m_home->path());
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QTRY_VERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QTRY_VERIFY(ui.showsText(QStringLiteral("Recently Added in Series")));
        const auto rowRequests = [&ui] {
            return std::count_if(
                ui.server.itemsRoutes.cbegin(), ui.server.itemsRoutes.cend(),
                [](const QByteArray &route) { return route.contains("sortBy=Random"); });
        };
        // Off: no Favorites row and no request for it
        QCOMPARE(ui.controller.homeRows()->rowCount(), 4);
        QCOMPARE(rowRequests(), 0);
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("home.favorites"),
                                     true));
        QTRY_VERIFY(ui.item(QStringLiteral("homeRow_2")) != nullptr
                    && ui.item(QStringLiteral("homeRow_2"))->property("title").toString()
                           == QStringLiteral("Favorites"));
        QTRY_COMPARE(rowRequests(), 1);
        QCOMPARE(ui.controller.homeRows()->rowCount(), 5);
        // The other rows stay
        QVERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QVERIFY(ui.showsText(QStringLiteral("Next Up"), false));
        QVERIFY(ui.showsText(QStringLiteral("Recently Added in Series")));
        QVERIFY(ui.settings.setValue(QStringLiteral("library"), QStringLiteral("home.favorites"),
                                     false));
        QTRY_COMPARE(ui.controller.homeRows()->rowCount(), 4);
        // The slot of the row now holds the first Recently Added row (the Favorites tab of the header has the same name)
        QTRY_VERIFY(ui.item(QStringLiteral("homeRow_2")) != nullptr
                    && ui.item(QStringLiteral("homeRow_2"))->property("title").toString()
                           == QStringLiteral("Recently Added in Movies"));
        QVERIFY(ui.showsText(QStringLiteral("Continue Watching")));
        QVERIFY(ui.showsText(QStringLiteral("Recently Added in Series")));
        // Off again: nothing more was asked
        QCOMPARE(rowRequests(), 1);
    }

    void playerPreviousAndNextShowOnlyWithSomewhereToGo()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *previous = ui.item(QStringLiteral("playerPreviousButton"));
        QQuickItem *next = ui.item(QStringLiteral("playerNextButton"));
        QVERIFY(previous && next);
        QVERIFY(!previous->isVisible() && !next->isVisible());
        stub.hasNext = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(next->isVisible());
        QVERIFY(!previous->isVisible());
        ui.clickCenter(next);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("next")));
        stub.hasPrevious = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(previous->isVisible());
        // No Stop in the row: Back stops
        for (const QQuickItem *item : allItems(ui.item(QStringLiteral("playerControlRow"))))
            QVERIFY(item->property("text").toString() != QLatin1String("Stop"));
    }

    void playerMoreMenuHoldsShuffleRepeatAndSpeed()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(menu->property("opened").toBool());
        // The bars stay while a menu is open
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QVERIFY(timer);
        timer->setProperty("interval", 300);
        QTest::qWait(800);
        QVERIFY(ui.item(QStringLiteral("playerOsd"))->isVisible());
        // Shuffle only for a queue: a series started from an episode plays in order
        QObject *shuffle = menu->findChild<QObject *>(QStringLiteral("playerShuffleItem"));
        QVERIFY(shuffle);
        QVERIFY(!shuffle->property("enabled").toBool());
        // Repeat stays on for an item played alone: it can repeat itself
        QObject *repeat = menu->findChild<QObject *>(QStringLiteral("playerRepeatMenu"));
        QVERIFY(repeat);
        const auto repeatEntry = [menu, repeat]() -> QQuickItem * {
            for (int i = 0; i < menu->property("count").toInt(); ++i) {
                QQuickItem *entry = nullptr;
                QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, entry),
                                          Q_ARG(int, i));
                if (entry && entry->property("subMenu").value<QObject *>() == repeat)
                    return entry;
            }
            return nullptr;
        };
        QVERIFY(repeatEntry());
        QVERIFY(repeatEntry()->isEnabled());
        stub.queueCount = 3;
        Q_EMIT stub.changed();
        QTRY_VERIFY(shuffle->property("enabled").toBool());
        QVERIFY(repeatEntry()->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(shuffle, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setShuffle true")));
        QObject *repeatAll = menu->findChild<QObject *>(QStringLiteral("playerRepeatAll"));
        QVERIFY(repeatAll);
        QVERIFY(QMetaObject::invokeMethod(repeatAll, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setRepeatMode 1")));
        QObject *speed = menu->findChild<QObject *>(QStringLiteral("playerSpeedMenu"));
        QVERIFY(speed);
        QTRY_COMPARE(speed->property("count").toInt(), 8);
        // Closing the menu lets the bars go again, once the pointer leaves them for the video
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        restPointerOnVideo(ui);
        QTRY_VERIFY(!ui.item(QStringLiteral("playerOsd"))->isVisible());
    }

    void playerSleepMenuOffersTheEndModes()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *sleep = page->findChild<QObject *>(QStringLiteral("playerSleepMenu"));
        QVERIFY(sleep);
        const auto entry = [sleep](const QString &text) -> QQuickItem * {
            for (int i = 0; i < sleep->property("count").toInt(); ++i) {
                QQuickItem *item = nullptr;
                QMetaObject::invokeMethod(sleep, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                                          Q_ARG(int, i));
                if (item && item->property("text").toString() == text)
                    return item;
            }
            return nullptr;
        };
        QQuickItem *endOfItem = entry(QStringLiteral("End of Current Item"));
        QQuickItem *endOfQueue = entry(QStringLiteral("End of Queue"));
        QQuickItem *off = entry(QStringLiteral("Off"));
        QVERIFY(endOfItem && endOfQueue && off);
        QVERIFY(endOfItem->property("checkable").toBool());
        QVERIFY(endOfQueue->property("checkable").toBool());

        QVERIFY(QMetaObject::invokeMethod(endOfItem, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setSleepAtEndOfItem true")));
        QVERIFY(stub.sleepAtEndOfItem);
        QVERIFY(!stub.sleepAtEndOfQueue);
        QTRY_COMPARE(sleep->property("title").toString(),
                     QStringLiteral("Sleep: End of Current Item"));

        QVERIFY(QMetaObject::invokeMethod(endOfQueue, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setSleepAtEndOfQueue true")));
        QVERIFY(stub.sleepAtEndOfQueue);
        QVERIFY(!stub.sleepAtEndOfItem);
        QTRY_COMPARE(sleep->property("title").toString(), QStringLiteral("Sleep: End of Queue"));

        QVERIFY(QMetaObject::invokeMethod(off, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setSleepTimer 0")));
        QVERIFY(!stub.sleepAtEndOfQueue);
        QTRY_COMPARE(sleep->property("title").toString(), QStringLiteral("Sleep Timer"));
    }

    void playerTimelineRowHoldsTheElapsedTimeTheSliderAndTheTotalOrRemainingTime()
    {
        PlayerStub stub;
        stub.positionTicks = 21500000000;
        stub.durationTicks = 72000000000;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *row = ui.item(QStringLiteral("playerTimelineRow"));
        QQuickItem *elapsed = ui.item(QStringLiteral("playerElapsedLabel"));
        QQuickItem *timeline = ui.item(QStringLiteral("playbackTimeline"));
        QQuickItem *time = ui.item(QStringLiteral("playerTimeButton"));
        QVERIFY(row && elapsed && timeline && time);
        // The elapsed time at the start of the row, then the slider, then the button; the control row has no time button any more
        QCOMPARE(elapsed->parentItem(), row);
        QCOMPARE(timeline->parentItem(), row);
        QCOMPARE(time->parentItem(), row);
        QTRY_VERIFY(elapsed->x() + elapsed->width() <= timeline->x());
        QVERIFY(timeline->x() + timeline->width() <= time->x());
        QVERIFY(!Ui::itemIn(ui.item(QStringLiteral("playerControlRow")),
                            QStringLiteral("playerTimeButton")));
        // The left time is a label, not a control; the right one is a button
        QVERIFY(!elapsed->inherits("QQuickAbstractButton"));
        QVERIFY(time->inherits("QQuickAbstractButton"));
        // The stub writes the seconds: 2150 elapsed of 7200
        QCOMPARE(elapsed->property("text").toString(), QStringLiteral("2150"));
        QCOMPARE(time->property("text").toString(), QStringLiteral("7200"));
        // The button names what it shows, without the minus sign of the remaining time
        QCOMPARE(accessibleNameOf(time), QStringLiteral("Total time 7200"));
        // Clicking the elapsed time does nothing; the button switches to the remaining time and back, and neither frame moves while the numbers change, here to fewer digits
        QVERIFY(elapsed->width() >= elapsed->implicitWidth());
        QVERIFY(time->width() >= time->implicitWidth());
        const qreal elapsedWidth = elapsed->width();
        const qreal timeWidth = time->width();
        const qreal sliderX = timeline->x();
        ui.clickCenter(elapsed);
        QTest::qWait(100);
        QCOMPARE(time->property("text").toString(), QStringLiteral("7200"));
        ui.clickCenter(time);
        QTRY_COMPARE(time->property("text").toString(), QStringLiteral("\u22125050"));
        QCOMPARE(accessibleNameOf(time), QStringLiteral("Remaining time 5050"));
        QCOMPARE(elapsed->property("text").toString(), QStringLiteral("2150"));
        QCOMPARE(time->width(), timeWidth);
        QVERIFY(time->width() >= time->implicitWidth());
        stub.positionTicks = 50000000;
        Q_EMIT stub.changed();
        QTRY_COMPARE(elapsed->property("text").toString(), QStringLiteral("5"));
        QTRY_COMPARE(time->property("text").toString(), QStringLiteral("\u22127195"));
        // The layout pass that holds the width runs before the next frame is drawn, not at the text change
        QTRY_COMPARE(elapsed->width(), elapsedWidth);
        QCOMPARE(time->width(), timeWidth);
        QCOMPARE(timeline->x(), sliderX);
        ui.clickCenter(time);
        QTRY_COMPARE(time->property("text").toString(), QStringLiteral("7200"));
        // A narrow row keeps both: the elapsed time never turns into the remaining one
        stub.positionTicks = 21500000000;
        Q_EMIT stub.changed();
        QVERIFY(ui.opensWindowOfWidth(440));
        QTRY_COMPARE(elapsed->property("text").toString(), QStringLiteral("2150"));
        QCOMPARE(time->property("text").toString(), QStringLiteral("7200"));
        // The row is laid out again for the narrow window before the button is clicked where it now is
        QTRY_VERIFY(time->mapToScene(QPointF(time->width(), 0)).x() <= ui.window->width());
        ui.clickCenter(time);
        QTRY_COMPARE(time->property("text").toString(), QStringLiteral("\u22125050"));
        QCOMPARE(elapsed->property("text").toString(), QStringLiteral("2150"));
        // An unknown length has no total to show
        stub.durationTicks = 0;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!time->isVisible());
        QVERIFY(elapsed->isVisible());
    }

    void playerEndsAtFollowsThePositionTheRateAndTheClock()
    {
        PlayerStub stub;
        stub.positionTicks = 0;
        stub.durationTicks = 72000000000;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *ends = ui.item(QStringLiteral("playerEndsAtLabel"));
        QQuickItem *audio = ui.item(QStringLiteral("playerAudioButton"));
        QVERIFY(ends && audio);
        // The clock is fixed at 20:00:40, a Sunday evening, so the minute of the answer does not depend on when the test runs; the end time rounds to the nearest minute
        QVERIFY(page->setProperty("fixedNow", QDateTime(QDate(2026, 11, 15), QTime(20, 0, 40))));
        const auto clockTime = [](int hour, int minute) {
            return QLocale().toString(QTime(hour, minute), QLocale::ShortFormat);
        };
        // Two hours left at normal speed: 22:00:40 shows as 22:01
        QTRY_COMPARE(ends->property("text").toString(),
                     QStringLiteral("Ends at %1").arg(clockTime(22, 1)));
        QVERIFY(ends->isVisible());
        QVERIFY(showsPlainText(ends));
        // At the right of the row, before the Audio button, which keeps its place at the end
        QVERIFY(ends->x() < audio->x());
        QVERIFY(ends->parentItem() == audio->parentItem());
        // A seek to the middle and then twice the speed each move it
        stub.positionTicks = 36000000000;
        Q_EMIT stub.changed();
        QTRY_COMPARE(ends->property("text").toString(),
                     QStringLiteral("Ends at %1").arg(clockTime(21, 1)));
        stub.rate = 2;
        Q_EMIT stub.changed();
        QTRY_COMPARE(ends->property("text").toString(),
                     QStringLiteral("Ends at %1").arg(clockTime(20, 31)));
        // Under half a minute past the minute rounds down
        QVERIFY(page->setProperty("fixedNow", QDateTime(QDate(2026, 11, 15), QTime(20, 0, 10))));
        QTRY_COMPARE(ends->property("text").toString(),
                     QStringLiteral("Ends at %1").arg(clockTime(20, 30)));
        // Nothing left, or no known length: no label
        stub.positionTicks = stub.durationTicks;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!ends->isVisible());
        QCOMPARE(ends->property("text").toString(), QString());
        stub.positionTicks = 0;
        stub.durationTicks = 0;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!ends->isVisible());
        QCOMPARE(ends->property("text").toString(), QString());
        // The clock moves on at least every minute while nothing else changes, as in a pause
        QObject *clock = page->findChild<QObject *>(QStringLiteral("playerClockTimer"));
        QVERIFY(clock);
        QVERIFY(clock->property("running").toBool());
        QVERIFY(clock->property("repeat").toBool());
        QVERIFY(clock->property("interval").toInt() <= 60000);
    }

    void playerAspectRatioMenuFollowsAndSetsTheMode()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *aspect = menu->findChild<QObject *>(QStringLiteral("playerAspectMenu"));
        QVERIFY(aspect);
        QCOMPARE(aspect->property("title").toString(), QStringLiteral("Aspect Ratio"));
        QCOMPARE(aspect->property("count").toInt(), 7);
        const QList<std::pair<QString, QString>> entries = {
            {QStringLiteral("normal"), QStringLiteral("Auto")},
            {QStringLiteral("zoom"), QStringLiteral("Zoom")},
            {QStringLiteral("stretch"), QStringLiteral("Stretch")},
            {QStringLiteral("force_4_3"), QStringLiteral("4:3")},
            {QStringLiteral("force_16_9"), QStringLiteral("16:9")},
            {QStringLiteral("force_16_9_if_4_3"), QStringLiteral("Widescreen for 4:3 Video")},
            {QStringLiteral("noscaling"), QStringLiteral("Original Size")}};
        const auto entry = [aspect](const QString &mode) {
            return aspect->findChild<QObject *>(QStringLiteral("playerAspect_") + mode);
        };
        // The entries in the order of the menu; the player's mode is the checked one
        for (int i = 0; i < entries.size(); ++i) {
            QObject *item = entry(entries.at(i).first);
            QVERIFY2(item, qPrintable(entries.at(i).first));
            QCOMPARE(item->property("text").toString(), entries.at(i).second);
            QVERIFY(item->property("checkable").toBool());
            QCOMPARE(item->property("checked").toBool(), i == 0);
            QQuickItem *at = nullptr;
            QVERIFY(QMetaObject::invokeMethod(aspect, "itemAt", Q_RETURN_ARG(QQuickItem *, at),
                                              Q_ARG(int, i)));
            QCOMPARE(static_cast<QObject *>(at), item);
        }
        // Choosing one sets the mode on the player and the check follows it
        QVERIFY(QMetaObject::invokeMethod(entry(QStringLiteral("zoom")), "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setAspectMode zoom")));
        QTRY_VERIFY(entry(QStringLiteral("zoom"))->property("checked").toBool());
        QVERIFY(!entry(QStringLiteral("normal"))->property("checked").toBool());
        // A mode the player changed by itself shows too
        stub.aspectMode = QStringLiteral("force_16_9_if_4_3");
        Q_EMIT stub.changed();
        QTRY_VERIFY(entry(QStringLiteral("force_16_9_if_4_3"))->property("checked").toBool());
        QVERIFY(!entry(QStringLiteral("zoom"))->property("checked").toBool());
        QCOMPARE(stub.calls.count(QStringLiteral("setAspectMode zoom")), 1);
    }

    void playerChaptersMarkTheTimelineAndFillTheirMenu()
    {
        PlayerStub stub;
        // Of the stub's two hours, chapters at the start, a quarter, a half and three quarters
        stub.chapters = {playerChapter(QStringLiteral("Opening"), 0, QStringLiteral("00:00:00")),
                         playerChapter(QStringLiteral("Heist"), 1800, QStringLiteral("00:30:00")),
                         playerChapter(QStringLiteral("Chase"), 3600, QStringLiteral("01:00:00")),
                         playerChapter(QStringLiteral("Ending"), 5400, QStringLiteral("01:30:00"))};
        stub.currentChapter = 0;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        // The bars stay up for the whole test
        page->findChild<QObject *>(QStringLiteral("playerChromeTimer"))
            ->setProperty("interval", 600000);
        QQuickItem *timeline = ui.item(QStringLiteral("playbackTimeline"));
        QVERIFY(timeline);
        const auto shownMarks = [timeline] {
            QList<QQuickItem *> shown;
            for (QQuickItem *mark : allItems(timeline)) {
                if (mark->objectName() == QLatin1String("playerChapterMark") && mark->isVisible())
                    shown.append(mark);
            }
            std::sort(shown.begin(), shown.end(),
                      [](const QQuickItem *a, const QQuickItem *b) { return a->x() < b->x(); });
            return shown;
        };
        // No mark for the chapter that starts with the video; the others sit where the handle's middle would be at their start, across the groove, thin and out of the accessibility tree
        QTRY_COMPARE(shownMarks().size(), 3);
        const auto *handle = timeline->property("handle").value<QQuickItem *>();
        QVERIFY(handle && handle->width() > 0);
        const qreal start = timeline->property("leftPadding").toReal() + handle->width() / 2;
        const qreal span = timeline->property("availableWidth").toReal() - handle->width();
        const qreal middle = timeline->property("topPadding").toReal()
                             + timeline->property("availableHeight").toReal() / 2;
        for (int i = 0; i < 3; ++i) {
            const QQuickItem *mark = shownMarks().at(i);
            QVERIFY2(qAbs(mark->x() - (start + span * (i + 1) / 4)) <= 1,
                     qPrintable(QString::number(i)));
            QVERIFY(qAbs(mark->y() + mark->height() / 2 - middle) <= 1);
            QCOMPARE(mark->width(), 1.0);
        }
        QQuickItem *marks = shownMarks().constFirst()->parentItem();
        QVERIFY(QQmlProperty(marks, QStringLiteral("Accessible.ignored"), qmlContext(marks))
                    .read()
                    .toBool());
        // The mark under the handle hides while the handle covers it
        stub.positionTicks = 1800 * 10'000'000LL;
        stub.currentChapter = 1;
        Q_EMIT stub.changed();
        QTRY_COMPARE(shownMarks().size(), 2);

        // The preview of a drag names the chapter it is in
        QQuickItem *previewChapter = ui.item(QStringLiteral("playerPreviewChapter"));
        QVERIFY(previewChapter);
        QCOMPARE(previewChapter->property("textFormat").toInt(), int(Qt::PlainText));
        timeline->setProperty("dragSeconds", 3700);
        QTRY_COMPARE(previewChapter->property("text").toString(), QStringLiteral("Chase"));
        timeline->setProperty("dragSeconds", 10);
        QTRY_COMPARE(previewChapter->property("text").toString(), QStringLiteral("Opening"));

        // Chapters follows Aspect Ratio in More Actions: "time — name" entries, the current one checked
        QObject *more = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(more);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(more->property("opened").toBool());
        QObject *menu = more->findChild<QObject *>(QStringLiteral("playerChaptersMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->property("title").toString(), QStringLiteral("Chapters"));
        const auto entryAt = [](QObject *owner, int index) {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(owner, "itemAt", Q_RETURN_ARG(QQuickItem *, item),
                                      Q_ARG(int, index));
            return item;
        };
        const auto submenuEntry = [&](QObject *submenu) {
            for (int i = 0; i < more->property("count").toInt(); ++i) {
                QQuickItem *item = entryAt(more, i);
                if (item && item->property("subMenu").value<QObject *>() == submenu)
                    return i;
            }
            return -1;
        };
        const int chaptersRow = submenuEntry(menu);
        QVERIFY(chaptersRow > 0);
        QVERIFY(chaptersRow
                > submenuEntry(more->findChild<QObject *>(QStringLiteral("playerAspectMenu"))));
        QVERIFY(entryAt(more, chaptersRow)->isEnabled());
        QTRY_COMPARE(menu->property("count").toInt(), 4);
        const QStringList texts{
            QStringLiteral("00:00:00 — Opening"), QStringLiteral("00:30:00 — Heist"),
            QStringLiteral("01:00:00 — Chase"), QStringLiteral("01:30:00 — Ending")};
        for (int i = 0; i < 4; ++i) {
            QQuickItem *entry = entryAt(menu, i);
            QVERIFY(entry);
            QCOMPARE(entry->property("plainText").toString(), texts.at(i));
            QCOMPARE(QQmlProperty(entry, QStringLiteral("Accessible.name"), qmlContext(entry))
                         .read()
                         .toString(),
                     texts.at(i));
            QVERIFY(entry->property("checkable").toBool());
            QCOMPARE(entry->property("checked").toBool(), i == 1);
        }
        QVERIFY(QMetaObject::invokeMethod(entryAt(menu, 2), "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("jumpToChapter 2")));
        QVERIFY(QMetaObject::invokeMethod(more, "close"));
        QTRY_VERIFY(!more->property("visible").toBool());

        // Page Up and Page Down step through the chapters, from the page and from the timeline
        QTRY_COMPARE(ui.window->activeFocusItem(), page);
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_PageUp);
        QTest::keyClick(ui.window, Qt::Key_PageDown);
        timeline->forceActiveFocus();
        QTest::keyClick(ui.window, Qt::Key_PageUp);
        QCOMPARE(stub.calls,
                 QStringList({QStringLiteral("stepChapter 1"), QStringLiteral("stepChapter -1"),
                              QStringLiteral("stepChapter 1")}));

        // Without chapters the entry stays, disabled, and the timeline has no marks
        stub.chapters.clear();
        Q_EMIT stub.chaptersChanged();
        QTRY_COMPARE(shownMarks().size(), 0);
        QVERIFY(!menu->property("enabled").toBool());
        QCOMPARE(menu->property("count").toInt(), 0);
        QVERIFY(!entryAt(more, chaptersRow)->isEnabled());
    }

    void playerMenusShowNamesFromTheServerAndTheFileAsPlainText()
    {
        PlayerStub stub;
        const QString hostile =
            QStringLiteral("<b>Dub</b> & <img src=\"http://evil.invalid/x.png\">");
        stub.audioTracks = {QVariantMap{{QStringLiteral("id"), 1},
                                        {QStringLiteral("name"), hostile},
                                        {QStringLiteral("selected"), true}}};
        stub.subtitleTracks = {QVariantMap{{QStringLiteral("id"), 2},
                                           {QStringLiteral("name"), hostile},
                                           {QStringLiteral("selected"), false}}};
        stub.externalSubtitles = {
            QVariantMap{{QStringLiteral("index"), 3}, {QStringLiteral("name"), hostile}}};
        // The drag preview, hidden, names the chapter at the start: the other one carries the markup
        stub.chapters = {playerChapter(QStringLiteral("One"), 0, QStringLiteral("00:00:00")),
                         playerChapter(hostile, 600, QStringLiteral("00:10:00"))};
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        // The name is never an entry's own text, which the style's label reads as markup and mnemonics: a label shows it in plain text, and screen readers get it as the entry's accessible name
        for (const auto &[name, labels] : {std::pair{QStringLiteral("playerAudioMenu"), 1},
                                           std::pair{QStringLiteral("playerSubtitlesMenu"), 2},
                                           std::pair{QStringLiteral("playerChaptersMenu"), 1}}) {
            QObject *menu = ui.popup(name);
            QVERIFY2(menu, qPrintable(name));
            // Chapters opens from its entry in More Actions
            QObject *more = ui.popup(QStringLiteral("playerMoreMenu"));
            if (name == QLatin1String("playerChaptersMenu")) {
                QVERIFY(QMetaObject::invokeMethod(more, "open"));
                QTRY_VERIFY(more->property("opened").toBool());
                for (int i = 0; i < more->property("count").toInt(); ++i) {
                    QQuickItem *entry = nullptr;
                    QMetaObject::invokeMethod(more, "itemAt", Q_RETURN_ARG(QQuickItem *, entry),
                                              Q_ARG(int, i));
                    if (entry && entry->property("subMenu").value<QObject *>() == menu)
                        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
                }
            } else {
                QVERIFY(QMetaObject::invokeMethod(menu, "open"));
            }
            QTRY_VERIFY2(menu->property("opened").toBool(), qPrintable(name));
            int found = 0;
            const QStringList problems = ui.notPlainText(hostile, &found);
            QVERIFY2(problems.isEmpty(),
                     qPrintable(name + QLatin1String(": ") + problems.join(QLatin1String(", "))));
            QCOMPARE(found, labels);
            for (QQuickItem *holder : ui.itemsWithText(hostile, false))
                QVERIFY2(holder->inherits("QQuickText"), holder->metaObject()->className());
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
            QVERIFY(QMetaObject::invokeMethod(more, "close"));
            QTRY_VERIFY(!menu->property("visible").toBool() && !more->property("visible").toBool());
        }
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void nightModeIsOneSwitchInBothAudioMenus()
    {
        PlayerStub stub;
        stub.audioTracks = {QVariantMap{{QStringLiteral("id"), 1},
                                        {QStringLiteral("name"), QStringLiteral("English")},
                                        {QStringLiteral("selected"), true}}};
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *barMenu = ui.popup(QStringLiteral("playerAudioMenu"));
        QObject *contextMenu = ui.popup(QStringLiteral("playerContextAudioMenu"));
        QVERIFY(barMenu && contextMenu);
        // The bottom bar's menu: the tracks, then Night Mode after a separator
        QVERIFY(QMetaObject::invokeMethod(barMenu, "open"));
        QTRY_VERIFY(barMenu->property("opened").toBool());
        QObject *entry = barMenu->findChild<QObject *>(QStringLiteral("playerNightMode"));
        QVERIFY(entry);
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Night Mode"));
        QVERIFY(entry->property("checkable").toBool());
        QVERIFY(!entry->property("checked").toBool());
        // The check follows the setting, from whichever side changes it
        stub.nightMode = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(entry->property("checked").toBool());
        stub.nightMode = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!entry->property("checked").toBool());
        // The entry writes the setting through the player with its checked state
        entry->setProperty("checked", true);
        QVERIFY(QMetaObject::invokeMethod(entry, "triggered"));
        QVERIFY(stub.calls.contains(QStringLiteral("setNightMode true")));
        QVERIFY(stub.nightMode);
        entry->setProperty("checked", false);
        QVERIFY(QMetaObject::invokeMethod(entry, "triggered"));
        QVERIFY(stub.calls.contains(QStringLiteral("setNightMode false")));
        QVERIFY(!stub.nightMode);
        QVERIFY(Ui::closeMenu(barMenu));
        // The context menu's Audio submenu carries the same entry under its own name
        QObject *contextEntry =
            contextMenu->findChild<QObject *>(QStringLiteral("playerContextNightMode"));
        QVERIFY(contextEntry);
        QCOMPARE(contextEntry->property("text").toString(), QStringLiteral("Night Mode"));
        QVERIFY(contextEntry->property("checkable").toBool());
    }

    void secondarySubtitlesOfferTheTextTracksButThePrimary()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        const QList<std::pair<QString, QString>> menus{
            {QStringLiteral("playerSubtitlesMenu"), QStringLiteral("player")},
            {QStringLiteral("playerContextSubtitlesMenu"), QStringLiteral("playerContext")}};
        // Without a text track other than the primary the submenu is disabled
        for (const auto &[menuName, prefix] : menus) {
            QObject *menu = ui.popup(menuName);
            QVERIFY2(menu, qPrintable(menuName));
            QObject *submenu =
                menu->findChild<QObject *>(prefix + QStringLiteral("SecondarySubtitlesMenu"));
            QVERIFY2(submenu, qPrintable(prefix));
            QVERIFY(!submenu->property("enabled").toBool());
        }
        stub.subtitleTracks = {QVariantMap{{QStringLiteral("id"), 1},
                                           {QStringLiteral("name"), QStringLiteral("English")},
                                           {QStringLiteral("selected"), true},
                                           {QStringLiteral("secondary"), false},
                                           {QStringLiteral("text"), true}},
                               QVariantMap{{QStringLiteral("id"), 2},
                                           {QStringLiteral("name"), QStringLiteral("Italian")},
                                           {QStringLiteral("selected"), false},
                                           {QStringLiteral("secondary"), false},
                                           {QStringLiteral("text"), true}},
                               QVariantMap{{QStringLiteral("id"), 3},
                                           {QStringLiteral("name"), QStringLiteral("Bitmap")},
                                           {QStringLiteral("selected"), false},
                                           {QStringLiteral("secondary"), false},
                                           {QStringLiteral("text"), false}}};
        Q_EMIT stub.changed();
        for (const auto &[menuName, prefix] : menus) {
            QObject *menu = ui.popup(menuName);
            QVERIFY(menu);
            QObject *submenu =
                menu->findChild<QObject *>(prefix + QStringLiteral("SecondarySubtitlesMenu"));
            QVERIFY(submenu);
            QTRY_VERIFY(submenu->property("enabled").toBool());
        }
        // The bar's menu opens the submenu: Off and the text track the primary does not use, in plain text (a bitmap track never takes the slot)
        QObject *menu = ui.popup(QStringLiteral("playerSubtitlesMenu"));
        QObject *submenu =
            menu->findChild<QObject *>(QStringLiteral("playerSecondarySubtitlesMenu"));
        QVERIFY(submenu);
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());
        QQuickItem *entry = nullptr;
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerSubtitlesMenu"))) {
            if (item->property("subMenu").value<QObject *>() == submenu) {
                entry = item;
                break;
            }
        }
        QVERIFY(entry);
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(submenu->property("opened").toBool());
        QStringList names;
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerSecondarySubtitlesMenu"))) {
            if (!item->isVisible())
                continue;
            names << (item->objectName().isEmpty() ? item->property("plainText").toString()
                                                   : item->property("text").toString());
        }
        QCOMPARE(names, (QStringList{QStringLiteral("Off"), QStringLiteral("Italian")}));
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerSecondarySubtitlesMenu"))) {
            if (item->isVisible()
                && item->property("plainText").toString() == QLatin1String("Italian"))
                QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
        }
        QVERIFY(stub.calls.contains(QStringLiteral("selectSecondarySubtitleTrack 2")));
        QVERIFY(Ui::closeMenu(menu));
        QTest::qWait(100);
    }

    void clipMenuStatesTheRangeAndSaves()
    {
        PlayerStub stub;
        stub.clipLine = QStringLiteral("00:10 to 00:20 (10 s)");
        stub.clipVideoEnabled = true;
        stub.clipGifEnabled = true;
        stub.clipStartTicks = 100'000'000;
        stub.clipEndTicks = 200'000'000;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *more = ui.popup(QStringLiteral("playerMoreMenu"));
        QObject *clip = ui.popup(QStringLiteral("playerClipMenu"));
        QVERIFY(more && clip);
        QVERIFY(QMetaObject::invokeMethod(more, "open"));
        QTRY_VERIFY(more->property("opened").toBool());
        QQuickItem *entry = nullptr;
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerMoreMenu"))) {
            if (item->property("subMenu").value<QObject *>() == clip) {
                entry = item;
                break;
            }
        }
        QVERIFY(entry);
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(clip->property("opened").toBool());
        QQuickItem *line = clip->findChild<QQuickItem *>(QStringLiteral("playerClipLine"));
        QQuickItem *saveVideo =
            clip->findChild<QQuickItem *>(QStringLiteral("playerClipSaveVideo"));
        QQuickItem *saveGif = clip->findChild<QQuickItem *>(QStringLiteral("playerClipSaveGif"));
        QQuickItem *clear = clip->findChild<QQuickItem *>(QStringLiteral("playerClipClear"));
        QVERIFY(line && saveVideo && saveGif && clear);
        // The range in words, both Save entries offered
        QCOMPARE(line->property("text").toString(), QStringLiteral("00:10 to 00:20 (10 s)"));
        QVERIFY(!line->isEnabled());
        QVERIFY(saveVideo->isEnabled());
        QVERIFY(saveGif->isEnabled());
        QVERIFY(clear->isEnabled());
        // A GIF too long leaves its entry disabled and the line states the video limit
        stub.clipGifEnabled = false;
        stub.clipLine = QStringLiteral("A video clip lasts at most 60 seconds");
        Q_EMIT stub.changed();
        QTRY_COMPARE(line->property("text").toString(),
                     QStringLiteral("A video clip lasts at most 60 seconds"));
        QVERIFY(!saveGif->isEnabled());
        QVERIFY(saveVideo->isEnabled());
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerClipMenu"))) {
            if (item->objectName() == QLatin1String("playerClipStart"))
                QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
        }
        QVERIFY(stub.calls.contains(QStringLiteral("markClipStart")));
        QVERIFY(Ui::closeMenu(more));
        // The marked range shows on the timeline
        QQuickItem *band = ui.item(QStringLiteral("playerClipBand"));
        QVERIFY(band);
        QTRY_VERIFY(band->isVisible());
        // The export card shows the progress and Cancel stops it
        stub.clipSaving = true;
        stub.clipSavingGif = true;
        stub.clipProgress = 0.5;
        Q_EMIT stub.changed();
        QQuickItem *card = ui.item(QStringLiteral("playerClipCard"));
        QVERIFY(card);
        QTRY_VERIFY(card->isVisible());
        QQuickItem *label =
            card->findChild<QQuickItem *>(QStringLiteral("playerClipProgressLabel"));
        QVERIFY(label);
        QCOMPARE(label->property("text").toString(), QStringLiteral("Saving the GIF: 50%"));
        QQuickItem *cancel = card->findChild<QQuickItem *>(QStringLiteral("playerClipCancel"));
        QVERIFY(cancel);
        QTRY_VERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
        QVERIFY(stub.calls.contains(QStringLiteral("cancelClip")));
    }

    void gameControllerGroupsShowOnlyWithAPad()
    {
        PlayerStub stub;
        GamepadStub gamepad;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        root->setProperty("gamepad", QVariant::fromValue<QObject *>(&gamepad));
        QObject *dialog = ui.popup(QStringLiteral("shortcutsDialog"));
        QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTRY_VERIFY(dialog->property("visible").toBool());
        const auto texts = [dialog](bool visibleOnly) {
            QStringList found;
            for (QQuickItem *item : dialog->findChildren<QQuickItem *>()) {
                const QString text = item->property("text").toString();
                if ((!visibleOnly || item->isVisible()) && !text.isEmpty())
                    found.append(text);
            }
            return found;
        };
        QVERIFY(!texts(true).contains(QStringLiteral("Game Controller")));
        gamepad.connectedCount = 1;
        Q_EMIT gamepad.changed();
        QTRY_VERIFY(texts(true).contains(QStringLiteral("Game Controller")));
        QVERIFY(texts(true).contains(QStringLiteral("Game Controller in the Player")));
        // The pad's own printed labels reach the dialog; the mapping itself is tested with the virtual pad
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
    }

    void gamepadCommandsReachThePlayer()
    {
        PlayerStub stub;
        stub.hasNext = true;
        stub.hasPrevious = true;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *root = ui.engine.rootObjects().constFirst();
        // The shoulders step the player's items while the full player is in front
        QVERIFY(QMetaObject::invokeMethod(root, "remoteAction",
                                          Q_ARG(QVariant, QStringLiteral("nexttab"))));
        QVERIFY(stub.calls.contains(QStringLiteral("next")));
        QVERIFY(QMetaObject::invokeMethod(root, "remoteAction",
                                          Q_ARG(QVariant, QStringLiteral("previoustab"))));
        QVERIFY(stub.calls.contains(QStringLiteral("previous")));
        // The triggers step chapters in every playback mode
        QVERIFY(QMetaObject::invokeMethod(root, "remoteAction",
                                          Q_ARG(QVariant, QStringLiteral("nextchapter"))));
        QVERIFY(stub.calls.contains(QStringLiteral("stepChapter 1")));
        QVERIFY(QMetaObject::invokeMethod(root, "remoteAction",
                                          Q_ARG(QVariant, QStringLiteral("previouschapter"))));
        QVERIFY(stub.calls.contains(QStringLiteral("stepChapter -1")));
    }

    void qualityMenuListsTheSteps()
    {
        PlayerStub stub;
        stub.qualityAvailable = true;
        stub.qualitySteps = {QVariantMap{{QStringLiteral("cap"), 0},
                                         {QStringLiteral("text"), QStringLiteral("Automatic")},
                                         {QStringLiteral("checked"), true}},
                             QVariantMap{{QStringLiteral("cap"), 8'000'000},
                                         {QStringLiteral("text"), QStringLiteral("8 Mbps")},
                                         {QStringLiteral("checked"), false}}};
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *more = ui.popup(QStringLiteral("playerMoreMenu"));
        QObject *quality = ui.popup(QStringLiteral("playerQualityMenu"));
        QVERIFY(more && quality);
        QVERIFY(QMetaObject::invokeMethod(more, "open"));
        QTRY_VERIFY(more->property("opened").toBool());
        QQuickItem *entry = nullptr;
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerMoreMenu"))) {
            if (item->property("subMenu").value<QObject *>() == quality) {
                entry = item;
                break;
            }
        }
        QVERIFY(entry);
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(quality->property("opened").toBool());
        QStringList names;
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerQualityMenu"))) {
            if (item->isVisible())
                names << item->property("plainText").toString();
        }
        QCOMPARE(names, (QStringList{QStringLiteral("Automatic"), QStringLiteral("8 Mbps")}));
        for (QQuickItem *item : ui.menuItems(QStringLiteral("playerQualityMenu"))) {
            if (item->isVisible()
                && item->property("plainText").toString() == QLatin1String("8 Mbps"))
                QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
        }
        QVERIFY(stub.calls.contains(QStringLiteral("setQuality 8000000")));
        QVERIFY(Ui::closeMenu(more));
        QTest::qWait(100);
    }

    void playRequestedItemPlaysOrOpensTheDetails()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QObject *root = ui.engine.rootObjects().constFirst();
        // Signed out the request waits for the sign-in, as a link does
        QVERIFY(QMetaObject::invokeMethod(root, "playRequestedItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, 0)));
        QCOMPARE(root->property("requestedItemId").toString(), kMovieId);
        root->setProperty("requestedItemId", QString());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        // Signed in, a KRunner Play starts the player
        stub.calls.clear();
        QVERIFY(QMetaObject::invokeMethod(root, "playRequestedItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, 0)));
        QVERIFY(stub.calls.contains(QStringLiteral("playItem ") + kMovieId));
        // A play the player refuses opens the item's details page instead
        stub.playRefused = true;
        QVERIFY(QMetaObject::invokeMethod(root, "playRequestedItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, 0)));
        QTRY_VERIFY(ui.currentDetails()
                    && ui.currentDetails()->objectName() == QStringLiteral("itemDetailsPage"));
    }

    void playerOnlineSubtitleSearchIsPolicyGatedAndDownloads()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));

        QObject *menu = ui.popup(QStringLiteral("playerSubtitlesMenu"));
        QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());
        const auto searchEntry = [&]() -> QQuickItem * {
            for (QQuickItem *item : ui.menuItems(QStringLiteral("playerSubtitlesMenu"))) {
                if (item->objectName() == QLatin1String("playerSearchOnline"))
                    return item;
            }
            return nullptr;
        };
        // Hidden, not disabled, without the server policy
        QVERIFY(searchEntry());
        QVERIFY(!searchEntry()->isVisible());
        stub.canManageSubtitles = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(searchEntry()->isVisible());
        QVERIFY(QMetaObject::invokeMethod(searchEntry(), "click"));

        QObject *dialog = ui.popup(QStringLiteral("subtitleSearchDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_COMPARE(stub.remoteSearches, QStringList{QStringLiteral("eng")});

        // The no-provider answer is explained, and an empty answer is not mistaken for a pending one
        auto label = [dialog](const char *name) {
            return dialog->findChild<QQuickItem *>(QString::fromLatin1(name));
        };
        stub.remoteSubtitleSearching = false;
        stub.remoteSubtitleError = QStringLiteral("noprovider");
        Q_EMIT stub.remoteSubtitlesChanged();
        QTRY_VERIFY(label("subtitleSearchError")->isVisible());
        QCOMPARE(label("subtitleSearchError")->property("text").toString(),
                 QStringLiteral("No online subtitle provider is installed on the server. Ask the "
                                "server administrator to install one"));
        QVERIFY(!label("subtitleSearchEmpty")->isVisible());
        stub.remoteSubtitleError.clear();
        Q_EMIT stub.remoteSubtitlesChanged();
        QTRY_VERIFY(label("subtitleSearchEmpty")->isVisible());
        QCOMPARE(label("subtitleSearchEmpty")->property("text").toString(),
                 QStringLiteral("No subtitles found"));

        // A result: Download hands the id over and closes the dialog
        stub.remoteSubtitles = {
            QVariantMap{{QStringLiteral("id"), QStringLiteral("42")},
                        {QStringLiteral("name"), QStringLiteral("English (SDH)")},
                        {QStringLiteral("provider"), QStringLiteral("OpenSubtitles")},
                        {QStringLiteral("format"), QStringLiteral("srt")},
                        {QStringLiteral("downloads"), 12}}};
        Q_EMIT stub.remoteSubtitlesChanged();
        QObject *results = dialog->findChild<QObject *>(QStringLiteral("subtitleResults"));
        QVERIFY(results);
        QTRY_COMPARE(results->property("count").toInt(), 1);
        QQuickItem *row = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(results, "itemAtIndex",
                                              Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, 0))
                    && row);
        QQuickItem *download = nullptr;
        for (QQuickItem *candidate : allItems(row)) {
            if (candidate->objectName() == QLatin1String("subtitleResultDownloadButton"))
                download = candidate;
        }
        QVERIFY(download);
        QVERIFY(QMetaObject::invokeMethod(download, "click"));
        QCOMPARE(stub.remoteDownloads, QStringList{QStringLiteral("42")});
        QTRY_VERIFY(!dialog->property("opened").toBool());
    }

    void playerUpNextCardPlaysOnReturnAndCancelsOnEscape()
    {
        PlayerStub stub;
        const QString hostile =
            QStringLiteral("<b>Abyss</b> & <img src=\"http://evil.invalid/x.png\">");
        stub.skipLabel = QStringLiteral("Skip Outro");
        stub.upNext = {{QStringLiteral("title"), hostile},
                       {QStringLiteral("subtitle"), QString(QStringLiteral("S2:E7 — ") + hostile)},
                       {QStringLiteral("imageUrl"), QUrl()}};
        stub.upNextSeconds = 12;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *card = ui.item(QStringLiteral("playerUpNextCard"));
        QQuickItem *skip = ui.item(QStringLiteral("skipSegmentButton"));
        QVERIFY(card && skip);
        QVERIFY(!card->isVisible());
        QVERIFY(skip->isVisible());
        // Up: the next item's names in plain text, the seconds left, and Skip Outro out of its way
        stub.upNextShown = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(card->isVisible());
        // Skip fades out
        QTRY_VERIFY(!skip->isVisible());
        int found = 0;
        const QStringList problems = ui.notPlainText(hostile, &found);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1String(", "))));
        QCOMPARE(found, 2);
        QCOMPARE(ui.item(QStringLiteral("playerUpNextCountdown"))->property("text").toString(),
                 QStringLiteral("Starts in 12 seconds"));
        QVERIFY(!ui.item(QStringLiteral("playerUpNextImage"))->isVisible());
        // The keyboard stays with the page: Return is Play Now
        QCOMPARE(ui.window->activeFocusItem(), page);
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_Return);
        QCOMPARE(stub.calls, QStringList({QStringLiteral("playUpNext")}));
        // A click on Play Now leaves the keyboard with the page too
        ui.clickCenter(ui.item(QStringLiteral("playerUpNextPlay")));
        QTRY_COMPARE(stub.calls.count(QStringLiteral("playUpNext")), 2);
        QCOMPARE(ui.window->activeFocusItem(), page);
        // Esc cancels the card before anything else it would do; Skip Outro comes back
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("cancelUpNext")));
        QTRY_VERIFY(!card->isVisible());
        QVERIFY(skip->isVisible());
        QCOMPARE(ui.currentDetails(), page);
        // Without the card Return does nothing
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTest::qWait(100);
        QVERIFY(stub.calls.isEmpty());
        // A button reached with Tab takes Return itself: on Cancel it cancels
        stub.upNextShown = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(card->isVisible());
        QQuickItem *cancel = ui.item(QStringLiteral("playerUpNextCancel"));
        QVERIFY(cancel);
        cancel->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(cancel->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_COMPARE(stub.calls, QStringList({QStringLiteral("cancelUpNext")}));
        QTest::qWait(300);
        QVERIFY(gWarnings.filter(QStringLiteral("evil.invalid")).isEmpty());
    }

    void playerForgetsSeriesChoicesOnlyWhenThereAreSome()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *forget = menu->findChild<QObject *>(QStringLiteral("playerForgetSeriesItem"));
        QVERIFY(forget);
        QCOMPARE(forget->property("text").toString(), QStringLiteral("Forget Series Choices"));
        // Disabled, not hidden, while the series has nothing remembered
        QVERIFY(!forget->property("enabled").toBool());
        QVERIFY(forget->property("visible").toBool());
        stub.hasSeriesChoices = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(forget->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(forget, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("forgetSeriesChoices")));
    }

    void playlistsOpenFromTheirTileAndTheMainMenuAndPlayAsTheQueue()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        const QString playlistsView = QStringLiteral("e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0");
        ui.serveViews({{kMovies, QStringLiteral("Movies"), QStringLiteral("movies")},
                       {playlistsView, QStringLiteral("Playlists"), QStringLiteral("playlists")}});
        QStringList films;
        for (int i = 1; i <= 2; ++i) {
            const QString id = QStringLiteral("f1%1").arg(i, 30, 16, QLatin1Char('0'));
            ui.server.itemsById.insert(
                id, QJsonDocument(
                        QJsonObject{{QStringLiteral("Id"), id},
                                    {QStringLiteral("Name"), QStringLiteral("Film %1").arg(i)},
                                    {QStringLiteral("Type"), QStringLiteral("Movie")}})
                        .toJson(QJsonDocument::Compact));
            films.append(id);
        }
        // The first film twice: the queue takes it once
        const QString evening = ui.server.seedPlaylist(QStringLiteral("Evening"),
                                                       {films.at(0), films.at(1), films.at(0)});
        ui.server.itemsById.insert(
            evening,
            QJsonDocument(QJsonObject{{QStringLiteral("Id"), evening},
                                      {QStringLiteral("Name"), QStringLiteral("Evening")},
                                      {QStringLiteral("Type"), QStringLiteral("Playlist")}})
                .toJson(QJsonDocument::Compact));
        const QString empty = ui.server.seedPlaylist(QStringLiteral("Anime"), {});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        // The server's Playlists view is a tile that opens the Playlists page
        QTRY_VERIFY(ui.item(QStringLiteral("libraryCard_") + playlistsView) != nullptr);
        QQuickItem *tile = ui.item(QStringLiteral("libraryCard_") + playlistsView);
        QVERIFY(tile->isEnabled());
        ui.clickCenter(tile);
        QTRY_VERIFY(ui.item(QStringLiteral("playlistsPage")) != nullptr);
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        QVERIFY(ui.item(QStringLiteral("mediaCard_") + empty) != nullptr);
        QCOMPARE(root->property("currentSection").toString(), QStringLiteral("playlists"));
        // The tile opens it as the root of the stack, and the Playlists tab is the one marked; the menu has no entry for it
        QTRY_COMPARE(ui.rootTab(), QStringLiteral("playlists"));
        QCOMPARE(ui.stackDepth(), 1);
        // Only a movie library: Home, Movies, Favorites, Playlists
        const auto playlistsTab = [&ui] {
            return ui.tabButtons().value(ui.tabNames().indexOf(QStringLiteral("Playlists")));
        };
        QVERIFY(playlistsTab());
        QTRY_VERIFY(playlistsTab()->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(root, "goHome"));
        QTRY_VERIFY(ui.item(QStringLiteral("playlistsPage")) == nullptr);
        QVERIFY(!playlistsTab()->property("checked").toBool());
        QVERIFY(!ui.shellAction(QStringLiteral("playlistsAction")));
        QVERIFY(ui.clickTab(QStringLiteral("Playlists")));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        // An empty playlist says so
        ui.clickCenter(ui.item(QStringLiteral("mediaCard_") + empty));
        QTRY_VERIFY(ui.item(QStringLiteral("playlistEmpty"))
                    && ui.item(QStringLiteral("playlistEmpty"))->isVisible());
        QQuickItem *page = ui.currentDetails();
        QCOMPARE(page->objectName(), QStringLiteral("playlistPage"));
        QCOMPARE(page->property("title").toString(), QStringLiteral("Anime"));
        QVERIFY(!page->findChild<QObject *>(QStringLiteral("playPlaylistAction"))
                     ->property("enabled")
                     .toBool());
        // A playlist lists its entries in order, under its name
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        ui.clickCenter(ui.item(QStringLiteral("mediaCard_") + evening));
        QTRY_VERIFY(ui.item(QStringLiteral("playlistRow_2")) != nullptr);
        page = ui.currentDetails();
        QCOMPARE(page->property("title").toString(), QStringLiteral("Evening"));
        QCOMPARE(ui.item(QStringLiteral("playlistRow_0"))->property("name").toString(),
                 QStringLiteral("Film 1"));
        QCOMPARE(ui.item(QStringLiteral("playlistRow_1"))->property("name").toString(),
                 QStringLiteral("Film 2"));
        QCOMPARE(root->property("currentSection").toString(), QStringLiteral("playlists"));
        // Play From Here makes the playlist the queue, each item once, and starts at the row's item
        QVERIFY(QMetaObject::invokeMethod(page, "play", Q_ARG(QVariant, films.at(1)),
                                          Q_ARG(QVariant, false)));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("playQueue 1")));
        QCOMPARE(ui.controller.queue()->itemIds(), films);
        // Play starts it at the top, Shuffle in a random order of the same items
        stub.calls.clear();
        QObject *play = page->findChild<QObject *>(QStringLiteral("playPlaylistAction"));
        QVERIFY(play && play->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(play, "trigger"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("playQueue 0")));
        stub.calls.clear();
        QVERIFY(QMetaObject::invokeMethod(page, "play", Q_ARG(QVariant, QString()),
                                          Q_ARG(QVariant, true)));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("playQueue 0")));
        QStringList queued = ui.controller.queue()->itemIds();
        std::sort(queued.begin(), queued.end());
        QCOMPARE(queued, films);
        // Undo on the queue takes the last replacement back
        QVERIFY(ui.controller.queue()->canUndo());
        // Back on the page from an item, the playlist is read again
        const auto reads = [&ui] {
            return std::count_if(ui.server.playlistRequests.cbegin(),
                                 ui.server.playlistRequests.cend(),
                                 [](const auto &request) { return request.method == "GET"; });
        };
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(
            root, "openPlaylist", Q_ARG(QVariant, evening),
            Q_ARG(QVariant, QVariant::fromValue<QObject *>(ui.currentDetails())),
            Q_ARG(QVariant, QString())));
        QTRY_VERIFY(ui.currentDetails()->objectName() == QLatin1String("playlistPage"));
        page = ui.currentDetails();
        QTRY_COMPARE(page->property("loading").toBool(), false);
        const qsizetype before = reads();
        QVERIFY(QMetaObject::invokeMethod(root, "openItem", Q_ARG(QVariant, films.at(0)),
                                          Q_ARG(QVariant, QVariant::fromValue<QObject *>(page))));
        QTRY_VERIFY(ui.currentDetails()->objectName() == QLatin1String("itemDetailsPage"));
        QTest::keyClick(ui.window, Qt::Key_Left, Qt::AltModifier);
        QTRY_VERIFY(ui.currentDetails() == page);
        QTRY_VERIFY(reads() > before);
        // The card menu's Open takes a playlist card to its page, not to the details of the item
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        QQuickItem *card = ui.item(QStringLiteral("mediaCard_") + evening);
        QVERIFY(QMetaObject::invokeMethod(
            root, "openCardMenu", Q_ARG(QVariant, QVariant::fromValue<QObject *>(card)),
            Q_ARG(QVariant, evening), Q_ARG(QVariant, QStringLiteral("Playlist")),
            Q_ARG(QVariant, QString()), Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, -1),
            Q_ARG(QVariant, false), Q_ARG(QVariant, false)));
        QObject *open = ui.popup(QStringLiteral("cardMenuOpen"));
        QTRY_VERIFY(open && open->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(open, "click"));
        QTRY_VERIFY(ui.currentDetails()->objectName() == QLatin1String("playlistPage"));
        QCOMPARE(ui.currentDetails()->property("playlistId").toString(), evening);
    }

    void playlistsSayWhenThereAreNone()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_VERIFY(ui.item(QStringLiteral("playlistsEmpty"))
                    && ui.item(QStringLiteral("playlistsEmpty"))->isVisible());
        QCOMPARE(ui.item(QStringLiteral("playlistsEmpty"))->property("text").toString(),
                 QStringLiteral("No playlists"));
    }

    void playlistsAreEditedFromTheKeysTheMenuAndTheirDialogs()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QStringList films;
        for (int i = 1; i <= 3; ++i) {
            const QString id = QStringLiteral("f2%1").arg(i, 30, 16, QLatin1Char('0'));
            ui.server.itemsById.insert(
                id, QJsonDocument(
                        QJsonObject{{QStringLiteral("Id"), id},
                                    {QStringLiteral("Name"), QStringLiteral("Film %1").arg(i)},
                                    {QStringLiteral("Type"), QStringLiteral("Movie")}})
                        .toJson(QJsonDocument::Compact));
            films.append(id);
        }
        const QString evening = ui.server.seedPlaylist(QStringLiteral("Evening"), films);
        const QString other = ui.server.seedPlaylist(QStringLiteral("Other"), {films.at(0)});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        ui.clickCenter(ui.item(QStringLiteral("mediaCard_") + evening));
        QTRY_VERIFY(ui.item(QStringLiteral("playlistRow_2")) != nullptr);
        QQuickItem *page = ui.currentDetails();
        QCOMPARE(page->objectName(), QStringLiteral("playlistPage"));
        QTRY_COMPARE(page->property("loading").toBool(), false);
        QQuickItem *list = ui.item(QStringLiteral("playlistEntries"));
        QVERIFY(list);
        QVERIFY(QMetaObject::invokeMethod(page, "focusPrimary"));
        QTRY_VERIFY(list->hasActiveFocus());
        // Ctrl+Down moves the current entry and the row follows it; the server gets the same order
        QCOMPARE(list->property("currentIndex").toInt(), 0);
        QTest::keyClick(ui.window, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QCOMPARE(ui.item(QStringLiteral("playlistRow_1"))->property("itemId").toString(),
                 films.at(0));
        QTRY_COMPARE(ui.server.playlistItemIds(evening),
                     (QStringList{films.at(1), films.at(0), films.at(2)}));
        // Delete removes it, and the next entry takes its row
        QTest::keyClick(ui.window, Qt::Key_Delete);
        QTRY_COMPARE(ui.server.playlistItemIds(evening), (QStringList{films.at(1), films.at(2)}));
        QCOMPARE(list->property("currentIndex").toInt(), 1);
        QTRY_COMPARE(page->property("loading").toBool(), false);
        // A change the server refuses comes back, and the page says why
        ui.server.playlistWriteStatus = 403;
        QTest::keyClick(ui.window, Qt::Key_Up, Qt::ControlModifier);
        QQuickItem *refused = ui.item(QStringLiteral("playlistEditFailed"));
        QTRY_VERIFY(refused && refused->isVisible());
        QCOMPARE(refused->property("text").toString(),
                 QStringLiteral("Missing permission to change this playlist"));
        QTRY_COMPARE(page->property("loading").toBool(), false);
        QTRY_COMPARE(ui.item(QStringLiteral("playlistRow_0"))->property("itemId").toString(),
                     films.at(1));
        ui.server.playlistWriteStatus = 0;
        // The row menu moves and removes too; the next change clears the message
        QQuickItem *first = ui.item(QStringLiteral("playlistRow_0"));
        QVERIFY(QMetaObject::invokeMethod(page, "openMenu",
                                          Q_ARG(QVariant, QVariant::fromValue<QObject *>(first)),
                                          Q_ARG(QVariant, 0), Q_ARG(QVariant, 0)));
        QObject *up = ui.popup(QStringLiteral("playlistMenuMoveUp"));
        QObject *down = ui.popup(QStringLiteral("playlistMenuMoveDown"));
        QObject *remove = ui.popup(QStringLiteral("playlistMenuRemove"));
        QVERIFY(up && down && remove);
        QTRY_VERIFY(down->property("visible").toBool());
        QVERIFY(!up->property("enabled").toBool());
        QVERIFY(down->property("enabled").toBool());
        QVERIFY(remove->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(down, "click"));
        QTRY_COMPARE(ui.server.playlistItemIds(evening), (QStringList{films.at(2), films.at(1)}));
        QTRY_VERIFY(!refused->isVisible());
        QTRY_COMPARE(page->property("loading").toBool(), false);
        // Rename… asks for the new name, filled with the old one, and the page takes it
        QObject *renameAction = page->findChild<QObject *>(QStringLiteral("renamePlaylistAction"));
        QVERIFY(renameAction && renameAction->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(renameAction, "trigger"));
        QObject *renameDialog = ui.popup(QStringLiteral("playlistNameDialog"));
        QTRY_VERIFY(renameDialog && renameDialog->property("opened").toBool());
        QObject *field = ui.popup(QStringLiteral("playlistNameField"));
        QVERIFY(field);
        QCOMPARE(field->property("text").toString(), QStringLiteral("Evening"));
        QVERIFY(!renameDialog->property("acceptable").toBool());
        QObject *invalid = ui.popup(QStringLiteral("playlistNameInvalid"));
        QVERIFY(invalid && !invalid->property("visible").toBool());
        QVERIFY(field->setProperty("text", QStringLiteral("   ")));
        QVERIFY(!renameDialog->property("acceptable").toBool());
        QTRY_VERIFY(invalid->property("visible").toBool());
        QVERIFY(field->setProperty("text", QStringLiteral("  Late Night  ")));
        QVERIFY(renameDialog->property("acceptable").toBool());
        QTRY_VERIFY(!invalid->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(renameDialog, "accept"));
        QTRY_COMPARE(ui.server.playlistById(evening)->name, QStringLiteral("Late Night"));
        QTRY_COMPARE(page->property("title").toString(), QStringLiteral("Late Night"));
        // Delete… asks first; deleting closes the page
        QTRY_VERIFY(!renameDialog->property("visible").toBool());
        QObject *deleteAction = page->findChild<QObject *>(QStringLiteral("deletePlaylistAction"));
        QTRY_VERIFY(deleteAction && deleteAction->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(deleteAction, "trigger"));
        QObject *deleteDialog = ui.popup(QStringLiteral("deletePlaylistDialog"));
        QTRY_VERIFY(deleteDialog && deleteDialog->property("opened").toBool());
        QVERIFY(ui.server.playlistById(evening));
        QVERIFY(QMetaObject::invokeMethod(deleteDialog, "confirm"));
        QTRY_VERIFY(ui.server.playlistById(evening) == nullptr);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) == nullptr);
        // The card menu of a playlist renames and deletes it; other cards have no such entries
        QTRY_VERIFY(!deleteDialog->property("visible").toBool());
        QQuickItem *card = ui.item(QStringLiteral("mediaCard_") + other);
        QVERIFY(card);
        const auto openCardMenu = [&](const QString &id, const QString &type) {
            return QMetaObject::invokeMethod(
                root, "openCardMenu", Q_ARG(QVariant, QVariant::fromValue<QObject *>(card)),
                Q_ARG(QVariant, id), Q_ARG(QVariant, type), Q_ARG(QVariant, QString()),
                Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, -1), Q_ARG(QVariant, false),
                Q_ARG(QVariant, false));
        };
        QObject *cardMenu = ui.popup(QStringLiteral("cardMenu"));
        QObject *renameEntry = ui.popup(QStringLiteral("cardMenuRenamePlaylist"));
        QObject *deleteEntry = ui.popup(QStringLiteral("cardMenuDeletePlaylist"));
        QVERIFY(cardMenu && renameEntry && deleteEntry);
        QVERIFY(openCardMenu(films.at(0), QStringLiteral("Movie")));
        QTRY_VERIFY(cardMenu->property("opened").toBool());
        QVERIFY(!renameEntry->property("visible").toBool());
        QVERIFY(!deleteEntry->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(cardMenu, "close"));
        QTRY_VERIFY(!cardMenu->property("visible").toBool());
        QVERIFY(openCardMenu(other, QStringLiteral("Playlist")));
        QTRY_VERIFY(renameEntry->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(renameEntry, "click"));
        QTRY_VERIFY(renameDialog->property("opened").toBool());
        QCOMPARE(field->property("text").toString(), QStringLiteral("Other"));
        QVERIFY(QMetaObject::invokeMethod(renameDialog, "close"));
        QTRY_VERIFY(!renameDialog->property("visible").toBool());
        // A playlist the server says the user may not delete is never sent a DELETE
        ui.server.playlistsDeletable = false;
        QVERIFY(openCardMenu(other, QStringLiteral("Playlist")));
        QTRY_VERIFY(deleteEntry->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(deleteEntry, "click"));
        QTRY_VERIFY(deleteDialog->property("opened").toBool());
        const qsizetype requests = ui.server.playlistRequests.size();
        QVERIFY(QMetaObject::invokeMethod(deleteDialog, "confirm"));
        QTRY_VERIFY(ui.server.playlistRequests.size() > requests);
        QTRY_VERIFY(!ui.controller.playlists()->writing());
        QVERIFY(ui.server.playlistById(other));
        QVERIFY(std::none_of(ui.server.playlistRequests.cbegin() + requests,
                             ui.server.playlistRequests.cend(),
                             [](const auto &request) { return request.method == "DELETE"; }));
    }

    void videosGoIntoPlaylistsFromTheirMenuTheirPageAndANewPlaylist()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QStringList films;
        for (int i = 1; i <= 3; ++i) {
            const QString id = QStringLiteral("f3%1").arg(i, 30, 16, QLatin1Char('0'));
            ui.server.itemsById.insert(
                id, QJsonDocument(
                        QJsonObject{{QStringLiteral("Id"), id},
                                    {QStringLiteral("Name"), QStringLiteral("Film %1").arg(i)},
                                    {QStringLiteral("Type"), QStringLiteral("Movie")}})
                        .toJson(QJsonDocument::Compact));
            films.append(id);
        }
        const QString evening = ui.server.seedPlaylist(QStringLiteral("Evening"), {films.at(0)});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        const auto playlistNamed = [&ui](const QString &name) {
            for (const auto &playlist : std::as_const(ui.server.playlists)) {
                if (playlist.name == name)
                    return playlist.id;
            }
            return QString();
        };
        // New Playlist… on the Playlists page makes an empty one and stays on the list
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        QObject *newAction =
            ui.currentDetails()->findChild<QObject *>(QStringLiteral("newPlaylistAction"));
        QVERIFY(newAction && newAction->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(newAction, "trigger"));
        QObject *nameDialog = ui.popup(QStringLiteral("playlistNameDialog"));
        QTRY_VERIFY(nameDialog && nameDialog->property("opened").toBool());
        QCOMPARE(nameDialog->property("title").toString(), QStringLiteral("New Playlist"));
        QObject *field = ui.popup(QStringLiteral("playlistNameField"));
        QVERIFY(field && field->property("text").toString().isEmpty());
        QVERIFY(!nameDialog->property("acceptable").toBool());
        QVERIFY(field->setProperty("text", QStringLiteral(" Weekend ")));
        QVERIFY(nameDialog->property("acceptable").toBool());
        QVERIFY(QMetaObject::invokeMethod(nameDialog, "accept"));
        QTRY_VERIFY(!playlistNamed(QStringLiteral("Weekend")).isEmpty());
        const QString weekend = playlistNamed(QStringLiteral("Weekend"));
        QVERIFY(ui.server.playlistItemIds(weekend).isEmpty());
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + weekend) != nullptr);
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QTRY_VERIFY(!nameDialog->property("visible").toBool());
        // A video's card menu offers Add to Playlist…, a series' does not
        QQuickItem *card = ui.item(QStringLiteral("mediaCard_") + evening);
        const auto openCardMenu = [&](const QString &id, const QString &type) {
            return QMetaObject::invokeMethod(
                root, "openCardMenu", Q_ARG(QVariant, QVariant::fromValue<QObject *>(card)),
                Q_ARG(QVariant, id), Q_ARG(QVariant, type), Q_ARG(QVariant, QString()),
                Q_ARG(QVariant, 0), Q_ARG(QVariant, 0), Q_ARG(QVariant, -1), Q_ARG(QVariant, false),
                Q_ARG(QVariant, false));
        };
        QObject *cardMenu = ui.popup(QStringLiteral("cardMenu"));
        QObject *addEntry = ui.popup(QStringLiteral("cardMenuAddToPlaylist"));
        QVERIFY(cardMenu && addEntry);
        QVERIFY(openCardMenu(films.at(0), QStringLiteral("Series")));
        QTRY_VERIFY(cardMenu->property("opened").toBool());
        QVERIFY(!addEntry->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(cardMenu, "close"));
        QTRY_VERIFY(!cardMenu->property("visible").toBool());
        // The picker lists the playlists by name; a pick adds the video at the end
        QVERIFY(openCardMenu(films.at(1), QStringLiteral("Movie")));
        QTRY_VERIFY(addEntry->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(addEntry, "click"));
        QObject *picker = ui.popup(QStringLiteral("addToPlaylistDialog"));
        QTRY_VERIFY(picker && picker->property("opened").toBool());
        QCOMPARE(picker->property("itemId").toString(), films.at(1));
        QTRY_VERIFY(ui.item(QStringLiteral("addToPlaylistRow_1")) != nullptr);
        // The rows are delegates, items of the popup's overlay rather than QObject children of the window
        QQuickItem *first = ui.item(QStringLiteral("addToPlaylistRow_0"));
        QCOMPARE(first->property("name").toString(), QStringLiteral("Evening"));
        QCOMPARE(ui.item(QStringLiteral("addToPlaylistRow_1"))->property("name").toString(),
                 QStringLiteral("Weekend"));
        QVERIFY(QMetaObject::invokeMethod(first, "click"));
        QTRY_COMPARE(ui.server.playlistItemIds(evening), (QStringList{films.at(0), films.at(1)}));
        QTRY_VERIFY(!picker->property("visible").toBool());
        QTRY_VERIFY(!ui.controller.playlists()->writing());
        // A video already in the playlist is not added twice
        const auto posts = [&ui] {
            return std::count_if(ui.server.playlistRequests.cbegin(),
                                 ui.server.playlistRequests.cend(),
                                 [](const auto &request) { return request.method == "POST"; });
        };
        const qsizetype postsBefore = posts();
        QSignalSpy already(ui.controller.playlists(),
                           &Kinefin::PlaylistsController::alreadyInPlaylist);
        QVERIFY(QMetaObject::invokeMethod(root, "addToPlaylist", Q_ARG(QVariant, films.at(0))));
        QTRY_VERIFY(picker->property("opened").toBool());
        QTRY_VERIFY(ui.item(QStringLiteral("addToPlaylistRow_0")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "pick", Q_ARG(QVariant, evening)));
        QTRY_COMPARE(already.size(), 1);
        QCOMPARE(posts(), postsBefore);
        QCOMPARE(ui.server.playlistItemIds(evening), (QStringList{films.at(0), films.at(1)}));
        QTRY_VERIFY(!picker->property("visible").toBool());
        // New Playlist… in the picker asks for a name and makes a playlist holding the video
        QVERIFY(QMetaObject::invokeMethod(root, "addToPlaylist", Q_ARG(QVariant, films.at(2))));
        QTRY_VERIFY(picker->property("opened").toBool());
        QVERIFY(ui.item(QStringLiteral("addToNewPlaylist")) != nullptr);
        QVERIFY(QMetaObject::invokeMethod(picker, "requestNewPlaylist"));
        QTRY_VERIFY(nameDialog->property("opened").toBool());
        QCOMPARE(nameDialog->property("title").toString(), QStringLiteral("New Playlist"));
        QVERIFY(field->property("text").toString().isEmpty());
        QVERIFY(field->setProperty("text", QStringLiteral("Late")));
        QVERIFY(QMetaObject::invokeMethod(nameDialog, "accept"));
        QTRY_VERIFY(!playlistNamed(QStringLiteral("Late")).isEmpty());
        QCOMPARE(ui.server.playlistItemIds(playlistNamed(QStringLiteral("Late"))),
                 QStringList{films.at(2)});
        QTRY_VERIFY(!nameDialog->property("visible").toBool());
        QTRY_VERIFY(!ui.controller.playlists()->writing());
        // The details page of a video has the same entry
        QQuickItem *details = openDetails(ui, kMovieId);
        QVERIFY(details);
        QObject *detailsAction =
            details->findChild<QObject *>(QStringLiteral("addToPlaylistAction"));
        QTRY_VERIFY(detailsAction && detailsAction->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(detailsAction, "trigger"));
        QTRY_VERIFY(picker->property("opened").toBool());
        QCOMPARE(picker->property("itemId").toString(), kMovieId);
        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QTRY_VERIFY(!picker->property("visible").toBool());
        // The picker reads the list beside the Playlists page: closing it leaves the page's reading alone
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QTRY_COMPARE(root->property("playlistsPages").toInt(), 1);
        QVERIFY(QMetaObject::invokeMethod(root, "addToPlaylist", Q_ARG(QVariant, kMovieId)));
        QTRY_VERIFY(picker->property("opened").toBool());
        QCOMPARE(root->property("playlistsPages").toInt(), 2);
        QTRY_VERIFY(ui.item(QStringLiteral("addToPlaylistRow_0")) != nullptr);
        // Up from the first playlist reaches New Playlist…, whose Return hands over to the name dialog
        QQuickItem *pickerList = ui.item(QStringLiteral("addToPlaylistList"));
        QVERIFY(pickerList);
        QTRY_VERIFY(pickerList->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QCOMPARE(pickerList->property("currentIndex").toInt(), 0);
        QTest::keyClick(ui.window, Qt::Key_Up);
        QCOMPARE(pickerList->property("currentIndex").toInt(), -1);
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_VERIFY(nameDialog->property("opened").toBool());
        QTRY_VERIFY(field->property("activeFocus").toBool());
        QTRY_VERIFY(!picker->property("visible").toBool());
        QCOMPARE(root->property("playlistsPages").toInt(), 1);
        QVERIFY(QMetaObject::invokeMethod(nameDialog, "close"));
        QTRY_VERIFY(!nameDialog->property("visible").toBool());
        // Down and Return pick the first playlist
        QVERIFY(QMetaObject::invokeMethod(root, "addToPlaylist", Q_ARG(QVariant, kMovieId)));
        QTRY_VERIFY(picker->property("opened").toBool());
        QTRY_VERIFY(pickerList->hasActiveFocus());
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTest::keyClick(ui.window, Qt::Key_Return);
        QTRY_VERIFY(ui.server.playlistItemIds(evening).contains(kMovieId));
        QTRY_VERIFY(!picker->property("visible").toBool());
        QTRY_VERIFY(!ui.controller.playlists()->writing());
        // A picker closed while it still opens leaves the count of readers as it was
        QVERIFY(QMetaObject::invokeMethod(root, "addToPlaylist", Q_ARG(QVariant, kMovieId)));
        QVERIFY(QMetaObject::invokeMethod(picker, "close"));
        QTRY_VERIFY(!picker->property("visible").toBool());
        QTest::qWait(100);
        QCOMPARE(root->property("playlistsPages").toInt(), 1);
    }

    void everyPageUnderTheHeaderUsesTheRowOfTheTabs()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        const QString evening = ui.server.seedPlaylist(QStringLiteral("Evening"), {kMovieId});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);
        const QVariant noTitleRow =
            ui.item(QStringLiteral("homePage"))->property("globalToolBarStyle");
        const auto backShown = [](QQuickItem *page) {
            QQuickItem *back = Ui::itemIn(page, QStringLiteral("pageBackButton"));
            return back && back->isVisible();
        };
        // A section of the header: no title row and no way back, its actions in the row
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QQuickItem *page = ui.currentDetails();
        QCOMPARE(page->property("globalToolBarStyle"), noTitleRow);
        QVERIFY(!backShown(page));
        QTRY_VERIFY(ui.item(QStringLiteral("playlistsToolbar"))->isVisible());
        // A page above another: the way back and the name lead the same row
        QTRY_VERIFY(ui.item(QStringLiteral("mediaCard_") + evening) != nullptr);
        ui.clickCenter(ui.item(QStringLiteral("mediaCard_") + evening));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistPage"));
        page = ui.currentDetails();
        QCOMPARE(page->property("globalToolBarStyle"), noTitleRow);
        QTRY_VERIFY(backShown(page));
        QQuickItem *toolbar = Ui::itemIn(page, QStringLiteral("playlistToolbar"));
        QVERIFY(toolbar);
        // The name sits in the row itself, beside the way back
        QTRY_VERIFY(std::ranges::any_of(
            ui.itemsWithText(QStringLiteral("Evening"), true), [&](QQuickItem *label) {
                return label->isVisible() && label->parentItem() == toolbar->parentItem();
            }));
        ui.clickCenter(Ui::itemIn(page, QStringLiteral("pageBackButton")));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        // The details of an item: the way back and the actions, the hero names it
        page = openDetails(ui, kMovieId);
        QVERIFY(page);
        QCOMPARE(page->property("globalToolBarStyle"), noTitleRow);
        QTRY_VERIFY(backShown(page));
        QTRY_VERIFY(Ui::itemIn(page, QStringLiteral("detailsToolbar"))->isVisible());
        // The Continue Watching list and the search page
        QVERIFY(QMetaObject::invokeMethod(root, "goHome"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QVERIFY(QMetaObject::invokeMethod(root, "openResumeItems"));
        QTRY_VERIFY(ui.currentDetails()->objectName() != QLatin1String("homePage"));
        page = ui.currentDetails();
        QCOMPARE(page->property("globalToolBarStyle"), noTitleRow);
        QTRY_VERIFY(backShown(page));
        QVERIFY(QMetaObject::invokeMethod(root, "openSearch"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("searchPage"));
        QCOMPARE(ui.currentDetails()->property("globalToolBarStyle"), noTitleRow);
        QVERIFY(!backShown(ui.currentDetails()));
    }

    void aPlaceholderHiddenWhileFadingInLeavesNothingDrawn()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        // The details load while their Loading placeholder still fades in: once hidden, it must not stay drawn over the hero
        QQuickItem *page = openDetails(ui, kMovieId);
        QVERIFY(page);
        QTRY_VERIFY(page->property("showContent").toBool());
        QQuickItem *hero = Ui::itemIn(page, QStringLiteral("detailsHero"));
        QTRY_VERIFY(hero && hero->height() > 100);
        QTest::qWait(600);
        const QImage shot = ui.window->grabWindow();
        QVERIFY(!shot.isNull());
        // The fixture's backdrop is one colour under a vertical fade: every pixel row across the middle of the hero's top is uniform, a leftover text is not
        const QRect top = hero->mapRectToScene(QRectF(0, 0, hero->width(), 48)).toRect();
        const int left = top.left() + top.width() / 4;
        const int right = top.left() + top.width() * 3 / 4;
        for (int y = std::max(0, top.top()); y < std::min(shot.height(), top.bottom()); ++y) {
            const QColor first = shot.pixelColor(left, y);
            for (int x = left; x < right; ++x) {
                const QColor pixel = shot.pixelColor(x, y);
                const int deviation = std::max({std::abs(pixel.red() - first.red()),
                                                std::abs(pixel.green() - first.green()),
                                                std::abs(pixel.blue() - first.blue())});
                QVERIFY2(deviation <= 12,
                         qPrintable(QStringLiteral("something drawn at %1,%2").arg(x).arg(y)));
            }
        }
    }

    void placeholdersFadeInAndEndFullyVisible()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        // Shown again and again, it always ends fully visible
        for (int round = 0; round < 3; ++round) {
            QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
            QQuickItem *empty = nullptr;
            QTRY_VERIFY((empty = ui.item(QStringLiteral("playlistsEmpty"))) && empty->isVisible());
            QTRY_COMPARE(empty->opacity(), 1.0);
            QVERIFY(QMetaObject::invokeMethod(root, "goHome"));
            QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        }
    }

    void headerSectionsFadeInAndBackSlides()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QObject *row = root->property("pageStack").value<QObject *>();
        QVERIFY(row);
        // Each section of the header fades in, also from another section
        for (const char *section : {"openFavorites", "openPlaylists", "openFavorites"}) {
            QVERIFY(QMetaObject::invokeMethod(root, section));
            QTRY_VERIFY(root->property("tabSwitching").toBool());
            QTRY_VERIFY(!root->property("tabSwitching").toBool());
            QCOMPARE(row->property("opacity").toDouble(), 1.0);
        }
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("favoritesPage"));
        QCOMPARE(ui.stackDepth(), 1);
        // The section in front asked again: nothing moves
        QVERIFY(QMetaObject::invokeMethod(root, "openFavorites"));
        QVERIFY(!root->property("tabSwitching").toBool());
        // A page pushed above a section still fading in shows at once, fully visible
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QVERIFY(root->property("tabSwitching").toBool());
        QVERIFY(QMetaObject::invokeMethod(root, "openItem", Q_ARG(QVariant, kMovieId),
                                          Q_ARG(QVariant, QVariant())));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("itemDetailsPage"));
        QTRY_VERIFY(!root->property("tabSwitching").toBool());
        QCOMPARE(row->property("opacity").toDouble(), 1.0);
        // A section under the page in front comes back with the same fade
        QVERIFY(QMetaObject::invokeMethod(root, "openPlaylists"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("playlistsPage"));
        QTRY_VERIFY(!root->property("tabSwitching").toBool());
        QCOMPARE(row->property("opacity").toDouble(), 1.0);
        QTRY_COMPARE(ui.stackDepth(), 1);
        // Home is a tab like the others: the page of Playlists leaves once the stack settles
        QVERIFY(QMetaObject::invokeMethod(root, "goHome"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_COMPARE(ui.stackDepth(), 1);
        // Signing out fades the next page in, and the row ends fully visible
        ui.controller.signOut();
        QTRY_VERIFY(ui.currentDetails()->objectName() != QLatin1String("homePage"));
        QTRY_VERIFY(!root->property("tabSwitching").toBool());
        QCOMPARE(row->property("opacity").toDouble(), 1.0);
    }

    void anotherSessionsKeysNeverTypeOrConfirmInADialog()
    {
        Ui ui(m_home->path());
        ui.serveMovie();
        const QString evening = ui.server.seedPlaylist(QStringLiteral("Evening"), {});
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QObject *root = ui.engine.rootObjects().constFirst();
        Kinefin::RemoteInputServer keys(QString(), QStringLiteral("1.2.3"));
        keys.setWindow(ui.window);
        // A page in front takes the navigation keys
        QTRY_VERIFY(ui.currentDetails() != nullptr);
        QVERIFY(keys.dispatchNetworkKeycode(QStringLiteral("down")));
        // The rename dialog's field has the focus: Select would submit what the user types
        QVERIFY(QMetaObject::invokeMethod(root, "renamePlaylist", Q_ARG(QVariant, evening),
                                          Q_ARG(QVariant, QStringLiteral("Evening"))));
        QObject *nameDialog = ui.popup(QStringLiteral("playlistNameDialog"));
        QTRY_VERIFY(nameDialog && nameDialog->property("opened").toBool());
        QTRY_VERIFY(
            ui.popup(QStringLiteral("playlistNameField"))->property("activeFocus").toBool());
        QVERIFY(!keys.dispatchNetworkKeycode(QStringLiteral("select")));
        QVERIFY(!keys.dispatchNetworkKeycode(QStringLiteral("left")));
        QVERIFY(QMetaObject::invokeMethod(nameDialog, "close"));
        QTRY_VERIFY(!nameDialog->property("visible").toBool());
        // The delete question: whatever has the focus in it, Select never confirms
        QVERIFY(QMetaObject::invokeMethod(root, "askDeletePlaylist", Q_ARG(QVariant, evening)));
        QObject *deleteDialog = ui.popup(QStringLiteral("deletePlaylistDialog"));
        QTRY_VERIFY(deleteDialog && deleteDialog->property("opened").toBool());
        QVERIFY(!keys.dispatchNetworkKeycode(QStringLiteral("select")));
        QVERIFY(!keys.dispatchNetworkKeycode(QStringLiteral("down")));
        QTest::qWait(100);
        QVERIFY(ui.server.playlistById(evening));
        QVERIFY(QMetaObject::invokeMethod(deleteDialog, "close"));
        QTRY_VERIFY(!deleteDialog->property("visible").toBool());
    }

    void anotherSessionDrivesThePlayerAndTheQueue()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QStringList films;
        for (int i = 1; i <= 3; ++i) {
            const QString id = QStringLiteral("f4%1").arg(i, 30, 16, QLatin1Char('0'));
            ui.server.itemsById.insert(
                id, QJsonDocument(
                        QJsonObject{{QStringLiteral("Id"), id},
                                    {QStringLiteral("Name"), QStringLiteral("Film %1").arg(i)},
                                    {QStringLiteral("Type"), QStringLiteral("Movie")}})
                        .toJson(QJsonDocument::Compact));
            films.append(id);
        }
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        Kinefin::RemoteCommands *remote = ui.controller.remoteCommands();
        QObject *root = ui.engine.rootObjects().constFirst();
        QVERIFY(root->setProperty("player", QVariant::fromValue<QObject *>(&stub)));
        // Nothing plays: a seek goes nowhere
        remote->handlePlaystate(QStringLiteral("Seek"), 600);
        QTest::qWait(50);
        QVERIFY(!stub.calls.contains(QStringLiteral("seek 600")));
        QVERIFY(openPlayer(ui, stub));
        stub.calls.clear();
        // While something plays the player takes the commands that carry arguments
        remote->handlePlaystate(QStringLiteral("Seek"), 600);
        remote->handleGeneralCommand(QStringLiteral("SetVolume"),
                                     {{QStringLiteral("Volume"), QStringLiteral("40")}});
        remote->handleGeneralCommand(QStringLiteral("Mute"), {});
        remote->handleGeneralCommand(QStringLiteral("SetAudioStreamIndex"),
                                     {{QStringLiteral("Index"), QStringLiteral("2")}});
        remote->handleGeneralCommand(QStringLiteral("SetSubtitleStreamIndex"),
                                     {{QStringLiteral("Index"), QStringLiteral("-1")}});
        remote->handleGeneralCommand(QStringLiteral("SetRepeatMode"),
                                     {{QStringLiteral("RepeatMode"), QStringLiteral("RepeatAll")}});
        remote->handleGeneralCommand(QStringLiteral("SetShuffleQueue"),
                                     {{QStringLiteral("ShuffleMode"), QStringLiteral("Shuffle")}});
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setShuffle true")));
        for (const char *call :
             {"seek 600", "setVolume 40", "toggleMute", "selectAudioStreamIndex 2",
              "selectSubtitleStreamIndex -1", "setRepeatMode 1"})
            QVERIFY2(stub.calls.contains(QLatin1String(call)), call);
        // Unmute while already unmuted changes nothing
        stub.calls.clear();
        remote->handleGeneralCommand(QStringLiteral("Unmute"), {});
        QTest::qWait(50);
        QVERIFY(!stub.calls.contains(QStringLiteral("toggleMute")));
        // PlayNow of several videos makes them the queue and starts at the one named, at its position
        remote->handlePlay({QStringLiteral("PlayNow"), films, 500, 1});
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("playQueue 1")));
        QCOMPARE(stub.lastStartTicks, 500);
        QCOMPARE(ui.controller.queue()->itemIds(), films);
        // One video plays on its own and leaves the queue alone
        stub.calls.clear();
        remote->handlePlay({QStringLiteral("PlayNow"), {kMovieId}, 300, -1});
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("playItem ") + kMovieId));
        QCOMPARE(ui.controller.queue()->itemIds(), films);
        // PlayLast puts the items at the end, PlayNext after the current one in the order sent
        QTRY_VERIFY(!ui.controller.queue()->busy());
        remote->handlePlay({QStringLiteral("PlayLast"), {kMovieId}, -1, -1});
        QTRY_COMPARE(ui.controller.queue()->itemIds().size(), 4);
        QCOMPARE(ui.controller.queue()->itemIds().constLast(), kMovieId);
        QTRY_VERIFY(!ui.controller.queue()->busy());
        ui.controller.queue()->setCurrentPlaybackItemId(films.at(0));
        remote->handlePlay({QStringLiteral("PlayNext"), {films.at(2), kMovieId}, -1, -1});
        QTRY_COMPARE(ui.controller.queue()->itemIds(),
                     (QStringList{films.at(0), films.at(2), kMovieId, films.at(1)}));
        // Commands that come while the queue is busy run after it, in their order
        QTRY_VERIFY(!ui.controller.queue()->busy());
        ui.server.holdListings = true;
        remote->handlePlay({QStringLiteral("PlayLast"), {films.at(0)}, -1, -1});
        QTRY_VERIFY(ui.controller.queue()->busy());
        stub.calls.clear();
        remote->handlePlay({QStringLiteral("PlayNow"), {films.at(1), films.at(2)}, -1, 0});
        ui.server.holdListings = false;
        ui.server.releaseHeld();
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("playQueue 0")));
        QCOMPARE(ui.controller.queue()->itemIds(), (QStringList{films.at(1), films.at(2)}));
    }

    void playerRepeatIsAlwaysOnAndAllNeedsSomewhereToGo()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *repeat = menu->findChild<QObject *>(QStringLiteral("playerRepeatMenu"));
        QObject *all = menu->findChild<QObject *>(QStringLiteral("playerRepeatAll"));
        QObject *one = menu->findChild<QObject *>(QStringLiteral("playerRepeatOne"));
        QObject *off = menu->findChild<QObject *>(QStringLiteral("playerRepeatOff"));
        QVERIFY(repeat && all && one && off);
        QQuickItem *entry = nullptr;
        for (int i = 0; i < menu->property("count").toInt() && !entry; ++i) {
            QQuickItem *candidate = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, candidate),
                                      Q_ARG(int, i));
            if (candidate && candidate->property("subMenu").value<QObject *>() == repeat)
                entry = candidate;
        }
        QVERIFY(entry);
        // A lone item: the menu is on, Current Item is there, All has nothing to advance to
        QVERIFY(entry->isEnabled());
        QVERIFY(one->property("enabled").toBool());
        QVERIFY(off->property("enabled").toBool());
        QVERIFY(!all->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(one, "click"));
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("setRepeatMode 2")));
        // Somewhere to go, whichever way, turns All on
        for (const auto &setUp : {std::function<void()>([&] { stub.queueCount = 2; }),
                                  std::function<void()>([&] { stub.hasNext = true; }),
                                  std::function<void()>([&] { stub.hasPrevious = true; })}) {
            stub.queueCount = 0;
            stub.hasNext = false;
            stub.hasPrevious = false;
            Q_EMIT stub.changed();
            QTRY_VERIFY(!all->property("enabled").toBool());
            setUp();
            Q_EMIT stub.changed();
            QTRY_VERIFY(all->property("enabled").toBool());
        }
        // A repeat of all that is on can be turned off whatever the queue became
        stub.queueCount = 0;
        stub.hasNext = false;
        stub.hasPrevious = false;
        stub.repeatMode = 1;
        Q_EMIT stub.changed();
        QTRY_VERIFY(all->property("checked").toBool());
        QVERIFY(all->property("enabled").toBool());
        QVERIFY(entry->isEnabled());
    }

    void playerTimelineSeeksWhereADragEndsAndShowsNoTicks()
    {
        PlayerStub stub;
        stub.durationTicks = 72000000000;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *timeline = ui.item(QStringLiteral("playbackTimeline"));
        QVERIFY(timeline);
        // The style draws a tick mark at every step of a slider unless told otherwise
        auto *background = timeline->property("background").value<QQuickItem *>();
        QVERIFY(background);
        QCOMPARE(background->property("activeControl").toString(), QString());
        // A drag seeks once, where it ends
        const QPoint from =
            timeline->mapToScene(QPointF(timeline->width() * 0.2, timeline->height() / 2))
                .toPoint();
        const QPoint to =
            timeline->mapToScene(QPointF(timeline->width() * 0.37, timeline->height() / 2))
                .toPoint();
        QTest::mousePress(ui.window, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(ui.window, to);
        QTRY_VERIFY(timeline->property("pressed").toBool());
        QTRY_VERIFY(timeline->property("dragSeconds").toDouble() > 0);
        const double dragged = timeline->property("dragSeconds").toDouble();
        // A drag lands where the pointer is, not on ten-second steps
        QVERIFY2(std::fmod(dragged, 10.0) != 0.0, qPrintable(QString::number(dragged)));
        stub.calls.clear();
        QTest::mouseRelease(ui.window, Qt::LeftButton, Qt::NoModifier, to);
        QTRY_VERIFY(!timeline->property("pressed").toBool());
        // The last seek is where the drag ended: the release adds none of its own
        const QString wanted = QStringLiteral("seek %1").arg(qRound64(dragged * 10000000));
        QTRY_VERIFY(stub.calls.contains(wanted));
        QTest::qWait(200);
        QStringList seeks;
        for (const QString &call : std::as_const(stub.calls)) {
            if (call.startsWith(QLatin1String("seek ")))
                seeks << call;
        }
        QCOMPARE(seeks.constLast(), wanted);
        // With the focus on the timeline the arrow keys seek as the page's do, once each
        timeline->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(timeline->hasActiveFocus());
        stub.calls.clear();
        QTest::keyClick(ui.window, Qt::Key_Right);
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("seekRelative 10")), 1);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("seekRelative -60")), 1);
        QCOMPARE(stub.calls.count(QStringLiteral("seekRelative -10")), 0);
    }

    void playerControlRowTurnsToIconsWhenNarrow()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *row = ui.item(QStringLiteral("playerControlRow"));
        QQuickItem *subtitles = ui.item(QStringLiteral("playerSubtitlesButton"));
        QVERIFY(row && subtitles);
        // AbstractButton.TextBesideIcon, then IconOnly
        QCOMPARE(subtitles->property("display").toInt(), 2);
        QVERIFY(ui.opensWindowOfWidth(440));
        QTRY_COMPARE(subtitles->property("display").toInt(), 0);
        QVERIFY(ui.item(QStringLiteral("playerMuteButton"))->isVisible());
        // Every button keeps its text for the tooltip and the screen reader, and the row stays inside the window
        int buttons = 0;
        for (const QQuickItem *item : allItems(row)) {
            if (!item->isVisible() || !item->inherits("QQuickToolButton"))
                continue;
            ++buttons;
            QVERIFY(!item->property("text").toString().isEmpty());
            const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            QVERIFY2(rect.right() <= ui.window->width() && rect.left() >= 0,
                     qPrintable(item->property("text").toString()));
        }
        QVERIFY(buttons >= 6);
    }

    void playerFullScreenButtonFollowsTheWindow()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *button = ui.item(QStringLiteral("playerFullScreenButton"));
        QVERIFY(button);
        QCOMPARE(button->property("text").toString(), QStringLiteral("Full Screen Mode"));
        ui.clickCenter(button);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QTRY_COMPARE(button->property("text").toString(), QStringLiteral("Exit Full Screen Mode"));
        ui.clickCenter(button);
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);
        QTRY_COMPARE(button->property("text").toString(), QStringLiteral("Full Screen Mode"));
    }

    void playerVolumePopsUpOverTheSpeakerAndFollowsTheWheel()
    {
        PlayerStub stub;
        stub.volume = 50;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *speaker = ui.item(QStringLiteral("playerMuteButton"));
        QObject *popup = ui.popup(QStringLiteral("playerVolumePopup"));
        QVERIFY(speaker && popup);
        QVERIFY(!popup->property("opened").toBool());
        const QPoint onSpeaker =
            speaker->mapToScene(QPointF(speaker->width() / 2, speaker->height() / 2)).toPoint();
        // The pointer over the speaker shows a vertical slider with the volume
        QTest::mouseMove(ui.window, onSpeaker);
        QTRY_VERIFY(popup->property("opened").toBool());
        QQuickItem *slider = ui.item(QStringLiteral("playerVolumeSlider"));
        QVERIFY(slider);
        QCOMPARE(slider->property("orientation").toInt(), int(Qt::Vertical));
        QCOMPARE(slider->property("value").toReal(), 50.0);
        auto *background = slider->property("background").value<QQuickItem *>();
        QVERIFY(background);
        QCOMPARE(background->property("activeControl").toString(), QString());
        // The popup keeps its width whatever the number of digits of the level
        const qreal width = popup->property("width").toReal();
        stub.setVolume(100);
        QTRY_COMPARE(slider->property("value").toReal(), 100.0);
        QCOMPARE(popup->property("width").toReal(), width);
        stub.setVolume(5);
        QTRY_COMPARE(slider->property("value").toReal(), 5.0);
        QCOMPARE(popup->property("width").toReal(), width);
        stub.setVolume(50);
        QTRY_COMPARE(slider->property("value").toReal(), 50.0);
        // One notch of the wheel is five, a touchpad's half notches add up
        const auto wheel = [&ui](const QPoint &at, int delta) {
            QWheelEvent event(at, ui.window->mapToGlobal(at), QPoint(), QPoint(0, delta),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(ui.window, &event);
        };
        wheel(onSpeaker, 120);
        QTRY_COMPARE(stub.volume, 55);
        wheel(onSpeaker, -60);
        QTest::qWait(50);
        QCOMPARE(stub.volume, 55);
        wheel(onSpeaker, -60);
        QTRY_COMPARE(stub.volume, 50);
        // The wheel over the slider works the same, and the slider follows the volume
        const QPoint onSlider =
            slider->mapToScene(QPointF(slider->width() / 2, slider->height() / 2)).toPoint();
        QTest::mouseMove(ui.window, onSlider);
        wheel(onSlider, -120);
        QTRY_COMPARE(stub.volume, 45);
        QTRY_COMPARE(slider->property("value").toReal(), 45.0);
        QVERIFY(popup->property("opened").toBool());
        // A click on the speaker mutes and keeps the slider
        QTest::mouseMove(ui.window, onSpeaker);
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, onSpeaker);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("toggleMute")));
        QTest::qWait(100);
        QVERIFY(popup->property("opened").toBool());
        // Turning it up while muted unmutes
        stub.muted = true;
        Q_EMIT stub.changed();
        stub.calls.clear();
        wheel(onSpeaker, 120);
        QTRY_COMPARE(stub.volume, 50);
        QVERIFY(stub.calls.contains(QStringLiteral("toggleMute")));
        // The pointer leaving both lets the slider go
        QTest::mouseMove(ui.window, QPoint(ui.window->width() / 2, ui.window->height() / 2));
        QTRY_VERIFY(!popup->property("opened").toBool());
    }

    void playerVolumeKeysWorkOnTheSpeakerButton()
    {
        PlayerStub stub;
        stub.volume = 50;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *speaker = ui.item(QStringLiteral("playerMuteButton"));
        QObject *popup = ui.popup(QStringLiteral("playerVolumePopup"));
        QVERIFY(speaker && popup);
        // Keyboard focus on the speaker shows the slider; Up and Down change the volume and do not seek
        speaker->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(popup->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_COMPARE(stub.volume, 55);
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_COMPARE(stub.volume, 45);
        QVERIFY(stub.calls.filter(QStringLiteral("seekRelative")).isEmpty());
        // Esc closes the slider and stays in the player
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!popup->property("opened").toBool());
        QTest::qWait(100);
        QCOMPARE(ui.currentDetails(), page);
        // The keyboard resting on the speaker does not keep the bars: they go with the slider, and the page takes the keys back
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QVERIFY(timer);
        QQuickItem *osd = ui.item(QStringLiteral("playerOsd"));
        ui.item(QStringLiteral("playerTimeButton"))->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(!speaker->property("visualFocus").toBool());
        speaker->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(popup->property("opened").toBool());
        // The pointer rests on the video, off the bars, so only the keyboard is left on the speaker
        restPointerOnVideo(ui);
        timer->setProperty("interval", 1000);
        QTRY_VERIFY(!osd->isVisible());
        QVERIFY(!popup->property("opened").toBool());
        QCOMPARE(ui.window->activeFocusItem(), page);
        QTest::keyClick(ui.window, Qt::Key_Down);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("seekRelative -60")));
        // The bars coming back do not hand the speaker its focus back unseen: the keys stay with the page
        QTRY_VERIFY(osd->isVisible());
        QCOMPARE(ui.window->activeFocusItem(), page);
        // Back on the speaker, Tab moving on lets the slider go, and Up seeks again
        timer->setProperty("interval", 5000);
        speaker->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(popup->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Tab);
        QTRY_VERIFY(!speaker->property("visualFocus").toBool());
        QTRY_VERIFY(!popup->property("opened").toBool());
        QTest::keyClick(ui.window, Qt::Key_Up);
        QTRY_VERIFY(stub.calls.contains(QStringLiteral("seekRelative 60")));
    }

    void playerVolumeOpensOnALongPressWithoutMuting()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        QQuickItem *speaker = ui.item(QStringLiteral("playerMuteButton"));
        QObject *popup = ui.popup(QStringLiteral("playerVolumePopup"));
        QVERIFY(speaker && popup);
        // Touch has no hover: a long press is the way to the slider, and it does not mute
        const QPoint onSpeaker =
            speaker->mapToScene(QPointF(speaker->width() / 2, speaker->height() / 2)).toPoint();
        QPointingDevice *touch = QTest::createTouchDevice();
        QTest::touchEvent(ui.window, touch).press(0, onSpeaker);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 300);
        QVERIFY(popup->property("opened").toBool());
        QTest::touchEvent(ui.window, touch).release(0, onSpeaker);
        QTest::qWait(100);
        QVERIFY(!stub.calls.contains(QStringLiteral("toggleMute")));
        QVERIFY(popup->property("opened").toBool());
    }

    void playerBarsStayWhileLockedPausedOrUnderThePointer()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QObject *pointer = page->findChild<QObject *>(QStringLiteral("playerPointer"));
        QQuickItem *osd = ui.item(QStringLiteral("playerOsd"));
        QQuickItem *bottomBar = ui.item(QStringLiteral("playerBottomBar"));
        QVERIFY(timer && pointer && osd && bottomBar);
        timer->setProperty("interval", 300);
        const QPoint onVideo(ui.window->width() / 2, ui.window->height() / 2);
        // The shade above the controls is part of the bar, with no control under the pointer
        const QPoint onBar = bottomBar->mapToScene(QPointF(bottomBar->width() / 2, 4)).toPoint();
        int nudge = 0;
        const auto wake = [&] {
            QTest::mouseMove(ui.window, onVideo + QPoint(++nudge % 2 ? 3 : -3, 0));
        };
        // At rest on the video the bars go, and the pointer with them
        restPointerOnVideo(ui);
        QCOMPARE(pointer->property("cursorShape").toInt(), int(Qt::ArrowCursor));
        QTRY_VERIFY(!osd->isVisible());
        QCOMPARE(pointer->property("cursorShape").toInt(), int(Qt::BlankCursor));
        // The lock keeps them
        wake();
        QTRY_VERIFY(osd->isVisible());
        QCOMPARE(pointer->property("cursorShape").toInt(), int(Qt::ArrowCursor));
        QQuickItem *lock = ui.item(QStringLiteral("playerLockButton"));
        QVERIFY(lock);
        QCOMPARE(lock->property("text").toString(), QStringLiteral("Keep Controls Visible"));
        ui.clickCenter(lock);
        QTRY_VERIFY(page->property("osdLocked").toBool());
        QVERIFY(lock->property("checked").toBool());
        QTest::mouseMove(ui.window, onVideo);
        QTest::qWait(900);
        QVERIFY(osd->isVisible());
        ui.clickCenter(lock);
        QTRY_VERIFY(!page->property("osdLocked").toBool());
        wake();
        QTRY_VERIFY(!osd->isVisible());
        // A pause keeps them, and once it ends they get their time again
        stub.paused = true;
        Q_EMIT stub.changed();
        QTRY_VERIFY(osd->isVisible());
        QTest::qWait(900);
        QVERIFY(osd->isVisible());
        stub.paused = false;
        Q_EMIT stub.changed();
        QTRY_VERIFY(!osd->isVisible());
        // The pointer resting on a bar keeps them, on its shade as on one of its controls
        QQuickItem *pause = ui.item(QStringLiteral("playerPauseButton"));
        QVERIFY(pause);
        for (const QPoint &on :
             {onBar,
              pause->mapToScene(QPointF(pause->width() / 2, pause->height() / 2)).toPoint()}) {
            wake();
            QTRY_VERIFY(osd->isVisible());
            QTest::mouseMove(ui.window, on);
            QTest::qWait(900);
            QVERIFY(osd->isVisible());
        }
        QTest::mouseMove(ui.window, onVideo);
        QTRY_VERIFY(!osd->isVisible());
        // The pointer leaving the window from a bar does not keep them
        wake();
        QTRY_VERIFY(osd->isVisible());
        QTest::mouseMove(ui.window, onBar);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(ui.window, &leave);
        QTRY_VERIFY(!osd->isVisible());
    }

    void playerClickPausesSoonAndDoubleClickTogglesFullScreenWithoutAPause()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *wait = page->findChild<QObject *>(QStringLiteral("playerClickPauseTimer"));
        QVERIFY(wait);
        QVERIFY(wait->property("interval").toInt()
                < QGuiApplication::styleHints()->mouseDoubleClickInterval());
        // Each run of clicks lands somewhere else, so it never joins the run before it
        const QPoint onVideo(ui.window->width() / 2, ui.window->height() / 2);
        const QPoint elsewhere = onVideo + QPoint(80, 0);
        const QPoint thirdPlace = onVideo - QPoint(80, 0);
        // One click pauses after the short wait, and only once
        restPointerOnVideo(ui);
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, onVideo);
        QCOMPARE(stub.calls.count(QStringLiteral("togglePause")), 0);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        QVERIFY(stub.paused);
        // A double click toggles full screen and never touches the pause
        stub.calls.clear();
        QTest::mouseDClick(ui.window, Qt::LeftButton, Qt::NoModifier, elsewhere);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QTest::qWait(400);
        QCOMPARE(stub.calls.count(QStringLiteral("togglePause")), 0);
        QVERIFY(stub.paused);
        // A slower double click, its first click paused already: the pause is put back with the full screen
        const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
        timedClick(ui.window, thirdPlace, 1'000'000);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        QVERIFY(!stub.paused);
        timedClick(ui.window, thirdPlace, 1'000'000 + interval - 50);
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);
        QCOMPARE(stub.calls.count(QStringLiteral("togglePause")), 2);
        QVERIFY(stub.paused);
        stub.calls.clear();
        // A click on a bar beside its controls is not a click on the video, once the bars are laid out for the window again
        QQuickItem *bottomBar = ui.item(QStringLiteral("playerBottomBar"));
        QVERIFY(bottomBar);
        QTRY_VERIFY(bottomBar->mapToScene(QPointF(0, bottomBar->height())).y()
                    <= ui.window->height());
        const QPoint onBar = bottomBar->mapToScene(QPointF(bottomBar->width() / 2, 4)).toPoint();
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, onBar);
        QTest::mouseDClick(ui.window, Qt::LeftButton, Qt::NoModifier, onBar);
        QTest::qWait(400);
        QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));
        QCOMPARE(ui.window->visibility(), QWindow::Windowed);
        // A click that closes a menu is spent on the menu
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, onVideo);
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTest::qWait(400);
        QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));
        // Nothing to pause while the item opens
        stub.loading = true;
        Q_EMIT stub.changed();
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, onVideo + QPoint(20, 0));
        QTest::qWait(400);
        QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));
        stub.loading = false;
        Q_EMIT stub.changed();
        // The bars stay up while paused: playing again lets them hide
        stub.paused = false;
        Q_EMIT stub.changed();
        // A tap on a touch screen only shows the bars
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QQuickItem *osd = ui.item(QStringLiteral("playerOsd"));
        QVERIFY(timer && osd);
        restPointerOnVideo(ui);
        timer->setProperty("interval", 300);
        QTRY_VERIFY(!osd->isVisible());
        QPointingDevice *touch = QTest::createTouchDevice();
        QTest::touchEvent(ui.window, touch).press(0, onVideo);
        QTest::touchEvent(ui.window, touch).release(0, onVideo);
        QTRY_VERIFY(osd->isVisible());
        QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));
    }

    void playerRightClickOpensTheContextMenuAtThePointerAndPausesNothing()
    {
        PlayerStub stub;
        stub.hasPrevious = true;
        stub.hasNext = true;
        stub.audioTracks = {QVariantMap{{QStringLiteral("id"), 1},
                                        {QStringLiteral("name"), QStringLiteral("English")},
                                        {QStringLiteral("selected"), true}},
                            QVariantMap{{QStringLiteral("id"), 2},
                                        {QStringLiteral("name"), QStringLiteral("Italian")},
                                        {QStringLiteral("selected"), false}}};
        stub.subtitleTracks = {QVariantMap{{QStringLiteral("id"), 3},
                                           {QStringLiteral("name"), QStringLiteral("English")},
                                           {QStringLiteral("selected"), false}}};
        stub.externalSubtitles = {QVariantMap{{QStringLiteral("index"), 4},
                                              {QStringLiteral("name"), QStringLiteral("French")}}};
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *menu = ui.popup(QStringLiteral("playerContextMenu"));
        QVERIFY(menu);
        restPointerOnVideo(ui);
        const QPoint at(ui.window->width() / 3, ui.window->height() / 3);
        QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier, at);
        QTRY_VERIFY(menu->property("opened").toBool());
        // At the pointer
        const QPointF origin = page->mapFromScene(QPointF(at));
        QVERIFY(qAbs(menu->property("x").toReal() - origin.x()) <= 1);
        // At the pointer, moved up only as far as a menu taller than the room below it must go to stay in the window
        const qreal top = menu->property("y").toReal();
        QVERIFY2(qAbs(top - origin.y()) <= 1
                     || (top < origin.y()
                         && top + menu->property("height").toReal() <= page->height() + 1),
                 qPrintable(QStringLiteral("menu at %1, pointer at %2").arg(top).arg(origin.y())));
        // The right click neither pauses nor arms the single click's wait
        QTest::qWait(400);
        QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));
        QCOMPARE(ui.window->visibility(), QWindow::Windowed);
        // The entries, separators aside: the bars' controls, the two track menus, the window, then every entry of More Actions, then the lock
        QStringList entries;
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem *entry = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, entry),
                                      Q_ARG(int, i));
            if (entry && entry->isVisible() && !entry->inherits("QQuickMenuSeparator"))
                entries << entry->property("text").toString();
        }
        QCOMPARE(
            entries,
            QStringList({QStringLiteral("Pause"), QStringLiteral("Previous"),
                         QStringLiteral("Next"), QStringLiteral("Audio"),
                         QStringLiteral("Subtitles"), QStringLiteral("Full Screen Mode"),
                         QStringLiteral("More Actions"), QStringLiteral("Keep Controls Visible")}));
        // The track submenus list what the bars' menus list
        for (const auto &[bar, context] :
             {std::pair{QStringLiteral("playerAudioMenu"),
                        QStringLiteral("playerContextAudioMenu")},
              std::pair{QStringLiteral("playerSubtitlesMenu"),
                        QStringLiteral("playerContextSubtitlesMenu")}}) {
            QObject *barMenu = ui.popup(bar);
            QObject *contextSub = ui.popup(context);
            QVERIFY2(barMenu && contextSub, qPrintable(bar));
            QTRY_COMPARE(contextSub->property("count").toInt(), barMenu->property("count").toInt());
            QVERIFY(contextSub->property("count").toInt() >= 2);
        }
        QVERIFY(Ui::closeMenu(menu));
        // A left click still pauses, once the one that closed the menu is over
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, at);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
    }

    void playerContextMenuEntriesDoWhatTheBarsDo()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *menu = ui.popup(QStringLiteral("playerContextMenu"));
        QQuickItem *overlay = ui.item(QStringLiteral("playbackInfoOverlay"));
        QVERIFY(menu && overlay);
        const auto choose = [&](const char *name) {
            QVERIFY(QMetaObject::invokeMethod(menu, "open"));
            QTRY_VERIFY(menu->property("opened").toBool());
            QObject *entry = menu->findChild<QObject *>(QString::fromLatin1(name));
            if (!entry)
                entry = page->findChild<QObject *>(QString::fromLatin1(name));
            QVERIFY2(entry, name);
            QVERIFY(QMetaObject::invokeMethod(entry, "click"));
            QTRY_VERIFY(!menu->property("visible").toBool());
        };
        choose("playerContextPauseItem");
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        choose("playerContextMorePlaybackInfoItem");
        if (QTest::currentTestFailed())
            return;
        QTRY_VERIFY(overlay->isVisible());
        choose("playerContextFullScreenItem");
        if (QTest::currentTestFailed())
            return;
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
    }

    void playerMenuKeyAndShiftF10OpenTheContextMenuAtTheCentre()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *menu = ui.popup(QStringLiteral("playerContextMenu"));
        QVERIFY(menu);
        const std::pair<Qt::Key, Qt::KeyboardModifiers> keys[] = {{Qt::Key_Menu, Qt::NoModifier},
                                                                  {Qt::Key_F10, Qt::ShiftModifier}};
        for (const auto &[key, modifiers] : keys) {
            QTest::keyClick(ui.window, key, modifiers);
            QTRY_VERIFY(menu->property("opened").toBool());
            QVERIFY(qAbs(menu->property("x").toReal() - page->width() / 2) <= 1);
            // At the centre, moved up only as far as a menu taller than the room below it must go to stay in the window
            const qreal top = menu->property("y").toReal();
            QVERIFY2(qAbs(top - page->height() / 2) <= 1
                         || (top < page->height() / 2
                             && top + menu->property("height").toReal() <= page->height() + 1),
                     qPrintable(QString::number(top)));
            QVERIFY(Ui::closeMenu(menu));
        }
        // F10 alone is no menu key
        QTest::keyClick(ui.window, Qt::Key_F10);
        QTest::qWait(200);
        QVERIFY(!menu->property("visible").toBool());
    }

    void playerContextMenuKeepsTheBarsUpWhileOpenAndLetsThemGoAfterIt()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QQuickItem *osd = ui.item(QStringLiteral("playerOsd"));
        QObject *menu = ui.popup(QStringLiteral("playerContextMenu"));
        QVERIFY(timer && osd && menu);
        timer->setProperty("interval", 300);
        restPointerOnVideo(ui);
        QTest::mouseClick(ui.window, Qt::RightButton, Qt::NoModifier,
                          QPoint(ui.window->width() / 3, ui.window->height() / 3));
        QTRY_VERIFY(menu->property("opened").toBool());
        QTest::qWait(800);
        QVERIFY(osd->isVisible());
        QVERIFY(Ui::closeMenu(menu));
        restPointerOnVideo(ui);
        QTRY_VERIFY(!osd->isVisible());
    }

    void playerDoubleClickDoesNotTogglePauseWhenTheFirstClickChangedNothing()
    {
        PlayerStub stub;
        stub.pauseChangesNothing = true;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(openPlayer(ui, stub));
        const QPoint onVideo(ui.window->width() / 2, ui.window->height() / 2);
        restPointerOnVideo(ui);
        // A slow double click whose first click asked for a pause that did not happen: nothing to put back
        const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
        timedClick(ui.window, onVideo, 1'000'000);
        QTRY_COMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        timedClick(ui.window, onVideo, 1'000'000 + interval - 50);
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        QCOMPARE(stub.calls.count(QStringLiteral("togglePause")), 1);
        QVERIFY(!stub.paused);
    }

    void playerPlaybackInfoOverlayOpensFromTheMenuAndShowsTheInfo()
    {
        PlayerStub stub;
        stub.info = {{QStringLiteral("playMethod"), QStringLiteral("Transcode")},
                     {QStringLiteral("streamType"), QStringLiteral("HLS")},
                     {QStringLiteral("protocol"), QStringLiteral("https")},
                     {QStringLiteral("videoWidth"), 1920},
                     {QStringLiteral("videoHeight"), 1080},
                     {QStringLiteral("droppedFrames"), 3},
                     {QStringLiteral("decoderDroppedFrames"), 0},
                     {QStringLiteral("hwdec"), QStringLiteral("vaapi")},
                     {QStringLiteral("targetVideoCodec"), QStringLiteral("h264")},
                     {QStringLiteral("targetAudioCodec"), QStringLiteral("aac")},
                     {QStringLiteral("transcodeReasons"),
                      QStringList{QStringLiteral("VideoCodecNotSupported"),
                                  QStringLiteral("NoSuchReasonYet"),
                                  QStringLiteral("AudioChannelsNotSupported")}},
                     // Remote text is shown as it is, never as markup
                     {QStringLiteral("container"), QStringLiteral("<b>mkv</b> &amp; <img src=x>")},
                     {QStringLiteral("sizeBytes"), qint64(3) * 1024 * 1024 * 1024},
                     {QStringLiteral("bitrate"), 8500000},
                     {QStringLiteral("videoCodec"), QStringLiteral("hevc")},
                     {QStringLiteral("videoProfile"), QStringLiteral("Main 10")},
                     {QStringLiteral("videoLevel"), 120},
                     {QStringLiteral("videoBitrate"), 7000000},
                     {QStringLiteral("videoFps"), 23.976},
                     {QStringLiteral("videoBitDepth"), 10},
                     {QStringLiteral("videoPixelFormat"), QStringLiteral("yuv420p10le")},
                     {QStringLiteral("videoRange"), QStringLiteral("HDR10")},
                     {QStringLiteral("colorSpace"), QStringLiteral("bt2020nc")},
                     {QStringLiteral("colorPrimaries"), QStringLiteral("bt2020")},
                     {QStringLiteral("colorTransfer"), QStringLiteral("smpte2084")},
                     {QStringLiteral("audioCodec"), QStringLiteral("eac3")},
                     {QStringLiteral("audioChannels"), 6},
                     {QStringLiteral("audioBitrate"), 640000},
                     {QStringLiteral("audioSampleRate"), 48000}};
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.opensWindowOfWidth(1000));
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        // The bars stay up while the menu is worked, whatever the speed of the machine
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QVERIFY(timer);
        timer->setProperty("interval", 60000);
        QQuickItem *overlay = ui.item(QStringLiteral("playbackInfoOverlay"));
        QVERIFY(overlay);
        QVERIFY(!overlay->isVisible());
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(menu);
        ui.clickCenter(ui.item(QStringLiteral("playerMoreButton")));
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject *entry = menu->findChild<QObject *>(QStringLiteral("playerPlaybackInfoItem"));
        QVERIFY(entry);
        QCOMPARE(entry->property("text").toString(), QStringLiteral("Playback Info"));
        QVERIFY(entry->property("checkable").toBool());
        QVERIFY(!entry->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(entry, "click"));
        QTRY_VERIFY(overlay->isVisible());
        QTRY_VERIFY(entry->property("checked").toBool());
        QTRY_VERIFY(!menu->property("visible").toBool());

        const auto value = [&ui](const QString &key) -> QString {
            const QQuickItem *label = ui.item(QStringLiteral("playbackInfoValue_") + key);
            return label ? label->property("text").toString() : QStringLiteral("<no row>");
        };
        // The sections and rows of the stub's answer, each as plain text
        QTRY_COMPARE(value(QStringLiteral("player")), QStringLiteral("mpv (Transcoding)"));
        QCOMPARE(value(QStringLiteral("streamType")), QStringLiteral("HLS (https)"));
        const QQuickItem *video = ui.item(QStringLiteral("videoRenderer"));
        QVERIFY(video);
        QCOMPARE(value(QStringLiteral("sizes")),
                 QStringLiteral("%1\u00d7%2 / 1920\u00d71080")
                     .arg(qRound(video->width() * ui.window->devicePixelRatio()))
                     .arg(qRound(video->height() * ui.window->devicePixelRatio())));
        QCOMPARE(value(QStringLiteral("hwdec")), QStringLiteral("vaapi"));
        QCOMPARE(value(QStringLiteral("dropped")), QStringLiteral("3"));
        QCOMPARE(value(QStringLiteral("decoderDropped")), QStringLiteral("0"));
        QCOMPARE(ui.item(QStringLiteral("playbackInfoValue_decoderDropped"))
                     ->parentItem()
                     ->childItems()
                     .constFirst()
                     ->property("text")
                     .toString(),
                 QStringLiteral("Frames dropped by the decoder"));
        QVERIFY(ui.item(QStringLiteral("playbackInfoSection_transcoding")));
        QCOMPARE(ui.item(QStringLiteral("playbackInfoSectionTitle_playback"))
                     ->property("text")
                     .toString(),
                 QStringLiteral("Playback Info"));
        QCOMPARE(ui.item(QStringLiteral("playbackInfoSectionTitle_transcoding"))
                     ->property("text")
                     .toString(),
                 QStringLiteral("Transcoding Info"));
        QCOMPARE(
            ui.item(QStringLiteral("playbackInfoSectionTitle_media"))->property("text").toString(),
            QStringLiteral("Original Media Info"));
        QCOMPARE(value(QStringLiteral("targetCodecs")), QStringLiteral("h264, aac"));
        // A known reason is a sentence, an unknown name is left out
        QCOMPARE(value(QStringLiteral("reasons")),
                 QStringLiteral("The video codec is not supported\n"
                                "The number of audio channels is not supported"));
        const QString container = value(QStringLiteral("container"));
        QVERIFY2(container.startsWith(QStringLiteral("<b>mkv</b> &amp; <img src=x>")),
                 qPrintable(container));
        QVERIFY2(container.contains(QStringLiteral("GiB")), qPrintable(container));
        QVERIFY2(container.contains(QStringLiteral("Mbps")), qPrintable(container));
        QCOMPARE(value(QStringLiteral("videoCodec")), QStringLiteral("hevc"));
        const QString videoAttributes = value(QStringLiteral("videoAttributes"));
        QVERIFY2(videoAttributes.startsWith(QStringLiteral("Main 10 \u00b7 Lv 120")),
                 qPrintable(videoAttributes));
        QVERIFY2(videoAttributes.contains(QStringLiteral("fps")), qPrintable(videoAttributes));
        QVERIFY(value(QStringLiteral("videoBitDepth")).contains(QStringLiteral("yuv420p10le")));
        QVERIFY(value(QStringLiteral("videoBitDepth")).contains(QStringLiteral("HDR10")));
        QVERIFY(value(QStringLiteral("videoColors")).contains(QStringLiteral("smpte2084")));
        QCOMPARE(value(QStringLiteral("audioCodec")), QStringLiteral("eac3"));
        const QString audioAttributes = value(QStringLiteral("audioAttributes"));
        QVERIFY2(audioAttributes.contains(QStringLiteral("6 Ch"))
                     && audioAttributes.contains(QStringLiteral("640 kbps"))
                     && audioAttributes.contains(QStringLiteral("Hz")),
                 qPrintable(audioAttributes));
        // Every text of the panel is plain text
        int labels = 0;
        const auto inButton = [overlay](const QQuickItem *item) {
            for (; item && item != overlay; item = item->parentItem()) {
                if (item->inherits("QQuickAbstractButton"))
                    return true;
            }
            return false;
        };
        for (const QQuickItem *item : allItems(overlay)) {
            if (!item->isVisible() || !item->inherits("QQuickText") || inButton(item))
                continue;
            ++labels;
            QVERIFY2(showsPlainText(item), qPrintable(item->property("text").toString()));
        }
        QVERIFY(labels > 10);
        // The panel sits at the top left, inside the window and clear of the bottom bar
        const QRectF area = overlay->mapRectToScene(overlay->boundingRect());
        QVERIFY(area.left() >= 0 && area.right() <= ui.window->width());
        QVERIFY(area.bottom() <= ui.item(QStringLiteral("playerBottomBar"))
                                     ->mapRectToScene(QRectF(0, 0, 1, 1))
                                     .top());

        // It reads the player again every second while it shows
        stub.info.insert(QStringLiteral("droppedFrames"), 7);
        QTRY_COMPARE(value(QStringLiteral("dropped")), QStringLiteral("7"));
        // Without a transcode there is no transcoding section, and the method says so
        stub.info.insert(QStringLiteral("playMethod"), QStringLiteral("DirectPlay"));
        QTRY_COMPARE(value(QStringLiteral("player")), QStringLiteral("mpv (Direct Play)"));
        QTRY_VERIFY(!ui.item(QStringLiteral("playbackInfoSection_transcoding")));
        // Hardware decoding off and rows without a value are left out, the unknown hardware decoder among them
        stub.info.insert(QStringLiteral("hwdec"), QStringLiteral("no"));
        stub.info.remove(QStringLiteral("audioCodec"));
        QTRY_COMPARE(value(QStringLiteral("hwdec")), QStringLiteral("Off"));
        QTRY_VERIFY(!ui.item(QStringLiteral("playbackInfoValue_audioCodec")));
        stub.info.remove(QStringLiteral("hwdec"));
        QTRY_VERIFY(!ui.item(QStringLiteral("playbackInfoValue_hwdec")));

        // A resize changes the sizes row alone: the rows are not built again
        const QPointer<QQuickItem> playerRow = ui.item(QStringLiteral("playbackInfoValue_player"));
        QVERIFY(playerRow);
        const QString sizes = value(QStringLiteral("sizes"));
        QVERIFY(ui.opensWindowOfWidth(1100));
        QTRY_VERIFY(value(QStringLiteral("sizes")) != sizes);
        QCOMPARE(ui.item(QStringLiteral("playbackInfoValue_player")), playerRow.data());
        // The panel scrolls with a bar of its own and is a pane with a name
        bool scrollBar = false;
        for (const QQuickItem *item : allItems(overlay))
            scrollBar = scrollBar || item->inherits("QQuickScrollBar");
        QVERIFY(scrollBar);
        QCOMPARE(accessibleNameOf(overlay), QStringLiteral("Playback Info"));

        // A click on the panel is not a click on the video
        stub.calls.clear();
        QTest::mouseClick(
            ui.window, Qt::LeftButton, Qt::NoModifier,
            overlay->mapToScene(QPointF(overlay->width() / 2, overlay->height() - 2)).toPoint());
        QTest::qWait(100);
        QVERIFY(!stub.calls.contains(QStringLiteral("togglePause")));

        // It stays when the bars go
        QQuickItem *osd = ui.item(QStringLiteral("playerOsd"));
        QVERIFY(osd);
        restPointerOnVideo(ui);
        timer->setProperty("interval", 300);
        QTRY_VERIFY(!osd->isVisible());
        QTest::qWait(300);
        QVERIFY(overlay->isVisible());

        // The close button has an accessible name and closes it, and the player stays
        QQuickItem *close = ui.item(QStringLiteral("playbackInfoCloseButton"));
        QVERIFY(close && close->isVisible());
        QCOMPARE(close->property("text").toString(), QStringLiteral("Close Playback Info"));
        QCOMPARE(close->property("display").toInt(), 0);
        ui.clickCenter(close);
        QTRY_VERIFY(!overlay->isVisible());
        QCOMPARE(ui.currentDetails(), page);
        QObject *infoTimer = overlay->findChild<QObject *>(QStringLiteral("playbackInfoTimer"));
        QVERIFY(infoTimer);
        QVERIFY(!infoTimer->property("running").toBool());
    }

    void playerPlaybackInfoSaysItWaitsWhileThereIsNothingToShow()
    {
        PlayerStub stub;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QQuickItem *overlay = ui.item(QStringLiteral("playbackInfoOverlay"));
        QQuickItem *waiting = ui.item(QStringLiteral("playbackInfoWaiting"));
        QVERIFY(overlay && waiting);
        QVERIFY(page->setProperty("playbackInfoShown", true));
        QTRY_VERIFY(overlay->isVisible());
        // While the item opens or restarts one line takes the place of the rows
        QTRY_VERIFY(waiting->isVisible());
        QCOMPARE(waiting->property("text").toString(), QStringLiteral("Waiting for playback"));
        QVERIFY(showsPlainText(waiting));
        QVERIFY(!ui.item(QStringLiteral("playbackInfoSection_playback")));
        stub.info = {{QStringLiteral("playMethod"), QStringLiteral("DirectPlay")}};
        QTRY_VERIFY(ui.item(QStringLiteral("playbackInfoSection_playback")));
        QVERIFY(!waiting->isVisible());
        stub.info.clear();
        QTRY_VERIFY(waiting->isVisible());
        QVERIFY(!ui.item(QStringLiteral("playbackInfoSection_playback")));
    }

    void playerEscapeClosesThePlaybackInfoBeforeLeavingThePlayer()
    {
        PlayerStub stub;
        stub.info = {{QStringLiteral("playMethod"), QStringLiteral("DirectPlay")}};
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        QObject *timer = page->findChild<QObject *>(QStringLiteral("playerChromeTimer"));
        QVERIFY(timer);
        timer->setProperty("interval", 60000);
        QQuickItem *overlay = ui.item(QStringLiteral("playbackInfoOverlay"));
        QObject *menu = ui.popup(QStringLiteral("playerMoreMenu"));
        QVERIFY(overlay && menu);
        // The menu opens through its own method: in a full-screen window the button can lie outside the window the test clicks in
        const auto open = [&] {
            QVERIFY(QMetaObject::invokeMethod(menu, "open"));
            QTRY_VERIFY(menu->property("opened").toBool());
            QObject *entry = menu->findChild<QObject *>(QStringLiteral("playerPlaybackInfoItem"));
            QVERIFY(entry);
            QVERIFY(QMetaObject::invokeMethod(entry, "click"));
            QTRY_VERIFY(overlay->isVisible());
            QTRY_VERIFY(!menu->property("visible").toBool());
        };
        open();
        if (QTest::currentTestFailed())
            return;
        // Esc closes the panel and stays; a second one leaves the player
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!overlay->isVisible());
        QTest::qWait(100);
        QCOMPARE(ui.currentDetails(), page);
        QObject *entry = menu->findChild<QObject *>(QStringLiteral("playerPlaybackInfoItem"));
        QVERIFY(entry && !entry->property("checked").toBool());
        // The panel comes back from the menu, and it closes before the full screen the player entered
        QVERIFY(QMetaObject::invokeMethod(ui.engine.rootObjects().constFirst(),
                                          "togglePlayerFullscreen"));
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        open();
        if (QTest::currentTestFailed())
            return;
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY(!overlay->isVisible());
        QTest::qWait(100);
        QCOMPARE(ui.window->visibility(), QWindow::FullScreen);
        QCOMPARE(ui.currentDetails(), page);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);
        QCOMPARE(ui.currentDetails(), page);
        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
    }

    void remoteHomeAndBack()
    {
        RemoteStub remote;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(attach(ui, "remoteInput", &remote));
        QTRY_VERIFY(ui.item(QStringLiteral("homePage")) != nullptr);

        // Back on the first page has nowhere to go
        Q_EMIT remote.actionRequested(QStringLiteral("back"));
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QCOMPARE(ui.stackDepth(), 1);

        // Back leaves the details page like Esc does
        QVERIFY(openDetails(ui, kMovieId));
        QTRY_COMPARE(ui.stackDepth(), 2);
        Q_EMIT remote.actionRequested(QStringLiteral("back"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_COMPARE(ui.stackDepth(), 1);

        // With a popup open Back never moves the page under it
        QVERIFY(openDetails(ui, kMovieId));
        QTRY_COMPARE(ui.stackDepth(), 2);
        QObject *menu = ui.popup(QStringLiteral("mainMenu"));
        QVERIFY(menu && QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());
        Q_EMIT remote.actionRequested(QStringLiteral("back"));
        QCOMPARE(ui.stackDepth(), 2);
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());

        // Home drops the pages above the Home tab
        Q_EMIT remote.actionRequested(QStringLiteral("home"));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
        QTRY_COMPARE(ui.stackDepth(), 1);

        // A name nobody knows does nothing
        Q_EMIT remote.actionRequested(QStringLiteral("bogus"));
        QCOMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
    }

    void remotePlayerActionsNeedPlayerPage()
    {
        PlayerStub stub;
        RemoteStub remote;
        Ui ui(m_home->path());
        ui.serveMovie();
        QVERIFY(ui.load());
        QVERIFY(ui.signIn());
        QVERIFY(attach(ui, "remoteInput", &remote));
        QVERIFY(attach(ui, "player", &stub));
        const QStringList commands =
            QStringLiteral("playpause play pause stop next previous fastforward rewind volumeup "
                           "volumedown togglemute togglefullscreen")
                .split(QLatin1Char(' '));

        // Without a playback nothing reaches the player
        for (const QString &command : commands)
            Q_EMIT remote.actionRequested(command);
        QVERIFY(stub.calls.isEmpty());
        QCOMPARE(ui.window->visibility(), QWindow::Windowed);

        QQuickItem *page = openPlayer(ui, stub);
        QVERIFY(page);
        stub.calls.clear();
        const QStringList sequence =
            QStringLiteral("playpause pause pause play play next previous fastforward rewind "
                           "volumedown volumeup togglemute bogus")
                .split(QLatin1Char(' '));
        for (const QString &command : sequence)
            Q_EMIT remote.actionRequested(command);
        // Pause and play act only on the state they change; the stub starts at full volume
        QCOMPARE(stub.calls,
                 QStringList({QStringLiteral("togglePause"), QStringLiteral("resume"),
                              QStringLiteral("next"), QStringLiteral("previous"),
                              QStringLiteral("seekRelative 10"), QStringLiteral("seekRelative -10"),
                              QStringLiteral("setVolume 95"), QStringLiteral("setVolume 100"),
                              QStringLiteral("toggleMute")}));
        QCOMPARE(ui.currentDetails(), page);

        Q_EMIT remote.actionRequested(QStringLiteral("togglefullscreen"));
        QTRY_COMPARE(ui.window->visibility(), QWindow::FullScreen);
        Q_EMIT remote.actionRequested(QStringLiteral("togglefullscreen"));
        QTRY_COMPARE(ui.window->visibility(), QWindow::Windowed);

        // Stop leaves the player page
        stub.calls.clear();
        Q_EMIT remote.actionRequested(QStringLiteral("stop"));
        QCOMPARE(stub.calls, QStringList({QStringLiteral("stop")}));
        QTRY_COMPARE(ui.currentDetails()->objectName(), QStringLiteral("homePage"));
    }

  private:
    std::unique_ptr<QTemporaryDir> m_home;
};

// Same application class and Quick Controls style as src/main.cpp, so the tests run on the style the app ships
int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_Use96Dpi, true);
    QApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
        // Inferred: name() only echoes the request, so the module is looked for in the import paths
        const QStringList paths = QQmlEngine().importPathList();
        const bool installed = std::any_of(paths.cbegin(), paths.cend(), [](const QString &path) {
            return QFile::exists(path + QStringLiteral("/org/kde/desktop/qmldir"));
        });
        if (QQuickStyle::name() != QLatin1String("org.kde.desktop") || !installed) {
            qCritical("The Quick Controls style org.kde.desktop is not installed (searched: %s)",
                      qPrintable(paths.join(QLatin1Char(' '))));
            return 1;
        }
    }
    TestUiPages test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_ui_pages.moc"
