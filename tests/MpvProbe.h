// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QUrl>

#include <mpv/client.h>

namespace MpvProbe {
// The duration of a media file, read by an idle libmpv of its own
inline double duration(const QString &path)
{
    mpv_handle *handle = mpv_create();
    if (!handle)
        return 0;
    mpv_set_option_string(handle, "config", "no");
    mpv_set_option_string(handle, "terminal", "no");
    mpv_set_option_string(handle, "msg-level", "all=no");
    mpv_set_option_string(handle, "vo", "null");
    mpv_set_option_string(handle, "ao", "null");
    mpv_set_option_string(handle, "idle", "yes");
    if (mpv_initialize(handle) < 0) {
        mpv_terminate_destroy(handle);
        return 0;
    }
    const QByteArray url = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded).toUtf8();
    const char *command[] = {"loadfile", url.constData(), nullptr};
    mpv_command(handle, command);
    double seconds = 0;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 15000) {
        mpv_event *event = mpv_wait_event(handle, 0.1);
        if (!event)
            break;
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            mpv_get_property(handle, "duration", MPV_FORMAT_DOUBLE, &seconds);
            break;
        }
        if (event->event_id == MPV_EVENT_END_FILE)
            break;
    }
    mpv_terminate_destroy(handle);
    return seconds;
}
} // namespace MpvProbe
