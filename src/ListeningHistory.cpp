#include "ListeningHistory.h"

#include "AppPaths.h"
#include "Library.h"
#include "soundcloud/SoundCloudService.h"
#include "openmusic/OpenMusicService.h"
#include "youtube/YouTubeService.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSEngine>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cstdlib>

namespace {

constexpr int kMaxEvents = 50000;
constexpr int kKeptTracks = 300;   // streaming tracks kept playable after a restart

QString historyFile() { return AppPaths::dataDir() + QStringLiteral("/history.json"); }

} // namespace

ListeningHistory *ListeningHistory::instance()
{
    static ListeningHistory *inst = new ListeningHistory;
    return inst;
}

ListeningHistory *ListeningHistory::create(QQmlEngine *, QJSEngine *)
{
    ListeningHistory *h = instance();
    QJSEngine::setObjectOwnership(h, QJSEngine::CppOwnership);
    return h;
}

ListeningHistory::ListeningHistory(QObject *parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(10000);
    connect(&m_saveTimer, &QTimer::timeout, this, &ListeningHistory::save);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        if (m_saveTimer.isActive())
            save();
    });
    load();
    QTimer::singleShot(0, this, &ListeningHistory::keepStreamingTracks);
}

void ListeningHistory::notePlay(const Track &t, double secondsSoFar)
{
    if (t.demo || t.id.isEmpty() || t.source == QLatin1String("radio"))
        return;
    m_events.append({ t.id, QDateTime::currentMSecsSinceEpoch(), secondsSoFar });
    if (m_events.size() > kMaxEvents)
        m_events.remove(0, m_events.size() - kMaxEvents);
    const bool streaming = (t.source == QLatin1String("youtube") || t.source == QLatin1String("soundcloud")
                            || OpenMusicService::isOpenSource(t.source))
                           && !m_tracks.contains(t.id);
    m_tracks.insert(t.id, t);
    if (streaming)
        keepStreamingTracks();
    ++m_revision;
    emit changed();
    emit playNoted(t);
    m_saveTimer.start();
}

void ListeningHistory::addRemote(const QList<std::pair<Track, qint64>> &plays)
{
    constexpr qint64 kSame = 3 * 60 * 1000;   // our own play, back from the server, is a few seconds apart
    bool added = false;
    for (const auto &[t, at] : plays) {
        if (t.id.isEmpty() || at <= 0)
            continue;
        const bool known = std::any_of(m_events.cbegin(), m_events.cend(), [&](const Event &e) {
            return e.id == t.id && std::abs(e.at - at) < kSame;
        });
        if (known)
            continue;
        const auto pos = std::upper_bound(m_events.begin(), m_events.end(), at,
                                          [](qint64 v, const Event &e) { return v < e.at; });
        m_events.insert(pos, { t.id, at, -1 });
        if (!m_tracks.contains(t.id))
            m_tracks.insert(t.id, t);
        added = true;
    }
    if (!added)
        return;
    if (m_events.size() > kMaxEvents)
        m_events.remove(0, m_events.size() - kMaxEvents);
    keepStreamingTracks();
    ++m_revision;
    emit changed();
    m_saveTimer.start();
}

void ListeningHistory::addListening(const QString &id, double seconds)
{
    if (m_events.isEmpty() || m_events.last().id != id || m_events.last().seconds < 0)
        return;
    m_events.last().seconds += seconds;
    if (!m_saveTimer.isActive())
        m_saveTimer.start();
}

QStringList ListeningHistory::recentIds(int limit) const
{
    QStringList ids;
    QSet<QString> seen;
    for (auto it = m_events.crbegin(); it != m_events.crend() && ids.size() < limit; ++it)
        if (!seen.contains(it->id)) {
            seen.insert(it->id);
            ids << it->id;
        }
    return ids;
}

void ListeningHistory::keepStreamingTracks()
{
    // YouTube Music tracks only live in the model while a search or a page shows them — the recent
    // ones are kept there like the recommendations, so "Recently played" can play them.
    QList<Track> keep, soundcloud, open;
    for (const QString &id : recentIds(kKeptTracks)) {
        const auto it = m_tracks.constFind(id);
        if (it != m_tracks.cend() && it->source == QLatin1String("youtube"))
            keep << *it;
        else if (it != m_tracks.cend() && it->source == QLatin1String("soundcloud"))
            soundcloud << *it;
        else if (it != m_tracks.cend() && OpenMusicService::isOpenSource(it->source))
            open << *it;
    }
    YouTubeService::instance()->setHistory(keep);
    SoundCloudService::instance()->setHistory(soundcloud);
    OpenMusicService::instance()->setHistory(open);
}

QVariantMap ListeningHistory::stats(const QString &period) const
{
    const QDate today = QDate::currentDate();
    qint64 from = 0;
    if (period == QLatin1String("month"))
        from = QDate(today.year(), today.month(), 1).startOfDay().toMSecsSinceEpoch();
    else if (period == QLatin1String("year"))
        from = QDate(today.year(), 1, 1).startOfDay().toMSecsSinceEpoch();

    struct Count { int plays = 0; double seconds = 0; QString name, cover, artistId, id; qint64 last = 0; };
    QHash<QString, Count> artists, tracks, genres;
    QList<double> hours(24, 0.0);
    QSet<QDate> days;
    double seconds = 0;
    int plays = 0;
    qint64 first = 0;
    Library *library = Library::instance();
    for (const Event &e : m_events) {
        if (e.at < from || e.seconds < 0)
            continue;
        const auto meta = m_tracks.constFind(e.id);
        if (meta == m_tracks.cend())
            continue;
        // The library's copy may know more (a cover, the artist's page) than what was kept.
        const int row = library->tracks()->rowOf(e.id);
        const Track *known = row >= 0 ? &library->tracks()->at(row) : nullptr;
        ++plays;
        seconds += e.seconds;
        if (!first)
            first = e.at;
        const QDateTime when = QDateTime::fromMSecsSinceEpoch(e.at);
        days.insert(when.date());
        hours[when.time().hour()] += e.seconds;

        const QString artist = TrackModel::primaryArtist(meta->artist);
        Count &a = artists[artist.toLower()];
        ++a.plays;
        a.seconds += e.seconds;
        a.name = artist;
        if (e.at >= a.last) {
            a.last = e.at;
            const QString cover = !meta->cover.isEmpty() ? meta->cover : known ? known->cover : QString();
            const QString artistId = !meta->artistId.isEmpty() ? meta->artistId : known ? known->artistId : QString();
            if (!cover.isEmpty())
                a.cover = cover;
            if (!artistId.isEmpty())
                a.artistId = artistId;
        }
        Count &t = tracks[e.id];
        ++t.plays;
        t.seconds += e.seconds;
        t.id = e.id;
        for (const QString &g : library->genresOf(*meta)) {
            Count &c = genres[g.toLower()];
            ++c.plays;
            c.name = g;
        }
    }

    auto top = [](const QHash<QString, Count> &all, int n) {
        QList<Count> list = all.values();
        std::sort(list.begin(), list.end(), [](const Count &a, const Count &b) {
            return a.plays != b.plays ? a.plays > b.plays : a.seconds > b.seconds;
        });
        return list.mid(0, n);
    };
    QVariantList topArtists, topTracks, topGenres, byHour;
    for (const Count &a : top(artists, 6))
        topArtists << QVariantMap { { "name", a.name }, { "plays", a.plays }, { "minutes", qRound(a.seconds / 60) },
                                    { "cover", a.cover }, { "artistId", a.artistId } };
    for (const Count &t : top(tracks, 10)) {
        const Track &meta = m_tracks.value(t.id);
        topTracks << QVariantMap { { "row", library->tracks()->rowOf(t.id) }, { "title", meta.title },
                                   { "artist", meta.artist }, { "cover", meta.cover }, { "plays", t.plays } };
    }
    int genrePlays = 0;
    for (const Count &g : std::as_const(genres))
        genrePlays += g.plays;
    for (const Count &g : top(genres, 6))
        topGenres << QVariantMap { { "name", g.name }, { "plays", g.plays },
                                   { "share", genrePlays > 0 ? double(g.plays) / genrePlays : 0.0 } };
    for (double h : std::as_const(hours))
        byHour << qRound(h / 60);
    return {
        { "minutes", qRound(seconds / 60) }, { "plays", plays }, { "tracks", int(tracks.size()) },
        { "artists", int(artists.size()) }, { "days", int(days.size()) },
        { "since", first ? QDateTime::fromMSecsSinceEpoch(first) : QDateTime() },
        { "topArtists", topArtists }, { "topTracks", topTracks }, { "topGenres", topGenres }, { "byHour", byHour },
    };
}

// ---- storage ----------------------------------------------------------------------

void ListeningHistory::load()
{
    QFile file(historyFile());
    if (!file.open(QIODevice::ReadOnly)) {
        // After startup: the import looks tracks up in the library, which may be what's creating us.
        QTimer::singleShot(0, this, [this] {
            importPlays();
            keepStreamingTracks();
            ++m_revision;
            emit changed();
        });
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &v : root.value("tracks").toArray()) {
        const Track t = YouTubeService::trackFromJson(v.toObject());
        if (!t.id.isEmpty())
            m_tracks.insert(t.id, t);
    }
    // Events as [id, ms, seconds] — compact, there can be tens of thousands.
    for (const QJsonValue &v : root.value("events").toArray()) {
        const QJsonArray e = v.toArray();
        if (e.size() >= 3 && m_tracks.contains(e.at(0).toString()))
            m_events.append({ e.at(0).toString(), qint64(e.at(1).toDouble()), e.at(2).toDouble() });
    }
}

void ListeningHistory::importPlays()
{
    // Before the history existed the recommendations counted plays (how many and the last one) — each
    // becomes that many entries at the last time, so the stats don't start empty.
    QFile file(AppPaths::dataDir() + QStringLiteral("/recommendations.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray plays = QJsonDocument::fromJson(file.readAll()).object().value("plays").toArray();
    TrackModel *model = Library::instance()->tracks();
    for (const QJsonValue &v : plays) {
        const QJsonObject p = v.toObject();
        const QString id = p.value("id").toString();
        if (id.isEmpty())
            continue;
        Track t;
        if (const int row = model->rowOf(id); row >= 0) {
            t = model->at(row);
        } else {
            t.id = id;
            t.title = p.value("title").toString();
            t.artist = p.value("artist").toString();
            t.duration = p.value("duration").toInt();
            if (id.startsWith(QLatin1String("sc:"))) {
                t.source = QStringLiteral("soundcloud");
            } else if (id.startsWith(QLatin1String("ia:")) || id.startsWith(QLatin1String("wc:"))) {
                t.source = id.startsWith(QLatin1String("ia:")) ? QStringLiteral("archive") : QStringLiteral("commons");
                t.quality = QStringLiteral("FLAC");
            } else if (id.startsWith(QLatin1String("yt:"))) {
                t.source = QStringLiteral("youtube");
                t.path = id.mid(3);
            }
        }
        m_tracks.insert(id, t);
        const double heard = t.duration > 0 ? t.duration : 180;
        for (int i = 0; i < std::max(1, p.value("count").toInt()); ++i)
            m_events.append({ id, qint64(p.value("last").toDouble()), heard });
    }
    std::sort(m_events.begin(), m_events.end(), [](const Event &a, const Event &b) { return a.at < b.at; });
    if (!m_events.isEmpty())
        m_saveTimer.start();
}

void ListeningHistory::save() const
{
    QSet<QString> used;
    QJsonArray events;
    for (const Event &e : m_events) {
        used.insert(e.id);
        events.append(QJsonArray { e.id, double(e.at), qRound(e.seconds * 10) / 10.0 });
    }
    QJsonArray tracks;
    for (const QString &id : std::as_const(used))
        tracks.append(YouTubeService::trackJson(m_tracks.value(id)));
    QSaveFile file(historyFile());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject { { "version", 1 }, { "tracks", tracks }, { "events", events } })
                   .toJson(QJsonDocument::Compact));
    file.commit();
}
