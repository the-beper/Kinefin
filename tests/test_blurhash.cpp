// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/BlurHashImageProvider.h"
#include "imagecache/BlurHash.h"

#include <QtTest>

using Kinefin::decodeBlurHash;
using Kinefin::isValidBlurHash;

namespace {
constexpr QLatin1StringView kAlphabet(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~");

QString encode83(int value, int length)
{
    QString text;
    for (int i = 0; i < length; ++i) {
        text.prepend(kAlphabet.at(value % 83));
        value /= 83;
    }
    return text;
}

// A grid of one component: its colour alone
QString solid(int red, int green, int blue)
{
    return QStringLiteral("00") + encode83((red << 16) | (green << 8) | blue, 4);
}

// A grid of two by one: a grey with one component that is red on one side and not on the other
QString leftToRight()
{
    return QStringLiteral("1") + encode83(40, 1) + encode83((128 << 16) | (128 << 8) | 128, 4)
           + encode83(18 * 19 * 19 + 9 * 19 + 9, 2);
}
} // namespace

class TestBlurHash : public QObject {
    Q_OBJECT
  private Q_SLOTS:
    void aHashIsCheckedForItsCharactersAndItsLength()
    {
        QVERIFY(isValidBlurHash(solid(255, 0, 0)));
        QVERIFY(isValidBlurHash(leftToRight()));
        // The widest grid there is, 9 by 9: 4 + 2 * 81 characters
        QVERIFY(isValidBlurHash(QString(QStringLiteral("|") + QString(165, QLatin1Char('0')))));
        QVERIFY(!isValidBlurHash(QString()));
        QVERIFY(!isValidBlurHash(QStringLiteral("00")));
        // One character short and one long for the grid the first one names
        QVERIFY(!isValidBlurHash(solid(255, 0, 0).chopped(1)));
        QVERIFY(!isValidBlurHash(QString(solid(255, 0, 0) + QLatin1Char('0'))));
        // A character outside the alphabet, a space and a letter beyond ASCII
        QVERIFY(!isValidBlurHash(QString(QStringLiteral("00 ") + solid(255, 0, 0).mid(3))));
        QVERIFY(!isValidBlurHash(QString(QStringLiteral("00é") + solid(255, 0, 0).mid(3))));
        // The first character names a grid of 1 by 10 components, of the right length for it
        QVERIFY(!isValidBlurHash(QString(QStringLiteral("}") + QString(23, QLatin1Char('0')))));
    }

    void aSolidHashGivesItsColourEverywhere()
    {
        const QImage image = decodeBlurHash(solid(255, 0, 0), 16, 9);
        QCOMPARE(image.size(), QSize(16, 9));
        for (const auto &point : {QPoint(0, 0), QPoint(15, 8), QPoint(7, 4)}) {
            const QColor color = image.pixelColor(point);
            QVERIFY2(color.red() >= 253 && color.green() <= 2 && color.blue() <= 2,
                     qPrintable(color.name()));
        }
    }

    void theComponentsOfAHashShapeTheImage()
    {
        const QImage image = decodeBlurHash(leftToRight(), 16, 4);
        QVERIFY(!image.isNull());
        // Redder on the left than on the right, over the grey of the middle
        QVERIFY2(
            image.pixelColor(0, 2).red() > image.pixelColor(15, 2).red() + 20,
            qPrintable(QStringLiteral("%1 and %2")
                           .arg(image.pixelColor(0, 2).name(), image.pixelColor(15, 2).name())));
    }

    void aSizeOutOfRangeOrAnInvalidHashGivesNoImage()
    {
        QVERIFY(decodeBlurHash(solid(1, 2, 3), 0, 9).isNull());
        QVERIFY(decodeBlurHash(solid(1, 2, 3), 16, 0).isNull());
        QVERIFY(decodeBlurHash(solid(1, 2, 3), 65, 9).isNull());
        QVERIFY(decodeBlurHash(solid(1, 2, 3), 16, 65).isNull());
        QVERIFY(!decodeBlurHash(solid(1, 2, 3), 64, 64).isNull());
        QVERIFY(decodeBlurHash(QStringLiteral("nope"), 16, 9).isNull());
    }

    void theProviderReadsTheHashFromItsHexadecimalId()
    {
        Kinefin::BlurHashImageProvider provider;
        const QString id = QString::fromLatin1(solid(0, 255, 0).toLatin1().toHex());
        QSize size;
        // 32 by 18 when no size is asked for, the size asked for otherwise
        QCOMPARE(provider.requestImage(id, &size, QSize()).size(), QSize(32, 18));
        QCOMPARE(size, QSize(32, 18));
        QCOMPARE(provider.requestImage(id, &size, QSize(8, 4)).size(), QSize(8, 4));
        // What is no hexadecimal, an odd number of digits, a hash that is not one and a size past the limit give nothing
        QVERIFY(provider.requestImage(QStringLiteral("not hex"), &size, QSize()).isNull());
        QVERIFY(size.isEmpty());
        QVERIFY(provider.requestImage(id + QLatin1Char('0'), &size, QSize()).isNull());
        QVERIFY(
            provider.requestImage(QString::fromLatin1(QByteArray("hello").toHex()), &size, QSize())
                .isNull());
        QVERIFY(provider.requestImage(id, &size, QSize(100, 100)).isNull());
    }
};

QTEST_GUILESS_MAIN(TestBlurHash)
#include "test_blurhash.moc"
