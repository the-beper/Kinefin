// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CultureOptionsModel.h"

#include "FilterOptionsModel.h"

#include <QJsonObject>

namespace Kinefin {
namespace {
// Letters, digits and '-' only: a code ends up in a configuration the server stores and in nothing else
bool isUsableCode(const QString &code)
{
    if (code.isEmpty() || code.size() > CultureOptionsModel::MaxCodeChars)
        return false;
    for (const QChar c : code) {
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('-'))
            return false;
    }
    return true;
}
} // namespace

CultureOptionsModel::CultureOptionsModel(QObject *parent) : QAbstractListModel(parent) {}

void CultureOptionsModel::setCultures(const QJsonArray &cultures)
{
    std::vector<Option> options;
    QHash<QString, int> rows;
    for (const QJsonValue &entry : cultures) {
        if (int(options.size()) == MaxOptions)
            break;
        const QJsonObject culture = entry.toObject();
        const QString code = culture.value(QStringLiteral("ThreeLetterISOLanguageName")).toString();
        if (!isUsableCode(code) || rows.contains(code.toLower()))
            continue;
        QString name =
            FilterOptionsModel::cappedName(culture.value(QStringLiteral("DisplayName")).toString());
        if (name.isEmpty())
            name = FilterOptionsModel::cappedName(culture.value(QStringLiteral("Name")).toString());
        if (name.isEmpty())
            name = code;
        const int row = int(options.size());
        options.push_back({code, name});
        rows.insert(code.toLower(), row);
        const QJsonValue variants = culture.value(QStringLiteral("ThreeLetterISOLanguageNames"));
        if (variants.isArray()) {
            for (const QJsonValue &variant : variants.toArray()) {
                const QString spelling = variant.toString().toLower();
                if (isUsableCode(spelling) && !rows.contains(spelling))
                    rows.insert(spelling, row);
            }
        }
    }
    const bool wasAvailable = available();
    beginResetModel();
    m_options = std::move(options);
    m_rowByCode = std::move(rows);
    endResetModel();
    if (wasAvailable != available())
        Q_EMIT availableChanged();
}

void CultureOptionsModel::clear()
{
    if (m_options.empty())
        return;
    beginResetModel();
    m_options.clear();
    m_rowByCode.clear();
    endResetModel();
    Q_EMIT availableChanged();
}

int CultureOptionsModel::indexOfCode(const QString &code) const
{
    return m_rowByCode.value(code.toLower(), -1);
}

QString CultureOptionsModel::nameOf(const QString &code) const
{
    const int row = indexOfCode(code);
    return row < 0 ? QString() : m_options[size_t(row)].name;
}

QVariantList CultureOptionsModel::entries() const
{
    QVariantList list;
    list.reserve(qsizetype(m_options.size()));
    for (const Option &option : m_options)
        list.append(QVariantMap{{QStringLiteral("code"), option.code},
                                {QStringLiteral("name"), option.name}});
    return list;
}

int CultureOptionsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_options.size());
}

QHash<int, QByteArray> CultureOptionsModel::roleNames() const
{
    return {{CodeRole, "code"}, {NameRole, "name"}};
}

QVariant CultureOptionsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.parent().isValid() || index.row() < 0
        || index.row() >= int(m_options.size()))
        return {};
    const Option &option = m_options[size_t(index.row())];
    switch (role) {
    case CodeRole:
        return option.code;
    case Qt::DisplayRole:
    case NameRole:
        return option.name;
    default:
        return {};
    }
}
} // namespace Kinefin
