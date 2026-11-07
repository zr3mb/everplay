#include "AudioEngine.h"

#include "Equalizer.h"

#include <QAudioBuffer>
#include <QAudioDevice>
#include <QAudioSink>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QTimer>

#include <utility>

#include <algorithm>
#include <cmath>
#include <cstring>

// Ring buffer read by QAudioSink (on the output thread) and written on the decoders' threads.
class AudioEngine::Ring : public QIODevice
{
public:
    Ring(const QAudioFormat &format, QObject *parent)
        : QIODevice(parent)
        , m_channels(format.channelCount())
        , m_int16(format.sampleFormat() == QAudioFormat::Int16)
        , m_capacity(size_t(format.sampleRate()) * 2)            // 2 s of headroom
        , m_prebuffer(size_t(format.sampleRate()) / 5)           // 200 ms cushion
        , m_data(m_capacity * size_t(m_channels), 0.0f)
    {
    }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

    void append(const float *samples, size_t frames)
    {
        QMutexLocker lock(&m_mutex);
        for (size_t f = 0; f < frames; ++f) {
            if (m_count == m_capacity) {           // overflow — drop the oldest
                m_read = (m_read + 1) % m_capacity;
                --m_count;
            }
            const size_t pos = (m_read + m_count) % m_capacity;
            std::memcpy(&m_data[pos * m_channels], samples + f * m_channels, sizeof(float) * m_channels);
            ++m_count;
        }
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_read = 0;
        m_count = 0;
        m_primed = false;
    }

    void takePlayed(std::vector<float> &out)
    {
        QMutexLocker lock(&m_mutex);
        out.swap(m_played);
        m_played.clear();
    }

    int takeUnderruns()
    {
        QMutexLocker lock(&m_mutex);
        return std::exchange(m_underruns, 0);
    }

    size_t buffered()
    {
        QMutexLocker lock(&m_mutex);
        return m_count;
    }

protected:
    qint64 writeData(const char *, qint64) override { return -1; }

    qint64 readData(char *data, qint64 maxSize) override
    {
        const int bytesPerSample = m_int16 ? 2 : 4;
        const size_t frames = size_t(maxSize) / size_t(bytesPerSample * m_channels);
        QMutexLocker lock(&m_mutex);

        // Gather the cushion first; after an underrun we gather it again.
        if (!m_primed && m_count >= m_prebuffer)
            m_primed = true;
        const size_t take = m_primed ? std::min(frames, m_count) : 0;
        if (m_primed && take < frames) {
            m_primed = false;
            ++m_underruns;
        }

        for (size_t f = 0; f < frames; ++f) {
            const float *src = f < take ? &m_data[((m_read + f) % m_capacity) * m_channels] : nullptr;
            float mono = 0.0f;
            for (int c = 0; c < m_channels; ++c) {
                const float v = src ? src[c] : 0.0f;
                mono += v;
                if (m_int16)
                    reinterpret_cast<qint16 *>(data)[f * m_channels + c] = qint16(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
                else
                    reinterpret_cast<float *>(data)[f * m_channels + c] = v;
            }
            if (src && m_played.size() < 16384)
                m_played.push_back(mono / m_channels);
        }
        m_read = (m_read + take) % m_capacity;
        m_count -= take;
        return qint64(frames) * bytesPerSample * m_channels;
    }

private:
    QMutex m_mutex;
    const int m_channels;
    const bool m_int16;
    const size_t m_capacity;
    const size_t m_prebuffer;
    std::vector<float> m_data;
    size_t m_read = 0;
    size_t m_count = 0;
    bool m_primed = false;
    std::vector<float> m_played;
    int m_underruns = 0;
};

namespace {

constexpr double kPi = 3.14159265358979323846;

double toDb(double energy)
{
    return energy > 1e-12 ? 10.0 * std::log10(energy) : -120.0;
}

double smoothstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Equal power: during a crossfade the summed energy doesn't dip in the middle.
double fadeIn(double t)  { return std::sin(std::clamp(t, 0.0, 1.0) * kPi / 2); }
double fadeOut(double t) { return std::cos(std::clamp(t, 0.0, 1.0) * kPi / 2); }

} // namespace

void AudioEngine::Stage::consume(size_t n, int ch)
{
    read += n;
    // Release the consumed part once it grows (instead of shifting on every block).
    if (read * size_t(ch) > 1 << 16 && read * size_t(ch) * 2 > data.size()) {
        data.erase(data.begin(), data.begin() + std::ptrdiff_t(read * size_t(ch)));
        read = 0;
    }
}

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
{
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    m_format = device.preferredFormat();
    if (m_format.sampleRate() <= 0)
        m_format.setSampleRate(48000);
    if (m_format.channelCount() <= 0)
        m_format.setChannelCount(2);
    m_format.setChannelCount(std::min(m_format.channelCount(), 2));
    m_format.setSampleFormat(QAudioFormat::Float);
    if (!device.isFormatSupported(m_format))
        m_format.setSampleFormat(QAudioFormat::Int16);

    Equalizer::instance()->setSampleRate(m_format.sampleRate());

    m_ring = new Ring(m_format, nullptr);
    m_ring->open(QIODevice::ReadOnly);

    // The sink is made on its own thread, so its timers (it pulls from the ring with them) run there.
    m_outputThread.setObjectName(QStringLiteral("Everplay audio output"));
    m_outputThread.start(QThread::TimeCriticalPriority);
    m_output = new QObject;
    m_output->moveToThread(&m_outputThread);
    m_ring->moveToThread(&m_outputThread);
    QMetaObject::invokeMethod(m_output, [this, device] {
        openSink(device);
        auto *probe = new QTimer(m_output);   // what the device holds, for latency() on other threads
        probe->setInterval(20);
        QObject::connect(probe, &QTimer::timeout, m_output, [this] {
            const qint64 queued = m_sink ? std::max<qint64>(0, m_sink->bufferSize() - m_sink->bytesFree()) : 0;
            m_deviceQueuedUs = m_format.durationForBytes(qint32(queued));
        });
        probe->start();
    }, Qt::BlockingQueuedConnection);

    // A new default device (headphones plugged in, the default changed in Windows' settings) or a device
    // that went away: the list changes in bursts, so it's looked at once it settles. The poll catches
    // a change the system didn't announce.
    m_deviceCheck = new QTimer(this);
    m_deviceCheck->setSingleShot(true);
    m_deviceCheck->setInterval(400);
    connect(m_deviceCheck, &QTimer::timeout, this, &AudioEngine::followDevices);
    m_devices = new QMediaDevices(this);
    connect(m_devices, &QMediaDevices::audioOutputsChanged, m_deviceCheck, qOverload<>(&QTimer::start));
    auto *poll = new QTimer(this);
    poll->setInterval(2000);
    connect(poll, &QTimer::timeout, this, [this] {
        if (!m_deviceCheck->isActive())
            followDevices();
    });
    poll->start();
}

void AudioEngine::openSink(const QAudioDevice &device)
{
    if (m_sink) {
        m_sink->disconnect(m_output);
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    {
        QMutexLocker lock(&m_deviceLock);
        m_device = device;
    }
    m_sinkFailed = false;
    if (!m_ring->isOpen())
        m_ring->open(QIODevice::ReadOnly);
    // The system resamples when the device runs at another rate, so the format stays the same.
    m_sink = new QAudioSink(device, m_format, m_output);
    m_sink->setBufferSize(m_format.bytesForDuration(80000));   // 80 ms device buffer
    m_sink->setVolume(m_volume);
    QObject::connect(m_sink, &QAudioSink::stateChanged, m_output, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && m_sink && m_sink->error() != QAudio::NoError) {
            // The device was unplugged or turned off — open the default one (or this one again) soon.
            qWarning("audio: the output device failed (error %d)", int(m_sink->error()));
            m_sinkFailed = true;
            QMetaObject::invokeMethod(this, [this] { m_deviceCheck->start(1500); });
        }
    });
    m_sink->start(m_ring);
    if (m_sink->error() != QAudio::NoError) {
        m_sinkFailed = true;
        QMetaObject::invokeMethod(this, [this] { m_deviceCheck->start(1500); });
    } else if (m_paused) {
        m_sink->suspend();
    }
    if (qEnvironmentVariableIsSet("EVERPLAY_AUDIO_DEBUG"))
        qInfo("audio: playing on \"%s\"", qPrintable(device.description()));
}

void AudioEngine::followDevices()
{
    QAudioDevice wanted;
    if (!m_pinnedId.isEmpty()) {
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            if (d.id() == m_pinnedId)
                wanted = d;
    }
    if (wanted.isNull())
        wanted = QMediaDevices::defaultAudioOutput();
    if (wanted.isNull())
        return;   // no output at all now — the next change brings one
    if (wanted.id() == outputDevice().id() && !m_sinkFailed)
        return;
    QMetaObject::invokeMethod(m_output, [this, wanted] { openSink(wanted); }, Qt::BlockingQueuedConnection);
    emit outputDeviceChanged();
}

bool AudioEngine::pinOutput(const QString &nameFragment)
{
    QByteArray id;
    if (!nameFragment.isEmpty()) {
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            if (id.isEmpty() && d.description().contains(nameFragment, Qt::CaseInsensitive))
                id = d.id();
        if (id.isEmpty())
            return false;
    }
    m_pinnedId = id;
    followDevices();
    return true;
}

QAudioDevice AudioEngine::outputDevice() const
{
    QMutexLocker lock(&m_deviceLock);
    return m_device;
}

AudioEngine::~AudioEngine()
{
    QMetaObject::invokeMethod(m_output, [this] {
        if (m_sink)
            m_sink->stop();
        delete m_output;   // the sink and the probe with it
        m_output = nullptr;
        m_sink = nullptr;
    }, Qt::BlockingQueuedConnection);
    m_outputThread.quit();
    m_outputThread.wait();
    delete m_ring;
}

QAudioFormat AudioEngine::bufferFormat() const
{
    QAudioFormat f;
    f.setSampleFormat(QAudioFormat::Float);
    f.setChannelCount(m_format.channelCount());
    f.setSampleRate(m_format.sampleRate());
    return f;
}

void AudioEngine::push(int deck, const QAudioBuffer &buffer)
{
    if (!buffer.isValid() || deck < 0 || deck >= kDecks)
        return;
    QMutexLocker lock(&m_lock);
    const QAudioFormat in = buffer.format();
    const int outCh = m_format.channelCount();
    const qsizetype frames = buffer.frameCount();
    m_scratch.resize(size_t(frames) * outCh);

    if (in.sampleFormat() == QAudioFormat::Float && in.channelCount() == outCh) {
        std::memcpy(m_scratch.data(), buffer.constData<float>(), m_scratch.size() * sizeof(float));
    } else {
        // A different format — convert sample by sample (spreading mono to both channels).
        const char *data = buffer.constData<char>();
        const int inCh = std::max(1, in.channelCount());
        const int bpf = in.bytesPerFrame();
        const int bps = in.bytesPerSample();
        for (qsizetype f = 0; f < frames; ++f)
            for (int c = 0; c < outCh; ++c)
                m_scratch[f * outCh + c] = in.normalizedSampleValue(data + f * bpf + std::min(c, inCh - 1) * bps);
    }

    // Deck loudness level (to detect silence and fade-outs at the end of a track).
    if (frames > 0) {
        double energy = 0;
        for (float v : m_scratch)
            energy += double(v) * v;
        energy /= double(m_scratch.size());
        Levels &l = m_levels[deck];
        const double alpha = 1.0 - std::exp(-double(frames) / (0.1 * m_format.sampleRate()));
        l.shortEnergy += alpha * (energy - l.shortEnergy);
        l.sumEnergy += energy * double(frames);
        l.frames += double(frames);
    }

    Stage &stage = m_stage[deck];
    stage.data.insert(stage.data.end(), m_scratch.begin(), m_scratch.end());
    pump();

    // Diagnostics (EVERPLAY_AUDIO_DEBUG=1): whether samples arrive at playback speed.
    static const bool debug = qEnvironmentVariableIsSet("EVERPLAY_AUDIO_DEBUG");
    if (debug && deck == m_active) {
        static QElapsedTimer clock;
        static qint64 total = 0;
        if (!clock.isValid())
            clock.start();
        total += frames;
        if (clock.elapsed() >= 3000) {
            qInfo("audio: %.2f s of audio in %.2f s (format %d Hz / %d ch., buffer underruns: %d, transition: %s)",
                  double(total) / m_format.sampleRate(), clock.elapsed() / 1000.0,
                  m_format.sampleRate(), outCh, m_ring->takeUnderruns(), m_tr.active ? "yes" : "no");
            total = 0;
            clock.restart();
        }
    }
}

// ---- mixing ---------------------------------------------------------------------

void AudioEngine::emit_(const float *frames, size_t count)
{
    const int ch = m_format.channelCount();
    m_out.assign(frames, frames + count * size_t(ch));
    Equalizer::instance()->process(m_out.data(), qsizetype(count), ch);
    m_ring->append(m_out.data(), count);
}

void AudioEngine::pump()
{
    const int ch = m_format.channelCount();
    if (!m_tr.active) {
        Stage &s = m_stage[m_active];
        if (const size_t n = s.frames(ch)) {
            emit_(s.at(0, ch), n);
            s.consume(n, ch);
        }
        Stage &other = m_stage[1 - m_active];      // outside a transition the other deck is silent
        if (const size_t n = other.frames(ch))
            other.consume(n, ch);
        return;
    }

    Stage &a = m_stage[m_tr.from];
    Stage &b = m_stage[m_tr.to];
    const size_t rate = size_t(m_format.sampleRate());

    if (!m_tr.started) {
        // Silence at the start of the new track (up to 4 s) is skipped — the transition starts with music.
        if (m_tr.t.trimIntro) {
            const size_t maxTrim = rate * 4;
            while (b.frames(ch) > 0 && m_tr.trimmed < maxTrim) {
                const float *f = b.at(0, ch);
                float peak = 0;
                for (int c = 0; c < ch; ++c)
                    peak = std::max(peak, std::abs(f[c]));
                if (peak > 0.004f)   // ~ -48 dBFS
                    break;
                b.consume(1, ch);
                ++m_tr.trimmed;
            }
        }
        if (b.frames(ch) > 0)
            m_tr.started = true;
        if (!m_tr.started) {
            // The new track isn't playing yet — the old one carries on unchanged.
            if (const size_t n = a.frames(ch)) {
                emit_(a.at(0, ch), n);
                a.consume(n, ch);
            }
            return;
        }
    }

    while (m_tr.active) {
        const size_t availA = a.frames(ch);
        const size_t availB = b.frames(ch);
        if (availB == 0)
            break;
        size_t n;
        if (availA >= availB)
            n = availB;
        else if (a.ended || availB > rate * 35 / 100)
            n = availB;    // the old track has ended (or stalled) — the missing samples are silence
        else
            n = availA;    // wait for the old deck so the two streams don't drift apart
        n = std::min(n, m_tr.length - m_tr.pos);
        if (n == 0)
            break;
        mix(n);
        if (m_tr.pos >= m_tr.length)
            finishTransition();
    }
    if (!m_tr.active) {
        Stage &s = m_stage[m_active];
        if (const size_t n = s.frames(ch)) {
            emit_(s.at(0, ch), n);
            s.consume(n, ch);
        }
    }
}

void AudioEngine::mix(size_t n)
{
    const int ch = m_format.channelCount();
    const double rate = m_format.sampleRate();
    Stage &a = m_stage[m_tr.from];
    Stage &b = m_stage[m_tr.to];
    const size_t availA = std::min(n, a.frames(ch));
    m_out.resize(n * size_t(ch));

    const double len = double(m_tr.length);
    const Style style = m_tr.t.style;

    // Bass swap: a 180 Hz crossover made of two one-pole filters.
    const double lpAlpha = 1.0 - std::exp(-2.0 * kPi * 180.0 / rate);
    // Rise: a high-pass filter on the old track, retuned every 32 frames.
    double hb0 = 1, hb1 = 0, hb2 = 0, ha1 = 0, ha2 = 0;
    const size_t echoFrames = m_tr.echo.size() / size_t(std::max(1, ch));

    for (size_t i = 0; i < n; ++i) {
        const double t = double(m_tr.pos + i) / len;

        if (style == Style::Rise && i % 32 == 0) {
            const double sweep = std::clamp(t / 0.85, 0.0, 1.0);
            const double fc = 30.0 * std::pow(4000.0 / 30.0, sweep * sweep);
            const double w0 = 2 * kPi * fc / rate;
            const double alpha = std::sin(w0) / (2 * 0.8);
            const double cosw = std::cos(w0);
            const double a0 = 1 + alpha;
            hb0 = (1 + cosw) / 2 / a0;
            hb1 = -(1 + cosw) / a0;
            hb2 = (1 + cosw) / 2 / a0;
            ha1 = -2 * cosw / a0;
            ha2 = (1 - alpha) / a0;
        }

        for (int c = 0; c < ch; ++c) {
            const double xa = i < availA ? a.at(i, ch)[c] : 0.0;
            const double xb = b.at(i, ch)[c];
            const int k = std::min(c, 1);
            double y = 0;
            switch (style) {
            case Style::Fade:
                y = xa * fadeOut(t) + xb * fadeIn(t);
                break;
            case Style::Rise: {
                const double ya = hb0 * xa + hb1 * m_tr.hpX1[k] + hb2 * m_tr.hpX2[k] - ha1 * m_tr.hpY1[k] - ha2 * m_tr.hpY2[k];
                m_tr.hpX2[k] = m_tr.hpX1[k];
                m_tr.hpX1[k] = xa;
                m_tr.hpY2[k] = m_tr.hpY1[k];
                m_tr.hpY1[k] = ya;
                const double gA = t < 0.7 ? 1.0 : fadeOut((t - 0.7) / 0.3);
                const double gB = t < 0.35 ? 0.0 : fadeIn((t - 0.35) / 0.65);
                y = ya * gA + xb * gB;
                break;
            }
            case Style::BassSwap: {
                m_tr.lpA[k] += lpAlpha * (xa - m_tr.lpA[k]);
                m_tr.lpA2[k] += lpAlpha * (m_tr.lpA[k] - m_tr.lpA2[k]);
                m_tr.lpB[k] += lpAlpha * (xb - m_tr.lpB[k]);
                m_tr.lpB2[k] += lpAlpha * (m_tr.lpB[k] - m_tr.lpB2[k]);
                const double lowA = m_tr.lpA2[k], highA = xa - lowA;
                const double lowB = m_tr.lpB2[k], highB = xb - lowB;
                // The highs crossfade over the whole transition, the bass switches quickly halfway
                // (like a DJ: never two kick drums at once).
                const double swap = smoothstep(0.46, 0.54, t);
                y = highA * fadeOut(t) + highB * fadeIn(t) + lowA * (1.0 - swap) + lowB * swap;
                break;
            }
            case Style::Echo: {
                const double dry = t < 0.25 ? fadeOut(t / 0.25) : 0.0;
                double wet = 0;
                if (echoFrames > 0) {
                    float &slot = m_tr.echo[m_tr.echoPos * size_t(ch) + size_t(c)];
                    wet = slot;
                    slot = float(xa * dry * 0.55 + wet * 0.45);
                }
                const double gB = t < 0.3 ? 0.0 : fadeIn((t - 0.3) / 0.7);
                y = xa * dry + wet * (1.0 - smoothstep(0.8, 1.0, t)) + xb * gB;
                break;
            }
            }
            m_out[i * size_t(ch) + size_t(c)] = float(y);
        }
        if (style == Style::Echo && echoFrames > 0)
            m_tr.echoPos = (m_tr.echoPos + 1) % echoFrames;
    }

    a.consume(availA, ch);
    b.consume(n, ch);
    m_tr.pos += n;
    Equalizer::instance()->process(m_out.data(), qsizetype(n), ch);
    m_ring->append(m_out.data(), n);
}

void AudioEngine::beginTransition(int from, int to, const Transition &t)
{
    QMutexLocker lock(&m_lock);
    if (from == to || from < 0 || to < 0 || from >= kDecks || to >= kDecks)
        return;
    m_tr = TransitionState {};
    m_tr.active = true;
    m_tr.from = from;
    m_tr.to = to;
    m_tr.t = t;
    m_tr.length = std::max<size_t>(1, size_t(std::max(0.2, t.seconds) * m_format.sampleRate()));
    if (t.style == Style::Echo)
        m_tr.echo.assign(size_t(0.375 * m_format.sampleRate()) * size_t(m_format.channelCount()), 0.0f);
    m_active = from;
    pump();
}

void AudioEngine::finishTransition()
{
    const int from = m_tr.from;
    m_stage[from].clear();
    m_active = m_tr.to;
    m_tr = TransitionState {};
    emit transitionFinished(from);
}

void AudioEngine::finishTransitionNow()
{
    QMutexLocker lock(&m_lock);
    if (!m_tr.active)
        return;
    finishTransition();
    pump();
}

bool AudioEngine::inTransition() const
{
    QMutexLocker lock(&m_lock);
    return m_tr.active;
}

int AudioEngine::activeDeck() const
{
    QMutexLocker lock(&m_lock);
    return m_active;
}

double AudioEngine::transitionProgress() const
{
    QMutexLocker lock(&m_lock);
    return m_tr.active && m_tr.length > 0 ? double(m_tr.pos) / double(m_tr.length) : 0.0;
}

void AudioEngine::setDeckEnded(int deck)
{
    if (deck < 0 || deck >= kDecks)
        return;
    QMutexLocker lock(&m_lock);
    m_stage[deck].ended = true;
    pump();
}

void AudioEngine::dropDeck(int deck)
{
    QMutexLocker lock(&m_lock);
    if (deck >= 0 && deck < kDecks)
        m_stage[deck].clear();
}

void AudioEngine::setActiveDeck(int deck)
{
    QMutexLocker lock(&m_lock);
    if (deck >= 0 && deck < kDecks)
        m_active = deck;
}

double AudioEngine::shortLevelDb(int deck) const
{
    QMutexLocker lock(&m_lock);
    return toDb(m_levels[deck].shortEnergy);
}

double AudioEngine::averageLevelDb(int deck) const
{
    QMutexLocker lock(&m_lock);
    const Levels &l = m_levels[deck];
    return l.frames > 0 ? toDb(l.sumEnergy / l.frames) : -120.0;
}

void AudioEngine::resetLevels(int deck)
{
    QMutexLocker lock(&m_lock);
    if (deck >= 0 && deck < kDecks)
        m_levels[deck] = Levels {};
}

// ---- the rest -----------------------------------------------------------------

void AudioEngine::flush()
{
    QMutexLocker lock(&m_lock);
    m_ring->clear();
    for (Stage &s : m_stage)
        s.clear();
    m_tr = TransitionState {};
    Equalizer::instance()->resetState();
}

void AudioEngine::setPaused(bool paused)
{
    m_paused = paused;
    QMetaObject::invokeMethod(m_output, [this, paused] {
        if (!m_sink)
            return;
        if (paused && m_sink->state() == QAudio::ActiveState)
            m_sink->suspend();
        else if (!paused && m_sink->state() == QAudio::SuspendedState)
            m_sink->resume();
    });
}

void AudioEngine::setVolume(float linear)
{
    m_volume = linear;
    QMetaObject::invokeMethod(m_output, [this, linear] {
        if (m_sink)
            m_sink->setVolume(linear);
    });
}

double AudioEngine::latency() const
{
    // What is decoded but not heard yet: the ring buffer plus the audio device's own buffer.
    const double rate = m_format.sampleRate();
    const double seconds = rate > 0 ? double(m_ring->buffered()) / rate : 0.0;
    return seconds + double(m_deviceQueuedUs.load()) / 1e6;
}

void AudioEngine::takePlayed(std::vector<float> &mono)
{
    m_ring->takePlayed(mono);
}
