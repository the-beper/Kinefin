// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace Kinefin {

// A description, an SCPD or a GetProtocolInfo document is read only up to this size (64 KiB)
inline constexpr int kMaxDescriptionBytes = 64 * 1024;
// A description or SCPD fetch gives up after this long (5 s); the fetching code enforces it
inline constexpr int kDescriptionTimeoutMs = 5000;
// Entries of one list (services, actions, allowed values) and characters of one entry (256 entries of 256 characters); more entries are ignored, a longer entry is skipped
inline constexpr int kMaxListEntries = 256;
inline constexpr int kMaxListEntryChars = 256;
// A sink lists hundreds of entries (338 seen), alphabetically: its raw entries are read up to this many, and the distinct MIME types kept are capped apart for `video/*` and for the rest, so that neither can push the other out
inline constexpr int kMaxSinkEntries = 4096;
inline constexpr int kMaxSinkVideoTypes = 256;
inline constexpr int kMaxSinkOtherTypes = 64;
// Longest service URL, absolute or relative
inline constexpr int kMaxServiceUrlChars = 512;
// Devices (the root and its embedded ones) a description is searched for the renderer
inline constexpr int kMaxDescriptionDevices = 16;

struct UpnpService {
    // The full `serviceType` as the device wrote it, `urn:schemas-upnp-org:service:AVTransport:1` for example: the SOAPACTION and the action element use it unchanged
    QString serviceType;
    QUrl controlUrl;
    QUrl eventSubUrl;
    QUrl scpdUrl;

    bool isValid() const
    {
        return !serviceType.isEmpty() && controlUrl.isValid() && !controlUrl.isEmpty();
    }
};

// One renderer, from its device description (parseDeviceDescription), the AVTransport SCPD (parseAvTransportScpd) and the sink of GetProtocolInfo (parseSinkProtocolInfo). The strings the renderer sent are sanitized plain text
struct RendererInfo {
    // `uuid:` plus the id, lower-cased
    QString udn;
    QString name;
    QString manufacturer;
    QString model;
    QHostAddress address;
    QUrl location;
    UpnpService avTransport;
    UpnpService connectionManager;
    // Not valid when the device has no RenderingControl
    UpnpService renderingControl;
    // Action names of the AVTransport SCPD
    QStringList actions;
    // Allowed values of A_ARG_TYPE_SeekMode, upper case
    QStringList seekModes;
    // Lower-case MIME types of the `http-get` entries of the sink
    QStringList sinkMimeTypes;

    bool hasAction(const QString &action) const { return actions.contains(action); }
    bool canPause() const { return hasAction(QStringLiteral("Pause")); }
    bool canSeekRelTime() const
    {
        return hasAction(QStringLiteral("Seek")) && seekModes.contains(QStringLiteral("REL_TIME"));
    }
    bool hasVolume() const { return renderingControl.isValid(); }
};

// True when both are the same scheme, port (80 by default) and host, an IP literal compared as an address
bool isSameOrigin(const QUrl &a, const QUrl &b);

// The MediaRenderer of a device description fetched from `location`, the answer of `address`. `expectedUdn` (from the SSDP USN, may be empty) picks the device when the root embeds several. Nullopt, with a fixed message in `error`, when: `location` is not the origin of `address` (isValidLocation), the document breaks a parseXml limit, there is no MediaRenderer device with a valid UDN, or its AVTransport or ConnectionManager service is missing or dropped. A service is dropped when its URLs do not resolve, or do not keep the scheme, host and port of `location`, or carry credentials; the same holds for `URLBase`, whose failure drops every service. Icons and everything else are never read
std::optional<RendererInfo> parseDeviceDescription(const QByteArray &xml, const QUrl &location,
                                                   const QHostAddress &address,
                                                   const QString &expectedUdn = {},
                                                   QString *error = nullptr);

// Reads the AVTransport SCPD into `info.actions` and `info.seekModes`. False, with a fixed message in `error`, when the document breaks a limit or lacks SetAVTransportURI, Play, Stop, GetTransportInfo or GetPositionInfo, which the service requires
bool parseAvTransportScpd(const QByteArray &xml, RendererInfo &info, QString *error = nullptr);

// The MIME types of the `http-get` entries of a ConnectionManager `Sink` (`protocol:network:contentFormat:additionalInfo`, comma separated): lower case, parameters removed, no duplicates, wildcards and malformed entries skipped, in the order the sink lists them. At most kMaxSinkEntries raw entries are read, and at most kMaxSinkVideoTypes `video/*` and kMaxSinkOtherTypes other types are kept
QStringList parseSinkProtocolInfo(const QString &sink);

} // namespace Kinefin
