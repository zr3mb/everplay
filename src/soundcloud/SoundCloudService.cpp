#include "SoundCloudService.h"

#include <QThreadPool>

#include "AppPaths.h"
#include "LibraryScanner.h"
#include "youtube/YouTubeService.h"
#include "youtube/YtCookies.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <memory>

namespace {

const char *kApi = "https://api-v2.soundcloud.com";
const char *kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                         "Chrome/131.0.0.0 Safari/537.36";
constexpr int kMaxLikes = 2000;

QString tokenFile() { return AppPaths::dataDir() + QStringLiteral("/soundcloud.token"); }
QString cacheFile() { return AppPaths::dataDir() + QStringLiteral("/soundcloud.json"); }
QString browserProfile() { return AppPaths::dataDir() + QStringLiteral("/browser-soundcloud"); }

QString numericId(const QString &trackId) { return trackId.mid(3); }   // "sc:123" -> "123"

// The token is a JWT: its "exp" (seconds since the epoch), 0 when it can't be read.
qint64 tokenExpiry(const QString &token)
{
    const QStringList parts = token.split(QChar('.'));
    if (parts.size() < 2)
        return 0;
    const QByteArray payload = QByteArray::fromBase64(parts.at(1).toLatin1(),
                                                      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QJsonDocument::fromJson(payload).object().value("exp").toVariant().toLongLong();
}

QString idOf(const QJsonValue &v)
{
    const qint64 n = v.toVariant().toLongLong();
    return n > 0 ? QString::number(n) : QString();
}

// Artwork comes as "-large" (100 px); "-t500x500" is the same image in 500 px.
QString bigArtwork(QString url)
{
    return url.replace(QLatin1String("-large."), QLatin1String("-t500x500."));
}

// The stream variants, best first: AAC over HLS, then Opus, then MP3; never the preview of a full track.
int transcodingRank(const QJsonObject &t)
{
    const QJsonObject format = t.value("format").toObject();
    const QString protocol = format.value("protocol").toString();
    const QString mime = format.value("mime_type").toString();
    const QString preset = t.value("preset").toString();
    int rank;
    if (protocol == QLatin1String("hls") && mime.contains(QLatin1String("mp4")))
        rank = 0;
    else if (protocol == QLatin1String("hls") && mime.contains(QLatin1String("ogg")))
        rank = 10;
    else if (protocol == QLatin1String("progressive"))
        rank = 20;
    else if (protocol == QLatin1String("hls") && mime == QLatin1String("audio/mpeg"))
        rank = 30;
    else if (protocol == QLatin1String("hls"))
        rank = 40;   // adaptive ("abr") playlists
    else
        return -1;
    if (preset.contains(QLatin1String("160")))
        --rank;      // the higher bitrate (SoundCloud Go+)
    return rank;
}

QJsonObject toJson(const Track &t, bool preview)
{
    return {
        { "id", numericId(t.id) }, { "url", t.path }, { "title", t.title }, { "artist", t.artist }, { "album", t.album },
        { "genre", t.genre }, { "year", t.year }, { "duration", t.duration }, { "cover", t.cover }, { "preview", preview },
        { "colorA", t.colorA.name() }, { "colorB", t.colorB.name() }, { "user", t.artistId.mid(4) },
    };
}

QString qualityText(bool preview)
{
    return preview ? QCoreApplication::translate("SoundCloudService", "SoundCloud · 30 s preview")
                   : QCoreApplication::translate("SoundCloudService", "SoundCloud · stream");
}

Track fromJson(const QJsonObject &o)
{
    Track t;
    t.id = QStringLiteral("sc:") + o.value("id").toString();
    t.path = o.value("url").toString();
    t.title = o.value("title").toString();
    t.artist = o.value("artist").toString();
    t.album = o.value("album").toString();
    if (const QString user = o.value("user").toString(); !user.isEmpty())
        t.artistId = QStringLiteral("scu:") + user;
    t.genre = o.value("genre").toString();
    t.year = o.value("year").toInt();
    t.duration = o.value("duration").toInt();
    t.cover = o.value("cover").toString();
    t.colorA = QColor(o.value("colorA").toString());
    t.colorB = QColor(o.value("colorB").toString());
    t.seed = Scanner::stableSeed(t.id);
    t.source = QStringLiteral("soundcloud");
    t.quality = qualityText(o.value("preview").toBool());
    return t;
}

QJsonArray toArray(const QStringList &list)
{
    QJsonArray arr;
    for (const QString &s : list)
        arr.append(s);
    return arr;
}

QStringList fromArray(const QJsonArray &arr)
{
    QStringList out;
    for (const QJsonValue &v : arr)
        out << v.toString();
    return out;
}

} // namespace

SoundCloudService *SoundCloudService::instance()
{
    static SoundCloudService *inst = new SoundCloudService;
    return inst;
}

SoundCloudService *SoundCloudService::create(QQmlEngine *, QJSEngine *)
{
    SoundCloudService *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

SoundCloudService::SoundCloudService(QObject *parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, [this] { writeCache(false); });
    // A save still waiting when the app quits is written before it ends.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_saveTimer.isActive())
            writeCache(true);
        QThreadPool::globalInstance()->waitForDone(3000);
    });

    m_login.setSite(BrowserLogin::Site::SoundCloud);
    connect(&m_login, &BrowserLogin::runningChanged, this, &SoundCloudService::loggingInChanged);
    connect(&m_login, &BrowserLogin::statusChanged, this, &SoundCloudService::loggingInChanged);
    connect(&m_login, &BrowserLogin::succeeded, this, [this](const YtCookies &cookies) {
        const QString token = cookies.value(QStringLiteral("oauth_token"));
        if (!m_renewing) {
            useToken(token, true);
            return;
        }
        m_renewing = false;
        m_token = token;
        m_tokenSuspect = m_tokenDead = false;
        saveToken();
        for (const auto &next : std::exchange(m_waitingForToken, {}))
            next();
    });
    connect(&m_login, &BrowserLogin::failed, this, [this](const QString &reason) {
        if (!m_renewing) {
            emit notice(reason, QStringLiteral("close"));
            return;
        }
        // Without a fresh session the account is left out; playback and search go on without it.
        m_renewing = false;
        m_tokenDead = true;
        emit notice(tr("The SoundCloud sign-in has expired — sign in again in Sources (playback works without it)"),
                    QStringLiteral("wavecloud"));
        for (const auto &next : std::exchange(m_waitingForToken, {}))
            next();
    });

    // "sc:" streams come from here, through the same resolver (cache, signals) as YouTube's.
    YouTubeService::instance()->resolver()->setExternal(QStringLiteral("sc:"),
                                                        [this](const QString &id, StreamResolver::Done done) {
        resolveStream(id, std::move(done));
    });

    m_clientId = AppPaths::settings()->value("soundcloud/clientId").toString();
    QFile f(tokenFile());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject saved = QJsonDocument::fromJson(unprotectSecret(f.readAll())).object();
        m_token = saved.value("token").toString();
    }
    if (connected()) {
        loadCache();
        // The library shows the cached copy at once; the account is synced a moment after startup.
        QTimer::singleShot(4000, this, &SoundCloudService::sync);
    }
}

QString SoundCloudService::lastSync() const
{
    if (!m_lastSync.isValid())
        return {};
    const qint64 mins = m_lastSync.secsTo(QDateTime::currentDateTime()) / 60;
    if (mins < 1)
        return tr("just now");
    if (mins < 60)
        return tr("%1 min ago").arg(mins);
    return m_lastSync.toString(QStringLiteral("d.MM, HH:mm"));
}

// ---- signing in ----------------------------------------------------------------

void SoundCloudService::login()
{
    if (BrowserLogin::findBrowser().isEmpty()) {
        emit notice(tr("Chrome, Edge or Brave was not found — paste the oauth_token cookie instead"),
                    QStringLiteral("close"));
        return;
    }
    m_login.start(browserProfile());
}

void SoundCloudService::cancelLogin()
{
    m_login.cancel();
}

void SoundCloudService::loginWithToken(const QString &token)
{
    QString t = token.trimmed();
    if (t.startsWith(QLatin1String("oauth_token="), Qt::CaseInsensitive))
        t = t.mid(12);
    if (t.startsWith(QLatin1String("OAuth "), Qt::CaseInsensitive))
        t = t.mid(6);
    if (t.size() < 10 || t.contains(QChar(' '))) {
        emit notice(tr("That doesn't look like a SoundCloud oauth_token"), QStringLiteral("close"));
        return;
    }
    useToken(t, true);
}

bool SoundCloudService::tokenUsable() const
{
    if (m_token.isEmpty() || m_tokenDead || m_tokenSuspect)
        return false;
    const qint64 expires = tokenExpiry(m_token);
    return expires == 0 || expires > QDateTime::currentSecsSinceEpoch() + 60;
}

void SoundCloudService::withToken(std::function<void()> next)
{
    // Signed out, a good token, or given up on: go ahead (with the account or without it).
    // A token renewed in the last 10 minutes that the API still refuses isn't renewed again right away.
    const bool recentlyRenewed = QDateTime::currentSecsSinceEpoch() - m_lastRenew < 600;
    if (m_token.isEmpty() || m_tokenDead || tokenUsable() || (recentlyRenewed && !m_renewing)) {
        next();
        return;
    }
    m_waitingForToken << std::move(next);
    renewSession();
}

void SoundCloudService::renewSession()
{
    if (m_renewing)
        return;
    if (m_login.running()) {   // the sign-in window is open — its result will do
        m_tokenDead = true;
        for (const auto &next : std::exchange(m_waitingForToken, {}))
            next();
        return;
    }
    m_renewing = true;
    m_lastRenew = QDateTime::currentSecsSinceEpoch();
    const QString old = m_token;
    m_login.refresh(browserProfile(), [old](const YtCookies &cookies) {
        const QString token = cookies.value(QStringLiteral("oauth_token"));
        const qint64 expires = tokenExpiry(token);
        return !token.isEmpty() && (token != old || expires == 0) && (expires == 0 || expires > QDateTime::currentSecsSinceEpoch() + 60);
    });
}

void SoundCloudService::useToken(const QString &token, bool fresh)
{
    if (token.isEmpty())
        return;
    m_token = token;
    m_tokenSuspect = m_tokenDead = false;
    saveToken();
    emit accountChanged();
    if (fresh)
        emit notice(tr("Signed in to SoundCloud — syncing your likes and playlists"), QStringLiteral("check"));
    sync();
}

void SoundCloudService::logout()
{
    m_login.cancel();
    m_token.clear();
    m_tokenSuspect = m_tokenDead = false;
    QFile::remove(tokenFile());
    m_saveTimer.stop();
    QThreadPool::globalInstance()->waitForDone(3000);   // a write on its way would bring the file back
    QFile::remove(cacheFile());
    QDir(browserProfile()).removeRecursively();   // the session in the browser profile goes too
    clearAccountData();
    emit accountChanged();
    emit dataChanged();
}

void SoundCloudService::clearAccountData()
{
    m_userId = 0;
    m_accountName.clear();
    m_accountPhoto.clear();
    m_liked.clear();
    m_likedSet.clear();
    m_playlists.clear();
    m_playlistTracks.clear();
    m_refreshedPlaylists.clear();
    m_lastSync = {};
    // Only what search / history still shows stays.
    QSet<QString> keep;
    for (const QStringList *ids : { &m_search, &m_history })
        for (const QString &id : *ids)
            keep.insert(id);
    for (const QStringList &ids : std::as_const(m_collections))
        for (const QString &id : ids)
            keep.insert(id);
    for (auto it = m_tracks.begin(); it != m_tracks.end();) {
        if (keep.contains(it.key())) {
            it->transient = true;
            ++it;
        } else {
            it = m_tracks.erase(it);
        }
    }
}

void SoundCloudService::saveToken() const
{
    const QByteArray json = QJsonDocument(QJsonObject { { "token", m_token } }).toJson(QJsonDocument::Compact);
    QSaveFile f(tokenFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(protectSecret(json));
        f.commit();
    }
}

// ---- the API -------------------------------------------------------------------

void SoundCloudService::withClientId(std::function<void(const QString &error)> next)
{
    if (!m_clientId.isEmpty()) {
        next({});
        return;
    }
    m_waitingForClientId << std::move(next);
    if (m_fetchingClientId)
        return;
    m_fetchingClientId = true;

    auto finish = [this](const QString &error) {
        m_fetchingClientId = false;
        if (!m_clientId.isEmpty())
            AppPaths::settings()->setValue("soundcloud/clientId", m_clientId);
        const auto waiting = std::exchange(m_waitingForClientId, {});
        for (const auto &w : waiting)
            w(error);
    };
    // The website's scripts carry the client_id — usually the last of them.
    QNetworkRequest request(QUrl(QStringLiteral("https://soundcloud.com/")));
    request.setRawHeader("User-Agent", kUserAgent);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, finish] {
        reply->deleteLater();
        const QString html = QString::fromUtf8(reply->readAll());
        QStringList scripts;
        static const QRegularExpression scriptRe(QStringLiteral(R"(<script[^>]+src="(https://a-v2\.sndcdn\.com/assets/[^"]+\.js)\")"));
        for (auto it = scriptRe.globalMatch(html); it.hasNext();)
            scripts.prepend(it.next().captured(1));
        if (scripts.isEmpty()) {
            finish(tr("SoundCloud isn't responding"));
            return;
        }
        auto tryNext = std::make_shared<std::function<void(QStringList)>>();
        *tryNext = [this, finish, tryNext](QStringList left) {
            if (left.isEmpty()) {
                finish(tr("Couldn't connect to SoundCloud (no client_id)"));
                return;
            }
            QNetworkRequest r(QUrl(left.takeFirst()));
            r.setRawHeader("User-Agent", kUserAgent);
            QNetworkReply *js = m_net.get(r);
            connect(js, &QNetworkReply::finished, this, [this, js, left, finish, tryNext] {
                js->deleteLater();
                static const QRegularExpression idRe(QStringLiteral(R"(client_id\s*[:=]\s*"([A-Za-z0-9]{20,})\")"));
                const QRegularExpressionMatch m = idRe.match(QString::fromUtf8(js->readAll()));
                if (m.hasMatch()) {
                    m_clientId = m.captured(1);
                    finish({});
                    return;
                }
                (*tryNext)(left);
            });
        };
        (*tryNext)(scripts);
    });
}

void SoundCloudService::get(const QString &pathOrUrl, const QUrlQuery &query, JsonCallback callback, int attempt)
{
    withClientId([this, pathOrUrl, query, callback, attempt](const QString &error) {
        if (!error.isEmpty()) {
            callback({}, error);
            return;
        }
        withToken([this, pathOrUrl, query, callback, attempt] {
            QUrl url(pathOrUrl.startsWith(QLatin1String("http")) ? pathOrUrl : QString::fromLatin1(kApi) + pathOrUrl);
            QUrlQuery q(url);
            for (const auto &item : query.queryItems(QUrl::FullyDecoded))
                q.addQueryItem(item.first, item.second);
            q.removeAllQueryItems(QStringLiteral("client_id"));
            q.addQueryItem(QStringLiteral("client_id"), m_clientId);
            url.setQuery(q);

            QNetworkRequest request(url);
            request.setRawHeader("User-Agent", kUserAgent);
            request.setRawHeader("Accept", "application/json");
            const bool authed = tokenUsable();
            if (authed)
                request.setRawHeader("Authorization", "OAuth " + m_token.toUtf8());
            QNetworkReply *reply = m_net.get(request);
            connect(reply, &QNetworkReply::finished, this, [this, reply, pathOrUrl, query, callback, attempt, authed] {
                reply->deleteLater();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 401 && authed && attempt < 2) {
                    // The account's token was refused: renew it (or go on without it) and try again.
                    m_tokenSuspect = true;
                    get(pathOrUrl, query, callback, attempt + 1);
                    return;
                }
                if ((status == 401 || status == 403) && attempt == 0) {
                    // The client_id changes now and then — read it from the site again and retry once.
                    m_clientId.clear();
                    get(pathOrUrl, query, callback, attempt + 1);
                    return;
                }
                if (status == 401 && !m_token.isEmpty()) {
                    callback({}, tr("the sign-in has expired — sign in to SoundCloud again"));
                    return;
                }
                if (status == 429) {
                    callback({}, tr("too many requests — try again in a moment"));
                    return;
                }
                if (reply->error() != QNetworkReply::NoError) {
                    callback({}, status == 404 ? tr("not found") : reply->errorString());
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
                callback(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), {});
            });
        });
    });
}

void SoundCloudService::send(const QByteArray &verb, const QString &path, std::function<void(const QString &error)> done)
{
    withClientId([this, verb, path, done](const QString &error) {
        if (!error.isEmpty()) {
            done(error);
            return;
        }
        withToken([this, verb, path, done] {
            if (!tokenUsable()) {
                done(tr("the sign-in has expired — sign in to SoundCloud again"));
                return;
            }
            QUrl url(QString::fromLatin1(kApi) + path);
            url.setQuery(QUrlQuery { { QStringLiteral("client_id"), m_clientId } });
            QNetworkRequest request(url);
            request.setRawHeader("User-Agent", kUserAgent);
            request.setRawHeader("Authorization", "OAuth " + m_token.toUtf8());
            QNetworkReply *reply = m_net.sendCustomRequest(request, verb);
            connect(reply, &QNetworkReply::finished, this, [reply, done] {
                reply->deleteLater();
                done(reply->error() == QNetworkReply::NoError ? QString() : reply->errorString());
            });
        });
    });
}

void SoundCloudService::getPages(const QString &path, QUrlQuery query, int limit, QJsonArray collected,
                                 std::function<void(const QJsonArray &items, const QString &error)> done)
{
    get(path, query, [this, limit, collected, done](const QJsonValue &json, const QString &error) mutable {
        if (!error.isEmpty()) {
            done(collected, error);
            return;
        }
        const QJsonObject page = json.toObject();
        for (const QJsonValue &v : page.value("collection").toArray())
            collected.append(v);
        const QString next = page.value("next_href").toString();
        if (next.isEmpty() || collected.size() >= limit) {
            done(collected, {});
            return;
        }
        getPages(next, {}, limit, collected, done);
    });
}

// ---- tracks ---------------------------------------------------------------------

Track SoundCloudService::storeTrack(const QJsonObject &o, bool transient)
{
    const QString num = idOf(o.value("id"));
    if (num.isEmpty() || !o.contains("title") || o.value("policy").toString() == QLatin1String("BLOCK"))
        return {};

    QList<QPair<int, QString>> ranked;
    bool full = false;
    for (const QJsonValue &v : o.value("media").toObject().value("transcodings").toArray()) {
        const QJsonObject tc = v.toObject();
        const int rank = transcodingRank(tc);
        if (rank < 0)
            continue;
        full = full || !tc.value("snipped").toBool();
        ranked << qMakePair(rank + (tc.value("snipped").toBool() ? 100 : 0), tc.value("url").toString());
    }
    if (ranked.isEmpty())
        return {};
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    Media media;
    for (const auto &r : std::as_const(ranked))
        media.transcodings << r.second;
    media.authorization = o.value("track_authorization").toString();
    media.preview = !full || o.value("policy").toString() == QLatin1String("SNIP");

    const QJsonObject meta = o.value("publisher_metadata").toObject();
    const QJsonObject user = o.value("user").toObject();
    Track t;
    t.id = QStringLiteral("sc:") + num;
    t.path = o.value("permalink_url").toString();
    t.title = o.value("title").toString().trimmed();
    t.artist = meta.value("artist").toString().trimmed();
    if (t.artist.isEmpty())
        t.artist = user.value("username").toString().trimmed();
    t.album = meta.value("album_title").toString().trimmed();
    // The uploader's profile (ProfilePage): "scu:<user id>" where YouTube tracks hold a channel id.
    if (const QString uploader = idOf(user.value("id")); !uploader.isEmpty())
        t.artistId = QStringLiteral("scu:") + uploader;
    t.genre = o.value("genre").toString().trimmed();
    t.year = QDateTime::fromString(o.value("release_date").toString(o.value("created_at").toString()), Qt::ISODate).date().year();
    t.duration = int(o.value("duration").toDouble() / 1000);
    QString cover = o.value("artwork_url").toString();
    if (cover.isEmpty())
        cover = user.value("avatar_url").toString();
    t.cover = bigArtwork(cover);
    t.source = QStringLiteral("soundcloud");
    t.quality = qualityText(media.preview);
    t.seed = Scanner::stableSeed(t.id);

    const auto existing = m_tracks.constFind(t.id);
    if (existing != m_tracks.cend()) {
        // Keep the colors computed from the cover and the track's "permanence" in the library.
        t.colorA = existing->colorA;
        t.colorB = existing->colorB;
        t.transient = existing->transient && transient;
    } else {
        const auto [a, b] = Scanner::fallbackPalette(t.id);
        t.colorA = a;
        t.colorB = b;
        t.transient = transient;
    }
    m_tracks.insert(t.id, t);
    m_media.insert(t.id, media);
    return t;
}

QVariantMap SoundCloudService::playlistInfo(const QJsonObject &o) const
{
    const QJsonObject user = o.value("user").toObject();
    QString cover = o.value("artwork_url").toString();
    if (cover.isEmpty()) {
        const QJsonArray tracks = o.value("tracks").toArray();
        if (!tracks.isEmpty())
            cover = tracks.first().toObject().value("artwork_url").toString();
    }
    const int count = o.value("track_count").toInt();
    return {
        { "id", idOf(o.value("id")) },
        { "title", o.value("title").toString() },
        { "subtitle", user.value("username").toString() },
        { "cover", bigArtwork(cover) },
        { "count", count },
    };
}

QList<Track> SoundCloudService::tracks() const
{
    // Order: liked, tracks from playlists, search results, history, opened playlists.
    QList<Track> out;
    QSet<QString> seen;
    auto add = [&](const QString &id) {
        if (seen.contains(id))
            return;
        const auto it = m_tracks.constFind(id);
        if (it == m_tracks.cend())
            return;
        seen.insert(id);
        Track t = *it;
        t.liked = m_likedSet.contains(id);
        out << t;
    };
    for (const QString &id : m_liked)
        add(id);
    for (const QStringList &ids : m_playlistTracks)
        for (const QString &id : ids)
            add(id);
    for (const QString &id : m_search)
        add(id);
    for (const QString &id : m_history)
        add(id);
    for (const QStringList &ids : m_collections)
        for (const QString &id : ids)
            add(id);
    for (const QStringList &ids : m_profileTracks)
        for (const QString &id : ids)
            add(id);
    return out;
}

void SoundCloudService::publishResults(const QStringList &ids)
{
    // Only the tracks the model doesn't have yet are appended (Library::appendTracks), like YouTube's.
    QList<Track> added;
    for (const QString &id : ids) {
        const auto it = m_tracks.constFind(id);
        if (it == m_tracks.cend())
            continue;
        Track t = *it;
        t.liked = m_likedSet.contains(id);
        added << t;
    }
    if (!added.isEmpty())
        emit tracksAdded(added);
    ++m_resultsRevision;
    emit resultsChanged();
}

void SoundCloudService::setHistory(const QList<Track> &tracks)
{
    m_history.clear();
    for (Track t : tracks) {
        if (!m_tracks.contains(t.id)) {
            t.transient = true;
            m_tracks.insert(t.id, t);
        }
        m_history << t.id;
    }
    publishResults(m_history);
}

void SoundCloudService::setTrackColors(const QString &trackId, const QColor &a, const QColor &b)
{
    auto it = m_tracks.find(trackId);
    if (it == m_tracks.end())
        return;
    it->colorA = a;
    it->colorB = b;
}

void SoundCloudService::setTrackLiked(const QString &trackId, bool liked)
{
    if (!connected() || liked == m_likedSet.contains(trackId))
        return;
    if (liked) {
        m_liked.prepend(trackId);
        m_likedSet.insert(trackId);
        if (m_tracks.contains(trackId))
            m_tracks[trackId].transient = false;
    } else {
        m_liked.removeAll(trackId);
        m_likedSet.remove(trackId);
    }
    saveCache();
    if (m_userId <= 0)
        return;
    // The heart goes to the account too.
    send(liked ? "PUT" : "DELETE", QStringLiteral("/users/%1/track_likes/%2").arg(m_userId).arg(numericId(trackId)),
         [this](const QString &error) {
        if (!error.isEmpty())
            emit notice(tr("SoundCloud didn't save the like: %1").arg(error), QStringLiteral("close"));
    });
}

void SoundCloudService::resolveStream(const QString &trackId, std::function<void(const QUrl &url, const QString &error)> done)
{
    const auto media = m_media.constFind(trackId);
    if (media == m_media.cend() || media->transcodings.isEmpty()) {
        // Tracks from the cache (or the history) come without their stream variants — fetch the track first.
        get(QStringLiteral("/tracks/") + numericId(trackId), {}, [this, trackId, done](const QJsonValue &json, const QString &error) {
            if (!error.isEmpty()) {
                done({}, tr("SoundCloud: %1").arg(error));
                return;
            }
            const bool transient = !m_tracks.contains(trackId) || m_tracks.value(trackId).transient;
            if (storeTrack(json.toObject(), transient).id.isEmpty()) {
                done({}, tr("This track can't be played in your country or has been removed from SoundCloud"));
                return;
            }
            resolveStream(trackId, done);
        });
        return;
    }
    if (media->preview)
        emit notice(tr("Only a 30-second preview — the full track needs SoundCloud Go+"), QStringLiteral("wavecloud"));
    QUrlQuery query;
    if (!media->authorization.isEmpty())
        query.addQueryItem(QStringLiteral("track_authorization"), media->authorization);
    get(media->transcodings.first(), query, [this, trackId, done](const QJsonValue &json, const QString &error) {
        const QString url = json.toObject().value("url").toString();
        if (!error.isEmpty() || url.isEmpty()) {
            m_media.remove(trackId);   // the authorization may have expired — the next try fetches the track again
            done({}, tr("SoundCloud: %1").arg(error.isEmpty() ? tr("no stream") : error));
            return;
        }
        done(QUrl(url), {});
    });
}

void SoundCloudService::fetchDescription(const QString &trackId, std::function<void(const QString &)> done)
{
    get(QStringLiteral("/tracks/") + numericId(trackId), {}, [done](const QJsonValue &json, const QString &error) {
        done(error.isEmpty() ? json.toObject().value("description").toString() : QString());
    });
}

// ---- search ---------------------------------------------------------------------

void SoundCloudService::search(const QString &query)
{
    const QString q = query.trimmed();
    const quint64 serial = ++m_searchSerial;
    if (q.isEmpty()) {
        m_search.clear();
        m_searching = false;
        emit searchingChanged();
        publishResults({});
        m_playlistResults.clear();
        emit playlistResultsChanged();
        m_userResults.clear();
        emit userResultsChanged();
        return;
    }
    m_searching = true;
    emit searchingChanged();
    get(QStringLiteral("/search/playlists_without_albums"), QUrlQuery { { "q", q }, { "limit", "10" } },
        [this, serial](const QJsonValue &json, const QString &) {
        if (serial != m_searchSerial)
            return;
        m_playlistResults.clear();
        for (const QJsonValue &v : json.toObject().value("collection").toArray())
            if (v.toObject().value("track_count").toInt() > 0)
                m_playlistResults << playlistInfo(v.toObject());
        emit playlistResultsChanged();
    });
    get(QStringLiteral("/search/users"), QUrlQuery { { "q", q }, { "limit", "8" } },
        [this, serial](const QJsonValue &json, const QString &) {
        if (serial != m_searchSerial)
            return;
        m_userResults.clear();
        for (const QJsonValue &v : json.toObject().value("collection").toArray()) {
            const QVariantMap user = userInfo(v.toObject());
            if (!user.value("id").toString().isEmpty())
                m_userResults << user;
        }
        emit userResultsChanged();
    });
    get(QStringLiteral("/search/tracks"), QUrlQuery { { "q", q }, { "limit", "30" } },
        [this, serial](const QJsonValue &json, const QString &error) {
        if (serial != m_searchSerial)
            return;   // a newer query has already arrived
        m_searching = false;
        emit searchingChanged();
        if (!error.isEmpty()) {
            emit notice(tr("SoundCloud search: %1").arg(error), QStringLiteral("close"));
            return;
        }
        m_search.clear();
        for (const QJsonValue &v : json.toObject().value("collection").toArray()) {
            // Previews only (SoundCloud Go+ tracks) aren't worth offering in search.
            if (v.toObject().value("policy").toString() == QLatin1String("SNIP"))
                continue;
            const Track t = storeTrack(v.toObject(), true);
            if (!t.id.isEmpty())
                m_search << t.id;
        }
        publishResults(m_search);
    });
}

// ---- profiles -------------------------------------------------------------------

QVariantMap SoundCloudService::userInfo(const QJsonObject &o)
{
    const int followers = o.value("followers_count").toInt();
    const int tracks = o.value("track_count").toInt();
    QStringList subtitle;
    if (followers > 0)
        subtitle << tr("%n followers", "", followers);
    if (tracks > 0)
        subtitle << tr("%n tracks", "", tracks);
    QString place = o.value("city").toString().trimmed();
    QString banner;
    for (const QJsonValue &v : o.value("visuals").toObject().value("visuals").toArray())
        if (banner.isEmpty())
            banner = v.toObject().value("visual_url").toString();
    return {
        { "id", idOf(o.value("id")) },
        { "name", o.value("username").toString().trimmed() },
        { "fullName", o.value("full_name").toString().trimmed() },
        { "subtitle", subtitle.join(QStringLiteral(" · ")) },
        { "photo", bigArtwork(o.value("avatar_url").toString()) },
        { "banner", banner },
        { "description", o.value("description").toString().trimmed() },
        { "place", place },
        { "followers", followers },
        { "trackCount", tracks },
        { "verified", o.value("verified").toBool() },
        { "url", o.value("permalink_url").toString() },
    };
}

void SoundCloudService::updateProfile(const QString &userId, const QVariantMap &fields)
{
    QVariantMap &p = m_profiles[userId];
    for (auto it = fields.cbegin(); it != fields.cend(); ++it)
        p.insert(it.key(), it.value());
    ++m_profilesRevision;
    emit profilesChanged();
}

void SoundCloudService::loadProfile(const QString &userId)
{
    if (userId.isEmpty() || m_profiles.contains(userId))
        return;
    // Five parts arrive on their own; the page shows each as it comes. "loading" is cleared by the last.
    auto pending = std::make_shared<int>(5);
    auto finished = [this, userId, pending] {
        if (--*pending == 0)
            updateProfile(userId, { { "loading", false } });
    };
    updateProfile(userId, { { "loading", true } });

    get(QStringLiteral("/users/") + userId, {}, [this, userId, finished](const QJsonValue &json, const QString &error) {
        if (!error.isEmpty())
            updateProfile(userId, { { "error", error } });
        else
            updateProfile(userId, userInfo(json.toObject()));
        finished();
    });
    for (const QString which : { QStringLiteral("popular"), QStringLiteral("tracks") }) {
        const QString path = QStringLiteral("/users/%1/%2").arg(userId, which == QLatin1String("popular") ? "toptracks" : "tracks");
        get(path, QUrlQuery { { "limit", which == QLatin1String("popular") ? "10" : "50" } },
            [this, userId, which, finished](const QJsonValue &json, const QString &) {
            QStringList ids;
            for (const QJsonValue &v : json.toObject().value("collection").toArray()) {
                const Track t = storeTrack(v.toObject(), true);
                if (!t.id.isEmpty())
                    ids << t.id;
            }
            m_profileTracks.insert(userId + QChar('/') + which, ids);
            publishResults(ids);
            ++m_profilesRevision;
            emit profilesChanged();
            finished();
        });
    }
    for (const QString which : { QStringLiteral("albums"), QStringLiteral("playlists") }) {
        const QString path = QStringLiteral("/users/%1/%2").arg(userId, which == QLatin1String("albums") ? "albums" : "playlists_without_albums");
        get(path, QUrlQuery { { "limit", "24" } }, [this, userId, which, finished](const QJsonValue &json, const QString &) {
            QVariantList lists;
            for (const QJsonValue &v : json.toObject().value("collection").toArray())
                if (v.toObject().value("track_count").toInt() > 0)
                    lists << playlistInfo(v.toObject());
            updateProfile(userId, { { which, lists } });
            finished();
        });
    }
}

// ---- playlists ------------------------------------------------------------------

void SoundCloudService::fillTracks(QStringList missing, bool transient, std::function<void()> done)
{
    // Long playlists list only the first tracks in full — the rest is fetched 50 at a time.
    if (missing.isEmpty()) {
        done();
        return;
    }
    const QStringList batch = missing.mid(0, 50);
    missing = missing.mid(50);
    get(QStringLiteral("/tracks"), QUrlQuery { { "ids", batch.join(QChar(',')) } },
        [this, missing, transient, done](const QJsonValue &json, const QString &) {
        for (const QJsonValue &v : json.toArray())
            storeTrack(v.toObject(), transient);
        fillTracks(missing, transient, done);
    });
}

void SoundCloudService::loadPlaylistTracks(const QString &playlistId, bool transient,
                                           std::function<void(const QStringList &ids, const QString &error)> done)
{
    m_loadingCollections.insert(playlistId);
    get(QStringLiteral("/playlists/") + playlistId, QUrlQuery { { "representation", "full" } },
        [this, playlistId, transient, done](const QJsonValue &json, const QString &error) {
        if (!error.isEmpty()) {
            m_loadingCollections.remove(playlistId);
            done({}, error);
            return;
        }
        QStringList order, missing;
        for (const QJsonValue &v : json.toObject().value("tracks").toArray()) {
            const QJsonObject o = v.toObject();
            const QString num = idOf(o.value("id"));
            if (num.isEmpty())
                continue;
            order << QStringLiteral("sc:") + num;
            if (storeTrack(o, transient).id.isEmpty() && !o.contains("title"))
                missing << num;
        }
        fillTracks(missing, transient, [this, playlistId, order, done] {
            m_loadingCollections.remove(playlistId);
            QStringList ids;
            for (const QString &id : order)
                if (m_tracks.contains(id))
                    ids << id;
            done(ids, {});
        });
    });
}

void SoundCloudService::ensurePlaylist(const QString &playlistId)
{
    if (m_loadingCollections.contains(playlistId) || m_refreshedPlaylists.contains(playlistId))
        return;
    m_refreshedPlaylists.insert(playlistId);
    loadPlaylistTracks(playlistId, false, [this, playlistId](const QStringList &ids, const QString &error) {
        if (!error.isEmpty()) {
            emit notice(tr("SoundCloud playlist: %1").arg(error), QStringLiteral("close"));
            emit dataChanged();
            return;
        }
        m_playlistTracks.insert(playlistId, ids);
        saveCache();
        emit dataChanged();
    });
}

void SoundCloudService::ensureCollection(const QString &playlistId)
{
    if (m_collections.contains(playlistId) || m_loadingCollections.contains(playlistId))
        return;
    loadPlaylistTracks(playlistId, true, [this, playlistId](const QStringList &ids, const QString &error) {
        if (!error.isEmpty())
            emit notice(tr("SoundCloud playlist: %1").arg(error), QStringLiteral("close"));
        m_collections.insert(playlistId, ids);
        publishResults(ids);
    });
}

// ---- syncing the account ----------------------------------------------------------

void SoundCloudService::refresh()
{
    m_refreshedPlaylists.clear();
    sync();
}

void SoundCloudService::sync()
{
    if (!connected() || m_syncing)
        return;
    m_syncing = true;
    emit syncingChanged();
    get(QStringLiteral("/me"), {}, [this](const QJsonValue &json, const QString &error) {
        const QJsonObject me = json.toObject();
        if (!error.isEmpty() || idOf(me.value("id")).isEmpty()) {
            finishSync(error.isEmpty() ? tr("couldn't read the account") : error);
            return;
        }
        m_userId = me.value("id").toVariant().toLongLong();
        m_accountName = me.value("username").toString();
        m_accountPhoto = bigArtwork(me.value("avatar_url").toString());
        emit accountChanged();

        const QString likes = QStringLiteral("/users/%1/track_likes").arg(m_userId);
        getPages(likes, QUrlQuery { { "limit", "200" }, { "linked_partitioning", "1" } }, kMaxLikes, {},
                 [this](const QJsonArray &items, const QString &error) {
            if (!error.isEmpty()) {
                finishSync(error);
                return;
            }
            QStringList liked;
            for (const QJsonValue &v : items) {
                const Track t = storeTrack(v.toObject().value("track").toObject(), false);
                if (!t.id.isEmpty())
                    liked << t.id;
            }
            m_liked = liked;
            m_likedSet = QSet<QString>(liked.cbegin(), liked.cend());

            const QString lists = QStringLiteral("/users/%1/playlists_without_albums").arg(m_userId);
            getPages(lists, QUrlQuery { { "limit", "50" }, { "linked_partitioning", "1" } }, 200, {},
                     [this](const QJsonArray &items, const QString &error) {
                if (!error.isEmpty()) {
                    finishSync(error);
                    return;
                }
                QVariantList playlists;
                QSet<QString> ids;
                for (const QJsonValue &v : items) {
                    const QVariantMap p = playlistInfo(v.toObject());
                    playlists << p;
                    ids.insert(p.value("id").toString());
                }
                m_playlists = playlists;
                for (auto it = m_playlistTracks.begin(); it != m_playlistTracks.end();)
                    it = ids.contains(it.key()) ? std::next(it) : m_playlistTracks.erase(it);
                finishSync({});
            });
        });
    });
}

void SoundCloudService::finishSync(const QString &error)
{
    m_syncing = false;
    emit syncingChanged();
    if (!error.isEmpty()) {
        emit notice(tr("SoundCloud sync: %1").arg(error), QStringLiteral("close"));
        emit dataChanged();
        return;
    }
    m_lastSync = QDateTime::currentDateTime();
    saveCache();
    emit dataChanged();
}

// ---- cache ------------------------------------------------------------------------

void SoundCloudService::loadCache()
{
    QFile file(cacheFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject account = root.value("account").toObject();
    m_userId = account.value("id").toVariant().toLongLong();
    m_accountName = account.value("name").toString();
    m_accountPhoto = account.value("photo").toString();
    for (const QJsonValue &v : root.value("tracks").toArray()) {
        const Track t = fromJson(v.toObject());
        m_tracks.insert(t.id, t);
    }
    m_liked = fromArray(root.value("liked").toArray());
    m_likedSet = QSet<QString>(m_liked.cbegin(), m_liked.cend());
    m_playlists = root.value("playlists").toArray().toVariantList();
    const QJsonObject lists = root.value("playlistTracks").toObject();
    for (auto it = lists.begin(); it != lists.end(); ++it)
        m_playlistTracks.insert(it.key(), fromArray(it.value().toArray()));
    m_lastSync = QDateTime::fromString(root.value("lastSync").toString(), Qt::ISODate);
}

void SoundCloudService::saveCache()
{
    m_saveTimer.start();
}

void SoundCloudService::writeCache(bool wait)
{
    m_saveTimer.stop();
    // Built and written from copies of the data (cheap, shared until changed) on a worker thread.
    auto job = [tracks = m_tracks, liked = m_liked, playlists = m_playlists, lists = m_playlistTracks,
                userId = m_userId, name = m_accountName, photo = m_accountPhoto, lastSync = m_lastSync,
                preview = qualityText(true), path = cacheFile()] {
        QJsonArray trackArray;
        for (const Track &t : tracks)
            if (!t.transient)
                trackArray.append(toJson(t, t.quality == preview));
        QJsonObject listObject;
        for (auto it = lists.cbegin(); it != lists.cend(); ++it)
            listObject.insert(it.key(), toArray(it.value()));
        const QJsonObject root {
            { "account", QJsonObject { { "id", double(userId) }, { "name", name }, { "photo", photo } } },
            { "tracks", trackArray },
            { "liked", toArray(liked) },
            { "playlists", QJsonArray::fromVariantList(playlists) },
            { "playlistTracks", listObject },
            { "lastSync", lastSync.toString(Qt::ISODate) },
        };
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
            file.commit();
        }
    };
    if (wait)
        job();
    else
        QThreadPool::globalInstance()->start(job);
}
