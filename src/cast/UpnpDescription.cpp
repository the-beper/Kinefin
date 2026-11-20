// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cast/UpnpDescription.h"

#include "cast/CastText.h"
#include "cast/Soap.h"
#include "cast/Ssdp.h"
#include "cast/XmlTree.h"

#include <QRegularExpression>

#include <vector>

namespace Kinefin {
namespace {
enum class ServiceKind { AvTransport, ConnectionManager, RenderingControl, Other };

ServiceKind kindOf(const QString &serviceType)
{
    const auto is = [&serviceType](QLatin1StringView prefix) {
        return serviceType.startsWith(prefix);
    };
    if (is(QLatin1StringView("urn:schemas-upnp-org:service:AVTransport:")))
        return ServiceKind::AvTransport;
    if (is(QLatin1StringView("urn:schemas-upnp-org:service:ConnectionManager:")))
        return ServiceKind::ConnectionManager;
    if (is(QLatin1StringView("urn:schemas-upnp-org:service:RenderingControl:")))
        return ServiceKind::RenderingControl;
    return ServiceKind::Other;
}

// The trimmed text of a child element, or empty when it is missing or longer than maxChars
QString boundedText(const XmlNode *node, int maxChars)
{
    if (!node)
        return {};
    const QString text = node->trimmedText();
    return text.size() > maxChars ? QString() : text;
}

QString textOf(const XmlNode *node)
{
    return node ? node->trimmedText() : QString();
}

// Resolves a service URL reference. Nullopt: it does not parse, is too long, resolves to another origin or carries credentials. An empty reference gives an empty URL
std::optional<QUrl> resolveServiceUrl(const QUrl &base, const QUrl &location, const QString &ref)
{
    if (ref.isEmpty())
        return QUrl();
    if (ref.size() > kMaxServiceUrlChars)
        return std::nullopt;
    const QUrl relative(ref, QUrl::StrictMode);
    if (!relative.isValid())
        return std::nullopt;
    const QUrl resolved = base.resolved(relative);
    if (!resolved.isValid() || !resolved.userInfo().isEmpty() || !isSameOrigin(resolved, location))
        return std::nullopt;
    return resolved;
}

std::optional<UpnpService> parseService(const XmlNode &node, const QUrl &base, const QUrl &location)
{
    UpnpService service;
    service.serviceType = boundedText(node.child(u"serviceType"), kMaxListEntryChars);
    // It ends up in the SOAPACTION header: a type with anything else is no service
    if (service.serviceType.isEmpty() || !isServiceType(service.serviceType))
        return std::nullopt;
    const auto control = resolveServiceUrl(
        base, location, boundedText(node.child(u"controlURL"), kMaxServiceUrlChars));
    const auto scpd =
        resolveServiceUrl(base, location, boundedText(node.child(u"SCPDURL"), kMaxServiceUrlChars));
    const auto event = resolveServiceUrl(
        base, location, boundedText(node.child(u"eventSubURL"), kMaxServiceUrlChars));
    if (!control || !scpd || !event || control->isEmpty() || scpd->isEmpty())
        return std::nullopt;
    service.controlUrl = *control;
    service.scpdUrl = *scpd;
    service.eventSubUrl = *event;
    return service;
}

void collectDevices(const XmlNode &device, std::vector<const XmlNode *> &out)
{
    if (static_cast<int>(out.size()) >= kMaxDescriptionDevices)
        return;
    out.push_back(&device);
    const XmlNode *list = device.child(u"deviceList");
    if (!list)
        return;
    for (const XmlNode &candidate : list->children) {
        if (candidate.is(u"device"))
            collectDevices(candidate, out);
    }
}

bool isMediaRenderer(const XmlNode &device)
{
    return boundedText(device.child(u"deviceType"), kMaxListEntryChars)
        .startsWith(QLatin1StringView("urn:schemas-upnp-org:device:MediaRenderer:"));
}

QStringList boundedStrings(const XmlNode *parent, QStringView childName, QStringView valueName,
                           bool upperCase)
{
    QStringList out;
    if (!parent)
        return out;
    int seen = 0;
    for (const XmlNode &entry : parent->children) {
        if (!entry.is(childName))
            continue;
        if (++seen > kMaxListEntries)
            break;
        const XmlNode *source = valueName.isEmpty() ? &entry : entry.child(valueName);
        const QString value = boundedText(source, kMaxListEntryChars);
        if (value.isEmpty())
            continue;
        out.append(upperCase ? value.toUpper() : value);
    }
    return out;
}
} // namespace

bool isSameOrigin(const QUrl &a, const QUrl &b)
{
    if (a.scheme() != b.scheme() || a.port(80) != b.port(80))
        return false;
    QHostAddress hostA;
    QHostAddress hostB;
    const bool literalA = hostA.setAddress(hostWithoutBrackets(a));
    const bool literalB = hostB.setAddress(hostWithoutBrackets(b));
    if (literalA != literalB)
        return false;
    if (literalA)
        return normalizedAddress(hostA) == normalizedAddress(hostB);
    return !a.host().isEmpty() && a.host().compare(b.host(), Qt::CaseInsensitive) == 0;
}

std::optional<RendererInfo> parseDeviceDescription(const QByteArray &xml, const QUrl &location,
                                                   const QHostAddress &address,
                                                   const QString &expectedUdn, QString *error)
{
    const auto fail = [error](const char *why) -> std::optional<RendererInfo> {
        if (error)
            *error = QLatin1StringView(why);
        return std::nullopt;
    };
    if (!isValidLocation(location, address))
        return fail("location is not the address of the device");
    const auto root = parseXml(xml, kMaxDescriptionBytes, error);
    if (!root)
        return std::nullopt;
    if (!root->is(u"root"))
        return fail("not a device description");

    QUrl base = location;
    const QString urlBase = boundedText(root->child(u"URLBase"), kMaxServiceUrlChars);
    if (!urlBase.isEmpty()) {
        const QUrl declared(urlBase, QUrl::StrictMode);
        if (!declared.isValid() || !declared.userInfo().isEmpty()
            || !isSameOrigin(declared, location))
            return fail("URLBase is off-origin");
        base = declared;
    }

    const XmlNode *rootDevice = root->child(u"device");
    if (!rootDevice)
        return fail("no device");
    std::vector<const XmlNode *> devices;
    collectDevices(*rootDevice, devices);

    // A UDN asked for but malformed matches nothing: it must not fall back to the first renderer of the description
    const std::optional<QString> wanted = normalizeUdn(expectedUdn);
    if (!expectedUdn.isEmpty() && !wanted)
        return fail("the expected UDN is malformed");
    const QString wantedUdn = wanted.value_or(QString());
    const XmlNode *device = nullptr;
    QString udn;
    for (const XmlNode *candidate : devices) {
        if (!isMediaRenderer(*candidate))
            continue;
        const auto candidateUdn =
            normalizeUdn(boundedText(candidate->child(u"UDN"), kMaxListEntryChars));
        if (!candidateUdn || (!wantedUdn.isEmpty() && *candidateUdn != wantedUdn))
            continue;
        device = candidate;
        udn = *candidateUdn;
        break;
    }
    if (!device)
        return fail("no media renderer with a valid UDN");

    RendererInfo info;
    info.udn = udn;
    info.address = address;
    info.location = location;
    info.manufacturer = sanitizeDisplayText(textOf(device->child(u"manufacturer")));
    info.model = sanitizeDisplayText(textOf(device->child(u"modelName")));
    info.name = sanitizeDisplayText(textOf(device->child(u"friendlyName")));
    if (info.name.isEmpty())
        info.name = info.model;
    if (info.name.isEmpty())
        info.name = address.toString();

    if (const XmlNode *list = device->child(u"serviceList")) {
        int seen = 0;
        for (const XmlNode &entry : list->children) {
            if (!entry.is(u"service"))
                continue;
            if (++seen > kMaxListEntries)
                break;
            const auto service = parseService(entry, base, location);
            if (!service)
                continue;
            switch (kindOf(service->serviceType)) {
            case ServiceKind::AvTransport:
                if (!info.avTransport.isValid())
                    info.avTransport = *service;
                break;
            case ServiceKind::ConnectionManager:
                if (!info.connectionManager.isValid())
                    info.connectionManager = *service;
                break;
            case ServiceKind::RenderingControl:
                if (!info.renderingControl.isValid())
                    info.renderingControl = *service;
                break;
            case ServiceKind::Other:
                break;
            }
        }
    }
    if (!info.avTransport.isValid() || !info.connectionManager.isValid())
        return fail("no usable AVTransport and ConnectionManager");
    return info;
}

bool parseAvTransportScpd(const QByteArray &xml, RendererInfo &info, QString *error)
{
    const auto fail = [error](const char *why) {
        if (error)
            *error = QLatin1StringView(why);
        return false;
    };
    const auto root = parseXml(xml, kMaxDescriptionBytes, error);
    if (!root)
        return false;
    if (!root->is(u"scpd"))
        return fail("not a service description");

    const QStringList actions =
        boundedStrings(root->child(u"actionList"), u"action", u"name", false);
    for (const char *required :
         {"SetAVTransportURI", "Play", "Stop", "GetTransportInfo", "GetPositionInfo"}) {
        if (!actions.contains(QLatin1StringView(required)))
            return fail("AVTransport lacks a required action");
    }

    QStringList seekModes;
    if (const XmlNode *table = root->child(u"serviceStateTable")) {
        int seen = 0;
        for (const XmlNode &variable : table->children) {
            if (!variable.is(u"stateVariable"))
                continue;
            if (++seen > kMaxListEntries)
                break;
            if (boundedText(variable.child(u"name"), kMaxListEntryChars)
                != QLatin1StringView("A_ARG_TYPE_SeekMode"))
                continue;
            seekModes =
                boundedStrings(variable.child(u"allowedValueList"), u"allowedValue", u"", true);
            break;
        }
    }
    info.actions = actions;
    info.seekModes = seekModes;
    return true;
}

QStringList parseSinkProtocolInfo(const QString &sink)
{
    static const QRegularExpression mimePattern(
        QStringLiteral("^[a-z0-9][a-z0-9!#$&^_.+-]*/[a-z0-9][a-z0-9!#$&^_.+-]*$"));
    QStringList mimes;
    int seen = 0;
    for (const QString &entry : sink.split(QLatin1Char(','))) {
        if (++seen > kMaxListEntries)
            break;
        const QString trimmed = entry.trimmed();
        if (trimmed.isEmpty() || trimmed.size() > kMaxListEntryChars)
            continue;
        const QStringList fields = trimmed.split(QLatin1Char(':'));
        if (fields.size() < 4
            || fields.at(0).trimmed().compare(QLatin1StringView("http-get"), Qt::CaseInsensitive)
                   != 0)
            continue;
        QString mime = fields.at(2);
        const qsizetype parameters = mime.indexOf(QLatin1Char(';'));
        if (parameters >= 0)
            mime.truncate(parameters);
        mime = mime.trimmed().toLower();
        if (!mimePattern.match(mime).hasMatch() || mimes.contains(mime))
            continue;
        mimes.append(mime);
    }
    return mimes;
}

} // namespace Kinefin
