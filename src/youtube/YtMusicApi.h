#pragma once

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <type_traits>
#include "TrackModel.h"
#include "YtCookies.h"
#include "lyrics/Lyrics.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrlQuery>
#include <QVariantList>

#include <functional>

// Client for YouTube Music's internal API ("InnerTube", the same one music.youtube.com uses).
// Authentication: account cookies + the SAPISIDHASH header — just like the browser does.
class YtMusicApi : public QObject
{
    Q_OBJECT

public:
    struct Account { QString name; QString handle; QString photo; };
    // A channel the signed-in browser session can act as: one of the Google accounts in it
    // (authUser, the browser's account index) and its own or a brand channel (pageId).
    struct Identity { QString name, handle, photo, email, pageId; int authUser = 0; };
    // An artist's YouTube Music page: header, top songs and the carousels (albums, singles,
    // "featured on" playlists, related artists) as QML-ready maps.
    struct Artist {
        QString browseId, name, description, subscribers, photo;
        QList<Track> songs;          // "Top songs" — or a channel's videos (videosOnly)
        bool videosOnly = false;     // a YouTube channel without songs (remixes, mixes…)
        QString songsPlaylistId;     // the full song list ("See all")
        QVariantList sections;       // [{ title, kind: "albums"|"playlists"|"artists", items: [...] }]
    };

    using JsonCallback = std::function<void(const QJsonObject &response, const QString &error)>;
    using TracksCallback = std::function<void(const QList<Track> &tracks, const QString &error)>;
    using LyricsCallback = std::function<void(const Lyrics &lyrics, const QString &error)>;

    // The client we send the request as (the website by default; the mobile app
    // gets timed lyrics that the web version doesn't provide).
    // signedIn: whether the account's browser cookies go along — they belong to the website; the Android
    // client with them gets no timed lyrics at all.
    struct Client { QString name; QString version; QByteArray id; QByteArray userAgent; bool signedIn = true; };
    static Client webClient();
    static Client androidClient();

    explicit YtMusicApi(QObject *parent = nullptr);

    void setCookies(const YtCookies &cookies) { m_cookies = cookies; }
    bool authenticated() const { return m_cookies.isLoggedIn(); }
    void setIdentity(int authUser, const QString &pageId) { m_authUser = authUser; m_pageId = pageId; }

    void call(const QString &endpoint, QJsonObject body, JsonCallback callback, const QUrlQuery &query = {});
    void callAs(const Client &client, const QString &endpoint, QJsonObject body, JsonCallback callback,
                const QUrlQuery &query = {});

    // Fetches the whole list (with further pages) — e.g. "VLLM" (liked) or "VL<playlist id>".
    void browseTracks(const QString &browseId, TracksCallback callback, int maxPages = 30);
    void fetchAccount(std::function<void(const Account &, const QString &error)> callback);
    // Every channel of every Google account signed in to the session.
    void fetchIdentities(std::function<void(const QList<Identity> &, const QString &error)> callback);
    void fetchLibraryPlaylists(std::function<void(const QVariantList &, const QString &error)> callback);
    // filter: "" (everything), "songs" (official audio tracks only) or "videos".
    void search(const QString &query, TracksCallback callback, const QString &filter = {});
    // Public playlists for a query: YouTube Music's own ("featured") or made by users ("community").
    // [{ id, title, subtitle, cover }]
    void searchPlaylists(const QString &query, bool featured,
                         std::function<void(const QVariantList &playlists, const QString &error)> callback);
    static QVariantList parsePlaylistResults(const QJsonValue &root);
    // The track's radio — similar tracks, the way "Start radio" in YouTube Music picks them.
    void radio(const QString &videoId, TracksCallback callback);
    void fetchArtist(const QString &browseId, std::function<void(const Artist &, const QString &error)> callback);
    // A page of album / playlist cards (an artist's "More": all singles, all albums), as parseCard gives them.
    void fetchCards(const QString &browseId, const QString &params,
                    std::function<void(const QVariantList &cards, const QString &error)> callback);
    // Finds the artist's channel by name (exact, case-insensitive match in the search results);
    // an empty id without an error = not found.
    void findArtistId(const QString &name, std::function<void(const QString &browseId, const QString &error)> callback);
    static Artist parseArtist(const QJsonObject &root);
    void setLiked(const QString &videoId, bool liked);

    // The account's own playlists (needs the signed-in account). privacy: "PRIVATE" | "UNLISTED" | "PUBLIC".
    void createPlaylist(const QString &title, const QString &description, const QString &privacy,
                        const QStringList &videoIds, std::function<void(const QString &playlistId, const QString &error)> callback);
    // actions: { action: "ACTION_ADD_VIDEO", addedVideoId } / ACTION_REMOVE_VIDEO / ACTION_SET_PLAYLIST_NAME …
    void editPlaylist(const QString &playlistId, const QJsonArray &actions, std::function<void(const QString &error)> callback);
    void deletePlaylist(const QString &playlistId, std::function<void(const QString &error)> callback);
    // Removing needs each entry's "setVideoId": videoId -> setVideoId for the whole playlist.
    void fetchSetVideoIds(const QString &playlistId, std::function<void(const QHash<QString, QString> &, const QString &error)> callback);
    // Lyrics: the timed version first (mobile client), then plain text.
    // An empty result without an error = YouTube Music has no lyrics for this track.
    void fetchLyrics(const QString &videoId, LyricsCallback callback);

    static QList<Track> parseTracks(const QJsonValue &root);
    static QVariantList parsePlaylists(const QJsonValue &root);
    static QList<Track> parseQueue(const QJsonValue &root);   // the player's queue ("next")
    // How well a search result fits a local file (title, artist, duration ±12 s): up to 4, below
    // kMatchThreshold = a different track. Covers, live, remixes, sped up / slowed versions lose
    // unless the local title is one too.
    static constexpr double kMatchThreshold = 2.0;
    static double matchScore(const Track &local, const Track &result);
    // The best result above the threshold; -1 = none.
    static int bestMatch(const Track &local, const QList<Track> &results);
    static Lyrics parseTimedLyrics(const QJsonValue &root);
    static Lyrics parsePlainLyrics(const QJsonValue &root);

private:
    // work() on a worker thread, then done(its result) back on this object's thread (dropped if it's gone).
    template <typename Work, typename Done>
    void onWorker(Work work, Done done)
    {
        using Result = std::invoke_result_t<Work>;
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [watcher, done]() mutable {
            done(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(std::move(work)));
    }
    void fetchCardsPage(const QString &browseId, const QString &params, const QString &continuation,
                        QVariantList collected, int pagesLeft,
                        std::function<void(const QVariantList &cards, const QString &error)> callback);
    void browsePage(const QString &browseId, const QString &continuation, bool commandStyle,
                    QList<Track> collected, int pagesLeft, TracksCallback callback);

    void send(const Client &client, const QString &endpoint, QJsonObject body, JsonCallback callback,
              const QUrlQuery &query, int authUser, const QString &pageId);
    void identitiesFrom(int authUser, QList<Identity> found, QStringList emails,
                        std::function<void(const QList<Identity> &, const QString &error)> callback);

    QNetworkAccessManager m_net;
    int m_authUser = 0;
    QString m_pageId;
    YtCookies m_cookies;
    Client m_web;
};
