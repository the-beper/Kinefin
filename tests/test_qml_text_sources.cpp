// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// A source scan of the QML files for the two places where Qt reads a text as rich text whatever its source: the attached tooltip of a control and the texts of a placeholder message. Server text must not reach either (see MessageBox.qml and PlainToolTip.qml for the plain-text ways), so each text there is a translated string or one of the few bindings below that were read and found to be the application's own
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QtTest>

namespace {
// A translation of a literal and nothing else: `i18n("text")`, `i18nc("context", "text")` and their plural forms with a count, or a choice between such calls under a condition that is a property or a comparison. A call with an argument that is not a literal (a name, a title) is not one: plain i18n does not escape what it is given, and the result would be read as rich text
bool isTranslation(const QString &value)
{
    // A double quoted string literal
    static const QString literal = QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"");
    static const QString call =
        QStringLiteral("(?:i18nc?\\(\\s*%1(?:\\s*,\\s*%1)*\\s*\\)"
                       "|i18nc?p\\(\\s*%1(?:\\s*,\\s*%1)*\\s*,\\s*[A-Za-z0-9_.?]+\\s*\\))")
            .arg(literal);
    static const QString condition =
        QStringLiteral("(?:[A-Za-z0-9_.!&|=<>\\s]|\\?\\.|\"[A-Za-z0-9_]*\")+");
    // A branch is a call or the empty text; a chain of choices ends in a branch
    static const QString branch = QStringLiteral("(?:%1|\"\")").arg(call);
    static const QRegularExpression pure(
        QStringLiteral("\\A(?:%1|(?:%2\\?\\s*%3\\s*:\\s*)+%3)\\z").arg(call, condition, branch));
    return pure.match(value.trimmed()).hasMatch();
}

// Tooltip bindings that are not a translation call or the control's own translated `text`, with what they bind: the application's own strings, checked by hand, each matched as a whole
const QList<std::pair<QString, QString>> &knownTooltipBindings()
{
    static const QList<std::pair<QString, QString>> known{
        // A tooltip made of translated strings and the control's own text
        {QStringLiteral("PlayerPage.qml"),
         QStringLiteral(
             "blockedByGroup ? i18nc(\"@info:tooltip\", \"Not available while following a "
             "SyncPlay group\") : text")},
        {QStringLiteral("PlayerPage.qml"),
         QStringLiteral("page.player.casting ? i18nc(\"@info:tooltip\", \"Not available while "
                        "casting\") : text")},
        {QStringLiteral("ShellHeader.qml"),
         QStringLiteral("queued > 0 ? i18ncp(\"@info:tooltip the queue button and how many items "
                        "the queue holds\", \"Queue, %1 item\", \"Queue, %1 items\", queued) : "
                        "text")},
        // The text of an action of the application, or a button whose text is set by the page that uses it
        {QStringLiteral("LibraryPage.qml"), QStringLiteral("text")},
        {QStringLiteral("OsdButton.qml"), QStringLiteral("text")},
        // The icon-only buttons of a download row: their texts are the application's own strings (Play, Resume, Retry, Pause, Export, Cancel, Delete)
        {QStringLiteral("DownloadsPage.qml"), QStringLiteral("text")},
        {QStringLiteral("ShellHeader.qml"),
         QStringLiteral("display === QQC2.AbstractButton.IconOnly ? text : \"\"")},
        // Properties that hold a translated string
        {QStringLiteral("ShellHeader.qml"), QStringLiteral("label")},
        {QStringLiteral("UserPickerPage.qml"), QStringLiteral("lockedText")},
    };
    return known;
}

// Placeholder texts that are properties of the page: each is a translated string chosen by the page (the filter note, the empty text of a list)
const QStringList &knownPlaceholderBindings()
{
    static const QStringList known{QStringLiteral("dialog.note"),
                                   QStringLiteral("content.emptyText"),
                                   QStringLiteral("page.flatEmptyText")};
    return known;
}

struct Frame {
    QString type;
    // The value of `text:` and of `explanation:` set in this object, and where
    QString text;
    QString explanation;
    int textLine = 0;
    int explanationLine = 0;
    QList<std::pair<int, QString>> tooltips;
    // A property whose value is a block ("text: {"): the lines of `return` it holds
    QString blockProperty;
    QStringList blockReturns;
    int blockLine = 0;
};

// The code of a line without its strings and its comment, for counting braces
QString codeOf(QString line)
{
    static const QRegularExpression doubleQuoted(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\""));
    static const QRegularExpression singleQuoted(QStringLiteral("'(?:\\\\.|[^'\\\\])*'"));
    static const QRegularExpression comment(QStringLiteral("//.*"));
    line.replace(doubleQuoted, QStringLiteral("\"\""));
    line.replace(singleQuoted, QStringLiteral("''"));
    line.remove(comment);
    return line;
}

QString shortValue(const QString &value)
{
    return value.left(60);
}

struct Findings {
    QStringList problems;
};

void scanFile(const QString &name, const QStringList &lines, Findings &findings)
{
    static const QRegularExpression typeBeforeBrace(
        QStringLiteral("([A-Za-z_][A-Za-z0-9_.]*)\\s*\\{\\s*$"));
    static const QRegularExpression textProperty(QStringLiteral("^\\s*text\\s*:\\s*(.*)$"));
    static const QRegularExpression explanationProperty(
        QStringLiteral("^\\s*explanation\\s*:\\s*(.*)$"));
    // The tooltip of a control or of an action, as an assignment at the start of a line
    static const QRegularExpression tooltipProperty(
        QStringLiteral("^\\s*(?:(?:[A-Za-z0-9]+\\.)*ToolTip\\.text|tooltip)\\s*:\\s*(.*)$"));
    static const QRegularExpression returnStatement(QStringLiteral("^\\s*return\\s+(.*)$"));
    QList<Frame> stack;
    for (int index = 0; index < lines.size(); ++index) {
        const QString &raw = lines.at(index);
        const QString code = codeOf(raw);
        const int number = index + 1;
        if (!stack.isEmpty()) {
            Frame &top = stack.last();
            if (const auto match = textProperty.match(raw); match.hasMatch()) {
                top.text = match.captured(1).trimmed();
                // A choice that goes on at the start of the next lines (`: other` or `? other`)
                for (int next = index + 1; next < lines.size(); ++next) {
                    const QString more = lines.at(next).trimmed();
                    if (!more.startsWith(QLatin1Char(':')) && !more.startsWith(QLatin1Char('?')))
                        break;
                    top.text += QLatin1Char(' ') + more;
                }
                top.textLine = number;
                if (top.text == QLatin1String("{")) {
                    top.blockProperty = QStringLiteral("text");
                    top.blockLine = number;
                }
            } else if (const auto match = explanationProperty.match(raw); match.hasMatch()) {
                top.explanation = match.captured(1).trimmed();
                top.explanationLine = number;
                if (top.explanation == QLatin1String("{")) {
                    top.blockProperty = QStringLiteral("explanation");
                    top.blockLine = number;
                }
            } else if (const auto match = tooltipProperty.match(raw); match.hasMatch()) {
                top.tooltips.append({number, match.captured(1).trimmed()});
            }
            // A `return` inside the block of a property belongs to that property, however deep in the block it is
            if (const auto match = returnStatement.match(raw); match.hasMatch()) {
                for (qsizetype depth = stack.size() - 1; depth >= 0; --depth) {
                    if (!stack.at(depth).blockProperty.isEmpty()) {
                        stack[depth].blockReturns.append(match.captured(1).trimmed().chopped(
                            match.captured(1).trimmed().endsWith(QLatin1Char(';')) ? 1 : 0));
                        break;
                    }
                }
            }
        }
        for (qsizetype i = 0; i < code.size(); ++i) {
            if (code.at(i) == QLatin1Char('{')) {
                Frame frame;
                if (const auto match = typeBeforeBrace.match(code.left(i + 1)); match.hasMatch())
                    frame.type = match.captured(1);
                stack.append(frame);
            } else if (code.at(i) == QLatin1Char('}') && !stack.isEmpty()) {
                Frame done = stack.takeLast();
                // A ToolTip object of its own (PlainToolTip is plain text by its make)
                if (done.type.endsWith(QLatin1String("ToolTip"))
                    && !done.type.endsWith(QLatin1String("PlainToolTip")) && !done.text.isEmpty())
                    done.tooltips.append({done.textLine, done.text});
                const auto where = [&](int line) {
                    return QStringLiteral("%1:%2").arg(name).arg(line);
                };
                for (const auto &[line, value] : done.tooltips) {
                    if (isTranslation(value))
                        continue;
                    bool known = false;
                    for (const auto &[file, binding] : knownTooltipBindings())
                        known = known || (name == file && value == binding);
                    // The control's own text, which is a translation call
                    known = known || (value == QLatin1String("text") && isTranslation(done.text));
                    if (!known)
                        findings.problems << QStringLiteral("%1 binds a tooltip to `%2`")
                                                 .arg(where(line), shortValue(value));
                }
                if (done.type.endsWith(QLatin1String("PlaceholderMessage"))) {
                    const auto check = [&](const QString &property, const QString &value, int line,
                                           const QStringList &returns) {
                        if (value.isEmpty() || isTranslation(value))
                            return;
                        if (value == QLatin1String("{")) {
                            for (const QString &returned : returns) {
                                if (!isTranslation(returned))
                                    findings.problems
                                        << QStringLiteral(
                                               "%1 returns `%2` for the %3 of a placeholder")
                                               .arg(where(line), shortValue(returned), property);
                            }
                            return;
                        }
                        if (knownPlaceholderBindings().contains(value))
                            return;
                        findings.problems
                            << QStringLiteral("%1 binds the %2 of a placeholder to `%3`")
                                   .arg(where(line), property, shortValue(value));
                    };
                    check(QStringLiteral("text"), done.text, done.textLine,
                          done.blockProperty == QLatin1String("text") ? done.blockReturns
                                                                      : QStringList());
                    check(QStringLiteral("explanation"), done.explanation, done.explanationLine,
                          done.blockProperty == QLatin1String("explanation") ? done.blockReturns
                                                                             : QStringList());
                }
            }
        }
    }
}
} // namespace

class TestQmlTextSources : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void tooltipsAndPlaceholdersCarryOnlyTheApplicationsOwnText()
    {
        const QDir directory(QStringLiteral(KINEFIN_SOURCE_DIR "/src/qml"));
        const QStringList files = directory.entryList({QStringLiteral("*.qml")}, QDir::Files);
        QVERIFY2(files.size() > 50, "the QML sources were not found");
        Findings findings;
        for (const QString &name : files) {
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            scanFile(name, QString::fromUtf8(file.readAll()).split(QLatin1Char('\n')), findings);
        }
        QVERIFY2(findings.problems.isEmpty(),
                 qPrintable(findings.problems.join(QLatin1Char('\n'))));
    }
};

QTEST_APPLESS_MAIN(TestQmlTextSources)
#include "test_qml_text_sources.moc"
