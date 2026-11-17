// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QReadWriteLock>
#include <QUrl>

#include <atomic>

namespace Kinefin {

// The one server base URL the image fetcher may talk to, and whether it accepts that server's invalid TLS certificates; shared by pointer between the GUI thread (which sets them on connect, on disconnect and when the setting changes) and the QML network thread (which reads them per request)
class ImageOrigin {
  public:
    void set(const QUrl &base);
    // An empty origin makes the image fetcher refuse every request
    void clear();
    QUrl get() const;
    void setIgnoreCertificateErrors(bool ignore)
    {
        if (m_ignoreCertificateErrors.exchange(ignore) && !ignore)
            ++m_acceptanceEnded;
    }
    [[nodiscard]] bool ignoreCertificateErrors() const { return m_ignoreCertificateErrors; }
    // Counts the times the option went off: a fetcher that sees a new count drops its kept-alive connections, which may have been accepted with an invalid certificate
    [[nodiscard]] quint64 acceptanceEnded() const { return m_acceptanceEnded; }

  private:
    mutable QReadWriteLock m_lock;
    QUrl m_base;
    std::atomic<bool> m_ignoreCertificateErrors = false;
    std::atomic<quint64> m_acceptanceEnded = 0;
};

} // namespace Kinefin
