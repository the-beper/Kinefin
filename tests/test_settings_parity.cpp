// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings/SettingDefinitions.h"

#include <QFile>
#include <QRegularExpression>
#include <QtTest>

using Kinefin::SettingDefinition;

namespace {
struct ParityRow {
    QString section;
    QString key;
    QString defaultText;
    bool defined = false; // ✅ or ◇ in the Kinefin column
    bool hidden = false;  // ◇
};

QStringList backticked(const QString &cell)
{
    QStringList tokens;
    static const QRegularExpression pattern(QStringLiteral("`([^`]+)`"));
    for (const auto &match : pattern.globalMatch(cell))
        tokens << match.captured(1);
    return tokens;
}

// The §2.1-§2.5 tables of the parity document. §2.4 and §2.5 have no "Upstream UI" column and every row is implemented
QList<ParityRow> parityRows(const QString &text)
{
    QList<ParityRow> rows;
    QString section;
    bool inSettings = false;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        static const QRegularExpression heading(QStringLiteral("^### 2\\.[1-5] `(\\w+)`"));
        if (line.startsWith(QLatin1String("## ")) || line.startsWith(QLatin1String("### 2.6")))
            inSettings = false;
        if (const auto match = heading.match(line); match.hasMatch()) {
            section = match.captured(1);
            inSettings = true;
            continue;
        }
        if (!inSettings || !line.startsWith(QLatin1String("| `")))
            continue;
        const QStringList cells = line.split(QLatin1Char('|'), Qt::SkipEmptyParts);
        const bool fullTable =
            section != QLatin1String("subtitles") && section != QLatin1String("other");
        const QString status = fullTable ? cells.value(3) : QStringLiteral("✅");
        const QStringList ids = backticked(cells.value(0));
        if (ids.isEmpty() || ids.first().contains(QLatin1Char('{')) || ids.size() > 1)
            continue; // a multi-id row (`webMode` / `layout`) is never a defined setting
        const QString defaultCell = cells.value(1);
        QString defaultText = backticked(defaultCell).value(0);
        if (defaultText.startsWith(QLatin1Char('"')))
            defaultText = defaultText.mid(1, defaultText.size() - 2);
        rows.append({section, ids.first(), defaultText,
                     status.contains(QStringLiteral("✅")) || status.contains(QStringLiteral("◇")),
                     status.contains(QStringLiteral("◇"))});
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
    void tableMatchesTheParityDocument()
    {
        QFile file(QStringLiteral(KINEFIN_SOURCE_DIR "/docs/feature-parity.md"));
        if (!file.open(QIODevice::ReadOnly))
            QSKIP("docs/feature-parity.md is not part of this checkout");
        const QList<ParityRow> rows = parityRows(QString::fromUtf8(file.readAll()));
        QVERIFY(rows.size() > 50);

        int defined = 0;
        for (const ParityRow &row : rows) {
            const auto *definition = Kinefin::findSetting(row.section, row.key);
            if (!row.defined) {
                QVERIFY2(!definition,
                         qPrintable(row.section + QLatin1Char('.') + row.key
                                    + QStringLiteral(" is dropped in the parity doc but defined")));
                continue;
            }
            ++defined;
            const QString name = row.section + QLatin1Char('.') + row.key;
            QVERIFY2(definition,
                     qPrintable(name + QStringLiteral(" is in the parity doc but not defined")));
            QCOMPARE(definition->hidden, row.hidden);
            QCOMPARE(defaultText(*definition), row.defaultText);
            QVERIFY2(definition->hidden || !definition->label.isEmpty(),
                     qPrintable(name + QStringLiteral(" is visible but has no label")));
        }
        // The tables of §2.1-§2.5 plus the one §2.6 key the program uses, `state.geometry`
        const auto *geometry =
            Kinefin::findSetting(QStringLiteral("state"), QStringLiteral("geometry"));
        QVERIFY(geometry && geometry->hidden && geometry->defaultValue == QStringLiteral(""));
        // §2.7 is not parsed: the Kinefin additions are listed here
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
        // The downloads of parity §10: parallel transfers and the unmetered-only rule, both visible
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
        QCOMPARE(Kinefin::settingDefinitions().size(), defined + 8 + subTabs.size());
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
