#pragma once

#include "LrcLib.h"
#include "LyricsSearch.h"
#include "Lyrics.h"
#include "SpeechEngine.h"
#include "TrackModel.h"

#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <vector>

class QQmlEngine;
class QJSEngine;

// Lyrics for QML.
// Source order: YouTube Music (the timed version, then plain text; for local files
// the track is looked up by artist and title), and second the lyrics from the file
// (.lrc next to the track or the LYRICS tag). When the lyrics have no timestamps and the user turned on
// "automatic timing" (alpha), the lyrics are aligned to the singing in the recording (SpeechEngine).
// Results go to a cache on disk — recognition runs once per track.
class LyricsService : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Lyrics)
    QML_SINGLETON

    // "none" (nothing playing) | "loading" | "ready" | "missing" | "error"
    Q_PROPERTY(QString state READ state NOTIFY lyricsChanged)
    Q_PROPERTY(QString error READ error NOTIFY lyricsChanged)
    Q_PROPERTY(QVariantList lines READ lines NOTIFY lyricsChanged)
    Q_PROPERTY(bool synced READ synced NOTIFY lyricsChanged)
    Q_PROPERTY(QString source READ source NOTIFY lyricsChanged)
    Q_PROPERTY(QString origin READ origin NOTIFY lyricsChanged)
    // Manual timing correction for this track (s, + = the lyrics come later) — the synced lyrics may
    // belong to a different version (e.g. a music video with an intro). Remembered per track.
    Q_PROPERTY(double offset READ offset NOTIFY offsetChanged)

    Q_PROPERTY(bool aligning READ aligning NOTIFY alignChanged)
    Q_PROPERTY(qreal alignProgress READ alignProgress NOTIFY alignChanged)
    Q_PROPERTY(QString alignStage READ alignStage NOTIFY alignChanged)
    Q_PROPERTY(bool canAlign READ canAlign NOTIFY alignChanged)
    // A track without lyrics: they can be recognized from the recording (test feature).
    Q_PROPERTY(bool canTranscribe READ canTranscribe NOTIFY alignChanged)
    Q_PROPERTY(bool recognizing READ recognizing NOTIFY alignChanged)
    // Synced lyrics whose words only have estimated times: alignment can time every word.
    Q_PROPERTY(bool canTimeWords READ canTimeWords NOTIFY alignChanged)
    Q_PROPERTY(bool wordTimed READ wordTimed NOTIFY lyricsChanged)
    // The lyrics (recognized text) or their timing come from whisper speech recognition, which made many
    // mistakes: they can be removed and fetched again.
    Q_PROPERTY(bool madeByWhisper READ madeByWhisper NOTIFY lyricsChanged)
    // How many tracks in the cache have such lyrics (-1 = not counted yet; countWhisperLyrics()).
    Q_PROPERTY(int whisperLyricsCount READ whisperLyricsCount NOTIFY whisperLyricsChanged)

    Q_PROPERTY(bool autoAlign READ autoAlign WRITE setAutoAlign NOTIFY settingsChanged)
    // The current line lights up letter by letter (off: the whole line at once, no word timing).
    Q_PROPERTY(bool karaoke READ karaoke WRITE setKaraoke NOTIFY settingsChanged)
    // Recognizing lyrics nobody has (whisper): the model, picked by the hardware.
    Q_PROPERTY(QString sttModel READ sttModel WRITE setSttModel NOTIFY settingsChanged)
    Q_PROPERTY(QStringList sttModels READ sttModels CONSTANT)
    Q_PROPERTY(bool sttEngineReady READ sttEngineReady NOTIFY sttChanged)
    Q_PROPERTY(bool sttCanDownloadEngine READ sttCanDownloadEngine CONSTANT)
    Q_PROPERTY(bool sttAlignerReady READ sttAlignerReady NOTIFY sttChanged)
    Q_PROPERTY(bool sttSeparatorReady READ sttSeparatorReady NOTIFY sttChanged)
    Q_PROPERTY(bool sttModelReady READ sttModelReady NOTIFY sttChanged)
    // Lyric timing works (the engine and the aligner are there).
    Q_PROPERTY(bool sttReady READ sttReady NOTIFY sttChanged)
    // Recognizing lyrics works (the engine and a whisper model).
    Q_PROPERTY(bool sttRecognizeReady READ sttRecognizeReady NOTIFY sttChanged)
    // Everything "download components" fetches is there (with a graphics card: the vocal separator too).
    Q_PROPERTY(bool sttComplete READ sttComplete NOTIFY sttChanged)
    Q_PROPERTY(bool sttDownloading READ sttDownloading NOTIFY sttChanged)
    Q_PROPERTY(qreal sttProgress READ sttProgress NOTIFY sttChanged)
    Q_PROPERTY(QString sttDownloadLabel READ sttDownloadLabel NOTIFY sttChanged)
    Q_PROPERTY(QString sttEngineSize READ sttEngineSize CONSTANT)
    Q_PROPERTY(QString sttAlignerSize READ sttAlignerSize CONSTANT)
    Q_PROPERTY(QString sttSeparatorSize READ sttSeparatorSize CONSTANT)
    Q_PROPERTY(bool hardwareOk READ hardwareOk NOTIFY sttChanged)
    Q_PROPERTY(bool recognizeHardwareOk READ recognizeHardwareOk NOTIFY sttChanged)
    Q_PROPERTY(QString hardwareSummary READ hardwareSummary NOTIFY sttChanged)
    Q_PROPERTY(QString recommendedModel READ recommendedModel NOTIFY sttChanged)
    // The graphics card the engine found ("" = none, or the engine isn't there yet), and whether the vocals
    // are separated on it before timing.
    Q_PROPERTY(QString sttGpuName READ sttGpuName NOTIFY sttChanged)
    Q_PROPERTY(bool sttUsesGpu READ sttUsesGpu NOTIFY sttChanged)
    Q_PROPERTY(bool sttSeparates READ sttSeparates NOTIFY sttChanged)

public:
    static LyricsService *instance();
    static LyricsService *create(QQmlEngine *, QJSEngine *);

    QString state() const { return m_state; }
    QString error() const { return m_error; }
    QVariantList lines() const { return m_lines; }
    bool synced() const { return m_lyrics.synced; }
    QString source() const { return m_lyrics.source; }
    QString origin() const { return m_lyrics.origin; }
    double offset() const { return m_offset; }

    bool aligning() const { return m_speech.busy() || !m_waitingStream.isEmpty(); }
    qreal alignProgress() const { return m_alignProgress; }
    QString alignStage() const { return m_alignStage; }
    bool canAlign() const;
    bool canTranscribe() const;
    bool recognizing() const { return aligning() && m_mode == Recognize; }
    bool canTimeWords() const;
    bool wordTimed() const { return m_lyrics.wordTimed; }
    bool madeByWhisper() const { return m_whisperMade; }
    int whisperLyricsCount() const { return m_whisperCount; }

    bool autoAlign() const { return m_autoAlign && hardwareOk(); }
    void setAutoAlign(bool on);
    bool karaoke() const { return m_karaoke; }
    void setKaraoke(bool on);
    QString sttModel() const { return m_model; }
    void setSttModel(const QString &model);
    QStringList sttModels() const { return SpeechEngine::modelNames(); }
    bool sttEngineReady() const { return m_speech.engineReady(); }
    bool sttCanDownloadEngine() const { return SpeechEngine::canDownloadEngine(); }
    bool sttAlignerReady() const { return m_speech.alignerReady(); }
    bool sttSeparatorReady() const { return m_speech.separatorReady(); }
    bool sttModelReady() const { return m_speech.modelReady(m_model); }
    bool sttReady() const { return m_speech.alignReady(); }
    bool sttRecognizeReady() const { return sttEngineReady() && sttModelReady(); }
    bool sttComplete() const { return sttReady() && (!m_speech.gpu().present() || sttSeparatorReady()); }
    bool sttDownloading() const { return m_speech.downloading(); }
    qreal sttProgress() const { return m_speech.downloadProgress(); }
    QString sttDownloadLabel() const { return m_speech.downloadLabel(); }
    QString sttEngineSize() const { return SpeechEngine::engineSize(); }
    QString sttAlignerSize() const { return SpeechEngine::alignerSize(); }
    QString sttSeparatorSize() const { return SpeechEngine::separatorSize(); }
    // Aligning known lyrics takes seconds on any computer; only recognizing the singing needs a strong one
    // (or a graphics card).
    bool hardwareOk() const { return m_hardware.threads >= 2; }
    bool recognizeHardwareOk() const { return m_hardware.sufficient || m_speech.gpu().present(); }
    QString hardwareSummary() const
    {
        return m_hardware.summary + (m_speech.gpu().name.isEmpty() ? QString() : QStringLiteral(" · ") + m_speech.gpu().name);
    }
    QString recommendedModel() const
    {
        return m_speech.gpu().memoryMb >= 3000 ? QStringLiteral("medium") : m_hardware.recommendedModel;
    }
    QString sttGpuName() const { return m_speech.gpu().name; }
    bool sttUsesGpu() const { return m_speech.engineReady() && m_speech.gpu().present(); }
    bool sttSeparates() const { return m_speech.engineReady() && m_speech.separates(); }

    // Index of the line (in `lines`) being sung right now; -1 before the first line or without timestamps.
    Q_INVOKABLE int lineAt(double seconds) const;
    Q_INVOKABLE QString modelSize(const QString &model) const { return SpeechEngine::modelSize(model); }
    Q_INVOKABLE void retry();
    Q_INVOKABLE void nudge(double seconds);   // shifts the timing of this track's lyrics
    Q_INVOKABLE void resetOffset();
    Q_INVOKABLE void alignNow();
    Q_INVOKABLE void transcribeNow();
    Q_INVOKABLE void timeWordsNow();
    Q_INVOKABLE void cancelAlign();
    Q_INVOKABLE void countWhisperLyrics();
    // Deletes every cached result of whisper (recognized lyrics, timing); those tracks are fetched again.
    Q_INVOKABLE void removeWhisperLyrics();
    Q_INVOKABLE void downloadSttComponents() { m_speech.downloadComponents(); }
    Q_INVOKABLE void removeSttComponents() { m_speech.removeComponents(); }
    Q_INVOKABLE void redetectStt() { m_speech.detect(); }
    Q_INVOKABLE void downloadSttModel() { m_speech.downloadModel(m_model); }
    Q_INVOKABLE void cancelSttDownload() { m_speech.cancelDownload(); }
    Q_INVOKABLE void removeSttModel(const QString &model) { m_speech.removeModel(model); }

    // A cache entry (or lyrics stored at that time) made by whisper speech recognition.
    static bool isWhisperMade(const Lyrics &lyrics, qint64 storedAtMs);
    static bool isWhisperMadeEntry(const QJsonObject &entry);

    // Called by the player when the track changes (nullptr = nothing playing).
    void setTrack(const Track *track);

signals:
    void lyricsChanged();
    void offsetChanged();
    void alignChanged();
    void settingsChanged();
    void sttChanged();
    void whisperLyricsChanged();
    void notice(const QString &text, const QString &icon);

private:
    explicit LyricsService(QObject *parent = nullptr);

    void fetch();
    void fetchYouTube(const QString &videoId, quint64 serial, std::function<void(const Lyrics &, const QString &)> done);
    void fetchLocal(quint64 serial, const Lyrics &fromYouTube, const QString &ytError);
    void withLrcLib(quint64 serial, const Lyrics &best, const QString &error);   // LRCLIB when no timing yet
    void withWebSearch(quint64 serial, const QString &error);                    // lyrics sites when nothing yet
    void finish(const Lyrics &lyrics, const QString &error);
    void apply(const Lyrics &lyrics, bool store);
    void setState(const QString &state, const QString &error = {});
    void maybeAutoAlign();
    // What a speech recognition job is for: timing the lines, writing the lyrics, timing every word.
    enum SpeechMode { Align, Recognize, Words };
    void runSpeech(SpeechMode mode);   // the track's audio (a YouTube stream is resolved first), then startAlign
    void startWordTiming(const QUrl &audio);
    void startAlign(const QUrl &audio);
    void startRecognize(const QUrl &audio);
    // The lines given to the aligner (those with words), and its answer back on the lyric lines.
    static QStringList alignedText(const Lyrics &lyrics, QList<int> &index);
    static QList<QList<LyricsAligner::Word>> perLine(const Lyrics &lyrics, const QList<QList<LyricsAligner::Word>> &found,
                                                     const QList<int> &index);

    QString cachePath(const QString &trackId) const;
    QString offsetKey() const;
    void loadOffset();
    bool loadCached(const QString &trackId, Lyrics &out, qint64 *storedAt = nullptr) const;
    void storeCached(const QString &trackId, const Lyrics &lyrics) const;

    LrcLib m_lrclib;
    LyricsSearch m_search;
    SpeechEngine m_speech;
    SpeechEngine::Hardware m_hardware;

    Track m_track;
    bool m_hasTrack = false;
    quint64 m_serial = 0;
    Lyrics m_lyrics;
    QVariantList m_lines;
    std::vector<std::pair<qint64, int>> m_starts;   // (start ms, line index) — for binary search
    double m_offset = 0;
    QString m_state = QStringLiteral("none");
    QString m_error;

    QString m_waitingStream;   // videoId whose URL we're waiting for to start recognition
    qreal m_alignProgress = 0;
    QString m_alignStage;
    QHash<QString, bool> m_alignTried;   // tracks for which recognition has already failed this session
    SpeechMode m_mode = Align;

    bool m_whisperMade = false;
    qint64 m_cachedAt = 0;      // when the lyrics being applied from the cache were stored
    int m_whisperCount = -1;

    bool m_autoAlign = false;
    bool m_karaoke = true;
    QString m_model;
};
