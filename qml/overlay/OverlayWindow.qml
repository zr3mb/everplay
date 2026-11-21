import QtQuick
import QtQuick.Window
import Everplay

// The in-game overlay (GameOverlay): an XMB-style menu over the dimmed game. Categories run across,
// the chosen one's items run down from under it — the items before the selected one move above the
// row, as on the PS3. Arrows, mouse (click, wheel, drag sideways) or the pad; Esc / B goes back, the
// hotkey closes. Made on the first open (Main.qml's loader) and then only hidden: a new window would
// get a new GPU context, and the covers cached for the old one crash Qt's renderer.
Window {
    id: win

    // Windows: never the active window — the game keeps the focus (and keeps drawing); keys come from
    // GameOverlay's keyboard hook.
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool | Qt.NoDropShadowWindowHint
           | (Theme.windows ? Qt.WindowDoesNotAcceptFocus : 0)
    transientParent: null      // its own window: not hidden with a minimized main window
    color: "transparent"
    title: "Everplay"
    x: GameOverlay.area.x
    y: GameOverlay.area.y
    width: Math.max(640, GameOverlay.area.width)
    height: Math.max(480, GameOverlay.area.height)
    // Fades in when it opens and out when GameOverlay closes, then hides.
    property bool shown: false
    property bool appeared: false
    property real fade: shown ? 1 : 0
    Behavior on fade { NumberAnimation { duration: win.shown ? 260 : 180; easing.type: Easing.OutCubic } }
    onFadeChanged: if (fade >= 1) appeared = true
    visible: shown || fade > 0
    onVisibleChanged: GameOverlay.view = visible ? menu : null

    property bool prepared: false
    function open() {
        if (!prepared) {
            GameOverlay.prepare(win)   // before it's first shown (layer-shell on Wayland)
            prepared = true
        }
        menu.adjusting = false
        shown = true
        GameOverlay.activate(win)
        menu.forceActiveFocus()
    }
    Component.onCompleted: open()
    onActiveChanged: if (active) menu.forceActiveFocus()

    Connections {
        target: GameOverlay
        function onOpenChanged() {
            if (GameOverlay.open)
                win.open()   // also when it was still fading out
            else
                win.shown = false
        }
        function onPadInput(action) { if (win.shown) menu.act(action) }
    }

    // ---- the game, dimmed ----
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.66 * win.fade
    }

    FocusScope {
        id: menu
        anchors.fill: parent
        opacity: win.fade
        scale: 1.03 - 0.03 * win.fade
        focus: true

        readonly property real s: Math.max(0.7, Math.min(width / 1920, height / 1080))   // UI scale
        readonly property real rowY: height * 0.29            // the category row
        readonly property real anchorX: width * 0.22          // where the chosen category sits
        readonly property real itemH: 86 * s
        readonly property real rowH: 150 * s                  // gap the items above leave for the row

        // ---- what's in the menu ----
        readonly property var categories: [
            { key: "now", title: qsTr("Now playing"), icon: "playCircle" },
            { key: "queue", title: qsTr("Queue"), icon: "queue" },
            { key: "playlists", title: qsTr("Playlists"), icon: "library" }
        ]
        property int cat: 0
        property var picks: [0, 0, 0]       // the selected item in each category
        property var sub: null              // an opened playlist: { index, title, cover, context, … }
        property int subPick: 0
        property bool adjusting: false      // the volume item takes left / right
        readonly property int selection: sub ? subPick : picks[cat]

        function contextOf(p) {
            return p.kind === "ytplaylist" ? "yt:" + p.ytId : p.kind === "spplaylist" ? "sp:" + p.spId
                 : p.kind === "scplaylist" ? "sc:" + p.scId
                 : p.kind === "user" ? "user:" + p.userId : "playlist:" + p.kind
        }
        function trackItem(row, action) {
            const t = Library.tracks.get(row)
            return { kind: "track", row: row, title: t.title || "", subtitle: t.artist || "", cover: t.cover || "",
                     colorA: t.colorA, colorB: t.colorB, seed: t.seed || 0, action: action }
        }
        readonly property var items: {
            // Never true: the reads make the menu follow the library and the player.
            if (Library.tracks.revision < 0 || YouTube.resultsRevision < 0)
                return []
            if (sub) {
                const rows = Library.playlistRows(sub.index)
                const out = [
                    { kind: "action", icon: "play", title: qsTr("Play all"), subtitle: qsTr("%n tracks", "", rows.length), action: "playAll" },
                    { kind: "action", icon: "shuffle", title: qsTr("Shuffle"), subtitle: qsTr("In random order"), action: "shuffleAll" }
                ]
                if (rows.length === 0)
                    out.push({ kind: "action", icon: "music", title: Library.playlistLoading(sub.index) ? qsTr("Loading…") : qsTr("This playlist is still empty"), disabled: true })
                return out.concat(rows.slice(0, 400).map(r => trackItem(r, "playRow")))
            }
            const key = categories[cat].key
            if (key === "now") {
                const repeatNames = [qsTr("Off"), qsTr("All"), qsTr("One")]
                return [
                    { kind: "now", title: Player.hasTrack ? Player.title : qsTr("Nothing is playing"),
                      subtitle: Player.hasTrack ? Player.artist : qsTr("Pick something from your playlists"),
                      cover: Player.cover, colorA: Player.colorA, colorB: Player.colorB, seed: Player.seed,
                      action: "toggle" },
                    { kind: "action", icon: "next", title: qsTr("Next track"), action: "next" },
                    { kind: "action", icon: "prev", title: qsTr("Previous track"), action: "previous" },
                    { kind: "volume", icon: Player.muted || Player.volume === 0 ? "volumeOff" : "volumeUp", title: qsTr("Volume"),
                      subtitle: Math.round(Player.volume * 100) + "%", action: "volume" },
                    { kind: "action", icon: "shuffle", title: qsTr("Shuffle"), subtitle: Player.shuffle ? qsTr("On") : qsTr("Off"), action: "shuffle" },
                    { kind: "action", icon: Player.repeatMode === 2 ? "repeatOne" : "repeat", title: qsTr("Repeat"),
                      subtitle: repeatNames[Player.repeatMode] || "", action: "repeat" },
                    { kind: "action", icon: Player.liked ? "heart" : "heartOutline", title: Player.liked ? qsTr("Liked") : qsTr("Like"),
                      action: "like", disabled: !Player.hasTrack },
                    { kind: "action", icon: "lyrics", title: qsTr("In-game lyrics overlay"),
                      subtitle: GameOverlay.lyricsOpen ? qsTr("Visible") : qsTr("Hidden"),
                      action: "toggleLyricsOverlay" }
                ]
            }
            if (key === "queue") {
                const next = Player.upNext.slice(0, 200)
                if (next.length === 0)
                    return [{ kind: "action", icon: "queue", title: qsTr("Nothing up next"), subtitle: qsTr("Pick a playlist to fill the queue"), disabled: true }]
                return next.map(r => trackItem(r, "jump"))
            }
            return Library.playlists.map((p, i) => ({
                // A collage's first cover: the collage itself (a layer through a Shape) crashes Qt's
                // renderer in a second window.
                kind: "playlist", index: i, title: p.name, icon: p.icon || "music", cover: p.cover || (p.mosaic || [])[0] || "",
                colorA: p.colorA, colorB: p.colorB, seed: p.seed || 0,
                subtitle: p.count >= 0 ? qsTr("%n tracks", "", p.count) : "", action: "open" }))
        }
        onItemsChanged: if (selection >= items.length) select(Math.max(0, items.length - 1))

        function select(i) {
            i = Math.max(0, Math.min(items.length - 1, i))
            if (sub) {
                subPick = i
            } else {
                const p = picks.slice()
                p[cat] = i
                picks = p
            }
            adjusting = false
        }
        function setCategory(i) {
            i = Math.max(0, Math.min(categories.length - 1, i))
            if (i === cat && !sub)
                return
            sub = null
            adjusting = false
            cat = i
        }
        function back() {
            if (adjusting) adjusting = false
            else if (sub) sub = null
            else GameOverlay.hide()
        }
        function changeVolume(step) {
            Player.muted = false
            Player.volume = Math.max(0, Math.min(1, Math.round((Player.volume + step) * 20) / 20))
        }
        function activate(item) {
            if (!item || item.disabled)
                return
            switch (item.action) {
            case "toggle": Player.toggle(); break
            case "next": Player.next(); break
            case "previous": Player.previous(); break
            case "volume": adjusting = !adjusting; break
            case "shuffle": Player.shuffle = !Player.shuffle; break
            case "repeat": Player.cycleRepeat(); break
            case "like": Player.toggleLiked(); break
            case "toggleLyricsOverlay": GameOverlay.toggleLyrics(); break
            case "jump": Player.jumpTo(item.row); break
            case "open":
                sub = { index: item.index, title: item.title, cover: item.cover,
                        colorA: item.colorA, colorB: item.colorB, seed: item.seed, icon: item.icon,
                        context: contextOf(Library.playlists[item.index]) }
                subPick = 0
                break
            case "playAll": {
                const rows = Library.playlistRows(sub.index)
                if (rows.length) Player.playFrom(rows, rows[0], sub.context)
                break
            }
            case "shuffleAll": Player.playShuffled(Library.playlistRows(sub.index), sub.context); break
            case "playRow": Player.playFrom(Library.playlistRows(sub.index), item.row, sub.context); break
            }
        }
        // One step of input from the keyboard, the mouse or the pad.
        function act(action) {
            switch (action) {
            case "left":
                if (adjusting) changeVolume(-0.05)
                else if (sub) back()
                else setCategory(cat - 1)
                break
            case "right":
                if (adjusting) changeVolume(0.05)
                else if (!sub) setCategory(cat + 1)
                break
            case "up": adjusting ? changeVolume(0.05) : select(selection - 1); break
            case "down": adjusting ? changeVolume(-0.05) : select(selection + 1); break
            case "accept": activate(items[selection]); break
            case "back": back(); break
            case "playPause": Player.toggle(); break
            case "previousTrack": Player.previous(); break
            case "nextTrack": Player.next(); break
            }
        }

        Keys.onPressed: event => {
            const map = {
                [Qt.Key_Left]: "left", [Qt.Key_Right]: "right", [Qt.Key_Up]: "up", [Qt.Key_Down]: "down",
                [Qt.Key_Return]: "accept", [Qt.Key_Enter]: "accept", [Qt.Key_Space]: "accept",
                [Qt.Key_Escape]: "back", [Qt.Key_Backspace]: "back",
                [Qt.Key_MediaPlay]: "playPause", [Qt.Key_MediaTogglePlayPause]: "playPause",
                [Qt.Key_MediaNext]: "nextTrack", [Qt.Key_MediaPrevious]: "previousTrack"
            }
            if (map[event.key] !== undefined) {
                act(map[event.key])
                event.accepted = true
            } else if (event.key === Qt.Key_PageUp || event.key === Qt.Key_PageDown) {
                select(selection + (event.key === Qt.Key_PageUp ? -8 : 8))
                event.accepted = true
            } else if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                select(event.key === Qt.Key_Home ? 0 : items.length - 1)
                event.accepted = true
            }
        }

        // ---- mouse: wheel, drag sideways, right click goes back ----
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            property real pressX: 0
            property bool dragged: false
            onPressed: mouse => { pressX = mouse.x; dragged = false }
            onPositionChanged: mouse => {
                const dx = mouse.x - pressX
                if (Math.abs(dx) > 90 * menu.s) {
                    menu.act(dx < 0 ? "right" : "left")   // like swiping: drag left for the next category
                    pressX = mouse.x
                    dragged = true
                }
            }
            onClicked: mouse => { if (mouse.button === Qt.RightButton) menu.back() }
            property real wheelX: 0
            property real wheelY: 0
            onWheel: wheel => {
                const horizontal = wheel.angleDelta.x !== 0 || (wheel.modifiers & Qt.ShiftModifier)
                if (horizontal) {
                    wheelX += wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y
                    while (Math.abs(wheelX) >= 120) {
                        menu.act(wheelX > 0 ? "left" : "right")
                        wheelX -= wheelX > 0 ? 120 : -120
                    }
                } else {
                    wheelY += wheel.angleDelta.y
                    while (Math.abs(wheelY) >= 120) {
                        menu.act(wheelY > 0 ? "up" : "down")
                        wheelY -= wheelY > 0 ? 120 : -120
                    }
                }
            }
        }

        // ---- top: logo, clock ----
        Row {
            x: 56 * menu.s
            y: 44 * menu.s
            spacing: 12 * menu.s
            LogoMark {
                anchors.verticalCenter: parent.verticalCenter
                scale: 0.9 * menu.s
                transformOrigin: Item.Left
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Everplay"
                color: Theme.text
                font.family: Theme.fontDisplay
                font.pixelSize: 22 * menu.s
                font.weight: Font.DemiBold
            }
        }
        Column {
            id: clock
            anchors.right: parent.right
            anchors.rightMargin: 60 * menu.s
            y: 40 * menu.s
            property date now: new Date()
            Timer { interval: 5000; running: true; repeat: true; onTriggered: clock.now = new Date() }
            Text {
                anchors.right: parent.right
                text: Qt.formatTime(clock.now, "HH:mm")
                color: Theme.text
                font.family: Theme.fontLight
                font.pixelSize: 34 * menu.s
                font.weight: Font.Light
            }
            Text {
                anchors.right: parent.right
                text: clock.now.toLocaleDateString(Qt.locale(Language.effective), "dddd, d MMMM")
                color: Theme.textDim
                font.pixelSize: 15 * menu.s
            }
        }

        // ---- the category row ----
        Item {
            id: row
            x: menu.anchorX - menu.cat * spacing
            y: menu.rowY
            width: menu.categories.length * spacing
            height: 110 * menu.s
            readonly property real spacing: 190 * menu.s
            // Not while the window takes its size (it would slide in apart from the items).
            Behavior on x { enabled: win.appeared; NumberAnimation { duration: Theme.ms(380); easing.type: Easing.OutCubic } }

            Repeater {
                model: menu.categories
                Item {
                    id: catItem
                    required property var modelData
                    required property int index
                    readonly property bool current: index === menu.cat
                    x: index * row.spacing
                    width: row.spacing
                    height: row.height
                    opacity: current ? 1 : (menu.sub ? 0.18 : 0.42)
                    Behavior on opacity { NumberAnimation { duration: Theme.ms(260) } }

                    Icon {
                        id: catIcon
                        name: catItem.modelData.icon
                        size: (catItem.current ? 64 : 46) * menu.s
                        color: Theme.text
                        y: (64 * menu.s - size) / 2
                        Behavior on size { NumberAnimation { duration: Theme.ms(260); easing.type: Easing.OutCubic } }
                    }
                    Text {
                        y: 76 * menu.s
                        visible: catItem.current
                        text: menu.sub ? catItem.modelData.title + "  ›  " + menu.sub.title : catItem.modelData.title
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: 21 * menu.s
                        font.weight: Font.DemiBold
                        style: Text.Raised
                        styleColor: "#80000000"
                    }
                    MouseArea {
                        width: 70 * menu.s
                        height: 70 * menu.s
                        cursorShape: Qt.PointingHandCursor
                        onClicked: menu.setCategory(catItem.index)
                    }
                }
            }
        }

        // ---- an opened playlist: its cover to the left of its tracks ----
        ArtCover {
            cacheKey: "overlay"   // its own image copies (ArtCover.imageUrl)
            visible: menu.sub !== null
            x: menu.anchorX - width - 34 * menu.s
            y: menu.rowY + menu.rowH
            width: 86 * menu.s
            height: width
            radius: 10 * menu.s
            colorA: menu.sub ? menu.sub.colorA : Theme.accent
            colorB: menu.sub ? menu.sub.colorB : Theme.accent2
            seed: menu.sub ? menu.sub.seed : 0
            source: menu.sub ? menu.sub.cover : ""
            opacity: 0.55
        }

        // ---- the items: the selected one right under the row, the ones before it above the row ----
        ListView {
            id: list
            x: menu.anchorX
            y: 0
            width: parent.width - x - 80 * menu.s
            height: parent.height
            model: menu.items.length
            interactive: false
            clip: false
            cacheBuffer: 0
            highlightRangeMode: ListView.StrictlyEnforceRange
            preferredHighlightBegin: menu.rowY + menu.rowH
            preferredHighlightEnd: menu.rowY + menu.rowH + menu.itemH
            highlightMoveDuration: 220
            highlightFollowsCurrentItem: true
            currentIndex: menu.selection
            onCountChanged: currentIndex = Qt.binding(() => menu.selection)
            // A new list (another category, a playlist opened) slides in.
            property string key: menu.cat + "/" + (menu.sub ? menu.sub.index : "")
            onKeyChanged: enter.restart()
            NumberAnimation { id: enter; target: list; property: "opacity"; from: 0; to: 1; duration: Theme.ms(260); easing.type: Easing.OutCubic }

            delegate: Item {
                id: entry
                required property int index
                readonly property var item: menu.items[index] || ({})
                readonly property bool current: index === list.currentIndex
                readonly property bool above: index < list.currentIndex
                width: list.width
                height: menu.itemH
                // The further from the selection, the fainter (above the row most of all).
                readonly property int distance: Math.abs(index - list.currentIndex)
                opacity: (item.disabled ? 0.4 : current ? 1 : above ? 0.36 : 0.62) * Math.max(0.15, 1 - Math.max(0, distance - 2) * (above ? 0.3 : 0.12))
                Behavior on opacity { NumberAnimation { duration: Theme.ms(200) } }
                transform: Translate {
                    y: entry.above ? -menu.rowH - menu.itemH * 0.4 : 0
                    Behavior on y { NumberAnimation { duration: Theme.ms(220); easing.type: Easing.OutCubic } }
                }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 20 * menu.s
                    scale: entry.current ? 1 : 0.86
                    transformOrigin: Item.Left
                    Behavior on scale { NumberAnimation { duration: Theme.ms(220); easing.type: Easing.OutCubic } }

                    Item {
                        width: 66 * menu.s
                        height: width
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property bool art: entry.item.kind === "now" || entry.item.kind === "track"
                                                    || (entry.item.kind === "playlist" && entry.item.cover !== "")
                        ArtCover {
                            cacheKey: "overlay"   // its own image copies (ArtCover.imageUrl)
                            anchors.fill: parent
                            visible: parent.art
                            radius: 8 * menu.s
                            colorA: entry.item.colorA || Theme.accent
                            colorB: entry.item.colorB || Theme.accent2
                            seed: entry.item.seed || 0
                            source: parent.art ? entry.item.cover || "" : ""
                        }
                        Icon {
                            anchors.centerIn: parent
                            visible: !parent.art
                            name: entry.item.icon || "music"
                            size: 40 * menu.s
                            color: Theme.text
                        }
                        // Play / pause over the cover of what's playing
                        Rectangle {
                            visible: entry.item.kind === "now" && Player.hasTrack
                            anchors.fill: parent
                            radius: 8 * menu.s
                            color: "#66000000"
                            Icon {
                                anchors.centerIn: parent
                                name: Player.playing ? "pause" : "play"
                                size: 32 * menu.s
                                color: "#ffffff"
                            }
                        }
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4 * menu.s
                        width: list.width - 110 * menu.s
                        Text {
                            width: parent.width
                            text: entry.item.title || ""
                            color: Theme.text
                            font.family: Theme.fontDisplay
                            font.pixelSize: 26 * menu.s
                            font.weight: entry.current ? Font.DemiBold : Font.Normal
                            elide: Text.ElideRight
                            style: Text.Raised
                            styleColor: "#80000000"
                        }
                        Text {
                            width: parent.width
                            visible: text !== ""
                            text: entry.item.subtitle || ""
                            color: Theme.textDim
                            font.pixelSize: 17 * menu.s
                            elide: Text.ElideRight
                        }
                        // What's playing: how far it is
                        Rectangle {
                            visible: entry.item.kind === "now" && Player.hasTrack && Player.duration > 0
                            width: 320 * menu.s
                            height: 4 * menu.s
                            radius: height / 2
                            color: "#33ffffff"
                            Rectangle {
                                width: parent.width * Math.min(1, Player.position / Math.max(1, Player.duration))
                                height: parent.height
                                radius: parent.radius
                                color: Theme.text
                            }
                        }
                        // The volume while it's being changed
                        Row {
                            visible: entry.item.kind === "volume" && menu.adjusting && entry.current
                            spacing: 10 * menu.s
                            Text { text: "◀"; color: Theme.textDim; font.pixelSize: 14 * menu.s; anchors.verticalCenter: parent.verticalCenter }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 320 * menu.s
                                height: 6 * menu.s
                                radius: height / 2
                                color: "#33ffffff"
                                Rectangle {
                                    width: parent.width * Player.volume
                                    height: parent.height
                                    radius: parent.radius
                                    color: Theme.text
                                }
                            }
                            Text { text: "▶"; color: Theme.textDim; font.pixelSize: 14 * menu.s; anchors.verticalCenter: parent.verticalCenter }
                        }
                    }
                }

                MouseArea {
                    width: Math.min(parent.width, 700 * menu.s)
                    height: parent.height
                    cursorShape: Qt.PointingHandCursor
                    onClicked: entry.current ? menu.activate(entry.item) : menu.select(entry.index)
                }
            }
        }

        // ---- bottom: how to use it (over a darker band, so the list fades out under it) ----
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 190 * menu.s
            gradient: Gradient {
                GradientStop { position: 0; color: "#00000000" }
                GradientStop { position: 0.6; color: "#b0000000" }
                GradientStop { position: 1; color: "#d0000000" }
            }
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 40 * menu.s
            spacing: 30 * menu.s
            Repeater {
                model: GameOverlay.padConnected
                    ? [["A", qsTr("Select")], ["B", qsTr("Back")], ["Y", qsTr("Play / pause")], ["LB RB", qsTr("Previous / next track")],
                       ["View + Menu", qsTr("Close")]]
                    : [["← →", qsTr("Categories")], ["↑ ↓", qsTr("Choose")], ["Enter", qsTr("Select")], ["Esc", qsTr("Back")]]
                      .concat(GameOverlay.hotkey !== "" ? [[GameOverlay.hotkey, qsTr("Close")]] : [])   // unknown when the desktop binds it
                Row {
                    required property var modelData
                    spacing: 9 * menu.s
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: keyText.implicitWidth + 16 * menu.s
                        height: 26 * menu.s
                        radius: 6 * menu.s
                        color: "#26ffffff"
                        border.color: "#40ffffff"
                        border.width: 1
                        Text {
                            id: keyText
                            anchors.centerIn: parent
                            text: modelData[0]
                            color: Theme.text
                            font.pixelSize: 13 * menu.s
                            font.weight: Font.DemiBold
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData[1]
                        color: Theme.textDim
                        font.pixelSize: 15 * menu.s
                    }
                }
            }
        }
    }
}
