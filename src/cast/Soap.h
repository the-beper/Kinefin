// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace Kinefin {

// A SOAP response is read only up to this size, GetProtocolInfo's up to the larger one (design: Security, 16 KiB, 64 KiB for GetProtocolInfo)
inline constexpr int kMaxSoapResponseBytes = 16 * 1024;
inline constexpr int kMaxProtocolInfoResponseBytes = 64 * 1024;
// A control request gives up after this long (design: Security, 3 s); the sending code enforces it
inline constexpr int kSoapTimeoutMs = 3000;
// Longest error description kept from a fault
inline constexpr int kMaxFaultDescriptionChars = 128;
// Largest time position parseUpnpTime accepts, in hours (six digits)
inline constexpr qint64 kMaxUpnpHours = 999999;

// The byte limit of the response to `action`
int soapResponseLimit(const QString &action);

struct SoapArgument {
    QString name;
    QString value;
};

// The value of the Content-Type header of a control request
inline constexpr QLatin1StringView kSoapContentType{"text/xml; charset=\"utf-8\""};

// The envelope of one action: the arguments in the order given, names and the service type checked to be plain XML names and URN characters, values escaped as text. Empty when a name is not valid
QByteArray soapRequestBody(const QString &serviceType, const QString &action,
                           const QList<SoapArgument> &arguments);

// The SOAPACTION header value, quotes included: `"<serviceType>#<action>"`
// The quoted SOAPACTION value; empty when the service type or the action holds anything but their safe characters
QByteArray soapActionHeader(const QString &serviceType, const QString &action);
// A UPnP service type as a header may carry it: letters, digits and :._- only
bool isServiceType(const QString &type);

enum class SoapStatus {
    // The response element of the action was found
    Ok,
    // A SOAP fault: errorCode and errorDescription come from its UPnPError, 0 and empty when it has none
    Fault,
    // Not a SOAP response to this action, or a limit was broken
    Invalid,
};

struct SoapResult {
    SoapStatus status = SoapStatus::Invalid;
    // The out arguments asked for, trimmed; one that is missing is absent
    QHash<QString, QString> values;
    int errorCode = 0;
    // Sanitized plain text
    QString errorDescription;
};

// Reads the body of a control response, whatever its HTTP status (a fault comes with a 500). Only the elements named in `expected` are read out of the response element; everything else is skipped. Limits: soapResponseLimit(action), the parseXml depth, node and DTD rules
SoapResult parseSoapResponse(const QByteArray &body, const QString &action,
                             const QStringList &expected);

// Seconds precision `HH:MM:SS` (hours at least two digits) of a position in milliseconds: the form every renderer takes for Seek REL_TIME. A negative value gives zero, one beyond kMaxUpnpHours its maximum
QString upnpTime(qint64 milliseconds);

// `H+:MM:SS[.F+]`, optionally signed, to milliseconds (the fraction truncated to milliseconds). Nullopt for `NOT_IMPLEMENTED`, anything else that does not fit the form, and more than kMaxUpnpHours hours. A negative result is returned as such: the caller clamps
std::optional<qint64> parseUpnpTime(QStringView text);

} // namespace Kinefin
