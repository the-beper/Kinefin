// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/XmlTree.h"

#include <QXmlStreamReader>

namespace Kinefin {

const XmlNode *XmlNode::child(QStringView childName) const
{
    for (const XmlNode &candidate : children) {
        if (candidate.is(childName))
            return &candidate;
    }
    return nullptr;
}

std::optional<XmlNode> parseXml(const QByteArray &xml, qsizetype maxBytes, QString *error)
{
    const auto fail = [error](const char *why) -> std::optional<XmlNode> {
        if (error)
            *error = QLatin1StringView(why);
        return std::nullopt;
    };
    if (xml.isEmpty())
        return fail("empty document");
    if (xml.size() > maxBytes)
        return fail("document too large");

    QXmlStreamReader reader(xml);
    XmlNode root;
    bool haveRoot = false;
    int nodes = 0;
    // Pointers to the open elements only: a sibling is appended while its parent is the last open element, so no held pointer sees a reallocation
    std::vector<XmlNode *> open;
    while (!reader.atEnd()) {
        switch (reader.readNext()) {
        case QXmlStreamReader::DTD:
            return fail("DTD not allowed");
        case QXmlStreamReader::EntityReference:
            return fail("entity reference not allowed");
        case QXmlStreamReader::StartElement: {
            if (static_cast<int>(open.size()) >= kMaxXmlDepth)
                return fail("nesting too deep");
            if (++nodes > kMaxXmlNodes)
                return fail("too many elements");
            XmlNode *node = nullptr;
            if (open.empty()) {
                if (haveRoot)
                    return fail("more than one root element");
                haveRoot = true;
                node = &root;
            } else {
                open.back()->children.emplace_back();
                node = &open.back()->children.back();
            }
            node->name = reader.name().toString();
            node->ns = reader.namespaceUri().toString();
            open.push_back(node);
            break;
        }
        case QXmlStreamReader::EndElement:
            if (!open.empty())
                open.pop_back();
            break;
        case QXmlStreamReader::Characters:
            if (!open.empty()) {
                const QStringView chunk = reader.text();
                open.back()->text.append(chunk.data(), chunk.size());
            }
            break;
        default:
            break;
        }
    }
    if (reader.hasError())
        return fail("malformed XML");
    if (!haveRoot || !open.empty())
        return fail("incomplete document");
    return root;
}

} // namespace Kinefin
