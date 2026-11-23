// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The mpv.conf format Kinefin reads (the profile's file and the Manual MPV Configuration setting), libmpv's verdict on its entries, and the report the Settings page shows
#include "player/MpvConfValidator.h"
#include "settings/ClientSettings.h"
#include "settings/MpvConf.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Kinefin::ClientSettings;
using Kinefin::MpvConfProblem;
using Kinefin::parseMpvConf;

namespace {
QString kindOf(const Kinefin::MpvConf &conf, int line)
{
    for (const MpvConfProblem &problem : conf.problems) {
        if (problem.line == line)
            return QString::number(int(problem.kind));
    }
    return {};
}
} // namespace

class TestMpvConf : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void everyFormOfALine()
    {
        const Kinefin::MpvConf conf = parseMpvConf(QStringLiteral("# a comment\n"
                                                                  "\n"
                                                                  "  deband = yes  \n"
                                                                  "scale=ewa_lanczossharp\r\n"
                                                                  "save-position-on-quit\n"
                                                                  "no-osc\n"
                                                                  "sub-font=\"Noto Sans\"\n"
                                                                  "sub-ass-style-overrides='a=b'\n"
                                                                  "vf-add=\n"));
        QVERIFY(conf.problems.isEmpty());
        QCOMPARE(conf.entries.size(), 7);
        const auto at = [&conf](int row) { return conf.entries.at(row); };
        QCOMPARE(at(0).name, QStringLiteral("deband"));
        QCOMPARE(at(0).value, QStringLiteral("yes"));
        QCOMPARE(at(0).line, 3);
        QCOMPARE(at(1).value, QStringLiteral("ewa_lanczossharp"));
        QCOMPARE(at(2).name, QStringLiteral("save-position-on-quit"));
        QCOMPARE(at(2).value, QStringLiteral("yes"));
        QCOMPARE(at(3).name, QStringLiteral("osc"));
        QCOMPARE(at(3).value, QStringLiteral("no"));
        QCOMPARE(at(4).value, QStringLiteral("Noto Sans"));
        QCOMPARE(at(5).value, QStringLiteral("a=b"));
        QCOMPARE(at(6).value, QString());
    }

    // Comments after a value and the list suffixes, as mpv reads them
    void aTrailingCommentAndAListSuffixAreRead()
    {
        const Kinefin::MpvConf conf = parseMpvConf(QStringLiteral("volume-max=130 # loud\n"
                                                                  "deband # on\n"
                                                                  "sub-color=\"#FF0000\" # red\n"
                                                                  "sub-font='a #b'\n"
                                                                  "sub-border-color=#00FF00\n"
                                                                  "glsl-shaders-append=/x/a.glsl\n"
                                                                  "vf-clr=\n"
                                                                  "profile=fast\n"
                                                                  "sub-font=\"open\n"
                                                                  "sub-font=\"x\" junk\n"));
        QCOMPARE(conf.entries.size(), 8);
        const auto at = [&conf](int row) { return conf.entries.at(row); };
        QCOMPARE(at(0).value, QStringLiteral("130"));
        QCOMPARE(at(1).name, QStringLiteral("deband"));
        QCOMPARE(at(1).value, QStringLiteral("yes"));
        QCOMPARE(at(2).value, QStringLiteral("#FF0000"));
        QCOMPARE(at(3).value, QStringLiteral("a #b"));
        // Unquoted, the colour is a comment: the value is empty, as in mpv
        QCOMPARE(at(4).value, QString());
        QCOMPARE(at(5).name, QStringLiteral("glsl-shaders"));
        QCOMPARE(at(5).listOperation, QStringLiteral("append"));
        QCOMPARE(at(6).name, QStringLiteral("vf"));
        QCOMPARE(at(6).listOperation, QStringLiteral("clr"));
        QCOMPARE(at(7).name, QStringLiteral("profile"));
        QCOMPARE(at(7).value, QStringLiteral("fast"));
        // An unterminated quote and something after a closing one are syntax problems
        QCOMPARE(conf.problems.size(), 2);
        QCOMPARE(conf.problems.at(0).line, 9);
        QCOMPARE(conf.problems.at(0).kind, MpvConfProblem::Syntax);
        QCOMPARE(conf.problems.at(1).line, 10);
    }

    void aBadLineIsAProblemAndTheOthersApply()
    {
        const Kinefin::MpvConf conf = parseMpvConf(QStringLiteral("deband=yes\n"
                                                                  "Bad Name=1\n"
                                                                  "=value\n"
                                                                  "scripts=/tmp/x.lua\n"
                                                                  "http-header-fields=X: y\n"
                                                                  "include=/etc/other.conf\n"
                                                                  "input-ipc-server=/tmp/s\n"
                                                                  "volume-max=130\n"));
        QCOMPARE(conf.entries.size(), 2);
        QCOMPARE(conf.entries.at(1).name, QStringLiteral("volume-max"));
        QCOMPARE(conf.entries.at(1).line, 8);
        QCOMPARE(kindOf(conf, 2), QString::number(int(MpvConfProblem::Syntax)));
        QCOMPARE(kindOf(conf, 3), QString::number(int(MpvConfProblem::Syntax)));
        for (const int line : {4, 5, 6, 7})
            QCOMPARE(kindOf(conf, line), QString::number(int(MpvConfProblem::NotAllowed)));
        QVERIFY(!Kinefin::mpvOptionAllowed(QStringLiteral("input-conf")));
        QVERIFY(!Kinefin::mpvOptionAllowed(QStringLiteral("config-dir")));
        // A list suffix is the same option, and raw ffmpeg options are refused
        for (const char *name : {"scripts-append", "demuxer-lavf-o", "stream-lavf-o-append"})
            QVERIFY(!Kinefin::mpvOptionAllowed(QString::fromLatin1(name)));
        QVERIFY(Kinefin::mpvOptionAllowed(QStringLiteral("hwdec")));
        // A built-in profile is allowed: the file has no profiles of its own
        QVERIFY(Kinefin::mpvOptionAllowed(QStringLiteral("profile")));
        for (const char *name : {"input-ipc-server", "ytdl", "ytdl-raw-options", "load-scripts"})
            QVERIFY(!Kinefin::mpvOptionAllowed(QString::fromLatin1(name)));
    }

    void aProfileSectionAndWhatFollowsAreLeftOut()
    {
        const Kinefin::MpvConf conf = parseMpvConf(
            QStringLiteral("deband=yes\n[fast]\nscale=bilinear\n[slow]\ncscale=bilinear\n"));
        QCOMPARE(conf.entries.size(), 1);
        QCOMPARE(conf.problems.size(), 1);
        QCOMPARE(conf.problems.constFirst().line, 2);
        QCOMPARE(conf.problems.constFirst().kind, MpvConfProblem::Section);
    }

    void limitsAreEnforced()
    {
        const QString longName = QString(Kinefin::MaxMpvOptionNameLength + 1, QLatin1Char('a'));
        const QString longValue = QString(Kinefin::MaxMpvOptionValueLength + 1, QLatin1Char('x'));
        const Kinefin::MpvConf conf = parseMpvConf(longName + QStringLiteral("=1\nsub-font=")
                                                   + longValue + QStringLiteral("\n"));
        QVERIFY(conf.entries.isEmpty());
        QCOMPARE(conf.problems.size(), 2);
        QCOMPARE(conf.problems.at(0).kind, MpvConfProblem::TooLong);
        QCOMPARE(conf.problems.at(1).kind, MpvConfProblem::TooLong);
        QVERIFY(conf.problems.at(0).name.size() <= Kinefin::MaxMpvOptionNameLength);

        QString many;
        for (int i = 0; i < Kinefin::MaxMpvConfLines + 5; ++i)
            many += QStringLiteral("deband=yes\n");
        const Kinefin::MpvConf cut = parseMpvConf(many);
        QCOMPARE(cut.entries.size(), Kinefin::MaxMpvConfLines);
        QCOMPARE(cut.problems.size(), 1);
        QCOMPARE(cut.problems.constFirst().kind, MpvConfProblem::TooLong);
    }

    void libmpvNamesTheOptionsItDoesNotTakeAndNothingElse()
    {
        const Kinefin::MpvConf conf = parseMpvConf(QStringLiteral("deband=yes\n"
                                                                  "no-such-option=1\n"
                                                                  "cache-secs=lots\n"
                                                                  "volume-max=130\n"));
        QCOMPARE(conf.entries.size(), 4);
        const QList<MpvConfProblem> problems = Kinefin::validateMpvConf(conf.entries);
        QCOMPARE(problems.size(), 2);
        QCOMPARE(problems.at(0).line, 2);
        QCOMPARE(problems.at(0).kind, MpvConfProblem::UnknownOption);
        QCOMPARE(problems.at(1).line, 3);
        QCOMPARE(problems.at(1).kind, MpvConfProblem::BadValue);
        QVERIFY(Kinefin::validateMpvConf({}).isEmpty());
    }

    void theReportNamesTheFileAndWhatIsWrongWithIt()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ClientSettings settings(dir.filePath(QStringLiteral("kinefin.conf")));
        settings.setMpvConfValidator(&Kinefin::validateMpvConf);
        QCOMPARE(settings.mpvConfPath(), dir.filePath(QStringLiteral("mpv.conf")));
        // No file is no problem
        QVariantMap report = settings.mpvConfReport();
        QVERIFY(!report.value(QStringLiteral("exists")).toBool());
        QCOMPARE(report.value(QStringLiteral("applied")).toInt(), 0);
        QVERIFY(report.value(QStringLiteral("problems")).toList().isEmpty());

        QFile file(settings.mpvConfPath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(
            "deband=yes\nno-such-option=1\nscripts=/x.lua\nvolume-max=oops\nsub-font=Sans\n");
        file.close();
        report = settings.mpvConfReport();
        QVERIFY(report.value(QStringLiteral("exists")).toBool());
        QCOMPARE(report.value(QStringLiteral("applied")).toInt(), 2);
        const QVariantList problems = report.value(QStringLiteral("problems")).toList();
        QCOMPARE(problems.size(), 3);
        QCOMPARE(problems.at(0).toMap().value(QStringLiteral("line")).toInt(), 2);
        QCOMPARE(problems.at(0).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("unknownOption"));
        QCOMPARE(problems.at(1).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("notAllowed"));
        QCOMPARE(problems.at(2).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("volume-max"));
        QCOMPARE(problems.at(2).toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("badValue"));

        // Without libmpv's verdict only the parser decides
        ClientSettings plain(dir.filePath(QStringLiteral("kinefin.conf")));
        QCOMPARE(plain.mpvConfReport().value(QStringLiteral("problems")).toList().size(), 1);

        // A file larger than the limit, or a folder in its place, is not read
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(Kinefin::MaxMpvConfBytes + 1, 'a'));
        file.close();
        report = settings.mpvConfReport();
        QVERIFY(report.value(QStringLiteral("exists")).toBool());
        QCOMPARE(report.value(QStringLiteral("applied")).toInt(), 0);
        // The file as a whole is the problem, at line 0
        QVariantList whole = report.value(QStringLiteral("problems")).toList();
        QCOMPARE(whole.size(), 1);
        QCOMPARE(whole.constFirst().toMap().value(QStringLiteral("line")).toInt(), 0);
        QCOMPARE(whole.constFirst().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("tooLong"));

        // A folder in its place is not a regular file
        QVERIFY(QFile::remove(settings.mpvConfPath()));
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("mpv.conf")));
        whole = settings.mpvConfReport().value(QStringLiteral("problems")).toList();
        QCOMPARE(whole.size(), 1);
        QCOMPARE(whole.constFirst().toMap().value(QStringLiteral("kind")).toString(),
                 QStringLiteral("file"));
    }
};

QTEST_GUILESS_MAIN(TestMpvConf)
#include "test_mpv_conf.moc"
