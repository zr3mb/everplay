#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>

#include <functional>

// Signing in to Spotify without a developer app (which needs Premium) — the same way as YouTube:
//
// 1. Interactive: the installed Chrome / Edge / Brave opens the Spotify sign-in page as a separate
//    app window with its own clean profile and without DevTools. Once the web player appears
//    (window title), the window is closed gently so the session gets written to disk.
//
// 2. Token: the same profile starts invisibly (headless) with the DevTools Protocol, opens
//    open.spotify.com and picks up the access token the web player gets for itself
//    (the /api/token response; the Authorization header of its API calls as a fallback).
//    The token lasts about an hour — refresh() repeats only this stage, the profile stays signed in.
//
// The user's regular browser profiles are not touched.
class SpotifyWebLogin : public QObject
{
    Q_OBJECT

public:
    enum class Stage { Idle, Interactive, Closing, Reading };

    explicit SpotifyWebLogin(QObject *parent = nullptr);
    ~SpotifyWebLogin() override;

    bool running() const { return m_stage != Stage::Idle; }
    bool interactive() const { return m_stage == Stage::Interactive || m_stage == Stage::Closing; }
    QString status() const;
    // The last attempt found the profile signed out of Spotify (not a browser or network problem).
    bool signedOut() const { return m_signedOut; }
    void start(const QString &profileDir);     // sign-in window, then the token
    void refresh(const QString &profileDir);   // only a new token from the signed-in profile
    void cancel();

signals:
    void runningChanged();
    void statusChanged();
    // web: { queries: { name: { url, method, hash, variables } }, headers: { client-token, … } }
    void succeeded(const QString &accessToken, const QDateTime &expiresUtc, const QJsonObject &web);
    void failed(const QString &reason);

private:
    using Reply = std::function<void(const QJsonObject &result)>;

    void setStage(Stage stage);
    bool prepareProfile(const QString &profileDir);
    void launchWindow();
    void checkWindowTitle();
    void closeInteractiveWindow();
    void startReading();
    void waitForDevTools();
    void onConnected();
    void send(const QString &method, const QJsonObject &params, Reply reply = {});
    void onMessage(const QString &message);
    void onEvent(const QString &method, const QJsonObject &params);
    void onProcessFinished();
    void captureQuery(const QJsonObject &request);
    void gotToken(const QString &token, const QDateTime &expiresUtc);
    void nextPage();
    void findQueriesInCode();
    void succeed();
    void fail(const QString &reason);
    void stopAll();

    QString m_browser;
    QString m_profileDir;
    Stage m_stage = Stage::Idle;
    QProcess m_process;
    QWebSocket m_socket;
    QTimer m_titleTimer;
    QTimer m_closeTimer;
    QTimer m_portTimer;
    QTimer m_timeout;
    QTimer m_fallback;
    QTimer m_settle;
    int m_portAttempts = 0;
    bool m_signedOut = false;
    bool m_readerLaunched = false;   // the hidden browser of the readout has been started
    QStringList m_lastTitles;        // for the log: only changes are written
    QElapsedTimer m_launchClock;
    int m_requestId = 0;
    QHash<int, Reply> m_pending;
    QString m_session;          // CDP session of the open.spotify.com tab
    QString m_tokenRequest;     // network request id of the /api/token call
    QString m_bearer;           // Authorization header seen on the web player's API calls
    QString m_token;
    QDateTime m_expires;
    QStringList m_pages;        // pages still to open so the web player shows its queries
    QJsonObject m_queries;
    QJsonObject m_headers;
};
