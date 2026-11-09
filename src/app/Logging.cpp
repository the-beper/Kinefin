// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLatin1String>
#include <QMessageLogContext>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <array>
#include <cstdio>
#include <utility>

namespace Kinefin {
namespace {
constexpr int kKeptFiles = 10;

constexpr std::array<std::pair<QLatin1String, LogLevel>, 6> kLevelNames{{
    {QLatin1String("debug"), LogLevel::Debug},
    {QLatin1String("info"), LogLevel::Info},
    {QLatin1String("warn"), LogLevel::Warn},
    {QLatin1String("error"), LogLevel::Error},
    {QLatin1String("fatal"), LogLevel::Fatal},
    {QLatin1String("disable"), LogLevel::Disable},
}};

struct State {
    QMutex mutex;
    QFile file;
    LogLevel fileLevel = LogLevel::Disable;
    QtMessageHandler previous = nullptr;
    bool installed = false;
};

State &state()
{
    // Never destroyed: a message logged during static destruction must still find a live mutex and file.
    static State *instance = new State;
    return *instance;
}

LogLevel levelOf(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return LogLevel::Debug;
    case QtInfoMsg:
        return LogLevel::Info;
    case QtWarningMsg:
        return LogLevel::Warn;
    case QtCriticalMsg:
        return LogLevel::Error;
    case QtFatalMsg:
        return LogLevel::Fatal;
    }
    return LogLevel::Error;
}

QString nameOf(LogLevel level)
{
    for (const auto &[name, value] : kLevelNames) {
        if (value == level)
            return QString(name);
    }
    return {};
}

void rotate(const QDir &directory)
{
    const QString base = directory.filePath(QStringLiteral("kinefin.log"));
    QFile::remove(base + QLatin1Char('.') + QString::number(kKeptFiles - 1));
    for (int index = kKeptFiles - 2; index >= 1; --index)
        QFile::rename(base + QLatin1Char('.') + QString::number(index),
                      base + QLatin1Char('.') + QString::number(index + 1));
    QFile::rename(base, base + QStringLiteral(".1"));
}

void handleMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    State &s = state();
    QtMessageHandler previous = nullptr;
    // The access token must not reach any log, the console and the journal included.
    const QString safe = redactSecrets(message);
    {
        QMutexLocker locker(&s.mutex);
        previous = s.previous;
        const LogLevel level = levelOf(type);
        if (s.file.isOpen() && level >= s.fileLevel) {
            // A message with a newline would otherwise forge extra log lines.
            QString text = safe;
            text.replace(QLatin1Char('\r'), QStringLiteral("\\r"))
                .replace(QLatin1Char('\n'), QStringLiteral("\\n"));
            const QString category = context.category ? QString::fromLatin1(context.category)
                                                      : QStringLiteral("default");
            const QString line =
                QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                + QStringLiteral(" | ") + nameOf(level) + QStringLiteral(" | ") + category
                + QStringLiteral(" | ") + text + QLatin1Char('\n');
            s.file.write(line.toUtf8());
            s.file.flush();
        }
    }
    if (previous) {
        previous(type, context, safe);
    } else {
        const QByteArray formatted = qFormatLogMessage(type, context, safe).toLocal8Bit();
        std::fprintf(stderr, "%s\n", formatted.constData());
        std::fflush(stderr);
    }
}
} // namespace

std::optional<LogLevel> parseLogLevel(QStringView name)
{
    for (const auto &[text, level] : kLevelNames) {
        if (name == text)
            return level;
    }
    return std::nullopt;
}

QString redactSecrets(const QString &line)
{
    constexpr auto flags = QRegularExpression::CaseInsensitiveOption;
    // `Token="…"` of the MediaBrowser header (also with escaped quotes, unterminated, single-quoted or bare), the Emby token headers, a JSON token field and the api_key query parameter.
    static const QRegularExpression doubleQuoted(QStringLiteral(R"re((Token=\\?")[^"\\]*)re"),
                                                 flags);
    static const QRegularExpression singleQuoted(QStringLiteral(R"re((Token=')[^']*)re"), flags);
    static const QRegularExpression bare(QStringLiteral(R"re((Token=)(?!["'\\*])[^\s,;&"']+)re"),
                                         flags);
    static const QRegularExpression header(
        QStringLiteral(R"re((X-(?:Emby|MediaBrowser)-Token\s*[:=]\s*)[^\r\n]+)re"), flags);
    static const QRegularExpression json(QStringLiteral(R"re(("(?:Access)?Token"\s*:\s*")[^"]*)re"),
                                         flags);
    static const QRegularExpression apiKey(QStringLiteral(R"re((api_?key=)[^&\s"']*)re"), flags);
    QString redacted = line;
    for (const QRegularExpression *pattern :
         {&doubleQuoted, &singleQuoted, &bare, &header, &json, &apiKey})
        redacted.replace(*pattern, QStringLiteral(R"(\1***)"));
    return redacted;
}

bool installProfileLog(const QString &directory, LogLevel fileLevel, QString *error)
{
    removeProfileLog();

    QDir dir(directory);
    if (!dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("cannot create directory %1").arg(directory);
        return false;
    }
    rotate(dir);

    State &s = state();
    QMutexLocker locker(&s.mutex);
    s.file.setFileName(dir.filePath(QStringLiteral("kinefin.log")));
    if (!s.file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = s.file.errorString();
        return false;
    }
    if (!s.file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error)
            *error = s.file.errorString();
        s.file.close();
        return false;
    }
    s.fileLevel = fileLevel;
    s.previous = qInstallMessageHandler(handleMessage);
    s.installed = true;
    return true;
}

void removeProfileLog()
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    if (!s.installed)
        return;
    qInstallMessageHandler(s.previous);
    s.previous = nullptr;
    s.installed = false;
    s.file.close();
}
} // namespace Kinefin
