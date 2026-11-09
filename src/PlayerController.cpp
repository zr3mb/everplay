#include "PlayerController.h"

#include <QCoreApplication>
#include <QThread>

#include "AppPaths.h"
#include "Language.h"
#include "Library.h"
#include "Radio.h"
#include "ListeningHistory.h"
#include "Recommendations.h"
#include "TrackModel.h"
#include "audio/Equalizer.h"
#include "lyrics/LyricsService.h"
#include "online/OnlineService.h"
#include "platform/DiscordPresence.h"
#include "soundcloud/SoundCloudService.h"
#include "spotify/SpotifyService.h"
#include "youtube/LocalStreamProxy.h"
#include "youtube/StreamFetch.h"
#include "youtube/YouTubeService.h"

#ifdef EVERPLAY_MPRIS
#  include "platform/Mpris.h"
#endif
#ifdef EVERPLAY_SMTC
#  include "platform/WindowsMediaControls.h"
#endif

#include <QAudioDevice>
#include <QAudioFormat>
#include <QBuffer>
#include <QDataStream>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QtMultimedia/qtaudio.h>

#include <algorithm>
#include <utility>

namespace {

const QStringList kMixStyles = { QStringLiteral("auto"), QStringLiteral("fade"), QStringLiteral("rise"),
                                 QStringLiteral("bass"), QStringLiteral("echo") };

AudioEngine::Style styleFromId(const QString &id)
{
    if (id == QLatin1String("rise"))
        return AudioEngine::Style::Rise;
    if (id == QLatin1String("bass"))
        return AudioEngine::Style::BassSwap;
    if (id == QLatin1String("echo"))
        return AudioEngine::Style::Echo;
    return AudioEngine::Style::Fade;
}

} // namespace

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
    , m_tracks(Library::instance()->tracks())
    , m_analyzer(kBars)
    , m_spectrum(kBars, 0.0)
    , m_targets(kBars, 0.0)
{
    // Every deck plays into a muted output (it sets the playback pace), while the audible
    // sound takes a separate path: samples -> mixer -> equalizer -> AudioEngine (QAudioSink).
    for (int d = 0; d < AudioEngine::kDecks; ++d)
        setupDeck(d);
    // The muted outputs follow the sound to a new device: one left on an unplugged device would stop
    // its deck's clock, and with it the playback.
    connect(&m_engine, &AudioEngine::outputDeviceChanged, this, [this] {
        for (Deck &deck : m_decks)
            deck.clock->setDevice(m_engine.outputDevice());
    });

    connect(&m_cache, &AudioCache::changed, this, [this] {
        ++m_cacheRevision;
        emit cacheChanged();
    });

    connect(&m_engine, &AudioEngine::transitionFinished, this, [this](int fromDeck) {
        Deck &old = m_decks[fromDeck];
        old.player->stop();
        old.player->setSource(QUrl());
        old.trackId.clear();
        old.resolving.clear();
        m_transitionLabel.clear();
        emit transitionChanged();
    });

    auto settings = AppPaths::settings();
    m_volume = std::clamp(settings->value("player/volume", 0.72).toDouble(), 0.0, 1.0);
    m_muted = settings->value("player/muted", false).toBool();
    m_shuffle = settings->value("player/shuffle", false).toBool();
    m_repeat = std::clamp(settings->value("player/repeat", 0).toInt(), 0, 2);
    m_autoplay = settings->value("player/autoplay", true).toBool();
    applyVolume();
    loadMixSettings();

    connect(m_tracks, &QAbstractItemModel::modelReset, this, &PlayerController::onModelReset);
    connect(m_tracks, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex &from, const QModelIndex &to) {
        if (m_index >= from.row() && m_index <= to.row())
            emit colorsChanged();   // e.g. a palette computed from a cover downloaded from the web
    });

    // Tracks from YouTube: the stream URL comes from yt-dlp (asynchronously).
    StreamResolver *resolver = YouTubeService::instance()->resolver();
    connect(resolver, &StreamResolver::resolved, this, [this](const QString &videoId, const QUrl &url) {
        // The stream's server is known: its connection is opened now, not with the first request.
        if (url.host().endsWith(QLatin1String("googlevideo.com")))
            m_net.connectToHostEncrypted(url.host());
        if (m_wantAhead.contains(videoId))
            startAhead(videoId, url, m_wantAhead.take(videoId));
        for (int d = 0; d < AudioEngine::kDecks; ++d) {
            Deck &deck = m_decks[d];
            if (deck.resolving != videoId)
                continue;   // e.g. a prefetch result or the track has already changed
            setStream(d, videoId, url);
        }
        emit bufferingChanged();
    });
    connect(resolver, &StreamResolver::failed, this, [this](const QString &videoId, const QString &error) {
        if (m_decks[standby()].resolving == videoId)
            cancelPreload();   // the next track won't load — the transition will be a plain one
        if (m_decks[m_active].resolving != videoId)
            return;
        m_decks[m_active].resolving.clear();
        emit bufferingChanged();
        skipAfterError(videoId.startsWith(QLatin1String("sc:")) ? error : tr("Couldn't play from YouTube: %1").arg(error));
    });
    // A Spotify track got a different YouTube Music match (chosen by the user or matched again):
    // the playing track switches to it right away, a prepared next track is dropped.
    connect(SpotifyService::instance(), &SpotifyService::matchChanged, this, [this](const QString &trackId) {
        if (m_decks[standby()].trackId == trackId)
            cancelPreload();
        if (m_currentId != trackId || m_decks[m_active].resolving == QStringLiteral("match:") + trackId
            || SpotifyService::instance()->matchedVideoId(trackId).isEmpty())
            return;   // not playing, or still waiting for the match (it'll use the new one)
        load(trackId, m_wantPlay || playing());
    });
    connect(m_tracks, &TrackModel::likedChanged, this, [this](int row, bool) {
        if (row == m_index)
            emit likedChanged();
    });

    m_timer.setInterval(16);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PlayerController::tick);
    m_clock.start();

    DiscordPresence::instance()->attach(this);
    OnlineService::instance()->attach(this);
#ifdef EVERPLAY_MPRIS
    new Mpris(this);   // Linux: MPRIS (system panel, media keys)
#endif
#ifdef EVERPLAY_SMTC
    new WindowsMediaControls(this);   // Windows: media flyout, lock screen, media keys
#endif

    const QVariantMap resume = settings->value("player/resume").toMap();
    for (auto it = resume.cbegin(); it != resume.cend(); ++it)
        m_resume.insert(it.key(), it.value().toLongLong());

    // Labels computed here ("Nothing playing", mix style names) follow the interface language.
    connect(Language::instance(), &Language::changed, this, [this] {
        emit currentChanged();
        emit mixChanged();
    });

    // The first track of a session waited ~0.1 s more than the next ones while Qt Multimedia built
    // its codec list; a moment after start that's done ahead of the first click.
    QTimer::singleShot(1500, this, &PlayerController::warmUp);

    // Restore the last played track (paused, at the same position).
    const QString lastId = settings->value("player/lastId").toString();
    const int row = m_tracks->rowOf(lastId);
    if (row >= 0 && canPlay(row, false)) {
        for (const QVariant &v : Library::instance()->libraryRows())
            m_queue << m_tracks->at(v.toInt()).id;
        m_queuePos = int(m_queue.indexOf(lastId));
        m_pendingSeek = settings->value("player/lastPosition", 0).toLongLong();
        load(lastId, false);
    }
}

PlayerController::~PlayerController()
{
    saveState();
}

void PlayerController::warmUp()
{
    // A tenth of a second of silence (WAV) played into nothing: a player with no audio output
    // makes no sound and doesn't touch the decks.
    constexpr int rate = 8000, samples = rate / 10;
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + samples * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(samples * 2);
    wav.append(QByteArray(samples * 2, '\0'));

    auto *player = new QMediaPlayer(this);
    auto *data = new QBuffer(player);
    data->setData(wav);
    data->open(QIODevice::ReadOnly);
    connect(player, &QMediaPlayer::mediaStatusChanged, player, [player](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia)
            player->deleteLater();
    });
    QTimer::singleShot(5000, player, &QObject::deleteLater);   // in any case
    player->setSourceDevice(data, QUrl(QStringLiteral("warmup.wav")));
    player->play();
}

void PlayerController::setupDeck(int d)
{
    Deck &deck = m_decks[d];
    deck.player = new QMediaPlayer(this);
    deck.clock = new QAudioOutput(m_engine.outputDevice(), this);
    deck.clock->setVolume(0.0f);
    deck.player->setAudioOutput(deck.clock);
    deck.buffers = new QAudioBufferOutput(m_engine.bufferFormat(), this);
    deck.player->setAudioBufferOutput(deck.buffers);
    // Straight to the mixer, on the decoder's thread: a busy GUI thread mustn't hold the sound up.
    connect(deck.buffers, &QAudioBufferOutput::audioBufferReceived, deck.buffers,
            [this, d](const QAudioBuffer &buffer) { m_engine.push(d, buffer); }, Qt::DirectConnection);
    if (qEnvironmentVariableIsSet("EVERPLAY_AUDIO_DEBUG")) {
        connect(deck.buffers, &QAudioBufferOutput::audioBufferReceived, deck.buffers, [](const QAudioBuffer &) {
            static bool told = false;
            if (!std::exchange(told, true))
                qInfo("audio: buffers are emitted on %s thread",
                      QThread::currentThread() == QCoreApplication::instance()->thread() ? "the GUI" : "another");
        }, Qt::DirectConnection);
    }

    connect(deck.player, &QMediaPlayer::playbackStateChanged, this, [this, d] {
        if (d != m_active)
            return;
        if (playing() && m_loadClock.isValid() && qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
            qInfo("stream: deck %d playing %lld ms after the track change", d, m_loadClock.elapsed());
        m_engine.setPaused(!playing());
        emit playingChanged();
        emit positionChanged();
        ensureTicking();
    });
    connect(deck.player, &QMediaPlayer::mediaStatusChanged, this,
            [this, d](QMediaPlayer::MediaStatus status) { onDeckMediaStatus(d, status); });
    connect(deck.player, &QMediaPlayer::durationChanged, this, [this, d] {
        if (d == m_active)
            emit durationChanged();
    });
    connect(deck.player, &QMediaPlayer::positionChanged, this, [this, d] {
        if (d == m_active && !playing())
            emit positionChanged();
    });
    connect(deck.player, &QMediaPlayer::errorOccurred, this, [this, d](QMediaPlayer::Error, const QString &message) {
        if (d == m_active) {
            if (!retryStream(d))
                skipAfterError(tr("Can't play: %1").arg(message));
        } else if (!m_engine.inTransition() && !retryStream(d)) {
            cancelPreload();
        }
    });
}

void PlayerController::saveState() const
{
    auto settings = AppPaths::settings();
    settings->setValue("player/volume", m_volume);
    settings->setValue("player/muted", m_muted);
    settings->setValue("player/shuffle", m_shuffle);
    settings->setValue("player/repeat", m_repeat);
    settings->setValue("player/lastId", m_currentId);
    settings->setValue("player/lastPosition", qint64(position() * 1000));
    rememberPosition();
    settings->sync();
}

void PlayerController::rememberPosition() const
{
    // Mixes, sets and podcasts (15 min and longer) are remembered past their first minute and until
    // the last minute and a half; shorter tracks just start over.
    constexpr qint64 kLong = 15 * 60 * 1000;
    const Track *t = current();
    if (!t || live())
        return;
    const qint64 duration = player().duration() > 0 ? player().duration() : qint64(t->duration) * 1000;
    const qint64 pos = m_pendingSeek >= 0 ? m_pendingSeek : player().position();
    if (duration < kLong)
        return;
    if (pos > 60000 && pos < duration - 90000)
        m_resume.insert(m_currentId, pos);
    else
        m_resume.remove(m_currentId);
    storeResume();
}

void PlayerController::storeResume() const
{
    while (m_resume.size() > 60)
        m_resume.erase(m_resume.begin());
    QVariantMap stored;
    for (auto it = m_resume.cbegin(); it != m_resume.cend(); ++it)
        stored.insert(it.key(), it.value());
    AppPaths::settings()->setValue("player/resume", stored);
}

// ---- current track info ----------------------------------------------------

const Track *PlayerController::current() const
{
    return m_tracks->isValidRow(m_index) ? &m_tracks->at(m_index) : nullptr;
}

QString PlayerController::title() const
{
    return current() ? current()->title : tr("Nothing playing");
}

QString PlayerController::artist() const
{
    return current() ? current()->artist : tr("Pick a track from the library");
}

QString PlayerController::artistId() const
{
    return current() ? current()->artistId : QString();
}

QString PlayerController::album() const    { return current() ? current()->album : QString(); }
QString PlayerController::albumKey() const { return current() ? TrackModel::albumKey(*current()) : QString(); }
QString PlayerController::source() const   { return current() ? current()->source : QString(); }
QString PlayerController::quality() const  { return current() ? current()->quality : QString(); }
QString PlayerController::cover() const    { return current() ? current()->cover : QString(); }
QColor PlayerController::colorA() const    { return current() ? current()->colorA : QColor("#9fb5a8"); }
QColor PlayerController::colorB() const    { return current() ? current()->colorB : QColor("#3e4f5a"); }
int PlayerController::seed() const         { return current() ? current()->seed : 0; }
bool PlayerController::liked() const       { return current() && current()->liked; }
bool PlayerController::live() const        { return current() && current()->source == QLatin1String("radio"); }

int PlayerController::duration() const
{
    if (current() && current()->duration > 0)
        return current()->duration;
    return int(player().duration() / 1000);
}

double PlayerController::position() const
{
    if (m_pendingSeek >= 0)
        return m_pendingSeek / 1000.0;
    // The decoder is ahead of the speakers by what waits in the buffers (~0.3 s) — lyrics and the
    // progress bar follow what is heard.
    return std::max(0.0, player().position() / 1000.0 - m_heardLag);
}

bool PlayerController::buffering() const
{
    if (!m_decks[m_active].resolving.isEmpty())
        return true;
    const auto s = player().mediaStatus();
    return s == QMediaPlayer::LoadingMedia || s == QMediaPlayer::StalledMedia;
}

QVariantList PlayerController::rowsAhead(int limit) const
{
    QVariantList out;
    if (m_queue.isEmpty())
        return out;
    for (int k = 1; k < m_queue.size() && out.size() < limit; ++k) {
        int pos = m_queuePos + k;
        if (pos >= m_queue.size()) {
            if (m_repeat != 1 || inRoom())
                break;
            pos %= m_queue.size();
        }
        const int row = m_tracks->rowOf(m_queue.at(pos));
        if (row >= 0)
            out << row;
    }
    return out;
}

int PlayerController::queueLeft() const
{
    if (m_queue.isEmpty())
        return 0;
    if (m_repeat == 1 && !inRoom())   // repeating the queue: everything else comes round again
        return int(m_queue.size()) - 1;
    return std::max(0, int(m_queue.size()) - 1 - m_queuePos);
}

int PlayerController::queueSeconds() const
{
    int total = 0;
    const int left = queueLeft();
    for (int k = 1; k <= left; ++k) {
        const int row = m_tracks->rowOf(m_queue.at((m_queuePos + k) % m_queue.size()));
        if (row >= 0)
            total += m_tracks->at(row).duration;
    }
    return total;
}

// ---- settings -----------------------------------------------------------------

void PlayerController::setVolume(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    if (qFuzzyCompare(v + 1.0, m_volume + 1.0))
        return;
    m_volume = v;
    applyVolume();
    emit volumeChanged();
}

void PlayerController::applyVolume()
{
    // The slider works on an "audible" scale, the audio output expects a linear one.
    m_engine.setVolume(m_muted ? 0.0f
                               : float(QtAudio::convertVolume(m_volume, QtAudio::LogarithmicVolumeScale,
                                                              QtAudio::LinearVolumeScale)));
}

void PlayerController::setMuted(bool m)
{
    if (m == m_muted)
        return;
    m_muted = m;
    applyVolume();
    emit mutedChanged();
}

void PlayerController::setShuffle(bool s)
{
    if (s == m_shuffle)
        return;
    m_shuffle = s;
    if (!m_engine.inTransition())
        cancelPreload();   // the planned next track may have been picked a different way
    emit shuffleChanged();
}

void PlayerController::setRepeatMode(int r)
{
    r = std::clamp(r, 0, 2);
    if (r == m_repeat)
        return;
    m_repeat = r;
    if (!m_engine.inTransition())
        cancelPreload();
    emit repeatModeChanged();
    emit queueChanged();
}

// ---- controls -----------------------------------------------------------------

bool PlayerController::canPlay(int row, bool explain)
{
    if (!m_tracks->isValidRow(row))
        return false;
    const Track &t = m_tracks->at(row);
    if (t.demo) {
        if (explain) {
            emit notice(t.source == QLatin1String("youtube")
                            ? tr("YouTube Music isn't connected yet — this is a sample track")
                            : tr("Internet radio is coming in a later stage"),
                        t.source == QLatin1String("youtube") ? QStringLiteral("playCircle") : QStringLiteral("radio"));
        }
        return false;
    }
    if (t.source == QLatin1String("soundcloud"))
        return true;   // SoundCloud's own streams — no yt-dlp needed
    if (t.source == QLatin1String("youtube") || t.source == QLatin1String("spotify")) {
        if (!YouTubeService::instance()->engineReady()) {
            if (explain)
                emit notice(tr("Download the playback engine (yt-dlp) in Sources → YouTube Music"),
                            QStringLiteral("playCircle"));
            return false;
        }
        return true;
    }
    if (!QFileInfo::exists(t.path)) {
        if (explain)
            emit notice(tr("The file doesn't exist: %1").arg(QFileInfo(t.path).fileName()), QStringLiteral("close"));
        return false;
    }
    return true;
}

void PlayerController::load(const QString &id, bool autoplay, bool flushAudio)
{
    const int row = m_tracks->rowOf(id);
    if (row < 0)
        return;

    // A track change by the user interrupts the transition and drops the prepared next track.
    stopTransition();
    cancelPreload();

    if (!m_currentId.isEmpty() && m_currentId != id) {
        rememberPosition();
        m_history << m_currentId;
        if (m_history.size() > 200)
            m_history.removeFirst();
    }
    m_currentId = id;
    m_index = row;
    // A long recording started again goes on from where it was left.
    if (m_pendingSeek < 0) {
        if (const qint64 at = m_resume.value(id, -1); at > 0) {
            m_pendingSeek = at;
            const QTime time = QTime(0, 0).addMSecs(int(at));
            emit notice(tr("Continuing from %1").arg(time.toString(at >= 3600000 ? QStringLiteral("H:mm:ss") : QStringLiteral("m:ss"))),
                        QStringLiteral("play"));
        }
    }
    // On an automatic transition we don't clear the buffer — the end of the previous track plays out smoothly.
    if (flushAudio) {
        m_engine.flush();
        m_analyzer.clear();
    }
    m_engine.setActiveDeck(m_active);
    m_engine.resetLevels(m_active);
    m_silence = 0;
    m_wantPlay = autoplay;
    m_lastPosMs = -1;
    m_stall = 0;

    Deck &deck = m_decks[m_active];
    deck.trackId = id;
    m_loadClock.start();
    const Track &t = m_tracks->at(row);
    if (t.source == QLatin1String("youtube") || t.source == QLatin1String("spotify") || t.source == QLatin1String("soundcloud")) {
        m_autoplayAfterResolve = autoplay;
        Library::instance()->ensurePalette(row);
        startStream(m_active, t);
    } else {
        deck.resolving.clear();
        deck.player->setSource(QUrl::fromLocalFile(t.path));
        if (autoplay)
            deck.player->play();
    }

    emitTrackChanged();
    emit bufferingChanged();

    if (m_context.startsWith(QLatin1String("radio:")))
        extendRadio();
    else
        prepareAutoRadio();
}

void PlayerController::emitTrackChanged()
{
    if (inRoom() && !m_roomApplying && !m_roomTrack.isEmpty() && m_roomTrack != m_currentId && m_room.movedOn)
        m_room.movedOn(m_roomTrack);
    m_roomTrack = m_currentId;
    LyricsService::instance()->setTrack(current());
    emit currentChanged();
    emit colorsChanged();
    emit durationChanged();
    emit likedChanged();
    emit positionChanged();
    emit queueChanged();
}

void PlayerController::prefetchNext()
{
    // We prepare the URL of the next YouTube track in advance — the transition is then instant.
    if (m_queue.isEmpty())
        return;
    int next;
    if (m_shuffle && m_queue.size() > 1) {
        // With shuffle we pick the next track right away, so there's time to prepare its URL.
        if (!m_queue.contains(m_shuffleNextId) || m_shuffleNextId == m_currentId)
            m_shuffleNextId = m_queue.at(nextQueuePos());
        next = int(m_queue.indexOf(m_shuffleNextId));
    } else {
        next = m_queuePos + 1 < m_queue.size() ? m_queuePos + 1 : (m_repeat == 1 ? 0 : -1);
    }
    if (next < 0)
        return;
    // The whole next track comes ahead (into the disk cache), so skipping to it plays at once.
    prefetchTrack(m_tracks->rowOf(m_queue.at(next)), m_cache.limitBytes() > 0);
}

void PlayerController::prefetch(int row)
{
    prefetchTrack(row);
}

void PlayerController::prefetchTrack(int row, bool full)
{
    if (!m_tracks->isValidRow(row) || m_tracks->at(row).demo)
        return;
    // The URL first; for a YouTube stream then its start (or all of it, see startAhead).
    const auto want = [this, full](const QString &videoId) {
        if (videoId.isEmpty() || m_cache.has(videoId))
            return;
        StreamResolver *resolver = YouTubeService::instance()->resolver();
        resolver->prefetch(videoId);
        if (const QUrl url = resolver->cachedUrl(videoId); url.isValid())
            startAhead(videoId, url, full);
        else
            m_wantAhead[videoId] = m_wantAhead.value(videoId) || full;
    };
    const Track &t = m_tracks->at(row);
    if (t.source == QLatin1String("youtube")) {
        want(t.path);
    } else if (t.source == QLatin1String("soundcloud")) {
        YouTubeService::instance()->resolver()->prefetch(t.id);
    } else if (t.source == QLatin1String("spotify")) {
        SpotifyService::instance()->match(t.id, want);
    }
}

void PlayerController::startAhead(const QString &videoId, const QUrl &url, bool full)
{
    if (m_cache.has(videoId) || !StreamFetch::suits(url))
        return;
    for (const Deck &deck : m_decks)
        if (deck.fetch && deck.fetch->url() == url)
            return;   // a deck is fetching it already
    if (StreamFetch *existing = m_ahead.value(videoId)) {
        if (full)
            existing->continueAll();
        return;
    }
    // Only the latest few starts are kept: an older one is dropped (and its download stopped).
    int heads = 0;
    for (const QString &id : std::as_const(m_aheadOrder))
        heads += m_ahead.value(id)->headOnly() ? 1 : 0;
    for (int i = 0; heads >= 3 && i < m_aheadOrder.size(); ) {
        StreamFetch *old = m_ahead.value(m_aheadOrder.at(i));
        if (!old->headOnly()) {
            ++i;
            continue;
        }
        m_ahead.remove(m_aheadOrder.takeAt(i));
        delete old;
        --heads;
    }
    auto *fetch = new StreamFetch(&m_net, url, !full, this);
    m_ahead.insert(videoId, fetch);
    m_aheadOrder << videoId;
    connect(fetch, &StreamFetch::finished, this, [this, videoId, fetch](bool ok) {
        if (ok)
            m_cache.store(videoId, fetch->takeData());
        m_ahead.remove(videoId);
        m_aheadOrder.removeAll(videoId);
        fetch->deleteLater();
    });
}

StreamFetch *PlayerController::takeAhead(const QString &videoId, const QUrl &url)
{
    StreamFetch *fetch = m_ahead.take(videoId);
    if (!fetch)
        return nullptr;
    m_aheadOrder.removeAll(videoId);
    disconnect(fetch, nullptr, this, nullptr);
    if (fetch->url() != url) {   // a different URL by now (a fresh one after an error)
        delete fetch;
        return nullptr;
    }
    fetch->continueAll();
    return fetch;
}

QString PlayerController::outputDeviceName() const
{
    return m_engine.outputDevice().description();
}

bool PlayerController::cached(int row) const
{
    if (!m_tracks->isValidRow(row))
        return false;
    const Track &t = m_tracks->at(row);
    if (t.source != QLatin1String("youtube") && t.source != QLatin1String("spotify"))
        return false;
    const QString id = streamIdOf(t);
    return !id.isEmpty() && m_cache.has(id);
}

QString PlayerController::streamIdOf(const Track &t) const
{
    if (t.source == QLatin1String("youtube"))
        return t.path;
    if (t.source == QLatin1String("soundcloud"))
        return t.id;   // "sc:<id>" — StreamResolver hands it to SoundCloudService
    if (t.source == QLatin1String("spotify"))
        return SpotifyService::instance()->matchedVideoId(t.id);
    return {};
}

void PlayerController::startStream(int d, const Track &t)
{
    // A network track: YouTube gets its stream URL from yt-dlp, a Spotify track is first matched to
    // YouTube Music. The deck stays "resolving" until the URL arrives (the resolved / failed signals).
    Deck &deck = m_decks[d];
    deck.player->stop();
    deck.player->setSource(QUrl());
    if (t.source == QLatin1String("spotify") && !SpotifyService::instance()->matchKnown(t.id)) {
        const QString marker = QStringLiteral("match:") + t.id;
        deck.resolving = marker;
        SpotifyService::instance()->match(t.id, [this, d, marker, id = t.id](const QString &) {
            if (m_decks[d].resolving != marker)
                return;   // the deck has moved on
            if (const int row = m_tracks->rowOf(id); row >= 0)
                startStream(d, m_tracks->at(row));
        });
        return;
    }
    const QString videoId = streamIdOf(t);
    if (videoId.isEmpty()) {
        deck.resolving.clear();
        if (d == m_active)
            skipAfterError(tr("Not found on YouTube Music: %1").arg(t.title));
        else
            cancelPreload();
        return;
    }
    // Played before and kept on disk: no network at all.
    m_cache.played(videoId);
    QSet<QString> inUse{ videoId };
    if (const int other = m_tracks->rowOf(m_decks[1 - d].trackId); other >= 0)
        inUse << streamIdOf(m_tracks->at(other));
    m_cache.setInUse(inUse);
    if (m_cache.has(videoId)) {
        delete std::exchange(deck.fetch, nullptr);
        deck.resolving.clear();
        if (d == m_active && qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
            qInfo("stream: deck %d plays %s from the disk cache", d, qPrintable(videoId));
        deck.player->setSource(QUrl::fromLocalFile(m_cache.pathOf(videoId)));
        if (d == m_active && m_autoplayAfterResolve)
            deck.player->play();
        emit bufferingChanged();
        return;
    }
    StreamResolver *resolver = YouTubeService::instance()->resolver();
    if (d != m_active) {
        if (const QUrl url = resolver->cachedUrl(videoId); url.isValid()) {
            setStream(d, videoId, url);   // the prepared next track loads straight away
            return;
        }
    }
    deck.resolving = videoId;
    resolver->resolve(videoId);
}

void PlayerController::setStream(int d, const QString &videoId, const QUrl &url)
{
    // A YouTube stream comes into memory first (see StreamFetch) — the deck stays "resolving" meanwhile,
    // so it shows as loading and play / seek wait for it. Other streams play from the URL.
    Deck &deck = m_decks[d];
    if (d == m_active && qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
        qInfo("stream: deck %d has its URL %lld ms after the track change", d, m_loadClock.elapsed());
    delete std::exchange(deck.fetch, nullptr);
    if (!StreamFetch::suits(url)) {
        deck.resolving.clear();
        // Audio relayed by the Everplay server (Android) goes through the local proxy: Qt fetches it,
        // FFmpeg only reads plain HTTP from 127.0.0.1.
        const bool relayed = url.path().startsWith(QLatin1String("/api/v1/stream/"));
        deck.player->setSource(relayed ? LocalStreamProxy::instance()->wrap(url) : url);
        if (d == m_active && m_autoplayAfterResolve)
            deck.player->play();
        return;
    }
    const QString marker = QStringLiteral("fetch:") + videoId;
    deck.resolving = marker;
    // Already coming in when the cursor rested on it, or as the next track: that fetch goes on here.
    StreamFetch *fetch = takeAhead(videoId, url);
    const bool adopted = fetch != nullptr;
    if (!fetch)
        fetch = new StreamFetch(&m_net, url, false, this);
    deck.fetch = fetch;
    const auto debug = [this, d](const char *what) {
        if (d == m_active && qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
            qInfo("stream: deck %d %s %lld ms after the track change", d, what, m_loadClock.elapsed());
    };
    const auto playDevice = [this, d, url](QIODevice *device) {
        Deck &deck = m_decks[d];
        delete deck.previousMemory;
        deck.previousMemory = std::exchange(deck.memory, device);
        deck.player->setSourceDevice(device, url);
        if (d == m_active && m_autoplayAfterResolve)
            deck.player->play();
        emit bufferingChanged();
    };
    // The first part is in: playback starts while the rest keeps coming (reads past it wait for it).
    const auto onHead = [this, d, marker, fetch, debug, playDevice, adopted] {
        Deck &deck = m_decks[d];
        if (deck.fetch != fetch || deck.resolving != marker)
            return;   // the deck has moved on
        deck.resolving.clear();
        debug(adopted ? "plays a start fetched ahead" : "plays while downloading");
        auto *device = new StreamDevice(fetch->data(), this);
        device->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        playDevice(device);
    };
    connect(fetch, &StreamFetch::headReady, this, onHead);
    connect(fetch, &StreamFetch::finished, this, [this, d, marker, url, videoId, fetch, debug, playDevice](bool ok) {
        Deck &deck = m_decks[d];
        fetch->deleteLater();
        const QByteArray bytes = ok ? fetch->takeData() : QByteArray();
        if (ok)
            m_cache.store(videoId, bytes);   // shares the bytes; written in the background
        if (deck.fetch != fetch)
            return;   // the deck has moved on
        deck.fetch = nullptr;
        debug(ok ? "fetched (ok)" : "fetched (failed)");
        if (deck.resolving != marker)
            return;   // already playing while it downloaded (a failure then shows as a player error)
        deck.resolving.clear();
        if (ok) {
            // It came whole before any part was announced (a server that ignored the ranges)
            auto *buffer = new QBuffer(this);
            buffer->setData(bytes);
            buffer->open(QIODevice::ReadOnly);
            playDevice(buffer);
        } else {
            deck.player->setSource(url);   // couldn't fetch it — streamed as before
            if (d == m_active && m_autoplayAfterResolve)
                deck.player->play();
            emit bufferingChanged();
        }
    });
    if (adopted && fetch->headIsReady())
        onHead();
}

bool PlayerController::retryStream(int d)
{
    // The stream URL couldn't be opened (expired, bound to another address, refused for this YouTube
    // client): get a fresh one — through the account when signed in — once per track before giving up.
    Deck &deck = m_decks[d];
    const int row = m_tracks->rowOf(deck.trackId);
    if (row < 0)
        return false;
    const QString videoId = streamIdOf(m_tracks->at(row));
    if (videoId.isEmpty())
        return false;
    if (deck.resolving == videoId)
        return true;   // already retrying (errorOccurred and InvalidMedia come together)
    if (m_streamRetried.contains(videoId))
        return false;
    m_streamRetried.insert(videoId);
    deck.player->stop();
    deck.player->setSource(QUrl());
    m_cache.remove(videoId);   // a damaged copy on disk
    deck.resolving = videoId;
    if (d == m_active)
        m_autoplayAfterResolve = m_wantPlay;
    YouTubeService::instance()->resolver()->resolveAgain(videoId);
    return true;
}

void PlayerController::playFrom(const QVariantList &rows, int row, const QString &context)
{
    if (roomIntercept(QStringLiteral("play"), row))
        return;
    if (!canPlay(row, true))
        return;

    const QString id = m_tracks->at(row).id;
    QStringList queue = queueIds(rows);
    if (!queue.contains(id))
        queue.prepend(id);
    m_queue = queue;
    m_queuePos = int(m_queue.indexOf(id));
    if (context != m_context) {
        m_context = context;
        emit mixChanged();
    }

    if (id == m_currentId) {
        if (!m_engine.inTransition())
            cancelPreload();
        if (!playing())
            player().play();
        emit queueChanged();
        return;
    }
    m_pendingSeek = -1;
    load(id, true);
}

void PlayerController::playShuffled(const QVariantList &rows, const QString &context)
{
    if (roomIntercept(QStringLiteral("queue"), rows))
        return;
    QStringList ids = queueIds(rows);
    std::shuffle(ids.begin(), ids.end(), *QRandomGenerator::global());
    // The first one that can be played starts (not a sample track; the playback engine is ready).
    for (int i = 0; i < ids.size(); ++i) {
        const int row = m_tracks->rowOf(ids.at(i));
        if (!canPlay(row, i == ids.size() - 1))
            continue;
        ids.move(i, 0);
        if (ids.first() == m_currentId)
            ids.move(0, ids.size() - 1);   // something else than what's playing now
        playFrom(rows, m_tracks->rowOf(ids.first()), context);
        // playFrom keeps the list's order — replace it with the shuffled one around the new position.
        m_queue = ids;
        m_queuePos = 0;
        emit queueChanged();
        return;
    }
}

void PlayerController::playIndex(int row)
{
    playFrom(Library::instance()->libraryRows(), row, QStringLiteral("library"));
}

void PlayerController::jumpTo(int row)
{
    if (roomIntercept(QStringLiteral("play"), row))
        return;
    if (!canPlay(row, true))
        return;
    const QString id = m_tracks->at(row).id;
    const int pos = int(m_queue.indexOf(id));
    if (pos < 0) {
        playIndex(row);
        return;
    }
    m_queuePos = pos;
    m_pendingSeek = -1;
    load(id, true);
}

void PlayerController::play()
{
    if (roomIntercept(QStringLiteral("resume")))
        return;
    if (!hasTrack()) {
        const QVariantList local = Library::instance()->libraryRows();
        if (!local.isEmpty())
            playFrom(local, local.first().toInt(), QStringLiteral("library"));
        else
            emit notice(tr("Add a music folder to start listening"), QStringLiteral("folder"));
        return;
    }
    m_wantPlay = true;
    if (!m_decks[m_active].resolving.isEmpty()) {
        m_autoplayAfterResolve = true;
        return;
    }
    if (m_engine.inTransition())
        m_decks[standby()].player->play();   // the previous track still playing out
    player().play();
}

void PlayerController::pause()
{
    if (roomIntercept(QStringLiteral("pause"), position()))
        return;
    m_wantPlay = false;
    m_autoplayAfterResolve = false;
    if (m_engine.inTransition())
        m_decks[standby()].player->pause();
    player().pause();
}

void PlayerController::toggle()
{
    const bool active = playing() || (!m_decks[m_active].resolving.isEmpty() && m_autoplayAfterResolve);
    active ? pause() : play();
}

void PlayerController::next()
{
    if (roomIntercept(QStringLiteral("next")))
        return;
    advance(true);
}

int PlayerController::nextQueuePos() const
{
    if (m_queue.isEmpty())
        return -1;
    if (m_shuffle && !inRoom() && m_queue.size() > 1) {
        if (const int planned = int(m_queue.indexOf(m_shuffleNextId)); planned >= 0 && planned != m_queuePos)
            return planned;   // the one whose URL we've already prepared
        int pos;
        do {
            pos = int(QRandomGenerator::global()->bounded(int(m_queue.size())));
        } while (pos == m_queuePos);
        return pos;
    }
    const int pos = m_queuePos + 1;
    if (pos < m_queue.size())
        return pos;
    return m_repeat == 0 || inRoom() ? -1 : 0;
}

void PlayerController::advance(bool userInitiated)
{
    if (m_queue.isEmpty())
        return;

    int pos;
    if (!userInitiated && m_plannedPos >= 0 && m_plannedPos < m_queue.size()) {
        pos = m_plannedPos;   // the same choice that was prepared for the mix (matters with shuffle)
    } else if (m_shuffle && !inRoom() && m_queue.size() > 1) {
        pos = nextQueuePos();
    } else {
        pos = m_queuePos + 1;
        if (pos >= m_queue.size() && inRoom()) {
            // The room's queue ran out here first — the room decides what comes next.
            if (m_room.movedOn)
                m_room.movedOn(m_currentId);
            player().pause();
            return;
        }
        if (pos >= m_queue.size()) {
            if (!userInitiated && m_repeat == 0) {
                if (continueAfterQueue())
                    return;
                // Nothing to continue with — stop at the start of the last track.
                player().pause();
                player().setPosition(0);
                emit positionChanged();
                return;
            }
            pos = 0;
        }
    }
    m_queuePos = pos;
    m_pendingSeek = -1;
    m_shuffleNextId.clear();
    load(m_queue.at(pos), true, userInitiated);
}

QStringList PlayerController::queueIds(const QVariantList &rows)
{
    QStringList ids;
    for (const QVariant &v : rows) {
        const int r = v.toInt();
        if (m_tracks->isValidRow(r) && !m_tracks->at(r).demo && !ids.contains(m_tracks->at(r).id))
            ids << m_tracks->at(r).id;
    }
    return ids;
}

void PlayerController::startQueue(const QStringList &ids, int pos, const QString &context)
{
    m_queue = ids;
    m_queuePos = pos;
    if (context != m_context) {
        m_context = context;
        emit mixChanged();
    }
    m_pendingSeek = -1;
    m_shuffleNextId.clear();
    load(m_queue.at(pos), true, false);
}

bool PlayerController::continueAfterQueue()
{
    Library *lib = Library::instance();

    // 1. The queue is a snapshot from the moment of the click — a YouTube playlist may have loaded
    //    more pages since then. Continue in the same playlist if it has grown.
    if (!m_context.isEmpty()) {
        const QStringList ids = queueIds(lib->rowsForContext(m_context));
        const int at = int(ids.indexOf(m_currentId));
        if (at >= 0 && at + 1 < ids.size()) {
            startQueue(ids, at + 1, m_context);
            return true;
        }
    }
    if (!m_autoplay)
        return false;

    // 2. A radio of the track that has just ended — something new rather than the same library again.
    if (const Track *t = current(); t && t->id == m_autoRadioSeed) {
        QStringList ids;
        for (const QString &id : std::as_const(m_autoRadio))
            if (m_tracks->rowOf(id) >= 0)
                ids << id;
        if (!ids.isEmpty()) {
            emit notice(tr("Up next: a radio of %1").arg(t->title), QStringLiteral("radio"));
            startQueue(ids, 0, QStringLiteral("radio:") + t->id);
            return true;
        }
    }

    // Without one (no network, nothing like it found):
    auto startAt = [this](const QStringList &ids) {
        return m_shuffle ? int(QRandomGenerator::global()->bounded(int(ids.size()))) : 0;
    };

    // 3. The next non-empty playlist (in sidebar order) after the one that has just ended.
    const int current = lib->playlistIndex(m_context);
    if (current >= 0) {
        for (int step = 1; step < lib->playlistCount(); ++step) {
            const int i = (current + step) % lib->playlistCount();
            QStringList ids = queueIds(lib->playlistRows(i));
            ids.removeAll(m_currentId);
            if (ids.isEmpty())
                continue;
            emit notice(tr("Up next: %1").arg(lib->playlistName(i)), QStringLiteral("mix"));
            startQueue(ids, startAt(ids), lib->playlistContext(i));
            return true;
        }
    }

    // 4. Otherwise (album, artist, search results…) — the whole library from a random track.
    QStringList ids = queueIds(lib->libraryRows());
    ids.removeAll(m_currentId);
    if (ids.isEmpty())
        return false;
    emit notice(tr("Up next: your library"), QStringLiteral("mix"));
    startQueue(ids, int(QRandomGenerator::global()->bounded(int(ids.size()))), QStringLiteral("library"));
    return true;
}

void PlayerController::startRadio(int row)
{
    if (!m_tracks->isValidRow(row))
        return;
    const Track seed = m_tracks->at(row);
    if (seed.demo || seed.source == QLatin1String("radio")) {
        emit notice(tr("A radio can't start from this track"), QStringLiteral("radio"));
        return;
    }
    if (inRoom()) {
        emit notice(tr("A radio can't start in a listening room"), QStringLiteral("group"));
        return;
    }
    const QString context = QStringLiteral("radio:") + seed.id;
    if (seed.id == m_currentId) {
        // What plays goes on; only what comes after it changes.
        m_queue = { seed.id };
        m_queuePos = 0;
        if (m_context != context) {
            m_context = context;
            emit mixChanged();
        }
        if (!m_engine.inTransition())
            cancelPreload();
        if (!playing())
            player().play();
        emit queueChanged();
    } else {
        if (!canPlay(row, true))
            return;
        m_pendingSeek = -1;
        startQueue({ seed.id }, 0, context);
    }
    emit notice(tr("Radio: %1").arg(seed.title), QStringLiteral("radio"));
    m_radioExtending = true;
    Radio::instance()->tracksFor(seed, m_history.mid(std::max<qsizetype>(0, m_history.size() - 50)),
                                 [this, context, title = seed.title](const QStringList &ids, const QString &error) {
        m_radioExtending = false;
        if (m_context != context)
            return;   // something else plays by now
        if (ids.isEmpty()) {
            emit notice(tr("Couldn't start a radio of %1: %2").arg(title, error), QStringLiteral("close"));
            return;
        }
        for (const QString &id : ids)
            if (!m_queue.contains(id))
                m_queue << id;
        emit queueChanged();
    });
}

void PlayerController::extendRadio()
{
    if (m_radioExtending || inRoom() || m_queuePos < m_queue.size() - 3)
        return;
    const Track *t = current();
    if (!t)
        return;
    m_radioExtending = true;
    QStringList avoid = m_queue;
    avoid << m_history.mid(std::max<qsizetype>(0, m_history.size() - 100));
    Radio::instance()->tracksFor(*t, avoid, [this, context = m_context](const QStringList &ids, const QString &) {
        m_radioExtending = false;
        if (m_context != context)
            return;
        int added = 0;
        for (const QString &id : ids) {
            if (m_queue.contains(id) || m_history.contains(id))
                continue;
            m_queue << id;
            if (++added >= 25)
                break;
        }
        if (added > 0)
            emit queueChanged();
    });
}

void PlayerController::prepareAutoRadio()
{
    const Track *t = current();
    if (!m_autoplay || inRoom() || m_repeat != 0 || !t || t->demo || t->source == QLatin1String("radio")
        || m_queuePos != m_queue.size() - 1 || t->id == m_autoRadioSeed)
        return;
    m_autoRadioSeed = t->id;
    m_autoRadio.clear();
    QStringList avoid = m_queue;
    avoid << m_history.mid(std::max<qsizetype>(0, m_history.size() - 100));
    Radio::instance()->tracksFor(*t, avoid, [this, seed = t->id](const QStringList &ids, const QString &) {
        if (m_autoRadioSeed == seed)
            m_autoRadio = ids;
    });
}

void PlayerController::skipAfterError(const QString &message)
{
    if (!m_currentId.isEmpty() && m_skipPending == m_currentId)
        return;   // the same failure reported twice (errorOccurred + InvalidMedia)
    emit notice(message, QStringLiteral("close"));
    if (inRoom())
        return;   // the room moves on for everyone; skipping here alone would only be pulled back
    // Skip only when music was expected and there's somewhere to go; stop after a few failures in a row
    // (e.g. no network) instead of cycling through the whole queue.
    if (!m_wantPlay || m_queue.size() <= 1)
        return;
    if (++m_failures > 4) {
        m_failures = 0;
        m_wantPlay = false;
        return;
    }
    m_skipPending = m_currentId;
    QTimer::singleShot(1200, this, [this, id = m_currentId] {
        m_skipPending.clear();
        if (m_currentId == id && m_wantPlay)
            advance(false);
    });
}

void PlayerController::countListening(double dt)
{
    // A track counts as listened to after 30 s of playback (half of a shorter one) — the taste the
    // recommendations on the home page follow.
    if (m_listenedId != m_currentId) {
        m_listenedId = m_currentId;
        m_listened = 0;
        m_listenCounted = false;
    }
    const Track *t = current();
    if (!t || !playing() || live())
        return;
    if (m_listenCounted) {
        ListeningHistory::instance()->addListening(m_listenedId, dt);   // the minutes in the stats
        return;
    }
    m_listened += dt;
    const double total = player().duration() / 1000.0;
    if (m_listened < (total > 0 ? std::min(30.0, total * 0.5) : 30.0))
        return;
    m_listenCounted = true;
    Recommendations::instance()->notePlayed(*t);
    ListeningHistory::instance()->notePlay(*t, m_listened);
}

void PlayerController::checkStall(double dt)
{
    // A network stream can get stuck (the URL expired, the connection dropped): the player still says
    // "playing", but the position stops moving and EndOfMedia never comes — the music would just stop.
    if (!playing() || live() || !m_decks[m_active].resolving.isEmpty() || m_engine.inTransition()) {
        m_lastPosMs = -1;
        m_stall = 0;
        return;
    }
    const qint64 pos = player().position();
    if (pos != m_lastPosMs) {
        m_lastPosMs = pos;
        m_stall = 0;
        if (pos > 3000)
            m_failures = 0;   // the track really plays
        return;
    }
    m_stall += dt;
    if (m_stall < 8.0)
        return;
    m_stall = 0;
    m_lastPosMs = -1;

    const double total = player().duration() / 1000.0;
    if (total <= 0 || total - pos / 1000.0 < 15.0) {
        // Stuck at the end — treat it as the end of the track.
        if (m_repeat == 2 && !inRoom())
            seek(0.0);
        else
            advance(false);
        return;
    }
    // Stuck in the middle: reload the track from the same spot (YouTube gets a fresh URL).
    if (++m_failures > 2) {
        skipAfterError(tr("The stream stopped responding"));
        return;
    }
    const Track *t = current();
    if (t && !streamIdOf(*t).isEmpty())
        YouTubeService::instance()->resolver()->forget(streamIdOf(*t));
    const QString id = m_currentId;
    m_currentId.clear();   // load() of the same id would otherwise be treated as "already playing"
    load(id, true);
    m_pendingSeek = pos;
}

void PlayerController::setAutoplay(bool on)
{
    if (on == m_autoplay)
        return;
    m_autoplay = on;
    AppPaths::settings()->setValue("player/autoplay", on);
    emit mixChanged();
}

void PlayerController::previous()
{
    if (roomIntercept(QStringLiteral("previous")))
        return;
    if (position() > 3.0 || m_queue.isEmpty()) {
        seek(0.0);
        return;
    }
    QString id;
    if (m_shuffle && !m_history.isEmpty()) {
        id = m_history.takeLast();
        m_queuePos = int(m_queue.indexOf(id));
    } else {
        m_queuePos = (m_queuePos - 1 + int(m_queue.size())) % int(m_queue.size());
        id = m_queue.at(m_queuePos);
    }
    const QString before = m_currentId;
    m_pendingSeek = -1;
    load(id, true);
    if (!m_history.isEmpty() && m_history.last() == before && m_shuffle)
        m_history.removeLast();   // going back shouldn't add to the history
}

void PlayerController::seek(double seconds)
{
    if (!hasTrack() || roomIntercept(QStringLiteral("seek"), seconds))
        return;
    stopTransition();
    const qint64 ms = qint64(std::max(0.0, seconds) * 1000);
    if (player().mediaStatus() == QMediaPlayer::LoadingMedia || player().mediaStatus() == QMediaPlayer::NoMedia)
        m_pendingSeek = ms;
    else
        player().setPosition(ms);
    m_engine.flush();
    m_engine.setActiveDeck(m_active);
    // Seeking before the ending: the prepared next track stays, but silence detection starts over.
    m_silence = 0;
    emit positionChanged();
    emit seeked(ms / 1000.0);
}

void PlayerController::cycleRepeat()
{
    setRepeatMode((m_repeat + 1) % 3);
}

void PlayerController::toggleLiked()
{
    m_tracks->toggleLiked(m_index);
}

void PlayerController::onDeckMediaStatus(int d, QMediaPlayer::MediaStatus status)
{
    if (d != m_active) {
        // The other deck: either the prepared next track or the previous one playing out.
        if (status == QMediaPlayer::EndOfMedia && m_engine.inTransition())
            m_engine.setDeckEnded(d);
        else if (status == QMediaPlayer::InvalidMedia && !m_engine.inTransition() && !retryStream(d))
            cancelPreload();
        return;
    }
    emit bufferingChanged();
    switch (status) {
    case QMediaPlayer::LoadedMedia:
    case QMediaPlayer::BufferedMedia:
        if (m_pendingSeek >= 0) {
            player().setPosition(m_pendingSeek);
            m_pendingSeek = -1;
            emit positionChanged();
        }
        prefetchNext();
        break;
    case QMediaPlayer::EndOfMedia:
        if (m_resume.remove(m_currentId))
            storeResume();
        if (m_repeat == 2 && !inRoom()) {
            player().setPosition(0);
            player().play();
        } else {
            advance(false);
        }
        break;
    case QMediaPlayer::InvalidMedia:
        if (!retryStream(m_active))
            skipAfterError(tr("Unsupported or damaged file"));
        break;
    default:
        break;
    }
}

void PlayerController::onModelReset()
{
    // After a rescan the rows may have moved — we look tracks up by their ids.
    m_index = m_tracks->rowOf(m_currentId);
    if (m_index < 0 && !m_currentId.isEmpty()) {
        stopTransition();
        player().stop();
        player().setSource(QUrl());
        m_currentId.clear();
        m_decks[m_active].resolving.clear();
    }
    if (!m_plannedId.isEmpty() && m_tracks->rowOf(m_plannedId) < 0 && !m_engine.inTransition())
        cancelPreload();
    m_queue.erase(std::remove_if(m_queue.begin(), m_queue.end(),
                                 [this](const QString &id) { return m_tracks->rowOf(id) < 0; }),
                  m_queue.end());
    m_queuePos = int(m_queue.indexOf(m_currentId));
    if (!m_plannedId.isEmpty())
        m_plannedPos = int(m_queue.indexOf(m_plannedId));
    m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
                                   [this](const QString &id) { return m_tracks->rowOf(id) < 0; }),
                    m_history.end());

    LyricsService::instance()->setTrack(current());
    emit currentChanged();
    emit colorsChanged();
    emit durationChanged();
    emit likedChanged();
    emit queueChanged();
}

// ---- listening rooms ----------------------------------------------------------

bool PlayerController::roomIntercept(const QString &action, const QVariant &arg)
{
    return m_room.intercept && !m_roomApplying && m_room.intercept(action, arg);
}

void PlayerController::setRoomLink(RoomLink link)
{
    const bool was = inRoom();
    m_room = std::move(link);
    m_roomTrack = m_currentId;
    if (was && !inRoom() && m_context == QLatin1String("room"))
        m_context.clear();   // the room's queue stays and plays on here
    if (was != inRoom())
        emit roomLinkChanged();
}

void PlayerController::applyRoom(const QStringList &ids, double seconds, bool play)
{
    m_roomApplying = true;
    const auto done = qScopeGuard([this] { m_roomApplying = false; });
    if (m_context != QLatin1String("room")) {
        m_context = QStringLiteral("room");
        emit mixChanged();
    }
    if (ids.isEmpty()) {
        if (playing() || m_wantPlay)
            pause();
        if (!m_queue.isEmpty()) {
            m_queue.clear();
            m_queuePos = -1;
            cancelPreload();
            emit queueChanged();
        }
        m_roomTrack = m_currentId;
        return;
    }
    const QString id = ids.first();
    if (m_tracks->rowOf(id) < 0)
        return;   // not in the track model yet — the next state brings it
    const bool queueDiffers = ids != m_queue;
    m_queue = ids;
    if (id != m_currentId) {
        m_queuePos = 0;
        m_shuffleNextId.clear();
        // The room's position always (also 0 — not where a long recording was left last time).
        m_pendingSeek = qint64(std::max(0.0, seconds) * 1000);
        m_roomTrack = id;
        load(id, play);
        emit queueChanged();
        return;
    }
    m_queuePos = 0;
    // The prepared next track is no longer next (someone changed the queue).
    if (!m_plannedId.isEmpty() && !m_engine.inTransition()) {
        if (ids.value(1) != m_plannedId)
            cancelPreload();
        else
            m_plannedPos = 1;
    }
    const bool steady = !buffering() && !m_engine.inTransition();
    if (steady && std::abs(position() - seconds) > 3.0)
        seek(seconds);
    if (play && !playing())
        this->play();
    else if (!play && (playing() || m_wantPlay))
        pause();
    if (queueDiffers)
        emit queueChanged();
}

// ---- mixing -------------------------------------------------------------------

QStringList PlayerController::mixStyles() const
{
    return kMixStyles;
}

QString PlayerController::mixStyleName(const QString &style) const
{
    if (style == QLatin1String("fade"))
        return tr("Crossfade");
    if (style == QLatin1String("rise"))
        return tr("Rise");
    if (style == QLatin1String("bass"))
        return tr("Bass swap");
    if (style == QLatin1String("echo"))
        return tr("Echo");
    return tr("Automatic");
}

QString PlayerController::mixStyleDescription(const QString &style) const
{
    if (style == QLatin1String("fade"))
        return tr("Classic constant-power crossfade.");
    if (style == QLatin1String("rise"))
        return tr("A filter gradually cuts the lows of the ending track while the next one comes in ever fuller.");
    if (style == QLatin1String("bass"))
        return tr("Like a DJ: the highs crossfade and the bass switches halfway — never two kick drums at once.");
    if (style == QLatin1String("echo"))
        return tr("The ending track fades away into an echo and the next one emerges from it.");
    return tr("A transition picked for the track's ending: fade-outs and silence are detected, energetic endings get a bass swap.");
}

void PlayerController::loadMixSettings()
{
    auto settings = AppPaths::settings();
    m_crossfade = std::clamp(settings->value("mix/crossfade", 0).toInt(), 0, 12);
    m_crossfadeStyle = settings->value("mix/style", QStringLiteral("fade")).toString();
    if (!kMixStyles.contains(m_crossfadeStyle))
        m_crossfadeStyle = QStringLiteral("fade");
    m_smart = settings->value("mix/smart", true).toBool();
    const QJsonObject mixes = QJsonDocument::fromJson(settings->value("mix/playlists").toString().toUtf8()).object();
    for (auto it = mixes.begin(); it != mixes.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        MixConfig c;
        c.enabled = o.value("enabled").toBool();
        c.style = kMixStyles.contains(o.value("style").toString()) ? o.value("style").toString() : QStringLiteral("auto");
        c.length = std::clamp(o.value("length").toInt(8), 2, 16);
        m_mixes.insert(it.key(), c);
    }
}

void PlayerController::saveMixSettings()
{
    auto settings = AppPaths::settings();
    settings->setValue("mix/crossfade", m_crossfade);
    settings->setValue("mix/style", m_crossfadeStyle);
    settings->setValue("mix/smart", m_smart);
    QJsonObject mixes;
    for (auto it = m_mixes.cbegin(); it != m_mixes.cend(); ++it)
        mixes.insert(it.key(), QJsonObject { { "enabled", it->enabled }, { "style", it->style }, { "length", it->length } });
    settings->setValue("mix/playlists", QString::fromUtf8(QJsonDocument(mixes).toJson(QJsonDocument::Compact)));
    ++m_mixRevision;
    emit mixChanged();
}

void PlayerController::setCrossfade(int seconds)
{
    seconds = std::clamp(seconds, 0, 12);
    if (seconds == m_crossfade)
        return;
    m_crossfade = seconds;
    saveMixSettings();
}

void PlayerController::setCrossfadeStyle(const QString &style)
{
    if (style == m_crossfadeStyle || !kMixStyles.contains(style))
        return;
    m_crossfadeStyle = style;
    saveMixSettings();
}

void PlayerController::setSmartTransitions(bool on)
{
    if (on == m_smart)
        return;
    m_smart = on;
    saveMixSettings();
}

QVariantMap PlayerController::mixFor(const QString &context) const
{
    const MixConfig c = m_mixes.value(context);
    return { { "enabled", c.enabled }, { "style", c.style }, { "length", c.length } };
}

void PlayerController::setMix(const QString &context, bool enabled, const QString &style, int length)
{
    if (context.isEmpty())
        return;
    MixConfig c;
    c.enabled = enabled;
    c.style = kMixStyles.contains(style) ? style : QStringLiteral("auto");
    c.length = std::clamp(length, 2, 16);
    m_mixes.insert(context, c);
    saveMixSettings();
}

PlayerController::MixConfig PlayerController::effectiveMix() const
{
    // The playlist's mix takes precedence over the global crossfade.
    const auto it = m_mixes.constFind(m_context);
    if (it != m_mixes.cend() && it->enabled)
        return *it;
    MixConfig global;
    global.enabled = m_crossfade > 0;
    global.style = m_crossfadeStyle;
    global.length = m_crossfade;
    return global;
}

void PlayerController::preloadNext()
{
    const int pos = nextQueuePos();
    if (pos < 0)
        return;
    const QString id = m_queue.at(pos);
    const int row = m_tracks->rowOf(id);
    if (row < 0 || id == m_currentId || !canPlay(row, false))
        return;

    const int d = standby();
    Deck &deck = m_decks[d];
    deck.player->stop();
    m_engine.dropDeck(d);
    m_engine.resetLevels(d);
    deck.trackId = id;
    m_plannedPos = pos;
    m_plannedId = id;

    const Track &t = m_tracks->at(row);
    if (t.source == QLatin1String("youtube") || t.source == QLatin1String("spotify") || t.source == QLatin1String("soundcloud")) {
        startStream(d, t);
    } else {
        deck.resolving.clear();
        deck.player->setSource(QUrl::fromLocalFile(t.path));
    }
}

void PlayerController::cancelPreload()
{
    Deck &deck = m_decks[standby()];
    if (deck.trackId.isEmpty() && m_plannedPos < 0)
        return;
    deck.player->stop();
    deck.player->setSource(QUrl());
    deck.resolving.clear();
    deck.trackId.clear();
    m_engine.dropDeck(standby());
    m_plannedPos = -1;
    m_plannedId.clear();
}

bool PlayerController::standbyReady() const
{
    const Deck &deck = m_decks[standby()];
    if (deck.trackId.isEmpty() || !deck.resolving.isEmpty())
        return false;
    const auto s = deck.player->mediaStatus();
    return s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia;
}

void PlayerController::updateMix(double dt)
{
    if (m_engine.inTransition()) {
        emit transitionChanged();
        return;
    }
    const MixConfig mix = effectiveMix();
    if (!mix.enabled || !playing() || (m_repeat == 2 && !inRoom()) || live() || !m_decks[m_active].resolving.isEmpty())
        return;
    const double total = player().duration() > 0 ? player().duration() / 1000.0 : double(duration());
    if (total <= 0)
        return;
    const double pos = player().position() / 1000.0;   // the decoder's position — mixing happens there
    const double remaining = total - pos;
    const double length = std::min<double>(mix.length, std::max(2.0, total / 3));

    if (remaining <= length + 12 && m_plannedPos < 0)
        preloadNext();
    if (m_plannedPos < 0 || !standbyReady())
        return;

    // Smart transitions: silence at the end of the recording and fade-outs recorded in the track itself.
    if (m_smart && pos > 20 && remaining < 25) {
        const double level = m_engine.shortLevelDb(m_active);
        const double average = m_engine.averageLevelDb(m_active);
        m_silence = level < -50 ? m_silence + dt : 0;
        // A fade-out in the recording: there's still sound, but clearly quieter than the track's average (for ≥ 1 s).
        m_quiet = level >= -50 && level < average - 14 ? m_quiet + dt : 0;
        if (m_silence > 0.8) {
            // The old track is already silent — the next one comes in at once, without the quiet intro.
            startTransition(AudioEngine::Style::Fade, 0.3, tr("Skipped silence"));
            return;
        }
        if (m_quiet > 1.0 && remaining < length + 6 && remaining > 1.5) {
            startTransition(AudioEngine::Style::Fade, std::clamp(remaining, 2.0, length),
                            tr("Crossfade"));
            return;
        }
    }
    if (remaining > length)
        return;

    AudioEngine::Style style = styleFromId(mix.style);
    QString label = mixStyleName(mix.style);
    if (mix.style == QLatin1String("auto")) {
        const bool energetic = m_engine.shortLevelDb(m_active) > m_engine.averageLevelDb(m_active) - 4;
        style = energetic ? AudioEngine::Style::BassSwap : AudioEngine::Style::Fade;
        label = energetic ? tr("Bass swap") : tr("Crossfade");
    }
    startTransition(style, std::max(1.0, remaining), label);
}

void PlayerController::startTransition(AudioEngine::Style style, double seconds, const QString &label)
{
    const int from = m_active;
    const int to = standby();
    const QString id = m_plannedId;
    const int row = m_tracks->rowOf(id);
    if (row < 0) {
        cancelPreload();
        return;
    }

    m_engine.resetLevels(to);
    m_engine.beginTransition(from, to, { style, seconds, m_smart });
    m_decks[to].player->play();

    if (!m_currentId.isEmpty()) {
        m_history << m_currentId;
        if (m_history.size() > 200)
            m_history.removeFirst();
    }
    m_active = to;
    m_currentId = id;
    m_index = row;
    m_queuePos = m_plannedPos;
    m_plannedPos = -1;
    m_plannedId.clear();
    m_pendingSeek = -1;
    m_silence = 0;
    m_quiet = 0;
    m_transitionLabel = label;

    emitTrackChanged();
    emit playingChanged();
    emit bufferingChanged();
    emit transitionChanged();
}

void PlayerController::stopTransition()
{
    if (m_engine.inTransition())
        m_engine.finishTransitionNow();   // the transitionFinished signal will stop the old deck
}

// ---- spectrum -----------------------------------------------------------------

void PlayerController::ensureTicking()
{
    if (!m_timer.isActive()) {
        m_clock.restart();
        m_timer.start();
    }
}

void PlayerController::tick()
{
    const double dt = std::min(0.1, m_clock.restart() / 1000.0);
    const bool active = playing();

    // Samples that actually played (after EQ) -> spectrum analyzer.
    m_engine.takePlayed(m_played);
    if (!m_played.empty())
        m_analyzer.pushSamples(m_played.data(), m_played.size(), m_engine.sampleRate());
    Equalizer::instance()->tickMeters(dt);
    m_heardLag += (m_engine.latency() - m_heardLag) * std::min(1.0, dt * 8.0);   // smoothed — the buffers refill in chunks
    updateMix(dt);
    checkStall(dt);
    countListening(dt);

    if (active) {
        m_analyzer.compute(m_targets);
        emit positionChanged();
    } else {
        std::fill(m_targets.begin(), m_targets.end(), 0.0);
    }

    // Fast attack, slow decay — the bars "jump" to the beat and gently fade out.
    qreal sum = 0.0, peak = 0.0;
    for (int i = 0; i < kBars; ++i) {
        const qreal rate = m_targets[i] > m_spectrum[i] ? 0.6 : 0.13;
        const qreal k = std::min(1.0, rate * dt * 60.0);
        m_spectrum[i] += (m_targets[i] - m_spectrum[i]) * k;
        peak = std::max(peak, m_spectrum[i]);
        if (i < 10)
            sum += m_spectrum[i];
    }
    m_level = sum / 10.0;
    emit spectrumChanged();

    if (!active && peak < 0.002 && !m_engine.inTransition()) {
        std::fill(m_spectrum.begin(), m_spectrum.end(), 0.0);
        m_level = 0.0;
        emit spectrumChanged();
        m_timer.stop();
    }
}
