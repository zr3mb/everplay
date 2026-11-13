import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import Everplay

ApplicationWindow {
    id: win
    width: 1420
    height: 900
    minimumWidth: 1120
    minimumHeight: 720
    visible: true
    title: "Everplay"
    color: Theme.bg
    flags: Qt.Window | Qt.FramelessWindowHint

    readonly property bool maximized: visibility === Window.Maximized || visibility === Window.FullScreen
    onVisibilityChanged: visibility => { if (visibility === Window.Minimized) Memory.trimSoon(500) }

    // The whole window content in one item (also enables screenshots in --tour mode)
    Item {
        id: shell
        anchors.fill: parent

        // Startup: the sidebar, the title bar, the page and the dock arrive one after another.
        property real intro: 0
        NumberAnimation on intro { from: 0; to: 1; duration: Theme.ms(1800); easing.type: Easing.Linear }
        function stage(start) { return Math.max(0, Math.min(1, (intro - start) / 0.5)) }
        function ease(t) { return 1 - Math.pow(1 - t, 4) }

        Rectangle {
            anchors.fill: parent
            color: Theme.bg
        }

        // The mouse's side buttons go back / forward like in a browser.
        TapHandler {
            acceptedButtons: Qt.BackButton | Qt.ForwardButton
            gesturePolicy: TapHandler.WithinBounds
            onTapped: (eventPoint, button) => button === Qt.BackButton ? Nav.back() : Nav.forward()
        }

        ThemeBackdrop {
            anchors.fill: parent
        }

        AmbientBackground {
            anchors.fill: parent
            visible: Theme.ambient
            boost: 0.9
        }

        // Plugins: animated wallpapers behind the panels, logic without a look (docs/plugins.md)
        PluginHost {
            anchors.fill: parent
            slot: "wallpaper"
        }
        PluginHost {
            width: 0; height: 0
            slot: "main"
        }

        // The layout: a full-height sidebar on the left, the title bar and the page to its right,
        // the "Now playing" sheet on the right and the player docked, floating, along the bottom.
        Sidebar {
            id: sidebar
            opacity: shell.ease(shell.stage(0))
            transform: Translate { x: (1 - shell.ease(shell.stage(0))) * -60 }
            anchors {
                top: parent.top; left: parent.left; bottom: playerBar.top
                topMargin: Theme.gap; leftMargin: Theme.gap; bottomMargin: Theme.gap
            }
            width: Nav.sidebarCompact ? 80 : 268
            Behavior on width { NumberAnimation { duration: Theme.ms(460); easing.type: Easing.OutQuint } }
        }

        TitleBar {
            id: titleBar
            opacity: shell.ease(shell.stage(0.12))
            transform: Translate { y: (1 - shell.ease(shell.stage(0.12))) * -24 }
            anchors { top: parent.top; left: sidebar.right; right: parent.right; leftMargin: Theme.gap }
            height: 64
        }

        Panel {
            id: content
            radius: Theme.radiusXL
            opacity: shell.ease(shell.stage(0.2))
            scale: 0.96 + 0.04 * shell.ease(shell.stage(0.2))
            anchors {
                top: titleBar.bottom; left: sidebar.right; right: queuePanel.left; bottom: playerBar.top
                leftMargin: Theme.gap; rightMargin: Theme.gap; bottomMargin: Theme.gap
            }

            // Motion blur: while a page slides in or its content scrolls, the pages are drawn through MotionBlur,
            // smeared along the way they moved during the frame (a 360° shutter). Only while something moves —
            // the layer stays a moment after, so a burst of wheel notches doesn't rebuild it each time.
            Item {
                id: stackHost
                anchors.fill: parent
                property point motion: Qt.point(0, 0)
                property var lastItem: null
                property real lastX: 0
                readonly property bool usable: Theme.motionBlur && blurCheck.status !== ShaderEffect.Error
                layer.enabled: usable && blurHold.running
                layer.smooth: true
                layer.effect: MotionBlur { motion: stackHost.motion }

                MotionBlur { id: blurCheck; visible: false; width: 1; height: 1 }   // the shader loads here (or not)
                Timer { id: blurHold; interval: 260 }
                FrameAnimation {
                    running: stackHost.usable && (stack.busy || blurHold.running || Nav.motionY !== 0)
                    onTriggered: {
                        const item = stack.currentItem
                        const x = item ? item.x : 0
                        const dx = item === stackHost.lastItem && stack.busy ? x - stackHost.lastX : 0
                        stackHost.lastItem = item
                        stackHost.lastX = x
                        const dy = -Nav.takeMotion()
                        const shutter = 1.25, limit = 56
                        const clamp = v => Math.max(-limit, Math.min(limit, v * shutter))
                        stackHost.motion = Qt.point(clamp(dx), clamp(dy))
                        if (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5 || stack.busy)
                            blurHold.restart()
                    }
                }

                StackView {
                    id: stack
                    anchors.fill: parent
                    initialItem: homePage

                    replaceEnter: Transition {
                        ParallelAnimation {
                            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.ms(480); easing.type: Easing.OutCubic }
                            NumberAnimation { property: "x"; from: 70; to: 0; duration: Theme.ms(760); easing.type: Easing.OutQuint }
                            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: Theme.ms(760); easing.type: Easing.OutQuint }
                        }
                    }
                    replaceExit: Transition {
                        ParallelAnimation {
                            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.ms(240); easing.type: Easing.InCubic }
                            NumberAnimation { property: "x"; from: 0; to: -50; duration: Theme.ms(300); easing.type: Easing.InCubic }
                        }
                    }
                }
            }
        }

        NowPlayingPanel {
            id: queuePanel
            radius: Theme.radiusXL
            opacity: shell.ease(shell.stage(0.32))
            transform: Translate { x: (1 - shell.ease(shell.stage(0.32))) * 60 }
            anchors {
                top: titleBar.bottom; right: parent.right; bottom: playerBar.top
                rightMargin: Nav.queueOpen ? Theme.gap : 0; bottomMargin: Theme.gap
            }
            width: Nav.queueOpen ? 352 : 0
            Behavior on width { NumberAnimation { duration: Theme.ms(520); easing.type: Easing.OutQuint } }
            Behavior on anchors.rightMargin { NumberAnimation { duration: Theme.ms(520); easing.type: Easing.OutQuint } }
        }

        PlayerBar {
            id: playerBar
            opacity: shell.ease(shell.stage(0.4))
            transform: Translate { y: (1 - shell.ease(shell.stage(0.4))) * 70 }
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: Theme.gap }
            height: 88
        }

        ImmersiveView {
            anchors.fill: parent
        }

        // Plugins' overlays: above the interface, never in the way of the mouse
        PluginHost {
            anchors.fill: parent
            slot: "overlay"
            enabled: false
        }

        // First-run wizard (above everything except notifications)
        Onboarding {
            anchors.fill: parent
        }

        // New version from GitHub Releases
        UpdateDialog {
            anchors.fill: parent
        }

        // Once, for people coming from Nokturn: the new name and look
        RebrandIntro {
            anchors.fill: parent
        }

        // Correcting a Spotify track's YouTube Music match. The dialogs are built when they open.
        Loader {
            id: matchDialog
            active: false
            sourceComponent: MatchDialog { onClosed: matchDialog.active = false }
            function show(track) {
                active = true
                item.show(track)
            }
        }

        // New playlist / editing one
        Loader {
            id: playlistDialog
            active: false
            sourceComponent: PlaylistDialog { onClosed: playlistDialog.active = false }
            function show(mode, index, rows) {
                active = true
                item.show(mode, index, rows)
            }
        }

        // A playlist, all of it or a part, into the listening room
        Loader {
            id: roomAddDialog
            active: false
            sourceComponent: RoomAddDialog { onClosed: roomAddDialog.active = false }
            function show(rows, title) {
                active = true
                item.show(rows, title)
            }
        }

        // Playlist folders: a new one, renaming / deleting one, a playlist's folder
        Loader {
            id: playlistFolderDialog
            active: false
            sourceComponent: PlaylistFolderDialog { onClosed: playlistFolderDialog.active = false }
            function show(mode, folderId, playlistIndex) {
                active = true
                if (mode === "edit")
                    item.edit(folderId)
                else if (mode === "pick")
                    item.pick(playlistIndex)
                else
                    item.create(playlistIndex)
            }
        }

        // A shared playlist's password (joining one that has it, or the owner setting it)
        Loader {
            id: passwordDialog
            active: false
            sourceComponent: PasswordDialog { onClosed: passwordDialog.active = false }
            function ask(id, wrong) {
                active = true
                item.ask(id, wrong)
            }
            function edit(id) {
                active = true
                item.edit(id)
            }
        }

        // The Everplay account: a login and a password, signing in, the recovery key
        Loader {
            id: accountDialog
            active: false
            sourceComponent: AccountDialog { onClosed: accountDialog.active = false }
            function show(mode) {
                active = true
                item.show(mode)
            }
        }

        Toast {
            id: toast
            bottomOffset: playerBar.height + Theme.gap + 18
        }

        WindowResizer {
            anchors.fill: parent
            enabled: !win.maximized
        }
    }

    Loader {
        readonly property var argv: Qt.application.arguments
        readonly property int at: argv.indexOf("--tour")
        active: at >= 0 && at + 1 < argv.length
        sourceComponent: DevTour {
            target: shell
            dir: parent.argv[parent.at + 1]
        }
    }

    Component { id: homePage; HomePage {} }
    Component { id: searchPage; SearchPage {} }
    Component { id: libraryPage; LibraryPage {} }
    Component { id: playlistPage; PlaylistPage {} }
    Component { id: artistPage; ArtistPage {} }
    Component { id: soundCloudProfilePage; SoundCloudProfilePage {} }
    Component { id: spotifyArtistPage; SpotifyArtistPage {} }
    Component { id: sourcesPage; SourcesPage {} }
    Component { id: settingsPage; SettingsPage {} }
    Component { id: equalizerPage; EqualizerPage {} }
    Component { id: statsPage; StatsPage {} }
    Component { id: roomPage; RoomPage {} }
    Component { id: pluginsPage; PluginsPage {} }

    function pageComponent(name) {
        switch (name) {
        case "search": return searchPage
        case "library": return libraryPage
        case "playlist": return playlistPage
        case "artist": return artistPage
        case "scprofile": return soundCloudProfilePage
        case "spartist": return spotifyArtistPage
        case "sources": return sourcesPage
        case "settings": return settingsPage
        case "equalizer": return equalizerPage
        case "stats": return statsPage
        case "room": return roomPage
        case "plugins": return pluginsPage
        default: return homePage
        }
    }

    Connections {
        target: Nav
        function onPageRequested(page, args) {
            stack.replace(null, win.pageComponent(page), { args: args })
            Memory.trimSoon()   // the previous page is gone once the transition ends
        }
        function onImmersiveChanged() {
            if (!Nav.immersive)
                Memory.trimSoon()
        }
        function onOnboardingChanged() {
            if (!Nav.onboarding)
                Memory.trimSoon()
        }
        function onToastRequested(text, icon) {
            toast.show(text, icon)
        }
        function onAddFolderRequested() {
            folderDialog.open()
        }
        function onMatchPickerRequested(track) {
            matchDialog.show(track)
        }
        function onPasswordDialogRequested(playlistId) { passwordDialog.edit(playlistId) }
        function onAccountDialogRequested(mode) { accountDialog.show(mode) }
        function onRoomAddRequested(rows, title) { roomAddDialog.show(rows, title) }
        function onFolderDialogRequested(mode, folderId, playlistIndex) { playlistFolderDialog.show(mode, folderId, playlistIndex) }
        function onPlaylistDialogRequested(mode, index, rows) {
            playlistDialog.show(mode, index, rows)
        }
    }

    Connections {
        target: Playlists
        function onNotice(text, icon) { toast.show(text, icon) }
        function onCreated(kind, id) { Nav.openPlaylist(kind, id) }
    }

    Connections {
        target: Library
        function onNotice(text, icon) { toast.show(text, icon) }
        function onLibraryChanged() { Memory.trimSoon(3000) }
    }

    Connections {
        target: Player
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: YouTube
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: Spotify
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: SoundCloud
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: OpenMusic
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: Lyrics
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: Online
        function onNotice(text, icon) { toast.show(text, icon) }
        function onRoomOpened() { Nav.navigate("room") }
        function onPlaylistOpened(id) { Nav.openPlaylist("shared", id) }
        function onPasswordNeeded(id, wrong) { passwordDialog.ask(id, wrong) }
        // A everplay:// link opened from a browser or a chat app
        function onActivateRequested() {
            if (win.visibility === Window.Minimized || win.visibility === Window.Hidden)
                win.showNormal()
            win.raise()
            win.requestActivate()
        }
    }

    Connections {
        target: Updater
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    Connections {
        target: Plugins
        function onNotice(text, icon) { toast.show(text, icon) }
    }

    // The in-game overlay (GameOverlay): its window is made when the menu first opens.
    Loader {
        id: gameOverlay
        active: false
        sourceComponent: OverlayWindow {}
    }
    Connections {
        target: GameOverlay
        function onOpenChanged() { if (GameOverlay.open) gameOverlay.active = true }
    }

    // In-game lyrics HUD overlay: made when first opened.
    Loader {
        id: lyricsOverlay
        active: false
        sourceComponent: LyricsOverlayWindow {}
    }
    Connections {
        target: GameOverlay
        function onLyricsOpenChanged() { if (GameOverlay.lyricsOpen) lyricsOverlay.active = true }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose a music folder")
        onAccepted: Library.addFolder(selectedFolder)
    }

    Shortcut { sequence: "Space"; onActivated: Player.toggle() }
    Shortcut { sequence: "Ctrl+Right"; onActivated: Player.next() }
    Shortcut { sequence: "Ctrl+Left"; onActivated: Player.previous() }
    Shortcut { sequence: "Ctrl+F"; onActivated: Nav.openSearch() }
    Shortcut { sequence: "Alt+Left"; onActivated: Nav.back() }
    Shortcut { sequence: "Alt+Right"; onActivated: Nav.forward() }
    Shortcut { sequence: "F11"; onActivated: Nav.immersive = !Nav.immersive }
    Shortcut {
        sequence: "Esc"
        enabled: Nav.immersive
        onActivated: Nav.immersiveQueue !== "" ? Nav.immersiveQueue = "" : Nav.immersive = false   // the queue first
    }
}
