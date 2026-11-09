#include "Recommendations.h"

#include "AppPaths.h"
#include "Library.h"
#include "LibraryScanner.h"
#include "youtube/YouTubeService.h"

#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

QString stateFile() { return AppPaths::dataDir() + QStringLiteral("/recommendations.json"); }

constexpr int kSeeds = 8;                            // radios per refresh
constexpr int kFavouriteSeeds = 5;                   // of them from the artists that say the most; the rest explore
constexpr int kPicks = 60;                           // tracks kept
constexpr int kPerArtist = 2;                        // the same artist at most this many times
constexpr int kMaxPlays = 3000;                      // remembered tracks
constexpr qint64 kStaleMs = 4 * 3600 * 1000LL;       // older picks are made again
constexpr int kRefreshPlays = 12;                    // ...and after this many tracks listened to
constexpr qint64 kShownMs = 2 * 86'400'000LL;        // a pick offered this long ago and not played makes room
constexpr qint64 kForgetShownMs = 30 * 86'400'000LL; // after this it may be offered again
constexpr int kRadioSeeds = 8;
constexpr qint64 kRetryMs = 5 * 60 * 1000LL;         // a refresh that failed (no network) waits this long

// A play from a month ago counts half as much as one from today.
double recency(qint64 lastMs, qint64 nowMs)
{
    const double days = double(std::max<qint64>(0, nowMs - lastMs)) / 86'400'000.0;
    return std::pow(0.5, days / 30.0);
}

bool usableArtist(const QString &artist)
{
    const QString key = Recommendations::looseKey(artist);
    return !key.isEmpty() && !Scanner::isUnknownArtist(artist) && key != QLatin1String("youtubemusic")
           && key != QLatin1String("variousartists");
}

// Index into `weights`, drawn in proportion to them.
int weightedPick(const QList<double> &weights)
{
    double total = 0;
    for (double w : weights)
        total += w;
    double r = QRandomGenerator::global()->generateDouble() * total;
    for (int i = 0; i < weights.size(); ++i) {
        r -= weights.at(i);
        if (r <= 0)
            return i;
    }
    return int(weights.size()) - 1;
}

} // namespace

Recommendations *Recommendations::instance()
{
    static Recommendations *inst = new Recommendations;
    return inst;
}

Recommendations *Recommendations::create(QQmlEngine *, QJSEngine *)
{
    Recommendations *r = instance();
    QJSEngine::setObjectOwnership(r, QJSEngine::CppOwnership);
    return r;
}

Recommendations::Recommendations(QObject *parent)
    : QObject(parent)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(10'000);
    connect(&m_saveTimer, &QTimer::timeout, this, &Recommendations::save);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        if (m_saveTimer.isActive())
            save();
    });

    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(3000);
    connect(&m_retryTimer, &QTimer::timeout, this, &Recommendations::maybeRefresh);

    load();
    Library *lib = Library::instance();   // the track model takes the picks in (YouTubeService::tracksAdded)
    // The picks from last time show at once; stale ones (or none) are made again once the app is up.
    QTimer::singleShot(0, this, [this] {
        if (!m_picks.isEmpty())
            YouTubeService::instance()->setRecommended(m_picks);
        if (m_radioSeeds.isEmpty())
            pickRadioSeeds();
        m_retryTimer.start();
    });
    // A scan or a sync can bring the taste an earlier refresh didn't have.
    connect(lib, &Library::libraryChanged, &m_retryTimer, qOverload<>(&QTimer::start));
}

QString Recommendations::looseKey(const QString &text)
{
    // "Song (feat. X) [Remastered] - Live" -> "song": the same song from another source matches.
    QString s = text.toLower();
    static const QRegularExpression brackets(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]"));
    s.remove(brackets);
    if (const int dash = int(s.indexOf(QLatin1String(" - "))); dash > 0)
        s.truncate(dash);
    static const QRegularExpression feat(QStringLiteral("\\s(?:feat|ft)\\.?\\s.*$"));
    s.remove(feat);
    static const QRegularExpression other(QStringLiteral("[^\\p{L}\\p{N}]+"));
    s.remove(other);
    return s;
}

void Recommendations::notePlayed(const Track &t)
{
    if (t.demo || t.id.isEmpty() || t.source == QLatin1String("radio"))
        return;
    Play &p = m_plays[t.id];
    p.id = t.id;
    p.artist = t.artist;
    p.title = t.title;
    p.duration = t.duration;
    ++p.count;
    p.last = QDateTime::currentMSecsSinceEpoch();
    if (++m_playsSinceRefresh >= kRefreshPlays)
        m_retryTimer.start();
    if (m_plays.size() > kMaxPlays) {
        auto oldest = m_plays.begin();
        for (auto it = m_plays.begin(); it != m_plays.end(); ++it)
            if (it->last < oldest->last)
                oldest = it;
        m_plays.erase(oldest);
    }
    m_saveTimer.start();
}

// ---- picking ---------------------------------------------------------------------

QList<Recommendations::Seed> Recommendations::pickSeeds() const
{
    struct Artist {
        double score = 0;
        int inLibrary = 0;
        int liked = 0;
        QList<Seed> tracks;
        QList<double> weights;
        QHash<QString, int> spellings;
    };
    QHash<QString, Artist> artists;
    const auto artistOf = [&artists](const QString &name) -> Artist & {
        const QString primary = TrackModel::primaryArtist(name);
        Artist &a = artists[looseKey(primary)];
        ++a.spellings[primary];
        return a;
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // What the user listens to says the most…
    QSet<QString> played;
    for (const Play &p : m_plays) {
        if (!usableArtist(p.artist))
            continue;
        const double w = p.count * recency(p.last, now);
        Artist &a = artistOf(p.artist);
        a.score += 3.0 * w;
        a.tracks << Seed { TrackModel::primaryArtist(p.artist), p.title,
                           p.id.startsWith(QLatin1String("yt:")) ? p.id.mid(3) : QString(), p.duration };
        a.weights << 3.0 * w + 0.1;
        played.insert(p.id);
    }
    // …then what they liked and which artists fill the library.
    for (const Track &t : Library::instance()->tracks()->tracks()) {
        if (t.transient || t.demo || t.source == QLatin1String("radio") || !usableArtist(t.artist))
            continue;
        // Capped, so a big library doesn't drown out what the user actually plays.
        Artist &a = artistOf(t.artist);
        if (++a.inLibrary <= 25)
            a.score += 0.4;
        if (t.liked && ++a.liked <= 20)
            a.score += 1.5;
        if (played.contains(t.id))
            continue;   // already a candidate, with its plays
        a.tracks << Seed { TrackModel::primaryArtist(t.artist), t.title,
                           t.source == QLatin1String("youtube") ? t.path : QString(), t.duration };
        a.weights << (t.liked ? 2.0 : 0.3);
    }

    QList<Artist> ranked;
    for (auto it = artists.begin(); it != artists.end(); ++it)
        if (it->score > 0 && !it->tracks.isEmpty())
            ranked << *it;
    std::sort(ranked.begin(), ranked.end(), [](const Artist &a, const Artist &b) { return a.score > b.score; });

    // The artist that says the most, then a few drawn from the next ones (the higher, the likelier) —
    // every refresh brings other picks without drifting away from the taste.
    QList<int> chosen;
    if (!ranked.isEmpty())
        chosen << 0;
    QList<int> pool;
    for (int i = 1; i < ranked.size() && i < 16; ++i)
        pool << i;
    while (chosen.size() < kFavouriteSeeds && !pool.isEmpty()) {
        QList<double> weights;
        for (int i : std::as_const(pool))
            weights << ranked.at(i).score;
        chosen << pool.takeAt(weightedPick(weights));
    }
    // The rest from anywhere else in the taste, all equally likely: artists heard once or saved long ago
    // lead somewhere the favourites' radios don't.
    QList<int> far;
    for (int i = 1; i < ranked.size(); ++i)
        if (!chosen.contains(i))
            far << i;
    while (chosen.size() < kSeeds && !far.isEmpty())
        chosen << far.takeAt(int(QRandomGenerator::global()->bounded(int(far.size()))));

    QList<Seed> seeds;
    for (int i : std::as_const(chosen)) {
        const Artist &a = ranked.at(i);
        Seed seed = a.tracks.at(weightedPick(a.weights));
        // The name the way most of their tracks spell it.
        int most = 0;
        for (auto it = a.spellings.cbegin(); it != a.spellings.cend(); ++it)
            if (it.value() > most) {
                most = it.value();
                seed.artist = it.key();
            }
        seeds << seed;
    }
    return seeds;
}

QSet<QString> Recommendations::heardSince(qint64 sinceMs) const
{
    QSet<QString> out;
    for (const Play &p : m_plays)
        if (p.last >= sinceMs) {
            out.insert(p.id);
            out.insert(looseKey(TrackModel::primaryArtist(p.artist)) + QLatin1Char('|') + looseKey(p.title));
        }
    return out;
}

void Recommendations::pickRadioSeeds()
{
    // A track for each of the artists listened to most lately (one per artist), drawn so that the home
    // page offers other radios from time to time.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, QPair<double, QString>> best;   // artist -> (score, track id)
    QHash<QString, double> artistScore;
    for (const Play &p : m_plays) {
        if (!usableArtist(p.artist))
            continue;
        const QString artist = looseKey(TrackModel::primaryArtist(p.artist));
        const double w = p.count * recency(p.last, now);
        artistScore[artist] += w;
        if (w > best.value(artist).first)
            best.insert(artist, { w, p.id });
    }
    for (const Track &t : Library::instance()->tracks()->tracks()) {   // liked tracks when little was played
        if (!t.liked || t.transient || t.demo || !usableArtist(t.artist))
            continue;
        const QString artist = looseKey(TrackModel::primaryArtist(t.artist));
        if (!best.contains(artist)) {
            best.insert(artist, { 0.05, t.id });
            artistScore[artist] += 0.05;
        }
    }
    QStringList artists = best.keys();
    std::sort(artists.begin(), artists.end(),
              [&artistScore](const QString &a, const QString &b) { return artistScore.value(a) > artistScore.value(b); });
    artists = artists.mid(0, 30);
    QList<double> weights;
    for (const QString &a : std::as_const(artists))
        weights << artistScore.value(a) + 0.2;
    m_radioSeeds.clear();
    while (m_radioSeeds.size() < kRadioSeeds * 2 && !artists.isEmpty()) {
        const int i = weightedPick(weights);
        m_radioSeeds << best.value(artists.takeAt(i)).second;
        weights.removeAt(i);
    }
}

QVariantList Recommendations::radioSeeds() const
{
    TrackModel *model = Library::instance()->tracks();
    QVariantList out;
    for (const QString &id : m_radioSeeds) {
        const int row = model->rowOf(id);
        if (row < 0)
            continue;
        const Track &t = model->at(row);
        out << QVariantMap { { "row", row }, { "id", t.id }, { "title", t.title }, { "artist", t.artist }, { "cover", t.cover },
                             { "colorA", t.colorA }, { "colorB", t.colorB }, { "seed", t.seed } };
        if (out.size() >= kRadioSeeds)
            break;
    }
    return out;
}

void Recommendations::refresh()
{
    if (loading())
        return;
    const QList<Seed> seeds = pickSeeds();
    if (seeds.isEmpty())
        return;   // nothing to go on yet: an empty library and nothing played
    m_playsSinceRefresh = 0;
    pickRadioSeeds();
    m_lastAttempt.start();
    const quint64 serial = ++m_serial;
    m_radios = QList<QList<Track>>(seeds.size());
    m_seedArtists.clear();
    for (const Seed &s : seeds)
        m_seedArtists << s.artist;
    m_pending = int(seeds.size());
    emit changed();
    for (int i = 0; i < seeds.size(); ++i)
        startSeed(seeds.at(i), i, serial);
}

void Recommendations::maybeRefresh()
{
    if (loading())
        return;
    const bool stale = m_picks.isEmpty() || !m_generated.isValid() || m_playsSinceRefresh >= kRefreshPlays
                       || m_generated.msecsTo(QDateTime::currentDateTimeUtc()) > kStaleMs;
    if (!stale || (m_lastAttempt.isValid() && m_lastAttempt.elapsed() < kRetryMs))
        return;
    refresh();
}

void Recommendations::startSeed(const Seed &seed, int slot, quint64 serial)
{
    YtMusicApi *api = YouTubeService::instance()->api();
    const auto startRadio = [this, api, slot, serial](const QString &videoId) {
        if (videoId.isEmpty()) {
            finishSeed(slot, {}, serial);
            return;
        }
        api->radio(videoId, [this, slot, serial](const QList<Track> &tracks, const QString &) {
            finishSeed(slot, tracks, serial);
        });
    };
    // The song version of the track (not its music video) gives a radio of songs, with albums and covers.
    Track wanted;
    wanted.title = seed.title;
    wanted.artist = seed.artist;
    wanted.duration = seed.duration;
    api->search(seed.artist + QLatin1Char(' ') + seed.title,
                [wanted, fallback = seed.videoId, startRadio](const QList<Track> &results, const QString &) {
        const int best = YtMusicApi::bestMatch(wanted, results);
        startRadio(best >= 0 ? results.at(best).path : fallback);
    }, QStringLiteral("songs"));
}

void Recommendations::finishSeed(int slot, const QList<Track> &radio, quint64 serial)
{
    if (serial != m_serial)
        return;
    if (slot < m_radios.size())
        m_radios[slot] = radio;
    if (--m_pending > 0)
        return;
    publish();
}

void Recommendations::publish()
{
    // What the user already has or has heard isn't a recommendation — by id, or the same song elsewhere.
    QSet<QString> knownIds;
    QSet<QString> known;
    const auto songKey = [](const QString &artist, const QString &title) {
        return looseKey(TrackModel::primaryArtist(artist)) + QLatin1Char('|') + looseKey(title);
    };
    QSet<QString> knownArtists;
    for (const Track &t : Library::instance()->tracks()->tracks()) {
        if (t.transient || t.demo)
            continue;
        knownIds.insert(t.id);
        known.insert(songKey(t.artist, t.title));
        knownArtists.insert(looseKey(TrackModel::primaryArtist(t.artist)));
    }
    for (const Play &p : std::as_const(m_plays)) {
        knownIds.insert(p.id);
        known.insert(songKey(p.artist, p.title));
        knownArtists.insert(looseKey(TrackModel::primaryArtist(p.artist)));
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_shown.begin(); it != m_shown.end();)
        it = now - it.value() > kForgetShownMs ? m_shown.erase(it) : std::next(it);

    // The radios take turns, so every seed shows up near the top. Picks offered days ago and still not
    // played make room for others; only when that leaves too few do they come back.
    QList<Track> picks;
    QStringList basedOn;
    QSet<QString> taken;
    QHash<QString, int> perArtist;
    for (const bool rotate : { true, false }) {
        QList<int> cursor(m_radios.size(), 0);
        for (bool progress = true; progress && picks.size() < kPicks;) {
            progress = false;
            for (int s = 0; s < m_radios.size() && picks.size() < kPicks; ++s) {
                const QList<Track> &radio = m_radios.at(s);
                while (cursor[s] < radio.size()) {
                    const Track &t = radio.at(cursor[s]++);
                    const QString artist = looseKey(TrackModel::primaryArtist(t.artist));
                    const QString key = songKey(t.artist, t.title);
                    const qint64 shown = m_shown.value(key, 0);
                    if (knownIds.contains(t.id) || known.contains(key) || taken.contains(key)
                        || perArtist.value(artist) >= kPerArtist || (rotate && shown > 0 && now - shown > kShownMs))
                        continue;
                    taken.insert(key);
                    ++perArtist[artist];
                    picks << t;
                    if (!basedOn.contains(m_seedArtists.value(s)))
                        basedOn << m_seedArtists.value(s);
                    progress = true;
                    break;
                }
            }
        }
        if (picks.size() >= kPicks / 2)
            break;
    }
    m_radios.clear();

    // Artists the user doesn't know yet take every other place, from the top.
    QList<Track> familiar, fresh;
    for (const Track &t : std::as_const(picks))
        (knownArtists.contains(looseKey(TrackModel::primaryArtist(t.artist))) ? familiar : fresh) << t;
    QSet<QString> freshIds;
    for (const Track &t : std::as_const(fresh))
        freshIds.insert(t.id);
    picks.clear();
    for (int i = 0; i < std::max(familiar.size(), fresh.size()); ++i) {
        if (i < fresh.size())
            picks << fresh.at(i);
        if (i < familiar.size())
            picks << familiar.at(i);
    }

    if (!picks.isEmpty()) {   // otherwise (no network) the previous picks stay
        // The order of the seeds, not of the first pick from each.
        QStringList ordered;
        for (const QString &a : std::as_const(m_seedArtists))
            if (basedOn.contains(a) && !ordered.contains(a))
                ordered << a;
        m_picks = picks;
        m_fresh = freshIds;
        for (const Track &t : std::as_const(picks)) {
            const QString key = songKey(t.artist, t.title);
            if (!m_shown.contains(key))
                m_shown.insert(key, now);
        }
        m_basedOn = ordered;
        m_generated = QDateTime::currentDateTimeUtc();
        YouTubeService::instance()->setRecommended(m_picks);
        save();
    }
    emit changed();
}

// ---- storage ---------------------------------------------------------------------

void Recommendations::load()
{
    QFile file(stateFile());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (const QJsonValue &v : root.value("plays").toArray()) {
        const QJsonObject o = v.toObject();
        Play p { o.value("id").toString(), o.value("artist").toString(), o.value("title").toString(),
                 o.value("duration").toInt(), o.value("count").toInt(), qint64(o.value("last").toDouble()) };
        if (!p.id.isEmpty())
            m_plays.insert(p.id, p);
    }
    for (const QJsonValue &v : root.value("picks").toArray()) {
        const Track t = YouTubeService::trackFromJson(v.toObject());
        if (!t.path.isEmpty())
            m_picks << t;
    }
    m_basedOn = root.value("basedOn").toVariant().toStringList();
    for (const QJsonValue &v : root.value("fresh").toArray())
        m_fresh.insert(v.toString());
    const QJsonObject shown = root.value("shown").toObject();
    for (auto it = shown.begin(); it != shown.end(); ++it)
        m_shown.insert(it.key(), qint64(it.value().toDouble()));
    m_radioSeeds = root.value("radioSeeds").toVariant().toStringList();
    m_generated = QDateTime::fromString(root.value("generated").toString(), Qt::ISODate);
}

void Recommendations::save() const
{
    QJsonArray plays;
    for (const Play &p : m_plays)
        plays.append(QJsonObject { { "id", p.id }, { "artist", p.artist }, { "title", p.title },
                                   { "duration", p.duration }, { "count", p.count }, { "last", double(p.last) } });
    QJsonArray picks;
    for (const Track &t : m_picks)
        picks.append(YouTubeService::trackJson(t));
    QJsonObject shown;
    for (auto it = m_shown.cbegin(); it != m_shown.cend(); ++it)
        shown.insert(it.key(), double(it.value()));
    const QJsonObject root {
        { "plays", plays },
        { "picks", picks },
        { "basedOn", QJsonArray::fromStringList(m_basedOn) },
        { "fresh", QJsonArray::fromStringList(QStringList(m_fresh.cbegin(), m_fresh.cend())) },
        { "shown", shown },
        { "radioSeeds", QJsonArray::fromStringList(m_radioSeeds) },
        { "generated", m_generated.toString(Qt::ISODate) },
    };
    QSaveFile file(stateFile());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}
