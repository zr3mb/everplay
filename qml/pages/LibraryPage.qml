import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Everplay

// Library. The lists are virtualized (ListView / GridView), so they only create
// the visible rows — it stays smooth even with thousands of tracks.
Item {
    id: page
    property var args: ({})
    property int tab: args && args.tab !== undefined ? args.tab : 0
    // The tab the page opened on: its rows animate in. Each tab is built the first time it's shown (they
    // hold many covers); one opened later shows its rows at once, as if it had been there all along.
    readonly property int startTab: args && args.tab !== undefined ? args.tab : 0
    readonly property real pad: 34
    // Local files + liked on YouTube Music
    readonly property var libraryRows: {
        if (Library.tracks.revision < 0)   // never: the read makes the binding re-run (a bare read is compiled away)
            return []
        return Library.libraryRows()
    }

    component LibraryHeader: ColumnLayout {
        spacing: 22

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Library")
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 38
                    font.weight: Font.Bold
                }
                Text {
                    Layout.fillWidth: true
                    text: Library.scanning
                          ? Library.scanStatus
                          : qsTr("%n tracks", "", page.libraryRows.length) + "  ·  "
                            + qsTr("%n albums", "", Library.albums.length) + "  ·  "
                            + qsTr("%n artists", "", Library.artists.length)
                    color: Library.scanning ? Theme.accent : Theme.textDim
                    font.pixelSize: 14
                }
            }

            PillButton {
                kind: "secondary"
                icon: "add"
                text: qsTr("Add folder")
                onClicked: Nav.addFolder()
            }
        }

        Item {
            Layout.fillWidth: true
            implicitHeight: tabs.implicitHeight

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: 1
                color: Qt.rgba(1, 1, 1, 0.06)
            }
            SegmentedTabs {
                id: tabs
                style: "underline"
                model: [qsTr("Tracks"), qsTr("Albums"), qsTr("Artists")]
                currentIndex: page.tab
                onActivated: i => page.tab = i
            }
        }

        Rectangle {
            // Only for an empty library: with music from a service it's just in the way.
            visible: page.libraryRows.length === 0 && !Library.scanning
            Layout.fillWidth: true
            implicitHeight: 72
            radius: Theme.r(16)
            color: Theme.alpha(Theme.accent, 0.08)
            border.width: 1
            border.color: Theme.alpha(Theme.accent, 0.18)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 14
                spacing: 14
                Icon { name: "folder"; size: 24; color: Theme.accent }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("You don't have any local files yet. Add a music folder — Everplay will read the tags and covers.")
                    color: Theme.textDim
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
                PillButton {
                    icon: "folder"
                    text: qsTr("Choose a folder")
                    onClicked: Nav.addFolder()
                }
            }
        }
    }

    // ---- Tracks ----------------------------------------------------------------
    Loader {
        anchors.fill: parent
        visible: page.tab === 0
        active: page.tab === 0 || item !== null
        sourceComponent: ListView {
            id: tracksView
            clip: true
            leftMargin: page.pad
            rightMargin: page.pad
            bottomMargin: page.pad
            cacheBuffer: 800
            boundsBehavior: Flickable.StopAtBounds
            flickDeceleration: 3200
            maximumFlickVelocity: 4200
            ScrollBar.vertical: ThinScrollBar {}
            ScrollAssist { view: tracksView }
            model: page.libraryRows

            header: Item {
                width: tracksView.width - page.pad * 2
                height: page.pad + hdr.implicitHeight + 12 + listHead.implicitHeight
                LibraryHeader {
                    id: hdr
                    y: page.pad
                    width: parent.width
                }
                TrackListHeader {
                    id: listHead
                    anchors.bottom: parent.bottom
                    width: parent.width
                }
            }

            delegate: TrackRow {
                required property var modelData
                required property int index
                width: tracksView.width - page.pad * 2
                trackIndex: modelData
                number: index + 1
                queue: page.libraryRows
                revealDelay: index < 16 ? index * 28 : 0
                animateIn: page.startTab === 0
            }
        }
    }

    // ---- Albums and artists -------------------------------------------------------
    component CardGrid: GridView {
        id: grid
        property real minCell: 200
        property bool roundCards: false
        readonly property real inner: width - page.pad * 2
        readonly property int columns: Math.max(3, Math.floor(inner / minCell))
        readonly property alias cardShadow: cardShadow

        anchors.fill: parent
        clip: true
        leftMargin: page.pad
        rightMargin: page.pad
        bottomMargin: page.pad
        cellWidth: inner / columns
        cellHeight: cellWidth - 22 + 62 + 30
        cacheBuffer: 600
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 3200
        ScrollAssist { view: grid }
        maximumFlickVelocity: 4200
        ScrollBar.vertical: ThinScrollBar {}

        header: Item {
            width: grid.inner
            height: page.pad + gridHdr.implicitHeight + 28
            LibraryHeader {
                id: gridHdr
                y: page.pad
                width: parent.width
            }
        }

        CardShadow {
            id: cardShadow
            size: grid.cellWidth - 22
            radius: grid.roundCards ? size / 2 : Theme.r(14)
        }
    }

    Loader {
        anchors.fill: parent
        visible: page.tab === 1
        active: page.tab === 1 || item !== null
        sourceComponent: CardGrid {
            id: albumsView
            model: Library.albums

            delegate: Item {
                id: albumCell
                required property var modelData
                required property int index
                width: albumsView.cellWidth
                height: albumsView.cellHeight

                AlbumCard {
                    width: albumsView.cellWidth - 22
                    shadow: albumsView.cardShadow
                    title: albumCell.modelData.title
                    subtitle: albumCell.modelData.artist + (albumCell.modelData.year > 0 ? " · " + albumCell.modelData.year : "")
                    colorA: albumCell.modelData.colorA
                    colorB: albumCell.modelData.colorB
                    seed: albumCell.modelData.seed
                    cover: albumCell.modelData.cover
                    playing: Player.playing && Player.albumKey === albumCell.modelData.key
                    revealDelay: albumCell.index < 12 ? albumCell.index * 45 : 0
                    animateIn: page.startTab === 1
                    onClicked: Nav.navigate("playlist", { kind: "album", key: albumCell.modelData.key })
                    onPlayClicked: {
                        if (playing) { Player.toggle(); return }
                        const rows = Library.tracksOfAlbum(albumCell.modelData.key)
                        Player.playFrom(rows, rows[0])
                    }
                }
            }
        }
    }

    Loader {
        anchors.fill: parent
        visible: page.tab === 2
        active: page.tab === 2 || item !== null
        sourceComponent: CardGrid {
            id: artistsView
            minCell: 190
            roundCards: true
            model: Library.artists

            delegate: Item {
                id: artistCell
                required property var modelData
                required property int index
                width: artistsView.cellWidth
                height: artistsView.cellHeight

                AlbumCard {
                    width: artistsView.cellWidth - 22
                    round: true
                    shadow: artistsView.cardShadow
                    title: artistCell.modelData.name
                    subtitle: qsTr("%n tracks", "", artistCell.modelData.count)
                    colorA: artistCell.modelData.colorA
                    colorB: artistCell.modelData.colorB
                    seed: artistCell.modelData.seed
                    cover: artistCell.modelData.cover
                    playing: Player.playing && Player.artist === artistCell.modelData.name
                    revealDelay: artistCell.index < 12 ? artistCell.index * 45 : 0
                    animateIn: page.startTab === 2
                    onClicked: Nav.openArtist(artistCell.modelData.name, "")
                    onPlayClicked: {
                        if (playing) { Player.toggle(); return }
                        const rows = Library.tracksOfArtist(artistCell.modelData.name)
                        Player.playFrom(rows, rows[0])
                    }
                }
            }
        }
    }
}
