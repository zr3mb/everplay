#pragma once

#include "TrackModel.h"

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>

class QFile;
class QJSEngine;
class QNetworkReply;
class QQmlEngine;

// FLAC files from open sources, searched next to the streaming services once the user turns it on
// (Sources → Open FLAC sources). Nothing here needs an account or a key:
//
// - the Internet Archive: the Live Music Archive (concert recordings the bands allow to be shared),
//   netlabel releases and community uploads that carry a Creative Commons or public-domain licence.
//   Other community uploads are left out — anyone can upload there, licensed or not;
// - Wikimedia Commons: freely licensed and public-domain recordings (a lot of classical music).
//
// Tracks play straight from the file ("ia:<item>/<file>", "wc:<file name>") and can be saved into the
// music folder, where the library picks them up as local files.
class OpenMusicService : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(OpenMusic)
    QML_SINGLETON

    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool archiveEnabled READ archiveEnabled WRITE setArchiveEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool commonsEnabled READ commonsEnabled WRITE setCommonsEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    // Albums and concerts matching the latest search: [{ id, title, subtitle, cover, source }]
    Q_PROPERTY(QVariantList albumResults READ albumResults NOTIFY albumResultsChanged)
    // Bumps when search results / an opened album arrive (appended to the track model, like YouTube's).
    Q_PROPERTY(int resultsRevision READ resultsRevision NOTIFY resultsChanged)
    // Where saved files go, and the saves still running: [{ id, title, progress }]
    Q_PROPERTY(QString downloadFolder READ downloadFolder WRITE setDownloadFolder NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList downloads READ downloads NOTIFY downloadsChanged)

public:
    static OpenMusicService *instance();
    static OpenMusicService *create(QQmlEngine *, QJSEngine *);
    // "archive" (Internet Archive) or "commons" (Wikimedia Commons)
    static bool isOpenSource(const QString &source);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    bool archiveEnabled() const { return m_archive; }
    void setArchiveEnabled(bool on);
    bool commonsEnabled() const { return m_commons; }
    void setCommonsEnabled(bool on);
    bool searching() const { return m_pending > 0; }
    QVariantList albumResults() const { return m_albumResults; }
    int resultsRevision() const { return m_resultsRevision; }
    QString downloadFolder() const;
    void setDownloadFolder(const QString &folder);
    QVariantList downloads() const;

    Q_INVOKABLE void search(const QString &query);
    // An album / concert on the Internet Archive (its identifier): loaded once per run, kept in memory.
    Q_INVOKABLE void ensureCollection(const QString &itemId);
    Q_INVOKABLE bool collectionLoading(const QString &itemId) const { return m_loadingItems.contains(itemId); }
    // { title, creator, date, license, licenseUrl, page, bytes, count } — {} before it's loaded.
    Q_INVOKABLE QVariantMap collectionInfo(const QString &itemId) const { return m_items.value(itemId); }
    // The track's page at its source and the licence it's shared under ("" when unknown).
    Q_INVOKABLE QString pageUrl(const QString &trackId) const;
    Q_INVOKABLE QString license(const QString &trackId) const { return m_licenses.value(trackId); }
    // Saving the FLAC file into the download folder (and so into the library).
    Q_INVOKABLE void download(const QString &trackId);
    Q_INVOKABLE void downloadCollection(const QString &itemId);
    Q_INVOKABLE bool downloading(const QString &trackId) const { return m_jobs.contains(trackId); }
    Q_INVOKABLE void cancelDownloads();

    // For Library / Player
    QList<Track> tracks() const;   // liked, search results, history, opened albums
    const QStringList &likedIds() const { return m_liked; }
    const QStringList &searchIds() const { return m_search; }
    QStringList collectionTrackIds(const QString &itemId) const { return m_collections.value(itemId); }
    void setHistory(const QList<Track> &tracks);
    void setTrackLiked(const Track &track, bool liked);
    void setTrackColors(const QString &trackId, const QColor &a, const QColor &b);
    // The file's URL — known from the search already, so this answers at once.
    void resolveStream(const QString &trackId, std::function<void(const QUrl &url, const QString &error)> done);

signals:
    void settingsChanged();
    void searchingChanged();
    void albumResultsChanged();
    void resultsChanged();
    void dataChanged();                             // the liked tracks changed
    void tracksAdded(const QList<Track> &tracks);   // new transient tracks for the end of the model
    void downloadsChanged();
    void fileSaved(const QString &folder);          // a file landed in this folder (rescan it)
    void notice(const QString &text, const QString &icon);

private:
    struct Job
    {
        QString trackId;
        QString title;
        QUrl url;
        QString target;
        QJsonObject tags;     // filled into the file where its own tags are missing
        qint64 received = 0;
        qint64 total = 0;
        QPointer<QNetworkReply> reply;
        QFile *file = nullptr;
    };

    explicit OpenMusicService(QObject *parent = nullptr);

    using JsonCallback = std::function<void(const QJsonObject &json, const QString &error)>;
    void getJson(const QUrl &url, JsonCallback callback);

    void searchArchive(const QString &query, quint64 serial);
    void searchArchiveItems(const QString &fields, int rows,
                            std::function<void(const QJsonArray &docs, const QString &error)> done);
    void searchCommons(const QString &query, quint64 serial);
    void loadItem(const QString &itemId, std::function<void(const QStringList &ids, const QString &error)> done);
    QStringList storeItem(const QString &itemId, const QJsonObject &json);
    Track storeTrack(Track t);
    void finishSearch(quint64 serial);
    void publish(const QStringList &ids);

    void startNextDownload();
    void finishDownload(const QString &trackId, const QString &error);
    QString targetPath(const Track &t) const;

    void loadState();
    void saveState() const;

    QNetworkAccessManager m_net;
    bool m_enabled = false;
    bool m_archive = true;
    bool m_commons = true;
    QString m_folder;

    QHash<QString, Track> m_tracks;                // "ia:…" / "wc:…" -> track
    QHash<QString, QString> m_licenses;            // track id -> licence name
    QHash<QString, QString> m_pages;               // track id -> its page at the source
    QStringList m_liked;
    QStringList m_search;
    QStringList m_history;
    QHash<QString, QStringList> m_collections;     // item id -> track ids
    QHash<QString, QVariantMap> m_items;           // item id -> collectionInfo()
    QSet<QString> m_loadingItems;
    QSet<QString> m_opened;                        // items opened as a page (all their tracks published)
    QHash<QString, QList<std::function<void(const QStringList &, const QString &)>>> m_itemWaiters;
    QVariantList m_albumResults;
    QStringList m_archiveFound;                    // the latest search's tracks, per source
    QStringList m_commonsFound;
    QString m_query;
    quint64 m_serial = 0;
    int m_pending = 0;
    int m_resultsRevision = 0;

    QHash<QString, Job> m_jobs;                    // saves, queued and running
    QStringList m_queue;
    QString m_running;
    int m_batchSaved = 0;                          // since the queue was last empty
    int m_batchFailed = 0;
    QString m_lastSaved;
};
