// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// When the HDR output path is on, the mpv target it asks for and the image description it gives the compositor
#include "player/HdrPolicy.h"

#include <QtTest/QtTest>
#include <cmath>
#include <limits>

using Kinefin::HdrInputs;
using Kinefin::HdrReason;

namespace {
// Everything a path that is on needs: a 600 cd/m2 output with 200 cd/m2 reference white
HdrInputs hdrReady()
{
    HdrInputs in;
    in.buildSupported = true;
    in.compositor.parametric = true;
    in.compositor.setLuminances = true;
    in.compositor.setMasteringPrimaries = true;
    in.compositor.pq = true;
    in.compositor.bt2020 = true;
    in.settingAuto = true;
    in.mainView = true;
    in.casting = false;
    in.videoIsHdr = true;
    in.video.gamma = QStringLiteral("pq");
    in.video.primaries = QStringLiteral("bt.2020");
    in.output.known = true;
    in.output.minLum = 0.005;
    in.output.maxLum = 400;
    in.output.refLum = 200;
    in.output.targetMaxLum = 600;
    return in;
}

HdrInputs withOutput(double targetMax, double ref)
{
    HdrInputs in = hdrReady();
    in.output.targetMaxLum = targetMax;
    in.output.refLum = ref;
    return in;
}
} // namespace

class TestHdrPolicy : public QObject {
    Q_OBJECT

  private Q_SLOTS:
    void readyInputsTurnItOn()
    {
        const Kinefin::HdrDecision d = Kinefin::decideHdr(hdrReady());
        QVERIFY(d.on);
        QCOMPARE(d.reason, HdrReason::On);
    }

    void lowHeadroomStaysSdr()
    {
        const Kinefin::HdrDecision d = Kinefin::decideHdr(withOutput(400, 390));
        QVERIFY(!d.on);
        QCOMPARE(d.reason, HdrReason::LowHeadroom);
    }

    void headroomAboveThreshold()
    {
        const Kinefin::HdrDecision d = Kinefin::decideHdr(withOutput(600, 200));
        QVERIFY(d.on);
        QCOMPARE(d.reason, HdrReason::On);
        // The threshold itself counts as enough (inferred: the design says "at least")
        QVERIFY(Kinefin::decideHdr(withOutput(300, 200)).on);
        QVERIFY(!Kinefin::decideHdr(withOutput(299, 200)).on);
    }

    void targetLuminanceIsPeak()
    {
        // A PQ output description without a target says nothing about the panel: stays SDR
        HdrInputs pq = hdrReady();
        pq.output.targetMaxLum.reset();
        pq.output.pqTransfer = true;
        pq.output.maxLum = 10000;
        const Kinefin::HdrDecision d = Kinefin::decideHdr(pq);
        QVERIFY(!d.on);
        QCOMPARE(d.reason, HdrReason::NoOutputInfo);
        // Not PQ: the output's maxLum is the peak
        HdrInputs gamma = hdrReady();
        gamma.output.targetMaxLum.reset();
        gamma.output.pqTransfer = false;
        gamma.output.maxLum = 600;
        QVERIFY(Kinefin::decideHdr(gamma).on);
        // A target wins over maxLum
        HdrInputs both = withOutput(400, 390);
        both.output.maxLum = 1000;
        QCOMPARE(Kinefin::decideHdr(both).reason, HdrReason::LowHeadroom);
    }

    void targetPeakScaling()
    {
        QCOMPARE(Kinefin::decideHdr(withOutput(600, 200)).targetPeak, 609);
        QCOMPARE(Kinefin::decideHdr(withOutput(1000, 203)).targetPeak, 1000);
        // Clamped to what mpv accepts
        const Kinefin::HdrDecision high = Kinefin::decideHdr(withOutput(6000, 100));
        QVERIFY(high.on);
        QCOMPARE(high.targetPeak, 10000);
        // No usable reference white: off, whatever the peak
        for (const double ref : {0.0, -5.0}) {
            const Kinefin::HdrDecision d = Kinefin::decideHdr(withOutput(600, ref));
            QVERIFY(!d.on);
            QCOMPARE(d.reason, HdrReason::NoOutputInfo);
        }
    }

    void unknownOutputIsNoInfo()
    {
        HdrInputs in = hdrReady();
        in.output.known = false;
        const Kinefin::HdrDecision d = Kinefin::decideHdr(in);
        QVERIFY(!d.on);
        QCOMPARE(d.reason, HdrReason::NoOutputInfo);
    }

    void descriptionPqBt2020Ref203()
    {
        const Kinefin::HdrDecision d = Kinefin::decideHdr(hdrReady());
        QVERIFY(d.on);
        QCOMPARE(d.description.minLum, 0.005);
        QCOMPARE(d.description.maxLum, uint32_t(10000));
        QCOMPARE(d.description.refLum, uint32_t(203));
        // No mastering data in the file: none is described
        QVERIFY(!d.description.masteringMin);
        QVERIFY(!d.description.masteringMax);
        QVERIFY(!d.description.maxCll);
        QVERIFY(!d.description.maxFall);
    }

    void masteringAndCll()
    {
        HdrInputs in = hdrReady();
        in.video.minLuma = 0.01;
        in.video.maxLuma = 1000;
        in.video.maxCll = 1000;
        in.video.maxFall = 400;
        Kinefin::HdrDecision d = Kinefin::decideHdr(in);
        QVERIFY(d.on);
        QVERIFY(d.description.masteringMin && d.description.masteringMax);
        QCOMPARE(*d.description.masteringMin, 0.01);
        QCOMPARE(*d.description.masteringMax, 1000.0);
        QVERIFY(d.description.maxCll && d.description.maxFall);
        QCOMPARE(*d.description.maxCll, uint32_t(1000));
        QCOMPARE(*d.description.maxFall, uint32_t(400));

        // Mastering luminance needs the compositor's mastering feature
        HdrInputs noFeature = in;
        noFeature.compositor.setMasteringPrimaries = false;
        d = Kinefin::decideHdr(noFeature);
        QVERIFY(d.on);
        QVERIFY(!d.description.masteringMin && !d.description.masteringMax);
        QVERIFY(d.description.maxCll);

        // A max_fall above max_cll is dropped, max_cll stays
        HdrInputs fall = in;
        fall.video.maxFall = 1200;
        d = Kinefin::decideHdr(fall);
        QVERIFY(d.description.maxCll);
        QVERIFY(!d.description.maxFall);

        // Without a max_cll only the range limits max_fall
        HdrInputs alone = in;
        alone.video.maxCll = 0;
        d = Kinefin::decideHdr(alone);
        QVERIFY(!d.description.maxCll);
        QVERIFY(d.description.maxFall);

        // Mastering max not above min
        HdrInputs inverted = in;
        inverted.video.minLuma = 1000;
        d = Kinefin::decideHdr(inverted);
        QVERIFY(!d.description.masteringMin && !d.description.masteringMax);
    }

    // The compositor ends the connection when max_cll or max_fall lies outside the mastering range (or, without one, the PQ volume from the minimum luminance sent)
    void lightLevelsOutsideTheMasteringRangeAreDropped()
    {
        HdrInputs in = hdrReady();
        in.video.minLuma = 0.005;
        in.video.maxLuma = 1000;
        in.video.maxCll = 4000;
        in.video.maxFall = 500;
        Kinefin::HdrDecision d = Kinefin::decideHdr(in);
        QVERIFY(d.on);
        QVERIFY(d.description.masteringMax);
        QVERIFY(!d.description.maxCll);
        // max_fall within the range stays on its own
        QVERIFY(d.description.maxFall);
        QCOMPARE(*d.description.maxFall, uint32_t(500));

        // max_fall above the mastering maximum
        in.video.maxCll = 0;
        in.video.maxFall = 1200;
        d = Kinefin::decideHdr(in);
        QVERIFY(!d.description.maxCll);
        QVERIFY(!d.description.maxFall);
        // Without mastering luminance the range is the PQ volume: both stay
        in.compositor.setMasteringPrimaries = false;
        in.video.maxCll = 4000;
        d = Kinefin::decideHdr(in);
        QVERIFY(d.description.maxCll && d.description.maxFall);
        QCOMPARE(*d.description.maxCll, uint32_t(4000));
        QCOMPARE(*d.description.maxFall, uint32_t(1200));
    }

    // A mastering maximum that rounds to the minimum or below is not sent: the range is then the PQ volume
    void masteringDroppedByRoundingKeepsThePqRange()
    {
        HdrInputs in = hdrReady();
        in.video.minLuma = 1.4;
        in.video.maxLuma = 1.45;
        in.video.maxCll = 1000;
        in.video.maxFall = 400;
        const Kinefin::HdrDecision d = Kinefin::decideHdr(in);
        QVERIFY(d.on);
        QVERIFY(!d.description.masteringMin && !d.description.masteringMax);
        QVERIFY(d.description.maxCll && d.description.maxFall);
        QCOMPARE(*d.description.maxCll, uint32_t(1000));
        QCOMPARE(*d.description.maxFall, uint32_t(400));
    }

    // The values in the description are the rounded ones the protocol compares
    void descriptionHoldsTheRoundedValues()
    {
        HdrInputs in = hdrReady();
        in.output.minLum = 0.00123456;
        in.video.minLuma = 0.00014;
        in.video.maxLuma = 999.6;
        const Kinefin::HdrDecision d = Kinefin::decideHdr(in);
        QVERIFY(d.on);
        QCOMPARE(d.description.minLum, 0.0012);
        QVERIFY(d.description.masteringMin && d.description.masteringMax);
        QCOMPARE(*d.description.masteringMin, 0.0001);
        QCOMPARE(*d.description.masteringMax, 1000.0);
        // A minimum at or above the reference white would make the description invalid
        in.output.minLum = 250;
        QCOMPARE(Kinefin::decideHdr(in).description.minLum, 0.0);
    }

    void lightLevelsAtOrBelowTheMinimumAreDropped()
    {
        const auto levels = [](double outputMin, double masteringMin, double masteringMax,
                               double cll) {
            HdrInputs in = hdrReady();
            in.output.minLum = outputMin;
            in.video.minLuma = masteringMin;
            in.video.maxLuma = masteringMax;
            in.video.maxCll = cll;
            return Kinefin::decideHdr(in).description;
        };
        // The PQ volume's minimum is the output's
        QVERIFY(!levels(5, 0, 0, 4).maxCll);
        QVERIFY(!levels(5, 0, 0, 5).maxCll);
        QVERIFY(levels(5, 0, 0, 6).maxCll);
        // The minimum counts after its rounding to 0.0001 cd/m2
        QVERIFY(!levels(0.99996, 0, 0, 1).maxCll);
        QVERIFY(levels(0.99994, 0, 0, 1).maxCll);
        // The mastering minimum, when it is sent
        QVERIFY(!levels(0.005, 3, 1000, 3).maxCll);
        QVERIFY(levels(0.005, 3, 1000, 4).maxCll);
        QVERIFY(levels(0.005, 3, 1000, 1000).maxCll);
        QVERIFY(!levels(0.005, 3, 1000, 1001).maxCll);
    }

    void untrustedFileValuesAreDropped()
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (const double bad : {nan, inf, -inf, -1.0, 10001.0, 1e12}) {
            HdrInputs in = hdrReady();
            in.video.minLuma = 0.01;
            in.video.maxLuma = bad;
            in.video.maxCll = bad;
            in.video.maxFall = bad;
            const Kinefin::HdrDecision d = Kinefin::decideHdr(in);
            QVERIFY2(d.on, qPrintable(QString::number(bad)));
            QVERIFY2(!d.description.masteringMax && !d.description.masteringMin,
                     qPrintable(QString::number(bad)));
            QVERIFY2(!d.description.maxCll, qPrintable(QString::number(bad)));
            QVERIFY2(!d.description.maxFall, qPrintable(QString::number(bad)));
        }
        HdrInputs negativeMin = hdrReady();
        negativeMin.video.minLuma = -0.5;
        negativeMin.video.maxLuma = 1000;
        const Kinefin::HdrDecision d = Kinefin::decideHdr(negativeMin);
        QVERIFY(!d.description.masteringMin && !d.description.masteringMax);
    }

    void missingFeatureUnavailable()
    {
        HdrInputs notBuilt = hdrReady();
        notBuilt.buildSupported = false;
        QCOMPARE(Kinefin::decideHdr(notBuilt).reason, HdrReason::NotBuilt);
        QVERIFY(!Kinefin::decideHdr(notBuilt).on);

        const auto missing = [](auto clear) {
            HdrInputs in = hdrReady();
            clear(in.compositor);
            return Kinefin::decideHdr(in);
        };
        for (const Kinefin::HdrDecision &d :
             {missing([](Kinefin::HdrCompositorSupport &c) { c.parametric = false; }),
              missing([](Kinefin::HdrCompositorSupport &c) { c.setLuminances = false; }),
              missing([](Kinefin::HdrCompositorSupport &c) { c.pq = false; }),
              missing([](Kinefin::HdrCompositorSupport &c) { c.bt2020 = false; })}) {
            QVERIFY(!d.on);
            QCOMPARE(d.reason, HdrReason::NoCompositorSupport);
        }
        // Mastering primaries are optional
        QVERIFY(
            missing([](Kinefin::HdrCompositorSupport &c) { c.setMasteringPrimaries = false; }).on);
    }

    void hlgInputPq()
    {
        // The output is always PQ: HLG video needs the PQ feature, not an HLG one
        HdrInputs in = hdrReady();
        in.video.gamma = QStringLiteral("hlg");
        const Kinefin::HdrDecision d = Kinefin::decideHdr(in);
        QVERIFY(d.on);
        QCOMPARE(d.targetPeak, 609);
        QCOMPARE(d.description.maxLum, uint32_t(10000));
        QCOMPARE(d.description.refLum, uint32_t(203));
        in.compositor.pq = false;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::NoCompositorSupport);
    }

    void gateOrder()
    {
        // Everything fails, then one gate after the other is satisfied in the documented order
        HdrInputs in;
        in.settingAuto = false;
        in.mainView = false;
        in.casting = true;
        in.failed = true;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::NotBuilt);
        in.buildSupported = true;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::NoCompositorSupport);
        in.compositor = hdrReady().compositor;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::SettingOff);
        in.settingAuto = true;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::Casting);
        in.casting = false;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::NotMainView);
        in.mainView = true;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::NotHdrVideo);
        in.videoIsHdr = true;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::NoOutputInfo);
        in.output = withOutput(400, 390).output;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::LowHeadroom);
        in.output = withOutput(600, 200).output;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::Failed);
        in.failed = false;
        QCOMPARE(Kinefin::decideHdr(in).reason, HdrReason::On);
        QVERIFY(Kinefin::decideHdr(in).on);
    }

    void mpvGammaOverridesHint()
    {
        using Kinefin::videoIsHdr;
        // mpv's gamma decides once known
        QVERIFY(videoIsHdr(QStringLiteral("pq"), QStringLiteral("SDR"), true));
        QVERIFY(videoIsHdr(QStringLiteral("hlg"), QString(), false));
        QVERIFY(videoIsHdr(QStringLiteral("pq"), QStringLiteral("HDR10"), false));
        QVERIFY(!videoIsHdr(QStringLiteral("bt.1886"), QStringLiteral("HDR10"), true));
        QVERIFY(!videoIsHdr(QStringLiteral("srgb"), QStringLiteral("DOVIWithHDR10"), true));
        // Before that, the server's range type, on the routes that play the file as it is
        for (const QString &type :
             {QStringLiteral("HDR10"), QStringLiteral("HDR10Plus"), QStringLiteral("HLG"),
              QStringLiteral("DOVI"), QStringLiteral("DOVIWithHDR10"),
              QStringLiteral("DOVIWithHLG"), QStringLiteral("DOVIWithHDR10Plus")}) {
            QVERIFY2(videoIsHdr(QString(), type, true), qPrintable(type));
            // A transcode ignores it: the server may have tone mapped
            QVERIFY2(!videoIsHdr(QString(), type, false), qPrintable(type));
        }
        QVERIFY(!videoIsHdr(QString(), QStringLiteral("SDR"), true));
        // A Dolby Vision file with an SDR base layer shows SDR
        QVERIFY(!videoIsHdr(QString(), QStringLiteral("DOVIWithSDR"), true));
        // An mpv gamma that is not known yet leaves it to the hint
        QVERIFY(videoIsHdr(QStringLiteral("auto"), QStringLiteral("HDR10"), true));
        QVERIFY(!videoIsHdr(QString(), QString(), true));
        QVERIFY(!videoIsHdr(QString(), QStringLiteral("Unknown"), true));
    }
};

QTEST_GUILESS_MAIN(TestHdrPolicy)
#include "test_hdr_policy.moc"
