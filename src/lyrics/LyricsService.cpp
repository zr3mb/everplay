#include "LyricsService.h"

#include "AppPaths.h"
#include "Language.h"
#include "LyricsAligner.h"
#include "TagLibPath.h"
#include "spotify/SpotifyService.h"
#include "youtube/YouTubeService.h"
#include "openmusic/OpenMusicService.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJSEngine>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QtConcurrent/QtConcurrentRun>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kMissingRetryMs = 3LL * 24 * 3600 * 1000;   // missing lyrics are checked again after 3 days
// Everplay 0.25.0 (2026-11-07 00:35 UTC) replaced whisper with forced alignment but didn't yet mark its results:
// timing stored without a mark before then is whisper's.
constexpr qint64 kAlignerSinceMs = 1794011716000LL;

// Lyrics from the file: first an .lrc next to the track (may have timestamps), then the LYRICS / UNSYNCEDLYRICS tag.
// Runs on a worker thread.
Lyrics readLocalLyrics(const QString &path)
{
    const QFileInfo fi(path);
    for (const QString &ext : { QStringLiteral("lrc"), QStringLiteral("LRC") }) {
        QFile lrc(fi.absolutePath() + QChar('/') + fi.completeBaseName() + QChar('.') + ext);
        if (lrc.open(QIODevice::ReadOnly)) {
            const QByteArray data = lrc.read(1024 * 1024);
            QStringDecoder utf8(QStringDecoder::Utf8);
            QString text = utf8(data);
            if (utf8.hasError())
                text = QString::fromLocal8Bit(data);
            Lyrics l = Lyrics::fromLrc(text);
            if (!l.isEmpty()) {
                l.source = QCoreApplication::translate("LyricsService", "File %1").arg(QFileInfo(lrc).fileName());
                l.origin = QStringLiteral("local");
                return l;
            }
        }
    }

    const TagLibPath native(path);
    TagLib::FileRef ref(native.get(), false);
    if (ref.isNull())
        return {};
    const TagLib::PropertyMap props = ref.properties();
    for (const char *key : { "LYRICS", "UNSYNCEDLYRICS" }) {
        const auto it = props.find(key);
        if (it == props.end() || it->second.isEmpty())
            continue;
        const QString text = QString::fromStdWString(it->second.front().toWString());
        Lyrics l = Lyrics::fromLrc(text);   // some programs store the LRC format in the tag
        if (!l.isEmpty()) {
            l.source = QCoreApplication::translate("LyricsService", "Tag in the file");
            l.origin = QStringLiteral("local");
            return l;
        }
    }
    return {};
}

} // namespace

LyricsService *LyricsService::instance()
{
    static LyricsService *inst = new LyricsService;
    return inst;
}

LyricsService *LyricsService::create(QQmlEngine *, QJSEngine *)
{
    LyricsService *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

LyricsService::LyricsService(QObject *parent)
    : QObject(parent)
    , m_hardware(SpeechEngine::detectHardware())
{
    auto settings = AppPaths::settings();
    m_model = settings->value("lyrics/model", m_hardware.recommendedModel).toString();
    if (!SpeechEngine::modelNames().contains(m_model))
        m_model = m_hardware.recommendedModel;
    // Checked against the hardware when it's used: the graphics card is only found a moment after start.
    m_autoAlign = settings->value("lyrics/autoAlign", false).toBool();
    m_karaoke = settings->value("lyrics/karaoke", true).toBool();

    connect(&m_speech, &SpeechEngine::stateChanged, this, [this] {
        emit sttChanged();
        emit alignChanged();
    });
    connect(&m_speech, &SpeechEngine::notice, this, &LyricsService::notice);
    connect(&m_speech, &SpeechEngine::progress, this, [this](qreal value, const QString &stage) {
        m_alignProgress = value;
        m_alignStage = stage;
        emit alignChanged();
    });
    // After the engine or a model is downloaded, align the current track right away (if the feature is on).
    connect(&m_speech, &SpeechEngine::stateChanged, this, &LyricsService::maybeAutoAlign, Qt::QueuedConnection);
    // The hardware summary is a translated label — rebuild it in the interface language.
    connect(Language::instance(), &Language::changed, this, [this] {
        m_hardware.summary = SpeechEngine::detectHardware().summary;
    });

    StreamResolver *resolver = YouTubeService::instance()->resolver();
    connect(resolver, &StreamResolver::resolved, this, [this](const QString &videoId, const QUrl &url) {
        if (videoId != m_waitingStream)
            return;
        m_waitingStream.clear();
        startAlign(url);
    });
    connect(resolver, &StreamResolver::failed, this, [this](const QString &videoId, const QString &) {
        if (videoId != m_waitingStream)
            return;
        m_waitingStream.clear();
        emit alignChanged();
    });
}

// ---- settings -------------------------------------------------------------------

void LyricsService::setAutoAlign(bool on)
{
    on = on && hardwareOk();
    if (on == m_autoAlign)
        return;
    m_autoAlign = on;
    AppPaths::settings()->setValue("lyrics/autoAlign", on);
    if (!on)
        cancelAlign();
    emit settingsChanged();
    emit alignChanged();
    maybeAutoAlign();
}

void LyricsService::setKaraoke(bool on)
{
    if (on == m_karaoke)
        return;
    m_karaoke = on;
    AppPaths::settings()->setValue("lyrics/karaoke", on);
    emit settingsChanged();
    emit alignChanged();   // canTimeWords follows it
    maybeAutoAlign();
}

void LyricsService::setSttModel(const QString &model)
{
    if (model == m_model || !SpeechEngine::modelNames().contains(model))
        return;
    m_model = model;
    AppPaths::settings()->setValue("lyrics/model", model);
    m_alignTried.clear();
    emit settingsChanged();
    emit sttChanged();
    emit alignChanged();
}

bool LyricsService::canAlign() const
{
    return hardwareOk() && sttReady() && m_state == QLatin1String("ready") && !m_lyrics.isEmpty()
           && !m_lyrics.synced && !aligning();
}

bool LyricsService::canTranscribe() const
{
    return m_hasTrack && recognizeHardwareOk() && sttRecognizeReady() && m_state == QLatin1String("missing") && !aligning()
           && m_track.source != QLatin1String("radio") && !m_track.demo;
}

// ---- track and fetching ---------------------------------------------------------

void LyricsService::setTrack(const Track *track)
{
    if (!track) {
        if (!m_hasTrack)
            return;
        m_hasTrack = false;
        ++m_serial;
        cancelAlign();
        m_track = {};
        loadOffset();
        apply({}, false);
        setState(QStringLiteral("none"));
        return;
    }
    if (m_hasTrack && track->id == m_track.id)
        return;

    ++m_serial;
    cancelAlign();
    m_hasTrack = true;
    m_track = *track;
    loadOffset();
    fetch();
}

QString LyricsService::offsetKey() const
{
    // Track ids can be file paths — hashed into a plain settings key.
    return QStringLiteral("lyrics/offset/")
         + QString::fromLatin1(QCryptographicHash::hash(m_track.id.toUtf8(), QCryptographicHash::Md5).toHex());
}

void LyricsService::loadOffset()
{
    const double offset = m_hasTrack ? AppPaths::settings()->value(offsetKey(), 0.0).toDouble() : 0.0;
    if (offset != m_offset) {
        m_offset = offset;
        emit offsetChanged();
    }
}

void LyricsService::nudge(double seconds)
{
    if (!m_hasTrack)
        return;
    m_offset = std::clamp(std::round((m_offset + seconds) * 10.0) / 10.0, -30.0, 30.0);
    if (m_offset == 0.0)
        AppPaths::settings()->remove(offsetKey());
    else
        AppPaths::settings()->setValue(offsetKey(), m_offset);
    emit offsetChanged();
}

void LyricsService::resetOffset()
{
    nudge(-m_offset);
}

void LyricsService::retry()
{
    if (!m_hasTrack)
        return;
    ++m_serial;
    cancelAlign();
    QFile::remove(cachePath(m_track.id));
    m_alignTried.remove(m_track.id);
    fetch();
}

void LyricsService::fetch()
{
    apply({}, false);
    if (m_track.demo || m_track.source == QLatin1String("radio")) {
        setState(QStringLiteral("missing"));
        return;
    }

    Lyrics cached;
    if (loadCached(m_track.id, cached, &m_cachedAt)) {
        if (cached.isEmpty()) {
            setState(QStringLiteral("missing"));
        } else {
            apply(cached, false);
            setState(QStringLiteral("ready"));
            maybeAutoAlign();
        }
        return;
    }

    setState(QStringLiteral("loading"));
    const quint64 serial = m_serial;
    if (m_track.source == QLatin1String("youtube")) {
        fetchYouTube(m_track.path, serial, [this, serial](const Lyrics &l, const QString &error) {
            if (serial != m_serial)
                return;
            withLrcLib(serial, l, error);
        });
        return;
    }

    // Local file: look the track up on YouTube Music by artist and title.
    const QString query = (m_track.artist + QChar(' ') + m_track.title).trimmed();
    if (m_track.title.isEmpty()) {
        fetchLocal(serial, {}, {});
        return;
    }
    YouTubeService::instance()->api()->search(query, [this, serial](const QList<Track> &results, const QString &error) {
        if (serial != m_serial)
            return;
        const int match = error.isEmpty() ? YtMusicApi::bestMatch(m_track, results) : -1;
        const QString videoId = match >= 0 ? results.at(match).path : QString();
        if (videoId.isEmpty()) {
            fetchLocal(serial, {}, error);
            return;
        }
        fetchYouTube(videoId, serial, [this, serial](const Lyrics &l, const QString &ytError) {
            if (serial != m_serial)
                return;
            fetchLocal(serial, l, ytError);
        });
    });
}

void LyricsService::fetchYouTube(const QString &videoId, quint64 serial,
                                 std::function<void(const Lyrics &, const QString &)> done)
{
    YouTubeService::instance()->api()->fetchLyrics(videoId, [this, serial, done](const Lyrics &l, const QString &error) {
        if (serial == m_serial)
            done(l, error);
    });
}

void LyricsService::fetchLocal(quint64 serial, const Lyrics &fromYouTube, const QString &ytError)
{
    // Lyrics from YouTube with timestamps win; otherwise we check the file (it may have an .lrc with timestamps).
    if (fromYouTube.synced) {
        apply(fromYouTube, true);
        setState(QStringLiteral("ready"));
        return;
    }
    auto *watcher = new QFutureWatcher<Lyrics>(this);
    connect(watcher, &QFutureWatcher<Lyrics>::finished, this, [this, watcher, serial, fromYouTube, ytError] {
        watcher->deleteLater();
        if (serial != m_serial)
            return;
        const Lyrics local = watcher->result();
        withLrcLib(serial, local.synced || fromYouTube.isEmpty() ? local : fromYouTube, ytError);
    });
    watcher->setFuture(QtConcurrent::run(readLocalLyrics, m_track.path));
}

void LyricsService::withLrcLib(quint64 serial, const Lyrics &best, const QString &error)
{
    // Timed lyrics are what we're after: without them (none, or plain text only) LRCLIB gets a try.
    if (best.synced) {
        finish(best, error);
        return;
    }
    m_lrclib.fetch(m_track.artist, m_track.title, m_track.album, m_track.duration,
                   [this, serial, best, error](const Lyrics &lrc) {
        if (serial != m_serial)
            return;
        if (lrc.synced || (best.isEmpty() && !lrc.isEmpty()))
            finish(lrc, QString());
        else if (!best.isEmpty())
            finish(best, error);
        else
            withWebSearch(serial, error);
    });
}

void LyricsService::withWebSearch(quint64 serial, const QString &error)
{
    // Still nothing: lyrics sites and the description of the upload, before anyone has to recognize the
    // singing.
    LyricsSearch::Query query;
    query.artist = m_track.artist;
    query.title = m_track.title;
    query.durationSec = m_track.duration;
    if (m_track.source == QLatin1String("youtube"))
        query.videoId = m_track.path;
    else if (m_track.source == QLatin1String("spotify"))
        query.videoId = SpotifyService::instance()->matchedVideoId(m_track.id);
    else if (m_track.source == QLatin1String("soundcloud"))
        query.soundCloudId = m_track.id;
    m_search.fetch(query, [this, serial, error](const Lyrics &found) {
        if (serial != m_serial)
            return;
        finish(found, found.isEmpty() ? error : QString());
    });
}

void LyricsService::finish(const Lyrics &lyrics, const QString &error)
{
    if (!lyrics.isEmpty()) {
        apply(lyrics, true);
        setState(QStringLiteral("ready"));
        maybeAutoAlign();
    } else if (!error.isEmpty()) {
        setState(QStringLiteral("error"), error);
    } else {
        storeCached(m_track.id, {});
        setState(QStringLiteral("missing"));
    }
}

void LyricsService::apply(const Lyrics &lyrics, bool store)
{
    m_lyrics = lyrics;
    m_whisperMade = isWhisperMade(lyrics, store ? QDateTime::currentMSecsSinceEpoch() : m_cachedAt);
    if (m_lyrics.synced)
        m_lyrics.fillEnds(m_track.duration > 0 ? qint64(m_track.duration) * 1000 : -1);
    if (store && m_hasTrack)
        storeCached(m_track.id, m_lyrics);

    m_lines.clear();
    m_starts.clear();
    for (int i = 0; i < m_lyrics.lines.size(); ++i) {
        const LyricLine &l = m_lyrics.lines.at(i);
        // Every word's time (heard or estimated) for the karaoke highlight: [start s, end s, from, length].
        QVariantList words;
        qint64 end = l.end;
        if (m_lyrics.synced && l.start >= 0) {
            const QList<LyricWord> timed = l.timedWords();
            for (const LyricWord &w : timed)
                words << QVariant(QVariantList { w.start / 1000.0, w.end / 1000.0, w.from, w.length });
            // Before a long instrumental break the line fades out a while after its last word instead
            // of staying lit until the next line (longer after an estimated word, which may be late).
            if (!timed.isEmpty() && end > 0) {
                const bool heard = !l.words.isEmpty() && l.words.last().from == timed.last().from;
                end = std::min(end, timed.last().end + (heard ? 1500 : 4000));
            }
        }
        m_lines << QVariantMap {
            { "text", l.text },
            { "start", l.start >= 0 ? l.start / 1000.0 : -1.0 },
            { "end", end >= 0 ? end / 1000.0 : -1.0 },
            { "words", words },
        };
        if (m_lyrics.synced && l.start >= 0 && !l.text.isEmpty())
            m_starts.emplace_back(l.start, i);
    }
    emit lyricsChanged();
    emit alignChanged();
}

void LyricsService::setState(const QString &state, const QString &error)
{
    m_state = state;
    m_error = error;
    emit lyricsChanged();
    emit alignChanged();
}

int LyricsService::lineAt(double seconds) const
{
    if (m_starts.empty())
        return -1;
    const qint64 ms = qint64(seconds * 1000);
    auto it = std::upper_bound(m_starts.cbegin(), m_starts.cend(), ms,
                               [](qint64 v, const std::pair<qint64, int> &e) { return v < e.first; });
    return it == m_starts.cbegin() ? -1 : std::prev(it)->second;
}

// ---- timing alignment (alpha) ---------------------------------------------------

void LyricsService::maybeAutoAlign()
{
    if (!m_autoAlign || m_alignTried.contains(m_track.id))
        return;
    if (canAlign())
        alignNow();
    else if (canTimeWords())
        timeWordsNow();
}

bool LyricsService::canTimeWords() const
{
    return m_karaoke && m_hasTrack && hardwareOk() && sttReady() && m_state == QLatin1String("ready") && m_lyrics.synced
           && !m_lyrics.wordTimed && !aligning();
}

void LyricsService::timeWordsNow()
{
    if (canTimeWords())
        runSpeech(Words);
}

void LyricsService::alignNow()
{
    if (canAlign())
        runSpeech(Align);
}

void LyricsService::transcribeNow()
{
    if (canTranscribe())
        runSpeech(Recognize);
}

void LyricsService::runSpeech(SpeechMode mode)
{
    m_mode = mode;
    m_alignTried.insert(m_track.id, true);
    m_alignProgress = 0;
    m_alignStage = tr("Preparing");
    // Streaming tracks are heard from YouTube: a Spotify track through the video it was matched to.
    QString videoId;
    if (m_track.source == QLatin1String("youtube"))
        videoId = m_track.path;
    else if (m_track.source == QLatin1String("soundcloud") || OpenMusicService::isOpenSource(m_track.source))
        videoId = m_track.id;   // SoundCloud's own stream or an open source's FLAC file, through the same resolver
    else if (m_track.source == QLatin1String("spotify"))
        videoId = SpotifyService::instance()->matchedVideoId(m_track.id);
    if (m_track.source == QLatin1String("spotify") && videoId.isEmpty()) {
        emit notice(tr("This track hasn't been found on YouTube Music yet, so there's nothing to listen to"),
                    QStringLiteral("close"));
        emit alignChanged();
        return;
    }
    if (!videoId.isEmpty()) {
        StreamResolver *resolver = YouTubeService::instance()->resolver();
        const QUrl cached = resolver->cachedUrl(videoId);
        if (cached.isValid()) {
            startAlign(cached);
        } else {
            m_waitingStream = videoId;
            emit alignChanged();
            resolver->resolve(videoId);
        }
        return;
    }
    startAlign(QUrl::fromLocalFile(m_track.path));
}

void LyricsService::startAlign(const QUrl &audio)
{
    if (m_mode == Recognize) {
        startRecognize(audio);
        return;
    }
    if (m_mode == Words) {
        startWordTiming(audio);
        return;
    }
    const quint64 serial = m_serial;
    const Lyrics base = m_lyrics;
    const qint64 durationMs = m_track.duration > 0 ? qint64(m_track.duration) * 1000 : -1;
    QList<int> index;
    const QStringList text = alignedText(base, index);
    // The language of the lyrics, so the aligner reads their letters right.
    m_speech.align(audio, text, LyricsAligner::guessLanguage(base),
                   [this, serial, base, durationMs, index](const QList<QList<LyricsAligner::Word>> &found, const QString &error) {
        emit alignChanged();
        if (serial != m_serial)
            return;
        if (!error.isEmpty()) {
            emit notice(tr("Lyrics alignment: %1").arg(error), QStringLiteral("close"));
            return;
        }
        const LyricsAligner::Result r = LyricsAligner::fromLineWords(base, perLine(base, found, index), durationMs);
        if (r.coverage < 0.25 || r.anchoredLines < 2) {
            emit notice(tr("Couldn't align the lyrics to the recording"), QStringLiteral("close"));
            return;
        }
        Lyrics aligned = r.lyrics;
        aligned.source = base.source;
        aligned.engine = QStringLiteral("aligner");
        apply(aligned, true);
        emit notice(tr("Aligned the lyrics timing"), QStringLiteral("check"));
    });
    emit alignChanged();
}

void LyricsService::startWordTiming(const QUrl &audio)
{
    const quint64 serial = m_serial;
    const Lyrics base = m_lyrics;
    const qint64 durationMs = m_track.duration > 0 ? qint64(m_track.duration) * 1000 : -1;
    QList<int> index;
    const QStringList text = alignedText(base, index);
    m_speech.align(audio, text, LyricsAligner::guessLanguage(base),
                   [this, serial, base, durationMs, index](const QList<QList<LyricsAligner::Word>> &found, const QString &error) {
        emit alignChanged();
        if (serial != m_serial)
            return;
        if (!error.isEmpty()) {
            emit notice(tr("Word timing: %1").arg(error), QStringLiteral("close"));
            return;
        }
        const LyricsAligner::Result r = LyricsAligner::fromLineWords(base, perLine(base, found, index), durationMs);
        if (r.coverage < 0.25 || r.anchoredLines < 2) {
            emit notice(tr("Couldn't time the words"), QStringLiteral("close"));
            return;
        }

        // Lyrics from LRCLIB are sometimes timed for another cut of the track (a radio edit, a video with an
        // intro): every line is off by the same several seconds. Then the recording's own timing wins.
        std::vector<qint64> shifts;
        for (int i = 0; i < base.lines.size() && i < r.lyrics.lines.size(); ++i)
            if (base.lines.at(i).start >= 0 && !r.lyrics.lines.at(i).words.isEmpty())
                shifts.push_back(r.lyrics.lines.at(i).start - base.lines.at(i).start);
        if (shifts.size() >= 4) {
            std::nth_element(shifts.begin(), shifts.begin() + shifts.size() / 2, shifts.end());
            if (std::abs(shifts[shifts.size() / 2]) > 1500) {
                Lyrics retimed = r.lyrics;
                retimed.source = base.source;
                retimed.engine = QStringLiteral("aligner");
                apply(retimed, true);
                resetOffset();
                emit notice(tr("These lyrics were timed for another version of the track — timed them to this recording"),
                            QStringLiteral("check"));
                return;
            }
        }

        // The lines keep their times; each word gets the time it was heard at, if that falls in its line.
        Lyrics timed = base;
        int lines = 0;
        for (int i = 0; i < timed.lines.size() && i < r.lyrics.lines.size(); ++i) {
            LyricLine &l = timed.lines[i];
            if (l.start < 0)
                continue;
            const qint64 until = (l.end > l.start ? l.end : l.start + 8000) + 500;
            QList<LyricWord> heard;
            for (LyricWord w : r.lyrics.lines.at(i).words) {
                if (w.start < l.start - 1000 || w.start > until)
                    continue;
                w.start = std::max(w.start, l.start);
                w.end = std::max(w.end, w.start + 80);
                heard << w;
            }
            if (!heard.isEmpty()) {
                l.words = heard;
                ++lines;
            }
        }
        if (lines < 2) {
            emit notice(tr("Couldn't time the words"), QStringLiteral("close"));
            return;
        }
        timed.wordTimed = true;
        timed.engine = QStringLiteral("aligner");
        apply(timed, true);
        emit notice(tr("Timed every word"), QStringLiteral("check"));
    });
    emit alignChanged();
}

QStringList LyricsService::alignedText(const Lyrics &lyrics, QList<int> &index)
{
    // The aligner gets the lines with words in them; `index` says which lyric line each one is.
    QStringList text;
    index.clear();
    for (int i = 0; i < lyrics.lines.size(); ++i) {
        const QString line = lyrics.lines.at(i).text.simplified();
        if (line.isEmpty())
            continue;
        text << line;
        index << i;
    }
    return text;
}

QList<QList<LyricsAligner::Word>> LyricsService::perLine(const Lyrics &lyrics, const QList<QList<LyricsAligner::Word>> &found,
                                                          const QList<int> &index)
{
    QList<QList<LyricsAligner::Word>> lines(lyrics.lines.size());
    for (int k = 0; k < found.size() && k < index.size(); ++k)
        lines[index.at(k)] = found.at(k);
    return lines;
}

void LyricsService::startRecognize(const QUrl &audio)
{
    // No lyrics to go by: the words and their times come from the recording alone (test feature).
    const quint64 serial = m_serial;
    const qint64 durationMs = m_track.duration > 0 ? qint64(m_track.duration) * 1000 : -1;
    m_speech.transcribe(audio, m_model, QString(),
                        [this, serial, durationMs](const QList<LyricsAligner::Word> &words, const QString &error) {
        emit alignChanged();
        if (serial != m_serial)
            return;
        if (!error.isEmpty()) {
            emit notice(tr("Lyrics recognition: %1").arg(error), QStringLiteral("close"));
            return;
        }
        Lyrics recognized = LyricsAligner::fromTranscription(words, durationMs);
        if (recognized.wordCount() < 6) {
            emit notice(tr("No singing was recognized in this track"), QStringLiteral("close"));
            return;
        }
        recognized.source = tr("Recognized automatically (test feature), may contain mistakes");
        recognized.engine = QStringLiteral("whisper");
        apply(recognized, true);
        setState(QStringLiteral("ready"));
        emit notice(tr("Recognized the lyrics (test feature)"), QStringLiteral("check"));
    });
    emit alignChanged();
}

void LyricsService::cancelAlign()
{
    m_waitingStream.clear();
    m_speech.cancel();
    m_alignProgress = 0;
    m_alignStage.clear();
    emit alignChanged();
}

// ---- whisper results ------------------------------------------------------------

bool LyricsService::isWhisperMade(const Lyrics &lyrics, qint64 storedAtMs)
{
    if (lyrics.origin == QLatin1String("stt-text"))
        return true;
    if (lyrics.origin != QLatin1String("stt") && !lyrics.wordTimed)
        return false;
    return lyrics.engine == QLatin1String("whisper") || (lyrics.engine.isEmpty() && storedAtMs < kAlignerSinceMs);
}

namespace {

// The cache files with whisper's results; removes them when `remove`.
int scanWhisperLyrics(const QString &dir, bool remove)
{
    int n = 0;
    for (const QFileInfo &fi : QDir(dir).entryInfoList({ QStringLiteral("*.json") }, QDir::Files)) {
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        if (!LyricsService::isWhisperMadeEntry(o))
            continue;
        ++n;
        if (remove)
            QFile::remove(fi.filePath());
    }
    return n;
}

} // namespace

bool LyricsService::isWhisperMadeEntry(const QJsonObject &entry)
{
    return isWhisperMade(Lyrics::fromJson(entry), qint64(entry.value("fetched").toDouble()));
}

void LyricsService::countWhisperLyrics()
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/lyrics");
    auto *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher] {
        watcher->deleteLater();
        m_whisperCount = watcher->result();
        emit whisperLyricsChanged();
    });
    watcher->setFuture(QtConcurrent::run(scanWhisperLyrics, dir, false));
}

void LyricsService::removeWhisperLyrics()
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/lyrics");
    auto *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const int removed = watcher->result();
        m_whisperCount = 0;
        emit whisperLyricsChanged();
        emit notice(tr("Removed lyrics made by speech recognition from %n track(s)", "", removed), QStringLiteral("check"));
        // The track playing now gets its lyrics again (and new timing, if automatic timing is on).
        if (m_hasTrack && m_whisperMade)
            retry();
    });
    watcher->setFuture(QtConcurrent::run(scanWhisperLyrics, dir, true));
}

// ---- cache ----------------------------------------------------------------------

QString LyricsService::cachePath(const QString &trackId) const
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/lyrics");
    QDir().mkpath(dir);
    return dir + QChar('/') + QCryptographicHash::hash(trackId.toUtf8(), QCryptographicHash::Sha1).toHex()
           + QStringLiteral(".json");
}

bool LyricsService::loadCached(const QString &trackId, Lyrics &out, qint64 *storedAt) const
{
    QFile f(cachePath(trackId));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value("id").toString() != trackId)
        return false;
    out = Lyrics::fromJson(o);
    if (storedAt)
        *storedAt = qint64(o.value("fetched").toDouble());
    // Before cache version 2, signed-in users got YouTube lyrics without timing (the Android request carried
    // the website's cookies) — those are fetched again once.
    if (o.value("v").toInt() < 2 && !out.synced && out.origin == QLatin1String("youtube"))
        return false;
    // Version 3 added LRCLIB, version 4 lyrics sites and descriptions: "missing" from before is checked again.
    if (o.value("v").toInt() < 4 && out.isEmpty())
        return false;
    // Before version 5 plain lyrics kept section headers ("[Chorus]", "Ref.") and repeat marks as sung lines,
    // and a chorus only named again wasn't written out — timing made from such a text drifts. Those are
    // fetched (and timed) again.
    if (o.value("v").toInt() < 5 && !out.isEmpty() && (!out.synced || out.origin == QLatin1String("stt"))
        && out.origin != QLatin1String("stt-text")) {
        const QString text = out.plainText();
        if (Lyrics::fromPlainText(text).plainText() != Lyrics::fromPlainText(text, false).plainText())
            return false;
    }
    // YouTube lyrics stored as "synced" without real times (all lines at 0:00) are fetched again — they're
    // plain lyrics, so LRCLIB gets a chance to give timed ones.
    if (out.synced && !out.hasRealTiming())
        return false;
    if (out.isEmpty()) {
        // Remembered missing lyrics — we check again after a few days.
        const qint64 at = qint64(o.value("fetched").toDouble());
        return QDateTime::currentMSecsSinceEpoch() - at < kMissingRetryMs;
    }
    return true;
}

void LyricsService::storeCached(const QString &trackId, const Lyrics &lyrics) const
{
    QJsonObject o = lyrics.toJson();
    o.insert("id", trackId);
    o.insert("v", 5);
    o.insert("fetched", double(QDateTime::currentMSecsSinceEpoch()));
    QSaveFile f(cachePath(trackId));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.commit();
    }
}
