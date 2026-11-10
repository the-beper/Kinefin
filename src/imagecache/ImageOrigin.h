// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QReadWriteLock>
#include <QUrl>

namespace Kinefin {

// The one server base URL the image fetcher may talk to; shared by pointer between the GUI thread (which sets it on connect and clears it on disconnect) and the QML network thread (which reads it per request)
class ImageOrigin {
  public:
    void set(const QUrl &base);
    // An empty origin makes the image fetcher refuse every request
    void clear();
    QUrl get() const;

  private:
    mutable QReadWriteLock m_lock;
    QUrl m_base;
};

} // namespace Kinefin
