#pragma once

#include "SpotifyWebLogin.h"
#include "TrackModel.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>

class QJSEngine;
class QQmlEngine;

// Spotify: playlists and liked songs synced through the official Web API. Spotify doesn't let other
// apps play its audio, so each track is matched to YouTube Music (title, artist, duration) when it
// plays and streamed from there; matches are remembered.
//
// Sign-in, two ways:
// - Browser (default, works without Premium): a separate browser window like for YouTube, then the
//   web player's own access token is read from the signed-in profile (SpotifyWebLogin). It lasts
//   about an hour and is renewed invisibly from the same profile.
// - Own Spotify app (developer mode needs Premium): OAuth 2.0 with PKCE — no client secret, the
//   redirect lands on a tiny local server at http://127.0.0.1:43117/callback. A build can ship
//   a default Client ID (-DEVERPLAY_SPOTIFY_CLIENT_ID=…). The refresh token is stored encrypted.
class SpotifyService : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Spotify)
    QML_SINGLETON

    Q_PROPERTY(bool connected READ connected NOTIFY accountChanged)
    Q_PROPERTY(QString accountName READ accountName NOTIFY accountChanged)
    Q_PROPERTY(bool loggingIn READ loggingIn NOTIFY loggingInChanged)
    Q_PROPERTY(QString loginStatus READ loginStatus NOTIFY loggingInChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(QString clientId READ clientId WRITE setClientId NOTIFY settingsChanged)
    Q_PROPERTY(bool builtInClientId READ builtInClientId CONSTANT)
    Q_PROPERTY(QString redirectUri READ redirectUri CONSTANT)
    Q_PROPERTY(int likedCount READ likedCount NOTIFY dataChanged)
    Q_PROPERTY(int playlistCount READ playlistCount NOTIFY dataChanged)
    Q_PROPERTY(QString lastSync READ lastSync NOTIFY dataChanged)
    Q_PROPERTY(int artistsRevision READ artistsRevision NOTIFY artistsChanged)   // an artist page or album arrived

public:
    static SpotifyService *instance();
    static SpotifyService *create(QQmlEngine *, QJSEngine *);

    bool connected() const { return m_web || !m_refreshToken.isEmpty(); }
    QString accountName() const { return m_accountName; }
    bool loggingIn() const { return m_server.isListening() || m_browserLogin; }
    QString loginStatus() const { return m_browserLogin ? m_webLogin.status() : QString(); }
    bool syncing() const { return m_syncing; }
    QString clientId() const { return m_clientId; }
    void setClientId(const QString &id);
    bool builtInClientId() const;
    QString redirectUri() const;
    int likedCount() const { return int(m_liked.size()); }
    int playlistCount() const { return int(m_playlists.size()); }
    QString lastSync() const;

    Q_INVOKABLE void loginWithBrowser();
    Q_INVOKABLE void login();   // own Spotify app (Client ID)
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void refresh();

    // An artist's page (needs the Spotify sign-in): { loading, error, name, photo, followers, genres,
    // albums: [{ id, title, subtitle, cover, count, kind: "album"|"single" }], singles: [...] } — {} before loadArtist.
    // The top tracks are Library.spotifyArtistRows(); an album opened from it, Library.spotifyAlbumRows().
    int artistsRevision() const { return m_artistsRevision; }
    Q_INVOKABLE void loadArtist(const QString &artistId);
    Q_INVOKABLE QVariantMap artist(const QString &artistId) const { return m_artists.value(artistId); }
    Q_INVOKABLE void ensureAlbum(const QString &albumId);
    Q_INVOKABLE bool albumLoading(const QString &albumId) const { return m_loadingAlbums.contains(albumId); }
    QStringList artistTrackIds(const QString &artistId) const { return m_artistTracks.value(artistId); }
    QStringList albumTrackIds(const QString &albumId) const { return m_albums.value(albumId); }

    // For Library / Player
    QList<Track> tracks() const;
    QVariantList playlists() const { return m_playlists; }   // [{ id, name, cover, count }]
    const QStringList &likedIds() const { return m_liked; }
    QStringList playlistTrackIds(const QString &playlistId) const { return m_playlistTracks.value(playlistId); }

    // YouTube Music match of a Spotify track ("sp:<id>"): videoId, "" = not found.
    bool matchKnown(const QString &trackId) const { return m_matches.contains(trackId); }
    Q_INVOKABLE QString matchedVideoId(const QString &trackId) const { return m_matches.value(trackId); }
    void match(const QString &trackId, std::function<void(const QString &videoId)> callback);
    Track trackFor(const QString &trackId) const;   // synced data, or the track model's (local playlists)

    // Correcting a wrong match: the search behind it ("artist title"), a few YouTube Music results
    // to choose from (matchesFound), the user's choice — it sticks — and matching from scratch.
    Q_INVOKABLE QString matchQuery(const QString &trackId) const;
    Q_INVOKABLE bool manualMatch(const QString &trackId) const { return m_manual.contains(trackId); }
    Q_INVOKABLE void findMatches(const QString &trackId, const QString &query);
    Q_INVOKABLE void chooseMatch(const QString &trackId, const QString &videoId);
    Q_INVOKABLE void rematch(const QString &trackId);

signals:
    // [{ videoId, title, artist, album, durationText, cover, kind: "song"|"video", current }]
    void matchesFound(const QString &trackId, const QVariantList &results, const QString &error);
    void matchChanged(const QString &trackId);   // a different videoId than before
    void accountChanged();
    void loggingInChanged();
    void syncingChanged();
    void dataChanged();
    void artistsChanged();
    void tracksAdded(const QList<Track> &tracks);   // new transient tracks (artist pages) for the end of the model
    void settingsChanged();
    void notice(const QString &text, const QString &icon);

private:
    using JsonCallback = std::function<void(const QJsonObject &json, const QString &error)>;

    explicit SpotifyService(QObject *parent = nullptr);

    // sign-in
    void onCallback();
    void exchangeCode(const QString &code);
    void tokenRequest(const QUrlQuery &form, std::function<void(bool ok, const QString &error)> done);
    void withAccessToken(std::function<void(const QString &error)> next);
    void onWebToken(const QString &token, const QDateTime &expiresUtc, const QJsonObject &web);
    void onWebFailed(const QString &reason);
    void deliverToken(const QString &error);
    void saveToken() const;
    void loadToken();

    // API
    void get(const QUrl &url, JsonCallback callback, int attempt = 0);
    void getAll(const QUrl &first, QJsonArray collected, std::function<void(const QJsonArray &items, const QString &error)> done);

    // sync through the web player's queries (browser sign-in)
    void query(const QString &name, const QJsonObject &variables, JsonCallback callback, int attempt = 0);
    void syncWeb();
    void syncWebLibrary(int offset, QVariantList playlists);
    void syncWebPlaylist(QStringList queue, int offset, QStringList ids);
    void syncWebLiked(int offset, QStringList ids);
    QString storeWebTrack(const QJsonObject &data, const QString &uri);

    // sync through the Web API (own Spotify app)
    void syncPlaylists();
    void syncNextPlaylist(QStringList queue);
    void syncLiked();
    void finishSync(const QString &error);
    QStringList storeItems(const QJsonArray &items, bool wrapped, bool transient = false, const QJsonObject &albumOf = {});

    void loadCache();
    void saveCache() const;
    void saveMatches() const;
    void finishMatch(const QString &trackId, const QString &videoId, bool remember);

    QNetworkAccessManager m_net;
    QTcpServer m_server;
    SpotifyWebLogin m_webLogin;
    bool m_web = false;            // signed in through the browser (web player token)
    bool m_browserLogin = false;   // the browser sign-in is in progress (not just a token renewal)
    QJsonObject m_webApi;          // the web player's queries and headers (SpotifyWebLogin::succeeded)
    bool m_webCaptured = false;    // captured during this run
    QString m_verifier;
    QString m_state;

    QString m_clientId;
    QString m_refreshToken;
    QString m_accessToken;
    QDateTime m_accessExpires;
    QList<std::function<void(const QString &)>> m_waitingForToken;
    bool m_refreshing = false;

    QString m_accountName;
    QHash<QString, Track> m_tracks;                 // "sp:<id>" -> track
    QStringList m_liked;
    QVariantList m_playlists;
    QHash<QString, QStringList> m_playlistTracks;
    QHash<QString, QString> m_snapshots;            // playlist id -> snapshot_id (skip unchanged playlists)
    bool m_syncing = false;
    QString m_syncError;
    QDateTime m_lastSync;

    QHash<QString, QVariantMap> m_artists;          // artist id -> artist()
    QHash<QString, QStringList> m_artistTracks;     // artist id -> top tracks
    QHash<QString, QStringList> m_albums;           // album id -> tracks (opened from an artist page)
    QSet<QString> m_loadingAlbums;
    int m_artistsRevision = 0;
    void updateArtist(const QString &artistId, const QVariantMap &fields);
    void publishTracks(const QStringList &ids);

    QHash<QString, QString> m_matches;              // "sp:<id>" -> YouTube videoId ("" = not found)
    QSet<QString> m_manual;                         // matches the user chose — never replaced automatically
    QHash<QString, QList<std::function<void(const QString &)>>> m_matching;
    QTimer m_matchesSave;
};
