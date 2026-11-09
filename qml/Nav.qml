pragma Singleton
import QtQuick

// Global UI state: current page, history, panels.
QtObject {
    id: nav

    property string page: "home"
    property var args: ({})
    property var backStack: []
    property var forwardStack: []
    // Scroll positions (ScrollAssist): the current page's, kept in its history entry; the one to restore on
    // the page being shown (0 = from the top); a serial per shown page, so a page on its way out can't write.
    property real scrollY: 0
    // How far the page's content scrolled since the last frame (ScrollAssist adds, the motion blur takes).
    property real motionY: 0
    function addMotion(dy) { motionY += dy }
    function takeMotion() { const m = motionY; motionY = 0; return m }
    property real restoreScroll: 0
    property int pageSerial: 0

    // Window layout — remembered between launches.
    property bool queueOpen: Prefs.boolValue("ui/queueOpen", true)
    property string panelTab: "queue"      // "Now playing" panel: "queue" | "lyrics"
    property bool immersiveLyrics: Prefs.boolValue("ui/immersiveLyrics", true)   // lyrics next to the cover in full-screen mode
    property bool immersive: false
    property string immersiveQueue: ""     // the queue over the full-screen player: "" | "half" | "full"
    onImmersiveChanged: if (!immersive) immersiveQueue = ""
    // Phone layout (Android, or --mobile on the desktop): bottom navigation, mini player, no side panels.
    property bool mobile: false
    property bool sidebarCompact: Prefs.boolValue("ui/sidebarCompact", false)
    onQueueOpenChanged: Prefs.setValue("ui/queueOpen", queueOpen)
    onImmersiveLyricsChanged: Prefs.setValue("ui/immersiveLyrics", immersiveLyrics)
    onSidebarCompactChanged: Prefs.setValue("ui/sidebarCompact", sidebarCompact)

    // First-run wizard (appearance, music sources, playback, integrations).
    property bool onboarding: !Prefs.boolValue("onboarding/done", false)
    property int onboardingStep: 0
    // >= 0: the Nokturn → Everplay intro held on that second (the tour's screenshots)
    property real introTime: -1
    function finishOnboarding() {
        Prefs.setValue("onboarding/done", true)
        Prefs.setValue("everplay/introSeen", true)   // the "Nokturn is now Everplay" show is for people coming from it
        onboarding = false
    }
    function startOnboarding() {
        onboardingStep = 0
        onboarding = true
    }

    readonly property bool canGoBack: backStack.length > 0
    readonly property bool canGoForward: forwardStack.length > 0

    signal pageRequested(string page, var args)
    signal searchFocusRequested()
    signal toastRequested(string text, string icon)
    signal addFolderRequested()
    signal matchPickerRequested(var track)
    signal playlistDialogRequested(string mode, int index, var rows)
    signal moreRequested(bool open)
    signal webLoginRequested(string service)   // Android: "youtube" | "soundcloud" in the in-app browser   // phone layout: the sheet with the pages the bottom bar has no room for
    signal passwordDialogRequested(string playlistId)   // the owner sets a shared playlist's password
    signal accountDialogRequested(string mode)          // AccountDialog: "create" | "signin" | "password" | "newkey"
    signal folderDialogRequested(string mode, string folderId, int playlistIndex)   // FolderDialog
    signal roomAddRequested(var rows, string title)     // RoomAddDialog: a playlist, or part of it, into the room

    // New playlist (optionally with tracks — rows of Library.tracks) / editing an existing one (PlaylistDialog).
    function createPlaylist(rows) {
        playlistDialogRequested("create", -1, rows || [])
    }
    function editPlaylistPassword(playlistId) { passwordDialogRequested(playlistId) }
    function account(mode) { accountDialogRequested(mode) }
    function addToRoom(rows, title) { roomAddRequested(rows, title || "") }
    function createFolder(playlistIndex) { folderDialogRequested("create", "", playlistIndex) }
    function editFolder(folderId) { folderDialogRequested("edit", folderId, -1) }
    function moveToFolder(playlistIndex) { folderDialogRequested("pick", "", playlistIndex) }
    function editPlaylist(index) {
        playlistDialogRequested("edit", index, [])
    }
    // The page of the playlist that Playlists has just created.
    function openPlaylist(kind, id) {
        const list = Library.playlists
        for (let i = 0; i < list.length; ++i) {
            const p = list[i]
            if (p.kind === kind && (p.userId === id || p.ytId === id || p.sharedId === id))
                return navigate("playlist", { kind: "playlist", index: i })
        }
    }

    // A Spotify track plays from the wrong YouTube Music version — choose another (MatchDialog).
    function chooseMatch(track) {
        matchPickerRequested(track)
    }

    function addFolder() {
        addFolderRequested()
    }

    function navigate(p, a) {
        a = a || {}
        if (p === page && JSON.stringify(a) === JSON.stringify(args))
            return
        backStack = backStack.concat([{ page: page, args: args, scroll: scrollY }])
        forwardStack = []
        _show(p, a, 0)
    }

    function back() {
        if (!canGoBack) return
        const e = backStack[backStack.length - 1]
        backStack = backStack.slice(0, -1)
        forwardStack = forwardStack.concat([{ page: page, args: args, scroll: scrollY }])
        _show(e.page, e.args, e.scroll || 0)
    }

    function forward() {
        if (!canGoForward) return
        const e = forwardStack[forwardStack.length - 1]
        forwardStack = forwardStack.slice(0, -1)
        backStack = backStack.concat([{ page: page, args: args, scroll: scrollY }])
        _show(e.page, e.args, e.scroll || 0)
    }

    // A page records its state (e.g. the search query) in the current history entry, so going back
    // to it restores it.
    function remember(a) {
        args = a
    }

    function openSearch() {
        navigate("search")
        searchFocusRequested()
    }

    // Artist profile. With a YouTube Music channel id the page loads it directly; with a name only
    // (local files) it looks the artist up — "A, B" or "A & B" goes to the first artist. A SoundCloud
    // track's artist ("scu:<user id>") opens the uploader's SoundCloud profile.
    function openArtist(name, browseId) {
        if (!name && !browseId)
            return
        immersive = false
        if (String(browseId || "").startsWith("scu:")) {
            openSoundCloudProfile(browseId.slice(4), name)
            return
        }
        // A Spotify track's artist: their Spotify page while signed in there, otherwise YouTube Music by name.
        if (String(browseId || "").startsWith("spa:")) {
            if (Spotify.connected) {
                navigate("spartist", { artistId: browseId.slice(4), name: name || "" })
                return
            }
            browseId = ""
        }
        const first = browseId ? name : String(name).split(/,\s+| & | feat\.? | ft\.? /i)[0].trim()
        navigate("artist", { name: first, browseId: browseId || "" })
    }

    function openSoundCloudProfile(userId, name, photo) {
        immersive = false
        navigate("scprofile", { userId: userId, name: name || "", photo: photo || "" })
    }

    // Where a Library.playlists entry comes from, for SourceBadge ("" = the user's own or a built-in list).
    function playlistSource(p) {
        const kind = (p || {}).kind || ""
        if (kind === "ytplaylist") return "youtube"
        if (kind === "spplaylist" || kind === "spotifyliked") return "spotify"
        if (kind === "scplaylist" || kind === "scliked") return "soundcloud"
        return ""
    }

    function toast(text, icon) {
        toastRequested(text, icon || "check")
    }

    function _show(p, a, scroll) {
        restoreScroll = scroll || 0
        scrollY = restoreScroll
        ++pageSerial
        page = p
        args = a
        pageRequested(p, a)
    }
}
