// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/CastText.h"

#include <QChar>

namespace Kinefin {
namespace {
constexpr char32_t kInvalid = 0xFFFFFFFF;

enum class Kind { Drop, Space, Keep };

char32_t nextCodePoint(QStringView text, qsizetype &i)
{
    const QChar c = text[i++];
    if (!c.isSurrogate())
        return c.unicode();
    if (c.isHighSurrogate() && i < text.size() && text[i].isLowSurrogate()) {
        const QChar low = text[i++];
        return QChar::surrogateToUcs4(c, low);
    }
    return kInvalid;
}

Kind classify(char32_t cp)
{
    switch (QChar::category(cp)) {
    case QChar::Separator_Space:
        return Kind::Space;
    case QChar::Other_Control:
        return cp == U'\t' || cp == U'\n' || cp == U'\r' ? Kind::Space : Kind::Drop;
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return Kind::Drop;
    default:
        return Kind::Keep;
    }
}

bool isXmlChar(char32_t cp)
{
    return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF)
           || (cp >= 0xE000 && cp <= 0xFFFD) || (cp >= 0x10000 && cp <= 0x10FFFF);
}
} // namespace

QString sanitizeDisplayText(QStringView text, int maxChars)
{
    QString out;
    int count = 0;
    bool pendingSpace = false;
    for (qsizetype i = 0; i < text.size();) {
        const char32_t cp = nextCodePoint(text, i);
        if (cp == kInvalid)
            continue;
        const Kind kind = classify(cp);
        if (kind == Kind::Drop)
            continue;
        if (kind == Kind::Space) {
            pendingSpace = count > 0;
            continue;
        }
        if (pendingSpace) {
            // A space as the last kept character is pointless
            if (count + 1 >= maxChars)
                break;
            out += QLatin1Char(' ');
            ++count;
            pendingSpace = false;
        }
        if (count >= maxChars)
            break;
        out += QString::fromUcs4(&cp, 1);
        ++count;
    }
    return out;
}

QString xmlEscape(QStringView text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size();) {
        const char32_t cp = nextCodePoint(text, i);
        if (cp == kInvalid || !isXmlChar(cp))
            continue;
        switch (cp) {
        case U'&':
            out += QLatin1StringView("&amp;");
            break;
        case U'<':
            out += QLatin1StringView("&lt;");
            break;
        case U'>':
            out += QLatin1StringView("&gt;");
            break;
        case U'"':
            out += QLatin1StringView("&quot;");
            break;
        case U'\'':
            out += QLatin1StringView("&apos;");
            break;
        default:
            out += QString::fromUcs4(&cp, 1);
        }
    }
    return out;
}

} // namespace Kinefin
