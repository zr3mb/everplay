#pragma once

#include "TrackModel.h"
#include "youtube/BrowserLogin.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>

class QJSEngine;
class QQmlEngine;

// SoundCloud: search, playback and (signed in) liked tracks and playlists — through the same
// api-v2.soundcloud.com the website uses. Its public client_id is read from the site's own scripts
// (and read again when SoundCloud changes it). Streams are SoundCloud's own HLS, not YouTube's.
//
// Signing in is optional: like YouTube, a separate browser window with its own profile, then the
// "oauth_token" cookie is read from it (BrowserLogin). The token is stored encrypted.
class SoundCloudService : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(SoundCloud)
    QML_SINGLETON

    Q_PROPERTY(bool connected READ connected NOTIFY accountChanged)
    Q_PROPERTY(QString accountName READ accountName NOTIFY accountChanged)
    Q_PROPERTY(QString accountPhoto READ accountPhoto NOTIFY accountChanged)
    Q_PROPERTY(bool loggingIn READ loggingIn NOTIFY loggingInChanged)
    Q_PROPERTY(QString loginStatus READ loginStatus NOTIFY loggingInChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString lastSync READ lastSync NOTIFY dataChanged)
    Q_PROPERTY(int likedCount READ likedCount NOTIFY dataChanged)
    Q_PROPERTY(int playlistCount READ playlistCount NOTIFY dataChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    // Playlists matching the latest search: [{ id, title, subtitle, cover, count }]
    Q_PROPERTY(QVariantList playlistResults READ playlistResults NOTIFY playlistResultsChanged)
    // Bumps when search results / an opened playlist arrive (appended to the track model, like YouTube's).
    Q_PROPERTY(int resultsRevision READ resultsRevision NOTIFY resultsChanged)

public:
    static SoundCloudService *instance();
    static SoundCloudService *create(QQmlEngine *, QJSEngine *);

    bool connected() const { return !m_token.isEmpty(); }
    QString accountName() const { return m_accountName; }
    QString accountPhoto() const { return m_accountPhoto; }
    bool loggingIn() const { return m_login.running() && !m_renewing; }
    QString loginStatus() const { return m_login.status(); }
    bool syncing() const { return m_syncing; }
    QString lastSync() const;
    int likedCount() const { return int(m_liked.size()); }
    int playlistCount() const { return int(m_playlists.size()); }
    bool searching() const { return m_searching; }
    QVariantList playlistResults() const { return m_playlistResults; }
    int resultsRevision() const { return m_resultsRevision; }

    Q_INVOKABLE void login();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void loginWithToken(const QString &token);   // the "oauth_token" cookie, pasted
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void search(const QString &query);
    // Playlists opened from search — kept in memory only, not added to the library.
    Q_INVOKABLE void ensureCollection(const QString &playlistId);
    Q_INVOKABLE bool collectionLoading(const QString &playlistId) const { return m_loadingCollections.contains(playlistId); }

    // For Library / Player
    QList<Track> tracks() const;
    const QStringList &likedIds() const { return m_liked; }
    QVariantList playlists() const { return m_playlists; }   // [{ id, title, cover, count }]
    QStringList playlistTrackIds(const QString &playlistId) const { return m_playlistTracks.value(playlistId); }
    void ensurePlaylist(const QString &playlistId);
    bool playlistLoading(const QString &playlistId) const { return m_loadingCollections.contains(playlistId); }
    const QStringList &searchIds() const { return m_search; }
    QStringList collectionTrackIds(const QString &playlistId) const { return m_collections.value(playlistId); }
    void setHistory(const QList<Track> &tracks);   // recently played (ListeningHistory), kept playable
    bool isLiked(const QString &trackId) const { return m_likedSet.contains(trackId); }
    void setTrackLiked(const QString &trackId, bool liked);
    void setTrackColors(const QString &trackId, const QColor &a, const QColor &b);
    // The stream of "sc:<id>" (StreamResolver hands these here).
    void resolveStream(const QString &trackId, std::function<void(const QUrl &url, const QString &error)> done);
    // The track's description on SoundCloud ("" when there is none or it can't be fetched): artists often
    // put the lyrics there.
    void fetchDescription(const QString &trackId, std::function<void(const QString &description)> done);

signals:
    void accountChanged();
    void loggingInChanged();
    void syncingChanged();
    void dataChanged();
    void searchingChanged();
    void playlistResultsChanged();
    void notice(const QString &text, const QString &icon);
    void tracksAdded(const QList<Track> &tracks);   // new transient tracks for the end of the model
    void resultsChanged();

private:
    using JsonCallback = std::function<void(const QJsonValue &json, const QString &error)>;

    // How a track plays: its stream variants (best first) and the authorization the API asks for.
    struct Media
    {
        QStringList transcodings;
        QString authorization;
        bool preview = false;   // only a 30 s preview (SoundCloud Go+ track)
    };

    explicit SoundCloudService(QObject *parent = nullptr);

    void withClientId(std::function<void(const QString &error)> next);
    // The token lasts a day: before it runs out the session is renewed invisibly in the sign-in profile.
    // Without a usable token, requests go without the account (search and playback keep working).
    void withToken(std::function<void()> next);
    bool tokenUsable() const;
    void renewSession();
    void get(const QString &pathOrUrl, const QUrlQuery &query, JsonCallback callback, int attempt = 0);
    void send(const QByteArray &verb, const QString &path, std::function<void(const QString &error)> done);
    void getPages(const QString &path, QUrlQuery query, int limit, QJsonArray collected,
                  std::function<void(const QJsonArray &items, const QString &error)> done);

    void useToken(const QString &token, bool fresh);
    void sync();
    void finishSync(const QString &error);
    void loadPlaylistTracks(const QString &playlistId, bool transient,
                            std::function<void(const QStringList &ids, const QString &error)> done);
    void fillTracks(QStringList missing, bool transient, std::function<void()> done);

    // Parsing the API's JSON; a track that can't be played comes back with an empty id.
    Track storeTrack(const QJsonObject &o, bool transient);
    QVariantMap playlistInfo(const QJsonObject &o) const;
    void publishResults(const QStringList &ids);

    void saveToken() const;
    void loadCache();
    void saveCache();               // soon, in the background (a burst of changes writes once)
    void writeCache(bool wait);     // now: built and written on a worker thread, or here
    void clearAccountData();

    QNetworkAccessManager m_net;
    BrowserLogin m_login;
    QString m_clientId;
    QList<std::function<void(const QString &)>> m_waitingForClientId;
    bool m_fetchingClientId = false;

    QString m_token;
    bool m_renewing = false;
    bool m_tokenSuspect = false;   // the API refused it — renew before using it again
    bool m_tokenDead = false;      // renewing failed: no account until signing in again
    qint64 m_lastRenew = 0;
    QList<std::function<void()>> m_waitingForToken;
    qint64 m_userId = 0;
    QString m_accountName;
    QString m_accountPhoto;

    QHash<QString, Track> m_tracks;            // "sc:<id>" -> track
    QHash<QString, Media> m_media;
    QStringList m_liked;
    QSet<QString> m_likedSet;
    QVariantList m_playlists;
    QHash<QString, QStringList> m_playlistTracks;
    QStringList m_search;
    QVariantList m_playlistResults;
    QStringList m_history;
    QHash<QString, QStringList> m_collections;   // playlistId -> track ids (opened from search)
    QSet<QString> m_loadingCollections;
    QSet<QString> m_refreshedPlaylists;          // the account's playlists loaded during this run
    quint64 m_searchSerial = 0;
    bool m_searching = false;
    int m_resultsRevision = 0;
    bool m_syncing = false;
    QDateTime m_lastSync;
    QTimer m_saveTimer;
};
