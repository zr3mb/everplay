#pragma once

#include "SpectrumAnalyzer.h"
#include "audio/AudioCache.h"
#include "audio/AudioEngine.h"

#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QColor>
#include <QElapsedTimer>
#include <QList>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <array>
#include <functional>

class QBuffer;
class StreamFetch;
class TrackModel;
struct Track;

// Player: QMediaPlayer (FFmpeg backend) + queue + spectrum from the real audio.
// Two "decks" (two QMediaPlayers) make it possible to mix tracks: the next one is loaded in advance
// and starts during the ending of the current one (transitions in AudioEngine). Mix settings are
// global (crossfade) or separate per playlist / album ("Mix" like in Spotify).
class PlayerController : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Player)
    QML_SINGLETON

    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY currentChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)
    Q_PROPERTY(QString title READ title NOTIFY currentChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY currentChanged)
    Q_PROPERTY(QString artistId READ artistId NOTIFY currentChanged)   // YouTube Music channel, "" for local files
    Q_PROPERTY(QString album READ album NOTIFY currentChanged)
    Q_PROPERTY(QString albumKey READ albumKey NOTIFY currentChanged)
    Q_PROPERTY(QString source READ source NOTIFY currentChanged)
    Q_PROPERTY(QString quality READ quality NOTIFY currentChanged)
    Q_PROPERTY(QString cover READ cover NOTIFY currentChanged)
    Q_PROPERTY(QColor colorA READ colorA NOTIFY colorsChanged)
    Q_PROPERTY(QColor colorB READ colorB NOTIFY colorsChanged)
    Q_PROPERTY(int seed READ seed NOTIFY currentChanged)
    Q_PROPERTY(int duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool live READ live NOTIFY currentChanged)
    Q_PROPERTY(bool liked READ liked NOTIFY likedChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY shuffleChanged)
    Q_PROPERTY(int repeatMode READ repeatMode WRITE setRepeatMode NOTIFY repeatModeChanged)
    Q_PROPERTY(QVariantList upNext READ upNext NOTIFY queueChanged)
    // The queue after the current track for the full-screen player: its first 200 rows, how many there are, their length
    Q_PROPERTY(QVariantList queueAhead READ queueAhead NOTIFY queueChanged)
    Q_PROPERTY(int queueLeft READ queueLeft NOTIFY queueChanged)
    Q_PROPERTY(int queueSeconds READ queueSeconds NOTIFY queueChanged)
    Q_PROPERTY(QList<qreal> spectrum READ spectrum NOTIFY spectrumChanged)
    Q_PROPERTY(qreal level READ level NOTIFY spectrumChanged)

    // ---- mixing ----
    Q_PROPERTY(int crossfade READ crossfade WRITE setCrossfade NOTIFY mixChanged)               // s, 0 = off
    Q_PROPERTY(QString crossfadeStyle READ crossfadeStyle WRITE setCrossfadeStyle NOTIFY mixChanged)
    Q_PROPERTY(bool smartTransitions READ smartTransitions WRITE setSmartTransitions NOTIFY mixChanged)
    Q_PROPERTY(QStringList mixStyles READ mixStyles NOTIFY mixChanged)
    Q_PROPERTY(int mixRevision READ mixRevision NOTIFY mixChanged)
    Q_PROPERTY(QString queueContext READ queueContext NOTIFY queueChanged)
    Q_PROPERTY(bool mixActive READ mixActive NOTIFY mixChanged)          // the current queue will be mixed
    Q_PROPERTY(bool transitioning READ transitioning NOTIFY transitionChanged)
    Q_PROPERTY(QString transitionLabel READ transitionLabel NOTIFY transitionChanged)
    // When the queue ends: continue with the next playlist (or the whole library) instead of stopping.
    Q_PROPERTY(bool autoplay READ autoplay WRITE setAutoplay NOTIFY mixChanged)
    // Network tracks kept on disk (AudioCache): a revision for bindings, the limit and what's used, in MB
    Q_PROPERTY(int cacheRevision READ cacheRevision NOTIFY cacheChanged)
    Q_PROPERTY(int cacheLimitMB READ cacheLimitMB WRITE setCacheLimitMB NOTIFY cacheChanged)
    Q_PROPERTY(int cacheUsedMB READ cacheUsedMB NOTIFY cacheChanged)
    // A listening room (Online) drives the player: the queue is the room's.
    Q_PROPERTY(bool inRoom READ inRoom NOTIFY roomLinkChanged)

public:
    explicit PlayerController(QObject *parent = nullptr);
    ~PlayerController() override;

    bool hasTrack() const { return m_index >= 0; }
    int currentIndex() const { return m_index; }
    QString title() const;
    QString artist() const;
    QString artistId() const;
    QString album() const;
    QString albumKey() const;
    QString source() const;
    QString quality() const;
    QString cover() const;
    QColor colorA() const;
    QColor colorB() const;
    int seed() const;
    int duration() const;
    bool live() const;
    bool liked() const;

    double position() const;
    bool playing() const { return player().playbackState() == QMediaPlayer::PlayingState; }
    bool buffering() const;
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    bool shuffle() const { return m_shuffle; }
    int repeatMode() const { return m_repeat; }
    QVariantList upNext() const { return rowsAhead(6); }
    QVariantList queueAhead() const { return rowsAhead(200); }
    int queueLeft() const;
    int queueSeconds() const;
    QList<qreal> spectrum() const { return m_spectrum; }
    qreal level() const { return m_level; }

    int crossfade() const { return m_crossfade; }
    void setCrossfade(int seconds);
    QString crossfadeStyle() const { return m_crossfadeStyle; }
    void setCrossfadeStyle(const QString &style);
    bool smartTransitions() const { return m_smart; }
    void setSmartTransitions(bool on);
    QStringList mixStyles() const;
    int mixRevision() const { return m_mixRevision; }
    QString queueContext() const { return m_context; }
    bool mixActive() const { return effectiveMix().enabled; }
    bool transitioning() const { return m_engine.inTransition(); }
    QString transitionLabel() const { return m_transitionLabel; }
    bool autoplay() const { return m_autoplay; }
    void setAutoplay(bool on);

    int cacheRevision() const { return m_cacheRevision; }
    int cacheLimitMB() const { return int(m_cache.limitBytes() / (1024 * 1024)); }
    void setCacheLimitMB(int mb) { m_cache.setLimitBytes(qint64(mb) * 1024 * 1024); }
    int cacheUsedMB() const { return int((m_cache.usedBytes() + 512 * 1024) / (1024 * 1024)); }
    // The row's audio is on disk: it plays at once, without the network.
    Q_INVOKABLE bool cached(int row) const;
    Q_INVOKABLE void clearCache() { m_cache.clear(); }
    // Developer tours: plays on the first output device whose name contains the text ("" = the system default).
    Q_INVOKABLE bool pinOutputDevice(const QString &nameFragment) { return m_engine.pinOutput(nameFragment); }
    Q_INVOKABLE QString outputDeviceName() const;

    void setVolume(double v);
    void setMuted(bool m);
    void setShuffle(bool s);
    void setRepeatMode(int r);

    // Plays row `row`; `rows` become the queue (e.g. an album, a playlist, search results).
    // `context` identifies the queue's origin ("playlist:…", "album:…") — the mix depends on it.
    Q_INVOKABLE void playFrom(const QVariantList &rows, int row, const QString &context = {});
    // The rows in a random order, once — the shuffle mode (the player bar's toggle) stays as it was.
    Q_INVOKABLE void playShuffled(const QVariantList &rows, const QString &context = {});
    // Plays row `row` with the whole local library as the queue.
    Q_INVOKABLE void playIndex(int row);
    // Jumps to a track in the current queue (e.g. a click in "Up next").
    Q_INVOKABLE void jumpTo(int row);
    // Prepares the URL of a YouTube track in advance (e.g. on hovering over a row).
    Q_INVOKABLE void prefetch(int row);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void cycleRepeat();
    Q_INVOKABLE void toggleLiked();
    // A radio of the track at `row` (Radio): it plays (or keeps playing) and tracks like it follow;
    // the station grows while it plays. The queue's context is "radio:<seed id>".
    Q_INVOKABLE void startRadio(int row);

    // Playlist / album mix: { enabled, style, length }.
    Q_INVOKABLE QVariantMap mixFor(const QString &context) const;
    Q_INVOKABLE void setMix(const QString &context, bool enabled, const QString &style, int length);
    Q_INVOKABLE QString mixStyleName(const QString &style) const;
    Q_INVOKABLE QString mixStyleDescription(const QString &style) const;

    // For integrations (Discord, MPRIS)
    QString currentTrackId() const { return m_currentId; }
    const Track *currentTrack() const { return current(); }

    // ---- listening rooms (OnlineService) ----
    // While a room is joined, the user's actions don't play locally: `intercept` gets them
    // ("play" + row, "queue" + rows, "resume", "pause" + seconds, "next", "previous", "seek" + seconds)
    // and the room decides. `movedOn` reports a track the player left by itself (its end, a transition).
    struct RoomLink
    {
        std::function<bool(const QString &action, const QVariant &arg)> intercept;
        std::function<void(const QString &trackId)> movedOn;
    };
    void setRoomLink(RoomLink link);
    bool inRoom() const { return bool(m_room.intercept); }
    // The room's state: its queue from the current track on (empty = nothing plays), where that
    // track is and whether it plays. Small drifts are left alone; over 3 s the player seeks.
    void applyRoom(const QStringList &ids, double seconds, bool play);

signals:
    void currentChanged();
    void colorsChanged();
    void durationChanged();
    void likedChanged();
    void positionChanged();
    void playingChanged();
    void bufferingChanged();
    void volumeChanged();
    void mutedChanged();
    void shuffleChanged();
    void repeatModeChanged();
    void queueChanged();
    void spectrumChanged();
    void mixChanged();
    void transitionChanged();
    void seeked(double seconds);
    void notice(const QString &text, const QString &icon);
    void roomLinkChanged();
    void cacheChanged();

private:
    static constexpr int kBars = 48;

    QVariantList rowsAhead(int limit) const;   // library rows after the current track, in play order

    struct Deck
    {
        QMediaPlayer *player = nullptr;
        QAudioOutput *clock = nullptr;          // muted — sets the playback pace
        QAudioBufferOutput *buffers = nullptr;
        QString trackId;
        QString resolving;                      // videoId whose URL we're waiting for
        StreamFetch *fetch = nullptr;           // a YouTube stream coming into memory
        QIODevice *memory = nullptr;            // the stream it plays from (in memory or still coming in)
        QIODevice *previousMemory = nullptr;      // kept a track longer — the player may still be closing it
    };

    struct MixConfig
    {
        bool enabled = false;
        QString style = QStringLiteral("auto");
        int length = 8;
    };

    QMediaPlayer &player() { return *m_decks[m_active].player; }
    const QMediaPlayer &player() const { return *m_decks[m_active].player; }
    int standby() const { return 1 - m_active; }
    void setupDeck(int d);
    void onDeckMediaStatus(int d, QMediaPlayer::MediaStatus status);
    MixConfig effectiveMix() const;
    void loadMixSettings();
    void saveMixSettings();
    int nextQueuePos() const;
    void preloadNext();
    void cancelPreload();
    void releaseReads(Deck &deck);
    bool standbyReady() const;
    void updateMix(double dt);
    void startTransition(AudioEngine::Style style, double seconds, const QString &label);
    void stopTransition();

    const Track *current() const;
    bool canPlay(int row, bool explain);
    void load(const QString &id, bool autoplay, bool flushAudio = true);
    void emitTrackChanged();
    void prefetchNext();
    void advance(bool userInitiated);
    QStringList queueIds(const QVariantList &rows);
    void startQueue(const QStringList &ids, int pos, const QString &context);
    bool continueAfterQueue();
    void extendRadio();        // near the end of a radio's queue: more from the current track's radio
    void prepareAutoRadio();   // the queue's last track: a radio to go on with when it ends (autoplay)
    void skipAfterError(const QString &message);
    void checkStall(double dt);
    void countListening(double dt);
    void rememberPosition() const;
    void storeResume() const;
    void startStream(int deck, const Track &t);
    void setStream(int deck, const QString &videoId, const QUrl &url);
    QString streamIdOf(const Track &t) const;
    bool retryStream(int deck);
    void prefetchTrack(int row, bool full = false);
    void startAhead(const QString &videoId, const QUrl &url, bool full);
    StreamFetch *takeAhead(const QString &videoId, const QUrl &url);
    void warmUp();
    void onModelReset();
    void onMediaStatus(QMediaPlayer::MediaStatus status);
    void ensureTicking();
    void tick();
    void saveState() const;
    void applyVolume();
    bool roomIntercept(const QString &action, const QVariant &arg = {});

    TrackModel *m_tracks = nullptr;
    AudioEngine m_engine;               // the actual output: deck mixer + equalizer + QAudioSink
    QNetworkAccessManager m_net;        // YouTube streams fetched into memory
    AudioCache m_cache;                 // ...and kept on disk afterwards
    // Streams fetched before they're played: the start of a track the cursor rests on, the whole next
    // track of the queue. Playing one takes its fetch over. In the order they were started.
    QHash<QString, StreamFetch *> m_ahead;
    QStringList m_aheadOrder;
    QHash<QString, bool> m_wantAhead;   // waiting for their URL: videoId -> the whole track
    int m_cacheRevision = 0;
    std::array<Deck, AudioEngine::kDecks> m_decks;
    int m_active = 0;
    SpectrumAnalyzer m_analyzer;

    QString m_currentId;
    int m_index = -1;
    QStringList m_queue;
    int m_queuePos = -1;
    QStringList m_history;
    qint64 m_pendingSeek = -1;
    mutable QHash<QString, qint64> m_resume;   // long recordings: where they were left (ms)
    bool m_autoplayAfterResolve = false;
    QString m_context;
    bool m_autoplay = true;
    bool m_radioExtending = false;
    QString m_autoRadioSeed;           // the track the prepared radio is of
    QStringList m_autoRadio;
    bool m_wantPlay = false;           // the user expects music (cleared by pause) — errors then skip ahead
    int m_failures = 0;                // failed tracks in a row — stops skipping after a few
    QElapsedTimer m_loadClock;         // EVERPLAY_STREAM_DEBUG: time from a track change to sound
    QSet<QString> m_streamRetried;     // videoIds whose stream already got a second, fresh URL
    QString m_skipPending;             // track id with a skip already scheduled (errors come in pairs)
    qint64 m_lastPosMs = -1;           // stall detection: the position stopped moving while "playing"
    double m_stall = 0;
    double m_heardLag = 0;             // seconds decoded but not heard yet (AudioEngine::latency, smoothed)
    QString m_listenedId;              // the track whose listening time is being counted (recommendations)
    double m_listened = 0;
    bool m_listenCounted = false;

    // mixing
    int m_crossfade = 0;
    QString m_crossfadeStyle = QStringLiteral("fade");
    bool m_smart = true;
    QHash<QString, MixConfig> m_mixes;
    int m_mixRevision = 0;
    int m_plannedPos = -1;             // queue position of the track loaded on the other deck
    QString m_plannedId;
    QString m_shuffleNextId;           // the next track picked in advance by shuffle (URL prefetch)
    double m_silence = 0;              // how long the current track has been silent (s) — smart transitions
    double m_quiet = 0;                // how long the current track has been clearly faded out (s)
    QString m_transitionLabel;

    double m_volume = 0.72;
    bool m_muted = false;
    bool m_shuffle = false;
    int m_repeat = 0; // 0 = none, 1 = all, 2 = one track

    QTimer m_timer;
    QElapsedTimer m_clock;
    QList<qreal> m_spectrum;
    QList<qreal> m_targets;
    std::vector<float> m_played;
    qreal m_level = 0.0;

    RoomLink m_room;
    bool m_roomApplying = false;       // the room's own state is being applied — not a user action
    QString m_roomTrack;               // the track the room knows we play (to report moving on from it)
};
