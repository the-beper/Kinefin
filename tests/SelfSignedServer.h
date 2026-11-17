// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// A TLS server with a self-signed certificate for the certificate tests. Header-only, like FakeJellyfin.h; needs KINEFIN_SOURCE_DIR
#pragma once

#include <QByteArray>
#include <QFile>
#include <QHostAddress>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

// A client that verifies the certificate rejects the handshake: `handshakes` counts the connections that began one and `completed` those that finished it, which only a client that accepts the certificate does. A completed connection gets one fixed answer, `body` as JSON, and is closed
class SelfSignedServer {
  public:
    // The pair is a local test fixture, not part of the repository: without it the certificate tests skip
    static bool fixturesPresent()
    {
        const QString directory = fixtureDirectory();
        return QFile::exists(directory + QStringLiteral("selfsigned_cert.pem"))
               && QFile::exists(directory + QStringLiteral("selfsigned_key.pem"));
    }

    bool listen()
    {
        const QString directory = fixtureDirectory();
        QFile certificateFile(directory + QStringLiteral("selfsigned_cert.pem"));
        QFile keyFile(directory + QStringLiteral("selfsigned_key.pem"));
        if (!certificateFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly))
            return false;
        const QSslCertificate certificate(&certificateFile, QSsl::Pem);
        const QSslKey key(&keyFile, QSsl::Ec, QSsl::Pem);
        if (certificate.isNull() || key.isNull())
            return false;
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(certificate);
        configuration.setPrivateKey(key);
        m_server.setSslConfiguration(configuration);
        QObject::connect(&m_server, &QSslServer::startedEncryptionHandshake, &m_server,
                         [this] { ++handshakes; });
        // Which of the two signals a QSslServer emits for a connection whose handshake completed depends on the Qt version: the queue is drained by either, once
        const auto serve = [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                ++completed;
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    socket->readAll();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Connection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        };
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, serve);
        QObject::connect(&m_server, &QTcpServer::pendingConnectionAvailable, &m_server, serve);
        return m_server.listen(QHostAddress::LocalHost);
    }

    QString address() const { return QStringLiteral("127.0.0.1:%1").arg(m_server.serverPort()); }

    int handshakes = 0;
    int completed = 0;
    QByteArray body = QByteArrayLiteral("{}");

  private:
    static QString fixtureDirectory()
    {
        return QString::fromUtf8(KINEFIN_SOURCE_DIR) + QStringLiteral("/tests/fixtures/");
    }

    QSslServer m_server;
};
