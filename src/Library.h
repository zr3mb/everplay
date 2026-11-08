#pragma once

#include "LibraryScanner.h"
#include "TrackModel.h"

#include <QFutureWatcher>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// Music library: local files from the scanned folders + sample
// tracks from sources that aren't connected yet (YouTube Music, radio).
class Library : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(TrackModel *tracks READ tracks CONSTANT)
    Q_PROPERTY(QVariantList albums READ albums NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList recentAlbums READ recentAlbums NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList artists READ artists NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)
    Q_PROPERTY(int localCount READ localCount NOTIFY libraryChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanChanged)
    Q_PROPERTY(qreal scanProgress READ scanProgress NOTIFY scanChanged)
    Q_PROPERTY(QString scanStatus READ scanStatus NOTIFY scanChanged)
    Q_PROPERTY(int genresRevision READ genresRevision NOTIFY genresChanged)   // artist genres came in
    // The playlists laid out: [{ key, name, folderId, collapsed, items: [Library.playlists index] }] — the
    // built-in lists first ("builtin", no header), the user's folders, then one per source; hidden ones
    // only with showHidden (their own group at the end). Kept on the account too (OnlineService).
    Q_PROPERTY(QVariantList playlistGroups READ playlistGroups NOTIFY playlistsChanged)
    Q_PROPERTY(QVariantList playlistFolders READ playlistFolders NOTIFY playlistsChanged)   // [{ id, name }]
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY playlistsChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY playlistsChanged)

public:
    static Library *instance();
    static Library *create(QQmlEngine *, QJSEngine *);

    TrackModel *tracks() const { return m_tracks; }
    QVariantList albums() const { return m_albums; }
    QVariantList recentAlbums() const { return m_recentAlbums; }
    QVariantList artists() const { return m_artists; }
    QVariantList playlists() const { return m_playlists; }
    QVariantList sources() const { return m_sources; }
    QVariantList folders() const;
    int localCount() const { return m_localCount; }
    bool scanning() const { return m_watcher.isRunning(); }
    qreal scanProgress() const;
    QString scanStatus() const;
    int genresRevision() const { return m_genresChanges; }
    QVariantList playlistGroups() const { return m_groups; }
    QVariantList playlistFolders() const;
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool on);
    int hiddenCount() const { return m_hiddenCount; }

    // ---- folders and hidden playlists ----
    Q_INVOKABLE QString createPlaylistFolder(const QString &name, int moveIndex = -1);
    Q_INVOKABLE void renamePlaylistFolder(const QString &id, const QString &name);
    Q_INVOKABLE void deletePlaylistFolder(const QString &id);   // its playlists go back to their source's group
    Q_INVOKABLE void movePlaylistToFolder(int index, const QString &folderId);   // "" = no folder
    Q_INVOKABLE void setPlaylistHidden(int index, bool hidden);
    // Dragged (Ctrl + drag in the sidebar) in front of another playlist (beforeIndex; -1 = the group's end) in a
    // group: the user's order; into a folder's group the playlist joins the folder, into its source's group it
    // leaves its folder. False when it can't go there (another source's group).
    Q_INVOKABLE bool reorderPlaylist(int index, int beforeIndex, const QString &groupKey);
    Q_INVOKABLE void setGroupCollapsed(const QString &key, bool collapsed);
    // { folders, assign: { playlist key: folder id }, hidden: [playlist key], order: [playlist key], updated (ms) }
    // — what the account keeps
    QJsonObject organizer() const { return m_org; }
    void applyOrganizer(const QJsonObject &org);   // from the account (newer than ours)

    Q_INVOKABLE void addFolder(const QUrl &url);
    Q_INVOKABLE void removeFolder(const QString &path);
    Q_INVOKABLE void rescan(bool manual = true);

    // They return lists of row indices in `tracks`.
    Q_INVOKABLE QVariantList search(const QString &query, const QString &source = {}, int limit = 200) const;
    Q_INVOKABLE QVariantList tracksOfAlbum(const QString &albumKey) const;
    Q_INVOKABLE QVariantList tracksOfArtist(const QString &artist) const;
    Q_INVOKABLE QVariantList likedTracks() const;
    Q_INVOKABLE QVariantList localTracks() const;
    Q_INVOKABLE QVariantList recentlyAdded(int limit = 50) const;
    Q_INVOKABLE QVariantList playlistRows(int index);       // YouTube playlists are loaded in the background
    Q_INVOKABLE bool playlistLoading(int index) const;
    Q_INVOKABLE QVariantList artistSongRows(const QString &browseId) const;   // top songs from YouTube Music
    Q_INVOKABLE QVariantList collectionRows(const QString &playlistId);       // album / playlist opened from an artist
    Q_INVOKABLE QVariantList soundCloudCollectionRows(const QString &playlistId);   // a SoundCloud playlist opened from search
    // A SoundCloud profile's tracks: which = "popular" or "tracks" (newest first).
    Q_INVOKABLE QVariantList soundCloudProfileRows(const QString &userId, const QString &which) const;

    // Queue contexts: "playlist:<kind>", "yt:<playlist id>", "album:<key>", "artist:<name>", "library",
    // "ytartist:<browseId>" (top songs), "ytc:<playlist id>" (album / playlist from an artist page),
    // "genre:<name>", "recs" (recommended for you).
    QString playlistContext(int index) const;
    int playlistIndex(const QString &context) const;   // -1 when the context isn't a playlist
    QString playlistName(int index) const;
    int playlistCount() const { return int(m_playlists.size()); }
    QVariantList rowsForContext(const QString &context);   // current rows (YouTube playlists may still be growing)
    Q_INVOKABLE QVariantList libraryRows() const;           // local + liked on YouTube Music
    Q_INVOKABLE QVariantList onlineResults() const;         // latest YouTube Music search results
    Q_INVOKABLE QVariantList soundCloudResults() const;     // latest SoundCloud search results
    Q_INVOKABLE QVariantList recommendedRows() const;       // "Recommended for you" (Recommendations)
    // Genres from the tags, most tracks first: [{ key, name, count, colorA, colorB, seed, cover, mosaic }].
    Q_INVOKABLE QVariantList genres() const;
    Q_INVOKABLE QVariantList tracksOfGenre(const QString &genre) const;
    Q_INVOKABLE int totalDuration(const QVariantList &rows) const;   // sum of durations (seconds)

    // Playlist covers: the user's own picture, otherwise the source's cover (YouTube Music, Spotify),
    // otherwise a 2×2 collage of four random covers of the playlist's tracks ("mosaic" in the map).
    Q_INVOKABLE bool setPlaylistCover(int index, const QUrl &image);
    Q_INVOKABLE void resetPlaylistCover(int index);
    Q_INVOKABLE void shufflePlaylistCover(int index);   // four other covers for the collage

    void ensurePalette(int row);

signals:
    void libraryChanged();
    void playlistsChanged();
    void foldersChanged();
    void scanChanged();
    void sourcesChanged();
    void genresChanged();
    void notice(const QString &text, const QString &icon);
    void organizerChanged();   // changed here: the account gets it

public:
    // The track's genres in the interface language: its tags, or its artist's genre (streaming).
    QStringList genresOf(const Track &t) const;

private:
    explicit Library(QObject *parent = nullptr);

    void loadCache();
    void saveCache() const;
    void saveFolders() const;
    void applyLocalTracks(QList<Track> local);
    void rebuildModel();
    void requestRebuild();      // rebuildModel() once for a burst of changes
    void buildSources();
    void rebuildCollections();
    void rebuildPlaylists();
    void loadOrganizer();
    void organizerEdited();     // saved, laid out again, announced
    void layOutPlaylists();     // the hidden / folder marks and m_groups
    QVariantList rowsOfIds(const QStringList &ids) const;
    static QString coverKey(const QVariantMap &playlist);
    void applyPlaylistCover(QVariantMap &playlist) const;
    void refreshPlaylistCover(int index);
    void sendCustomCovers();
    void onScanFinished();
    QList<Track> demoTracks() const;

    TrackModel *m_tracks;
    QList<Track> m_local;
    QStringList m_folders;
    QSet<QString> m_liked;
    int m_localCount = 0;

    QVariantList m_albums;
    QVariantList m_recentAlbums;
    QVariantList m_artists;
    QVariantList m_playlists;
    QJsonObject m_org;
    QVariantList m_groups;
    QSet<QString> m_collapsed;   // groups folded on this device
    bool m_showHidden = false;
    int m_hiddenCount = 0;
    QVariantList m_sources;
    mutable QVariantList m_genres;        // genres(), built when asked
    mutable int m_genresRevision = -1;
    mutable int m_genresArtistRevision = -1;
    int m_genresChanges = 0;

    QFutureWatcher<Scanner::Result> m_watcher;
    QTimer m_rebuildTimer;
    QNetworkAccessManager m_net;
    QSet<QString> m_paletteDone;
    QSet<QString> m_sendingCovers;   // cover keys on their way to the server
    bool m_rescanPending = false;
    bool m_manualScan = false;
};
