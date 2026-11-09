#pragma once

#include "BrowserLogin.h"
#include "StreamResolver.h"
#include "TrackModel.h"
#include "YtMusicApi.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// YouTube Music: signing in (browser + cookies), library sync
// (liked, playlists), search and the stream engine.
class YouTubeService : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(YouTube)
    QML_SINGLETON

    Q_PROPERTY(bool connected READ connected NOTIFY accountChanged)
    Q_PROPERTY(QString accountName READ accountName NOTIFY accountChanged)
    Q_PROPERTY(QString accountHandle READ accountHandle NOTIFY accountChanged)
    Q_PROPERTY(QString accountPhoto READ accountPhoto NOTIFY accountChanged)
    // The channels the session can use ({ name, handle, photo, email, selected }) — more than one
    // when the browser has several Google accounts or the account has brand channels.
    Q_PROPERTY(QVariantList identities READ identities NOTIFY accountChanged)
    Q_PROPERTY(bool loggingIn READ loggingIn NOTIFY loggingInChanged)
    Q_PROPERTY(QString loginStatus READ loginStatus NOTIFY loggingInChanged)
    Q_PROPERTY(QString browserName READ browserName CONSTANT)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString lastSync READ lastSync NOTIFY dataChanged)
    Q_PROPERTY(int likedCount READ likedCount NOTIFY dataChanged)
    Q_PROPERTY(int playlistCount READ playlistCount NOTIFY dataChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    // Public playlists matching the latest search: YouTube Music's own first, then users'.
    Q_PROPERTY(QVariantList playlistResults READ playlistResults NOTIFY playlistResultsChanged)
    Q_PROPERTY(bool engineReady READ engineReady NOTIFY engineChanged)
    Q_PROPERTY(QString engineVersion READ engineVersion NOTIFY engineChanged)
    Q_PROPERTY(bool engineDownloading READ engineDownloading NOTIFY engineChanged)
    Q_PROPERTY(qreal engineProgress READ engineProgress NOTIFY engineChanged)
    Q_PROPERTY(int artistsRevision READ artistsRevision NOTIFY artistsChanged)   // bumps when an artist page loads
    // Bumps when search results / an artist's songs / an opened collection arrive. Those tracks are only
    // appended to the track model (no full rebuild), so lists of them depend on this instead of the model.
    Q_PROPERTY(int resultsRevision READ resultsRevision NOTIFY resultsChanged)

public:
    static YouTubeService *instance();
    static YouTubeService *create(QQmlEngine *, QJSEngine *);

    bool connected() const { return m_cookies.isLoggedIn(); }
    QString accountName() const { return m_account.name; }
    QString accountHandle() const { return m_account.handle; }
    QString accountPhoto() const { return m_account.photo; }
    QVariantList identities() const;
    bool loggingIn() const { return m_login.running(); }
    QString loginStatus() const { return m_login.status(); }
    QString browserName() const { return m_browserName; }
    bool syncing() const { return m_pendingSync > 0; }
    QString lastSync() const;
    int likedCount() const { return int(m_liked.size()); }
    int playlistCount() const { return int(m_playlists.size()); }
    bool searching() const { return m_searching; }
    QVariantList playlistResults() const { return m_playlistResults; }
    bool engineReady() const { return m_resolver.ready(); }
    QString engineVersion() const { return m_resolver.version(); }
    bool engineDownloading() const { return m_resolver.downloading(); }
    qreal engineProgress() const { return m_resolver.downloadProgress(); }

    Q_INVOKABLE void login();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void loginWithCookies(const QString &cookieHeader);
    Q_INVOKABLE void logout();
    Q_INVOKABLE void selectIdentity(int index);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void downloadEngine();
    Q_INVOKABLE void search(const QString &query);

    // Artist profiles (YouTube Music). `artist()` returns { browseId, name, description, subscribers, photo,
    // songsPlaylistId, sections, loading, error }; songs are in Library.artistSongRows(browseId).
    Q_INVOKABLE void loadArtist(const QString &browseId);
    Q_INVOKABLE QVariantMap artist(const QString &browseId) const;
    int artistsRevision() const { return m_artistsRevision; }
    int resultsRevision() const { return m_resultsRevision; }
    // Finds the channel of an artist known only by name (local files); the result comes as artistFound().
    Q_INVOKABLE void findArtist(const QString &name);
    Q_INVOKABLE QString knownArtistId(const QString &name) const { return m_artistIdByName.value(name.toLower()); }
    // Albums / playlists opened from an artist page — kept in memory only, not added to the library.
    Q_INVOKABLE void ensureCollection(const QString &playlistId);
    Q_INVOKABLE bool collectionLoading(const QString &playlistId) const { return m_loadingCollections.contains(playlistId); }
    QStringList collectionTrackIds(const QString &playlistId) const { return m_collections.value(playlistId); }
    QStringList artistSongIds(const QString &browseId) const { return m_artistSongs.value(browseId); }

    // For Library / Player
    StreamResolver *resolver() { return &m_resolver; }
    YtMusicApi *api() { return &m_api; }
    QList<Track> tracks() const;
    const QStringList &likedIds() const { return m_liked; }
    QVariantList playlists() const { return m_playlists; }
    QStringList playlistTrackIds(const QString &playlistId) const { return m_playlistTracks.value(playlistId); }

    // Editing the account's playlists (Playlists does the UI side). The local copy updates on success.
    using ErrorCallback = std::function<void(const QString &error)>;
    void createPlaylist(const QString &title, const QString &description, const QString &privacy,
                        const QList<Track> &tracks, std::function<void(const QString &playlistId, const QString &error)> done);
    void addToPlaylist(const QString &playlistId, const QList<Track> &tracks, ErrorCallback done);
    void removeFromPlaylist(const QString &playlistId, const QString &videoId, ErrorCallback done);
    void renamePlaylist(const QString &playlistId, const QString &title, ErrorCallback done);
    void setPlaylistPrivacy(const QString &playlistId, const QString &privacy, ErrorCallback done);
    void deletePlaylist(const QString &playlistId, ErrorCallback done);
    bool playlistLoading(const QString &playlistId) const { return m_loadingPlaylists.contains(playlistId); }
    void ensurePlaylist(const QString &playlistId);
    const QStringList &searchIds() const { return m_search; }
    // "Recommended for you" (Recommendations): kept like search results, so they can be played.
    void setRecommended(const QList<Track> &tracks);
    // Recently played YouTube Music tracks (ListeningHistory): kept in the model like the recommendations.
    void setHistory(const QList<Track> &tracks);
    const QStringList &recommendedIds() const { return m_recommended; }
    // Tracks of a radio (Radio), kept playable under `key` like an opened album; the oldest radios go
    // once there are many. Returns their ids.
    QStringList keepRadio(const QString &key, const QList<Track> &tracks);
    static QJsonObject trackJson(const Track &t);
    static Track trackFromJson(const QJsonObject &o);
    bool isLiked(const QString &trackId) const { return m_likedSet.contains(trackId); }
    void setTrackLiked(const QString &trackId, bool liked);
    // Likes and unlikes from the account's other devices (OnlineService) — like the heart pressed here,
    // they go to the YouTube Music account too.
    void adoptLiked(const QList<Track> &liked, const QStringList &unliked);
    void setTrackColors(const QString &trackId, const QColor &a, const QColor &b);

signals:
    void accountChanged();
    void loggingInChanged();
    void syncingChanged();
    void dataChanged();
    void searchingChanged();
    void playlistResultsChanged();
    void engineChanged();
    void notice(const QString &text, const QString &icon);
    void artistsChanged();
    void artistFound(const QString &name, const QString &browseId);
    void tracksAdded(const QList<Track> &tracks);   // new transient tracks for the end of the model
    void resultsChanged();

private:
    explicit YouTubeService(QObject *parent = nullptr);

    void useCookies(const YtCookies &cookies, bool fresh);
    void syncStep(const QString &error);
    void storeTracks(const QList<Track> &tracks, bool transient);
    void publishResults(const QStringList &ids);
    void updatePlaylistInfo(const QString &playlistId, const QVariantMap &changes);
    void setLikedList(const QStringList &ids);
    void clearAccountData();
    void loadCache();
    void saveCache();               // soon, in the background (a burst of changes writes once)
    void writeCache(bool wait);     // now: built and written on a worker thread, or here

    YtCookies m_cookies;
    BrowserLogin m_login;
    YtMusicApi m_api;
    StreamResolver m_resolver;
    QString m_browserName;

    YtMusicApi::Account m_account;
    QList<YtMusicApi::Identity> m_identities;
    int m_authUser = 0;
    QString m_pageId;
    QHash<QString, Track> m_tracks;
    QStringList m_liked;
    QSet<QString> m_likedSet;
    QVariantList m_playlists;
    QHash<QString, QStringList> m_playlistTracks;
    QSet<QString> m_loadingPlaylists;
    QSet<QString> m_refreshedPlaylists;
    QStringList m_search;
    QVariantList m_playlistResults;
    QStringList m_recommended;
    QStringList m_history;
    QHash<QString, QVariantMap> m_artists;        // browseId -> artist page
    QHash<QString, QStringList> m_artistSongs;    // browseId -> top song ids
    QSet<QString> m_loadingArtists;
    int m_artistsRevision = 0;
    int m_resultsRevision = 0;
    QHash<QString, QString> m_artistIdByName;     // lower-case name -> browseId ("" = not on YouTube Music)
    QHash<QString, QStringList> m_collections;    // playlistId -> track ids (albums, "all songs")
    QSet<QString> m_loadingCollections;
    QStringList m_radioKeys;                      // radios in m_collections, oldest first
    QString m_searchQuery;
    quint64 m_searchSerial = 0;
    bool m_searching = false;
    int m_pendingSync = 0;
    QString m_syncError;
    QDateTime m_lastSync;
    QTimer m_saveTimer;
};
