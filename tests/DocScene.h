// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library of the documentation screenshots: the open films of the Blender Foundation and Blender Studio (CC BY, credits in the README) with their posters and frames from tests/assets/doc, so that no cover is drawn here and no personal data is in the pictures
#pragma once

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPainter>
#include <QSize>
#include <QString>

namespace DocScene {
struct Film {
    const char *key;
    const char *name;
    int year;
    int minutes;
    const char *genre;
    const char *overview;
};

// In the order of their years
inline const QList<Film> &films()
{
    static const QList<Film> list{
        {"elephants-dream", "Elephants Dream", 2006, 11, "Animation",
         "Two travellers, Emo and Proog, wander through a vast machine in the first open movie "
         "made with Blender."},
        {"big-buck-bunny", "Big Buck Bunny", 2008, 10, "Animation",
         "A giant rabbit takes his revenge on three rodents who bully the forest."},
        {"sintel", "Sintel", 2010, 15, "Animation",
         "A young woman searches a hostile world for the baby dragon she befriended."},
        {"tears-of-steel", "Tears of Steel", 2012, 12, "Science Fiction",
         "In a ruined Amsterdam, a group of fighters and scientists try to stop the robots that "
         "have taken over the city."},
        {"cosmos-laundromat", "Cosmos Laundromat", 2015, 12, "Animation",
         "A sheep called Franck meets a salesman who offers him a new life."},
        {"spring", "Spring", 2019, 8, "Animation",
         "A shepherd girl and her dog face the spirits of an ancient valley."},
        {"coffee-run", "Coffee Run", 2020, 3, "Animation",
         "A short animated film from Blender Studio."},
        {"sprite-fright", "Sprite Fright", 2021, 10, "Animation",
         "A horror comedy about a group of friends and the mushroom sprites of the forest."},
        {"charge", "Charge", 2022, 4, "Animation", "A short animated film from Blender Studio."}};
    return list;
}

// A 32 digit hexadecimal id: the kind (1 films, 2 the show, 3 seasons, 4 episodes, 5 people, 6 libraries) and the number
inline QString id(int kind, int number)
{
    return QStringLiteral("%1").arg((quint64(kind) << 32) | quint64(number), 32, 16,
                                    QLatin1Char('0'));
}

inline QByteArray fileBytes(const QString &name)
{
    QFile file(QStringLiteral(KINEFIN_SOURCE_DIR "/tests/assets/doc/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

inline QByteArray encoded(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 88);
    return bytes;
}

inline QByteArray poster(const Film &film)
{
    return fileBytes(QStringLiteral("poster-%1.jpg").arg(QString::fromLatin1(film.key)));
}

// A 16:9 picture out of the poster, for a wide card: its full width from a little under the top
inline QByteArray landscape(const Film &film)
{
    const QImage image = QImage::fromData(poster(film));
    const int height = image.width() * 9 / 16;
    return encoded(image.copy(0, image.height() / 10, image.width(), height)
                       .scaled(640, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// The initials in a circle, for a person: a portrait of the cast is not part of the open films' material
inline QByteArray avatar(const QString &name, int hue)
{
    QImage image(QSize(300, 300), QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(image.rect(), QColor::fromHsl(hue % 360, 110, 130));
    QFont font(QStringLiteral("Noto Sans"));
    font.setBold(true);
    font.setPixelSize(120);
    painter.setFont(font);
    painter.setPen(Qt::white);
    const QStringList words = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString initials =
        words.isEmpty() ? QString()
                        : words.first().left(1).toUpper()
                              + (words.size() > 1 ? words.last().left(1).toUpper() : QString());
    painter.drawText(image.rect(), Qt::AlignCenter, initials);
    painter.end();
    return encoded(image);
}

inline QJsonObject userData(double progress, qint64 ticks)
{
    QJsonObject data{{QStringLiteral("Played"), false},
                     {QStringLiteral("IsFavorite"), false},
                     {QStringLiteral("PlaybackPositionTicks"), qint64(ticks * progress)}};
    if (progress > 0)
        data.insert(QStringLiteral("PlayedPercentage"), progress * 100);
    return data;
}

// The BaseItemDto of a film as a card lists it
inline QJsonObject movieCard(int number, double progress = 0)
{
    const Film &film = films().at(number);
    const qint64 ticks = qint64(film.minutes) * 60 * 10'000'000;
    return QJsonObject{
        {QStringLiteral("Id"), id(1, number)},
        {QStringLiteral("Name"), QString::fromLatin1(film.name)},
        {QStringLiteral("Type"), QStringLiteral("Movie")},
        {QStringLiteral("ProductionYear"), film.year},
        {QStringLiteral("RunTimeTicks"), ticks},
        {QStringLiteral("ImageTags"), QJsonObject{{QStringLiteral("Primary"), QStringLiteral("p")},
                                                  {QStringLiteral("Thumb"), QStringLiteral("t")}}},
        {QStringLiteral("BackdropImageTags"), QJsonArray{QStringLiteral("b")}},
        {QStringLiteral("UserData"), userData(progress, ticks)}};
}

// An episode of season 1 of the show: the film of this number
inline QJsonObject episode(int number, const QString &seasonId, double progress = 0)
{
    const Film &film = films().at(number);
    const qint64 ticks = qint64(film.minutes) * 60 * 10'000'000;
    return QJsonObject{{QStringLiteral("Id"), id(4, number)},
                       {QStringLiteral("Name"), QString::fromLatin1(film.name)},
                       {QStringLiteral("Type"), QStringLiteral("Episode")},
                       {QStringLiteral("IndexNumber"), number + 1},
                       {QStringLiteral("ParentIndexNumber"), 1},
                       {QStringLiteral("SeasonId"), seasonId},
                       {QStringLiteral("SeriesId"), id(2, 0)},
                       {QStringLiteral("SeriesName"), QStringLiteral("Blender Open Movies")},
                       {QStringLiteral("RunTimeTicks"), ticks},
                       {QStringLiteral("Overview"), QString::fromLatin1(film.overview)},
                       {QStringLiteral("PremiereDate"),
                        QStringLiteral("%1-01-01T00:00:00.0000000Z").arg(film.year)},
                       {QStringLiteral("ImageTags"),
                        QJsonObject{{QStringLiteral("Primary"), QStringLiteral("p")}}},
                       {QStringLiteral("UserData"), userData(progress, ticks)}};
}
} // namespace DocScene
