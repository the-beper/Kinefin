// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

using Kinefin::LogLevel;

Q_LOGGING_CATEGORY(testLog, "kinefin.test")

namespace {
int g_seen = 0;

void countingHandler(QtMsgType, const QMessageLogContext &, const QString &)
{
    ++g_seen;
}

QString readText(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}
} // namespace

class TestLogging : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir m_dir;
    QtMessageHandler m_original = nullptr;

    QString logDir() const { return m_dir.filePath(QStringLiteral("logs")); }
    QString logFile(const QString &suffix = QString()) const
    {
        return logDir() + QStringLiteral("/kinefin.log") + suffix;
    }

  private Q_SLOTS:
    void initTestCase()
    {
        // QtTest disables debug output unless asked; these tests need qDebug to reach the handler.
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true"));
    }

    void init()
    {
        QDir(logDir()).removeRecursively();
        g_seen = 0;
        m_original = qInstallMessageHandler(countingHandler);
    }

    void cleanup()
    {
        Kinefin::removeProfileLog();
        qInstallMessageHandler(m_original);
    }

    void parseLogLevelAcceptsTheSixNames()
    {
        QVERIFY(Kinefin::parseLogLevel(u"debug") == LogLevel::Debug);
        QVERIFY(Kinefin::parseLogLevel(u"info") == LogLevel::Info);
        QVERIFY(Kinefin::parseLogLevel(u"warn") == LogLevel::Warn);
        QVERIFY(Kinefin::parseLogLevel(u"error") == LogLevel::Error);
        QVERIFY(Kinefin::parseLogLevel(u"fatal") == LogLevel::Fatal);
        QVERIFY(Kinefin::parseLogLevel(u"disable") == LogLevel::Disable);
    }

    void parseLogLevelRejectsEverythingElse()
    {
        QVERIFY(!Kinefin::parseLogLevel(u"Debug").has_value());
        QVERIFY(!Kinefin::parseLogLevel(u"").has_value());
        QVERIFY(!Kinefin::parseLogLevel(u"trace").has_value());
    }

    void redactsTheTokenOfTheAuthorizationHeader()
    {
        QCOMPARE(Kinefin::redactSecrets(QStringLiteral(
                     R"(Authorization: MediaBrowser Client="Kinefin", Token="abc123")")),
                 QStringLiteral(R"(Authorization: MediaBrowser Client="Kinefin", Token="***")"));
    }

    void redactsApiKeyInAQuery()
    {
        QCOMPARE(Kinefin::redactSecrets(QStringLiteral("GET /Items?api_key=SECRET&x=1")),
                 QStringLiteral("GET /Items?api_key=***&x=1"));
    }

    void redactsApiKeyAtTheEndOfTheLine()
    {
        QCOMPARE(Kinefin::redactSecrets(QStringLiteral("ApiKey=SECRET")),
                 QStringLiteral("ApiKey=***"));
    }

    void redactsTheOtherShapesOfATokenAndSeveralPerLine()
    {
        const auto redacted = [](const QString &line) { return Kinefin::redactSecrets(line); };
        QCOMPARE(redacted(QStringLiteral("x Token='abc' y")), QStringLiteral("x Token='***' y"));
        QCOMPARE(redacted(QStringLiteral("x token=abc, y")), QStringLiteral("x token=***, y"));
        QCOMPARE(redacted(QStringLiteral("x Token=\\\"abc\\\" y")),
                 QStringLiteral("x Token=\\\"***\\\" y"));
        QCOMPARE(redacted(QStringLiteral("x Token=\"abc")), QStringLiteral("x Token=\"***"));
        QCOMPARE(redacted(QStringLiteral("X-Emby-Token: abc")),
                 QStringLiteral("X-Emby-Token: ***"));
        QCOMPARE(redacted(QStringLiteral("{\"AccessToken\":\"abc\",\"x\":1}")),
                 QStringLiteral("{\"AccessToken\":\"***\",\"x\":1}"));
        const QString several =
            redacted(QStringLiteral("Token=\"a1\" GET /x?api_key=b2&ApiKey=c3"));
        QVERIFY(!several.contains(QStringLiteral("a1")));
        QVERIFY(!several.contains(QStringLiteral("b2")));
        QVERIFY(!several.contains(QStringLiteral("c3")));
        QCOMPARE(redacted(QStringLiteral("TOKEN=\"abc\"")), QStringLiteral("TOKEN=\"***\""));
    }

    void leavesALineWithoutSecretsAlone()
    {
        const QString line = QStringLiteral("GET /Items?limit=10 status=200 \"quoted\"");
        QCOMPARE(Kinefin::redactSecrets(line), line);
    }

    void rotationKeepsTenFiles()
    {
        for (int run = 0; run < 12; ++run) {
            QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Debug));
            qInfo("marker-%d", run);
            Kinefin::removeProfileLog();
        }
        QVERIFY(readText(logFile()).contains(QStringLiteral("marker-11")));
        for (int index = 1; index <= 9; ++index) {
            QVERIFY2(QFile::exists(logFile(QStringLiteral(".%1").arg(index))),
                     qPrintable(QString::number(index)));
            QVERIFY(readText(logFile(QStringLiteral(".%1").arg(index)))
                        .contains(QStringLiteral("marker-%1").arg(11 - index)));
        }
        QVERIFY(!QFile::exists(logFile(QStringLiteral(".10"))));
    }

    void lineFormat()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Debug));
        qCWarning(testLog) << "message";
        Kinefin::removeProfileLog();

        const QStringList lines = readText(logFile()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 1);
        const QRegularExpression format(QStringLiteral(
            R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} \| warn \| kinefin.test \| message$)"));
        QVERIFY2(format.match(lines.first()).hasMatch(), qPrintable(lines.first()));
    }

    void everyLevelIsNamedInLowercase()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Debug));
        qDebug("d");
        qInfo("i");
        qWarning("w");
        qCritical("e");
        Kinefin::removeProfileLog();

        const QString text = readText(logFile());
        QVERIFY(text.contains(QStringLiteral(" | debug | default | d\n")));
        QVERIFY(text.contains(QStringLiteral(" | info | default | i\n")));
        QVERIFY(text.contains(QStringLiteral(" | warn | default | w\n")));
        QVERIFY(text.contains(QStringLiteral(" | error | default | e\n")));
    }

    void fileLevelFiltersTheFileOnly()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Error));
        qWarning("only-warning");
        qCritical("a-critical");
        Kinefin::removeProfileLog();

        const QString text = readText(logFile());
        QVERIFY(!text.contains(QStringLiteral("only-warning")));
        QVERIFY(text.contains(QStringLiteral("a-critical")));
    }

    void disableWritesNothing()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Disable));
        qInfo("info");
        qCritical("critical");
        Kinefin::removeProfileLog();

        QVERIFY(QFile::exists(logFile()));
        QVERIFY(QFileInfo(logFile()).size() == 0);
    }

    void tokenIsRedactedInTheFile()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Debug));
        qInfo("Authorization: MediaBrowser Token=\"abc123\"");
        Kinefin::removeProfileLog();

        const QString text = readText(logFile());
        QVERIFY(text.contains(QStringLiteral("Token=\"***\"")));
        QVERIFY(!text.contains(QStringLiteral("abc123")));
    }

    void logFileIsPrivate()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Debug));
        const QFileDevice::Permissions permissions = QFile::permissions(logFile());
        QVERIFY(permissions.testFlags(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QVERIFY(!(permissions
                  & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther
                     | QFileDevice::WriteOther)));
    }

    void previousHandlerSeesEveryMessage()
    {
        QVERIFY(Kinefin::installProfileLog(logDir(), LogLevel::Error));
        qDebug("d");
        qInfo("i");
        qWarning("w");
        qCritical("e");
        QCOMPARE(g_seen, 4);

        Kinefin::removeProfileLog();
        qInfo("after");
        QCOMPARE(g_seen, 5);
        QVERIFY(!readText(logFile()).contains(QStringLiteral("after")));
    }

    void installingTwiceDoesNotStack()
    {
        QVERIFY(Kinefin::installProfileLog(logDir() + QStringLiteral("/one"), LogLevel::Debug));
        QVERIFY(Kinefin::installProfileLog(logDir() + QStringLiteral("/two"), LogLevel::Debug));
        qInfo("once");
        QCOMPARE(g_seen, 1);
        Kinefin::removeProfileLog();

        QVERIFY(!readText(logDir() + QStringLiteral("/one/kinefin.log"))
                     .contains(QStringLiteral("once")));
        QVERIFY(readText(logDir() + QStringLiteral("/two/kinefin.log"))
                    .contains(QStringLiteral("once")));
        QVERIFY(qInstallMessageHandler(countingHandler) == countingHandler);
    }

    void unwritableLocationFailsAndInstallsNothing()
    {
        const QString blocker = m_dir.filePath(QStringLiteral("file.txt"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        QString error;
        QVERIFY(!Kinefin::installProfileLog(blocker + QStringLiteral("/logs"), LogLevel::Debug,
                                            &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(qInstallMessageHandler(countingHandler) == countingHandler);
        qInfo("still stderr-only");
        QCOMPARE(g_seen, 1);
    }
};

QTEST_GUILESS_MAIN(TestLogging)
#include "test_logging.moc"
