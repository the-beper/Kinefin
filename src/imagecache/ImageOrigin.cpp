// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagecache/ImageOrigin.h"

#include <QReadLocker>
#include <QWriteLocker>

namespace Kinefin {

void ImageOrigin::set(const QUrl &base)
{
    const QWriteLocker locker(&m_lock);
    m_base = base;
}

void ImageOrigin::clear()
{
    const QWriteLocker locker(&m_lock);
    m_base.clear();
}

QUrl ImageOrigin::get() const
{
    const QReadLocker locker(&m_lock);
    return m_base;
}

} // namespace Kinefin
