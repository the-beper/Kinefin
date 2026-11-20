// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <optional>
#include <vector>

namespace Kinefin {

// Deepest element nesting accepted in a document a renderer sent (depth 32)
inline constexpr int kMaxXmlDepth = 32;
// Most elements in one document; the size limits already bound it, this stops a document made of empty elements from building a large tree
inline constexpr int kMaxXmlNodes = 4096;

// One element of a parsed document: local name, namespace, the character data directly inside it and the child elements. Attributes and comments are not kept
struct XmlNode {
    QString name;
    QString ns;
    QString text;
    std::vector<XmlNode> children;

    bool is(QStringView localName) const { return QStringView(name) == localName; }
    // First child element with this local name, or nullptr
    const XmlNode *child(QStringView childName) const;
    QString trimmedText() const { return text.trimmed(); }
};

// Parses an untrusted document into a tree. Refused, with a fixed message in `error`: more than maxBytes, any DTD (so no entity can be declared and none can expand or fetch anything), deeper than kMaxXmlDepth, more than kMaxXmlNodes elements, anything that is not well formed
std::optional<XmlNode> parseXml(const QByteArray &xml, qsizetype maxBytes,
                                QString *error = nullptr);

} // namespace Kinefin
