#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QMutex>
#include <QObject>
#include <QThread>

#include <array>
#include <atomic>
#include <vector>

class QAudioBuffer;
class QAudioSink;
class QMediaDevices;
class QTimer;

// Custom output path: decoded samples (from QAudioBufferOutput) go through the equalizer
// into a ring buffer that QAudioSink reads from. The buffer keeps a "cushion" of ~200 ms.
// Nothing of it runs on the GUI thread: the samples are mixed on the decoders' threads (push() is
// connected directly), and the sink has a thread of its own — so a busy interface (dragging the
// window's edge re-lays it out for every pixel) can't starve the sound. The state is under m_lock.
// Samples that were actually played are kept for the spectrum analyzer — so the visualizer
// shows what you hear (after EQ), at the right moment.
//
// Mixing: the player has two "decks" (two QMediaPlayers). Each one's samples go to a separate
// queue; outside a transition only the active deck plays, and during a transition both are summed
// with envelopes that depend on the style (crossfade, filter, bass swap, echo). The mix happens
// before the equalizer, so the equalizer, limiter and visualizer work on what you hear.
class AudioEngine : public QObject
{
    Q_OBJECT

public:
    static constexpr int kDecks = 2;

    enum class Style { Fade, Rise, BassSwap, Echo };

    struct Transition
    {
        Style style = Style::Fade;
        double seconds = 6.0;
        bool trimIntro = true;     // skip silence at the start of the new track
    };

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    QAudioFormat bufferFormat() const;   // the format we want samples from the player in
    int sampleRate() const { return m_format.sampleRate(); }

    void push(int deck, const QAudioBuffer &buffer);
    void flush();                        // clears everything (user changes track, seeks)
    void dropDeck(int deck);             // drops a deck's samples (e.g. a stopped one)
    void setActiveDeck(int deck);
    int activeDeck() const;
    void setPaused(bool paused);
    void setVolume(float linear);
    double latency() const;              // seconds between a decoded sample and hearing it

    // Transition from `from` to `to`. The envelope starts when the first samples of the new track
    // arrive (until then the old one plays normally), so a delayed start leaves no gap.
    void beginTransition(int from, int to, const Transition &t);
    void finishTransitionNow();          // e.g. a seek during the transition — the old deck goes silent at once
    void setDeckEnded(int deck);         // the deck reached the end of the file: no samples = silence, don't wait
    bool inTransition() const;
    double transitionProgress() const;

    // Deck loudness (dBFS): short-term (~0.1 s) and the average since the start of the track — to detect
    // silence and natural fade-outs at the end of a recording.
    double shortLevelDb(int deck) const;
    double averageLevelDb(int deck) const;
    void resetLevels(int deck);

    // Played samples (mono) since the last call — for the spectrum analyzer.
    void takePlayed(std::vector<float> &mono);

    // The sound follows the system's default output device (a headset plugged in, the default changed
    // in Windows) and comes back after the device fails. pinOutput() keeps it on the first device whose
    // name contains the text instead ("" = follow the default again); returns false when none matches.
    bool pinOutput(const QString &nameFragment);
    QAudioDevice outputDevice() const;

    // Everything below may be called from any thread.

signals:
    void transitionFinished(int fromDeck);
    void outputDeviceChanged();          // on the GUI thread: the decks' clocks should move to outputDevice()

private:
    class Ring;

    void followDevices();                // GUI thread: opens the sink again when the wanted device changed
    void openSink(const QAudioDevice &device);   // output thread

    struct Stage
    {
        std::vector<float> data;    // interleaved samples
        size_t read = 0;            // index of the first unread frame
        bool ended = false;
        size_t frames(int ch) const { return (data.size() / size_t(ch)) - read; }
        const float *at(size_t frame, int ch) const { return data.data() + (read + frame) * size_t(ch); }
        void consume(size_t n, int ch);
        void clear() { data.clear(); read = 0; ended = false; }
    };

    struct Levels
    {
        double shortEnergy = 0;     // smoothed energy (exponential)
        double sumEnergy = 0;
        double frames = 0;
    };

    struct TransitionState
    {
        bool active = false;
        bool started = false;       // the first (non-silent) samples of the new track have arrived
        int from = 0;
        int to = 1;
        Transition t;
        size_t length = 0;          // frames
        size_t pos = 0;
        size_t trimmed = 0;
        // effect state (per channel)
        std::array<double, 2> hpX1 {}, hpX2 {}, hpY1 {}, hpY2 {};
        std::array<double, 2> lpA {}, lpA2 {}, lpB {}, lpB2 {};
        std::vector<float> echo;
        size_t echoPos = 0;
    };

    void pump();
    void mix(size_t frames);
    void emit_(const float *frames, size_t count);
    void finishTransition();

    QAudioFormat m_format;
    QThread m_outputThread;               // the sink and its timers
    QObject *m_output = nullptr;          // lives in m_outputThread: invokeMethod's target there
    QAudioSink *m_sink = nullptr;         // used only on m_outputThread
    QMediaDevices *m_devices = nullptr;   // GUI thread
    QTimer *m_deviceCheck = nullptr;      // GUI thread: the device list changes in bursts
    mutable QMutex m_deviceLock;
    QAudioDevice m_device;                // what the sink plays on (under m_deviceLock)
    QByteArray m_pinnedId;                // GUI thread; empty = the system default
    std::atomic<bool> m_sinkFailed { false };
    std::atomic<float> m_volume { 1.0f };
    std::atomic<bool> m_paused { false };
    Ring *m_ring = nullptr;
    std::atomic<qint64> m_deviceQueuedUs { 0 };   // what the device holds (for latency())
    mutable QRecursiveMutex m_lock;      // the decks' queues, the transition, the levels
    std::vector<float> m_scratch;
    std::vector<float> m_out;
    std::array<Stage, kDecks> m_stage;
    std::array<Levels, kDecks> m_levels;
    int m_active = 0;
    TransitionState m_tr;
};
