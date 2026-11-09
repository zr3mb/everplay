#include "YouTubeService.h"

#include <QThreadPool>

#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>

namespace {

QString cookieFile() { return AppPaths::dataDir() + QStringLiteral("/youtube.cookies"); }
QString cacheFile() { return AppPaths::dataDir() + QStringLiteral("/youtube.json"); }
QString browserProfile() { return AppPaths::dataDir() + QStringLiteral("/browser"); }

QJsonObject toJson(const Track &t)
{
    QJsonObject o {
        { "id", t.path }, { "title", t.title }, { "artist", t.artist }, { "artistId", t.artistId }, { "album", t.album },
        { "duration", t.duration }, { "cover", t.cover },
        { "colorA", t.colorA.name() }, { "colorB", t.colorB.name() }, { "seed", t.seed },
    };
    if (!t.source.isEmpty() && t.source != QLatin1String("youtube")) {
        o.insert("uid", t.id);
        o.insert("source", t.source);
        o.insert("quality", t.quality);
    }
    return o;
}

Track fromJson(const QJsonObject &o)
{
    Track t;
    t.path = o.value("id").toString();
    t.id = QStringLiteral("yt:") + t.path;
    t.title = o.value("title").toString();
    t.artist = o.value("artist").toString();
    t.artistId = o.value("artistId").toString();
    t.album = o.value("album").toString();
    t.duration = o.value("duration").toInt();
    t.cover = o.value("cover").toString();
    t.colorA = QColor(o.value("colorA").toString());
    t.colorB = QColor(o.value("colorB").toString());
    t.seed = o.value("seed").toInt();
    t.source = QStringLiteral("youtube");
    t.quality = QCoreApplication::translate("YouTubeService", "YouTube Music · stream");
    if (o.contains("uid")) {
        t.id = o.value("uid").toString();
        t.source = o.value("source").toString();
        t.quality = o.value("quality").toString();
    }
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

// Search text without case and diacritics ("Zayebały" = "zayebaly"), split into words.
QString foldText(const QString &text)
{
    QString out;
    for (const QChar c : text.toLower().normalized(QString::NormalizationForm_D)) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        out += c == QChar(0x0142) ? QChar('l') : c.isLetterOrNumber() ? c : QChar(' ');
    }
    return out;
}

QStringList matchWords(const QString &query)
{
    return foldText(query).split(QChar(' '), Qt::SkipEmptyParts);
}

// How well a result fits what was typed: the words found in its title and artist, the title most.
int matchScore(const Track &t, const QStringList &words)
{
    if (words.isEmpty())
        return 0;
    const QString title = foldText(t.title);
    const QString artist = foldText(t.artist);
    int points = 0, found = 0;
    for (const QString &w : words) {
        const int p = title.contains(w) ? 2 : artist.contains(w) ? 1 : 0;
        points += p;
        found += p > 0;
    }
    // All words there: it's what was looked for, ahead of results that only share a word.
    return found == int(words.size()) ? 100 + points : points;
}

} // namespace

QJsonObject YouTubeService::trackJson(const Track &t) { return toJson(t); }
Track YouTubeService::trackFromJson(const QJsonObject &o) { return fromJson(o); }

YouTubeService *YouTubeService::instance()
{
    static YouTubeService *inst = new YouTubeService;
    return inst;
}

YouTubeService *YouTubeService::create(QQmlEngine *, QJSEngine *)
{
    YouTubeService *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

YouTubeService::YouTubeService(QObject *parent)
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
    BrowserLogin::findBrowser(&m_browserName);

    connect(&m_login, &BrowserLogin::runningChanged, this, &YouTubeService::loggingInChanged);
    connect(&m_login, &BrowserLogin::statusChanged, this, &YouTubeService::loggingInChanged);
    connect(&m_login, &BrowserLogin::succeeded, this, [this](const YtCookies &cookies) {
        useCookies(cookies, true);
    });
    connect(&m_login, &BrowserLogin::failed, this, [this](const QString &reason) {
        emit notice(reason, QStringLiteral("close"));
    });

    connect(&m_resolver, &StreamResolver::engineChanged, this, &YouTubeService::engineChanged);
    connect(&m_resolver, &StreamResolver::notice, this, &YouTubeService::notice);
    m_resolver.detect();

    m_authUser = AppPaths::settings()->value("youtube/authUser", 0).toInt();
    m_pageId = AppPaths::settings()->value("youtube/pageId").toString();
    m_api.setIdentity(m_authUser, m_pageId);
    const YtCookies saved = YtCookies::load(cookieFile());
    if (saved.isLoggedIn()) {
        loadCache();
        useCookies(saved, false);
    }
}

// ---- signing in ----------------------------------------------------------------

void YouTubeService::login()
{
    if (m_browserName.isEmpty()) {
        emit notice(tr("Chrome, Edge or Brave was not found — paste the cookies manually"),
                    QStringLiteral("close"));
        return;
    }
    m_login.start(browserProfile());
}

void YouTubeService::cancelLogin()
{
    m_login.cancel();
}

void YouTubeService::loginWithCookies(const QString &cookieHeader)
{
    const YtCookies cookies = YtCookies::fromHeader(cookieHeader);
    if (!cookies.isLoggedIn()) {
        emit notice(QStringLiteral("W ciasteczkach brakuje SAPISID lub LOGIN_INFO"), QStringLiteral("key"));
        return;
    }
    useCookies(cookies, true);
}

void YouTubeService::useCookies(const YtCookies &cookies, bool fresh)
{
    m_cookies = cookies;
    m_api.setCookies(cookies);
    m_resolver.setCookies(cookies);
    if (fresh) {
        // A new session: start from its first account; the others can be picked once they're listed.
        m_authUser = 0;
        m_pageId.clear();
        m_api.setIdentity(0, {});
        AppPaths::settings()->remove("youtube/authUser");
        AppPaths::settings()->remove("youtube/pageId");
        m_identities.clear();
        if (!cookies.save(cookieFile()))
            emit notice(tr("Couldn't save the session to disk"), QStringLiteral("close"));
        emit notice(tr("Signed in to YouTube Music"), QStringLiteral("check"));
    }
    emit accountChanged();
    emit dataChanged();
    // On app startup give the interface a moment to load; after signing in, sync right away.
    QTimer::singleShot(fresh ? 0 : 1500, this, &YouTubeService::refresh);
}

void YouTubeService::logout()
{
    m_login.cancel();
    m_cookies = {};
    m_api.setCookies({});
    m_resolver.setCookies({});
    QFile::remove(cookieFile());
    m_saveTimer.stop();
    QThreadPool::globalInstance()->waitForDone(3000);   // a write on its way would bring the file back
    QFile::remove(cacheFile());
    QDir(browserProfile()).removeRecursively();   // the session in the browser profile goes too

    m_account = {};
    m_identities.clear();
    m_authUser = 0;
    m_pageId.clear();
    m_api.setIdentity(0, {});
    AppPaths::settings()->remove("youtube/authUser");
    AppPaths::settings()->remove("youtube/pageId");
    QList<Track> recommended;   // they don't come from the account (nor does the history)
    for (const QStringList *ids : { &m_recommended, &m_history })
        for (const QString &id : *ids)
            if (const auto it = m_tracks.constFind(id); it != m_tracks.cend())
                recommended << *it;
    m_tracks.clear();
    storeTracks(recommended, true);
    clearAccountData();
    m_search.clear();
    emit accountChanged();
    emit dataChanged();
    emit notice(tr("Signed out of YouTube Music"), QStringLiteral("person"));
}

void YouTubeService::clearAccountData()
{
    m_liked.clear();
    m_likedSet.clear();
    m_playlists.clear();
    m_playlistTracks.clear();
    m_refreshedPlaylists.clear();
}

QVariantList YouTubeService::identities() const
{
    QVariantList out;
    for (const YtMusicApi::Identity &id : m_identities)
        out << QVariantMap { { "name", id.name }, { "handle", id.handle }, { "photo", id.photo }, { "email", id.email },
                             { "selected", id.authUser == m_authUser && id.pageId == m_pageId } };
    return out;
}

void YouTubeService::selectIdentity(int index)
{
    if (index < 0 || index >= m_identities.size())
        return;
    const YtMusicApi::Identity &id = m_identities.at(index);
    AppPaths::settings()->setValue("youtube/authUser", id.authUser);   // chosen, even if unchanged
    AppPaths::settings()->setValue("youtube/pageId", id.pageId);
    if (id.authUser == m_authUser && id.pageId == m_pageId)
        return;
    m_authUser = id.authUser;
    m_pageId = id.pageId;
    m_api.setIdentity(m_authUser, m_pageId);
    // Liked tracks and playlists belong to the channel — the other one's are fetched anew.
    m_account = { id.name, id.handle, id.photo };
    clearAccountData();
    emit accountChanged();
    emit dataChanged();
    refresh();
}

// ---- syncing ----------------------------------------------------------------------

void YouTubeService::refresh()
{
    if (!connected() || syncing())
        return;
    m_pendingSync = 4;
    m_syncError.clear();
    emit syncingChanged();

    m_api.fetchAccount([this](const YtMusicApi::Account &account, const QString &error) {
        if (error.isEmpty()) {
            m_account = account;
            emit accountChanged();
        }
        syncStep(error);
    });

    m_api.fetchIdentities([this](const QList<YtMusicApi::Identity> &identities, const QString &) {
        if (!identities.isEmpty()) {
            const bool first = m_identities.isEmpty();
            m_identities = identities;
            emit accountChanged();
            if (first && identities.size() > 1 && !AppPaths::settings()->contains("youtube/authUser"))
                emit notice(tr("Several YouTube channels found — pick yours in Sources"), QStringLiteral("person"));
        }
        syncStep({});
    });

    m_api.browseTracks(QStringLiteral("VLLM"), [this](const QList<Track> &tracks, const QString &error) {
        // An empty response for a non-empty list is usually an expired session, not un-liking everything.
        if (!tracks.isEmpty() || (error.isEmpty() && m_liked.isEmpty())) {
            storeTracks(tracks, false);
            QStringList ids;
            for (const Track &t : tracks)
                ids << t.id;
            setLikedList(ids);
        }
        syncStep(error);
    });

    m_api.fetchLibraryPlaylists([this](const QVariantList &playlists, const QString &error) {
        if (error.isEmpty()) {
            m_playlists = playlists;
            QSet<QString> ids;
            for (const QVariant &p : playlists)
                ids.insert(p.toMap().value("id").toString());
            for (auto it = m_playlistTracks.begin(); it != m_playlistTracks.end();)
                it = ids.contains(it.key()) ? std::next(it) : m_playlistTracks.erase(it);
            m_refreshedPlaylists.clear();
        }
        syncStep(error);
    });
}

void YouTubeService::syncStep(const QString &error)
{
    if (!error.isEmpty() && m_syncError.isEmpty())
        m_syncError = error;
    if (--m_pendingSync > 0)
        return;

    m_lastSync = QDateTime::currentDateTime();
    saveCache();
    emit syncingChanged();
    emit dataChanged();
    if (!m_syncError.isEmpty())
        emit notice(QStringLiteral("YouTube Music: ") + m_syncError, QStringLiteral("close"));
}

QString YouTubeService::lastSync() const
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

void YouTubeService::ensurePlaylist(const QString &playlistId)
{
    // Each playlist is refreshed once per session, in the background — until then the cached version shows.
    if (m_loadingPlaylists.contains(playlistId) || m_refreshedPlaylists.contains(playlistId))
        return;
    m_loadingPlaylists.insert(playlistId);
    m_refreshedPlaylists.insert(playlistId);
    // Sometimes called from a QML binding — we notify only in the next event-loop pass.
    QTimer::singleShot(0, this, &YouTubeService::dataChanged);

    m_api.browseTracks(QStringLiteral("VL") + playlistId, [this, playlistId](const QList<Track> &tracks, const QString &error) {
        m_loadingPlaylists.remove(playlistId);
        if (!error.isEmpty() && tracks.isEmpty()) {
            emit notice(tr("Couldn't load the playlist: %1").arg(error), QStringLiteral("close"));
            emit dataChanged();
            return;
        }
        storeTracks(tracks, false);
        QStringList ids;
        for (const Track &t : tracks)
            ids << t.id;
        m_playlistTracks.insert(playlistId, ids);
        saveCache();
        emit dataChanged();
    });
}

// ---- editing the account's playlists -----------------------------------------------

void YouTubeService::createPlaylist(const QString &title, const QString &description, const QString &privacy,
                                    const QList<Track> &tracks, std::function<void(const QString &, const QString &)> done)
{
    QStringList videoIds;
    for (const Track &t : tracks)
        videoIds << t.path;
    m_api.createPlaylist(title, description, privacy, videoIds, [this, title, tracks, done](const QString &id, const QString &error) {
        if (!error.isEmpty()) {
            done({}, error);
            return;
        }
        storeTracks(tracks, false);
        QStringList ids;
        for (const Track &t : tracks)
            if (!ids.contains(t.id))
                ids << t.id;
        m_playlists.prepend(QVariantMap { { "id", id }, { "title", title }, { "count", ids.size() },
                                          { "cover", tracks.isEmpty() ? QString() : tracks.first().cover } });
        m_playlistTracks.insert(id, ids);
        m_refreshedPlaylists.insert(id);
        saveCache();
        emit dataChanged();
        done(id, {});
    });
}

void YouTubeService::addToPlaylist(const QString &playlistId, const QList<Track> &tracks, ErrorCallback done)
{
    QJsonArray actions;
    for (const Track &t : tracks)
        actions.append(QJsonObject { { "action", "ACTION_ADD_VIDEO" }, { "addedVideoId", t.path },
                                     { "dedupeOption", "DEDUPE_OPTION_SKIP" } });
    m_api.editPlaylist(playlistId, actions, [this, playlistId, tracks, done](const QString &error) {
        if (error.isEmpty()) {
            storeTracks(tracks, false);
            QStringList &ids = m_playlistTracks[playlistId];
            for (const Track &t : tracks)
                if (!ids.contains(t.id))
                    ids << t.id;
            updatePlaylistInfo(playlistId, { { "count", ids.size() } });
        }
        done(error);
    });
}

void YouTubeService::removeFromPlaylist(const QString &playlistId, const QString &videoId, ErrorCallback done)
{
    m_api.fetchSetVideoIds(playlistId, [this, playlistId, videoId, done](const QHash<QString, QString> &setIds, const QString &error) {
        const QString setVideoId = setIds.value(videoId);
        if (setVideoId.isEmpty()) {
            done(!error.isEmpty() ? error : tr("The track isn't in this playlist anymore"));
            return;
        }
        const QJsonArray actions { QJsonObject { { "action", "ACTION_REMOVE_VIDEO" }, { "setVideoId", setVideoId },
                                                 { "removedVideoId", videoId } } };
        m_api.editPlaylist(playlistId, actions, [this, playlistId, videoId, done](const QString &error) {
            if (error.isEmpty()) {
                QStringList &ids = m_playlistTracks[playlistId];
                ids.removeAll(QStringLiteral("yt:") + videoId);
                updatePlaylistInfo(playlistId, { { "count", ids.size() } });
            }
            done(error);
        });
    });
}

void YouTubeService::renamePlaylist(const QString &playlistId, const QString &title, ErrorCallback done)
{
    const QJsonArray actions { QJsonObject { { "action", "ACTION_SET_PLAYLIST_NAME" }, { "playlistName", title } } };
    m_api.editPlaylist(playlistId, actions, [this, playlistId, title, done](const QString &error) {
        if (error.isEmpty())
            updatePlaylistInfo(playlistId, { { "title", title } });
        done(error);
    });
}

void YouTubeService::setPlaylistPrivacy(const QString &playlistId, const QString &privacy, ErrorCallback done)
{
    const QJsonArray actions { QJsonObject { { "action", "ACTION_SET_PLAYLIST_PRIVACY" }, { "playlistPrivacy", privacy } } };
    m_api.editPlaylist(playlistId, actions, done);
}

void YouTubeService::deletePlaylist(const QString &playlistId, ErrorCallback done)
{
    m_api.deletePlaylist(playlistId, [this, playlistId, done](const QString &error) {
        if (error.isEmpty()) {
            for (int i = 0; i < m_playlists.size(); ++i)
                if (m_playlists.at(i).toMap().value("id").toString() == playlistId)
                    m_playlists.removeAt(i--);
            m_playlistTracks.remove(playlistId);
            saveCache();
            emit dataChanged();
        }
        done(error);
    });
}

void YouTubeService::updatePlaylistInfo(const QString &playlistId, const QVariantMap &changes)
{
    for (QVariant &v : m_playlists) {
        QVariantMap p = v.toMap();
        if (p.value("id").toString() != playlistId)
            continue;
        for (auto it = changes.cbegin(); it != changes.cend(); ++it)
            p.insert(it.key(), it.value());
        v = p;
    }
    saveCache();
    emit dataChanged();
}

void YouTubeService::search(const QString &query)
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
        return;
    }
    // Playlists alongside: the featured ones, then the community's (without repeats), at most 12.
    m_api.searchPlaylists(q, true, [this, serial, q](const QVariantList &featured, const QString &) {
        if (serial != m_searchSerial)
            return;
        m_api.searchPlaylists(q, false, [this, serial, featured](const QVariantList &community, const QString &) {
            if (serial != m_searchSerial)
                return;
            QVariantList all;
            QSet<QString> seen;
            for (const QVariantList *list : { &featured, &community })
                for (const QVariant &v : *list)
                    if (all.size() < 12 && !seen.contains(v.toMap().value("id").toString())) {
                        seen.insert(v.toMap().value("id").toString());
                        all << v;
                    }
            m_playlistResults = all;
            emit playlistResultsChanged();
        });
    });
    m_searching = true;
    m_searchQuery = q;
    emit searchingChanged();
    // The unfiltered search mixes its shelves (top result, covers, videos, episodes) in no useful order —
    // the "Songs" and "Videos" chips are interleaved by rank instead (a song, then a video), so an upload
    // that's what was searched for (a DJ's mix) isn't buried under twenty merely related songs.
    m_api.search(q, [this, serial, q](const QList<Track> &songs, const QString &error) {
        if (serial != m_searchSerial)
            return;   // a newer query has already arrived
        m_api.search(q, [this, serial, songs, error](const QList<Track> &videos, const QString &videoError) {
            if (serial != m_searchSerial)
                return;
            m_searching = false;
            emit searchingChanged();
            if (!error.isEmpty() && !videoError.isEmpty()) {
                emit notice(tr("YouTube Music search: %1").arg(error), QStringLiteral("close"));
                return;
            }
            storeTracks(songs, true);
            storeTracks(videos, true);
            QList<Track> all;
            QSet<QString> seen;
            for (qsizetype i = 0; i < std::max(songs.size(), videos.size()); ++i)
                for (const QList<Track> *list : { &songs, &videos })
                    if (i < list->size() && !seen.contains(list->at(i).id)) {
                        seen.insert(list->at(i).id);
                        all << list->at(i);
                    }
            // For niche tracks the "Songs" chip fills up with merely related songs while the track itself
            // is only a video — whatever has the typed words in its title / artist goes first.
            const QStringList words = matchWords(m_searchQuery);
            std::stable_sort(all.begin(), all.end(), [&words](const Track &a, const Track &b) {
                return matchScore(a, words) > matchScore(b, words);
            });
            m_search.clear();
            for (const Track &t : std::as_const(all))
                m_search << t.id;
            publishResults(m_search);
        }, QStringLiteral("videos"));
    }, QStringLiteral("songs"));
}

void YouTubeService::setRecommended(const QList<Track> &tracks)
{
    storeTracks(tracks, true);
    m_recommended.clear();
    for (const Track &t : tracks)
        m_recommended << t.id;
    publishResults(m_recommended);
}

QStringList YouTubeService::keepRadio(const QString &key, const QList<Track> &tracks)
{
    storeTracks(tracks, true);
    QStringList ids;
    for (const Track &t : tracks)
        ids << t.id;
    m_radioKeys.removeAll(key);
    m_radioKeys << key;
    while (m_radioKeys.size() > 12)
        m_collections.remove(m_radioKeys.takeFirst());
    m_collections.insert(key, ids);
    publishResults(ids);
    return ids;
}

void YouTubeService::setHistory(const QList<Track> &tracks)
{
    storeTracks(tracks, true);
    m_history.clear();
    for (const Track &t : tracks)
        m_history << t.id;
    publishResults(m_history);
}

void YouTubeService::publishResults(const QStringList &ids)
{
    // Only the tracks the model doesn't have yet are appended (Library::appendTracks) — a full model
    // rebuild re-creates every list in the UI, which stalls it (and the audio that passes through it).
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

// ---- artists and collections ------------------------------------------------------

void YouTubeService::loadArtist(const QString &browseId)
{
    if (browseId.isEmpty() || m_artists.contains(browseId) || m_loadingArtists.contains(browseId))
        return;
    m_loadingArtists.insert(browseId);
    QTimer::singleShot(0, this, [this] { ++m_artistsRevision; emit artistsChanged(); });   // may be called from a QML binding
    m_api.fetchArtist(browseId, [this, browseId](const YtMusicApi::Artist &a, const QString &error) {
        m_loadingArtists.remove(browseId);
        if (!error.isEmpty()) {
            m_artists.insert(browseId, { { "browseId", browseId }, { "error", error } });
            ++m_artistsRevision;
        emit artistsChanged();
            return;
        }
        storeTracks(a.songs, true);
        QStringList ids;
        for (const Track &t : a.songs)
            ids << t.id;
        m_artistSongs.insert(browseId, ids);
        m_artistIdByName.insert(a.name.toLower(), browseId);
        m_artists.insert(browseId, {
            { "browseId", browseId }, { "name", a.name }, { "description", a.description },
            { "subscribers", a.subscribers }, { "photo", a.photo }, { "songsPlaylistId", a.songsPlaylistId }, { "videosOnly", a.videosOnly },
            { "sections", a.sections } });
        publishResults(ids);   // the top songs join the track model, so they can be played
        ++m_artistsRevision;
        emit artistsChanged();
    });
}

void YouTubeService::loadArtistSection(const QString &browseId, int section)
{
    QVariantMap artist = m_artists.value(browseId);
    QVariantList sections = artist.value("sections").toList();
    if (section < 0 || section >= sections.size())
        return;
    QVariantMap s = sections.at(section).toMap();
    const QString moreId = s.value("moreId").toString();
    if (moreId.isEmpty() || s.value("loading").toBool() || s.value("complete").toBool())
        return;
    s.insert("loading", true);
    sections[section] = s;
    artist.insert("sections", sections);
    m_artists.insert(browseId, artist);
    QTimer::singleShot(0, this, [this] { ++m_artistsRevision; emit artistsChanged(); });   // called from QML
    m_api.fetchCards(moreId, s.value("moreParams").toString(),
                     [this, browseId, section](const QVariantList &cards, const QString &error) {
        QVariantMap artist = m_artists.value(browseId);
        QVariantList sections = artist.value("sections").toList();
        if (section >= sections.size())
            return;
        QVariantMap s = sections.at(section).toMap();
        s.remove("loading");
        if (!cards.isEmpty()) {
            s.insert("items", cards);
            s.insert("complete", true);
        } else {
            emit notice(tr("Couldn't load the whole list: %1").arg(error), QStringLiteral("close"));
        }
        sections[section] = s;
        artist.insert("sections", sections);
        m_artists.insert(browseId, artist);
        ++m_artistsRevision;
        emit artistsChanged();
    });
}

QVariantMap YouTubeService::artist(const QString &browseId) const
{
    QVariantMap a = m_artists.value(browseId);
    a.insert("loading", m_loadingArtists.contains(browseId));
    return a;
}

void YouTubeService::findArtist(const QString &name)
{
    const QString key = name.trimmed().toLower();
    if (key.isEmpty())
        return;
    if (m_artistIdByName.contains(key)) {
        const QString id = m_artistIdByName.value(key);
        QTimer::singleShot(0, this, [this, name, id] { emit artistFound(name, id); });
        return;
    }
    m_api.findArtistId(name.trimmed(), [this, name, key](const QString &browseId, const QString &error) {
        if (error.isEmpty())
            m_artistIdByName.insert(key, browseId);   // "" = not on YouTube Music, don't ask again
        emit artistFound(name, browseId);
    });
}

void YouTubeService::ensureCollection(const QString &playlistId)
{
    if (playlistId.isEmpty() || m_collections.contains(playlistId) || m_loadingCollections.contains(playlistId))
        return;
    m_loadingCollections.insert(playlistId);
    QTimer::singleShot(0, this, [this] { ++m_resultsRevision; emit resultsChanged(); });   // "loading" (called from a binding)
    // A playlist ("VL<id>"), or an album page ("MPREb_…") when its card had no playlist.
    const QString browseId = playlistId.startsWith(QLatin1String("MPRE")) ? playlistId : QStringLiteral("VL") + playlistId;
    m_api.browseTracks(browseId, [this, playlistId](const QList<Track> &tracks, const QString &error) {
        m_loadingCollections.remove(playlistId);
        if (!error.isEmpty() && tracks.isEmpty())
            emit notice(tr("Couldn't load the playlist: %1").arg(error), QStringLiteral("close"));
        storeTracks(tracks, true);
        QStringList ids;
        for (const Track &t : tracks)
            ids << t.id;
        m_collections.insert(playlistId, ids);
        publishResults(ids);
    }, 10);
}

// ---- data -------------------------------------------------------------------------

void YouTubeService::storeTracks(const QList<Track> &tracks, bool transient)
{
    for (Track t : tracks) {
        const auto existing = m_tracks.constFind(t.id);
        if (existing != m_tracks.cend()) {
            // Keep the colors computed from the cover and the track's "permanence" in the library.
            t.colorA = existing->colorA;
            t.colorB = existing->colorB;
            t.transient = existing->transient && transient;
        } else {
            t.transient = transient;
        }
        m_tracks.insert(t.id, t);
    }
}

void YouTubeService::setLikedList(const QStringList &ids)
{
    m_liked = ids;
    m_likedSet = QSet<QString>(ids.cbegin(), ids.cend());
}

QList<Track> YouTubeService::tracks() const
{
    // Order: liked, tracks from playlists, search results.
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
    for (const QString &id : m_recommended)
        add(id);
    for (const QString &id : m_history)
        add(id);
    for (const QStringList &ids : m_artistSongs)
        for (const QString &id : ids)
            add(id);
    for (const QStringList &ids : m_collections)
        for (const QString &id : ids)
            add(id);
    return out;
}

void YouTubeService::setTrackLiked(const QString &trackId, bool liked)
{
    if (liked == m_likedSet.contains(trackId))
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
    const auto it = m_tracks.constFind(trackId);
    if (it != m_tracks.cend())
        m_api.setLiked(it->path, liked);   // syncing with the account
    saveCache();
}

void YouTubeService::adoptLiked(const QList<Track> &liked, const QStringList &unliked)
{
    storeTracks(liked, false);
    bool changed = false;
    // newest first, like the account's list: the oldest goes in first, each next one in front of it
    for (auto it = liked.crbegin(); it != liked.crend(); ++it) {
        const Track &t = *it;
        if (m_likedSet.contains(t.id))
            continue;
        m_liked.prepend(t.id);
        m_likedSet.insert(t.id);
        m_tracks[t.id].transient = false;
        m_api.setLiked(t.path, true);
        changed = true;
    }
    for (const QString &id : unliked) {
        if (!m_likedSet.remove(id))
            continue;
        m_liked.removeAll(id);
        if (const auto it = m_tracks.constFind(id); it != m_tracks.cend())
            m_api.setLiked(it->path, false);
        changed = true;
    }
    if (!changed)
        return;
    saveCache();
    emit dataChanged();
}

void YouTubeService::setTrackColors(const QString &trackId, const QColor &a, const QColor &b)
{
    auto it = m_tracks.find(trackId);
    if (it == m_tracks.end())
        return;
    it->colorA = a;
    it->colorB = b;
}

void YouTubeService::downloadEngine()
{
    m_resolver.download();
}

// ---- cache ------------------------------------------------------------------------

void YouTubeService::loadCache()
{
    QFile file(cacheFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

    const QJsonObject account = root.value("account").toObject();
    m_account = { account.value("name").toString(), account.value("handle").toString(), account.value("photo").toString() };

    for (const QJsonValue &v : root.value("tracks").toArray()) {
        const Track t = fromJson(v.toObject());
        m_tracks.insert(t.id, t);
    }
    setLikedList(fromArray(root.value("liked").toArray()));
    m_playlists = root.value("playlists").toArray().toVariantList();
    const QJsonObject lists = root.value("playlistTracks").toObject();
    for (auto it = lists.begin(); it != lists.end(); ++it)
        m_playlistTracks.insert(it.key(), fromArray(it.value().toArray()));
    m_lastSync = QDateTime::fromString(root.value("lastSync").toString(), Qt::ISODate);
}

void YouTubeService::saveCache()
{
    m_saveTimer.start();
}

void YouTubeService::writeCache(bool wait)
{
    m_saveTimer.stop();
    // The cache holds every track seen (about a megabyte of JSON): it's built and written from copies
    // of the data (cheap, shared until changed) on a worker thread, so the interface doesn't stall.
    auto job = [tracks = m_tracks, liked = m_liked, playlists = m_playlists, lists = m_playlistTracks,
                account = m_account, lastSync = m_lastSync, path = cacheFile()] {
        QJsonArray trackArray;
        for (const Track &t : tracks)
            if (!t.transient)
                trackArray.append(toJson(t));
        QJsonObject listObject;
        for (auto it = lists.cbegin(); it != lists.cend(); ++it)
            listObject.insert(it.key(), toArray(it.value()));
        const QJsonObject root {
            { "account", QJsonObject { { "name", account.name }, { "handle", account.handle }, { "photo", account.photo } } },
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
