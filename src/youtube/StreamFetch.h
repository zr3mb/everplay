#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QWaitCondition>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

// The bytes of a stream coming in: filled by StreamFetch on the GUI thread, read by the player's
// demuxer thread through StreamDevice. `ready` is how much of the start has arrived without gaps,
// `arrived` the parts in past it (from -> end); `wanted` is where a waiting read is (a seek ahead).
struct StreamData
{
    QMutex mutex;
    QWaitCondition grown;
    QByteArray bytes;
    qint64 total = 0;
    qint64 ready = 0;
    QMap<qint64, qint64> arrived;
    qint64 wanted = -1;
    bool failed = false;
    int waitMs = 0;   // how long a read waits for its bytes (0: the usual)
};

// Fetches a whole YouTube audio stream into memory with Range requests, several parts at once.
// Played straight from the URL, the connection is read at playback speed, and YouTube's server
// drops such a slow connection after ~30 s — the sound cut out mid-track. The parts are asked for
// from the start, so playback can begin as soon as the first one is in (headReady) while the rest
// keeps coming; finished() says when it's all there.
class StreamFetch : public QObject
{
    Q_OBJECT

public:
    // headOnly: just the first part (a track the cursor rests on); continueAll() then fetches the rest.
    StreamFetch(QNetworkAccessManager *net, const QUrl &url, bool headOnly = false, QObject *parent = nullptr);
    ~StreamFetch() override;

    // A googlevideo.com stream that isn't huge (a very long mix keeps streaming from the URL), or a file on
    // Wikimedia's servers (fetched whole in one request: they turn away a player that keeps reconnecting).
    static bool suits(const QUrl &url);
    // Also worth fetching ahead (the next track, one the cursor rests on).
    static bool suitsAhead(const QUrl &url);

    // The stream's bytes, for a StreamDevice (null until the size is known)
    std::shared_ptr<StreamData> data() const { return m_shared; }
    // All of it, once finished(true)
    QByteArray takeData();
    QUrl url() const { return m_url; }
    bool headIsReady() const { return m_headSent; }
    bool headOnly() const { return m_headOnly; }
    bool whole() const { return m_wholeFile; }   // one request for all of it (Wikimedia), not parts
    // The player stops reading this stream: a read waiting for bytes still to come gives up at once.
    void abandonReads();
    void continueAll();

signals:
    void headReady();          // the start is in: a StreamDevice can be played
    void finished(bool ok);

private:
    struct Part { qint64 from; qint64 to; int attempts = 0; };

    void request(Part part);
    void fetchWhole();
    void plan(qint64 from);
    void startMore();
    void fail();
    void allocate(qint64 total);
    void store(const Part &part, const QByteArray &body);

    QNetworkAccessManager *m_net;
    QUrl m_url;
    std::shared_ptr<StreamData> m_shared;
    QByteArray m_whole;                        // a server that ignored the Range header sent it all at once
    qint64 m_total = -1;
    QList<Part> m_pending;                     // parts not asked for yet
    QList<QPointer<QNetworkReply>> m_replies;  // parts on their way
    qint64 m_received = 0;
    qint64 m_planned = 0;                      // parts up to here are asked for (or queued)
    bool m_headOnly = false;
    bool m_headSent = false;
    bool m_done = false;
    bool m_wholeFile = false;
};

// Plays a stream while it's still coming in: reads wait until the bytes they need have arrived.
class StreamDevice : public QIODevice
{
    Q_OBJECT

public:
    explicit StreamDevice(std::shared_ptr<StreamData> data, QObject *parent = nullptr);

    bool isSequential() const override { return false; }
    qint64 size() const override { return m_data->total; }

protected:
    qint64 readData(char *out, qint64 max) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    std::shared_ptr<StreamData> m_data;
};
