// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The clip exporter on a real libmpv, from a generated lavfi source: no media files in the repo,
// no window and no network. The build image has no libx264, so a video clip is WebM there; on the
// runtime with codecs-extra it is MP4
#include "MpvProbe.h"

#include "player/ClipExporter.h"

#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <mpv/client.h>

using Kinefin::ClipExporter;
using Kinefin::ClipRequest;

namespace {
constexpr qint64 kSecond = 10'000'000;

// A four-second video with a sine, made by an idle libmpv from the lavfi source: the lavfi URL
// cannot travel through a QUrl (no valid host/port shape), so mpv gets it as one command argument
// here, and every export reads the file this made
QString makeSource(const QTemporaryDir &dir)
{
    const bool mp4 = ClipExporter::hasEncoder(QStringLiteral("libx264"));
    const QString path =
        dir.filePath(mp4 ? QStringLiteral("source.mp4") : QStringLiteral("source.webm"));
    mpv_handle *handle = mpv_create();
    if (!handle)
        return QString();
    mpv_set_option_string(handle, "config", "no");
    mpv_set_option_string(handle, "terminal", "no");
    mpv_set_option_string(handle, "msg-level", "all=no");
    mpv_set_option_string(handle, "o", path.toUtf8().constData());
    mpv_set_option_string(handle, "of", mp4 ? "mp4" : "webm");
    mpv_set_option_string(handle, "end", "4");
    mpv_set_option_string(handle, "ovc", mp4 ? "libx264" : "libvpx-vp9");
    mpv_set_option_string(handle, "ovcopts",
                          mp4 ? "preset=ultrafast,crf=30"
                              : "crf=40,b=0,deadline=realtime,cpu-used=8");
    mpv_set_option_string(handle, "oac", mp4 ? "aac" : "libopus");
    if (mpv_initialize(handle) < 0) {
        mpv_terminate_destroy(handle);
        return QString();
    }
    mpv_command_string(handle, "loadfile \"av://lavfi:testsrc2=duration=4\"");
    QElapsedTimer timer;
    timer.start();
    bool ok = false;
    while (timer.elapsed() < 60000) {
        mpv_event *event = mpv_wait_event(handle, 0.1);
        if (!event)
            break;
        if (event->event_id == MPV_EVENT_END_FILE) {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            ok = end && end->reason == MPV_END_FILE_REASON_EOF;
            break;
        }
    }
    mpv_terminate_destroy(handle);
    return ok ? path : QString();
}

ClipRequest request(const QString &source, int start, int end)
{
    ClipRequest clip;
    clip.source = QUrl::fromLocalFile(source);
    clip.startTicks = start * kSecond;
    clip.endTicks = end * kSecond;
    return clip;
}

QString clipPath(const QTemporaryDir &dir, bool gif)
{
    const QString extension = gif ? QStringLiteral(".gif")
                              : ClipExporter::hasEncoder(QStringLiteral("libx264"))
                                  ? QStringLiteral(".mp4")
                                  : QStringLiteral(".webm");
    return dir.filePath(QStringLiteral("clip") + extension);
}
} // namespace

class TestClipExporter : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void webmClipHasTheRangeLength()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(ClipExporter::hasEncoder(QStringLiteral("libvpx-vp9")));
        QVERIFY(ClipExporter::hasEncoder(QStringLiteral("libopus")));
        const QString source = makeSource(dir);
        QVERIFY(!source.isEmpty());
        ClipExporter exporter;
        QSignalSpy finished(&exporter, &ClipExporter::finished);
        QSignalSpy failed(&exporter, &ClipExporter::failed);
        QSignalSpy progress(&exporter, &ClipExporter::progress);
        const QString path = clipPath(dir, false);
        QVERIFY(exporter.start(request(source, 1, 3), path));
        QVERIFY(exporter.running());
        QVERIFY(QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 60000));
        QVERIFY2(finished.size() == 1, qPrintable(failed.isEmpty() ? QStringLiteral("no signal")
                                                                   : QStringLiteral("failed")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo(path).size() > 0);
        const double duration = MpvProbe::duration(path);
        QVERIFY2(duration > 1.8 && duration < 3.2, qPrintable(QString::number(duration)));
        // Progress never goes back, and the marks are reached
        QVERIFY(progress.count() > 0);
        double last = -1;
        for (const QList<QVariant> &values : std::as_const(progress)) {
            const double fraction = values.constFirst().toDouble();
            QVERIFY(fraction >= last);
            last = fraction;
        }
        QVERIFY(last > 0.5);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".part")));
    }

    void gifClipIsWritten()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(ClipExporter::hasEncoder(QStringLiteral("gif")));
        const QString source = makeSource(dir);
        QVERIFY(!source.isEmpty());
        ClipExporter exporter;
        QSignalSpy finished(&exporter, &ClipExporter::finished);
        QSignalSpy failed(&exporter, &ClipExporter::failed);
        const QString path = clipPath(dir, true);
        ClipRequest gif = request(source, 1, 3);
        gif.format = ClipRequest::Format::Gif;
        QVERIFY(exporter.start(gif, path));
        QVERIFY(QTest::qWaitFor([&] { return !finished.isEmpty() || !failed.isEmpty(); }, 60000));
        QVERIFY2(finished.size() == 1, qPrintable(failed.isEmpty() ? QStringLiteral("no signal")
                                                                   : QStringLiteral("failed")));
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(QFileInfo(path).size() > 0);
    }

    void cancelRemovesTheStagingFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString source = makeSource(dir);
        QVERIFY(!source.isEmpty());
        ClipExporter exporter;
        const QString path = dir.filePath(QStringLiteral("cancelled.webm"));
        QVERIFY(exporter.start(request(source, 0, 4), path));
        QVERIFY(exporter.running());
        exporter.cancel();
        QVERIFY(!exporter.running());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".part")));
    }

    void startRefusesAnInvertedRange()
    {
        QTemporaryDir dir;
        ClipExporter exporter;
        QSignalSpy failed(&exporter, &ClipExporter::failed);
        QVERIFY(
            !exporter.start(request(QString(), 5, 5), dir.filePath(QStringLiteral("none.webm"))));
        QVERIFY(!exporter.running());
    }
};

QTEST_GUILESS_MAIN(TestClipExporter)
#include "test_clip_exporter.moc"
