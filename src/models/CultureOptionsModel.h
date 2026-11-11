// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QVariantList>

#include <vector>

namespace Kinefin {
// The language catalog of the server (`GET /Localization/Cultures`) as the options of an audio or subtitle language picker. A row is the code that goes in the user's configuration (the entry's `ThreeLetterISOLanguageName`, the value jellyfin-web stores) and a display name (server text: render it as plain text). Entries without a usable code, repeated codes and anything past `MaxOptions` are dropped, names are capped. The other three-letter spellings an entry lists (`ThreeLetterISOLanguageNames`) resolve to the same row, so a stored value with a variant spelling still shows its language
class CultureOptionsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)

  public:
    enum Role { CodeRole = Qt::UserRole + 1, NameRole };

    static constexpr int MaxOptions = 1000;
    static constexpr int MaxCodeChars = 16;

    explicit CultureOptionsModel(QObject *parent = nullptr);

    // Replaces the catalog; stays unavailable when the answer holds no usable entry
    void setCultures(const QJsonArray &cultures);
    void clear();

    bool available() const { return !m_options.empty(); }
    // Whether `code` (any spelling, any case) is a language of the catalog
    Q_INVOKABLE bool contains(const QString &code) const { return indexOfCode(code) >= 0; }
    // The row of `code`, or -1 when the catalog does not know it (a code the server stored that the catalog lacks)
    Q_INVOKABLE int indexOfCode(const QString &code) const;
    Q_INVOKABLE QString nameOf(const QString &code) const;
    // `{code, name}` of every option, in catalog order: what a picker that adds entries of its own (Automatic) lists
    Q_INVOKABLE QVariantList entries() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

  Q_SIGNALS:
    void availableChanged();

  private:
    struct Option {
        QString code;
        QString name;
    };

    std::vector<Option> m_options;
    QHash<QString, int> m_rowByCode; // lower-case spelling -> row
};
} // namespace Kinefin
