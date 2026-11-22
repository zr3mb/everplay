#include "OnlineService.h"

#include "AppPaths.h"
#include "Library.h"
#include "ListeningHistory.h"
#include "LibraryScanner.h"
#include "PlayerController.h"
#include "Profile.h"
#include "soundcloud/SoundCloudService.h"
#include "spotify/SpotifyService.h"
#include "youtube/YouTubeService.h"
#include "youtube/YtCookies.h"
#include "youtube/YtMusicApi.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace {

constexpr int kRoomBatch = 100;   // tracks per room message (the server's limit)
constexpr int kRoomQueue = 500;   // tracks a room's queue holds (the server's limit)

QString cacheFile() { return AppPaths::dataDir() + QStringLiteral("/shared-playlists.json"); }

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

constexpr int kLikeBatch = 200;      // likes per request
constexpr int kMirrorBatch = 400;    // tracks per request when copying a playlist

// A track as the server keeps it (tracks.py).
Track serverTrack(const QJsonObject &o)
{
    Track t;
    t.id = o.value("id").toString();
    t.path = o.value("path").toString();
    t.source = o.value("source").toString();
    t.title = o.value("title").toString();
    t.artist = o.value("artist").toString();
    t.artistId = o.value("artistId").toString();
    t.album = o.value("album").toString();
    t.duration = o.value("duration").toInt();
    t.cover = o.value("cover").toString();
    t.quality = t.source == QLatin1String("soundcloud") ? QStringLiteral("SoundCloud") : QStringLiteral("YouTube Music");
    const QString key = TrackModel::albumKey(t);
    const auto [a, b] = Scanner::fallbackPalette(key);
    t.colorA = a;
    t.colorB = b;
    t.seed = Scanner::stableSeed(key);
    return t;
}

// EVERPLAY_ONLINE_DEBUG=1: room states, what the player is told and what it reports.
bool debug()
{
    static const bool on = qEnvironmentVariableIsSet("EVERPLAY_ONLINE_DEBUG");
    return on;
}

QString defaultName()
{
    QString name = qEnvironmentVariable("USERNAME");
    if (name.isEmpty())
        name = qEnvironmentVariable("USER");
    return name.isEmpty() ? QStringLiteral("Listener") : name.left(40);
}

// "everplay://r/ABC123" / "https://host/r/ABC123" -> { "r", "ABC123" }
// (nokturn:// is the scheme of the app's former name: the server's link pages and old links use it.)
std::optional<std::pair<QString, QString>> parseLink(const QString &text, const QString &server)
{
    const QString trimmed = text.trimmed();
    const QUrl url(trimmed);
    static const QRegularExpression code(QStringLiteral("^[A-Za-z0-9]{4,20}$"));
    QString kind, value;
    if (url.scheme() == QLatin1String("everplay") || url.scheme() == QLatin1String("nokturn")) {
        kind = url.host();
        value = url.path().mid(1).section(QLatin1Char('/'), 0, 0);
    } else if (url.scheme().startsWith(QLatin1String("http"))) {
        // The default server answers under both its addresses: links shared before the rename use the former.
        const QString host = QUrl(server).host();
        const QStringList same { QUrl(QString::fromLatin1(OnlineService::kDefaultServer)).host(),
                                 QUrl(QString::fromLatin1(OnlineService::kFormerServer)).host() };
        const bool known = url.host().compare(host, Qt::CaseInsensitive) == 0
                           || (same.contains(host, Qt::CaseInsensitive) && same.contains(url.host(), Qt::CaseInsensitive));
        if (!known)
            return std::nullopt;
        const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            return std::nullopt;
        kind = parts.at(0);
        value = parts.at(1);
    } else if (code.match(trimmed).hasMatch() && trimmed.size() == 6) {
        kind = QStringLiteral("r");   // a room code typed by hand
        value = trimmed;
    } else {
        return std::nullopt;
    }
    if ((kind != QLatin1String("t") && kind != QLatin1String("r") && kind != QLatin1String("p"))
        || !code.match(value).hasMatch())
        return std::nullopt;
    return std::make_pair(kind, value);
}

} // namespace

OnlineService *OnlineService::instance()
{
    static OnlineService *inst = new OnlineService;
    return inst;
}

OnlineService *OnlineService::create(QQmlEngine *, QJSEngine *)
{
    OnlineService *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

OnlineService::OnlineService(QObject *parent)
    : QObject(parent)
{
    m_monotonic.start();
    auto settings = AppPaths::settings();
    m_server = qEnvironmentVariable("EVERPLAY_SERVER");
    if (m_server.isEmpty())
        m_server = settings->value("online/server", QString::fromLatin1(kDefaultServer)).toString();
    m_server = m_server.trimmed();
    while (m_server.endsWith(QLatin1Char('/')))
        m_server.chop(1);
    if (m_server == QLatin1String(kFormerServer))
        m_server = QString::fromLatin1(kDefaultServer);   // the same server, set by hand before the rename
    m_name = settings->value("online/name", defaultName()).toString();
    m_librarySync = settings->value("online/librarySync", true).toBool();
    loadAccount();
    loadLibrarySync();
    loadPlaylists();

    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &OnlineService::connectSocket);
    m_pingTimer.setInterval(15'000);
    connect(&m_pingTimer, &QTimer::timeout, this, &OnlineService::ping);
    // While in a room the player is checked against the room's clock every few seconds.
    m_syncTimer.setInterval(2'500);
    connect(&m_syncTimer, &QTimer::timeout, this, &OnlineService::syncPlayer);
    // The library also comes with every change over the connection; this catches a missed one.
    m_libTimer.setInterval(10 * 60'000);
    connect(&m_libTimer, &QTimer::timeout, this, &OnlineService::syncLibrary);
    // The YouTube Music playlists are copied a moment after they change (they load one by one).
    m_mirrorTimer.setSingleShot(true);
    m_mirrorTimer.setInterval(4'000);
    connect(&m_mirrorTimer, &QTimer::timeout, this, &OnlineService::syncMirrors);
    connect(YouTubeService::instance(), &YouTubeService::dataChanged, this, &OnlineService::scheduleMirrors);
    connect(SpotifyService::instance(), &SpotifyService::dataChanged, this, &OnlineService::scheduleMirrors);
    connect(SoundCloudService::instance(), &SoundCloudService::dataChanged, this, &OnlineService::scheduleMirrors);
    // Spotify tracks are matched one by one, gently: each is one or two YouTube Music searches.
    m_matchTimer.setSingleShot(true);
    m_matchTimer.setInterval(1'500);
    connect(&m_matchTimer, &QTimer::timeout, this, &OnlineService::matchNext);
    m_orgTimer.setSingleShot(true);
    m_orgTimer.setInterval(1'500);
    connect(&m_orgTimer, &QTimer::timeout, this, &OnlineService::pushOrganizer);

    // The profile (Profile) is what the others see of you in rooms and shared playlists: its name is the
    // account's name, its picture goes to the server. A name set here before the profile existed carries over.
    Profile *profile = Profile::instance();
    if (profile->name().isEmpty() && settings->contains("online/name"))
        profile->setName(m_name);
    connect(profile, &Profile::changed, this, &OnlineService::syncProfile);
    connect(this, &OnlineService::accountChanged, this, &OnlineService::syncProfile);
    QTimer::singleShot(0, this, &OnlineService::syncProfile);

    connect(&m_ws, &QWebSocket::connected, this, [this] {
        m_connected = true;
        m_backoff = 2;
        m_bestRtt = -1;
        emit connectionChanged();
        ping();
        m_pingTimer.start();
        if (!m_roomCode.isEmpty())
            send({ { "type", "room.join" }, { "code", m_roomCode } });   // back after a dropped connection
        const QList<QJsonObject> waiting = std::exchange(m_outbox, {});
        for (const QJsonObject &m : waiting)
            send(m);
        // Changes made while we were away.
        if (!m_playlists.isEmpty())
            refreshPlaylists();
        if (m_librarySync)
            syncLibrary();
    });
    connect(&m_ws, &QWebSocket::disconnected, this, [this] {
        const bool was = m_connected;
        m_connected = false;
        m_pingTimer.stop();
        if (was)
            emit connectionChanged();
        if (m_ws.closeCode() == 4001) {   // the server signed this device out
            refreshAccount();
            return;
        }
        if (wantSocket()) {
            m_reconnect.start(m_backoff * 1000);
            m_backoff = std::min(60, m_backoff * 2);
        }
    });
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &OnlineService::onSocketMessage);

    // Shared playlists and the library may have changed while Everplay was closed.
    if (registered() && (!m_playlists.isEmpty() || m_librarySync))
        QTimer::singleShot(4'000, this, &OnlineService::connectSocket);
    if (registered() && m_librarySync)
        m_libTimer.start();
    QTimer::singleShot(0, this, &OnlineService::registerUrlScheme);
}

void OnlineService::attach(PlayerController *player)
{
    m_player = player;
    connect(ListeningHistory::instance(), &ListeningHistory::playNoted, this, &OnlineService::notePlayed);
    // (here, not in the constructor: the library makes this service while it's being made itself)
    connect(Library::instance(), &Library::organizerChanged, this, [this] {
        if (m_librarySync && registered())
            m_orgTimer.start();
    });
}

// ---- settings ---------------------------------------------------------------------

void OnlineService::setServerUrl(const QString &url)
{
    QString clean = url.trimmed();
    while (clean.endsWith(QLatin1Char('/')))
        clean.chop(1);
    if (clean.isEmpty() || clean == QLatin1String(kFormerServer))
        clean = QString::fromLatin1(kDefaultServer);
    if (clean == m_server)
        return;
    if (inRoom() || joiningRoom())
        leaveRoom();
    m_ws.close();
    m_server = clean;
    AppPaths::settings()->setValue("online/server", clean == QLatin1String(kDefaultServer) ? QVariant() : QVariant(clean));
    // Accounts, their libraries and playlists belong to a server.
    loadAccount();
    loadLibrarySync();
    m_playlists.clear();
    savePlaylists();
    emit playlistsChanged();
    emit settingsChanged();
    emit accountChanged();
}

void OnlineService::setDisplayName(const QString &name)
{
    const QString clean = name.simplified().left(40);
    if (clean.isEmpty() || clean == m_name)
        return;
    m_name = clean;
    AppPaths::settings()->setValue("online/name", clean);
    Profile::instance()->setName(clean);   // the same name in the profile
    emit settingsChanged();
    if (registered()) {
        request("PATCH", QStringLiteral("/me"), { { "name", clean } }, [this](const QJsonObject &, const QString &error) {
            if (!error.isEmpty())
                emit notice(tr("Couldn't change the name: %1").arg(error), QStringLiteral("close"));
        });
    }
}

void OnlineService::syncProfile()
{
    Profile *profile = Profile::instance();
    if (!profile->name().isEmpty())
        setDisplayName(profile->name());
    if (!registered() || m_avatarBusy)
        return;
    // What went up last for this account: its id and the picture's file name (a new picture is a new file).
    const QString file = profile->picture().isEmpty() ? QString() : profile->picture().fileName();
    const QString key = m_userId + QLatin1Char('|') + file;
    const QString sent = AppPaths::settings()->value("online/avatarSent").toString();
    if (sent == key)
        return;
    // No picture here and none sent from here: the account may have one from another device, which stays.
    if (file.isEmpty() && !sent.startsWith(m_userId + QLatin1Char('|')))
        return;
    const auto finished = [this, key](const QJsonObject &, const QString &error) {
        m_avatarBusy = false;
        if (!error.isEmpty()) {
            qWarning().noquote() << "online: profile picture:" << error;
            return;
        }
        AppPaths::settings()->setValue("online/avatarSent", key);
        syncProfile();   // the picture may have changed again meanwhile
    };
    m_avatarBusy = true;
    if (file.isEmpty()) {
        request("DELETE", QStringLiteral("/me/avatar"), {}, finished);
        return;
    }
    // Small enough for the rooms' lists: 256 px, as a JPEG.
    QImage image(profile->picture().toLocalFile());
    if (image.isNull()) {
        m_avatarBusy = false;
        return;
    }
    image = image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 88);
    send("PUT", QStringLiteral("/me/avatar"), jpeg, "image/jpeg", finished);
}

// ---- account and requests ---------------------------------------------------------

QString OnlineService::tokenFile() const
{
    // One account per server.
    const QByteArray key = QCryptographicHash::hash(storedServer().toUtf8(), QCryptographicHash::Sha1).toHex().left(10);
    return AppPaths::dataDir() + QStringLiteral("/online-") + QString::fromLatin1(key) + QStringLiteral(".token");
}

QString OnlineService::storedServer() const
{
    return m_server == QLatin1String(kDefaultServer) ? QString::fromLatin1(kFormerServer) : m_server;
}

void OnlineService::loadAccount()
{
    m_token.clear();
    m_userId.clear();
    QFile f(tokenFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(unprotectSecret(f.readAll())).object();
    m_token = o.value("token").toString();
    m_userId = o.value("id").toString();
    m_login = o.value("login").toString();
}

void OnlineService::ensureAccount(std::function<void(bool ok)> then)
{
    if (registered()) {
        then(true);
        return;
    }
    m_waitingForAccount << std::move(then);
    if (m_registering)
        return;
    m_registering = true;
    request("POST", QStringLiteral("/register"), { { "name", m_name } }, [this](const QJsonObject &reply, const QString &error) {
        m_registering = false;
        const QString token = reply.value("token").toString();
        const bool ok = error.isEmpty() && !token.isEmpty();
        if (ok) {
            m_token = token;
            m_userId = reply.value("user").toObject().value("id").toString();
            saveToken();
            loadLibrarySync();
            emit accountChanged();
            if (m_librarySync) {
                m_libTimer.start();
                QTimer::singleShot(0, this, &OnlineService::syncLibrary);
            }
        } else {
            emit notice(tr("Can't reach the Everplay server: %1").arg(error), QStringLiteral("close"));
        }
        const auto waiting = std::exchange(m_waitingForAccount, {});
        for (const auto &cb : waiting)
            cb(ok);
    });
}

void OnlineService::saveToken() const
{
    QSaveFile f(tokenFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(protectSecret(QJsonDocument(QJsonObject { { "token", m_token }, { "id", m_userId }, { "login", m_login } })
                                  .toJson(QJsonDocument::Compact)));
        f.commit();
    }
}

void OnlineService::forgetAccount()
{
    if (!registered())
        return;
    request("DELETE", QStringLiteral("/me"), {}, [this](const QJsonObject &, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("Couldn't delete the account: %1").arg(error), QStringLiteral("close"));
            return;
        }
        dropAccount();
        emit notice(tr("Online account deleted"), QStringLiteral("check"));
    });
}

void OnlineService::dropAccount()
{
    // This device has no account any more: what came from it (shared playlists, the library's state) goes too.
    if (inRoom() || joiningRoom())
        leaveRoom();
    m_ws.close();
    QFile::remove(tokenFile());
    QFile::remove(syncFile());
    m_token.clear();
    m_userId.clear();
    m_login.clear();
    loadLibrarySync();
    m_libTimer.stop();
    m_playlists.clear();
    savePlaylists();
    m_devices.clear();
    m_pairCode.clear();
    emit pairChanged();
    emit devicesChanged();
    emit playlistsChanged();
    emit accountChanged();
}

void OnlineService::switchAccount(const QString &token, const QJsonObject &user)
{
    // This device signs in to that account: its rooms and playlists are that account's from now on (what this
    // device's own account had, the server moved into it).
    const QString id = user.value("id").toString();
    const bool other = id != m_userId;
    if (other && (inRoom() || joiningRoom()))
        leaveRoom();
    m_ws.close();
    m_token = token;
    m_userId = id;
    m_login = user.value("login").toString();
    saveToken();
    if (other) {
        // The likes here join the account's (nothing is removed from either).
        loadLibrarySync();
        resetLibrarySync();
        m_playlists.clear();
        savePlaylists();
        emit playlistsChanged();
    }
    m_pairCode.clear();
    emit pairChanged();
    emit accountChanged();
    refreshPlaylists();
    if (m_librarySync) {
        m_libTimer.start();
        syncLibrary();
    }
    connectSocket();
    refreshAccount();
}

void OnlineService::request(const QByteArray &method, const QString &path, const QJsonObject &body, JsonCallback done)
{
    const QByteArray data = (method == "GET" || method == "DELETE") && body.isEmpty()
                                ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    send(method, path, data, "application/json", std::move(done));
}

void OnlineService::send(const QByteArray &method, const QString &path, const QByteArray &data, const QByteArray &type,
                         JsonCallback done)
{
    QNetworkRequest req(QUrl(m_server + QStringLiteral("/api/v1") + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(type));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Everplay/" EVERPLAY_VERSION));
    req.setTransferTimeout(15'000);
    if (!m_token.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    req.setRawHeader("X-Nokturn-Device", deviceName().toUtf8());   // its name in the account's list of devices
    QNetworkReply *reply = m_net.sendCustomRequest(req, method, data);
    beginBusy();
    connect(reply, &QNetworkReply::finished, this, [this, reply, done = std::move(done)] {
        reply->deleteLater();
        endBusy();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        if (status == 401 && !m_token.isEmpty() && o.value("error").toString() == QLatin1String("unauthorized")) {
            // Signed out from another device (or the server forgot us): the next action makes a new account.
            const bool hadLogin = !m_login.isEmpty();
            dropAccount();
            if (hadLogin)
                emit notice(tr("This device was signed out of your Everplay account"), QStringLiteral("close"));
        }
        QString error;
        if (reply->error() != QNetworkReply::NoError || status >= 400) {
            error = o.value("message").toString();
            if (error.isEmpty())
                error = reply->errorString();
        }
        done(o, error);
    });
}

void OnlineService::beginBusy()
{
    if (++m_busy == 1)
        emit busyChanged();
}

void OnlineService::endBusy()
{
    if (--m_busy == 0)
        emit busyChanged();
}

// ---- WebSocket ------------------------------------------------------------------

bool OnlineService::wantSocket() const
{
    return registered() && (!m_roomCode.isEmpty() || !m_outbox.isEmpty() || !m_playlists.isEmpty() || m_librarySync);
}

void OnlineService::connectSocket()
{
    if (!registered() || m_ws.state() != QAbstractSocket::UnconnectedState)
        return;
    QUrl url(m_server + QStringLiteral("/api/v1/ws"));
    url.setScheme(url.scheme() == QLatin1String("http") ? QStringLiteral("ws") : QStringLiteral("wss"));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Everplay/" EVERPLAY_VERSION));
    m_ws.open(req);
}

void OnlineService::send(const QJsonObject &message)
{
    if (!m_connected) {
        m_outbox << message;
        ensureAccount([this](bool ok) {
            if (ok)
                connectSocket();
            else
                m_outbox.clear();
        });
        return;
    }
    m_ws.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void OnlineService::ping()
{
    if (m_connected)
        send({ { "type", "ping" }, { "t", double(m_monotonic.elapsed()) } });
}

void OnlineService::onSocketMessage(const QString &text)
{
    const QJsonObject m = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = m.value("type").toString();
    if (type == QLatin1String("hello")) {
        const QJsonObject user = m.value("user").toObject();
        m_userId = user.value("id").toString();
        if (user.value("login").toString() != m_login) {   // given on another device
            m_login = user.value("login").toString();
            saveToken();
            emit accountChanged();
        }
    } else if (type == QLatin1String("pong")) {
        // The clock offset from the round trip with the smallest delay (the most exact one).
        const qint64 rtt = m_monotonic.elapsed() - qint64(m.value("t").toDouble());
        if (rtt >= 0 && (m_bestRtt < 0 || rtt <= m_bestRtt * 1.5)) {
            m_bestRtt = m_bestRtt < 0 ? rtt : std::min(m_bestRtt, rtt);
            m_clockOffset = qint64(m.value("serverTime").toDouble()) + rtt / 2 - nowMs();
        }
    } else if (type == QLatin1String("room")) {
        applyRoom(m.value("room").toObject(), qint64(m.value("serverTime").toDouble()));
    } else if (type == QLatin1String("room.left")) {
        if (m.value("code").toString() == m_roomCode)
            clearRoom(m.value("reason") == QLatin1String("kicked") ? tr("The host removed you from the room") : QString());
    } else if (type == QLatin1String("playlist")) {
        const QString id = m.value("id").toString();
        const SharedPlaylist *p = findPlaylist(id);
        if (!p || p->version != m.value("version").toInt())
            fetchPlaylist(id);
    } else if (type == QLatin1String("library")) {
        if (m.value("version").toInt() != m_libVersion)
            syncLibrary();   // another device liked or played something
    } else if (type == QLatin1String("playlist.removed")) {
        const QString id = m.value("id").toString();
        for (int i = 0; i < m_playlists.size(); ++i) {
            if (m_playlists.at(i).id != id)
                continue;
            const QString name = m_playlists.at(i).name;
            m_playlists.removeAt(i);
            savePlaylists();
            emit playlistsChanged();
            emit notice(tr("%1 is no longer shared with you").arg(name), QStringLiteral("close"));
            break;
        }
    } else if (type == QLatin1String("error")) {
        const QString request = m.value("request").toString();
        emit notice(roomErrorText(m.value("error").toString(), m.value("message").toString()), QStringLiteral("close"));
        // A room that's gone (also after a dropped connection, when the server restarted meanwhile).
        if (request == QLatin1String("room.join") || (request == QLatin1String("room.create") && m_roomState.isEmpty()))
            clearRoom();
    }
}

QString OnlineService::roomErrorText(const QString &code, const QString &fallback) const
{
    if (code == QLatin1String("host_only"))
        return tr("Only the host controls this room");
    if (code == QLatin1String("no_room"))
        return tr("This room doesn't exist anymore");
    if (code == QLatin1String("bad_track"))
        return tr("Only YouTube Music and SoundCloud tracks can be shared");
    if (code == QLatin1String("queue_full"))
        return tr("The room's queue is full");
    if (code == QLatin1String("rate_limited"))
        return tr("Too many requests — try again in a moment");
    return fallback;
}

// ---- tracks ---------------------------------------------------------------------

QJsonObject OnlineService::trackJson(const Track &t)
{
    return { { "id", t.id }, { "source", t.source }, { "path", t.path }, { "title", t.title },
             { "artist", t.artist }, { "artistId", t.artistId }, { "album", t.album },
             { "duration", t.duration }, { "cover", t.cover.startsWith(QLatin1String("http")) ? t.cover : QString() } };
}

QJsonArray OnlineService::tracksJson(const QList<Track> &tracks) const
{
    QJsonArray out;
    for (const Track &t : tracks)
        out.append(trackJson(t));
    return out;
}

Track OnlineService::storeTrack(const QJsonObject &o)
{
    const QString id = o.value("id").toString();
    if (id.isEmpty())
        return {};
    if (const auto it = m_tracks.constFind(id); it != m_tracks.cend())
        return *it;
    Track t = serverTrack(o);
    t.transient = true;
    m_tracks.insert(id, t);
    // The track model may already have it (liked, in the library) — only new ones are appended.
    if (Library::instance()->tracks()->rowOf(id) < 0)
        emit tracksAdded({ t });
    return t;
}

void OnlineService::portableTracks(const QVariantList &rows, TracksCallback done)
{
    // Only YouTube Music and SoundCloud tracks can be played by everyone: Spotify tracks go as
    // their YouTube Music match, local files as the YouTube Music track that fits their tags.
    struct State
    {
        std::vector<std::optional<Track>> entries;
        int pending = 0;
        int skipped = 0;
        TracksCallback done;
        void finish()
        {
            QList<Track> out;
            for (const auto &e : entries)
                if (e)
                    out << *e;
            done(out, skipped);
        }
    };
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    const TrackModel *model = Library::instance()->tracks();
    for (const QVariant &v : rows) {
        if (!model->isValidRow(v.toInt()))
            continue;
        const Track t = model->at(v.toInt());
        if (t.demo) {
            ++state->skipped;
            continue;
        }
        const size_t slot = state->entries.size();
        state->entries.emplace_back();
        auto asYouTube = [t](const QString &videoId, const Track *found) {
            Track yt = found ? *found : t;
            yt.id = QStringLiteral("yt:") + videoId;
            yt.path = videoId;
            yt.source = QStringLiteral("youtube");
            if (!found && !t.cover.startsWith(QLatin1String("http")))
                yt.cover.clear();
            return yt;
        };
        if (t.source == QLatin1String("youtube") || t.source == QLatin1String("soundcloud")) {
            state->entries[slot] = t;
        } else if (t.source == QLatin1String("spotify")) {
            ++state->pending;
            SpotifyService::instance()->match(t.id, [state, slot, asYouTube](const QString &videoId) {
                if (videoId.isEmpty())
                    ++state->skipped;
                else
                    state->entries[slot] = asYouTube(videoId, nullptr);
                if (--state->pending == 0)
                    state->finish();
            });
        } else if (t.source == QLatin1String("local") && !t.title.isEmpty()) {
            ++state->pending;
            YouTubeService::instance()->api()->search(t.artist + QLatin1Char(' ') + t.title,
                                                      [state, slot, t, asYouTube](const QList<Track> &results, const QString &error) {
                const int match = error.isEmpty() ? YtMusicApi::bestMatch(t, results) : -1;
                if (match < 0)
                    ++state->skipped;
                else
                    state->entries[slot] = asYouTube(results.at(match).path, &results.at(match));
                if (--state->pending == 0)
                    state->finish();
            });
        } else {
            ++state->skipped;
        }
    }
    if (state->pending == 0)
        state->finish();
}

// ---- links ----------------------------------------------------------------------

bool OnlineService::isLink(const QString &text)
{
    return parseLink(text, instance()->m_server).has_value();
}

bool OnlineService::openLink(const QString &link)
{
    const auto parsed = parseLink(link, m_server);
    if (!parsed)
        return false;
    const auto &[kind, value] = *parsed;
    emit activateRequested();
    if (kind == QLatin1String("t"))
        openShare(value);
    else if (kind == QLatin1String("r"))
        joinRoom(value);
    else
        joinPlaylist(value);
    return true;
}

void OnlineService::copyToClipboard(const QString &text, const QString &message)
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
    emit notice(message, QStringLiteral("link"));
}

void OnlineService::shareTrack(int row)
{
    portableTracks({ row }, [this](const QList<Track> &tracks, int) {
        if (tracks.isEmpty()) {
            emit notice(tr("No YouTube Music version of this track was found to share"), QStringLiteral("close"));
            return;
        }
        const Track t = tracks.first();
        ensureAccount([this, t](bool ok) {
            if (!ok)
                return;
            request("POST", QStringLiteral("/shares"), { { "track", trackJson(t) } },
                    [this, t](const QJsonObject &reply, const QString &error) {
                if (!error.isEmpty()) {
                    emit notice(tr("Couldn't make the link: %1").arg(error), QStringLiteral("close"));
                    return;
                }
                copyToClipboard(reply.value("url").toString(), tr("Link to %1 copied").arg(t.title));
            });
        });
    });
}

void OnlineService::shareCurrent()
{
    if (m_player && m_player->hasTrack())
        shareTrack(m_player->currentIndex());
}

void OnlineService::openShare(const QString &code)
{
    request("GET", QStringLiteral("/shares/") + code, {}, [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("Couldn't open the link: %1").arg(error), QStringLiteral("close"));
            return;
        }
        const Track t = storeTrack(reply.value("track").toObject());
        if (t.id.isEmpty())
            return;
        const int row = Library::instance()->tracks()->rowOf(t.id);
        if (row < 0 || !m_player)
            return;
        const QString by = reply.value("by").toString();
        if (inRoom()) {
            addToRoom({ row });
            return;
        }
        m_player->playFrom({ row }, row, QStringLiteral("share"));
        emit notice(by.isEmpty() ? tr("Playing %1").arg(t.title) : tr("%1 shared %2").arg(by, t.title),
                    QStringLiteral("link"));
    });
}

// ---- listening rooms --------------------------------------------------------------

void OnlineService::createRoom(const QString &name, const QString &mode)
{
    if (inRoom() || joiningRoom())
        leaveRoom();
    m_creatingRoom = true;
    emit roomChanged();
    ensureAccount([this, name, mode](bool ok) {
        if (!ok) {
            m_creatingRoom = false;
            emit roomChanged();
            return;
        }
        send({ { "type", "room.create" }, { "name", name.trimmed() },
               { "mode", mode == QLatin1String("host") ? "host" : "open" } });
    });
}

void OnlineService::joinRoom(const QString &codeOrLink)
{
    QString code = codeOrLink.trimmed();
    if (const auto parsed = parseLink(code, m_server); parsed && parsed->first == QLatin1String("r"))
        code = parsed->second;
    code = code.toUpper();
    static const QRegularExpression valid(QStringLiteral("^[A-Z0-9]{6}$"));
    if (!valid.match(code).hasMatch()) {
        emit notice(tr("That isn't a room link or code"), QStringLiteral("close"));
        return;
    }
    if (code == m_roomCode && inRoom()) {
        emit roomOpened();
        return;
    }
    if (inRoom() || joiningRoom())
        leaveRoom();
    m_roomCode = code;
    emit roomChanged();
    emit roomOpened();
    ensureAccount([this, code](bool ok) {
        if (!ok) {
            clearRoom();
            return;
        }
        if (m_connected)
            send({ { "type", "room.join" }, { "code", code } });
        else
            connectSocket();   // joins on connect
    });
}

void OnlineService::leaveRoom()
{
    if (m_roomCode.isEmpty() && !m_creatingRoom)
        return;
    if (m_connected)
        send({ { "type", "room.leave" } });
    clearRoom();
}

void OnlineService::clearRoom(const QString &message)
{
    const bool was = !m_roomCode.isEmpty() || m_creatingRoom;
    m_creatingRoom = false;
    m_roomCode.clear();
    // Room messages still waiting for the connection don't belong anywhere now.
    m_outbox.erase(std::remove_if(m_outbox.begin(), m_outbox.end(),
                                  [](const QJsonObject &m) { return m.value("type").toString().startsWith(QLatin1String("room.")); }),
                   m_outbox.end());
    m_roomState = {};
    m_roomItems.clear();
    m_roomCurrent = -1;
    m_roomView.clear();
    m_syncTimer.stop();
    if (m_player)
        m_player->setRoomLink({});
    if (!message.isEmpty())
        emit notice(message, QStringLiteral("close"));
    if (was)
        emit roomChanged();
    if (!wantSocket())
        m_ws.close();
}

void OnlineService::roomSend(const QString &type, QJsonObject message)
{
    if (!inRoom())
        return;
    message.insert("type", type);
    send(message);
}

void OnlineService::addToRoom(const QVariantList &rows, bool next)
{
    if (!inRoom())
        return;
    if (!canControl()) {
        emit notice(tr("Only the host controls this room"), QStringLiteral("close"));
        return;
    }
    portableTracks(rows.mid(0, kRoomQueue), [this, next](const QList<Track> &tracks, int skipped) {
        if (tracks.isEmpty()) {
            emit notice(tr("No YouTube Music version of this track was found to share"), QStringLiteral("close"));
            return;
        }
        // In messages of kRoomBatch. "next" puts each one right after the playing track, so the last
        // batch goes first to keep the order.
        QList<QList<Track>> batches;
        for (qsizetype i = 0; i < tracks.size(); i += kRoomBatch)
            batches << tracks.mid(i, kRoomBatch);
        if (next)
            std::reverse(batches.begin(), batches.end());
        for (const auto &batch : batches)
            roomSend(QStringLiteral("room.add"), { { "tracks", tracksJson(batch) }, { "next", next } });
        const QString text = tracks.size() == 1 ? tr("Added %1 to the room").arg(tracks.first().title)
                                                : tr("Added %n tracks to the room", "", int(tracks.size()));
        emit notice(skipped > 0 ? text + QStringLiteral(" — ") + tr("%n without a YouTube Music version skipped", "", skipped) : text,
                    QStringLiteral("queue"));
    });
}

void OnlineService::shuffleRoom() { roomSend(QStringLiteral("room.shuffle")); }
void OnlineService::clearRoomQueue() { roomSend(QStringLiteral("room.clear")); }
void OnlineService::removeFromRoom(const QString &itemId) { roomSend(QStringLiteral("room.remove"), { { "itemId", itemId } }); }
void OnlineService::playInRoom(const QString &itemId) { roomSend(QStringLiteral("room.play"), { { "itemId", itemId } }); }
void OnlineService::setRoomMode(const QString &mode) { roomSend(QStringLiteral("room.mode"), { { "mode", mode } }); }
void OnlineService::makeHost(const QString &userId) { roomSend(QStringLiteral("room.host"), { { "userId", userId } }); }
void OnlineService::kick(const QString &userId) { roomSend(QStringLiteral("room.kick"), { { "userId", userId } }); }

void OnlineService::renameRoom(const QString &name)
{
    if (!name.trimmed().isEmpty())
        roomSend(QStringLiteral("room.rename"), { { "name", name.trimmed() } });
}

void OnlineService::copyRoomLink()
{
    if (inRoom())
        copyToClipboard(m_roomView.value("url").toString(), tr("Invite link copied — anyone with it can join"));
}

bool OnlineService::canControl() const
{
    return m_roomState.value("mode").toString() == QLatin1String("open")
           || m_roomState.value("hostId").toString() == m_userId;
}

double OnlineService::roomPosition() const
{
    const double position = m_roomState.value("position").toDouble();
    if (!m_roomState.value("playing").toBool())
        return position;
    const qint64 serverNow = nowMs() + m_clockOffset;
    return position + std::max<qint64>(0, serverNow - qint64(m_roomState.value("since").toDouble())) / 1000.0;
}

void OnlineService::applyRoom(const QJsonObject &state, qint64 serverTime)
{
    const QString code = state.value("code").toString();
    if (code.isEmpty())
        return;
    // Only the room we're joining or in — not a late message from one we've left.
    if (code != m_roomCode && !(m_creatingRoom && m_roomCode.isEmpty()))
        return;
    m_creatingRoom = false;
    const bool entered = m_roomState.isEmpty();
    // Before the first pong the server's clock in the message is the best guess.
    if (m_bestRtt < 0 && serverTime > 0)
        m_clockOffset = serverTime - nowMs();
    m_roomCode = code;
    m_roomState = state;
    m_roomItems.clear();
    for (const QJsonValue &v : state.value("queue").toArray()) {
        const QJsonObject o = v.toObject();
        const Track t = storeTrack(o.value("track").toObject());
        m_roomItems << RoomItem { o.value("itemId").toString(), t.id, o.value("addedByName").toString() };
    }
    m_roomCurrent = state.value("current").toInt(-1);
    if (debug())
        qInfo("online: room %s rev %d: %lld tracks, current %d, %s at %.1f s, %lld members, host %s, clock offset %lld ms",
              qPrintable(code), state.value("revision").toInt(), qint64(m_roomItems.size()), m_roomCurrent,
              state.value("playing").toBool() ? "playing" : "paused", roomPosition(),
              qint64(state.value("members").toArray().size()),
              state.value("hostId").toString() == m_userId ? "me" : "someone else", m_clockOffset);
    if (entered && m_player) {
        m_player->setRoomLink({
            [this](const QString &action, const QVariant &arg) { return interceptPlayer(action, arg); },
            [this](const QString &trackId) { playerMovedOn(trackId); },
        });
        m_syncTimer.start();
        if (!YouTubeService::instance()->engineReady())
            emit notice(tr("Download the playback engine (yt-dlp) in Sources → YouTube Music to hear the room"),
                        QStringLiteral("playCircle"));
    }
    buildRoomView();
    syncPlayer();
    emit roomChanged();
    if (entered)
        emit roomOpened();
}

void OnlineService::buildRoomView()
{
    const QString hostId = m_roomState.value("hostId").toString();
    QVariantList members;
    for (const QJsonValue &v : m_roomState.value("members").toArray()) {
        const QJsonObject m = v.toObject();
        const QString id = m.value("id").toString();
        // The picture's address carries its version; your own comes from this computer, so it shows at once.
        const qint64 avatar = m.value("avatar").toVariant().toLongLong();
        const QUrl own = Profile::instance()->picture();
        const QString picture = id == m_userId && !own.isEmpty() ? own.toString()
            : avatar > 0 ? m_server + QStringLiteral("/api/v1/users/") + id + QStringLiteral("/avatar?v=") + QString::number(avatar)
                         : QString();
        members << QVariantMap { { "id", id }, { "name", m.value("name").toString() },
                                 { "online", m.value("online").toBool() }, { "host", id == hostId },
                                 { "me", id == m_userId }, { "avatar", picture } };
    }
    const bool finished = m_roomState.value("finished").toBool();
    const TrackModel *model = Library::instance()->tracks();
    QVariantList queue;
    for (int i = 0; i < m_roomItems.size(); ++i) {
        const RoomItem &item = m_roomItems.at(i);
        queue << QVariantMap { { "itemId", item.itemId }, { "row", model->rowOf(item.trackId) },
                               { "addedBy", item.addedBy }, { "current", i == m_roomCurrent && !finished },
                               { "played", i < m_roomCurrent || (i == m_roomCurrent && finished) } };
    }
    m_roomView = {
        { "code", m_roomCode },
        { "name", m_roomState.value("name").toString() },
        { "mode", m_roomState.value("mode").toString() },
        { "url", m_server + QStringLiteral("/r/") + m_roomCode },
        { "isHost", hostId == m_userId },
        { "canControl", canControl() },
        { "playing", m_roomState.value("playing").toBool() },
        { "finished", finished },
        { "members", members },
        { "queue", queue },
        { "currentRow", m_roomCurrent >= 0 && m_roomCurrent < m_roomItems.size() && !finished
                            ? model->rowOf(m_roomItems.at(m_roomCurrent).trackId) : -1 },
    };
}

void OnlineService::syncPlayer()
{
    if (!m_player || !inRoom())
        return;
    const bool finished = m_roomState.value("finished").toBool();
    if (m_roomCurrent < 0 || m_roomCurrent >= m_roomItems.size() || finished) {
        m_player->applyRoom({}, 0, false);
        return;
    }
    // The local queue: the room's from the current track on (each track once).
    QStringList ids;
    for (int i = m_roomCurrent; i < m_roomItems.size(); ++i)
        if (!ids.contains(m_roomItems.at(i).trackId))
            ids << m_roomItems.at(i).trackId;
    const double position = roomPosition();
    const bool playing = m_roomState.value("playing").toBool();
    // Our player has already moved on to the next track (a transition, or the end came here first)
    // and the room is about to follow: don't pull it back.
    if (playing && m_roomCurrent + 1 < m_roomItems.size()
        && m_player->currentTrackId() == m_roomItems.at(m_roomCurrent + 1).trackId) {
        const int duration = m_tracks.value(ids.first()).duration;
        if (duration <= 0 || position > duration - 30)
            return;
    }
    if (debug())
        qInfo("online: player <- %s at %.1f s (%s), local %s at %.1f s", qPrintable(ids.first()), position,
              playing ? "playing" : "paused", qPrintable(m_player->currentTrackId()), m_player->position());
    m_player->applyRoom(ids, position, playing);
}

bool OnlineService::interceptPlayer(const QString &action, const QVariant &arg)
{
    if (debug())
        qInfo("online: player action %s %s -> room", qPrintable(action), qPrintable(arg.toString()));
    if (!canControl()) {
        emit notice(tr("Only the host controls this room"), QStringLiteral("close"));
        return true;
    }
    const TrackModel *model = Library::instance()->tracks();
    if (action == QLatin1String("play")) {
        const int row = arg.toInt();
        if (!model->isValidRow(row))
            return true;
        const QString id = model->at(row).id;
        // Already in the room's queue: jump to it (the upcoming copy first), otherwise add it.
        for (int i = std::max(0, m_roomCurrent); i < m_roomItems.size(); ++i) {
            if (m_roomItems.at(i).trackId == id) {
                if (i == m_roomCurrent && !m_roomState.value("finished").toBool()) {
                    if (!m_roomState.value("playing").toBool())
                        roomSend(QStringLiteral("room.resume"));
                } else
                    playInRoom(m_roomItems.at(i).itemId);
                return true;
            }
        }
        addToRoom({ row });
    } else if (action == QLatin1String("queue")) {
        QVariantList rows = arg.toList();
        std::shuffle(rows.begin(), rows.end(), *QRandomGenerator::global());
        addToRoom(rows);
    } else if (action == QLatin1String("resume")) {
        roomSend(QStringLiteral("room.resume"));
    } else if (action == QLatin1String("pause")) {
        roomSend(QStringLiteral("room.pause"), { { "position", roomPosition() } });
    } else if (action == QLatin1String("next")) {
        roomSend(QStringLiteral("room.next"));
    } else if (action == QLatin1String("previous")) {
        roomSend(QStringLiteral("room.previous"));
    } else if (action == QLatin1String("seek")) {
        roomSend(QStringLiteral("room.seek"), { { "position", arg.toDouble() } });
    }
    return true;
}

void OnlineService::playerMovedOn(const QString &trackId)
{
    if (m_roomCurrent < 0 || m_roomCurrent >= m_roomItems.size())
        return;
    const RoomItem &item = m_roomItems.at(m_roomCurrent);
    if (debug())
        qInfo("online: player moved on from %s (room's current %s)", qPrintable(trackId), qPrintable(item.trackId));
    if (item.trackId == trackId)
        roomSend(QStringLiteral("room.ended"), { { "itemId", item.itemId } });
}

// ---- shared playlists -------------------------------------------------------------

OnlineService::SharedPlaylist *OnlineService::findPlaylist(const QString &id)
{
    for (SharedPlaylist &p : m_playlists)
        if (p.id == id)
            return &p;
    return nullptr;
}

QStringList OnlineService::playlistTrackIds(const QString &id) const
{
    for (const SharedPlaylist &p : m_playlists)
        if (p.id == id)
            return p.trackIds;
    return {};
}

bool OnlineService::isPlaylistOwner(const QString &id) const
{
    for (const SharedPlaylist &p : m_playlists)
        if (p.id == id)
            return p.ownerId == m_userId;
    return false;
}

QVariantMap OnlineService::playlistInfo(const QString &id) const
{
    for (const SharedPlaylist &p : m_playlists) {
        if (p.id == id)
            return { { "owner", p.ownerName }, { "members", p.memberNames }, { "url", p.url },
                     { "isOwner", p.ownerId == m_userId }, { "protected", p.locked } };
    }
    return {};
}

void OnlineService::applyPlaylist(const QJsonObject &o)
{
    const QString id = o.value("id").toString();
    if (id.isEmpty() || !o.contains("tracks"))
        return;
    // Our own copy of a YouTube Music or Spotify playlist: this app shows the original.
    const QString origin = o.value("origin").toString();
    if (!origin.isEmpty() && o.value("owner").toObject().value("id").toString() == m_userId) {
        m_mirrors.insert(origin, id);
        return;
    }
    SharedPlaylist p;
    p.id = id;
    p.name = o.value("name").toString();
    p.description = o.value("description").toString();
    p.ownerId = o.value("owner").toObject().value("id").toString();
    p.ownerName = o.value("owner").toObject().value("name").toString();
    p.url = o.value("url").toString();
    p.version = o.value("version").toInt();
    p.locked = o.value("protected").toBool();
    p.cover = o.value("cover").toString();
    for (const QJsonValue &m : o.value("members").toArray())
        p.memberNames << m.toObject().value("name").toString();
    for (const QJsonValue &v : o.value("tracks").toArray()) {
        const QJsonObject e = v.toObject();
        const Track t = storeTrack(e.value("track").toObject());
        if (t.id.isEmpty() || p.entryOf.contains(t.id))
            continue;
        p.trackIds << t.id;
        p.entryOf.insert(t.id, e.value("entryId").toString());
    }
    if (SharedPlaylist *existing = findPlaylist(id))
        *existing = p;
    else
        m_playlists << p;
    savePlaylists();
    emit playlistsChanged();
}

// ---- YouTube Music's home page, through the server (as the browser version shows it) ---------

QVariantList OnlineService::storeMusicTracks(const QJsonArray &tracks)
{
    QVariantList rows;
    for (const QJsonValue &v : tracks) {
        const QJsonObject m = v.toObject();
        // the search's shape ({ id, videoId, … }) as the server's shared tracks have it
        const Track t = storeTrack({ { "id", m.value("id") }, { "source", "youtube" }, { "path", m.value("videoId") },
                                     { "title", m.value("title") }, { "artist", m.value("artist") },
                                     { "artistId", m.value("artistId") }, { "album", m.value("album") },
                                     { "duration", m.value("duration") }, { "cover", m.value("cover") } });
        if (const int row = Library::instance()->tracks()->rowOf(t.id); row >= 0)
            rows << row;
    }
    return rows;
}

void OnlineService::loadMusicShelves()
{
    request("GET", QStringLiteral("/music/home"), {}, [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty())
            return;   // the home page goes on without them
        QVariantList shelves;
        for (const QJsonValue &v : reply.value("shelves").toArray()) {
            const QJsonObject s = v.toObject();
            QVariantMap shelf { { "title", s.value("title").toString() }, { "kind", s.value("kind").toString() } };
            if (shelf.value("kind") == QLatin1String("tracks"))
                shelf.insert("rows", storeMusicTracks(s.value("items").toArray()));
            else
                shelf.insert("items", s.value("items").toArray().toVariantList());
            shelves << shelf;
        }
        m_musicShelves = shelves;
        emit musicShelvesChanged();
    });
}

void OnlineService::playCollection(const QString &id, const QString &title)
{
    request("GET", QStringLiteral("/music/collection/") + QString::fromLatin1(QUrl::toPercentEncoding(id)), {},
            [this, title](const QJsonObject &reply, const QString &error) {
        const QVariantList rows = error.isEmpty() ? storeMusicTracks(reply.value("tracks").toArray()) : QVariantList();
        if (rows.isEmpty()) {
            emit notice(tr("Couldn't open %1").arg(title), QStringLiteral("close"));
            return;
        }
        if (m_player)
            m_player->playFrom(rows, rows.first().toInt(), QStringLiteral("ytmusic:") + title);
    });
}

void OnlineService::fetchPlaylist(const QString &id)
{
    request("GET", QStringLiteral("/playlists/") + id, {}, [this](const QJsonObject &reply, const QString &error) {
        if (error.isEmpty() && reply.value("member").toBool())
            applyPlaylist(reply);
    });
}

void OnlineService::refreshPlaylists()
{
    if (!registered())
        return;
    request("GET", QStringLiteral("/playlists"), {}, [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty())
            return;
        m_mirrors.clear();
        QSet<QString> present;
        for (const QJsonValue &v : reply.value("playlists").toArray()) {
            present.insert(v.toObject().value("id").toString());
            applyPlaylist(v.toObject());
        }
        const auto before = m_playlists.size();
        m_playlists.erase(std::remove_if(m_playlists.begin(), m_playlists.end(),
                                         [&](const SharedPlaylist &p) { return !present.contains(p.id); }),
                          m_playlists.end());
        if (m_playlists.size() != before) {
            savePlaylists();
            emit playlistsChanged();
        }
        if (!m_playlists.isEmpty())
            connectSocket();   // live changes from the others
        m_mirrorsListed = true;
        emit mirrorsChanged();
        scheduleMirrors();
    });
}

void OnlineService::createPlaylist(const QString &name, const QString &description, const QVariantList &rows)
{
    portableTracks(rows, [this, name, description](const QList<Track> &tracks, int skipped) {
        ensureAccount([this, name, description, tracks, skipped](bool ok) {
            if (!ok)
                return;
            request("POST", QStringLiteral("/playlists"),
                    { { "name", name }, { "description", description }, { "tracks", tracksJson(tracks) } },
                    [this, name, skipped](const QJsonObject &reply, const QString &error) {
                if (!error.isEmpty()) {
                    emit notice(tr("Couldn't create the playlist: %1").arg(error), QStringLiteral("close"));
                    return;
                }
                applyPlaylist(reply);
                connectSocket();
                const QString url = reply.value("url").toString();
                if (QClipboard *clipboard = QGuiApplication::clipboard())
                    clipboard->setText(url);
                emit notice(skipped > 0 ? tr("Created %1 — the invite link is copied; %n tracks without a YouTube Music version were skipped", "", skipped).arg(name)
                                        : tr("Created %1 — the invite link is copied").arg(name),
                            QStringLiteral("link"));
                emit playlistOpened(reply.value("id").toString());
            });
        });
    });
}

void OnlineService::joinWithPassword(const QString &id, const QString &password)
{
    joinPlaylist(id, password);
}

void OnlineService::joinPlaylist(const QString &id, const QString &password)
{
    if (findPlaylist(id)) {
        emit playlistOpened(id);
        return;
    }
    ensureAccount([this, id, password](bool ok) {
        if (!ok)
            return;
        const QJsonObject body = password.isEmpty() ? QJsonObject() : QJsonObject { { "password", password } };
        request("POST", QStringLiteral("/playlists/") + id + QStringLiteral("/join"), body,
                [this, id](const QJsonObject &reply, const QString &error) {
            const QString code = reply.value("error").toString();
            if (code == QLatin1String("password_required") || code == QLatin1String("bad_password")) {
                emit activateRequested();
                emit passwordNeeded(id, code == QLatin1String("bad_password"));
                return;
            }
            if (code == QLatin1String("rate_limited")) {
                emit notice(tr("Too many wrong passwords — try again in a few minutes"), QStringLiteral("close"));
                return;
            }
            if (!error.isEmpty()) {
                emit notice(tr("Couldn't join the playlist: %1").arg(error), QStringLiteral("close"));
                return;
            }
            applyPlaylist(reply);
            connectSocket();
            emit notice(tr("You've joined %1 — what you add, everyone sees").arg(reply.value("name").toString()),
                        QStringLiteral("check"));
            emit playlistOpened(id);
        });
    });
}

void OnlineService::setPlaylistPassword(const QString &id, const QString &password)
{
    request("PATCH", QStringLiteral("/playlists/") + id, { { "password", password } },
            [this, password](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("Couldn't change the password: %1").arg(error), QStringLiteral("close"));
            return;
        }
        applyPlaylist(reply);
        emit notice(password.isEmpty() ? tr("Password removed — anyone with the link can join")
                                       : tr("Password set — others need it to join"), QStringLiteral("check"));
    });
}

void OnlineService::setPlaylistCover(const QString &id, const QByteArray &jpeg, std::function<void(bool)> done, bool quiet)
{
    send("PUT", QStringLiteral("/playlists/") + id + QStringLiteral("/cover"), jpeg, "image/jpeg",
         [this, quiet, done = std::move(done)](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty() && !quiet)
            emit notice(tr("Couldn't share the cover with the others: %1").arg(error), QStringLiteral("close"));
        else if (error.isEmpty())
            applyPlaylist(reply);
        done(error.isEmpty());
    });
}

void OnlineService::removePlaylistCover(const QString &id)
{
    request("DELETE", QStringLiteral("/playlists/") + id + QStringLiteral("/cover"), {},
            [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty())
            emit notice(tr("Couldn't remove the cover: %1").arg(error), QStringLiteral("close"));
        else
            applyPlaylist(reply);
    });
}

void OnlineService::addToPlaylist(const QString &id, const QVariantList &rows)
{
    const SharedPlaylist *p = findPlaylist(id);
    if (!p)
        return;
    const QString name = p->name;
    portableTracks(rows, [this, id, name](const QList<Track> &tracks, int skipped) {
        if (tracks.isEmpty()) {
            emit notice(tr("No YouTube Music version of this track was found to share"), QStringLiteral("close"));
            return;
        }
        request("POST", QStringLiteral("/playlists/") + id + QStringLiteral("/tracks"), { { "tracks", tracksJson(tracks) } },
                [this, name, skipped](const QJsonObject &reply, const QString &error) {
            if (!error.isEmpty()) {
                emit notice(tr("Couldn't add to %1: %2").arg(name, error), QStringLiteral("close"));
                return;
            }
            applyPlaylist(reply);
            const int added = reply.value("added").toInt();
            emit notice(added == 0 ? tr("Already in %1").arg(name)
                        : skipped > 0 ? tr("Added to %1 — %n without a YouTube Music version skipped", "", skipped).arg(name)
                                      : tr("Added to %1").arg(name),
                        QStringLiteral("check"));
        });
    });
}

void OnlineService::removeFromPlaylist(const QString &id, const QString &trackId)
{
    const SharedPlaylist *p = findPlaylist(id);
    if (!p || !p->entryOf.contains(trackId))
        return;
    const QString name = p->name;
    request("DELETE", QStringLiteral("/playlists/") + id + QStringLiteral("/tracks/") + p->entryOf.value(trackId), {},
            [this, name](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("Couldn't remove from %1: %2").arg(name, error), QStringLiteral("close"));
            return;
        }
        applyPlaylist(reply);
        emit notice(tr("Removed from %1").arg(name), QStringLiteral("check"));
    });
}

bool OnlineService::moveInPlaylist(const QString &id, const QString &trackId, const QString &beforeTrackId)
{
    SharedPlaylist *p = findPlaylist(id);
    if (!p || !p->entryOf.contains(trackId))
        return false;
    // here at once; the server says the same a moment later (and tells the others)
    p->trackIds.removeAll(trackId);
    const int at = beforeTrackId.isEmpty() ? -1 : p->trackIds.indexOf(beforeTrackId);
    const int to = at < 0 ? int(p->trackIds.size()) : at;
    p->trackIds.insert(to, trackId);
    savePlaylists();
    emit playlistsChanged();
    const QString name = p->name;
    request("POST", QStringLiteral("/playlists/") + id + QStringLiteral("/tracks/") + p->entryOf.value(trackId) + QStringLiteral("/move"),
            { { "to", to } }, [this, id, name](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("Couldn't move it in %1: %2").arg(name, error), QStringLiteral("close"));
            fetchPlaylist(id);
            return;
        }
        applyPlaylist(reply);
    });
    return true;
}

void OnlineService::editPlaylist(const QString &id, const QString &name, const QString &description)
{
    if (!isPlaylistOwner(id)) {
        emit notice(tr("Only the playlist's owner can rename it"), QStringLiteral("close"));
        return;
    }
    request("PATCH", QStringLiteral("/playlists/") + id, { { "name", name }, { "description", description } },
            [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty())
            emit notice(tr("Couldn't rename the playlist: %1").arg(error), QStringLiteral("close"));
        else
            applyPlaylist(reply);
    });
}

void OnlineService::deleteOrLeavePlaylist(const QString &id)
{
    const SharedPlaylist *p = findPlaylist(id);
    if (!p)
        return;
    const QString name = p->name;
    const bool owner = p->ownerId == m_userId;
    auto drop = [this, id] {
        for (int i = 0; i < m_playlists.size(); ++i)
            if (m_playlists.at(i).id == id)
                m_playlists.removeAt(i--);
        savePlaylists();
        emit playlistsChanged();
    };
    if (owner) {
        request("DELETE", QStringLiteral("/playlists/") + id, {}, [this, name, drop](const QJsonObject &, const QString &error) {
            if (!error.isEmpty()) {
                emit notice(tr("Couldn't delete %1: %2").arg(name, error), QStringLiteral("close"));
                return;
            }
            drop();
            emit notice(tr("Deleted %1 for everyone").arg(name), QStringLiteral("check"));
        });
    } else {
        request("POST", QStringLiteral("/playlists/") + id + QStringLiteral("/leave"), {},
                [this, name, drop](const QJsonObject &, const QString &error) {
            if (!error.isEmpty()) {
                emit notice(tr("Couldn't leave %1: %2").arg(name, error), QStringLiteral("close"));
                return;
            }
            drop();
            emit notice(tr("You've left %1").arg(name), QStringLiteral("check"));
        });
    }
}

void OnlineService::copyPlaylistLink(const QString &id)
{
    if (const SharedPlaylist *p = findPlaylist(id))
        copyToClipboard(p->url, tr("Invite link copied — anyone with it can add tracks"));
}

void OnlineService::loadPlaylists()
{
    QFile f(cacheFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value("server").toString() != storedServer())
        return;
    // Library doesn't exist yet here — the tracks reach the model through tracks().
    for (const QJsonValue &v : root.value("playlists").toArray()) {
        const QJsonObject o = v.toObject();
        SharedPlaylist p;
        p.id = o.value("id").toString();
        p.name = o.value("name").toString();
        p.description = o.value("description").toString();
        p.ownerId = o.value("ownerId").toString();
        p.ownerName = o.value("ownerName").toString();
        p.url = o.value("url").toString();
        p.version = o.value("version").toInt();
        p.cover = o.value("cover").toString();
        for (const QJsonValue &m : o.value("members").toArray())
            p.memberNames << m.toString();
        for (const QJsonValue &e : o.value("tracks").toArray()) {
            const QJsonObject entry = e.toObject();
            const QJsonObject track = entry.value("track").toObject();
            const QString trackId = track.value("id").toString();
            if (trackId.isEmpty())
                continue;
            if (!m_tracks.contains(trackId)) {
                Track t;
                t.id = trackId;
                t.path = track.value("path").toString();
                t.source = track.value("source").toString();
                t.title = track.value("title").toString();
                t.artist = track.value("artist").toString();
                t.artistId = track.value("artistId").toString();
                t.album = track.value("album").toString();
                t.duration = track.value("duration").toInt();
                t.cover = track.value("cover").toString();
                t.quality = t.source == QLatin1String("soundcloud") ? QStringLiteral("SoundCloud") : QStringLiteral("YouTube Music");
                t.transient = true;
                const QString key = TrackModel::albumKey(t);
                const auto [a, b] = Scanner::fallbackPalette(key);
                t.colorA = a;
                t.colorB = b;
                t.seed = Scanner::stableSeed(key);
                m_tracks.insert(trackId, t);
            }
            p.trackIds << trackId;
            p.entryOf.insert(trackId, entry.value("entryId").toString());
        }
        if (!p.id.isEmpty())
            m_playlists << p;
    }
}

void OnlineService::savePlaylists() const
{
    QJsonArray playlists;
    for (const SharedPlaylist &p : m_playlists) {
        QJsonArray tracks;
        for (const QString &id : p.trackIds)
            tracks.append(QJsonObject { { "entryId", p.entryOf.value(id) }, { "track", trackJson(m_tracks.value(id)) } });
        playlists.append(QJsonObject { { "id", p.id }, { "name", p.name }, { "description", p.description },
                                       { "ownerId", p.ownerId }, { "ownerName", p.ownerName }, { "url", p.url },
                                       { "version", p.version }, { "members", QJsonArray::fromStringList(p.memberNames) },
                                       { "cover", p.cover }, { "tracks", tracks } });
    }
    QSaveFile f(cacheFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject { { "server", storedServer() }, { "playlists", playlists } }).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

// ---- the account's library: YouTube Music likes and the plays, on every device ---------

void OnlineService::setLibrarySync(bool on)
{
    if (on == m_librarySync)
        return;
    m_librarySync = on;
    AppPaths::settings()->setValue("online/librarySync", on);
    emit settingsChanged();
    if (on && registered()) {
        m_libTimer.start();
        syncLibrary();
        connectSocket();
    } else {
        m_libTimer.stop();
    }
}

QString OnlineService::syncFile() const
{
    const QByteArray key = QCryptographicHash::hash(storedServer().toUtf8(), QCryptographicHash::Sha1).toHex().left(10);
    return AppPaths::dataDir() + QStringLiteral("/library-sync-") + QString::fromLatin1(key) + QStringLiteral(".json");
}

void OnlineService::loadLibrarySync()
{
    m_libSynced = false;
    m_libVersion = -1;
    m_serverLiked.clear();
    m_libOps.clear();
    m_mirrors.clear();
    m_mirrorSigs.clear();
    m_mirrorsListed = false;
    QFile f(syncFile());
    if (m_userId.isEmpty() || !f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value("user").toString() != m_userId)
        return;   // another account's
    m_libSynced = o.value("synced").toBool();
    m_libVersion = o.value("version").toInt(-1);
    for (const QJsonValue &v : o.value("liked").toArray())
        m_serverLiked.insert(v.toString());
    for (const QJsonValue &v : o.value("ops").toArray())
        m_libOps << v.toObject();
    const QJsonObject sigs = o.value("mirrors").toObject();
    for (auto it = sigs.begin(); it != sigs.end(); ++it)
        m_mirrorSigs.insert(it.key(), it.value().toString());
}

void OnlineService::saveLibrarySync() const
{
    QJsonArray liked, ops;
    for (const QString &id : m_serverLiked)
        liked.append(id);
    for (const QJsonObject &op : m_libOps)
        ops.append(op);
    QJsonObject sigs;
    for (auto it = m_mirrorSigs.cbegin(); it != m_mirrorSigs.cend(); ++it)
        sigs.insert(it.key(), it.value());
    QSaveFile f(syncFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject { { "user", m_userId }, { "synced", m_libSynced }, { "version", m_libVersion },
                                            { "liked", liked }, { "ops", ops }, { "mirrors", sigs } }).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void OnlineService::resetLibrarySync()
{
    m_libSynced = false;
    m_libVersion = -1;
    m_serverLiked.clear();
    m_libOps.clear();
    m_mirrors.clear();
    m_mirrorSigs.clear();
    m_mirrorsListed = false;
    saveLibrarySync();
}

void OnlineService::noteLiked(const Track &t, bool liked)
{
    // Before the first sync this device's likes all go up anyway.
    if (!m_librarySync || !registered() || !m_libSynced || t.demo || t.source != QLatin1String("youtube"))
        return;
    queueLibraryOp(liked ? QJsonObject { { "op", "like" }, { "id", t.id }, { "track", trackJson(t) } }
                         : QJsonObject { { "op", "unlike" }, { "id", t.id } });
    flushLibraryOps({});
}

void OnlineService::queueLibraryOp(const QJsonObject &op)
{
    // A later change of the same track replaces one still waiting.
    const QString id = op.value("id").toString();
    m_libOps.erase(std::remove_if(m_libOps.begin(), m_libOps.end(),
                                  [&](const QJsonObject &o) { return o.value("id").toString() == id; }),
                   m_libOps.end());
    m_libOps << op;
    saveLibrarySync();
}

// The server answers a change with the account's new version: one step on means nothing else changed.
void OnlineService::libraryVersion(const QJsonObject &reply)
{
    const int version = reply.value("version").toInt(-1);
    if (version >= 0 && m_libVersion >= 0 && version == m_libVersion + 1)
        m_libVersion = version;
}

void OnlineService::flushLibraryOps(std::function<void()> then)
{
    if (m_libFlushing || m_libOps.isEmpty() || !registered()) {
        if (then && !m_libFlushing)
            then();
        return;
    }
    m_libFlushing = true;
    // Likes go up together (up to kLikeBatch, in the order they were queued); an unlike on its own.
    QList<QJsonObject> batch { m_libOps.first() };
    const bool like = batch.first().value("op") == QLatin1String("like");
    while (like && batch.size() < kLikeBatch && batch.size() < m_libOps.size()
           && m_libOps.at(batch.size()).value("op") == QLatin1String("like"))
        batch << m_libOps.at(batch.size());
    auto done = [this, batch, like, then](const QJsonObject &reply, const QString &error) {
        m_libFlushing = false;
        const bool offline = !error.isEmpty() && (reply.isEmpty() || reply.value("error") == QLatin1String("rate_limited"));
        if (offline) {   // try again with the next sync
            if (then)
                then();
            return;
        }
        for (const QJsonObject &op : batch) {
            m_libOps.removeOne(op);
            if (!error.isEmpty())
                continue;
            if (like)
                m_serverLiked.insert(op.value("id").toString());
            else
                m_serverLiked.remove(op.value("id").toString());
        }
        if (error.isEmpty())
            libraryVersion(reply);
        saveLibrarySync();
        flushLibraryOps(then);
    };
    if (like) {
        QJsonArray items;
        for (const QJsonObject &op : batch) {
            QJsonObject item { { "track", op.value("track") } };
            if (op.contains("at"))
                item.insert("at", op.value("at"));
            items.append(item);
        }
        request("POST", QStringLiteral("/library/liked"), { { "items", items } }, done);
    } else {
        const QString id = batch.first().value("id").toString();
        request("DELETE", QStringLiteral("/library/liked/") + QString::fromUtf8(QUrl::toPercentEncoding(id)), {}, done);
    }
}

void OnlineService::syncLibrary()
{
    if (!m_librarySync || !registered())
        return;
    if (m_libSyncing) {
        m_libAgain = true;
        return;
    }
    m_libSyncing = true;
    flushLibraryOps([this] {
        request("GET", QStringLiteral("/library"), {}, [this](const QJsonObject &reply, const QString &error) {
            if (error.isEmpty() && reply.contains("liked"))
                applyLibrary(reply);
            m_libSyncing = false;
            if (std::exchange(m_libAgain, false))
                syncLibrary();
        });
    });
}

void OnlineService::applyLibrary(const QJsonObject &library)
{
    YouTubeService *yt = YouTubeService::instance();
    const TrackModel *model = Library::instance()->tracks();

    // Likes: what's on the account and not here was liked elsewhere; what's here and not there was either
    // unliked elsewhere (it was there at the last sync) or liked here meanwhile (it wasn't).
    QHash<QString, qint64> serverAt;   // the account's likes and when (ms)
    QList<Track> adopt;               // newest first, as the server lists them
    const QStringList localIds = yt->likedIds();
    const QSet<QString> local(localIds.cbegin(), localIds.cend());
    for (const QJsonValue &v : library.value("liked").toArray()) {
        const Track t = serverTrack(v.toObject());
        if (t.source != QLatin1String("youtube") || t.path.isEmpty())
            continue;
        serverAt.insert(t.id, qint64(v.toObject().value("at").toDouble()));
        if (!local.contains(t.id))
            adopt << t;
    }
    // The likes here, newest first: each one the account lacks goes up just below the newer one before it,
    // and one the account has in another order is moved there — so the account keeps this order.
    QStringList unlike;
    qint64 cursor = nowMs();
    for (const QString &id : localIds) {
        const bool pending = std::any_of(m_libOps.cbegin(), m_libOps.cend(),
                                         [&](const QJsonObject &o) { return o.value("id").toString() == id; });
        if (pending)
            continue;
        const auto at = serverAt.constFind(id);
        if (at != serverAt.cend() && *at > 0 && *at < cursor) {
            cursor = *at;
            continue;
        }
        if (at == serverAt.cend() && m_libSynced && m_serverLiked.contains(id)) {
            unlike << id;   // unliked on another device
            continue;
        }
        const int row = model->rowOf(id);
        if (row < 0)
            continue;
        queueLibraryOp({ { "op", "like" }, { "id", id }, { "track", trackJson(model->at(row)) }, { "at", double(--cursor) } });
    }
    m_serverLiked = QSet<QString>(serverAt.keyBegin(), serverAt.keyEnd());
    m_libVersion = library.value("version").toInt();
    m_libSynced = true;
    saveLibrarySync();
    if (!adopt.isEmpty() || !unlike.isEmpty())
        yt->adoptLiked(adopt, unlike);

    // Plays on the other devices join "Recently played".
    QList<std::pair<Track, qint64>> plays;
    for (const QJsonValue &v : library.value("history").toArray()) {
        const QJsonObject o = v.toObject();
        const Track t = serverTrack(o);
        if (!t.id.isEmpty() && (t.source == QLatin1String("youtube") || t.source == QLatin1String("soundcloud")))
            plays.append({ t, qint64(o.value("at").toDouble()) });
    }
    if (!plays.isEmpty())
        ListeningHistory::instance()->addRemote(plays);

    if (!m_libOps.isEmpty())
        flushLibraryOps({});

    // The playlists' folders and the hidden ones: the newer of the two layouts wins.
    const QJsonObject serverOrg = library.value("organizer").toObject();
    const double serverUpdated = serverOrg.value("updated").toDouble();
    const double localUpdated = Library::instance()->organizer().value("updated").toDouble();
    if (serverUpdated > localUpdated)
        Library::instance()->applyOrganizer(serverOrg);
    else if (localUpdated > serverUpdated)
        m_orgTimer.start();

    scheduleMirrors();
}

void OnlineService::pushOrganizer()
{
    if (!m_librarySync || !registered())
        return;
    request("PUT", QStringLiteral("/library/organizer"), Library::instance()->organizer(),
            [this](const QJsonObject &reply, const QString &error) {
                if (error.isEmpty())
                    libraryVersion(reply);
            });
}

void OnlineService::notePlayed(const Track &t)
{
    if (!m_librarySync || !registered() || t.demo
        || (t.source != QLatin1String("youtube") && t.source != QLatin1String("soundcloud")))
        return;
    request("POST", QStringLiteral("/library/history"), { { "track", trackJson(t) } },
            [this](const QJsonObject &reply, const QString &error) {
                if (error.isEmpty())
                    libraryVersion(reply);
            });
}

// ---- copies of the YouTube Music playlists -------------------------------------------

void OnlineService::scheduleMirrors()
{
    if (m_librarySync && registered()
        && (YouTubeService::instance()->connected() || SpotifyService::instance()->connected()
            || SoundCloudService::instance()->connected()))
        m_mirrorTimer.start();
}

void OnlineService::syncMirrors()
{
    YouTubeService *yt = YouTubeService::instance();
    SpotifyService *sp = SpotifyService::instance();
    SoundCloudService *sc = SoundCloudService::instance();
    if (!m_librarySync || !registered() || (!yt->connected() && !sp->connected() && !sc->connected()) || m_mirrorBusy)
        return;
    if (!m_mirrorsListed) {   // which copies exist first, so none is made twice
        refreshPlaylists();
        return;
    }
    struct Source
    {
        QString origin;
        QString name;
        QList<Track> tracks;
        bool complete = true;   // every track's YouTube Music version is known
    };
    QList<Source> sources;
    QSet<QString> listed;       // sources whose list is in hand: a copy missing from it goes
    const TrackModel *model = Library::instance()->tracks();

    if (yt->connected()) {
        const QVariantList playlists = yt->playlists();
        if (!playlists.isEmpty())
            listed << QStringLiteral("ytmusic");
        for (const QVariant &v : playlists) {
            const QVariantMap p = v.toMap();
            const QString id = p.value("id").toString();
            if (id.isEmpty())
                continue;
            yt->ensurePlaylist(id);   // fresh once per session (it calls back through dataChanged)
            Source s { QStringLiteral("ytmusic:") + id, p.value("title").toString(), {}, !yt->playlistLoading(id) };
            for (const QString &trackId : yt->playlistTrackIds(id)) {
                const int row = model->rowOf(trackId);
                if (row >= 0 && model->at(row).source == QLatin1String("youtube") && !model->at(row).path.isEmpty())
                    s.tracks << model->at(row);
            }
            if (s.name.isEmpty())
                s.name = QStringLiteral("YouTube Music");
            sources << s;
        }
    }
    if (sp->connected()) {
        // Spotify tracks travel as their YouTube Music versions; unknown ones are queued for matching.
        auto spotify = [&](const QString &origin, const QString &name, const QStringList &ids) {
            Source s { origin, name, {}, true };
            for (const QString &id : ids) {
                if (!sp->matchKnown(id)) {
                    s.complete = false;
                    if (!m_matchQueued.contains(id)) {
                        m_matchQueued.insert(id);
                        m_matchQueue << id;
                    }
                    continue;
                }
                const QString videoId = sp->matchedVideoId(id);
                if (videoId.isEmpty())
                    continue;   // no YouTube Music version
                Track t = sp->trackFor(id);
                t.id = QStringLiteral("yt:") + videoId;
                t.path = videoId;
                t.source = QStringLiteral("youtube");
                s.tracks << t;
            }
            sources << s;
        };
        const QVariantList playlists = sp->playlists();
        if (!playlists.isEmpty() || !sp->likedIds().isEmpty())
            listed << QStringLiteral("spotify");
        if (!sp->likedIds().isEmpty())
            spotify(QStringLiteral("spotify:liked"), tr("Liked on Spotify"), sp->likedIds());
        for (const QVariant &v : playlists) {
            const QVariantMap p = v.toMap();
            spotify(QStringLiteral("spotify:") + p.value("id").toString(), p.value("name").toString(),
                    sp->playlistTrackIds(p.value("id").toString()));
        }
        if (!m_matchQueue.isEmpty() && !m_matchTimer.isActive())
            m_matchTimer.start();
    }

    if (sc->connected()) {
        // SoundCloud tracks travel as they are: every app plays them (the browser version through the server).
        auto soundcloud = [&](const QString &origin, const QString &name, const QStringList &ids, bool complete) {
            Source s { origin, name, {}, complete };
            for (const QString &id : ids) {
                const int row = model->rowOf(id);
                if (row >= 0 && model->at(row).source == QLatin1String("soundcloud"))
                    s.tracks << model->at(row);
            }
            sources << s;
        };
        const QVariantList playlists = sc->playlists();
        if (!playlists.isEmpty() || !sc->likedIds().isEmpty())
            listed << QStringLiteral("soundcloud");
        if (!sc->likedIds().isEmpty())
            soundcloud(QStringLiteral("soundcloud:liked"), tr("Liked on SoundCloud"), sc->likedIds(), true);
        for (const QVariant &v : playlists) {
            const QString id = v.toMap().value("id").toString();
            if (id.isEmpty())
                continue;
            sc->ensurePlaylist(id);
            soundcloud(QStringLiteral("soundcloud:") + id, v.toMap().value("title").toString(), sc->playlistTrackIds(id),
                       !sc->playlistLoading(id));
        }
    }

    QSet<QString> current;
    for (const Source &s : std::as_const(sources)) {
        current.insert(s.origin);
        // Unfinished: the first copy goes up with what's there (once there's something), later ones wait
        // until it's complete.
        if (!s.complete && (m_mirrors.contains(s.origin) || s.tracks.isEmpty()))
            continue;
        QStringList ids;
        for (const Track &t : s.tracks)
            ids << t.id;
        const QString signature = QString::fromLatin1(QCryptographicHash::hash((s.name + QLatin1Char('\n') + ids.join(QLatin1Char(','))).toUtf8(),
                                                                              QCryptographicHash::Sha1).toHex());
        if (m_mirrors.contains(s.origin) && m_mirrorSigs.value(s.origin) == signature)
            continue;
        pushMirror(s.origin, s.name, s.tracks, signature);
        return;   // one at a time; the next one when this is done
    }
    // A playlist gone from its source takes its copy with it (only with the source's list in hand).
    for (auto it = m_mirrors.cbegin(); it != m_mirrors.cend(); ++it) {
        if (current.contains(it.key()) || !listed.contains(it.key().section(QLatin1Char(':'), 0, 0)))
            continue;
        const QString origin = it.key();
        m_mirrorBusy = true;
        request("DELETE", QStringLiteral("/playlists/") + it.value(), {}, [this, origin](const QJsonObject &, const QString &error) {
            m_mirrorBusy = false;
            if (error.isEmpty()) {
                m_mirrors.remove(origin);
                m_mirrorSigs.remove(origin);
                saveLibrarySync();
                scheduleMirrors();
            }
        });
        return;
    }
}

void OnlineService::pushMirror(const QString &origin, const QString &name, const QList<Track> &tracks, const QString &signature)
{
    // The first batch makes or replaces the copy, the rest is appended (a request has a size limit).
    auto rest = std::make_shared<QList<Track>>(tracks.mid(kMirrorBatch));
    const QList<Track> first = tracks.mid(0, kMirrorBatch);
    m_mirrorBusy = true;
    auto finish = [this, origin, signature](bool ok) {
        m_mirrorBusy = false;
        if (ok) {
            m_mirrorSigs.insert(origin, signature);
            saveLibrarySync();
            QTimer::singleShot(300, this, &OnlineService::syncMirrors);   // the next playlist
        }
    };
    auto append = std::make_shared<std::function<void(const QString &)>>();
    *append = [this, rest, finish, append](const QString &id) {
        if (rest->isEmpty()) {
            finish(true);
            return;
        }
        const QList<Track> batch = rest->mid(0, kMirrorBatch);
        *rest = rest->mid(kMirrorBatch);
        request("POST", QStringLiteral("/playlists/") + id + QStringLiteral("/tracks"), { { "tracks", tracksJson(batch) } },
                [finish, append, id](const QJsonObject &, const QString &error) {
                    if (!error.isEmpty())
                        finish(false);
                    else
                        (*append)(id);
                });
    };
    const QString existing = m_mirrors.value(origin);
    if (existing.isEmpty()) {
        request("POST", QStringLiteral("/playlists"),
                { { "name", name.left(100) }, { "tracks", tracksJson(first) }, { "origin", origin } },
                [this, origin, finish, append](const QJsonObject &reply, const QString &error) {
                    const QString id = reply.value("id").toString();
                    if (!error.isEmpty() || id.isEmpty()) {
                        finish(false);
                        return;
                    }
                    m_mirrors.insert(origin, id);
                    emit mirrorsChanged();
                    (*append)(id);
                });
        return;
    }
    request("PATCH", QStringLiteral("/playlists/") + existing, { { "name", name.left(100) } },
            [](const QJsonObject &, const QString &) {});
    request("PUT", QStringLiteral("/playlists/") + existing + QStringLiteral("/tracks"), { { "tracks", tracksJson(first) } },
            [this, origin, existing, finish, append](const QJsonObject &, const QString &error) {
                if (!error.isEmpty()) {
                    if (error.contains(QLatin1String("no such playlist"))) {   // deleted on the server: made again next time
                        m_mirrors.remove(origin);
                        m_mirrorSigs.remove(origin);
                    }
                    finish(false);
                    return;
                }
                (*append)(existing);
            });
}

void OnlineService::matchNext()
{
    SpotifyService *sp = SpotifyService::instance();
    while (!m_matchQueue.isEmpty() && sp->matchKnown(m_matchQueue.first()))
        m_matchQueued.remove(m_matchQueue.takeFirst());
    if (m_matchQueue.isEmpty() || !sp->connected()) {
        scheduleMirrors();
        return;
    }
    const QString id = m_matchQueue.takeFirst();
    sp->match(id, [this, id](const QString &) {
        m_matchQueued.remove(id);
        // the copies grow every now and then, not with every track
        if (m_matchQueue.isEmpty() || m_matchQueue.size() % 25 == 0)
            scheduleMirrors();
        if (!m_matchQueue.isEmpty())
            m_matchTimer.start();
    });
}

// ---- devices: one account on several -------------------------------------------------

void OnlineService::createPairCode()
{
    ensureAccount([this](bool ok) {
        if (!ok)
            return;
        request("POST", QStringLiteral("/me/pair"), {}, [this](const QJsonObject &reply, const QString &error) {
            if (!error.isEmpty()) {
                emit notice(tr("Couldn't get a code: %1").arg(error), QStringLiteral("close"));
                return;
            }
            m_pairCode = reply.value("code").toString();
            m_pairExpires = reply.value("expires").toDouble() * 1000;
            emit pairChanged();
        });
    });
}

void OnlineService::redeemPairCode(const QString &code)
{
    QString clean = code.toUpper();
    clean.remove(QRegularExpression(QStringLiteral("[^A-Z0-9]")));
    if (clean.size() != 8) {
        emit notice(tr("The code has 8 letters and digits"), QStringLiteral("close"));
        return;
    }
    request("POST", QStringLiteral("/pair"), { { "code", clean } }, [this](const QJsonObject &reply, const QString &error) {
        const QString token = reply.value("token").toString();
        if (!error.isEmpty() || token.isEmpty()) {
            emit notice(reply.value("error") == QLatin1String("bad_code") ? tr("Wrong or expired code")
                                                                           : tr("Couldn't link: %1").arg(error),
                        QStringLiteral("close"));
            return;
        }
        switchAccount(token, reply.value("user").toObject());
        emit notice(tr("Linked — this device uses the same Everplay account now"), QStringLiteral("check"));
    });
}

// ---- signing in with a login and a password -----------------------------------------

QString OnlineService::deviceName()
{
    // Plain printable ASCII: it goes in a header, and anything else breaks HTTP/2's header compression.
    const QString host = QSysInfo::machineHostName();
    const QString os = QSysInfo::prettyProductName();
    const QString name = host.isEmpty() || host == QLatin1String("localhost") ? os : host + QStringLiteral(" - ") + os;
    QString ascii;
    for (const QChar c : name)
        if (c.unicode() >= 0x20 && c.unicode() < 0x7f)
            ascii += c;
    return ascii.simplified().left(60);
}

void OnlineService::accountRequest(const QString &action, const QString &path, const QJsonObject &body)
{
    request("POST", path, body, [this, action](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit accountFailed(reply.value("error").toString(), error);
            return;
        }
        const QJsonObject user = reply.value("user").toObject();
        const QString token = reply.value("token").toString();
        if (!token.isEmpty()) {
            switchAccount(token, user);
        } else {
            m_login = user.value("login").toString();
            saveToken();
            emit accountChanged();
            refreshAccount();
        }
        if (reply.contains("recovery"))
            emit recoveryKeyShown(reply.value("recovery").toString());
        if (action == QLatin1String("signin"))
            emit notice(tr("Signed in as %1").arg(m_login), QStringLiteral("check"));
        emit accountDone(action);
    });
}

void OnlineService::createLogin(const QString &login, const QString &password)
{
    // With an account here it gets the login (and keeps everything); without one it's a new account.
    accountRequest(QStringLiteral("create"), QStringLiteral("/account"),
                   { { "login", login.trimmed() }, { "password", password }, { "name", m_name } });
}

void OnlineService::signIn(const QString &login, const QString &password)
{
    accountRequest(QStringLiteral("signin"), QStringLiteral("/account/login"),
                   { { "login", login.trimmed() }, { "password", password } });
}

void OnlineService::recoverAccount(const QString &login, const QString &recoveryKey, const QString &password)
{
    accountRequest(QStringLiteral("recover"), QStringLiteral("/account/recover"),
                   { { "login", login.trimmed() }, { "recovery", recoveryKey }, { "password", password } });
}

void OnlineService::changePassword(const QString &password, const QString &newPassword)
{
    request("POST", QStringLiteral("/account/password"), { { "password", password }, { "new", newPassword } },
            [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit accountFailed(reply.value("error").toString(), error);
            return;
        }
        const int others = reply.value("signedOut").toInt();
        emit notice(others > 0 ? tr("Password changed — %n other device(s) signed out", nullptr, others)
                               : tr("Password changed"), QStringLiteral("check"));
        emit accountDone(QStringLiteral("password"));
        refreshAccount();
    });
}

void OnlineService::newRecoveryKey(const QString &password)
{
    request("POST", QStringLiteral("/account/recovery"), { { "password", password } },
            [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty()) {
            emit accountFailed(reply.value("error").toString(), error);
            return;
        }
        emit recoveryKeyShown(reply.value("recovery").toString());
        emit accountDone(QStringLiteral("recovery"));
    });
}

void OnlineService::refreshAccount()
{
    if (!registered())
        return;
    request("GET", QStringLiteral("/me"), {}, [this](const QJsonObject &reply, const QString &error) {
        if (!error.isEmpty())
            return;
        const QString login = reply.value("user").toObject().value("login").toString();
        if (login != m_login) {
            m_login = login;
            saveToken();
            emit accountChanged();
        }
        if (m_login.isEmpty()) {
            m_devices.clear();
            emit devicesChanged();
            return;
        }
        request("GET", QStringLiteral("/account/devices"), {}, [this](const QJsonObject &reply, const QString &error) {
            if (!error.isEmpty())
                return;
            m_devices = reply.value("devices").toArray().toVariantList();
            emit devicesChanged();
        });
    });
}

void OnlineService::signOutDevice(const QString &id)
{
    request("DELETE", QStringLiteral("/account/devices/") + id, {}, [this](const QJsonObject &, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("Couldn't sign the device out: %1").arg(error), QStringLiteral("close"));
            return;
        }
        emit notice(tr("Device signed out"), QStringLiteral("check"));
        refreshAccount();
    });
}

void OnlineService::signOut()
{
    if (!registered() || m_login.isEmpty())
        return;   // an account with no login would be lost
    request("POST", QStringLiteral("/account/logout"), {}, [this](const QJsonObject &, const QString &) {
        dropAccount();   // signed out here even if the server couldn't be told
        emit notice(tr("Signed out of your Everplay account"), QStringLiteral("check"));
    });
}

// ---- everplay:// links open Everplay ------------------------------------------------

void OnlineService::registerUrlScheme()
{
    if (qEnvironmentVariableIsSet("EVERPLAY_PROFILE"))
        return;   // test profiles shouldn't take the links over
#ifdef Q_OS_WIN
    // Per user, no admin rights needed; points at this executable, so it follows updates and moves.
    // Both schemes: nokturn:// is the one of the app's former name, still used by the server's link pages.
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString command = QStringLiteral("\"%1\" \"%2\"").arg(exe, QStringLiteral("%1"));
    for (const char *scheme : { "everplay", "nokturn" }) {
        QSettings key(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") + QLatin1String(scheme), QSettings::NativeFormat);
        if (key.value(QStringLiteral("shell/open/command/Default")).toString() == command)
            continue;
        key.setValue(QStringLiteral("Default"), QStringLiteral("URL:Everplay"));
        key.setValue(QStringLiteral("URL Protocol"), QString());
        key.setValue(QStringLiteral("DefaultIcon/Default"), QStringLiteral("\"%1\",0").arg(exe));
        key.setValue(QStringLiteral("shell/open/command/Default"), command);
    }
#elif defined(Q_OS_LINUX)
    // An AppImage has no installed .desktop file: a hidden one for the links points at it.
    // (Installed from source, packaging/linux/everplay.desktop handles the links itself.)
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (appImage.isEmpty())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    const QString file = dir + QStringLiteral("/everplay-links.desktop");
    const QString exec = QString(appImage).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    const QByteArray content = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=Everplay\nNoDisplay=true\nIcon=everplay\n"
        "Exec=\"%1\" %u\nMimeType=x-scheme-handler/everplay;x-scheme-handler/nokturn;\n").arg(exec).toUtf8();
    QFile current(file);
    if (current.open(QIODevice::ReadOnly) && current.readAll() == content)
        return;
    current.close();
    QDir().mkpath(dir);
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.write(content);
    if (!out.commit())
        return;
    QProcess::startDetached(QStringLiteral("xdg-mime"),
                            { QStringLiteral("default"), QStringLiteral("everplay-links.desktop"),
                              QStringLiteral("x-scheme-handler/everplay"), QStringLiteral("x-scheme-handler/nokturn") });
    QProcess::startDetached(QStringLiteral("update-desktop-database"), { dir });
#endif
}
