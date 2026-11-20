#include "SpotifyWebLogin.h"

#include "Language.h"
#include "youtube/BrowserLogin.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

namespace {

// Spotify's sign-in page, returning to the web player afterwards (with a signed-in profile it goes straight through).
const char *kLoginUrl = "https://accounts.spotify.com/{lang}/login?continue=https%3A%2F%2Fopen.spotify.com%2F";
const char *kPlayerUrl = "https://open.spotify.com/";

} // namespace

SpotifyWebLogin::SpotifyWebLogin(QObject *parent)
    : QObject(parent)
{
    m_titleTimer.setInterval(700);
    connect(&m_titleTimer, &QTimer::timeout, this, &SpotifyWebLogin::checkWindowTitle);

    // When the window doesn't close by itself, we kill the process.
    m_closeTimer.setSingleShot(true);
    m_closeTimer.setInterval(12000);
    connect(&m_closeTimer, &QTimer::timeout, this, [this] { m_process.kill(); });

    m_portTimer.setInterval(250);
    connect(&m_portTimer, &QTimer::timeout, this, &SpotifyWebLogin::waitForDevTools);

    m_timeout.setSingleShot(true);
    m_timeout.setInterval(60000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_token.isEmpty())
            succeed();   // with whatever queries were seen
        else
            fail(tr("Couldn't read the Spotify session from the browser"));
    });

    // The web player sends its token on every API call — used if the /api/token response wasn't caught.
    m_fallback.setSingleShot(true);
    m_fallback.setInterval(5000);
    connect(&m_fallback, &QTimer::timeout, this, [this] {
        if (!m_bearer.isEmpty())
            gotToken(m_bearer, QDateTime::currentDateTimeUtc().addSecs(30 * 60));
    });

    // A page counts as loaded once no new query has come for a while.
    m_settle.setSingleShot(true);
    m_settle.setInterval(6000);
    connect(&m_settle, &QTimer::timeout, this, &SpotifyWebLogin::nextPage);

    connect(&m_socket, &QWebSocket::connected, this, &SpotifyWebLogin::onConnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &SpotifyWebLogin::onMessage);
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this] {
        BrowserLogin::log(QStringLiteral("Spotify DevTools connection: %1").arg(m_socket.errorString()));
    });
    connect(&m_process, &QProcess::finished, this, &SpotifyWebLogin::onProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        BrowserLogin::log(QStringLiteral("Spotify browser process error %1: %2").arg(int(error)).arg(m_process.errorString()));
    });
}

SpotifyWebLogin::~SpotifyWebLogin()
{
    if (running())
        stopAll();
}

QString SpotifyWebLogin::status() const
{
    switch (m_stage) {
    case Stage::Interactive:
#ifdef Q_OS_WIN
        return tr("Sign in to Spotify in the window that opened. When the web player appears, the window closes by itself.");
#else
        return tr("Sign in to Spotify in the window that opened, and close it once you see the web player.");
#endif
    case Stage::Closing:
    case Stage::Reading:
        return tr("Reading the Spotify session…");
    case Stage::Idle:
        break;
    }
    return {};
}

void SpotifyWebLogin::setStage(Stage stage)
{
    const bool wasRunning = running();
    m_stage = stage;
    if (wasRunning != running())
        emit runningChanged();
    emit statusChanged();
}

bool SpotifyWebLogin::prepareProfile(const QString &profileDir)
{
    m_browser = BrowserLogin::findBrowser();
    if (m_browser.isEmpty()) {
        emit failed(tr("Chrome, Edge or Brave was not found"));
        return false;
    }
    m_profileDir = profileDir;
    m_signedOut = false;
    QDir().mkpath(profileDir);
    m_readerLaunched = false;
    m_lastTitles.clear();
    m_launchClock.invalidate();
    return true;
}

void SpotifyWebLogin::start(const QString &profileDir)
{
    if (running() || !prepareProfile(profileDir))
        return;
    BrowserLogin::log(QStringLiteral("--- Spotify sign-in with %1%2").arg(
        m_browser, BrowserLogin::debuggingBlocked(m_browser) ? QStringLiteral(" (remote debugging blocked by policy)") : QString()));
    setStage(Stage::Interactive);
    // A browser still on this profile would get the new window handed over, so it goes first.
    BrowserLogin::whenProfileFree(this, m_profileDir, 0, [this] {
        if (m_stage == Stage::Interactive)
            launchWindow();
    });
}

void SpotifyWebLogin::launchWindow()
{
    QFile::remove(m_profileDir + QStringLiteral("/lockfile"));
    QFile::remove(m_profileDir + QStringLiteral("/DevToolsActivePort"));
    m_process.setProgram(m_browser);
    m_process.setArguments({
        QStringLiteral("--user-data-dir=") + QDir::toNativeSeparators(m_profileDir),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--window-size=520,760"),
        QStringLiteral("--app=") + QString::fromLatin1(kLoginUrl).replace(QStringLiteral("{lang}"), Language::instance()->effective()),
    });
    m_process.start();
    m_launchClock.start();
    BrowserLogin::log(QStringLiteral("Spotify sign-in window started (pid %1)").arg(m_process.processId()));
    m_titleTimer.start();
}

void SpotifyWebLogin::refresh(const QString &profileDir)
{
    if (running())
        return;
    if (!QDir(profileDir).exists()) {
        emit failed(tr("Not signed in to Spotify"));
        return;
    }
    if (prepareProfile(profileDir)) {
        BrowserLogin::log(QStringLiteral("--- renewing the Spotify token with %1").arg(m_browser));
        startReading();
    }
}

void SpotifyWebLogin::cancel()
{
    if (!running())
        return;
    stopAll();
    emit failed(tr("Spotify sign-in was cancelled"));
}

// ---- stage 1: regular sign-in window ------------------------------------------------

void SpotifyWebLogin::checkWindowTitle()
{
    // After signing in Spotify redirects to the web player ("Spotify – Web Player: …"); before the page
    // provides a title, the window is named after the URL.
    const QList<qint64> owners = BrowserLogin::profileProcesses(m_profileDir, m_process.processId());
    const QStringList titles = BrowserLogin::profileWindowTitles(m_profileDir, m_process.processId());
    if (titles != m_lastTitles) {
        m_lastTitles = titles;
        BrowserLogin::log(QStringLiteral("Spotify window titles (processes %1): %2").arg(owners.size()).arg(titles.join(QStringLiteral(" | "))));
    }
#ifdef Q_OS_WIN
    // Nothing of the browser left on the profile: the user closed the window, maybe already signed in.
    if (owners.isEmpty() && m_launchClock.isValid() && m_launchClock.elapsed() > 1500) {
        BrowserLogin::log(QStringLiteral("the Spotify sign-in window was closed"));
        m_titleTimer.stop();
        startReading();
        return;
    }
#endif
    for (const QString &title : titles) {
        if (title.contains(QLatin1String("Web Player"), Qt::CaseInsensitive)
            || title.contains(QLatin1String("open.spotify.com"), Qt::CaseInsensitive)) {
            closeInteractiveWindow();
            return;
        }
    }
}

void SpotifyWebLogin::closeInteractiveWindow()
{
    m_titleTimer.stop();
    setStage(Stage::Closing);
    BrowserLogin::log(QStringLiteral("Spotify web player reached, closing the window"));
#ifdef Q_OS_WIN
    // Gently: Chrome then writes the cookies to disk. The readout waits until the profile is free.
    BrowserLogin::closeProfileWindows(m_profileDir, m_process.processId());
#else
    m_process.terminate();
#endif
    startReading();
}

void SpotifyWebLogin::onProcessFinished()
{
    BrowserLogin::log(QStringLiteral("Spotify browser process ended (exit code %1, %2 ms after start)")
                          .arg(m_process.exitCode()).arg(m_launchClock.isValid() ? m_launchClock.elapsed() : -1));
    switch (m_stage) {
    case Stage::Interactive:
#ifdef Q_OS_WIN
        // Not necessarily the window closing: a launcher hands the window to another process and quits.
        // checkWindowTitle() watches the profile and notices when its window is really gone.
#else
        // The user closed the window — they may already have signed in, so we check anyway.
        m_titleTimer.stop();
        startReading();
#endif
        break;
    case Stage::Closing:
        break;   // the readout already waits for the profile
    case Stage::Reading:
        if (m_readerLaunched && m_socket.state() == QAbstractSocket::UnconnectedState && !m_portTimer.isActive())
            fail(tr("Couldn't read the Spotify session from the browser"));
        break;
    case Stage::Idle:
        break;
    }
}

// ---- stage 2: invisible token readout ----------------------------------------------

void SpotifyWebLogin::startReading()
{
    if (m_stage == Stage::Reading)
        return;
    setStage(Stage::Reading);
    m_readerLaunched = false;
    m_portAttempts = 0;
    m_pending.clear();
    m_session.clear();
    m_tokenRequest.clear();
    m_bearer.clear();
    m_token.clear();
    m_queries = {};
    m_headers = {};
    m_pages.clear();
    m_timeout.start();

    // The hidden browser needs the profile to itself: a window still open on it would take the launch
    // over, and DevTools would never answer. A closing window gets a few seconds to save its cookies.
    BrowserLogin::whenProfileFree(this, m_profileDir, 12000, [this] {
        if (m_stage != Stage::Reading || m_readerLaunched)
            return;
        m_readerLaunched = true;
        QFile::remove(m_profileDir + QStringLiteral("/lockfile"));
        QFile::remove(m_profileDir + QStringLiteral("/DevToolsActivePort"));
        m_process.setProgram(m_browser);
        m_process.setArguments({
            QStringLiteral("--headless=new"),
            QStringLiteral("--remote-debugging-port=0"),
            QStringLiteral("--user-data-dir=") + QDir::toNativeSeparators(m_profileDir),
            QStringLiteral("--no-first-run"),
            QStringLiteral("--no-default-browser-check"),
            QStringLiteral("--mute-audio"),
            QStringLiteral("--window-size=1280,900"),   // the desktop layout of the web player
            QStringLiteral("about:blank"),
        });
        m_process.start();
        m_launchClock.start();
        BrowserLogin::log(QStringLiteral("Spotify hidden readout started (pid %1)").arg(m_process.processId()));
        m_portTimer.start();
    });
}

void SpotifyWebLogin::waitForDevTools()
{
    QFile file(m_profileDir + QStringLiteral("/DevToolsActivePort"));
    if (file.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
        if (lines.size() >= 2) {
            m_portTimer.stop();
            BrowserLogin::log(QStringLiteral("Spotify DevTools on port %1").arg(lines.at(0).trimmed()));
            m_socket.open(QUrl(QStringLiteral("ws://127.0.0.1:%1%2").arg(lines.at(0).trimmed(), lines.at(1).trimmed())));
            return;
        }
    }
    if (++m_portAttempts > 80) {   // ~20 s
        BrowserLogin::log(QStringLiteral("Spotify DevTools never answered; browser processes on the profile: %1")
                              .arg(BrowserLogin::profileProcesses(m_profileDir).size()));
        fail(BrowserLogin::debuggingBlocked(m_browser)
                 ? tr("A policy on this computer turns off remote debugging in the browser, so the session can't be read")
                 : tr("Couldn't read the Spotify session from the browser (DevTools isn't responding)"));
    }
}

void SpotifyWebLogin::send(const QString &method, const QJsonObject &params, Reply reply)
{
    QJsonObject request { { "id", ++m_requestId }, { "method", method }, { "params", params } };
    // Page-level commands go through the tab's session (flattened protocol).
    if (!m_session.isEmpty() && !method.startsWith(QLatin1String("Browser.")) && !method.startsWith(QLatin1String("Target.")))
        request.insert("sessionId", m_session);
    if (reply)
        m_pending.insert(m_requestId, reply);
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
}

void SpotifyWebLogin::onConnected()
{
    send(QStringLiteral("Browser.getVersion"), {}, [this](const QJsonObject &version) {
        // Headless Chrome says "HeadlessChrome" in its user agent — the web player gets the regular one.
        const QString userAgent = version.value("userAgent").toString().replace(QLatin1String("HeadlessChrome"), QLatin1String("Chrome"));
        send(QStringLiteral("Target.createTarget"), { { "url", "about:blank" } }, [this, userAgent](const QJsonObject &target) {
            const QJsonObject attach { { "targetId", target.value("targetId") }, { "flatten", true } };
            send(QStringLiteral("Target.attachToTarget"), attach, [this, userAgent](const QJsonObject &attached) {
                m_session = attached.value("sessionId").toString();
                if (m_session.isEmpty()) {
                    fail(tr("Couldn't read the Spotify session from the browser"));
                    return;
                }
                send(QStringLiteral("Network.enable"), {});
                if (!userAgent.isEmpty())
                    send(QStringLiteral("Network.setUserAgentOverride"), { { "userAgent", userAgent } });
                send(QStringLiteral("Page.navigate"), { { "url", QString::fromLatin1(kPlayerUrl) } });
            });
        });
    });
}

void SpotifyWebLogin::onMessage(const QString &message)
{
    if (m_stage != Stage::Reading)
        return;
    const QJsonObject msg = QJsonDocument::fromJson(message.toUtf8()).object();
    if (msg.contains("id")) {
        const Reply reply = m_pending.take(msg.value("id").toInt());
        if (reply)
            reply(msg.value("result").toObject());
        return;
    }
    onEvent(msg.value("method").toString(), msg.value("params").toObject());
}

void SpotifyWebLogin::captureQuery(const QJsonObject &request)
{
    // The web player's GraphQL calls ("persisted queries": operation name + hash of the query text).
    // Everplay repeats the same ones for the library — the public Web API is rate-limited for this token.
    const QUrl url(request.value("url").toString());
    if (url.host() != QLatin1String("api-partner.spotify.com") || !url.path().contains(QLatin1String("pathfinder")))
        return;
    QString name, hash;
    QJsonObject variables;
    if (request.value("method").toString() == QLatin1String("POST")) {
        const QJsonObject body = QJsonDocument::fromJson(request.value("postData").toString().toUtf8()).object();
        name = body.value("operationName").toString();
        variables = body.value("variables").toObject();
        hash = body.value("extensions").toObject().value("persistedQuery").toObject().value("sha256Hash").toString();
    } else {
        const QUrlQuery q(url);
        name = q.queryItemValue(QStringLiteral("operationName"), QUrl::FullyDecoded);
        variables = QJsonDocument::fromJson(q.queryItemValue(QStringLiteral("variables"), QUrl::FullyDecoded).toUtf8()).object();
        hash = QJsonDocument::fromJson(q.queryItemValue(QStringLiteral("extensions"), QUrl::FullyDecoded).toUtf8())
                   .object().value("persistedQuery").toObject().value("sha256Hash").toString();
    }
    if (name.isEmpty() || hash.isEmpty())
        return;
    m_queries.insert(name, QJsonObject { { "url", url.toString(QUrl::RemoveQuery) },
                                         { "method", request.value("method").toString() },
                                         { "hash", hash }, { "variables", variables } });
    const QJsonObject headers = request.value("headers").toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        const QString key = it.key().toLower();
        if (key == QLatin1String("client-token") || key == QLatin1String("app-platform")
            || key == QLatin1String("spotify-app-version"))
            m_headers.insert(key, it.value());
    }
    if (m_settle.isActive())
        m_settle.start();   // the page is still loading — wait until it's quiet
}

void SpotifyWebLogin::gotToken(const QString &token, const QDateTime &expiresUtc)
{
    if (!m_token.isEmpty())
        return;
    m_token = token;
    m_expires = expiresUtc;
    // Let the web player load the library, then open liked songs so the code for them gets loaded.
    m_pages = { QString::fromLatin1(kPlayerUrl) + QStringLiteral("collection/tracks") };
    m_settle.start();
}

void SpotifyWebLogin::findQueriesInCode()
{
    // Liked songs come from the web player's local cache, so their query may never be sent while we
    // watch. Its name and hash are in the player's code though — look up the ones we haven't seen.
    QStringList missing;
    for (const char *name : { "profileAttributes", "libraryV3", "fetchPlaylist", "fetchLibraryTracks" })
        if (!m_queries.contains(QLatin1String(name)))
            missing << QString::fromLatin1(name);
    if (missing.isEmpty()) {
        succeed();
        return;
    }
    const QString js = QStringLiteral(
        "(async () => { const want = %1, found = {};"
        " const urls = performance.getEntriesByType('resource').map(e => e.name).filter(u => /\\.js(\\?|$)/.test(u));"
        " for (const u of urls) { try { const t = await (await fetch(u)).text();"
        "   for (const m of t.matchAll(/\"(\\w+)\",\"query\",\"([0-9a-f]{64})\"/g))"
        "     if (want.includes(m[1])) found[m[1]] = m[2]; } catch (e) {} }"
        " return JSON.stringify(found); })()")
        .arg(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(missing)).toJson(QJsonDocument::Compact)));
    send(QStringLiteral("Runtime.evaluate"), { { "expression", js }, { "returnByValue", true }, { "awaitPromise", true } },
         [this](const QJsonObject &result) {
        const QJsonObject found = QJsonDocument::fromJson(result.value("result").toObject().value("value").toString().toUtf8()).object();
        for (auto it = found.begin(); it != found.end(); ++it) {
            m_queries.insert(it.key(), QJsonObject { { "url", "https://api-partner.spotify.com/pathfinder/v2/query" },
                                                     { "method", "POST" }, { "hash", it.value() },
                                                     { "variables", QJsonObject() } });
        }
        succeed();
    });
}

void SpotifyWebLogin::nextPage()
{
    if (m_pages.isEmpty()) {
        findQueriesInCode();
        return;
    }
    send(QStringLiteral("Page.navigate"), { { "url", m_pages.takeFirst() } });
    m_settle.start();
}

void SpotifyWebLogin::onEvent(const QString &method, const QJsonObject &params)
{
    if (method == QLatin1String("Network.requestWillBeSent")) {
        const QJsonObject request = params.value("request").toObject();
        const QJsonObject headers = request.value("headers").toObject();
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            const QString value = it.value().toString();
            if (it.key().compare(QLatin1String("authorization"), Qt::CaseInsensitive) == 0
                && value.startsWith(QLatin1String("Bearer "), Qt::CaseInsensitive) && m_bearer.isEmpty()) {
                m_bearer = value.mid(7).trimmed();
                m_fallback.start();
            }
        }
        captureQuery(request);
    } else if (method == QLatin1String("Network.responseReceived")) {
        const QString url = params.value("response").toObject().value("url").toString();
        if (url.contains(QLatin1String("open.spotify.com/api/token"))
            || url.contains(QLatin1String("open.spotify.com/get_access_token")))
            m_tokenRequest = params.value("requestId").toString();
    } else if (method == QLatin1String("Network.loadingFinished")) {
        if (m_tokenRequest.isEmpty() || params.value("requestId").toString() != m_tokenRequest)
            return;
        send(QStringLiteral("Network.getResponseBody"), { { "requestId", m_tokenRequest } }, [this](const QJsonObject &result) {
            QByteArray body = result.value("body").toString().toUtf8();
            if (result.value("base64Encoded").toBool())
                body = QByteArray::fromBase64(body);
            const QJsonObject json = QJsonDocument::fromJson(body).object();
            const QString token = json.value("accessToken").toString();
            if (token.isEmpty())
                return;   // maybe a later call — the timeout / fallback decide
            if (json.value("isAnonymous").toBool()) {
                m_signedOut = true;
                fail(tr("No Spotify sign-in detected — sign in in the window until you see the web player"));
                return;
            }
            const qint64 expires = qint64(json.value("accessTokenExpirationTimestampMs").toDouble());
            gotToken(token, expires > 0 ? QDateTime::fromMSecsSinceEpoch(expires, QTimeZone::UTC)
                                        : QDateTime::currentDateTimeUtc().addSecs(30 * 60));
        });
    }
}

void SpotifyWebLogin::succeed()
{
    BrowserLogin::log(QStringLiteral("Spotify signed in, token read"));
    const QString token = m_token;
    const QDateTime expires = m_expires;
    const QJsonObject web { { "queries", m_queries }, { "headers", m_headers } };
    stopAll();
    emit succeeded(token, expires, web);
}

void SpotifyWebLogin::fail(const QString &reason)
{
    BrowserLogin::log(QStringLiteral("Spotify failed: %1").arg(reason));
    stopAll();
    emit failed(reason);
}

void SpotifyWebLogin::stopAll()
{
    const Stage stage = m_stage;
    m_stage = Stage::Idle;   // first, so closing the process doesn't trigger further stages
    m_titleTimer.stop();
    m_closeTimer.stop();
    m_portTimer.stop();
    m_timeout.stop();
    m_fallback.stop();
    m_settle.stop();
    m_pending.clear();
    m_session.clear();

    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        const QJsonObject close { { "id", ++m_requestId }, { "method", "Browser.close" } };
        m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(close).toJson(QJsonDocument::Compact)));
        m_socket.flush();
    }
    m_socket.abort();

    if (m_process.state() != QProcess::NotRunning) {
#ifdef Q_OS_WIN
        if (stage == Stage::Interactive || stage == Stage::Closing)
            BrowserLogin::closeProfileWindows(m_profileDir, m_process.processId());
#else
        m_process.terminate();
#endif
        if (!m_process.waitForFinished(4000))
            m_process.kill();
    }

    if (stage != Stage::Idle) {
        emit runningChanged();
        emit statusChanged();
    }
}
