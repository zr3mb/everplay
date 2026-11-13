#include "StreamFetch.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QThread>
#include <QUrlQuery>

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

constexpr qint64 kPart = 1024 * 1024;
constexpr qint64 kHead = 512 * 1024;              // the first part, smaller: playback starts once it's in
constexpr int kParallel = 4;
constexpr qint64 kMaxSize = 96 * 1024 * 1024;   // ~1.5 h of audio — longer ones stream from the URL
constexpr int kReadWaitMs = 20000;               // a read waiting longer than this for its bytes fails

qint64 declaredLength(const QUrl &url)
{
    return QUrlQuery(url).queryItemValue(QStringLiteral("clen")).toLongLong();   // 0 when missing
}

// Wikimedia's file servers answer "429 Too Many Requests" to a player that asks for a big file again and
// again (every seek a new connection, or many parts). Their files (the open FLAC sources) come in one
// request, as fast as the connection allows; seeking reads what has arrived, waiting for the rest.
bool isWikimedia(const QUrl &url)
{
    return url.host() == QLatin1String("upload.wikimedia.org");
}

constexpr qint64 kMaxWhole = 320LL * 1024 * 1024;   // a 24-bit / 192 kHz recording is ~6 MB a minute
constexpr int kWholeReadWaitMs = 60000;             // a seek far ahead waits until the download gets there

void abortAll(QList<QPointer<QNetworkReply>> replies, QObject *receiver)
{
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            reply->disconnect(receiver);
            reply->abort();
            reply->deleteLater();
        }
    }
}

} // namespace

StreamFetch::StreamFetch(QNetworkAccessManager *net, const QUrl &url, bool headOnly, QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_url(url)
    , m_headOnly(headOnly)
{
    if (isWikimedia(url)) {
        fetchWhole();
        return;
    }
    if (const qint64 length = declaredLength(url); length > 0) {
        allocate(length);
        plan(0);
        startMore();
    } else {
        m_planned = kHead;
        request({ 0, kHead - 1 });   // the first part also tells the size (Content-Range)
    }
}

void StreamFetch::continueAll()
{
    if (!m_headOnly)
        return;
    m_headOnly = false;
    if (m_total > 0) {
        plan(m_planned);
        startMore();
    }
}

void StreamFetch::abandonReads()
{
    if (!m_shared)
        return;
    QMutexLocker lock(&m_shared->mutex);
    m_shared->failed = true;
    m_shared->grown.wakeAll();
}

StreamFetch::~StreamFetch()
{
    abortAll(std::exchange(m_replies, {}), this);
    // A player still reading what never came gets an error instead of waiting.
    if (!m_done && m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->failed = true;
        m_shared->grown.wakeAll();
    }
}

bool StreamFetch::suits(const QUrl &url)
{
    // googlevideo.com directly, and Wikimedia's files. Audio relayed by the Everplay server (the Android
    // app) plays from the URL as it arrives: the whole file first would add the relay's transfer to the wait.
    return (url.host().endsWith(QLatin1String("googlevideo.com")) && declaredLength(url) <= kMaxSize) || isWikimedia(url);
}

bool StreamFetch::suitsAhead(const QUrl &url)
{
    return suits(url) && !isWikimedia(url);   // one whole download per play, not another one ahead of it
}

void StreamFetch::fetchWhole()
{
    m_wholeFile = true;
    QNetworkRequest req(m_url);
    req.setRawHeader("User-Agent", QByteArrayLiteral("Everplay/" EVERPLAY_VERSION " (desktop music player)"));
    req.setTransferTimeout(30000);
    QNetworkReply *reply = m_net->get(req);
    m_replies << reply;
    // The size from the headers: the bytes then go where the player reads them as they come.
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (m_shared || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || length <= 0)
            return;
        if (length > kMaxWhole) {
            fail();   // played from the URL instead
            return;
        }
        allocate(length);
        m_shared->waitMs = kWholeReadWaitMs;
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (!m_shared || m_done)
            return;   // without a size it's all read at the end
        const QByteArray chunk = reply->readAll();
        if (m_received + chunk.size() > m_total) {
            fail();
            return;
        }
        if (!chunk.isEmpty())
            store({ m_received, m_received + chunk.size() - 1 }, chunk);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_replies.removeIf([reply](const QPointer<QNetworkReply> &r) { return r == reply; });
        if (m_done)
            return;
        if (reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            fail();
            return;
        }
        const QByteArray rest = reply->readAll();
        if (!m_shared) {   // no size announced: the whole file at once
            m_whole = rest;
            m_done = true;
            emit finished(!m_whole.isEmpty());
            return;
        }
        if (!rest.isEmpty() && m_received + rest.size() <= m_total)
            store({ m_received, m_received + rest.size() - 1 }, rest);
        if (m_received != m_total) {
            fail();
            return;
        }
        m_done = true;
        emit finished(true);
    });
}

QByteArray StreamFetch::takeData()
{
    if (!m_whole.isEmpty())
        return std::exchange(m_whole, {});
    if (!m_shared)
        return {};
    QMutexLocker lock(&m_shared->mutex);
    return m_shared->bytes;   // shared, not copied: nothing writes to it any more
}

void StreamFetch::allocate(qint64 total)
{
    m_total = total;
    m_shared = std::make_shared<StreamData>();
    m_shared->bytes = QByteArray(total, Qt::Uninitialized);
    m_shared->total = total;
}

void StreamFetch::plan(qint64 from)
{
    // The first part is the small head; a head-only fetch stops after it.
    for (qint64 at = from; at < m_total; ) {
        const qint64 end = std::min(at == 0 ? kHead : at + kPart, m_total);
        m_pending << Part{ at, end - 1 };
        at = end;
        m_planned = end;
        if (m_headOnly)
            break;
    }
}

void StreamFetch::startMore()
{
    // A read is waiting further on (a seek ahead): the parts from there on go first.
    qint64 wanted = -1;
    if (m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        wanted = std::exchange(m_shared->wanted, -1);
    }
    if (wanted >= 0)
        std::stable_partition(m_pending.begin(), m_pending.end(), [wanted](const Part &p) { return p.to >= wanted; });
    while (m_replies.size() < kParallel && !m_pending.isEmpty())
        request(m_pending.takeFirst());
}

void StreamFetch::fail()
{
    m_done = true;
    abortAll(std::exchange(m_replies, {}), this);
    if (m_shared) {
        QMutexLocker lock(&m_shared->mutex);
        m_shared->failed = true;
        m_shared->grown.wakeAll();
    }
    emit finished(false);
}

void StreamFetch::store(const Part &part, const QByteArray &body)
{
    {
        QMutexLocker lock(&m_shared->mutex);
        std::memcpy(m_shared->bytes.data() + part.from, body.constData(), size_t(body.size()));
        // Parts arrive in any order; the readable start grows while there's no gap.
        m_shared->arrived.insert(part.from, part.from + body.size());
        while (m_shared->arrived.contains(m_shared->ready))
            m_shared->ready = m_shared->arrived.take(m_shared->ready);
        m_shared->grown.wakeAll();
    }
    m_received += body.size();
    if (!m_headSent && m_shared->ready >= std::min(m_total, kHead)) {
        m_headSent = true;
        emit headReady();
    }
}

void StreamFetch::request(Part part)
{
    QNetworkRequest req(m_url);
    req.setRawHeader("User-Agent", "Mozilla/5.0");
    req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(part.from) + '-' + QByteArray::number(part.to));
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_net->get(req);
    m_replies << reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, part]() mutable {
        reply->deleteLater();
        m_replies.removeIf([reply](const QPointer<QNetworkReply> &r) { return r == reply; });
        if (m_done)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
        if (status == 200 && !body.isEmpty()) {   // no ranges: that's the whole file
            m_done = true;
            abortAll(std::exchange(m_replies, {}), this);
            if (m_shared && body.size() == m_total) {
                store({ 0, m_total - 1 }, body);
            } else {
                m_whole = body;
                if (m_shared) {   // a different size than announced: what's been read so far no longer fits
                    QMutexLocker lock(&m_shared->mutex);
                    m_shared->failed = true;
                    m_shared->grown.wakeAll();
                }
            }
            emit finished(true);
            return;
        }
        if (status == 206 && m_total < 0) {
            // "Content-Range: bytes 0-1048575/7340032" — the size, when the URL didn't say.
            static const QRegularExpression totalRe(QStringLiteral("/(\\d+)$"));
            const auto match = totalRe.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
            const qint64 total = match.hasMatch() ? match.captured(1).toLongLong() : 0;
            if (total <= 0 || total > kMaxSize) {
                fail();
                return;
            }
            allocate(total);
            if (!m_headOnly)
                plan(std::min(part.to + 1, total));
            part.to = std::min(part.to, total - 1);
        }
        const qint64 expected = m_total > 0 ? std::min(part.to, m_total - 1) - part.from + 1 : -1;
        if (status != 206 || body.size() != expected) {
            // A dropped or short part is asked for again; after a few failures the URL is played as before.
            if (++part.attempts < 3)
                request(part);
            else
                fail();
            return;
        }
        store(part, body);
        if (m_received >= m_total) {
            m_done = true;
            emit finished(true);
            return;
        }
        startMore();
    });
}

StreamDevice::StreamDevice(std::shared_ptr<StreamData> data, QObject *parent)
    : QIODevice(parent)
    , m_data(std::move(data))
{
}

qint64 StreamDevice::readData(char *out, qint64 max)
{
    QMutexLocker lock(&m_data->mutex);
    const qint64 at = pos();
    if (at >= m_data->total)
        return 0;
    // How much can be read at `at`: the gap-free start, or a part that came in further on.
    const auto available = [this, at]() -> qint64 {
        if (at < m_data->ready)
            return m_data->ready - at;
        auto it = m_data->arrived.upperBound(at);
        if (it == m_data->arrived.begin())
            return 0;
        --it;
        return at < it.value() ? it.value() - at : 0;
    };
    const QDeadlineTimer deadline(m_data->waitMs > 0 ? m_data->waitMs : kReadWaitMs);
    if (available() <= 0)
        m_data->wanted = at;
    while (available() <= 0 && !m_data->failed) {
        if (QThread::currentThread() == thread()) {
            // Read on the GUI thread (not the player's own): the bytes come through its event loop,
            // so it keeps running while this waits.
            lock.unlock();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
            lock.relock();
            if (deadline.hasExpired())
                return -1;
        } else if (!m_data->grown.wait(&m_data->mutex, deadline)) {
            return -1;
        }
    }
    const qint64 n = std::min(max, available());
    if (n <= 0)
        return -1;   // the download failed
    std::memcpy(out, m_data->bytes.constData() + at, size_t(n));
    return n;
}
