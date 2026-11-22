#pragma once

#include "TrackModel.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QWebSocket>
#include <QtQml/qqmlregistration.h>

#include <functional>

class PlayerController;
class QJSEngine;
class QQmlEngine;

// Online features through the Everplay server (server/ in the repository):
//  - share links for tracks (…/t/<code>), opened in Everplay through everplay://t/<code>;
//  - shared playlists: everyone who joins through the link (…/p/<id>) adds and removes tracks;
//  - listening rooms (…/r/<code>): one queue played at the same time for everyone in the room,
//    in "open" mode (everyone adds and controls) or "host" mode (one leader, the others listen along).
// Only YouTube Music and SoundCloud tracks travel: a Spotify track goes as its YouTube Music match,
// a local file as the YouTube Music track that matches its tags.
// The account is anonymous — a display name and a token from the server — and is made on first use,
// so nothing is sent anywhere until one of these features is used. Once there is an account, the YouTube
// Music likes and the plays go to it too (unless turned off), so every device signed in to it (paired with
// an 8-character code: the phone app, the browser version) has the same "Liked" and "Recently played".
// The YouTube Music, Spotify and SoundCloud playlists go there as copies too (shared playlists with an "origin", kept in
// step with their source by whichever signed-in device sees a change; Spotify tracks as their YouTube Music
// matches, found slowly in the background), so devices without those sign-ins have them as well; here they
// aren't listed twice. The playlists' folders and the hidden ones (Library::organizer) live on the account too.
class OnlineService : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Online)
    QML_SINGLETON
    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY settingsChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY settingsChanged)
    Q_PROPERTY(bool registered READ registered NOTIFY accountChanged)
    // The account's login; empty while it has none — then it lives only on this device's token.
    Q_PROPERTY(QString login READ login NOTIFY accountChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)   // [{ id, name, seen, current }]
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool inRoom READ inRoom NOTIFY roomChanged)
    Q_PROPERTY(bool joiningRoom READ joiningRoom NOTIFY roomChanged)
    // { code, name, mode, url, isHost, canControl, playing, finished, members: [{ id, name, online, host, me }],
    //   queue: [{ itemId, row, addedBy, current, played }], currentRow }
    Q_PROPERTY(QVariantMap room READ room NOTIFY roomChanged)
    // YouTube Music's home page through the server: shelves of { title, kind: "tracks", rows } or
    // { title, kind: "cards", items: [{ kind, id, title, subtitle, cover }] } (playlists and albums).
    Q_PROPERTY(QVariantList musicShelves READ musicShelves NOTIFY musicShelvesChanged)
    Q_PROPERTY(bool librarySync READ librarySync WRITE setLibrarySync NOTIFY settingsChanged)
    Q_PROPERTY(QString pairCode READ pairCode NOTIFY pairChanged)          // shown for another device to enter
    Q_PROPERTY(double pairExpires READ pairExpires NOTIFY pairChanged)     // ms since the epoch

public:
    static constexpr const char *kDefaultServer = "https://everplay.404piwko.wtf";
    // The same server under the app's former name: older versions talk to it, its link pages and the
    // links already shared use it, and the account files on disk are named after it.
    static constexpr const char *kFormerServer = "https://nokturn.404piwko.wtf";

    struct SharedPlaylist
    {
        QString id;
        QString name;
        QString description;
        QString ownerId;
        QString ownerName;
        QString url;
        QStringList memberNames;
        QStringList trackIds;
        QHash<QString, QString> entryOf;   // track id -> the server's entry id (for removing)
        int version = 0;
        bool locked = false;               // joining it needs a password
        QString cover;                     // the members' own picture on the server, "" = none
    };

    static OnlineService *instance();
    static OnlineService *create(QQmlEngine *, QJSEngine *);

    void attach(PlayerController *player);

    QString serverUrl() const { return m_server; }
    void setServerUrl(const QString &url);
    QString displayName() const { return m_name; }
    void setDisplayName(const QString &name);
    bool registered() const { return !m_token.isEmpty(); }
    QString login() const { return m_login; }
    QVariantList devices() const { return m_devices; }
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy > 0; }
    bool inRoom() const { return !m_roomCode.isEmpty() && !m_roomState.isEmpty(); }
    bool joiningRoom() const { return m_creatingRoom || (!m_roomCode.isEmpty() && m_roomState.isEmpty()); }
    QVariantMap room() const { return m_roomView; }
    bool librarySync() const { return m_librarySync; }
    void setLibrarySync(bool on);
    QString pairCode() const { return m_pairCode; }
    double pairExpires() const { return m_pairExpires; }

    // ---- the account's library and its devices ----
    Q_INVOKABLE void syncLibrary();
    Q_INVOKABLE void createPairCode();
    Q_INVOKABLE void redeemPairCode(const QString &code);   // this device switches to that account
    void noteLiked(const Track &t, bool liked);             // the heart on a YouTube Music track

    // ---- signing in with a login and a password (AccountDialog) ----
    // Each ends with accountDone(action) or accountFailed(code, message); a new recovery key comes with
    // recoveryKeyShown. Signing in brings this device's own account (one with no login) along.
    Q_INVOKABLE void createLogin(const QString &login, const QString &password);
    Q_INVOKABLE void signIn(const QString &login, const QString &password);
    Q_INVOKABLE void recoverAccount(const QString &login, const QString &recoveryKey, const QString &password);
    Q_INVOKABLE void changePassword(const QString &password, const QString &newPassword);
    Q_INVOKABLE void newRecoveryKey(const QString &password);
    Q_INVOKABLE void refreshAccount();   // the login (set on another device) and the list of devices
    Q_INVOKABLE void signOutDevice(const QString &id);
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void copyText(const QString &text) { copyToClipboard(text, tr("Copied")); }

    // ---- links ----
    Q_INVOKABLE void shareTrack(int row);   // copies the link to the clipboard
    Q_INVOKABLE void shareCurrent();
    // everplay://t|r|p/<code>, the same as an https link to the server, or a room code alone.
    // False when the text isn't an Everplay link.
    Q_INVOKABLE bool openLink(const QString &link);
    static bool isLink(const QString &text);

    // ---- listening rooms ----
    Q_INVOKABLE void createRoom(const QString &name, const QString &mode);   // "open" | "host"
    Q_INVOKABLE void joinRoom(const QString &codeOrLink);
    Q_INVOKABLE void leaveRoom();
    Q_INVOKABLE void addToRoom(const QVariantList &rows, bool next = false);
    Q_INVOKABLE void shuffleRoom();   // what is still to play; the playing track stays
    Q_INVOKABLE void clearRoomQueue();
    Q_INVOKABLE void removeFromRoom(const QString &itemId);
    Q_INVOKABLE void playInRoom(const QString &itemId);
    Q_INVOKABLE void setRoomMode(const QString &mode);
    Q_INVOKABLE void makeHost(const QString &userId);
    Q_INVOKABLE void kick(const QString &userId);
    Q_INVOKABLE void renameRoom(const QString &name);
    Q_INVOKABLE void copyRoomLink();

    // ---- shared playlists (Playlists and Library work through these) ----
    void createPlaylist(const QString &name, const QString &description, const QVariantList &rows);
    void addToPlaylist(const QString &id, const QVariantList &rows);
    void removeFromPlaylist(const QString &id, const QString &trackId);
    bool moveInPlaylist(const QString &id, const QString &trackId, const QString &beforeTrackId);   // "" = to the end
    void editPlaylist(const QString &id, const QString &name, const QString &description);
    void deleteOrLeavePlaylist(const QString &id);   // the owner deletes it, a member leaves
    Q_INVOKABLE void copyPlaylistLink(const QString &id);
    Q_INVOKABLE void refreshPlaylists();
    QVariantList musicShelves() const { return m_musicShelves; }
    Q_INVOKABLE void loadMusicShelves();
    // A playlist or an album from a shelf: its tracks play, in the queue named `title`.
    Q_INVOKABLE void playCollection(const QString &id, const QString &title);
    Q_INVOKABLE bool isPlaylistOwner(const QString &id) const;
    Q_INVOKABLE QVariantMap playlistInfo(const QString &id) const;   // { owner, members, url, isOwner, protected }
    Q_INVOKABLE void setPlaylistPassword(const QString &id, const QString &password);   // empty removes it
    // The playlist's own picture for every member (a JPEG); `done` tells whether the server took it.
    // `quiet`: no notice when it fails (a cover sent on its own, tried again later).
    void setPlaylistCover(const QString &id, const QByteArray &jpeg, std::function<void(bool)> done, bool quiet = false);
    // The server's copy of a YouTube Music, Spotify or SoundCloud playlist ("ytmusic:<id>", …), "" = none yet.
    QString mirrorId(const QString &origin) const { return m_mirrors.value(origin); }
    void removePlaylistCover(const QString &id);
    Q_INVOKABLE void joinWithPassword(const QString &id, const QString &password);
    const QList<SharedPlaylist> &playlists() const { return m_playlists; }
    QStringList playlistTrackIds(const QString &id) const;

    // Tracks of shared playlists, the room's queue and opened links — for the library's track model.
    QList<Track> tracks() const { return m_tracks.values(); }

    Q_INVOKABLE void forgetAccount();   // deletes the account on the server and the token here

signals:
    void settingsChanged();
    void musicShelvesChanged();
    void accountChanged();
    void connectionChanged();
    void busyChanged();
    void roomChanged();
    void playlistsChanged();
    void mirrorsChanged();   // the server's copies of the account's playlists are known (or one more was made)
    void tracksAdded(const QList<Track> &tracks);   // new tracks for the end of the model
    void notice(const QString &text, const QString &icon);
    void roomOpened();                               // QML shows the room page
    void playlistOpened(const QString &id);          // QML shows a shared playlist
    void activateRequested();                        // a link came from outside — bring the window up
    void pairChanged();
    void passwordNeeded(const QString &playlistId, bool wrong);   // QML asks for it (PasswordDialog)
    void devicesChanged();
    void accountDone(const QString &action);   // "create" | "signin" | "recover" | "password" | "recovery"
    void accountFailed(const QString &code, const QString &message);
    void recoveryKeyShown(const QString &key);

private:
    explicit OnlineService(QObject *parent = nullptr);

    using JsonCallback = std::function<void(const QJsonObject &reply, const QString &error)>;
    using TracksCallback = std::function<void(const QList<Track> &tracks, int skipped)>;

    struct RoomItem
    {
        QString itemId;
        QString trackId;
        QString addedBy;
    };

    // account and requests
    void ensureAccount(std::function<void(bool ok)> then);
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, JsonCallback done);
    void send(const QByteArray &method, const QString &path, const QByteArray &data, const QByteArray &type, JsonCallback done);
    QString tokenFile() const;
    // The server as the files on disk know it: the default one under its former address.
    QString storedServer() const;
    void loadAccount();
    void switchAccount(const QString &token, const QJsonObject &user);
    void dropAccount();
    void accountRequest(const QString &action, const QString &path, const QJsonObject &body);
    static QString deviceName();
    void beginBusy();
    void endBusy();

    // WebSocket
    bool wantSocket() const;
    void connectSocket();
    void send(const QJsonObject &message);
    void onSocketMessage(const QString &text);
    void ping();

    // tracks
    Track storeTrack(const QJsonObject &o);
    QVariantList storeMusicTracks(const QJsonArray &tracks);   // model rows of the server's YouTube Music tracks
    static QJsonObject trackJson(const Track &t);
    void portableTracks(const QVariantList &rows, TracksCallback done);
    QJsonArray tracksJson(const QList<Track> &tracks) const;

    // rooms
    void applyRoom(const QJsonObject &state, qint64 serverTime);
    void clearRoom(const QString &message = {});
    void buildRoomView();
    void syncPlayer();
    double roomPosition() const;
    bool canControl() const;
    bool interceptPlayer(const QString &action, const QVariant &arg);
    void playerMovedOn(const QString &trackId);
    void roomSend(const QString &type, QJsonObject message = {});
    QString roomErrorText(const QString &code, const QString &fallback) const;

    // shared playlists
    void applyPlaylist(const QJsonObject &o);
    void fetchPlaylist(const QString &id);
    void joinPlaylist(const QString &id, const QString &password = {});
    void openShare(const QString &code);
    SharedPlaylist *findPlaylist(const QString &id);
    void loadPlaylists();
    void savePlaylists() const;

    // the library
    QString syncFile() const;
    void loadLibrarySync();
    void saveLibrarySync() const;
    void resetLibrarySync();
    void queueLibraryOp(const QJsonObject &op);
    void flushLibraryOps(std::function<void()> then);
    void applyLibrary(const QJsonObject &library);
    void notePlayed(const Track &t);
    void libraryVersion(const QJsonObject &reply);
    void saveToken() const;
    void scheduleMirrors();
    void syncProfile();   // the profile's name and picture to the account
    void syncMirrors();
    void pushMirror(const QString &origin, const QString &name, const QList<Track> &tracks, const QString &signature);
    void matchNext();
    void pushOrganizer();

    void copyToClipboard(const QString &text, const QString &message);
    void registerUrlScheme();

    QNetworkAccessManager m_net;
    QWebSocket m_ws;
    QTimer m_reconnect;
    QTimer m_pingTimer;
    QTimer m_syncTimer;
    int m_backoff = 2;
    bool m_connected = false;
    QList<QJsonObject> m_outbox;     // messages waiting for the connection

    QString m_server;
    QString m_name;
    QString m_token;
    QVariantList m_musicShelves;
    QString m_userId;
    QString m_login;
    QVariantList m_devices;
    int m_busy = 0;
    bool m_registering = false;
    bool m_avatarBusy = false;       // a profile picture is on its way to the server
    QList<std::function<void(bool)>> m_waitingForAccount;

    qint64 m_clockOffset = 0;        // server time - local time (ms)
    qint64 m_bestRtt = -1;
    QElapsedTimer m_monotonic;

    PlayerController *m_player = nullptr;

    QString m_roomCode;              // the joined room (or the one being joined)
    QJsonObject m_roomState;
    qint64 m_roomReceived = 0;       // local time of the latest state (ms)
    QList<RoomItem> m_roomItems;
    int m_roomCurrent = -1;
    QVariantMap m_roomView;
    bool m_creatingRoom = false;     // waiting for the server to make our new room

    QList<SharedPlaylist> m_playlists;
    QHash<QString, Track> m_tracks;

    bool m_librarySync = true;
    bool m_libSynced = false;        // this device's likes went up to the account once
    int m_libVersion = -1;
    QSet<QString> m_serverLiked;     // the account's YouTube Music likes at the last sync
    QList<QJsonObject> m_libOps;     // likes and unlikes waiting for the server
    bool m_libFlushing = false;
    bool m_libSyncing = false;
    bool m_libAgain = false;
    QTimer m_libTimer;
    QString m_pairCode;
    double m_pairExpires = 0;

    // copies of the YouTube Music playlists: "ytmusic:<playlist id>" -> the server's playlist
    QHash<QString, QString> m_mirrors;
    QHash<QString, QString> m_mirrorSigs;    // what each copy was last made from (kept in the sync file)
    bool m_mirrorsListed = false;            // the account's playlists were listed this session
    bool m_mirrorBusy = false;
    QTimer m_mirrorTimer;
    QStringList m_matchQueue;                // Spotify tracks waiting for their YouTube Music match
    QSet<QString> m_matchQueued;
    QTimer m_matchTimer;
    QTimer m_orgTimer;                       // the folders / hidden playlists go up a moment after a change
};
