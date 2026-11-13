import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Everplay

// Playlist, album or artist view. The list is virtualized (ListView), because YouTube Music
// playlists can have thousands of tracks.
Item {
    id: page
    property var args: ({})

    readonly property var info: {
        const a = args || {}
        // Never true: the reads make the binding re-run when they change (a bare read is compiled away).
        // A collection's tracks are appended to the model, not rebuilt into it; genres fill in later.
        if (Library.tracks.revision < 0 || (a.kind === "ytcollection" && YouTube.resultsRevision < 0)
            || (a.kind === "sccollection" && SoundCloud.resultsRevision < 0)
            || (a.kind === "spcollection" && Spotify.artistsRevision < 0)
            || (a.kind === "flaccollection" && OpenMusic.resultsRevision < 0)
                || (a.kind === "genre" && Library.genresRevision < 0))
            return {}
        if (a.kind === "album") {
            const al = Library.albums.find(x => x.key === a.key) || { title: "", artist: "", year: 0 }
            return { label: qsTr("ALBUM"), title: al.title, meta: [al.artist].concat(al.year > 0 ? [String(al.year)] : []),
                     colorA: al.colorA || Theme.accent, colorB: al.colorB || Theme.accent2, seed: al.seed || 0,
                     cover: al.cover || "", icon: "", round: false,
                     rows: Library.tracksOfAlbum(a.key) }
        }
        if (a.kind === "artist") {
            const ar = Library.artists.find(x => x.name === a.artist) || {}
            return { label: qsTr("ARTIST"), title: a.artist, meta: [],
                     colorA: ar.colorA || Theme.accent, colorB: ar.colorB || Theme.accent2, seed: ar.seed || 0,
                     cover: ar.cover || "", icon: "", round: true,
                     rows: Library.tracksOfArtist(a.artist) }
        }
        if (a.kind === "genre") {
            // A genre of the library (the search page): tags, or the artists' genres for streaming tracks
            const g = Library.genres().find(x => x.key === a.key) || { name: a.name || "", seed: 0, cover: "", mosaic: [] }
            return { label: qsTr("GENRE"), title: g.name, meta: [qsTr("From your library")],
                     colorA: g.colorA || Theme.accent, colorB: g.colorB || Theme.accent2, seed: g.seed,
                     cover: g.mosaic.length >= 4 ? "" : g.cover, mosaic: g.mosaic, icon: "", round: false,
                     rows: Library.tracksOfGenre(a.key) }
        }
        if (a.kind === "sccollection") {
            // A playlist found on SoundCloud — loaded from there, not in the library.
            return { label: qsTr("PLAYLIST · SOUNDCLOUD"), title: a.title || "", meta: a.subtitle ? [a.subtitle] : [],
                     colorA: "#d9a27e", colorB: "#5a3a28", seed: 0, cover: a.cover || "", icon: "",
                     round: false, rows: Library.soundCloudCollectionRows(a.playlistId),
                     loading: SoundCloud.collectionLoading(a.playlistId) }
        }
        if (a.kind === "flaccollection") {
            // An album or a concert on the Internet Archive — its FLAC files, loaded from there.
            const c = OpenMusic.collectionInfo(a.playlistId)
            const meta = [a.subtitle || c.creator || ""].filter(x => x !== "")
            if (c.license)
                meta.push(c.license)
            return { label: qsTr("FLAC · INTERNET ARCHIVE"), title: a.title || c.title || "", meta: meta,
                     colorA: "#9cc3c9", colorB: "#2c4a4f", seed: 0, cover: a.cover || "", icon: "",
                     round: false, rows: Library.openMusicCollectionRows(a.playlistId),
                     loading: OpenMusic.collectionLoading(a.playlistId),
                     flacItem: a.playlistId, flacBytes: c.bytes || 0, flacPage: c.page || "" }
        }
        if (a.kind === "spcollection") {
            // An album opened from a Spotify artist — loaded from Spotify, played from YouTube Music.
            return { label: a.label || qsTr("ALBUM · SPOTIFY"), title: a.title || "", meta: a.subtitle ? [a.subtitle] : [],
                     colorA: "#1db954", colorB: "#14532d", seed: 0, cover: a.cover || "", icon: "",
                     round: false, rows: Library.spotifyAlbumRows(a.playlistId),
                     loading: Spotify.albumLoading(a.playlistId) }
        }
        if (a.kind === "ytcollection") {
            // An album / playlist opened from an artist profile — loaded from YouTube Music, not in the library.
            return { label: a.label || qsTr("ALBUM"), title: a.title || "", meta: a.subtitle ? [a.subtitle] : [],
                     colorA: Theme.accent, colorB: Theme.accent2, seed: 0, cover: a.cover || "", icon: "",
                     round: a.round === true, rows: Library.collectionRows(a.playlistId),
                     loading: YouTube.collectionLoading(a.playlistId) }
        }
        const index = a.index || 0
        const p = Library.playlists[index]
        if (!p)
            return { label: qsTr("PLAYLIST"), title: "", meta: [], colorA: Theme.accent, colorB: Theme.accent2,
                     seed: 0, cover: "", icon: "music", round: false, rows: [], loading: false }
        const yt = p.kind === "ytplaylist"
        const sp = p.kind === "spplaylist" || p.kind === "spotifyliked"
        const sc = p.kind === "scplaylist" || p.kind === "scliked"
        const shared = p.kind === "shared"
        const meta = yt ? "YouTube Music"
                   : shared ? (p.description || qsTr("Shared by %1 · %n people add to it", "", p.members).arg(p.owner))
                   : p.kind === "youtube" ? (YouTube.connected ? qsTr("Liked on your YouTube account") : qsTr("Sample tracks from YouTube Music"))
                   : p.kind === "spotifyliked" ? qsTr("Liked on your Spotify account")
                   : p.kind === "scliked" ? qsTr("Liked on your SoundCloud account")
                   : sc ? "SoundCloud"
                   : sp ? qsTr("Spotify — played from YouTube Music")
                   : p.kind === "user" ? (p.description || qsTr("Your playlist on this computer"))
                   : qsTr("Your playlist")
        return { label: yt ? qsTr("PLAYLIST · YOUTUBE MUSIC") : sp ? qsTr("PLAYLIST · SPOTIFY")
                        : shared ? qsTr("SHARED PLAYLIST")
                        : sc ? qsTr("PLAYLIST · SOUNDCLOUD") : qsTr("PLAYLIST"), title: p.name, meta: [meta],
                 colorA: p.colorA, colorB: p.colorB, seed: p.seed, cover: p.cover || "", icon: p.icon, round: false,
                 mosaic: p.mosaic || [], playlistIndex: index, customCover: p.customCover === true,
                 kind: p.kind, editable: Playlists.canEdit(index), sharedId: p.sharedId || "",
                 owner: shared ? Online.isPlaylistOwner(p.sharedId) : true,
                 rows: Library.playlistRows(index), loading: Library.playlistLoading(index) }
    }
    // Queue id for the mix settings (separate for each playlist / album / artist).
    readonly property string context: {
        const a = args || {}
        if (a.kind === "album") return "album:" + a.key
        if (a.kind === "artist") return "artist:" + a.artist
        if (a.kind === "ytcollection") return "ytc:" + a.playlistId
        if (a.kind === "sccollection") return "scc:" + a.playlistId
        if (a.kind === "flaccollection") return "flacc:" + a.playlistId
        if (a.kind === "spcollection") return "spalbum:" + a.playlistId
        if (a.kind === "genre") return "genre:" + a.key
        const p = Library.playlists[a.index || 0]
        if (!p) return ""
        return p.kind === "ytplaylist" ? "yt:" + p.ytId : p.kind === "spplaylist" ? "sp:" + p.spId
             : p.kind === "scplaylist" ? "sc:" + p.scId
             : p.kind === "user" ? "user:" + p.userId
             : p.kind === "shared" ? "shared:" + p.sharedId : "playlist:" + p.kind
    }

    // Sort order of this list — remembered per playlist / album ("" = as the source orders it).
    property string sortKey: Prefs.stringValue("playlist/sort/" + context, "")
    onContextChanged: sortKey = Prefs.stringValue("playlist/sort/" + context, "")
    function setSort(key) {
        sortKey = key
        Prefs.setValue("playlist/sort/" + context, key)
    }
    // Ctrl + drag a track: the user's own and shared playlists, in their own order (not sorted).
    readonly property bool reorderable: info.playlistIndex !== undefined && info.playlistIndex >= 0
                                        && Playlists.canReorder(info.playlistIndex) && sortKey === ""
    property int dragRow: -1          // the track being dragged (a row of the track model)
    property int dropBefore: -1       // in front of this one (-1 = the end)
    property real dropY: -1           // the line, in the list's content
    function trackDragOver(y) {
        const i = flick.indexAt(flick.width / 2, y)
        if (i < 0)
            return
        const item = flick.itemAtIndex(i)
        const lower = y > item.y + item.height / 2
        dropBefore = lower ? (i + 1 < rows.length ? rows[i + 1] : -1) : rows[i]
        dropY = lower ? item.y + item.height : item.y
        // near the edges the list scrolls along
        const inView = y - flick.contentY
        if (inView < 60)
            flick.contentY = Math.max(flick.originY - flick.topMargin, flick.contentY - 24)
        else if (inView > flick.height - 60)
            flick.contentY = Math.min(flick.contentHeight + flick.originY - flick.height, flick.contentY + 24)
    }
    function finishTrackDrag() {
        if (dragRow >= 0 && dropY >= 0 && dropBefore !== dragRow)
            Playlists.moveRow(info.playlistIndex, dragRow, dropBefore)
        dragRow = -1; dropBefore = -1; dropY = -1
    }

    readonly property var rows: {
        const rows = info.rows
        if (sortKey === "")
            return rows
        if (sortKey === "reverse")
            return rows.slice().reverse()
        const field = sortKey === "duration" ? "duration" : sortKey
        const items = rows.map(r => ({ r: r, v: Library.tracks.get(r)[field] }))
        items.sort((a, b) => field === "duration" ? a.v - b.v
                                                  : String(a.v).localeCompare(String(b.v), undefined, { sensitivity: "base" }))
        return items.map(x => x.r)
    }
    readonly property var mix: Player.mixRevision >= 0 ? Player.mixFor(context) : ({})
    readonly property int totalSeconds: Library.totalDuration(info.rows)
    readonly property real pad: Nav.mobile ? 16 : 34

    readonly property bool playingHere: Player.playing && info.rows.indexOf(Player.currentIndex) >= 0

    // Colored glow behind the header (with a slight parallax)
    Rectangle {
        width: parent.width
        height: 420
        readonly property real scrolled: flick.contentY - flick.originY
        y: -scrolled * 0.45
        opacity: Math.max(0, 1 - scrolled / 420)
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.alpha(page.info.colorA, 0.42) }
            GradientStop { position: 1; color: Theme.alpha(page.info.colorA, 0) }
        }
    }

    ListView {
        id: flick
        anchors.fill: parent
        clip: true
        leftMargin: page.pad
        rightMargin: page.pad
        bottomMargin: page.pad
        cacheBuffer: 800
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 3200
        maximumFlickVelocity: 4200
        ScrollBar.vertical: ThinScrollBar {}
        ScrollAssist { view: flick }
        model: page.rows

        header: Item {
            width: flick.width - page.pad * 2
            height: headerColumn.implicitHeight + page.pad + 12

            ColumnLayout {
                id: headerColumn
                y: page.pad
                width: parent.width
                spacing: Nav.mobile ? 20 : 34

                Reveal {
                    // Phone: the cover above the title
                    GridLayout {
                        width: parent.width
                        columns: Nav.mobile ? 1 : 2
                        columnSpacing: 30
                        rowSpacing: 18

                        ArtCover {
                            readonly property real side: Nav.mobile ? Math.min(220, page.width * 0.56) : 224
                            Layout.preferredWidth: side
                            Layout.preferredHeight: side
                            Layout.alignment: Qt.AlignHCenter
                            radius: page.info.round ? side / 2 : Theme.r(20)
                            colorA: page.info.colorA
                            colorB: page.info.colorB
                            seed: page.info.seed
                            source: page.info.cover
                            mosaic: page.info.mosaic || []
                            decorated: page.info.icon === ""
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                shadowEnabled: true
                                shadowBlur: 1.0
                                shadowOpacity: 0.6
                                shadowVerticalOffset: 18
                            }
                            Icon {
                                anchors.centerIn: parent
                                visible: page.info.icon !== "" && !parent.hasPicture
                                name: page.info.icon
                                size: 84
                                color: "#f4efe8"
                            }

                            // Playlists: your own picture or a different collage
                            HoverHandler { id: coverHover; enabled: page.info.playlistIndex !== undefined }
                            Rectangle {
                                id: coverButton
                                anchors { right: parent.right; bottom: parent.bottom; margins: 12 }
                                width: 40; height: 40
                                radius: 20
                                visible: page.info.playlistIndex !== undefined
                                opacity: coverHover.hovered || coverMenu.opened || Nav.mobile ? 1 : 0
                                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                                color: Qt.rgba(0, 0, 0, editArea.containsMouse ? 0.72 : 0.55)
                                Icon {
                                    anchors.centerIn: parent
                                    name: "edit"
                                    size: 18
                                    color: "#f4efe8"
                                }
                                MouseArea {
                                    id: editArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        const p = coverButton.mapToItem(page, 0, coverButton.height + 8)
                                        coverMenu.x = p.x
                                        coverMenu.y = p.y
                                        coverMenu.open()
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignBottom
                            spacing: 8

                            Text {
                                text: page.info.label
                                color: Theme.textDim
                                font.pixelSize: 12
                                font.weight: Font.Bold
                                font.letterSpacing: 2.4
                            }
                            Text {
                                Layout.fillWidth: true
                                text: page.info.title
                                color: Theme.text
                                font.family: Theme.fontDisplay
                                // One line: a long name gets a smaller font before it is cut.
                                font.pixelSize: Nav.mobile ? 36 : 60
                                fontSizeMode: Text.HorizontalFit
                                minimumPixelSize: Nav.mobile ? 22 : 34
                                font.weight: Font.Bold
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: page.info.meta.concat([
                                          qsTr("%n tracks", "", page.info.rows.length),
                                          Theme.formatTotal(page.totalSeconds)
                                      ]).join("  ·  ")
                                color: Theme.textDim
                                font.pixelSize: 14
                                elide: Text.ElideRight
                                wrapMode: Nav.mobile ? Text.Wrap : Text.NoWrap
                                maximumLineCount: 2
                            }
                        }
                    }
                }

                Reveal {
                    delay: 80
                    RowLayout {
                        width: parent.width
                        spacing: 12

                        PlayFab {
                            visible: page.info.rows.length > 0
                            size: 58
                            playing: page.playingHere
                            enabled: page.info.rows.length > 0
                            prefetchRow: page.rows.length > 0 ? page.rows[0] : -1
                            onClicked: page.playingHere ? Player.toggle() : Player.playFrom(page.rows, page.rows[0], page.context)
                        }
                        IconButton {
                            visible: page.info.rows.length > 1
                            icon: "shuffle"
                            iconSize: 24
                            implicitWidth: 46; implicitHeight: 46
                            onClicked: Player.playShuffled(page.rows, page.context)
                        }
                        // "Mix" like in Spotify: smooth, DJ-style transitions between this playlist's tracks.
                        PillButton {
                            visible: page.context !== "" && page.info.rows.length > 1
                            kind: page.mix.enabled ? "primary" : "ghost"
                            icon: "mix"
                            text: page.mix.enabled ? qsTr("Mix") + " · " + Player.mixStyleName(page.mix.style) : qsTr("Mix")
                            fontSize: 13
                            implicitHeight: 40
                            onClicked: {
                                Player.setMix(page.context, !page.mix.enabled, page.mix.style, page.mix.length)
                                Nav.toast(page.mix.enabled ? qsTr("Mixing on — tracks will flow smoothly")
                                                           : qsTr("Mixing off"), "mix")
                            }
                        }
                        IconButton {
                            visible: page.context !== "" && page.mix.enabled
                            icon: "tune"
                            iconSize: 22
                            implicitWidth: 42; implicitHeight: 42
                            showDot: false
                            onClicked: mixPopup.open()
                        }
                        // An Internet Archive album / concert: all its files into the library at once.
                        PillButton {
                            visible: (page.info.flacItem || "") !== "" && page.info.rows.length > 0
                            kind: "ghost"
                            icon: "download"
                            text: page.info.flacBytes > 0
                                  ? qsTr("Save FLAC (%1)").arg(page.info.flacBytes >= 1e9 ? (page.info.flacBytes / 1e9).toFixed(1) + " GB"
                                                                                         : Math.max(1, Math.round(page.info.flacBytes / 1e6)) + " MB")
                                  : qsTr("Save FLAC")
                            fontSize: 13
                            implicitHeight: 40
                            onClicked: OpenMusic.downloadCollection(page.info.flacItem)
                        }
                        IconButton {
                            visible: (page.info.flacPage || "") !== ""
                            icon: "link"
                            iconSize: 22
                            implicitWidth: 42; implicitHeight: 42
                            showDot: false
                            onClicked: Qt.openUrlExternally(page.info.flacPage)
                        }
                        IconButton {
                            id: moreButton
                            icon: "more"
                            iconSize: 24
                            implicitWidth: 46; implicitHeight: 46
                            showDot: false
                            onClicked: {
                                const p = moreButton.mapToItem(page, 0, moreButton.height + 4)
                                playlistMenu.x = Math.min(p.x, page.width - playlistMenu.width - 12)
                                playlistMenu.y = p.y
                                playlistMenu.confirmDelete = false
                                playlistMenu.open()
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    TrackListHeader {
                        visible: page.info.rows.length > 0
                        showAlbum: page.info.label !== "ALBUM"
                    }

                    ColumnLayout {
                        visible: page.info.rows.length === 0
                        Layout.fillWidth: true
                        Layout.topMargin: 40
                        spacing: 10
                        Icon {
                            Layout.alignment: Qt.AlignHCenter
                            name: page.info.loading ? "refresh" : "heartOutline"
                            size: 48
                            color: Theme.textFaint
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: page.info.loading ? qsTr("Loading the playlist…") : qsTr("Nothing here yet")
                            color: Theme.text
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: page.info.loading ? ((page.info.flacItem || "") !== "" ? qsTr("Fetching the files from the Internet Archive")
                                                                                     : qsTr("Fetching tracks from YouTube Music"))
                                  : page.info.editable === true ? qsTr("Add tracks with ⋯ → Add to playlist on any track.")
                                  : qsTr("Like a track with the heart and it will show up here.")
                            color: Theme.textDim
                            font.pixelSize: 14
                        }
                    }
                }
            }
        }

        // where a dragged track lands
        Rectangle {
            parent: flick.contentItem
            visible: page.dragRow >= 0 && page.dropY >= 0
            z: 5
            x: 0
            y: page.dropY - 1
            width: flick.width - page.pad * 2
            height: 2
            radius: 1
            color: Theme.accent
        }

        delegate: TrackRow {
            id: trackRow
            required property var modelData
            required property int index
            width: flick.width - page.pad * 2
            opacity: page.dragRow === modelData ? 0.4 : 1
            DragHandler {
                target: null
                acceptedModifiers: Qt.ControlModifier
                cursorShape: Qt.ClosedHandCursor
                enabled: page.info.playlistIndex !== undefined && page.info.playlistIndex >= 0
                onActiveChanged: {
                    if (!active) {
                        page.finishTrackDrag()
                        return
                    }
                    if (page.reorderable) {
                        page.dragRow = trackRow.modelData
                        return
                    }
                    Nav.toast(page.sortKey !== "" ? qsTr("Turn the sorting off to put the tracks in your order")
                                                  : qsTr("This playlist keeps its source's order — change it there"), "queue")
                }
                onCentroidChanged: if (active && page.dragRow >= 0)
                                       page.trackDragOver(trackRow.mapToItem(flick.contentItem, centroid.position.x, centroid.position.y).y)
            }
            trackIndex: modelData
            number: index + 1
            showAlbum: page.info.label !== "ALBUM"
            queue: page.rows
            context: page.context
            playlistIndex: page.info.editable === true ? page.info.playlistIndex : -1
            revealDelay: index < 16 ? 180 + index * 30 : 0
        }
    }

    // Small pieces of the "⋯" menu
    component MenuLabel: Text {
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.topMargin: 8
        Layout.bottomMargin: 2
        color: Theme.textFaint
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 1
    }
    component Separator: Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 4
        Layout.bottomMargin: 4
        height: 1
        color: Qt.rgba(1, 1, 1, 0.07)
    }

    // "⋯" next to Play: sort, save as a new playlist, and for the user's own playlists editing,
    // who can see it (YouTube Music) and deleting; for shared playlists the invite link and leaving.
    Popup {
        id: playlistMenu
        property bool confirmDelete: false
        readonly property bool own: page.info.editable === true
        width: 300
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

        contentItem: ColumnLayout {
            spacing: 2

            MenuEntry {
                visible: page.info.playlistIndex !== undefined && page.info.playlistIndex >= 0
                Layout.fillWidth: true
                icon: "folder"
                text: qsTr("Move to a folder…")
                onTriggered: {
                    playlistMenu.close()
                    Nav.moveToFolder(page.info.playlistIndex)
                }
            }
            MenuEntry {
                readonly property bool isHidden: page.info.playlistIndex >= 0 && (Library.playlists[page.info.playlistIndex] || {}).hidden === true
                visible: page.info.playlistIndex !== undefined && page.info.playlistIndex >= 0
                Layout.fillWidth: true
                icon: isHidden ? "add" : "close"
                text: isHidden ? qsTr("Show the playlist again") : qsTr("Hide the playlist")
                onTriggered: {
                    playlistMenu.close()
                    Library.setPlaylistHidden(page.info.playlistIndex, !isHidden)
                    Nav.toast(isHidden ? qsTr("The playlist is back in the list")
                                       : qsTr("Hidden — “Show hidden playlists” at the end of the list brings it back"), "check")
                }
            }
            MenuEntry {
                visible: page.info.kind === "shared"
                Layout.fillWidth: true
                icon: "link"
                text: qsTr("Copy the invite link")
                onTriggered: {
                    playlistMenu.close()
                    Online.copyPlaylistLink(page.info.sharedId)
                }
            }
            MenuEntry {
                visible: page.info.kind === "shared" && Online.isPlaylistOwner(page.info.sharedId)
                Layout.fillWidth: true
                icon: "key"
                text: Online.playlistInfo(page.info.sharedId).protected === true ? qsTr("Change or remove the password") : qsTr("Set a password")
                onTriggered: {
                    playlistMenu.close()
                    Nav.editPlaylistPassword(page.info.sharedId)
                }
            }
            MenuEntry {
                visible: Online.inRoom && Online.room.canControl === true && page.rows.length > 0
                Layout.fillWidth: true
                icon: "group"
                text: qsTr("Add to the listening room…")
                onTriggered: {
                    playlistMenu.close()
                    Nav.addToRoom(page.rows, page.info.name || "")
                }
            }
            MenuEntry {
                visible: playlistMenu.own && page.info.owner !== false
                Layout.fillWidth: true
                icon: "tune"
                text: page.info.kind === "ytplaylist" ? qsTr("Rename") : qsTr("Edit name and description")
                onTriggered: {
                    playlistMenu.close()
                    Nav.editPlaylist(page.info.playlistIndex)
                }
            }
            MenuEntry {
                Layout.fillWidth: true
                visible: page.rows.length > 0
                icon: "add"
                text: qsTr("Save as a new playlist…")
                onTriggered: {
                    playlistMenu.close()
                    Nav.createPlaylist(page.rows)
                }
            }

            // Who can see it — YouTube Music playlists
            MenuLabel {
                visible: playlistMenu.own && page.info.kind === "ytplaylist"
                text: qsTr("WHO CAN SEE IT")
            }
            Repeater {
                model: playlistMenu.own && page.info.kind === "ytplaylist"
                       ? [{ key: "PRIVATE", text: qsTr("Private"), icon: "person" },
                          { key: "UNLISTED", text: qsTr("Unlisted — anyone with the link"), icon: "link" },
                          { key: "PUBLIC", text: qsTr("Public"), icon: "sources" }] : []
                MenuEntry {
                    required property var modelData
                    Layout.fillWidth: true
                    icon: modelData.icon
                    text: modelData.text
                    onTriggered: {
                        playlistMenu.close()
                        Playlists.setPrivacy(page.info.playlistIndex, modelData.key)
                    }
                }
            }

            MenuLabel { text: qsTr("SORT BY") }
            Repeater {
                model: [{ key: "", text: qsTr("Playlist order") }, { key: "reverse", text: qsTr("Playlist order, reversed") },
                        { key: "title", text: qsTr("Title") }, { key: "artist", text: qsTr("Artist") },
                        { key: "album", text: qsTr("Album") }, { key: "duration", text: qsTr("Duration") }]
                MenuEntry {
                    required property var modelData
                    Layout.fillWidth: true
                    icon: page.sortKey === modelData.key ? "check" : ""
                    text: modelData.text
                    onTriggered: {
                        playlistMenu.close()
                        page.setSort(modelData.key)
                    }
                }
            }

            Separator { visible: playlistMenu.own }
            MenuEntry {
                visible: playlistMenu.own
                Layout.fillWidth: true
                icon: "close"
                text: page.info.kind === "shared" && page.info.owner === false
                      ? (playlistMenu.confirmDelete ? qsTr("Click again to leave") : qsTr("Leave the playlist"))
                      : playlistMenu.confirmDelete
                      ? (page.info.kind === "ytplaylist" ? qsTr("Click again — it's deleted from YouTube Music too")
                         : page.info.kind === "shared" ? qsTr("Click again — it's deleted for everyone")
                                                       : qsTr("Click again to delete"))
                      : qsTr("Delete playlist")
                onTriggered: {
                    if (!playlistMenu.confirmDelete) {
                        playlistMenu.confirmDelete = true
                        return
                    }
                    playlistMenu.close()
                    Playlists.deletePlaylist(page.info.playlistIndex)
                    Nav.navigate("home")
                }
            }
        }
    }

    // Playlist cover: upload a picture, shuffle the collage, go back to the default
    Popup {
        id: coverMenu
        width: 300
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

        contentItem: ColumnLayout {
            spacing: 2
            MenuEntry {
                Layout.fillWidth: true
                icon: "image"
                text: qsTr("Upload your own picture…")
                onTriggered: {
                    coverMenu.close()
                    coverDialog.open()
                }
            }
            MenuEntry {
                Layout.fillWidth: true
                visible: !page.info.customCover && (page.info.mosaic || []).length >= 4
                icon: "shuffle"
                text: qsTr("Shuffle the collage")
                onTriggered: {
                    coverMenu.close()
                    Library.shufflePlaylistCover(page.info.playlistIndex)
                }
            }
            MenuEntry {
                Layout.fillWidth: true
                visible: page.info.customCover === true
                icon: "refresh"
                text: qsTr("Restore the default cover")
                onTriggered: {
                    coverMenu.close()
                    Library.resetPlaylistCover(page.info.playlistIndex)
                    Nav.toast(qsTr("The default cover is back"), "image")
                }
            }
        }
    }

    FileDialog {
        id: coverDialog
        title: qsTr("Choose a picture for the playlist")
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp *.bmp *.gif)")]
        onAccepted: {
            if (Library.setPlaylistCover(page.info.playlistIndex, selectedFile))
                Nav.toast(qsTr("New playlist cover"), "image")
        }
    }

    // Mix settings for this playlist
    Popup {
        id: mixPopup
        anchors.centerIn: parent
        width: Math.min(560, page.width - 60)
        modal: true
        focus: true
        padding: 26
        background: Rectangle {
            radius: Theme.r(22)
            color: Theme.surfaceHi
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.07)
        }
        Overlay.modal: Rectangle { color: "#88000000" }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.ms(200) }
            NumberAnimation { property: "scale"; from: 0.95; to: 1; duration: Theme.ms(320); easing.type: Easing.OutBack }
        }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.ms(160) } }

        contentItem: ColumnLayout {
            spacing: 16
            RowLayout {
                Layout.fillWidth: true
                Icon { name: "mix"; size: 22; color: Theme.accent }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Mix: %1").arg(page.info.title)
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                IconButton { icon: "close"; showDot: false; onClicked: mixPopup.close() }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Transition style")
                color: Theme.textDim
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: 1.2
            }
            Flow {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: Player.mixStyles
                    PillButton {
                        required property string modelData
                        kind: page.mix.style === modelData ? "primary" : "secondary"
                        text: Player.mixStyleName(modelData)
                        fontSize: 13
                        implicitHeight: 34
                        onClicked: Player.setMix(page.context, true, modelData, page.mix.length)
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: Player.mixStyleDescription(page.mix.style)
                color: Theme.textDim
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 6
                text: qsTr("Transition length")
                color: Theme.textDim
                font.pixelSize: 12
                font.weight: Font.Bold
                font.letterSpacing: 1.2
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 14
                ProgressSlider {
                    Layout.fillWidth: true
                    value: (page.mix.length - 2) / 14
                    labelFor: function(v) { return Math.round(2 + v * 14) + " s" }
                    onMoved: v => Player.setMix(page.context, true, page.mix.style, Math.round(2 + v * 14))
                }
                Text {
                    text: page.mix.length + " s"
                    color: Theme.text
                    font.pixelSize: 14
                    font.features: { "tnum": 1 }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 12
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Smart transitions (skipping silence, detecting fade-outs) — shared by all mixes.")
                    color: Theme.textFaint
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
                Toggle { checked: Player.smartTransitions; onToggled: c => Player.smartTransitions = c }
            }
            PillButton {
                Layout.alignment: Qt.AlignRight
                kind: "ghost"
                text: qsTr("Turn off the mix")
                onClicked: {
                    Player.setMix(page.context, false, page.mix.style, page.mix.length)
                    mixPopup.close()
                }
            }
        }
    }
}