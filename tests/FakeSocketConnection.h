// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-written server side of one WebSocket connection (FakeJellyfin cannot hand already-buffered bytes to a QWebSocketServer, so the 101 handshake, unmasked server frames and the close-frame echo are written directly). One connection object per accepted upgrade attempt; client frames are always masked. A plain class on purpose: the fake recording is read after deterministic polling, and no test needs a signal
#pragma once

#include <QCryptographicHash>
#include <QList>
#include <QPointer>
#include <QTcpSocket>

class FakeSocketConnection final {
  public:
    explicit FakeSocketConnection(QTcpSocket *socket, const QByteArray &websocketKey)
        : m_socket(socket)
    {
        const QByteArray magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        QByteArray concatenate = websocketKey;
        concatenate += magic;
        const QByteArray accept =
            QCryptographicHash::hash(concatenate, QCryptographicHash::Sha1).toBase64();
        m_socket->write("HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: "
                        + accept + "\r\n\r\n");
        m_socket->flush();
    }

    // One complete text frame; server frames are never masked
    void sendText(const QString &text) { writeFrame(0x81, text.toUtf8()); }

    // One text frame with a payload of `bytes` letters, in the extended 64-bit length form
    void sendBigText(qint64 bytes)
    {
        writeFrame(0x81, QByteArray(qint64(bytes < 0 ? 0 : bytes), 'a'));
    }

    // A server-initiated close; the client is expected to echo with the same code
    void sendClose(quint16 code)
    {
        QByteArray payload;
        payload.append(char(quint8(code >> 8)));
        payload.append(char(quint8(code & 0xff)));
        writeFrame(0x88, payload);
    }

    // Abrupt TCP loss without any frame, the network-loss shape
    void kick()
    {
        if (m_socket)
            m_socket->abort();
    }

    bool alive() const { return m_socket && m_socket->state() == QAbstractSocket::ConnectedState; }

    // What the fake received from the client, in arrival order
    QStringList receivedTexts;
    QList<int> clientCloseCodes;

    // drain() is driven by FakeJellyfin's freshRead between frames; reads track the code and text of everything below
    void drain()
    {
        if (!m_socket)
            return;
        m_input.append(m_socket->readAll());
        for (;;) {
            if (m_input.size() < 2)
                return;
            const quint8 first = quint8(m_input.at(0));
            const quint8 second = quint8(m_input.at(1));
            const int opcode = first & 0x0f;
            const bool masked = second & 0x80;
            qint64 length = second & 0x7f;
            qsizetype offset = 2;
            if (length == 126) {
                if (m_input.size() < offset + 2)
                    return;
                length = (quint8(m_input.at(2)) << 8) | quint8(m_input.at(3));
                offset += 2;
            } else if (length == 127) {
                if (m_input.size() < offset + 8)
                    return;
                length = 0;
                for (int i = 0; i < 8; ++i)
                    length = qint64((length << 8) | quint8(m_input.at(2 + i)));
                offset += 8;
            }
            // Client frames are always masked; a fake is not a server mimic
            if (!masked)
                return;
            if (m_input.size() < offset + 4)
                return;
            const QByteArray maskKey = m_input.mid(offset, 4);
            offset += 4;
            if (m_input.size() - offset < length)
                return;
            QByteArray payload = m_input.mid(offset, int(length));
            for (qsizetype i = 0; i < payload.size(); ++i)
                payload[i] = char(payload.at(i) ^ maskKey.at(int(i) % 4));
            m_input.remove(0, int(offset + length));
            switch (opcode) {
            case 8: { // close
                int code = 1005;
                if (payload.size() >= 2)
                    code = (quint8(payload.at(0)) << 8) | quint8(payload.at(1));
                clientCloseCodes.append(code);
                writeFrame(0x88, payload);
                m_socket->disconnectFromHost();
                return;
            }
            case 1: // text
                receivedTexts.append(QString::fromUtf8(payload));
                break;
            case 9: { // ping: answer pong
                writeFrame(0x8a, payload);
                break;
            }
            default: // pong and control reminders: consume
                break;
            }
        }
    }

  private:
    void writeFrame(quint8 first, const QByteArray &payload)
    {
        if (!m_socket)
            return;
        QByteArray frame;
        frame.append(char(first));
        if (payload.size() <= 125) {
            frame.append(char(payload.size()));
        } else if (payload.size() <= 65535) {
            frame.append(char(126));
            frame.append(char(quint8((payload.size() >> 8) & 0xff)));
            frame.append(char(quint8(payload.size() & 0xff)));
        } else {
            frame.append(char(127));
            const quint64 length = quint64(payload.size());
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.append(char(quint8((length >> shift) & 0xff)));
        }
        frame.append(payload);
        m_socket->write(frame);
        m_socket->flush();
    }

    QPointer<QTcpSocket> m_socket;
    QByteArray m_input;
};
