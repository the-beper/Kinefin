// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/ConnectionController.h"
#include "app/ImageNetworkFactory.h"
#include "app/InputFilter.h"
#include "app/Logging.h"
#include "app/ProfileCommands.h"
#include "integration/GamepadInput.h"
#include "integration/KRunnerProvider.h"
#include "integration/LinkRouter.h"
#include "integration/Mpris.h"
#include "integration/MprisArtwork.h"
#include "integration/RemoteInputServer.h"
#include "integration/ScreenSaverInhibitor.h"
#include "player/PlayerController.h"
#include "settings/ClientSettings.h"
#include "settings/KWalletTokenStore.h"
#include "settings/ProfileIdentity.h"
#include "settings/ProfileIndex.h"
#include "settings/ProfilePaths.h"
#include "syncplay/SyncPlayController.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageBox>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QVariant>
#include <KAboutData>
#include <KDBusService>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <memory>
#include <optional>

namespace {
// Qt reads QT_QPA_PLATFORM before QApplication exists, so the option is applied here; the parser also accepts it for --help and for the `--platform=<p>` form Qt does not take itself
void applyPlatformOption(int argc, char **argv)
{
    const QByteArray prefix = QByteArrayLiteral("--platform=");
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        if (argument == "--platform" && i + 1 < argc) {
            qputenv("QT_QPA_PLATFORM", argv[i + 1]);
            return;
        }
        if (argument.startsWith(prefix)) {
            qputenv("QT_QPA_PLATFORM", argument.mid(prefix.size()));
            return;
        }
    }
}
} // namespace

int main(int argc, char *argv[])
{
    // mpvqt uses QQuickFramebufferObject, which requires Qt Quick's OpenGL graphics API. Select it before any QQuickWindow is constructed
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    applyPlatformOption(argc, argv);
    QApplication app(argc, argv);
    // org.kde.desktop needs QApplication; an environment variable naming another style wins
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QCoreApplication::setApplicationName(QStringLiteral("Kinefin"));
    // Reversed by KDBusService: io.github.the_beper.Kinefin
    QCoreApplication::setOrganizationDomain(QStringLiteral("the_beper.github.io"));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.the_beper.Kinefin"));
    // Stock icons come from Breeze when the platform theme names a theme that lacks them; the window icon falls back to a stock one while the app icon is not installed
    QIcon::setFallbackThemeName(QStringLiteral("breeze"));
    QApplication::setWindowIcon(
        QIcon::fromTheme(QStringLiteral("io.github.the_beper.Kinefin"),
                         QIcon::fromTheme(QStringLiteral("video-television"))));
    KLocalizedString::setApplicationDomain("kinefin");
    KAboutData aboutData(QStringLiteral("Kinefin"), QStringLiteral("Kinefin"),
                         QStringLiteral(KINEFIN_VERSION));
    // KAboutData::setApplicationData overwrites QCoreApplication metadata; set these on aboutData, not only on QCoreApplication, or the D-Bus service name becomes io.github.the_beper.kinefin (wrong case)
    aboutData.setOrganizationDomain(QByteArrayLiteral("the_beper.github.io"));
    aboutData.setDesktopFileName(QStringLiteral("io.github.the_beper.Kinefin"));
    aboutData.setShortDescription(i18n("A native Jellyfin client for KDE Plasma"));
    aboutData.setLicense(KAboutLicense::GPL_V3);
    aboutData.setCopyrightStatement(QStringLiteral("© 2026 Kinefin contributors"));
    aboutData.setHomepage(QStringLiteral("https://github.com/the-beper/Kinefin"));
    aboutData.setBugAddress("https://github.com/the-beper/Kinefin/issues");
    KAboutData::setApplicationData(aboutData);
    QCoreApplication::setApplicationVersion(QStringLiteral(KINEFIN_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Kinefin native Jellyfin client"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("url"),
                                 QStringLiteral("jellyfin://item/<32-hex-id> URL"),
                                 QStringLiteral("[url]"));
    parser.addOption(
        {QStringLiteral("log-level"),
         QStringLiteral("Log level of the log file: debug, info, warn, error, fatal or disable."),
         QStringLiteral("level")});
    parser.addOption({QStringLiteral("fullscreen"), QStringLiteral("Start in full screen.")});
    parser.addOption({QStringLiteral("windowed"), QStringLiteral("Start in a window.")});
    parser.addOption(
        {QStringLiteral("desktop"), QStringLiteral("Start the desktop interface (the only one).")});
    parser.addOption({QStringLiteral("tv"), QStringLiteral("Not supported.")});
    parser.addOption({QStringLiteral("platform"), QStringLiteral("Equivalent to QT_QPA_PLATFORM."),
                      QStringLiteral("platform")});
    parser.addOption({QStringLiteral("config-dir"),
                      QStringLiteral("Store profiles, downloads and caches under this directory "
                                     "instead of the XDG directories."),
                      QStringLiteral("path")});
    parser.addOption({{QStringLiteral("l"), QStringLiteral("licenses")},
                      QStringLiteral("Show the GPL license text and exit.")});
    parser.addOption({QStringLiteral("ignore-certificate-errors"),
                      QStringLiteral("Accept invalid TLS certificates from the server for this run "
                                     "(unsafe).")});
    Kinefin::addProfileOptions(parser);
    parser.process(app);
    if (parser.isSet(QStringLiteral("licenses"))) {
        QFile licenses(QStringLiteral(":/GPL-3.0-or-later.txt"));
        if (!licenses.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCritical("The license text is missing from this build");
            return 1;
        }
        QTextStream(stdout) << QString::fromUtf8(licenses.readAll());
        return 0;
    }
    if (parser.isSet(QStringLiteral("config-dir"))) {
        const QString value = parser.value(QStringLiteral("config-dir"));
        if (value.isEmpty()) {
            QTextStream(stderr) << "kinefin: --config-dir needs a path" << Qt::endl;
            return 2;
        }
        const QString root = QDir::cleanPath(QFileInfo(value).absoluteFilePath());
        if (!QFileInfo(root).dir().exists()) {
            QTextStream(stderr) << "kinefin: the parent of the config directory does not exist"
                                << Qt::endl;
            return 1;
        }
        // The root is made owner-only: a folder that already holds other files (a home, ~/Documents) must not have its permissions changed
        const QDir rootDir(root);
        if (QFileInfo(root).exists()
            && (!QFileInfo(root).isDir()
                || (!rootDir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                     | QDir::System)
                    && !rootDir.exists(QStringLiteral("profiles.json"))))) {
            QTextStream(stderr)
                << "kinefin: the config directory must be new, empty or an existing "
                   "Kinefin config directory"
                << Qt::endl;
            return 1;
        }
        Kinefin::setProfileRoot(root);
    }
    if (parser.isSet(QStringLiteral("tv"))) {
        qCritical("The TV interface is not supported");
        return 2;
    }
    if (parser.isSet(QStringLiteral("fullscreen")) && parser.isSet(QStringLiteral("windowed"))) {
        qCritical("--fullscreen and --windowed exclude each other");
        return 2;
    }
    if (parser.positionalArguments().size() > 1) {
        qCritical("Only one URL can be handled at startup");
        return 2;
    }
    if (!parser.positionalArguments().isEmpty()
        && !Kinefin::parseItemLink(parser.positionalArguments().first())) {
        qCritical("Unsupported or unsafe Jellyfin URL");
        return 2;
    }
    std::optional<Kinefin::LogLevel> cliLogLevel;
    if (parser.isSet(QStringLiteral("log-level"))) {
        cliLogLevel = Kinefin::parseLogLevel(parser.value(QStringLiteral("log-level")));
        if (!cliLogLevel) {
            qCritical("Unknown log level");
            return 2;
        }
    }

    QString profileError;
    if (!Kinefin::ensureDefaultProfileIndex(&profileError)) {
        QTextStream(stderr) << "kinefin: " << profileError << Qt::endl;
        return 1;
    }
    // The profile commands run before the single-instance service: they work while the app is open
    {
        QTextStream out(stdout);
        QTextStream err(stderr);
        Kinefin::KWalletTokenStore store;
        if (const auto exitCode = Kinefin::runProfileCommand(parser, store, out, err))
            return *exitCode;
    }
    const QString profileId = Kinefin::startupProfileId(parser, &profileError);
    if (profileId.isEmpty()) {
        QTextStream(stderr) << "kinefin: " << profileError << Qt::endl;
        return 2;
    }

    // Register the activation handler before creating the shell. A second launch may arrive while the first is still restoring its profile or loading QML. Keep only the latest item request, without ever logging its URI
    QPointer<QQuickWindow> window;
    std::optional<QString> pendingItemId;
    if (!parser.positionalArguments().isEmpty())
        pendingItemId = Kinefin::parseItemLink(parser.positionalArguments().first());
    // A second launch, a link and MPRIS's Raise all bring the window up the same way
    auto raiseWindow = [&window] {
        if (!window)
            return;
        // show() keeps a minimized state; clearing only that flag keeps fullscreen and maximized
        if (window->windowStates() & Qt::WindowMinimized)
            window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
        window->show();
        window->raise();
        window->requestActivate();
    };
    auto showItem = [&window, &pendingItemId, raiseWindow](const QString &itemId) {
        if (!window) {
            pendingItemId = itemId;
            return;
        }
        window->setProperty("requestedItemId", QVariant(itemId));
        raiseWindow();
    };
    // KRunner hands KRunner's XDG activation token: put it where the toolkit reads it for the window this raises, as KDBusService does for a second launch
    auto withActivationToken = [](const QString &token, auto &&action) {
        const QByteArray previous = qgetenv("XDG_ACTIVATION_TOKEN");
        if (!token.isEmpty())
            qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
        action();
        if (token.isEmpty())
            return;
        if (previous.isEmpty())
            qunsetenv("XDG_ACTIVATION_TOKEN");
        else
            qputenv("XDG_ACTIVATION_TOKEN", previous);
    };
    KDBusService service(KDBusService::Unique);
    QObject::connect(&service, &KDBusService::activateRequested, &app,
                     [showItem, raiseWindow](const QStringList &args, const QString &) {
                         if (const auto item = Kinefin::itemFromActivationArguments(args))
                             showItem(*item);
                         else
                             // Plain second launch: raise without changing pages
                             raiseWindow();
                     });

    const auto profileLock = Kinefin::lockProfile(profileId, &profileError);
    if (!profileLock) {
        QTextStream(stderr) << "kinefin: " << profileError << Qt::endl;
        return 1;
    }
    Kinefin::ClientSettings settings(Kinefin::ClientSettings::pathForProfile(profileId));
    settings.setCertificateErrorsIgnoredByCommandLine(
        parser.isSet(QStringLiteral("ignore-certificate-errors")));
    const auto settingsLevel = Kinefin::parseLogLevel(
        settings.value(QStringLiteral("main"), QStringLiteral("logLevel")).toString());
    // Warnings of the settings constructor (a newer-format file moved aside) precede the log file: they reach stderr only, because the log level is itself a setting
    struct LogGuard {
        ~LogGuard() { Kinefin::removeProfileLog(); }
    } logGuard;
    QString logError;
    if (!Kinefin::installProfileLog(
            Kinefin::profileLogDirectory(profileId),
            cliLogLevel.value_or(settingsLevel.value_or(Kinefin::LogLevel::Debug)), &logError))
        qWarning("No log file for this profile: %s", qPrintable(logError));

    const QString deviceId = Kinefin::profileDeviceId(profileId, &profileError);
    if (deviceId.isEmpty()) {
        qCritical("Unable to read the profile: %s", qPrintable(profileError));
        return 1;
    }
    Kinefin::ConnectionController connection(profileId, deviceId, QStringLiteral(KINEFIN_VERSION));
    connection.setIgnoreCertificateErrors(settings.certificateErrorsIgnored());
    // The download queue follows the client settings of this profile at once
    const auto applyDownloadSettings = [&settings, &connection] {
        connection.downloads()->setParallelDownloads(
            settings.value(QStringLiteral("downloads"), QStringLiteral("parallel")).toInt());
        connection.downloads()->setUnmeteredOnly(
            settings.value(QStringLiteral("downloads"), QStringLiteral("unmeteredOnly")).toBool());
        connection.downloads()->setKodiSidecars(
            settings.value(QStringLiteral("downloads"), QStringLiteral("kodiSidecars")).toBool());
    };
    applyDownloadSettings();
    QObject::connect(
        &settings, &Kinefin::ClientSettings::valueChanged, &connection,
        [&settings, &connection](const QString &section, const QString &key) {
            if (section != QLatin1String("downloads"))
                return;
            if (key == QLatin1String("parallel"))
                connection.downloads()->setParallelDownloads(settings.value(section, key).toInt());
            else if (key == QLatin1String("unmeteredOnly"))
                connection.downloads()->setUnmeteredOnly(settings.value(section, key).toBool());
            else if (key == QLatin1String("kodiSidecars"))
                connection.downloads()->setKodiSidecars(settings.value(section, key).toBool());
        });
    QObject::connect(&settings, &Kinefin::ClientSettings::certificateErrorsIgnoredChanged,
                     &connection, [&connection, &settings] {
                         connection.setIgnoreCertificateErrors(settings.certificateErrorsIgnored());
                     });
    Kinefin::PlayerController player(connection.playbackClient(), *connection.queue(), settings,
                                     profileId);
    QObject::connect(&connection, &Kinefin::ConnectionController::aboutToLeaveSession, &player,
                     &Kinefin::PlayerController::onAccountLeaving);
    // Watching together: the player's actions go to a joined group
    Kinefin::SyncPlayController syncPlay(connection.playbackClient(), *connection.socket(), player);
    player.setGroupPlayback(&syncPlay);
    QObject::connect(&connection, &Kinefin::ConnectionController::stateChanged, &player,
                     [&connection, &player] {
                         if (connection.state() != Kinefin::ConnectionController::Authenticated)
                             player.onAccountLeaving();
                     });

    // A projection change covers a confirmed configuration and the loss of the session (back to the default)
    QObject::connect(&connection, &Kinefin::ConnectionController::homeProjectionChanged, &player,
                     [&connection, &player] {
                         player.setNextEpisodeAutoPlay(connection.nextEpisodeAutoPlay());
                         player.setRememberSelections(connection.rememberAudioSelections(),
                                                      connection.rememberSubtitleSelections());
                     });
    QObject::connect(&connection, &Kinefin::ConnectionController::storedUserForgotten, &player,
                     &Kinefin::PlayerController::forgetStoredUser);
    QObject::connect(&connection, &Kinefin::ConnectionController::storedServerForgotten, &player,
                     &Kinefin::PlayerController::forgetStoredServer);

    const auto imageOrigin = std::make_shared<Kinefin::ImageOrigin>();
    connection.setImageOrigin(imageOrigin);
    // The factory is not owned by the engine: it is declared first so it outlives it
    Kinefin::ImageNetworkFactory imageFactory(imageOrigin, Kinefin::profileCacheDirectory(profileId)
                                                               + QStringLiteral("/images"));
    // Held by the QML shell, so declared before the engine
    Kinefin::RemoteInputServer remoteInput(Kinefin::remoteSocketPath(profileId),
                                           QStringLiteral(KINEFIN_VERSION));
    // SDL gamepads feed the same keycodes as the socket and the keyboard; the filter gates keyboard autorepeat
    Kinefin::GamepadInput gamepad;
    Kinefin::InputFilter inputFilter;
    gamepad.setRepeatEnabled(
        settings.value(QStringLiteral("main"), QStringLiteral("enableInputRepeat")).toBool());
    inputFilter.setRepeatEnabled(
        settings.value(QStringLiteral("main"), QStringLiteral("enableInputRepeat")).toBool());
    inputFilter.setMouseBlocked(
        settings.value(QStringLiteral("main"), QStringLiteral("disablemouse")).toBool());
    qApp->installEventFilter(&inputFilter);
    QObject::connect(&gamepad, &Kinefin::GamepadInput::keycodeRequested, &remoteInput,
                     [&remoteInput](const QString &keycode, bool autoRepeat) {
                         remoteInput.dispatchKeycode(keycode, autoRepeat);
                     });
    QObject::connect(
        &settings, &Kinefin::ClientSettings::valueChanged, &app,
        [&settings, &gamepad, &inputFilter](const QString &section, const QString &key) {
            if (section != QLatin1String("main"))
                return;
            if (key == QLatin1String("sdlEnabled")) {
                if (settings.value(section, key).toBool())
                    gamepad.start();
                else
                    gamepad.stop();
            } else if (key == QLatin1String("enableInputRepeat")) {
                const bool enabled = settings.value(section, key).toBool();
                gamepad.setRepeatEnabled(enabled);
                inputFilter.setRepeatEnabled(enabled);
            } else if (key == QLatin1String("disablemouse")) {
                inputFilter.setMouseBlocked(settings.value(section, key).toBool());
            }
        });
    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&imageFactory);
    // "fullscreen" or "windowed" when a flag decides this run; empty to follow the setting
    const QString windowMode =
        parser.isSet(QStringLiteral("fullscreen")) ? QStringLiteral("fullscreen")
        : parser.isSet(QStringLiteral("windowed")) ? QStringLiteral("windowed")
                                                   : QString();
    engine.setInitialProperties(
        {{QStringLiteral("connection"), QVariant::fromValue(&connection)},
         {QStringLiteral("settings"), QVariant::fromValue(&settings)},
         {QStringLiteral("player"), QVariant::fromValue(&player)},
         {QStringLiteral("syncPlay"), QVariant::fromValue(&syncPlay)},
         {QStringLiteral("remoteInput"), QVariant::fromValue(&remoteInput)},
         {QStringLiteral("gamepad"), QVariant::fromValue(&gamepad)},
         {QStringLiteral("startupWindowMode"), windowMode},
         {QStringLiteral("startupPage"), qEnvironmentVariable("KINEFIN_START_PAGE")},
         // About names a development Flatpak after its build time; the version stays KINEFIN_VERSION everywhere else
         {QStringLiteral("displayVersion"), QStringLiteral(KINEFIN_DISPLAY_VERSION)}});
    // Register the KI18n QML context so Main.qml can translate its strings
    KLocalization::setupLocalizedContext(&engine);
    engine.loadFromModule(QStringLiteral("org.kde.kinefin"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        qCritical("Could not load the Kinefin QML shell");
        // Only where a user can see it: the offscreen and minimal platforms (tests, headless runs) must not block on a dialog
        const QString platform = QGuiApplication::platformName();
        if (!platform.startsWith(QLatin1String("offscreen"))
            && !platform.startsWith(QLatin1String("minimal")))
            QMessageBox::critical(nullptr, QCoreApplication::applicationName(),
                                  i18n("The Kinefin interface could not be loaded"));
        return 1;
    }
    window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window) {
        qCritical("The QML root is not a window");
        return 1;
    }
    if (pendingItemId) {
        const QString itemId = *pendingItemId;
        pendingItemId.reset();
        showItem(itemId);
    }
    // The desktop's media controls and the screen kept on during playback; Quit closes the window as Ctrl+Q does, so unsaved preferences are asked about first
    Kinefin::Mpris mpris(player, profileId, QDBusConnection::sessionBus());
    QObject::connect(&mpris, &Kinefin::Mpris::raiseRequested, &app, raiseWindow);
    QObject::connect(&mpris, &Kinefin::Mpris::quitRequested, window, &QWindow::close);
    mpris.registerOnBus();
    Kinefin::KRunnerProvider krunner(connection.playbackClient(), QDBusConnection::sessionBus());
    QObject::connect(
        &krunner, &Kinefin::KRunnerProvider::openRequested, &app,
        [&showItem, &withActivationToken](const QString &itemId, const QString &token) {
            withActivationToken(token, [&showItem, itemId] { showItem(itemId); });
        });
    QObject::connect(&krunner, &Kinefin::KRunnerProvider::playRequested, &app,
                     [&raiseWindow, &withActivationToken,
                      &window](const QString &itemId, qint64 ticks, const QString &token) {
                         withActivationToken(token, [&raiseWindow, &window, itemId, ticks] {
                             raiseWindow();
                             if (window)
                                 QMetaObject::invokeMethod(window, "playRequestedItem",
                                                           Q_ARG(QVariant, itemId),
                                                           Q_ARG(QVariant, ticks));
                         });
                     });
    krunner.registerOnBus();
    Kinefin::MprisArtwork mprisArtwork(mpris, player, imageOrigin,
                                       Kinefin::profileCacheDirectory(profileId)
                                           + QStringLiteral("/mpris"));
    Kinefin::ScreenSaverInhibitor screenSaverInhibitor(player, QDBusConnection::sessionBus());
    // Navigation keys from a remote reach the window like a keyboard's; no socket is a warning, never a failure
    remoteInput.setWindow(window);
    remoteInput.listen();
    // Only now is there a window for the activity gate; the setting still decides whether SDL opens anything
    if (settings.value(QStringLiteral("main"), QStringLiteral("sdlEnabled")).toBool())
        gamepad.start();
    // Another Jellyfin session's keys and player commands take the same path
    QObject::connect(connection.remoteCommands(), &Kinefin::RemoteCommands::keycodeRequested,
                     &remoteInput, &Kinefin::RemoteInputServer::dispatchNetworkKeycode);
    return app.exec();
}
