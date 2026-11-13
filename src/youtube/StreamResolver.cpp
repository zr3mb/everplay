#include "StreamResolver.h"

#include "AppPaths.h"
#include "online/OnlineService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

namespace {

// The player API client of the fast path: its audio URLs come without a signature cipher and need
// no proof-of-origin token (it's the client yt-dlp asks first).
const char *kClientUa = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
constexpr int kVisitorHours = 6;

const char *kReleaseBase = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";

// File name in the tools directory and the name of the standalone build in the GitHub release
// (Linux: "yt-dlp_linux" doesn't need Python, unlike "yt-dlp").
// Windows: the unpacked build (yt-dlp_win.zip) starts ~3× faster than the single-file
// yt-dlp.exe, which unpacks itself to a temporary directory on every launch.
QString engineFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("yt-dlp.exe");
#else
    return QStringLiteral("yt-dlp");
#endif
}

QString releaseAsset()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("yt-dlp_win.zip");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("yt-dlp_macos");
#else
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (arch == QLatin1String("arm64"))
        return QStringLiteral("yt-dlp_linux_aarch64");
    return QStringLiteral("yt-dlp_linux");
#endif
}

void hideConsole(QProcess *process)
{
#ifdef Q_OS_WIN
    // Everplay is a GUI app — without this every yt-dlp call would flash a console.
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#else
    Q_UNUSED(process);
#endif
}

} // namespace

StreamResolver::StreamResolver(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_ANDROID
    m_remote = true;
#else
    m_remote = qEnvironmentVariableIsSet("EVERPLAY_SERVER_STREAMS");
#endif
}

QString StreamResolver::toolsDir() const
{
    const QString dir = AppPaths::dataDir() + QStringLiteral("/tools");
    QDir().mkpath(dir);
    return dir;
}

void StreamResolver::detect()
{
    if (m_remote) {
        m_version = tr("Everplay server");
        emit engineChanged();
        return;
    }
    const QStringList candidates = {
#ifdef Q_OS_WIN
        toolsDir() + QStringLiteral("/yt-dlp/") + engineFileName(),
#endif
        toolsDir() + QChar('/') + engineFileName(),
        QCoreApplication::applicationDirPath() + QChar('/') + engineFileName(),
        QStandardPaths::findExecutable(QStringLiteral("yt-dlp")),
    };
    m_enginePath.clear();
    for (const QString &c : candidates) {
        if (!c.isEmpty() && QFile::exists(c)) {
            m_enginePath = c;
            break;
        }
    }
    m_version.clear();
    emit engineChanged();
    if (ready())
        readVersion();
    if (m_visitor.isEmpty())
        fetchVisitor();   // ready before the first track is asked for
#ifdef Q_OS_WIN
    // An older install with the single-file yt-dlp.exe — we quietly switch to the faster unpacked build.
    const QString legacy = toolsDir() + QChar('/') + engineFileName();
    if (m_enginePath != legacy)
        QFile::remove(legacy);   // it may have stayed if it was in use during the upgrade
    else if (!m_downloading) {
        m_quiet = true;
        QTimer::singleShot(0, this, &StreamResolver::download);
    }
#endif
}

void StreamResolver::readVersion()
{
    auto *process = new QProcess(this);
    hideConsole(process);
    connect(process, &QProcess::finished, this, [this, process] {
        m_version = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        process->deleteLater();
        emit engineChanged();
    });
    process->start(m_enginePath, { QStringLiteral("--version") });
}

QStringList StreamResolver::baseArguments() const
{
    QStringList args = {
        QStringLiteral("--no-warnings"), QStringLiteral("--no-playlist"), QStringLiteral("--no-progress"),
        QStringLiteral("-f"), QStringLiteral("bestaudio[ext=m4a]/bestaudio/best"),
        QStringLiteral("--print"), QStringLiteral("urls"),
    };
    // yt-dlp needs a JavaScript runtime for full YouTube support (it looks for Deno by default).
    // The Windows installer ships deno.exe next to Everplay.exe — it takes precedence.
    const QString bundled = QStandardPaths::findExecutable(QStringLiteral("deno"),
                                                           { QCoreApplication::applicationDirPath() });
    const QString deno = QStandardPaths::findExecutable(QStringLiteral("deno"));
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (!bundled.isEmpty())
        args << QStringLiteral("--js-runtimes") << QStringLiteral("deno:") + bundled;
    else if (deno.isEmpty() && !node.isEmpty())
        args << QStringLiteral("--js-runtimes") << QStringLiteral("node:") + node;
    return args;
}

QUrl StreamResolver::cachedUrl(const QString &videoId) const
{
    const auto it = m_cache.constFind(videoId);
    if (it == m_cache.cend() || it->expires < QDateTime::currentDateTimeUtc().addSecs(120))
        return {};
    return it->url;
}

void StreamResolver::resolve(const QString &videoId)
{
    if (const QUrl cached = cachedUrl(videoId); cached.isValid()) {
        QTimer::singleShot(0, this, [this, videoId, cached] { emit resolved(videoId, cached); });
        return;
    }
    if (m_running.contains(videoId) || m_directRunning.contains(videoId))
        return;   // already running — the result will come as a signal
    if (isExternal(videoId)) {
        runExternal(videoId);
        return;
    }
    if (directUsable()) {
        resolveDirect(videoId);
        return;
    }
    fallback(videoId);
}

void StreamResolver::fallback(const QString &videoId)
{
    if (m_running.contains(videoId))
        return;
    if (!ready()) {
        emit failed(videoId, tr("The yt-dlp engine is missing"));
        return;
    }
    // Anonymous first: with account cookies yt-dlp switches to web clients that need the JavaScript
    // challenge solved by Deno — ~5 s per track instead of ~2 s. Cookies are only the fallback
    // (age-restricted tracks, "sign in to confirm" checks).
    run(videoId, false);
}

bool StreamResolver::directUsable() const
{
    if (qEnvironmentVariable("EVERPLAY_STREAM_DIRECT") == QLatin1String("0"))
        return false;
    return !m_directPausedUntil.isValid() || m_directPausedUntil < QDateTime::currentDateTimeUtc();
}

void StreamResolver::resolveDirect(const QString &videoId)
{
    m_directRunning.insert(videoId);
    const bool stale = !m_visitorAt.isValid()
                       || m_visitorAt.secsTo(QDateTime::currentDateTimeUtc()) > kVisitorHours * 3600;
    if (m_visitor.isEmpty() || stale) {
        if (!m_waitingForVisitor.contains(videoId))
            m_waitingForVisitor << videoId;
        fetchVisitor();
        return;
    }
    askPlayer(videoId);
}

void StreamResolver::fetchVisitor()
{
    if (m_visitorLoading)
        return;
    m_visitorLoading = true;
    // A tiny service-worker data file carries a fresh anonymous visitor id (the full page would too, ~1 MB).
    QNetworkRequest request(QUrl(QStringLiteral("https://www.youtube.com/sw.js_data")));
    request.setRawHeader("User-Agent", "Mozilla/5.0");
    request.setTransferTimeout(5000);
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_visitorLoading = false;
        static const QRegularExpression idRe(QStringLiteral("\"(Cg[A-Za-z0-9_%=-]{20,})\""));
        const auto match = idRe.match(QString::fromLatin1(reply->readAll()));
        if (reply->error() == QNetworkReply::NoError && match.hasMatch()) {
            m_visitor = match.captured(1).toLatin1();
            m_visitorAt = QDateTime::currentDateTimeUtc();
        }
        const QStringList waiting = std::exchange(m_waitingForVisitor, {});
        for (const QString &id : waiting) {
            if (m_visitor.isEmpty())
                directFailed(id, QStringLiteral("no visitor id"));
            else
                askPlayer(id);
        }
    });
}

void StreamResolver::askPlayer(const QString &videoId)
{
    QNetworkRequest request(QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/player?prettyPrint=false")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", kClientUa);
    request.setRawHeader("X-Youtube-Client-Name", "101");
    request.setRawHeader("X-Youtube-Client-Version", "1.02");
    request.setRawHeader("Origin", "https://www.youtube.com");
    request.setRawHeader("X-Goog-Visitor-Id", m_visitor);
    request.setTransferTimeout(6000);
    const QJsonObject client{
        { "clientName", "VISIONOS" }, { "clientVersion", "1.02" }, { "deviceMake", "Apple" },
        { "deviceModel", "RealityDevice17,1" }, { "userAgent", kClientUa }, { "osName", "visionOS" },
        { "osVersion", "26.5.23O471" }, { "hl", "en" },
    };
    const QJsonObject body{
        { "context", QJsonObject{ { "client", client } } },
        { "videoId", videoId }, { "contentCheckOk", true }, { "racyCheckOk", true },
    };
    QElapsedTimer started;
    started.start();
    QNetworkReply *reply = m_net.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, videoId, started] {
        reply->deleteLater();
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QString status = root.value("playabilityStatus").toObject().value("status").toString();
        if (reply->error() != QNetworkReply::NoError || status != QLatin1String("OK")) {
            if (status == QLatin1String("LOGIN_REQUIRED"))
                m_visitor.clear();   // a bot check: the next call starts with a new visitor id
            directFailed(videoId, status.isEmpty() ? reply->errorString() : status, !status.isEmpty());
            return;
        }
        // AAC audio (itag 140, what yt-dlp is asked for too) in the original language, without the
        // compressed-dynamics variant; otherwise the best other MP4 audio.
        QString best;
        qint64 bestScore = -1;
        const QJsonArray formats = root.value("streamingData").toObject().value("adaptiveFormats").toArray();
        for (const QJsonValue &v : formats) {
            const QJsonObject f = v.toObject();
            const QString url = f.value("url").toString();
            if (url.isEmpty() || !f.value("mimeType").toString().startsWith(QLatin1String("audio/mp4"))
                || f.value("isDrc").toBool()
                || !f.value("audioTrack").toObject().value("audioIsDefault").toBool(true))
                continue;
            const qint64 score = f.value("itag").toInt() == 140 ? (qint64(1) << 40) : f.value("bitrate").toInteger();
            if (score > bestScore) {
                bestScore = score;
                best = url;
            }
        }
        if (best.isEmpty()) {
            directFailed(videoId, QStringLiteral("no plain audio URL"));
            return;
        }
        m_directRunning.remove(videoId);
        m_directFailures = 0;
        if (qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
            qInfo("stream: %s resolved directly in %lld ms", qPrintable(videoId), started.elapsed());
        const QUrl u(best);
        const qint64 expire = QUrlQuery(u).queryItemValue(QStringLiteral("expire")).toLongLong();
        const QDateTime expires = expire > 0 ? QDateTime::fromSecsSinceEpoch(expire, QTimeZone::UTC)
                                             : QDateTime::currentDateTimeUtc().addSecs(3600);
        m_cache.insert(videoId, { u, expires });
        emit resolved(videoId, u);
    });
}

void StreamResolver::directFailed(const QString &videoId, const QString &why, bool trackOnly)
{
    m_directRunning.remove(videoId);
    if (qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
        qInfo("stream: %s direct lookup failed (%s), using yt-dlp", qPrintable(videoId), qPrintable(why));
    // A few failures in a row (YouTube changed something): yt-dlp alone for a while. A refusal of
    // one track (a music video that's unplayable here, an age check) says nothing about that.
    if (!trackOnly && ++m_directFailures >= 3) {
        m_directFailures = 0;
        m_directPausedUntil = QDateTime::currentDateTimeUtc().addSecs(1800);
    }
    fallback(videoId);
}

void StreamResolver::resolveAgain(const QString &videoId)
{
    m_cache.remove(videoId);
    if (m_running.contains(videoId))
        return;
    if (isExternal(videoId)) {
        runExternal(videoId);
        return;
    }
    if (!ready()) {
        emit failed(videoId, tr("The yt-dlp engine is missing"));
        return;
    }
    run(videoId, m_cookies.isLoggedIn());
}

void StreamResolver::setExternal(const QString &prefix, External resolve)
{
    for (auto &entry : m_external) {
        if (entry.first == prefix) {
            entry.second = std::move(resolve);
            return;
        }
    }
    m_external.append({ prefix, std::move(resolve) });
}

const StreamResolver::External *StreamResolver::externalFor(const QString &id) const
{
    for (const auto &entry : m_external)
        if (id.startsWith(entry.first))
            return &entry.second;
    return nullptr;
}

void StreamResolver::runExternal(const QString &id)
{
    if (m_externalRunning.contains(id))
        return;
    const External *external = externalFor(id);
    if (!external)
        return;
    m_externalRunning.insert(id);
    (*external)(id, [this, id](const QUrl &url, const QString &error) {
        m_externalRunning.remove(id);
        if (!url.isValid()) {
            emit failed(id, error);
            return;
        }
        // Signed stream URLs say when they stop working ("expires" / "expire", seconds).
        const QUrlQuery query(url);
        qint64 expire = query.queryItemValue(QStringLiteral("expires")).toLongLong();
        if (expire <= 0)
            expire = query.queryItemValue(QStringLiteral("expire")).toLongLong();
        const QDateTime expires = expire > 0 ? QDateTime::fromSecsSinceEpoch(expire, QTimeZone::UTC)
                                             : QDateTime::currentDateTimeUtc().addSecs(1800);
        m_cache.insert(id, { url, expires });
        emit resolved(id, url);
    });
}

void StreamResolver::run(const QString &videoId, bool withCookies)
{
    if (m_remote) {
        m_running.insert(videoId, nullptr);
        OnlineService::instance()->resolveStream(videoId, [this, videoId](const QUrl &url, const QString &error) {
            m_running.remove(videoId);
            if (!url.isValid()) {
                emit failed(videoId, error);
                return;
            }
            const qint64 expire = QUrlQuery(url).queryItemValue(QStringLiteral("e")).toLongLong();
            const QDateTime expires = expire > 0 ? QDateTime::fromSecsSinceEpoch(expire, QTimeZone::UTC)
                                                 : QDateTime::currentDateTimeUtc().addSecs(1800);
            m_cache.insert(videoId, { url, expires });
            emit resolved(videoId, url);
        });
        return;
    }
    QStringList args = baseArguments();

    // Account cookies (fallback: age-restricted tracks, bot checks).
    // The file exists only while the process runs.
    QString cookieFile;
    if (withCookies) {
        cookieFile = toolsDir() + QStringLiteral("/cookies-%1.txt").arg(QUuid::createUuid().toString(QUuid::Id128));
        QFile f(cookieFile);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(m_cookies.toNetscape().toUtf8());
            f.close();
            args << QStringLiteral("--cookies") << cookieFile;
        }
    }
    args << QStringLiteral("https://music.youtube.com/watch?v=") + videoId;

    auto *process = new QProcess(this);
    hideConsole(process);
    m_running.insert(videoId, process);
    QElapsedTimer started;
    started.start();
    connect(process, &QProcess::finished, this, [this, process, videoId, cookieFile, started](int code) {
        m_running.remove(videoId);
        process->deleteLater();
        if (!cookieFile.isEmpty())
            QFile::remove(cookieFile);
        // Diagnostics: EVERPLAY_STREAM_DEBUG=1 prints how long each yt-dlp call took.
        if (qEnvironmentVariableIsSet("EVERPLAY_STREAM_DEBUG"))
            qInfo("stream: %s resolved in %lld ms (%s, exit %d)", qPrintable(videoId), started.elapsed(),
                  cookieFile.isEmpty() ? "anonymous" : "with cookies", code);

        const QStringList out = QString::fromUtf8(process->readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        const QString url = out.isEmpty() ? QString() : out.first().trimmed();
        if (code == 0 && url.startsWith(QLatin1String("http"))) {
            const QUrl u(url);
            const qint64 expire = QUrlQuery(u).queryItemValue(QStringLiteral("expire")).toLongLong();
            const QDateTime expires = expire > 0 ? QDateTime::fromSecsSinceEpoch(expire, QTimeZone::UTC)
                                                 : QDateTime::currentDateTimeUtc().addSecs(3600);
            m_cache.insert(videoId, { u, expires });
            emit resolved(videoId, u);
            return;
        }
        QStringList err = QString::fromUtf8(process->readAllStandardError()).split('\n', Qt::SkipEmptyParts);
        QString message = err.isEmpty() ? tr("yt-dlp ended with an error") : err.last().trimmed();
        message.remove(QStringLiteral("ERROR: "));
        // The anonymous attempt failed (age restriction, bot check…) — try once more with the account.
        if (cookieFile.isEmpty() && m_cookies.isLoggedIn()) {
            run(videoId, true);
            return;
        }
        emit failed(videoId, message);
    });
    process->start(m_enginePath, args);
}

void StreamResolver::download()
{
    if (m_remote || m_downloading)
        return;
    m_downloading = true;
    m_progress = 0;
    emit engineChanged();

    // The checksum list first, then the program itself — we compare SHA-256 before saving.
    QNetworkReply *sumsReply = m_net.get(QNetworkRequest(QUrl(QString::fromLatin1(kReleaseBase) + "SHA2-256SUMS")));
    connect(sumsReply, &QNetworkReply::finished, this, [this, sumsReply] {
        sumsReply->deleteLater();
        if (sumsReply->error() != QNetworkReply::NoError) {
            m_downloading = false;
            emit engineChanged();
            report(tr("Couldn't download yt-dlp: %1").arg(sumsReply->errorString()), QStringLiteral("close"));
            return;
        }
        const QByteArray sums = sumsReply->readAll();
        QNetworkReply *binReply = m_net.get(QNetworkRequest(QUrl(QString::fromLatin1(kReleaseBase) + releaseAsset())));
        connect(binReply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
            m_progress = total > 0 ? qreal(received) / total : 0;
            emit engineChanged();
        });
        connect(binReply, &QNetworkReply::finished, this, [this, binReply, sums] {
            binReply->deleteLater();
            if (binReply->error() != QNetworkReply::NoError) {
                m_downloading = false;
                emit engineChanged();
                report(tr("Couldn't download yt-dlp: %1").arg(binReply->errorString()), QStringLiteral("close"));
                return;
            }
            finishDownload(binReply->readAll(), sums);
        });
    });
}

void StreamResolver::report(const QString &text, const QString &icon)
{
    // A quiet background upgrade doesn't litter notifications — the old version keeps working.
    if (!m_quiet)
        emit notice(text, icon);
    m_quiet = false;
}

void StreamResolver::finishDownload(const QByteArray &binary, const QByteArray &sums)
{
    m_downloading = false;
    const QByteArray hash = QCryptographicHash::hash(binary, QCryptographicHash::Sha256).toHex();
    bool verified = false;
    for (const QByteArray &line : sums.split('\n')) {
        if (line.trimmed().endsWith(" " + releaseAsset().toLatin1()) && line.startsWith(hash)) {
            verified = true;
            break;
        }
    }
    if (!verified) {
        emit engineChanged();
        report(tr("The downloaded yt-dlp doesn't match the checksum — aborted"), QStringLiteral("close"));
        return;
    }
#ifdef Q_OS_WIN
    // The archive is unpacked by the system tar.exe (Windows 10+) into a temporary directory,
    // which replaces the previous version on success.
    const QString zip = toolsDir() + QStringLiteral("/yt-dlp_win.zip");
    const QString staging = toolsDir() + QStringLiteral("/yt-dlp.new");
    QSaveFile out(zip);
    if (!out.open(QIODevice::WriteOnly) || out.write(binary) != binary.size() || !out.commit()) {
        emit engineChanged();
        report(tr("Couldn't save yt-dlp"), QStringLiteral("close"));
        return;
    }
    QDir(staging).removeRecursively();
    QDir().mkpath(staging);
    auto *tar = new QProcess(this);
    hideConsole(tar);
    connect(tar, &QProcess::finished, this, [this, tar, zip, staging](int code) {
        tar->deleteLater();
        QFile::remove(zip);
        const QString final = toolsDir() + QStringLiteral("/yt-dlp");
        if (code != 0 || !QFile::exists(staging + QChar('/') + engineFileName())) {
            QDir(staging).removeRecursively();
            emit engineChanged();
            report(tr("Couldn't unpack yt-dlp"), QStringLiteral("close"));
            return;
        }
        QDir(final).removeRecursively();
        if (!QDir().rename(staging, final)) {
            emit engineChanged();
            report(tr("Couldn't save yt-dlp"), QStringLiteral("close"));
            return;
        }
        QFile::remove(toolsDir() + QChar('/') + engineFileName());   // the old single-file one
        m_cache.clear();
        detect();
        report(tr("The YouTube playback engine is ready"), QStringLiteral("check"));
    });
    tar->start(QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot")) + QStringLiteral("/System32/tar.exe"),
               { QStringLiteral("-xf"), QDir::toNativeSeparators(zip), QStringLiteral("-C"), QDir::toNativeSeparators(staging) });
#else
    const QString target = toolsDir() + QChar('/') + engineFileName();
    QSaveFile out(target);
    if (out.open(QIODevice::WriteOnly) && out.write(binary) == binary.size() && out.commit()) {
        QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                          | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                          | QFileDevice::ReadOther | QFileDevice::ExeOther);
        m_cache.clear();
        detect();
        report(tr("The YouTube playback engine is ready"), QStringLiteral("check"));
    } else {
        emit engineChanged();
        report(tr("Couldn't save yt-dlp"), QStringLiteral("close"));
    }
#endif
}
