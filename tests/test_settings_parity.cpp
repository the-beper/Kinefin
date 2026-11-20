// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/SettingDefinitions.h"

#include <QFile>
#include <QRegularExpression>
#include <QtTest>

using Kinefin::SettingDefinition;

namespace {
struct ReferenceRow {
    QString line;
    QString section;
    QString key;
    QString defaultText;
    bool hidden = false;   // in the table of the settings without a control
    bool complete = false; // the key and the default are both in code font
};

QStringList backticked(const QString &cell)
{
    QStringList tokens;
    static const QRegularExpression pattern(QStringLiteral("`([^`]+)`"));
    for (const auto &match : pattern.globalMatch(cell))
        tokens << match.captured(1);
    return tokens;
}

QString unquoted(QString text)
{
    if (text.startsWith(QLatin1Char('"')))
        text = text.mid(1, text.size() - 2);
    return text;
}

// The tables of docs/settings.md: one per Settings category, headed "## Title (`section`)" with the key and the default in code font, then the settings without a control as `section.key`
QList<ReferenceRow> referenceRows(const QString &text)
{
    QList<ReferenceRow> rows;
    QString section;
    bool withoutControl = false;
    static const QRegularExpression heading(QStringLiteral("^## .*\\(`(\\w+)`\\)$"));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("## "))) {
            const auto match = heading.match(line);
            section = match.hasMatch() ? match.captured(1) : QString();
            withoutControl = line == QLatin1String("## Settings without a control");
            continue;
        }
        if (section.isEmpty() && !withoutControl)
            continue;
        if (!line.startsWith(QLatin1String("| ")) || line.startsWith(QLatin1String("| ---"))
            || line.startsWith(QLatin1String("| Setting |"))
            || line.startsWith(QLatin1String("| Key |")))
            continue;
        const QStringList cells = line.split(QLatin1Char('|'), Qt::SkipEmptyParts);
        const qsizetype keyCell = withoutControl ? 0 : 1;
        const QStringList key = backticked(cells.value(keyCell));
        const QStringList defaultValue = backticked(cells.value(keyCell + 1));
        ReferenceRow row{.line = line,
                         .section = section,
                         .key = key.value(0),
                         .defaultText = unquoted(defaultValue.value(0)),
                         .hidden = withoutControl,
                         .complete = !key.isEmpty() && !defaultValue.isEmpty()};
        if (withoutControl) {
            const qsizetype dot = row.key.indexOf(QLatin1Char('.'));
            row.complete = row.complete && dot > 0;
            row.section = row.key.left(dot);
            row.key = row.key.mid(dot + 1);
        }
        rows.append(row);
    }
    return rows;
}

QString defaultText(const SettingDefinition &definition)
{
    if (definition.type == SettingDefinition::Type::String)
        return definition.defaultValue.toString();
    if (definition.type == SettingDefinition::Type::Double)
        return QString::number(definition.defaultValue.toDouble(), 'f', 1);
    return definition.defaultValue.toString();
}
} // namespace

class TestSettingsParity : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    // Every setting the program defines is in the settings reference, with its default, and nothing else is
    void theSettingsReferenceMatchesTheCode()
    {
        QFile file(QStringLiteral(KINEFIN_SOURCE_DIR "/docs/settings.md"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QList<ReferenceRow> rows = referenceRows(QString::fromUtf8(file.readAll()));
        QSet<QString> listed;
        for (const ReferenceRow &row : rows) {
            QVERIFY2(row.complete,
                     qPrintable(row.line + QStringLiteral(" has no key or default in code font")));
            const QString name = row.section + QLatin1Char('.') + row.key;
            const auto *definition = Kinefin::findSetting(row.section, row.key);
            QVERIFY2(
                definition,
                qPrintable(name + QStringLiteral(" is in the settings reference but not defined")));
            QVERIFY2(
                definition->hidden == row.hidden,
                qPrintable(name + QStringLiteral(" has a control in one and none in the other")));
            QVERIFY2(defaultText(*definition) == row.defaultText,
                     qPrintable(name + QStringLiteral(" defaults to ") + defaultText(*definition)
                                + QStringLiteral(", the reference says ") + row.defaultText));
            QVERIFY2(!listed.contains(name), qPrintable(name + QStringLiteral(" is listed twice")));
            listed.insert(name);
        }
        for (const SettingDefinition &definition : Kinefin::settingDefinitions()) {
            const QString name = definition.section + QLatin1Char('.') + definition.key;
            QVERIFY2(
                listed.contains(name),
                qPrintable(
                    name + QStringLiteral(" is defined but missing from the settings reference")));
            QVERIFY2(definition.hidden || !definition.label.isEmpty(),
                     qPrintable(name + QStringLiteral(" is visible but has no label")));
        }
    }

    // The settings that are Kinefin's own keep their keys, types and defaults
    void theAdditionsKeepTheirDefaults()
    {
        // The window geometry the program keeps, never shown
        const auto *geometry =
            Kinefin::findSetting(QStringLiteral("state"), QStringLiteral("geometry"));
        QVERIFY(geometry && geometry->hidden && geometry->defaultValue == QStringLiteral(""));
        const auto *newBadge =
            Kinefin::findSetting(QStringLiteral("library"), QStringLiteral("newBadge"));
        QVERIFY(newBadge && !newBadge->hidden && newBadge->defaultValue == true);
        const auto *homeLibraries =
            Kinefin::findSetting(QStringLiteral("library"), QStringLiteral("home.libraries"));
        QVERIFY(homeLibraries && !homeLibraries->hidden && homeLibraries->defaultValue == true);
        const auto *homeFavorites =
            Kinefin::findSetting(QStringLiteral("library"), QStringLiteral("home.favorites"));
        QVERIFY(homeFavorites && !homeFavorites->hidden && homeFavorites->defaultValue == true);
        QCOMPARE(homeFavorites->type, SettingDefinition::Type::Bool);
        QCOMPARE(QString::fromUtf8(homeFavorites->group.untranslatedText()),
                 QString::fromUtf8(homeLibraries->group.untranslatedText()));
        // The downloads: parallel transfers and the unmetered-only rule, both visible
        const auto *parallelDownloads =
            Kinefin::findSetting(QStringLiteral("downloads"), QStringLiteral("parallel"));
        QVERIFY(parallelDownloads && !parallelDownloads->hidden
                && !parallelDownloads->label.isEmpty());
        QCOMPARE(parallelDownloads->type, SettingDefinition::Type::Int);
        QCOMPARE(parallelDownloads->defaultValue, QVariant(2));
        const auto *unmeteredOnly =
            Kinefin::findSetting(QStringLiteral("downloads"), QStringLiteral("unmeteredOnly"));
        QVERIFY(unmeteredOnly && !unmeteredOnly->hidden && !unmeteredOnly->label.isEmpty());
        QCOMPARE(unmeteredOnly->type, SettingDefinition::Type::Bool);
        QCOMPARE(unmeteredOnly->defaultValue, QVariant(true));
        const auto *sidecars =
            Kinefin::findSetting(QStringLiteral("downloads"), QStringLiteral("kodiSidecars"));
        QVERIFY(sidecars && !sidecars->hidden && !sidecars->disabled);
        QCOMPARE(sidecars->defaultValue, QVariant(false));
        const auto *autoExport =
            Kinefin::findSetting(QStringLiteral("downloads"), QStringLiteral("exportOnCompletion"));
        QVERIFY(autoExport && !autoExport->hidden && autoExport->disabled);
        QCOMPARE(autoExport->defaultValue, QVariant(false));
        // The library sub-tab switches: only the Favorites and Collections of Movies and TV Shows are on by default
        const QStringList subTabs{
            QStringLiteral("tabs.movies.suggestions"), QStringLiteral("tabs.movies.favorites"),
            QStringLiteral("tabs.movies.collections"), QStringLiteral("tabs.movies.genres"),
            QStringLiteral("tabs.tv.suggestions"),     QStringLiteral("tabs.tv.favorites"),
            QStringLiteral("tabs.tv.collections"),     QStringLiteral("tabs.tv.upcoming"),
            QStringLiteral("tabs.tv.genres"),          QStringLiteral("tabs.tv.networks"),
            QStringLiteral("tabs.tv.episodes")};
        const QStringList enabledByDefault{
            QStringLiteral("tabs.movies.favorites"), QStringLiteral("tabs.movies.collections"),
            QStringLiteral("tabs.tv.favorites"), QStringLiteral("tabs.tv.collections")};
        for (const QString &key : subTabs) {
            const auto *subTab = Kinefin::findSetting(QStringLiteral("library"), key);
            QVERIFY2(subTab && !subTab->hidden && !subTab->label.isEmpty()
                         && subTab->type == SettingDefinition::Type::Bool,
                     qPrintable(key));
            QCOMPARE(subTab->defaultValue, QVariant(enabledByDefault.contains(key)));
        }
    }

    void everyDefaultIsValid()
    {
        for (const SettingDefinition &definition : Kinefin::settingDefinitions()) {
            QVERIFY2(Kinefin::validatedSettingValue(definition, definition.defaultValue).isValid(),
                     qPrintable(definition.section + QLatin1Char('.') + definition.key));
            for (const auto &option : definition.options)
                QVERIFY(Kinefin::validatedSettingValue(definition, option.value).isValid());
        }
    }

    void subtitleValueListsMatchUpstream()
    {
        const auto optionsOf = [](const QString &key) {
            QStringList values;
            if (const auto *definition = Kinefin::findSetting(QStringLiteral("subtitles"), key)) {
                for (const auto &option : definition->options)
                    values << option.value.toString();
            }
            return values;
        };
        const QStringList colors{QStringLiteral(""),        QStringLiteral("#000000"),
                                 QStringLiteral("#FFFFFF"), QStringLiteral("#EEEEEE"),
                                 QStringLiteral("#CCCCCC"), QStringLiteral("#FBF93E"),
                                 QStringLiteral("#FFFFCC"), QStringLiteral("#EEEDB8")};
        QCOMPARE(optionsOf(QStringLiteral("color")), colors);
        QCOMPARE(optionsOf(QStringLiteral("border_color")), colors);
        QCOMPARE(optionsOf(QStringLiteral("background_color")), colors);
        const QStringList placements{QStringLiteral(""),
                                     QStringLiteral("left,bottom"),
                                     QStringLiteral("right,bottom"),
                                     QStringLiteral("center,bottom"),
                                     QStringLiteral("left,top"),
                                     QStringLiteral("right,top"),
                                     QStringLiteral("center,top")};
        QCOMPARE(optionsOf(QStringLiteral("placement")), placements);
        const QStringList transparencies{QStringLiteral(""),   QStringLiteral("FF"),
                                         QStringLiteral("C0"), QStringLiteral("80"),
                                         QStringLiteral("40"), QStringLiteral("00")};
        QCOMPARE(optionsOf(QStringLiteral("background_transparency")), transparencies);
        const QStringList styleOverrides{QStringLiteral(""),      QStringLiteral("yes"),
                                         QStringLiteral("no"),    QStringLiteral("force"),
                                         QStringLiteral("scale"), QStringLiteral("strip")};
        QCOMPARE(optionsOf(QStringLiteral("ass_style_override")), styleOverrides);
        const QStringList fonts{QStringLiteral(""),           QStringLiteral("serif"),
                                QStringLiteral("sans-serif"), QStringLiteral("script"),
                                QStringLiteral("monospace"),  QStringLiteral("display"),
                                QStringLiteral("cursive"),    QStringLiteral("fantasy")};
        QCOMPARE(optionsOf(QStringLiteral("font")), fonts);
    }

    void sectionsAndKeysAreUnique()
    {
        QSet<QString> seen;
        for (const SettingDefinition &definition : Kinefin::settingDefinitions()) {
            const QString id = definition.section + QLatin1Char('/') + definition.key;
            QVERIFY2(!seen.contains(id), qPrintable(id));
            seen.insert(id);
        }
    }
};
QTEST_APPLESS_MAIN(TestSettingsParity)
#include "test_settings_parity.moc"
