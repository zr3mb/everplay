#include "Radio.h"

#include "Library.h"
#include "Recommendations.h"
#include "soundcloud/SoundCloudService.h"
#include "youtube/YouTubeService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJSEngine>
#include <QSet>

namespace {

constexpr qint64 kHeardMs = 3 * 86'400'000LL;   // heard within this long: to the end of the station
constexpr int kArtistGap = 3;                   // the same artist again only after this many others

QString songKey(const Track &t)
{
    return Recommendations::looseKey(TrackModel::primaryArtist(t.artist)) + QLatin1Char('|')
           + Recommendations::looseKey(t.title);
}

} // namespace

Radio *Radio::instance()
{
    static Radio *inst = new Radio;
    return inst;
}

Radio *Radio::create(QQmlEngine *, QJSEngine *)
{
    Radio *r = instance();
    QJSEngine::setObjectOwnership(r, QJSEngine::CppOwnership);
    return r;
}

Radio::Radio(QObject *parent)
    : QObject(parent)
{
}

void Radio::begin()
{
    if (m_pending++ == 0)
        emit loadingChanged();
}

void Radio::end()
{
    if (--m_pending == 0)
        emit loadingChanged();
}

void Radio::tracksFor(const Track &seed, const QStringList &avoid, Done done)
{
    begin();
    if (seed.source == QLatin1String("youtube") && !seed.path.isEmpty()) {
        youTubeRadio(seed, seed.path, avoid, done);
        return;
    }
    // Local files, Spotify (played through YouTube anyway) and SoundCloud tracks SoundCloud has no
    // station for: the same song on YouTube Music, then its radio.
    const auto viaSearch = [this, seed, avoid, done] {
        YtMusicApi *api = YouTubeService::instance()->api();
        api->search(TrackModel::primaryArtist(seed.artist) + QLatin1Char(' ') + seed.title,
                    [this, seed, avoid, done](const QList<Track> &results, const QString &error) {
            const int best = YtMusicApi::bestMatch(seed, results);
            if (best < 0) {
                end();
                done({}, error.isEmpty() ? tr("this track wasn't found on YouTube Music") : error);
                return;
            }
            youTubeRadio(seed, results.at(best).path, avoid, done);
        }, QStringLiteral("songs"));
    };
    if (seed.source == QLatin1String("soundcloud")) {
        SoundCloudService::instance()->fetchRelated(seed.id, [this, seed, avoid, done, viaSearch](const QStringList &ids, const QString &) {
            QList<Track> tracks;
            TrackModel *model = Library::instance()->tracks();
            for (const QString &id : ids)
                if (const int row = model->rowOf(id); row >= 0)
                    tracks << model->at(row);
            if (tracks.size() < 5) {
                viaSearch();
                return;
            }
            finish(seed, tracks, avoid, done);
        });
        return;
    }
    viaSearch();
}

void Radio::youTubeRadio(const Track &seed, const QString &videoId, const QStringList &avoid, Done done)
{
    YouTubeService::instance()->api()->radio(videoId, [this, seed, avoid, done](const QList<Track> &tracks, const QString &error) {
        if (tracks.isEmpty()) {
            end();
            done({}, error.isEmpty() ? tr("YouTube Music has no radio for it") : error);
            return;
        }
        // Kept in the track model under the seed, so the station can be played and shown.
        YouTubeService::instance()->keepRadio(QStringLiteral("radio:") + seed.id, tracks);
        finish(seed, tracks, avoid, done);
    });
}

void Radio::finish(const Track &seed, QList<Track> tracks, const QStringList &avoid, Done done)
{
    const QSet<QString> avoidIds(avoid.cbegin(), avoid.cend());
    QSet<QString> avoidSongs { songKey(seed) };
    TrackModel *model = Library::instance()->tracks();
    for (const QString &id : avoid)
        if (const int row = model->rowOf(id); row >= 0)
            avoidSongs.insert(songKey(model->at(row)));
    const QSet<QString> heard = Recommendations::instance()->heardSince(QDateTime::currentMSecsSinceEpoch() - kHeardMs);

    // Not the seed, nothing already queued (by id or the same song elsewhere); what was heard lately last.
    QList<Track> fresh, later;
    for (const Track &t : std::as_const(tracks)) {
        const QString key = songKey(t);
        if (t.id == seed.id || avoidIds.contains(t.id) || avoidSongs.contains(key))
            continue;
        avoidSongs.insert(key);
        (heard.contains(t.id) || heard.contains(key) ? later : fresh) << t;
    }

    // The same artist not over and over: a track waits until a few others have played.
    QStringList ids;
    QStringList recent;
    for (QList<Track> *part : { &fresh, &later }) {
        QList<Track> waiting = *part;
        while (!waiting.isEmpty()) {
            int pick = 0;
            for (int i = 0; i < waiting.size(); ++i)
                if (!recent.contains(Recommendations::looseKey(TrackModel::primaryArtist(waiting.at(i).artist)))) {
                    pick = i;
                    break;
                }
            const Track t = waiting.takeAt(pick);
            ids << t.id;
            recent << Recommendations::looseKey(TrackModel::primaryArtist(t.artist));
            if (recent.size() > kArtistGap)
                recent.removeFirst();
        }
    }
    m_stations.insert(seed.id, ids);
    end();
    done(ids, {});
}
