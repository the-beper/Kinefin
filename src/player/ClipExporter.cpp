// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "player/ClipExporter.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QThread>

#include <mpv/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Kinefin {
namespace {
constexpr qint64 kMaxRunningBytes = 256LL * 1024 * 1024;

void setOption(mpv_handle *handle, const char *name, const QByteArray &value)
{
    mpv_set_option_string(handle, name, value.constData());
}

void setOption(mpv_handle *handle, const char *name, const char *value)
{
    setOption(handle, name, QByteArray(value));
}

void setOption(mpv_handle *handle, const char *name, const QString &value)
{
    setOption(handle, name, value.toUtf8());
}

int encoderThreads()
{
    return std::max(1, QThread::idealThreadCount() / 4);
}

const QSet<QString> &encoders()
{
    static const QSet<QString> list = [] {
        mpv_handle *handle = mpv_create();
        if (!handle)
            return QSet<QString>();
        setOption(handle, "config", "no");
        setOption(handle, "terminal", "no");
        setOption(handle, "msg-level", "all=no");
        if (mpv_initialize(handle) < 0) {
            mpv_terminate_destroy(handle);
            return QSet<QString>();
        }
        QSet<QString> names;
        mpv_node list{};
        if (mpv_get_property(handle, "encoder-list", MPV_FORMAT_NODE, &list) >= 0
            && list.format == MPV_FORMAT_NODE_ARRAY && list.u.list) {
            for (int i = 0; i < list.u.list->num; ++i) {
                const mpv_node &entry = list.u.list->values[i];
                if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list)
                    continue;
                for (int k = 0; k < entry.u.list->num; ++k) {
                    const mpv_node &field = entry.u.list->values[k];
                    if (entry.u.list->keys
                        && QByteArray(entry.u.list->keys[k]) == QByteArrayLiteral("driver")
                        && field.format == MPV_FORMAT_STRING && field.u.string)
                        names.insert(QString::fromUtf8(field.u.string));
                }
            }
            mpv_free_node_contents(&list);
        }
        mpv_terminate_destroy(handle);
        return names;
    }();
    return list;
}

void applySubtitleStyle(mpv_handle *handle, const QList<std::pair<QString, QVariant>> &properties)
{
    for (const auto &[name, value] : properties) {
        const QByteArray key = name.toUtf8();
        switch (value.typeId()) {
        case QMetaType::Double: {
            double number = value.toDouble();
            mpv_set_property(handle, key.constData(), MPV_FORMAT_DOUBLE, &number);
            break;
        }
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::Bool: {
            int64_t number = value.toLongLong();
            mpv_set_property(handle, key.constData(), MPV_FORMAT_INT64, &number);
            break;
        }
        default: {
            const QByteArray text = value.toString().toUtf8();
            mpv_set_property_string(handle, key.constData(), text.constData());
            break;
        }
        }
    }
}
} // namespace

ClipExporter::ClipExporter(QObject *parent) : QObject(parent) {}

ClipExporter::~ClipExporter()
{
    cancel();
}

void ClipExporter::wakeUp(void *context)
{
    QMetaObject::invokeMethod(static_cast<ClipExporter *>(context), &ClipExporter::processEvents,
                              Qt::QueuedConnection);
}

bool ClipExporter::hasEncoder(const QString &name)
{
    return encoders().contains(name);
}

bool ClipExporter::start(const ClipRequest &request, const QString &stagingPath)
{
    if (m_running || !request.source.isValid() || request.endTicks <= request.startTicks
        || stagingPath.isEmpty())
        return false;
    const bool gif = request.format == ClipRequest::Format::Gif;
    const bool mp4 = !gif && hasEncoder(QStringLiteral("libx264"));
    if (!gif && !mp4
        && !(hasEncoder(QStringLiteral("libvpx-vp9")) && hasEncoder(QStringLiteral("libopus")))) {
        Q_EMIT failed(Error::NoEncoder);
        return false;
    }
    if (gif && !hasEncoder(QStringLiteral("gif"))) {
        Q_EMIT failed(Error::NoEncoder);
        return false;
    }

    mpv_handle *handle = mpv_create();
    if (!handle) {
        Q_EMIT failed(Error::Source);
        return false;
    }
    m_start = double(request.startTicks) / 10'000'000.0;
    m_end = double(request.endTicks) / 10'000'000.0;
    m_staging = stagingPath;
    m_part = stagingPath + QStringLiteral(".part");
    m_tlsVerify = request.tlsVerify;
    m_tlsCaFile = request.tlsCaFile;
    m_authorization = request.authorization;

    setOption(handle, "o", m_part);
    setOption(handle, "of", QByteArray(gif ? "gif" : mp4 ? "mp4" : "webm"));
    setOption(handle, "start", QByteArray::number(m_start, 'f', 3));
    setOption(handle, "end", QByteArray::number(m_end, 'f', 3));
    setOption(handle, "hr-seek", "yes");
    setOption(handle, "ocopy-metadata", "no");
    setOption(handle, "config", "no");
    setOption(handle, "load-scripts", "no");
    setOption(handle, "ytdl", "no");
    setOption(handle, "hwdec", "no");
    setOption(handle, "terminal", "no");
    setOption(handle, "msg-level", "all=no");
    setOption(handle, "idle", "yes");
    setOption(handle, "demuxer-max-bytes", "32MiB");
    setOption(handle, "network-timeout", "15");
    setOption(handle, "vd-lavc-threads", QByteArray::number(encoderThreads()));
    setOption(handle, "ovcopts-add", QByteArray("threads=") + QByteArray::number(encoderThreads()));
    if (request.audioId > 0)
        setOption(handle, "aid", QByteArray::number(request.audioId));
    else
        setOption(handle, "aid", "no");
    if (request.subtitleId > 0)
        setOption(handle, "sid", QByteArray::number(request.subtitleId));
    if (request.secondarySubtitleId > 0)
        setOption(handle, "secondary-sid", QByteArray::number(request.secondarySubtitleId));
    if (!request.subtitleFiles.isEmpty()) {
        std::vector<QByteArray> files;
        files.reserve(size_t(request.subtitleFiles.size()));
        for (const QString &file : request.subtitleFiles)
            files.push_back(file.toUtf8());
        std::vector<mpv_node> nodes(files.size());
        mpv_node_list list{};
        list.num = int(nodes.size());
        list.values = nodes.data();
        for (size_t i = 0; i < files.size(); ++i) {
            nodes[i].format = MPV_FORMAT_STRING;
            nodes[i].u.string = files[i].data();
        }
        mpv_node node{};
        node.format = MPV_FORMAT_NODE_ARRAY;
        node.u.list = &list;
        mpv_set_option(handle, "sub-files", MPV_FORMAT_NODE, &node);
    }
    if (gif) {
        setOption(
            handle, "vf",
            QStringLiteral("sub,lavfi=[fps=15,scale=min(480\\,iw):-1:flags=lanczos,split[a][b];[a]"
                           "palettegen[p];[b][p]paletteuse]"));
        setOption(handle, "aid", "no");
    } else {
        setOption(handle, "vf", QStringLiteral("lavfi=[scale=-2:min(1080\\,ih),format=yuv420p]"));
        setOption(handle, "audio-channels", "stereo");
        if (mp4) {
            setOption(handle, "ovc", "libx264");
            setOption(handle, "ovcopts", "preset=veryfast,crf=21,maxrate=8M,bufsize=16M");
            setOption(handle, "oac", "aac");
            setOption(handle, "oacopts", "b=160000");
            setOption(handle, "ofopts", "movflags=+faststart");
        } else {
            setOption(handle, "ovc", "libvpx-vp9");
            setOption(handle, "ovcopts", "crf=32,b=0,row-mt=1,cpu-used=4");
            setOption(handle, "oac", "libopus");
            setOption(handle, "oacopts", "b=128000");
        }
    }
    if (mpv_initialize(handle) < 0) {
        mpv_terminate_destroy(handle);
        Q_EMIT failed(Error::Source);
        return false;
    }
    // The token only travels in the header node: never the URL, a file or a log line
    if (!m_authorization.isEmpty()) {
        const QByteArray header = QByteArrayLiteral("Authorization: ") + m_authorization;
        mpv_node value{};
        value.format = MPV_FORMAT_STRING;
        value.u.string = const_cast<char *>(header.constData());
        mpv_node_list list{};
        list.num = 1;
        list.values = &value;
        mpv_node node{};
        node.format = MPV_FORMAT_NODE_ARRAY;
        node.u.list = &list;
        mpv_set_property(handle, "http-header-fields", MPV_FORMAT_NODE, &node);
    }
    mpv_set_property_string(handle, "tls-verify", m_tlsVerify ? "yes" : "no");
    if (!m_tlsCaFile.isEmpty())
        mpv_set_property_string(handle, "tls-ca-file", m_tlsCaFile.toUtf8().constData());
    applySubtitleStyle(handle, request.subtitleStyle);

    m_handle = handle;
    m_running = true;
    mpv_set_wakeup_callback(handle, wakeUp, this);
    mpv_observe_property(handle, 1, "time-pos", MPV_FORMAT_DOUBLE);
    const QByteArray url = request.source.toString(QUrl::FullyEncoded).toUtf8();
    const char *command[] = {"loadfile", url.constData(), nullptr};
    if (mpv_command(handle, command) < 0) {
        finish(false, Error::Source);
        return false;
    }
    return true;
}

void ClipExporter::cancel()
{
    if (!m_running)
        return;
    m_running = false;
    mpv_set_wakeup_callback(m_handle, nullptr, nullptr);
    const char *command[] = {"stop", nullptr};
    mpv_command(m_handle, command);
    mpv_terminate_destroy(m_handle);
    m_handle = nullptr;
    QFile::remove(m_part);
}

void ClipExporter::processEvents()
{
    if (!m_running || !m_handle)
        return;
    while (true) {
        mpv_event *event = mpv_wait_event(m_handle, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            if (event->reply_userdata != 1)
                break;
            auto *property = static_cast<mpv_event_property *>(event->data);
            if (!property || property->format != MPV_FORMAT_DOUBLE || !property->data)
                break;
            const double position = *static_cast<double *>(property->data);
            const double span = m_end - m_start;
            const double fraction =
                span > 0 ? std::clamp((position - m_start) / span, 0.0, 1.0) : 0.0;
            Q_EMIT progress(fraction);
            if (QFileInfo(m_part).size() > kMaxRunningBytes) {
                finish(false, Error::TooLarge);
                return;
            }
            break;
        }
        case MPV_EVENT_END_FILE: {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            finish(end && end->reason == MPV_END_FILE_REASON_EOF, Error::Encoder);
            return;
        }
        default:
            break;
        }
    }
}

void ClipExporter::finish(bool ok, Error error)
{
    if (!m_running)
        return;
    m_running = false;
    if (m_handle) {
        mpv_set_wakeup_callback(m_handle, nullptr, nullptr);
        mpv_terminate_destroy(m_handle);
        m_handle = nullptr;
    }
    if (!ok || !QFileInfo::exists(m_part)) {
        QFile::remove(m_part);
        Q_EMIT failed(error);
        return;
    }
    // A fresh staging path: nothing that already exists is replaced silently
    QFile::remove(m_staging);
    if (!QFile::rename(m_part, m_staging)) {
        QFile::remove(m_part);
        Q_EMIT failed(Error::Encoder);
        return;
    }
    Q_EMIT finished(m_staging);
}
} // namespace Kinefin
