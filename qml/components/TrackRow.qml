import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Everplay

// Track row in lists: number ↔ play ↔ equalizer, cover, title, album, source, like, duration.
Item {
    id: root
    property int trackIndex: 0
    property int number: trackIndex + 1
    property int revealDelay: 0
    property bool animateIn: true   // false: shown at once (e.g. in a list built after its page appeared)
    property bool showAlbum: true
    property bool compact: false   // narrow grid cells: no album and source columns
    property var queue: null   // rows that become the queue on click (null = the whole library)
    property string context: "" // queue origin ("playlist:…", "album:…") — the mix depends on it

    function play() {
        if (current) Player.toggle()
        else if (queue) Player.playFrom(queue, trackIndex, context)
        else Player.playIndex(trackIndex)
    }

    // The playlist this row is shown in (Library.playlists index) — enables "Remove from this playlist".
    property int playlistIndex: -1

    // Track menu: add to a playlist, remove from this one, share links and the listening room,
    // the YouTube Music match of a Spotify track.
    readonly property int menuWidth: 320
    function openMenu(px, py) {
        if (t.demo)
            return false
        const menu = menuComponent.createObject(root, { x: px, y: py })
        menu.open()
        return true
    }

    readonly property var t: {
        if (Library.tracks.revision < 0)   // never: the read makes the binding re-run (a bare read is compiled away)
            return ({})
        return Library.tracks.get(trackIndex)
    }
    readonly property bool current: Player.currentIndex === trackIndex
    readonly property bool hovered: hover.hovered

    width: parent ? parent.width : 600
    implicitHeight: 60
    opacity: animateIn ? 0 : 1
    transform: Translate { id: shift; y: root.animateIn ? 16 : 0 }

    SequentialAnimation {
        id: reveal
        PauseAnimation { duration: Theme.ms(root.revealDelay) }
        ParallelAnimation {
            NumberAnimation { target: root; property: "opacity"; to: 1; duration: Theme.ms(420); easing.type: Easing.OutCubic }
            NumberAnimation { target: shift; property: "y"; to: 0; duration: Theme.ms(620); easing.type: Easing.OutQuint }
        }
    }
    Component.onCompleted: if (animateIn) reveal.start()

    HoverHandler { id: hover }

    // The cursor rests on a YouTube track for a moment — we prepare its URL before the click lands
    // (the delay keeps fast mouse sweeps over the list from starting yt-dlp for every row).
    Timer {
        interval: 200
        running: hover.hovered && !root.current
        onTriggered: Player.prefetch(root.trackIndex)
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: mouse => { if (mouse.button === Qt.RightButton) root.openMenu(mouse.x, mouse.y) }
        onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) root.current ? Player.play() : root.play() }
    }

    Rectangle {
        anchors.fill: parent
        radius: Theme.r(14)
        color: root.current ? Theme.alpha(Theme.accent, 0.14) : Theme.alpha(Theme.text, 0.06)
        border.width: root.current ? 1 : 0
        border.color: Theme.alpha(Theme.accent, 0.22)
        opacity: root.current || root.hovered ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.normal } }
        Behavior on color { ColorAnimation { duration: Theme.normal } }
    }

    Rectangle {
        visible: root.current
        anchors { left: parent.left; leftMargin: 3; verticalCenter: parent.verticalCenter }
        width: 3
        height: parent.height * (0.36 + Player.level * 0.3)
        radius: 1.5
        color: Theme.accent
        Behavior on height { NumberAnimation { duration: 90 } }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        transform: Translate {
            x: root.hovered && !root.current ? 4 : 0
            Behavior on x { NumberAnimation { duration: Theme.ms(280); easing.type: Easing.OutCubic } }
        }
        anchors.rightMargin: 10
        spacing: root.compact ? 10 : 14

        Item {
            Layout.preferredWidth: 26
            Layout.fillHeight: true

            Text {
                anchors.centerIn: parent
                text: root.number
                color: Theme.textFaint
                font.pixelSize: 14
                font.features: { "tnum": 1 }
                opacity: root.hovered || root.current ? 0 : 1
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            }
            // Controls that only show for the playing / hovered row are created on demand — lists
            // build many rows at once, and every item counts.
            Loader {
                anchors.centerIn: parent
                active: root.current && !root.hovered
                sourceComponent: EqualizerIcon { running: Player.playing }
            }
            Loader {
                anchors.centerIn: parent
                active: root.hovered
                sourceComponent: IconButton {
                    width: 34; height: 34
                    icon: root.current && Player.playing ? "pause" : "play"
                    iconSize: 18
                    color: Theme.text
                    showDot: false
                    onClicked: root.play()
                }
            }
        }

        ArtCover {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            radius: Theme.r(10)
            colorA: root.t.colorA
            colorB: root.t.colorB
            seed: root.t.seed
            source: root.t.cover || ""
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            // The title, with a bolt right after it when the track is kept on disk (the audio cache):
            // it plays at once, even offline. A long title is cut to leave the bolt room.
            Item {
                Layout.fillWidth: true
                implicitHeight: titleText.implicitHeight
                Text {
                    id: titleText
                    width: Math.min(implicitWidth, parent.width - (bolt.visible ? 19 : 0))
                    text: root.t.title
                    color: root.current ? Theme.accent : Theme.text
                    font.pixelSize: 15
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Behavior on color { ColorAnimation { duration: Theme.normal } }
                }
                Icon {
                    id: bolt
                    x: titleText.width + 4
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Player.cacheRevision >= 0 && Player.cached(root.trackIndex)
                    name: "bolt"
                    size: 15
                    color: Theme.warning
                }
            }
            ArtistLink {
                Layout.fillWidth: true
                text: root.t.artist
                artistId: root.t.artistId || ""
                linkEnabled: !root.t.demo
                font.pixelSize: 13
                elide: Text.ElideRight
            }
        }

        Text {
            visible: root.showAlbum && !root.compact && root.width > 680   // narrow rows keep the room for the title
            Layout.preferredWidth: root.width * 0.22
            text: root.t.album
            color: Theme.textDim
            font.pixelSize: 13
            elide: Text.ElideRight
        }

        SourceBadge {
            // Narrow grids (the home page's picks) have no source column: just its icon.
            visible: root.compact && root.t.source !== undefined && root.t.source !== "local"
            Layout.alignment: Qt.AlignVCenter
            plain: true
            showLabel: false
            source: root.t.source || "local"
        }

        Item {
            visible: !root.compact && root.width > 860   // the title comes first in narrower lists
            Layout.preferredWidth: 132
            Layout.fillHeight: true
            SourceBadge {
                anchors.verticalCenter: parent.verticalCenter
                plain: true
                source: root.t.source
            }
        }

        Loader {
            Layout.preferredWidth: 38
            Layout.preferredHeight: 38
            active: root.t.liked || root.hovered
            visible: !root.compact || active   // compact rows keep the room for the title
            sourceComponent: IconButton {
                icon: root.t.liked ? "heart" : "heartOutline"
                iconSize: 18
                active: root.t.liked
                showDot: false
                onClicked: {
                    Library.tracks.toggleLiked(root.trackIndex)
                    Nav.toast(root.t.liked ? qsTr("Added to Liked") : qsTr("Removed from Liked"), "heart")
                }
            }
        }

        Text {
            visible: !root.compact || !root.hovered   // the buttons take its place
            Layout.preferredWidth: 52
            horizontalAlignment: Text.AlignRight
            text: root.t.durationText
            color: root.t.duration > 0 ? Theme.textDim : Theme.warning
            font.pixelSize: root.t.duration > 0 ? 13 : 11
            font.weight: root.t.duration > 0 ? Font.Normal : Font.DemiBold
            font.features: { "tnum": 1 }
        }

        Loader {
            id: moreButton
            visible: !root.compact || active
            Layout.preferredWidth: 38
            Layout.preferredHeight: 38
            active: root.hovered
            sourceComponent: IconButton {
                icon: "more"
                iconSize: 18
                showDot: false
                onClicked: {
                    const p = moreButton.mapToItem(root, moreButton.width, moreButton.height)
                    root.openMenu(p.x - menuWidth, p.y + 4)
                }
            }
        }
    }

    Component {
        id: menuComponent
        Popup {
            id: menu
            width: root.menuWidth
            padding: 6
            margins: 10
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            background: Rectangle {
                radius: Theme.r(14)
                color: Theme.surfaceHi
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.07)
            }
            enter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.ms(140) }
                NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.ms(220); easing.type: Easing.OutCubic }
            }
            exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.ms(120) } }
            onClosed: destroy()

            property bool choosingPlaylist: false
            readonly property bool openSource: root.t.source === "archive" || root.t.source === "commons"

            contentItem: ColumnLayout {
                spacing: 2

                MenuEntry {
                    visible: !Player.inRoom && root.t.source !== "radio"
                    Layout.fillWidth: true
                    icon: "radio"
                    text: qsTr("Start radio")
                    onTriggered: {
                        menu.close()
                        Player.startRadio(root.trackIndex)
                    }
                }

                // ---- playlists ----
                MenuEntry {
                    Layout.fillWidth: true
                    icon: "add"
                    text: qsTr("Add to playlist")
                    onTriggered: menu.choosingPlaylist = !menu.choosingPlaylist
                }
                ColumnLayout {
                    visible: menu.choosingPlaylist
                    Layout.fillWidth: true
                    Layout.leftMargin: 14
                    spacing: 2
                    MenuEntry {
                        Layout.fillWidth: true
                        icon: "add"
                        text: qsTr("New playlist…")
                        onTriggered: {
                            menu.close()
                            Nav.createPlaylist([root.trackIndex])
                        }
                    }
                    Repeater {
                        model: menu.choosingPlaylist ? Playlists.targets().filter(p => p.index !== root.playlistIndex) : []
                        MenuEntry {
                            required property var modelData
                            Layout.fillWidth: true
                            icon: modelData.kind === "ytplaylist" ? "playCircle" : "music"
                            text: modelData.name
                            onTriggered: {
                                menu.close()
                                Playlists.addRows(modelData.index, [root.trackIndex])
                            }
                        }
                    }
                }
                MenuEntry {
                    visible: root.playlistIndex >= 0 && Playlists.canEdit(root.playlistIndex)
                    Layout.fillWidth: true
                    icon: "minus"
                    text: qsTr("Remove from this playlist")
                    onTriggered: {
                        menu.close()
                        Playlists.removeRow(root.playlistIndex, root.trackIndex)
                    }
                }

                // ---- online ----
                MenuEntry {
                    Layout.fillWidth: true
                    icon: "share"
                    text: qsTr("Copy a share link")
                    onTriggered: {
                        menu.close()
                        Online.shareTrack(root.trackIndex)
                    }
                }
                MenuEntry {
                    visible: Online.inRoom && Online.room.canControl === true
                    Layout.fillWidth: true
                    icon: "group"
                    text: qsTr("Add to the listening room")
                    onTriggered: {
                        menu.close()
                        Online.addToRoom([root.trackIndex])
                    }
                }
                MenuEntry {
                    visible: Online.inRoom && Online.room.canControl === true
                    Layout.fillWidth: true
                    icon: "queue"
                    text: qsTr("Play next in the room")
                    onTriggered: {
                        menu.close()
                        Online.addToRoom([root.trackIndex], true)
                    }
                }
                MenuEntry {
                    // this track and the ones after it in the list it's in (a playlist, an album)
                    readonly property int at: root.queue ? root.queue.indexOf(root.trackIndex) : -1
                    visible: Online.inRoom && Online.room.canControl === true && at >= 0 && at < root.queue.length - 1
                    Layout.fillWidth: true
                    icon: "queue"
                    text: qsTr("Add to the room from here on")
                    onTriggered: {
                        menu.close()
                        Online.addToRoom(root.queue.slice(at))
                    }
                }

                // ---- Spotify: the YouTube Music version it plays from ----
                Rectangle {
                    visible: root.t.source === "spotify"
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 2
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.07)
                }
                Text {
                    visible: root.t.source === "spotify"
                    Layout.fillWidth: true
                    Layout.margins: 10
                    Layout.bottomMargin: 4
                    text: Spotify.manualMatch(root.t.id) ? qsTr("Plays the version you chose on YouTube Music")
                                                         : qsTr("Plays from YouTube Music — matched automatically")
                    color: Theme.textFaint
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                MenuEntry {
                    visible: root.t.source === "spotify"
                    Layout.fillWidth: true
                    icon: "search"
                    text: qsTr("Wrong match — choose another")
                    onTriggered: {
                        menu.close()
                        Nav.chooseMatch(root.t)
                    }
                }
                MenuEntry {
                    visible: root.t.source === "spotify"
                    Layout.fillWidth: true
                    icon: "refresh"
                    text: qsTr("Match again")
                    onTriggered: {
                        menu.close()
                        Spotify.rematch(root.t.id)
                    }
                }

                // ---- the open FLAC sources: the file itself and the licence it's shared under ----
                Rectangle {
                    visible: menu.openSource
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 2
                    height: 1
                    color: Qt.rgba(1, 1, 1, 0.07)
                }
                Text {
                    visible: menu.openSource && OpenMusic.license(root.t.id) !== ""
                    Layout.fillWidth: true
                    Layout.margins: 10
                    Layout.bottomMargin: 4
                    text: qsTr("Licence: %1").arg(OpenMusic.license(root.t.id))
                    color: Theme.textFaint
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                MenuEntry {
                    visible: menu.openSource
                    enabled: !OpenMusic.downloading(root.t.id)
                    Layout.fillWidth: true
                    icon: "download"
                    text: OpenMusic.downloading(root.t.id) ? qsTr("Saving to your library…") : qsTr("Save the FLAC file to your library")
                    onTriggered: {
                        menu.close()
                        OpenMusic.download(root.t.id)
                    }
                }
                MenuEntry {
                    visible: menu.openSource
                    Layout.fillWidth: true
                    icon: "link"
                    text: root.t.source === "archive" ? qsTr("Open on archive.org") : qsTr("Open on Wikimedia Commons")
                    onTriggered: {
                        menu.close()
                        Qt.openUrlExternally(OpenMusic.pageUrl(root.t.id))
                    }
                }
            }
        }
    }
}
