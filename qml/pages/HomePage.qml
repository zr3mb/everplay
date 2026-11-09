import QtQuick
import QtQuick.Effects
import QtQuick.Layouts
import Everplay

Item {
    id: page
    property var args: ({})

    // The built-in lists, next to "Continue listening"; { p, index } with the Library.playlists index.
    readonly property var shortcuts: {
        const out = []
        Library.playlists.forEach((p, i) => {
            if (["liked", "recent", "history", "youtube"].includes(p.kind))
                out.push({ p: p, index: i })
        })
        return out.concat([
            { p: { name: qsTr("All tracks"), kind: "all", icon: "library", colorA: "#a3b3c1", colorB: "#3b4a57", seed: 0 }, index: -1 },
            { p: { name: qsTr("Random mix"), kind: "mix", icon: "shuffle", colorA: "#b7aac9", colorB: "#463c5c", seed: 0 }, index: -1 },
            { p: { name: qsTr("Night Jazz — live"), kind: "radio", icon: "live", colorA: "#b9a889", colorB: "#4a4232", seed: 0 }, index: -1 }
        ])
    }
    // Whole playlists at the end of the page, in their folders (Library.playlistGroups): { name, folder, items: [{ p, index }] }.
    readonly property var playlistSections: Library.playlistGroups
        .filter(g => g.key !== "builtin" && g.items.length > 0)
        .map(g => ({ name: g.name, folder: !!g.folderId, items: g.items.map(i => ({ p: Library.playlists[i], index: i })) }))

    // Picked from YouTube Music radios of the tracks that say the most about the taste (Recommendations).
    readonly property var recommended: {
        // The picks are appended to the model, not rebuilt into it. (Never true: the reads make the binding re-run.)
        if (Library.tracks.revision < 0 || YouTube.resultsRevision < 0)
            return []
        // The new artists have their own section below.
        const fresh = new Set(Library.newArtistRows())
        return Library.recommendedRows().filter(r => !fresh.has(r))
    }
    property bool allRecommended: false
    // The picks by artists new to the user, also mixed into "Recommended for you".
    readonly property var newArtists: {
        if (Library.tracks.revision < 0 || YouTube.resultsRevision < 0)
            return []
        return Library.newArtistRows()
    }
    readonly property var radioSeeds: Library.tracks.revision >= 0 ? Recommendations.radioSeeds : []

    function rowsOf(p, index) {
        if (p.kind === "radio") return Library.search("", "radio", 1)
        if (p.kind === "all" || p.kind === "mix") return Library.localTracks()
        return Library.playlistRows(index)
    }

    function playPick(p, index) {
        const rows = rowsOf(p, index)
        if (rows.length === 0) {
            Nav.toast(Library.localCount === 0 ? qsTr("Add a music folder first") : qsTr("This playlist is still empty"), "music")
            return
        }
        if (p.kind === "mix")
            Player.playShuffled(rows)
        else
            Player.playFrom(rows, rows[0])
    }

    function openPick(p, index) {
        if (p.kind === "all") Nav.navigate("library")
        else if (p.kind === "mix" || p.kind === "radio") playPick(p, index)
        else Nav.navigate("playlist", { kind: "playlist", index: index })
    }

    component SourceMini: Item {
        id: mini
        required property var modelData
        required property int index
        readonly property bool isLocal: modelData.id === "local"
        readonly property string status: modelData.id === "youtube"
                                         ? (YouTube.connected ? "connected" : YouTube.loggingIn ? "connecting" : "disconnected")
                                         : modelData.id === "spotify"
                                         ? (Spotify.connected ? "connected" : Spotify.loggingIn ? "connecting" : "disconnected")
                                         : modelData.id === "soundcloud"
                                         ? (SoundCloud.connected ? "connected" : SoundCloud.loggingIn ? "connecting" : "available")
                                         : isLocal ? (Library.folders.length > 0 ? "connected" : "disconnected")
                                         : modelData.status

        Layout.fillWidth: true
        Layout.preferredWidth: 100
        implicitHeight: 84

        HoverHandler { id: miniHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: Nav.navigate("sources") }

        Rectangle {
            anchors.fill: parent
            radius: Theme.r(16)
            color: Theme.alpha(Theme.text, miniHover.hovered ? 0.075 : 0.045)
            border.width: 1
            border.color: Theme.alpha(mini.modelData.color, miniHover.hovered ? 0.3 : 0.08)
            Behavior on color { ColorAnimation { duration: Theme.normal } }
            Behavior on border.color { ColorAnimation { duration: Theme.normal } }
        }

        Rectangle {
            id: miniIcon
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: 46; height: 46; radius: Theme.r(14)
            color: Theme.alpha(mini.modelData.color, 0.15)
            scale: miniHover.hovered ? 1.06 : 1
            rotation: miniHover.hovered ? -5 : 0
            Behavior on scale { NumberAnimation { duration: Theme.ms(320); easing.type: Easing.OutBack } }
            Behavior on rotation { NumberAnimation { duration: Theme.ms(320); easing.type: Easing.OutBack } }
            Icon {
                anchors.centerIn: parent
                name: mini.modelData.icon
                size: 22
                color: mini.modelData.color
            }
        }

        Column {
            anchors { left: miniIcon.right; leftMargin: 14; right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
            spacing: 4
            Text {
                width: parent.width
                text: mini.modelData.name
                color: Theme.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Row {
                spacing: 6
                Text {
                    text: mini.isLocal
                          ? (Library.scanning ? qsTr("Scanning…")
                             : Library.folders.length > 0 ? qsTr("%n tracks", "", Library.localCount)
                             : qsTr("Add folder →"))
                          : mini.modelData.id === "youtube" && YouTube.connected
                            ? qsTr("%n liked", "", YouTube.likedCount)
                          : mini.modelData.id === "soundcloud" && SoundCloud.connected
                            ? qsTr("%n liked", "", SoundCloud.likedCount)
                          : mini.status === "connected" ? qsTr("Connected")
                          : mini.status === "disconnected" ? qsTr("Sign in →") : mini.status === "connecting" ? qsTr("Signing in…") : qsTr("Ready to use")
                    color: Theme.textDim
                    font.pixelSize: 12
                }
            }
        }
    }

    // A built-in list on the home page: the cover at the left edge, the name, a Play button on hover.
    component QuickPick: Item {
        id: pick
        required property var modelData
        required property int index
        readonly property var p: modelData.p

        opacity: 0
        transform: [
            Scale { id: pickScale; origin.x: pick.width / 2; origin.y: pick.height / 2; xScale: 0.9; yScale: 0.9 },
            Translate { y: pickHover.hovered ? -3 : 0; Behavior on y { NumberAnimation { duration: Theme.ms(280); easing.type: Easing.OutCubic } } }
        ]
        SequentialAnimation {
            running: true
            PauseAnimation { duration: Theme.ms(200 + pick.index * 60) }
            ParallelAnimation {
                NumberAnimation { target: pick; property: "opacity"; to: 1; duration: Theme.ms(420); easing.type: Easing.OutCubic }
                NumberAnimation { target: pickScale; properties: "xScale,yScale"; to: 1; duration: Theme.ms(700); easing.type: Easing.OutBack }
            }
        }
        signal clicked
        signal playClicked

        Layout.fillWidth: true
        Layout.preferredWidth: 100
        implicitHeight: 72

        HoverHandler { id: pickHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: pick.clicked() }

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusM + 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.alpha(pick.p.colorA || Theme.accent, pickHover.hovered ? 0.32 : 0.2) }
                GradientStop { position: 1; color: Theme.alpha(Theme.text, pickHover.hovered ? 0.07 : 0.04) }
            }
            border.width: 1
            border.color: pickHover.hovered ? Theme.alpha(pick.p.colorA || Theme.accent, 0.4) : Theme.hairline
            scale: pickHover.hovered ? 1.015 : 1
            Behavior on scale { NumberAnimation { duration: Theme.ms(260); easing.type: Easing.OutCubic } }
        }

        ArtCover {
            id: pickCover
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 52; height: 52
            radius: Theme.r(12)
            decorated: false
            colorA: pick.p.colorA || Theme.accent
            colorB: pick.p.colorB || Theme.accent2
            seed: pick.p.seed || 0
            source: pick.p.cover || ""
            mosaic: pick.p.mosaic || []
            Icon {
                anchors.centerIn: parent
                visible: !pickCover.hasPicture
                name: pick.p.icon || "music"
                size: 22
                color: "#ffffff"
            }
        }
        Text {
            anchors { left: pickCover.right; leftMargin: 14; right: pickPlay.left; rightMargin: 10; verticalCenter: parent.verticalCenter }
            text: pick.p.name || ""
            color: Theme.text
            font.pixelSize: 15
            font.weight: Font.Bold
            elide: Text.ElideRight
        }
        PlayFab {
            id: pickPlay
            anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
            size: 38
            opacity: pickHover.hovered ? 1 : 0
            scale: pickHover.hovered ? 1 : 0.7
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            Behavior on scale { NumberAnimation { duration: Theme.ms(300); easing.type: Easing.OutBack } }
            onClicked: pick.playClicked()
        }
    }

    // The shadow shared by the album cards
    CardShadow {
        id: albumShadow
        size: albumGrid.cellW
    }
    CardShadow {
        id: radioShadow
        size: radioGrid.cellW
    }

    PageFlick {
        anchors.fill: parent

        // Cat theme: a neon banner above the greeting
        Reveal {
            visible: Theme.cat
            Rectangle {
                id: banner
                width: parent.width
                height: 168
                radius: Theme.r(22)
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.alpha(Theme.accent2, 0.5) }
                    GradientStop { position: 0.55; color: Theme.alpha(Theme.accent, 0.2) }
                    GradientStop { position: 1.0; color: Theme.alpha(Theme.accent, 0.06) }
                }
                border.width: 1
                border.color: Theme.alpha(Theme.accent, 0.3)

                Repeater {
                    model: [
                        { x: 0.62, y: 0.18, s: 40, i: "heartOutline", r: 12 },
                        { x: 0.74, y: 0.52, s: 26, i: "heart", r: -10 },
                        { x: 0.83, y: 0.14, s: 20, i: "sparkle", r: 0 },
                        { x: 0.90, y: 0.46, s: 52, i: "cat", r: 8 },
                        { x: 0.68, y: 0.74, s: 16, i: "sparkle", r: 0 }
                    ]
                    Icon {
                        required property var modelData
                        x: banner.width * modelData.x
                        y: banner.height * modelData.y
                        size: modelData.s
                        name: modelData.i
                        rotation: modelData.r
                        color: Theme.accent
                        opacity: 0.55
                    }
                }

                Column {
                    anchors { left: parent.left; leftMargin: 34; verticalCenter: parent.verticalCenter }
                    spacing: 2
                    Row {
                        spacing: 10
                        Text {
                            id: bannerTitle
                            text: "Everplay"
                            color: Qt.lighter(Theme.accent, 1.12)
                            font.family: Theme.fontDisplay
                            font.pixelSize: 64
                            font.weight: Font.Black
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                shadowEnabled: true
                                shadowColor: Theme.accent
                                shadowBlur: 0.8
                                shadowOpacity: 0.9
                                shadowHorizontalOffset: 0
                                shadowVerticalOffset: 0
                            }
                        }
                        Icon {
                            anchors.verticalCenter: bannerTitle.verticalCenter
                            anchors.verticalCenterOffset: -10
                            name: "heartOutline"
                            size: 30
                            rotation: 14
                            color: Theme.accent
                        }
                    }
                    Text {
                        leftPadding: 4
                        text: qsTr("Your music · Your rules")
                        color: Theme.text
                        font.pixelSize: 17
                        font.weight: Font.Medium
                    }
                }
            }
        }

        Reveal {
            ColumnLayout {
                width: parent.width
                spacing: 2
                Text {
                    text: new Date().toLocaleDateString(Qt.locale(), "dddd, d MMMM").toUpperCase()
                    color: Theme.accent
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    font.letterSpacing: 1.8
                }
                Text {
                    text: Theme.greeting()
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 48
                    font.weight: Font.Bold
                    font.letterSpacing: -1.4
                }
                Text {
                    visible: Theme.cat
                    text: qsTr("Back for more music? ♡")
                    color: Theme.textDim
                    font.pixelSize: 15
                }
            }
        }

        Reveal {
            delay: 70
            HeroCard { width: parent.width }
        }

        // The built-in lists as colour tiles, three to a row
        Reveal {
            delay: 110
            GridLayout {
                id: shortcutGrid
                width: parent.width
                columns: width > 980 ? 3 : 2
                columnSpacing: 14
                rowSpacing: 14
                Repeater {
                    model: page.shortcuts
                    QuickPick {
                        onClicked: page.openPick(modelData.p, modelData.index)
                        onPlayClicked: page.playPick(modelData.p, modelData.index)
                    }
                }
            }
        }

        // Radios of what the user listens to most lately: one click and similar songs play on and on.
        Reveal {
            objectName: "homeRadios"
            delay: 130
            visible: page.radioSeeds.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 18
                SectionHeader {
                    Layout.fillWidth: true
                    title: qsTr("Radios for you")
                    subtitle: qsTr("Similar songs without end, the ones you haven't heard first")
                }
                Grid {
                    id: radioGrid
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    columns: Math.max(3, Math.floor((width + columnSpacing) / 180))
                    columnSpacing: 22
                    rowSpacing: 22
                    readonly property real cellW: (width - columnSpacing * (columns - 1)) / columns

                    Repeater {
                        model: page.radioSeeds.slice(0, radioGrid.columns)
                        AlbumCard {
                            required property var modelData
                            required property int index
                            readonly property bool here: Player.queueContext === "radio:" + modelData.id
                            width: radioGrid.cellW
                            shadow: radioShadow
                            title: qsTr("%1 radio").arg(modelData.artist)
                            source: modelData.source || ""
                            subtitle: qsTr("From %1").arg(modelData.title)
                            colorA: modelData.colorA
                            colorB: modelData.colorB
                            seed: modelData.seed
                            cover: modelData.cover
                            playing: Player.playing && here
                            revealDelay: 200 + index * 50
                            onClicked: here ? Player.toggle() : Player.startRadio(modelData.row)
                            onPlayClicked: here ? Player.toggle() : Player.startRadio(modelData.row)
                        }
                    }
                }
            }
        }

        Reveal {
            delay: 150
            visible: page.recommended.length > 0 || Recommendations.loading
            ColumnLayout {
                width: parent.width
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Recommended for you")
                            color: Theme.text
                            font.family: Theme.fontDisplay
                            font.pixelSize: 22
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: Recommendations.basedOn.length > 0
                                  ? qsTr("Because you listen to %1").arg(Recommendations.basedOn.slice(0, 3).join(", "))
                                  : qsTr("Looking for music that fits your taste…")
                            color: Theme.textDim
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                    }
                    Spinner {
                        visible: Recommendations.loading
                        size: 18
                        color: Theme.textDim
                    }
                    IconButton {
                        icon: "refresh"
                        iconSize: 20
                        implicitWidth: 38; implicitHeight: 38
                        showDot: false
                        enabled: !Recommendations.loading
                        opacity: enabled ? 1 : 0.4
                        onClicked: Recommendations.refresh()
                    }
                    PillButton {
                        kind: "ghost"
                        icon: "play"
                        text: qsTr("Play")
                        fontSize: 13
                        implicitHeight: 38
                        enabled: page.recommended.length > 0
                        onClicked: Player.playFrom(page.recommended, page.recommended[0], "recs")
                    }
                }

                Grid {
                    id: recGrid
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    columns: width > 1100 ? 3 : width > 620 ? 2 : 1
                    columnSpacing: 10
                    readonly property real cellW: (width - columnSpacing * (columns - 1)) / columns
                    readonly property int folded: columns * 4

                    Repeater {
                        model: page.recommended.slice(0, page.allRecommended ? page.recommended.length : recGrid.folded)
                        TrackRow {
                            required property var modelData
                            required property int index
                            width: recGrid.cellW
                            compact: true
                            trackIndex: modelData
                            number: index + 1
                            queue: page.recommended
                            context: "recs"
                            revealDelay: 220 + Math.min(index, 12) * 35
                        }
                    }
                }

                PillButton {
                    visible: page.recommended.length > recGrid.folded
                    kind: "ghost"
                    icon: page.allRecommended ? "" : "expandMore"
                    text: page.allRecommended ? qsTr("Show less") : qsTr("Show more (%1)").arg(page.recommended.length)
                    fontSize: 13
                    onClicked: page.allRecommended = !page.allRecommended
                }
            }
        }

        // Picks by artists the user hasn't heard or saved yet (Recommendations).
        Reveal {
            delay: 170
            visible: page.newArtists.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 14
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    SectionHeader {
                        Layout.fillWidth: true
                        title: qsTr("New artists for you")
                        subtitle: qsTr("Nothing by them in your library or history yet")
                    }
                    PillButton {
                        kind: "ghost"
                        icon: "play"
                        text: qsTr("Play")
                        fontSize: 13
                        implicitHeight: 38
                        onClicked: Player.playFrom(page.newArtists, page.newArtists[0], "recs:new")
                    }
                }
                Grid {
                    id: freshGrid
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    columns: width > 1100 ? 3 : width > 620 ? 2 : 1
                    columnSpacing: 10
                    readonly property real cellW: (width - columnSpacing * (columns - 1)) / columns

                    Repeater {
                        model: page.newArtists.slice(0, freshGrid.columns * 3)
                        TrackRow {
                            required property var modelData
                            required property int index
                            width: freshGrid.cellW
                            compact: true
                            trackIndex: modelData
                            number: index + 1
                            queue: page.newArtists
                            context: "recs:new"
                            revealDelay: 240 + Math.min(index, 12) * 35
                        }
                    }
                }
            }
        }

        Reveal {
            delay: 140
            ColumnLayout {
                width: parent.width
                spacing: 18
                SectionHeader {
                    Layout.fillWidth: true
                    title: qsTr("Your albums")
                    action: qsTr("Show all")
                    onActionClicked: Nav.navigate("library", { tab: 1 })
                }
                Grid {
                    id: albumGrid
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    columns: Math.max(3, Math.floor((width + columnSpacing) / 200))
                    columnSpacing: 22
                    readonly property real cellW: (width - columnSpacing * (columns - 1)) / columns

                    Repeater {
                        model: Library.recentAlbums.slice(0, albumGrid.columns)
                        AlbumCard {
                            required property var modelData
                            required property int index
                            width: albumGrid.cellW
                            shadow: albumShadow
                            title: modelData.title
                            subtitle: modelData.artist + (modelData.year > 0 ? " · " + modelData.year : "")
                            colorA: modelData.colorA
                            colorB: modelData.colorB
                            seed: modelData.seed
                            cover: modelData.cover
                            playing: Player.playing && Player.albumKey === modelData.key
                            revealDelay: 320 + index * 60
                            onClicked: Nav.navigate("playlist", { kind: "album", key: modelData.key })
                            onPlayClicked: {
                                if (playing) { Player.toggle(); return }
                                const rows = Library.tracksOfAlbum(modelData.key)
                                Player.playFrom(rows, rows[0])
                            }
                        }
                    }
                }
            }
        }

        Reveal {
            delay: 210
            visible: recentRepeater.count > 0
            ColumnLayout {
                width: parent.width
                spacing: 6
                SectionHeader {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 10
                    title: qsTr("Recently added")
                    action: qsTr("Library")
                    onActionClicked: Nav.navigate("library")
                }
                Repeater {
                    id: recentRepeater
                    model: Library.tracks.revision >= 0 ? Library.recentlyAdded(5) : []
                    TrackRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        trackIndex: modelData
                        queue: Library.recentlyAdded(50)
                        number: index + 1
                        revealDelay: 400 + index * 50
                    }
                }
            }
        }

        Reveal {
            delay: 350
            visible: page.playlistSections.length > 0 || Library.hiddenCount > 0
            ColumnLayout {
                width: parent.width
                spacing: 18
                SectionHeader {
                    Layout.fillWidth: true
                    title: qsTr("Playlists")
                    subtitle: qsTr("Yours, from YouTube Music and Spotify")
                }
                Repeater {
                    model: page.playlistSections
                    ColumnLayout {
                        id: section
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 12
                        RowLayout {
                            spacing: 8
                            Icon { name: section.modelData.folder ? "folder" : "library"; size: 15; color: Theme.textFaint }
                            Text {
                                text: section.modelData.name + "  ·  " + section.modelData.items.length
                                color: Theme.textDim
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }
                        }
                        Grid {
                            id: playlistGrid
                            Layout.fillWidth: true
                            Layout.preferredWidth: 100
                            columns: Math.max(3, Math.floor((width + columnSpacing) / 200))
                            columnSpacing: 22
                            rowSpacing: 18
                            readonly property real cellW: (width - columnSpacing * (columns - 1)) / columns

                            Repeater {
                                model: section.modelData.items
                                AlbumCard {
                                    required property var modelData
                                    required property int index
                                    width: playlistGrid.cellW
                                    shadow: albumShadow
                                    opacity: modelData.p.hidden ? 0.55 : 1
                                    title: modelData.p.name
                                    source: Nav.playlistSource(modelData.p)
                                    subtitle: modelData.p.count !== undefined && modelData.p.count >= 0
                                              ? qsTr("%n tracks", "", modelData.p.count) : ""
                                    colorA: modelData.p.colorA
                                    colorB: modelData.p.colorB
                                    seed: modelData.p.seed
                                    cover: modelData.p.cover || ""
                                    mosaic: modelData.p.mosaic || []
                                    revealDelay: 380 + Math.min(index, 8) * 50
                                    onClicked: page.openPick(modelData.p, modelData.index)
                                    onPlayClicked: page.playPick(modelData.p, modelData.index)
                                }
                            }
                        }
                    }
                }
                PillButton {
                    visible: Library.hiddenCount > 0
                    kind: "ghost"
                    icon: Library.showHidden ? "close" : "add"
                    text: Library.showHidden ? qsTr("Hide hidden playlists") : qsTr("Show hidden playlists (%1)").arg(Library.hiddenCount)
                    fontSize: 13
                    implicitHeight: 36
                    onClicked: Library.showHidden = !Library.showHidden
                }
            }
        }

        Reveal {
            delay: 420
            ColumnLayout {
                width: parent.width
                spacing: 16
                SectionHeader {
                    Layout.fillWidth: true
                    title: qsTr("Your sources")
                    subtitle: qsTr("Where Everplay gets its music")
                    action: qsTr("Manage")
                    onActionClicked: Nav.navigate("sources")
                }
                // Six sources: one row when there's room, two rows of three otherwise.
                GridLayout {
                    Layout.fillWidth: true
                    columns: width > 1150 ? 6 : 3
                    rowSpacing: 14
                    columnSpacing: 14
                    Repeater {
                        model: Library.sources
                        SourceMini {}
                    }
                }
            }
        }
    }
}
