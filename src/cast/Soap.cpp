// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/Soap.h"

#include "cast/CastText.h"
#include "cast/XmlTree.h"

#include <QRegularExpression>

namespace Kinefin {
namespace {
constexpr QStringView kEnvelopeNs = u"http://schemas.xmlsoap.org/soap/envelope/";

bool isXmlName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.-]*$"));
    return pattern.match(name).hasMatch();
}

std::optional<qint64> parseDigits(QStringView text, int minDigits, int maxDigits)
{
    if (text.size() < minDigits || text.size() > maxDigits)
        return std::nullopt;
    qint64 value = 0;
    for (const QChar c : text) {
        if (c.unicode() < u'0' || c.unicode() > u'9')
            return std::nullopt;
        value = value * 10 + (c.unicode() - u'0');
    }
    return value;
}
} // namespace

bool isServiceType(const QString &type)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9:._-]+$"));
    return pattern.match(type).hasMatch();
}

int soapResponseLimit(const QString &action)
{
    return action == QLatin1StringView("GetProtocolInfo") ? kMaxProtocolInfoResponseBytes
                                                          : kMaxSoapResponseBytes;
}

QByteArray soapRequestBody(const QString &serviceType, const QString &action,
                           const QList<SoapArgument> &arguments)
{
    if (!isXmlName(action) || !isServiceType(serviceType))
        return {};
    QString body;
    for (const SoapArgument &argument : arguments) {
        if (!isXmlName(argument.name))
            return {};
        body += QLatin1Char('<');
        body += argument.name;
        body += QLatin1Char('>');
        body += xmlEscape(argument.value);
        body += QLatin1StringView("</");
        body += argument.name;
        body += QLatin1Char('>');
    }
    const QString envelope =
        QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
                       "<s:Body><u:%1 xmlns:u=\"%2\">%3</u:%1></s:Body></s:Envelope>")
            .arg(action, serviceType, body);
    return envelope.toUtf8();
}

QByteArray soapActionHeader(const QString &serviceType, const QString &action)
{
    // Both go into a header line: anything but the safe characters would let a device's description write headers of its own
    if (!isServiceType(serviceType) || !isXmlName(action))
        return {};
    return QByteArray("\"") + serviceType.toUtf8() + '#' + action.toUtf8() + '"';
}

SoapResult parseSoapResponse(const QByteArray &body, const QString &action,
                             const QStringList &expected)
{
    SoapResult result;
    const auto root = parseXml(body, soapResponseLimit(action));
    if (!root || !root->is(u"Envelope") || QStringView(root->ns) != kEnvelopeNs)
        return result;
    const XmlNode *envelopeBody = root->child(u"Body");
    if (!envelopeBody || envelopeBody->children.empty())
        return result;
    const XmlNode &payload = envelopeBody->children.front();

    if (payload.is(u"Fault")) {
        result.status = SoapStatus::Fault;
        if (const XmlNode *detail = payload.child(u"detail")) {
            if (const XmlNode *error = detail->child(u"UPnPError")) {
                const XmlNode *code = error->child(u"errorCode");
                bool ok = false;
                const int value = code ? code->trimmedText().toInt(&ok) : 0;
                result.errorCode = ok && value > 0 && value < 100000 ? value : 0;
                if (const XmlNode *description = error->child(u"errorDescription"))
                    result.errorDescription =
                        sanitizeDisplayText(description->trimmedText(), kMaxFaultDescriptionChars);
            }
        }
        return result;
    }
    const QString responseName = action + QLatin1StringView("Response");
    if (!payload.is(responseName))
        return result;
    result.status = SoapStatus::Ok;
    for (const XmlNode &argument : payload.children) {
        if (expected.contains(argument.name) && !result.values.contains(argument.name))
            result.values.insert(argument.name, argument.trimmedText());
    }
    return result;
}

QString upnpTime(qint64 milliseconds)
{
    constexpr qint64 maxSeconds = (kMaxUpnpHours + 1) * 3600 - 1;
    const qint64 seconds = qBound<qint64>(0, milliseconds / 1000, maxSeconds);
    return QStringLiteral("%1:%2:%3")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

std::optional<qint64> parseUpnpTime(QStringView text)
{
    text = text.trimmed();
    bool negative = false;
    if (text.startsWith(u'-') || text.startsWith(u'+')) {
        negative = text.startsWith(u'-');
        text = text.sliced(1);
    }
    const qsizetype firstColon = text.indexOf(u':');
    if (firstColon < 0)
        return std::nullopt;
    const qsizetype secondColon = text.indexOf(u':', firstColon + 1);
    if (secondColon < 0)
        return std::nullopt;
    const auto hours = parseDigits(text.first(firstColon), 1, 6);
    const auto minutes =
        parseDigits(text.sliced(firstColon + 1, secondColon - firstColon - 1), 1, 2);
    QStringView secondsPart = text.sliced(secondColon + 1);
    QStringView fraction;
    const qsizetype dot = secondsPart.indexOf(u'.');
    if (dot >= 0) {
        fraction = secondsPart.sliced(dot + 1);
        secondsPart = secondsPart.first(dot);
    }
    const auto seconds = parseDigits(secondsPart, 1, 2);
    if (!hours || !minutes || !seconds || *hours > kMaxUpnpHours || *minutes > 59 || *seconds > 59)
        return std::nullopt;
    qint64 millis = 0;
    if (dot >= 0) {
        const auto digits = parseDigits(fraction, 1, 9);
        if (!digits)
            return std::nullopt;
        // The first three digits are the milliseconds
        millis = fraction.left(3).toString().leftJustified(3, QLatin1Char('0')).toInt();
    }
    const qint64 total = ((*hours * 60 + *minutes) * 60 + *seconds) * 1000 + millis;
    return negative ? -total : total;
}

} // namespace Kinefin
