// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <QVariantList>
#include <QtQml/qqmlregistration.h>
#include <memory>

#include "api/JellyfinClient.h"
#include "settings/ServerCredentialSync.h"
#include "settings/TokenStore.h"

namespace Kinefin {
// What the Servers, sign-in and Home pages render: one active server at a time, the stored servers of the profile, and the state of the connection to the active one. It owns the JellyfinClient and the ServerCredentialSync of the profile and only reports what they report; the pages never talk to the client.
// `errorText` is translated and fixed: no URL, body or server text ever reaches it. `libraries` entries carry server metadata (`name`): render it as plain text.
class ConnectionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString quickConnectCode READ quickConnectCode NOTIFY quickConnectCodeChanged)
    Q_PROPERTY(QVariantList servers READ servers NOTIFY serversChanged)
    Q_PROPERTY(QVariantList libraries READ libraries NOTIFY librariesChanged)

  public:
    enum State {
        Idle,            // no active server
        Connecting,      // probing the address, or restoring a stored session
        ConfirmInsecure, // plain HTTP to a non-loopback host: waiting for the user's explicit opt-in
        NeedsLogin,      // the server is known, no session
        SigningIn,       // password login in flight
        QuickConnectPending, // a Quick Connect code is (about to be) shown
        Authenticated
    };
    Q_ENUM(State)

    // `deviceId` is the profile's stable device id (profileDeviceId()). Tokens go to `store`, which must outlive the controller; without one the user's KWallet is used.
    ConnectionController(const QString &profileId, const QString &deviceId, const QString &version,
                         TokenStore *store = nullptr, QObject *parent = nullptr);

    State state() const { return m_state; }
    QString errorText() const { return m_errorText; }
    QString serverUrl() const { return m_serverUrl; }
    QString quickConnectCode() const { return m_quickConnectCode; }
    QVariantList servers() const { return m_servers; }
    QVariantList libraries() const { return m_libraries; }

    // For tests; the default is 5 s.
    void setQuickConnectPollInterval(std::chrono::milliseconds interval);

    // Connects to the most recently used stored server, if there is one (its session is restored by ServerCredentialSync).
    Q_INVOKABLE void start();
    // `address` is what the user typed; without a scheme it is taken as https.
    Q_INVOKABLE void connectTo(const QString &address);
    // The user accepted sending credentials over plain HTTP to this server (this connection only).
    Q_INVOKABLE void confirmInsecureHttp();
    Q_INVOKABLE void login(const QString &username, const QString &password);
    Q_INVOKABLE void startQuickConnect();
    Q_INVOKABLE void cancelQuickConnect();
    // Disconnects the active server and returns to Idle, cancelling what is in flight; the stored session is kept (a later connect restores it).
    Q_INVOKABLE void cancel();
    // Forgets the active server's stored session and returns to Idle.
    Q_INVOKABLE void signOut();
    // Asks the server for the libraries again (the Home page's retry).
    Q_INVOKABLE void reloadLibraries();
    // Removes a stored server; signs out first when it is the active one.
    Q_INVOKABLE void removeServer(const QString &serverId);

  Q_SIGNALS:
    void stateChanged();
    void errorTextChanged();
    void serverUrlChanged();
    void quickConnectCodeChanged();
    void serversChanged();
    void librariesChanged();

  private:
    void onServerReady();
    void onRestoreFinished();
    void proceedAfterProbe();
    void onLoginSucceeded();
    void onSessionExpired();
    void onRequestFailed(int status);
    void onUserViews(const QJsonArray &items);
    void setState(State state);
    void setError(const QString &text);
    void clearQuickConnect();
    void clearLibraryError();
    void refreshServers();

    std::unique_ptr<TokenStore> m_ownedStore; // KWallet, unless a store was passed in
    JellyfinClient m_client;
    ServerCredentialSync m_sync;
    QTimer m_quickConnectTimer;
    State m_state = Idle;
    QString m_errorText;
    QString m_serverUrl;
    QString m_quickConnectCode;
    QVariantList m_servers;
    QVariantList m_libraries;
    bool m_identityValid = false;
    bool m_libraryError = false; // errorText currently shows the libraries' failure
};
} // namespace Kinefin
