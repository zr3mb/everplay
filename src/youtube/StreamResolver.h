#pragma once

#include "YtCookies.h"

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QUrl>

#include <functional>

class QProcess;

// Turns a videoId into a direct audio stream URL. The fast path asks YouTube's player API directly
// as a client whose audio URLs need no deciphering (~0.3 s); yt-dlp (~2 s, it handles signature
// deciphering and changes on YouTube's side and is updated separately) is the fallback whenever
// that answer isn't usable.
class StreamResolver : public QObject
{
    Q_OBJECT

public:
    explicit StreamResolver(QObject *parent = nullptr);

    bool ready() const { return m_remote || !m_enginePath.isEmpty(); }
    // Tracks are resolved by the Everplay server instead of a local yt-dlp — always on Android,
    // where apps can't run downloaded programs; EVERPLAY_SERVER_STREAMS=1 turns it on elsewhere.
    bool remote() const { return m_remote; }
    QString version() const { return m_version; }
    bool downloading() const { return m_downloading; }
    qreal downloadProgress() const { return m_progress; }

    void detect();
    void download();          // official release from GitHub, verified with SHA-256
    void setCookies(const YtCookies &cookies) { m_cookies = cookies; }
    // Ids with these prefixes are resolved by another service (SoundCloud's own API, the open FLAC
    // sources' files) instead of yt-dlp; they share the cache and the resolved / failed signals.
    using Done = std::function<void(const QUrl &url, const QString &error)>;
    using External = std::function<void(const QString &id, Done done)>;
    void setExternal(const QString &prefix, External resolve);

    void resolve(const QString &videoId);
    // A fresh URL through the account (another YouTube client) — when the anonymous one can't be opened.
    void resolveAgain(const QString &videoId);
    void prefetch(const QString &videoId) { if ((ready() || isExternal(videoId)) && !cachedUrl(videoId).isValid()) resolve(videoId); }
    bool isExternal(const QString &id) const { return externalFor(id) != nullptr; }
    QUrl cachedUrl(const QString &videoId) const;
    void forget(const QString &videoId) { m_cache.remove(videoId); }   // e.g. the URL expired mid-playback

signals:
    void engineChanged();
    void resolved(const QString &videoId, const QUrl &url);
    void failed(const QString &videoId, const QString &error);
    void notice(const QString &text, const QString &icon);

private:
    struct Entry { QUrl url; QDateTime expires; };

    QString toolsDir() const;
    QStringList baseArguments() const;
    void run(const QString &videoId, bool withCookies);
    bool directUsable() const;
    void resolveDirect(const QString &videoId);
    void askPlayer(const QString &videoId);
    // trackOnly: YouTube refused this one track (unplayable, sign-in needed), the fast path itself works
    void directFailed(const QString &videoId, const QString &why, bool trackOnly = false);
    void fetchVisitor();
    void fallback(const QString &videoId);
    void runExternal(const QString &id);
    const External *externalFor(const QString &id) const;
    void readVersion();
    void finishDownload(const QByteArray &binary, const QByteArray &sums);
    void report(const QString &text, const QString &icon);

    QString m_enginePath;
    QString m_version;
    YtCookies m_cookies;
    QHash<QString, Entry> m_cache;
    QHash<QString, QProcess *> m_running;
    QList<QPair<QString, External>> m_external;   // prefix -> resolver
    QSet<QString> m_externalRunning;
    // Fast path: an anonymous visitor id (YouTube asks bot checks without one), shared by all calls
    QByteArray m_visitor;
    QDateTime m_visitorAt;
    bool m_visitorLoading = false;
    QStringList m_waitingForVisitor;
    QSet<QString> m_directRunning;
    int m_directFailures = 0;           // in a row; several pause the fast path for a while
    QDateTime m_directPausedUntil;
    QNetworkAccessManager m_net;
    bool m_remote = false;
    bool m_downloading = false;
    bool m_quiet = false;              // a quiet upgrade is in progress (no notifications)
    qreal m_progress = 0;
};
