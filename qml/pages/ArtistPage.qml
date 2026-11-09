import QtQuick
import QtQuick.Effects
import QtQuick.Layouts
import Everplay

// Artist profile: the YouTube Music page (photo, description, popular songs, albums, singles,
// playlists, related artists) and the artist's tracks from the library. An artist known only by
// name (local files) is looked up on YouTube Music first.
Item {
    id: page
    property var args: ({})
    readonly property string name: args.name || ""
    property string browseId: args.browseId || ""
    property bool searched: false          // the name lookup has finished
    property bool descriptionOpen: false

    readonly property var yt: YouTube.artistsRevision >= 0 && browseId !== "" ? YouTube.artist(browseId) : ({})
    readonly property string displayName: yt.name || name
    readonly property bool loading: yt.loading === true || (browseId === "" && !searched && name !== "")
    readonly property var topRows: {
        if (Library.tracks.revision < 0 || YouTube.resultsRevision < 0)   // never: re-run when they change
            return []
        return browseId !== "" ? Library.artistSongRows(browseId) : []
    }
    // Library tracks by this artist (local files and liked songs — not search results).
    readonly property var libraryRows: {
        if (Library.tracks.revision < 0 || displayName === "")
            return []
        return Library.tracksOfArtist(displayName).filter(r => {
            const t = Library.tracks.get(r)
            return t.source === "local" || t.liked
        })
    }
    readonly property var playRows: topRows.length > 0 ? topRows : libraryRows
    readonly property string context: topRows.length > 0 ? "ytartist:" + browseId : "artist:" + displayName
    readonly property bool playingHere: Player.playing && playRows.indexOf(Player.currentIndex) >= 0

    function open(item) {
        if (item.kind === "artist") {
            Nav.navigate("artist", { name: item.title, browseId: item.browseId })
        } else if (item.playlistId) {
            Nav.navigate("playlist", { kind: "ytcollection", playlistId: item.playlistId, title: item.title,
                                       subtitle: item.subtitle, cover: item.cover,
                                       label: item.kind === "album" ? qsTr("ALBUM · YOUTUBE MUSIC") : qsTr("PLAYLIST · YOUTUBE MUSIC") })
        } else {
            Nav.toast(qsTr("This can't be opened yet"), "music")
        }
    }

    Component.onCompleted: {
        if (browseId !== "") {
            YouTube.loadArtist(browseId)
        } else if (name !== "") {
            const known = YouTube.knownArtistId(name)
            if (known !== "") {
                browseId = known
                YouTube.loadArtist(known)
            } else {
                YouTube.findArtist(name)
            }
        }
    }

    Connections {
        target: YouTube
        function onArtistFound(foundName, id) {
            if (foundName !== page.name || page.browseId !== "")
                return
            page.searched = true
            if (id !== "") {
                page.browseId = id
                YouTube.loadArtist(id)
            }
        }
    }

    // Colored glow behind the header (with a slight parallax)
    Rectangle {
        width: parent.width
        height: 460
        readonly property real scrolled: flick.contentY
        y: -scrolled * 0.45
        opacity: Math.max(0, 1 - scrolled / 460)
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.alpha(Theme.accent, 0.38) }
            GradientStop { position: 1; color: Theme.alpha(Theme.accent, 0) }
        }
    }

    // Shadows shared by the album / single / playlist cards and the related artists' round cards
    CardShadow { id: squareShadow; size: 168 }
    CardShadow { id: roundShadow; size: 168; radius: 84 }

    PageFlick {
        id: flick
        anchors.fill: parent
        spacing: 30

        // ---- header ----
        Reveal {
            RowLayout {
                width: parent.width
                spacing: 30

                ArtCover {
                    Layout.preferredWidth: 210
                    Layout.preferredHeight: 210
                    radius: Theme.r(105)
                    colorA: Theme.accent
                    colorB: Theme.accent2
                    seed: 0
                    source: page.yt.photo || ""
                    decorated: false
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        shadowEnabled: true
                        shadowBlur: 1.0
                        shadowOpacity: 0.6
                        shadowVerticalOffset: 18
                    }
                    Icon {
                        anchors.centerIn: parent
                        visible: !parent.hasPicture
                        name: "person"
                        size: 84
                        color: "#f4efe8"
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignBottom
                    spacing: 8

                    SourceBadge {
                        visible: page.browseId !== ""
                        source: "youtube"
                    }
                    Text {
                        text: qsTr("ARTIST")
                        color: Theme.textDim
                        font.pixelSize: 12
                        font.weight: Font.Bold
                        font.letterSpacing: 2.4
                    }
                    Text {
                        Layout.fillWidth: true
                        text: page.displayName
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: page.displayName.length > 18 ? 48 : 64
                        font.weight: Font.Bold
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        lineHeight: 0.95
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: [page.yt.subscribers || "",
                               page.libraryRows.length > 0 ? qsTr("%n tracks in your library", "", page.libraryRows.length) : ""]
                              .filter(s => s !== "").join("  ·  ")
                        color: Theme.textDim
                        font.pixelSize: 14
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // ---- actions ----
        Reveal {
            delay: 60
            RowLayout {
                width: parent.width
                spacing: 12

                PlayFab {
                    size: 58
                    playing: page.playingHere
                    enabled: page.playRows.length > 0
                    prefetchRow: page.playRows.length > 0 ? page.playRows[0] : -1
                    onClicked: page.playingHere ? Player.toggle() : Player.playFrom(page.playRows, page.playRows[0], page.context)
                }
                IconButton {
                    icon: "shuffle"
                    iconSize: 24
                    implicitWidth: 46; implicitHeight: 46
                    enabled: page.playRows.length > 0
                    onClicked: Player.playShuffled(page.playRows, page.context)
                }
                Spinner {
                    visible: page.loading
                    size: 20
                    color: Theme.textDim
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.yt.error ? qsTr("Couldn't load the artist from YouTube Music: %1").arg(page.yt.error)
                        : page.searched && page.browseId === "" ? qsTr("This artist wasn't found on YouTube Music.")
                        : ""
                    color: Theme.textDim
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
            }
        }

        // ---- description ----
        Reveal {
            delay: 90
            visible: (page.yt.description || "") !== ""
            ColumnLayout {
                width: Math.min(parent.width, 860)
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: page.yt.description || ""
                    color: Theme.textDim
                    font.pixelSize: 14
                    lineHeight: 1.2
                    wrapMode: Text.WordWrap
                    maximumLineCount: page.descriptionOpen ? 1000 : 3
                    elide: Text.ElideRight
                }
                Text {
                    text: page.descriptionOpen ? qsTr("Show less") : qsTr("Show more")
                    color: moreMouse.containsMouse ? Theme.text : Theme.accent
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    MouseArea {
                        id: moreMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: page.descriptionOpen = !page.descriptionOpen
                    }
                }
            }
        }

        // ---- popular songs ----
        Reveal {
            delay: 120
            visible: page.topRows.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 6
                SectionHeader {
                    title: page.yt.videosOnly ? qsTr("Videos") : qsTr("Popular")
                    action: (page.yt.songsPlaylistId || "") !== "" ? qsTr("Show all") : ""
                    onActionClicked: Nav.navigate("playlist", {
                        kind: "ytcollection", playlistId: page.yt.songsPlaylistId, title: page.displayName,
                        subtitle: qsTr("All songs"), cover: page.yt.photo || "", label: qsTr("ARTIST"), round: true })
                }
                Repeater {
                    model: page.topRows
                    TrackRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        trackIndex: modelData
                        number: index + 1
                        queue: page.topRows
                        compact: page.yt.videosOnly === true   // long upload titles get the album's room
                        context: "ytartist:" + page.browseId
                        revealDelay: 140 + index * 30
                    }
                }
            }
        }

        // ---- in your library ----
        Reveal {
            delay: 150
            visible: page.libraryRows.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 6
                SectionHeader { title: qsTr("In your library") }
                Repeater {
                    model: page.libraryRows.slice(0, 20)
                    TrackRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        trackIndex: modelData
                        number: index + 1
                        queue: page.libraryRows
                        context: "artist:" + page.displayName
                        revealDelay: 160 + index * 25
                    }
                }
            }
        }

        // ---- albums, singles, playlists, related artists (sections as YouTube Music names them) ----
        Repeater {
            model: page.yt.sections || []
            Reveal {
                id: section
                required property var modelData
                required property int index
                delay: 180 + index * 40
                Layout.fillWidth: true
                ColumnLayout {
                    width: parent.width
                    spacing: 14
                    SectionHeader {
                        title: section.modelData.title
                        // The carousel's first ten — the rest of the singles / albums on request.
                        action: section.modelData.moreId && !section.modelData.complete
                                ? (section.modelData.loading ? qsTr("Loading…") : qsTr("Show all")) : ""
                        onActionClicked: YouTube.loadArtistSection(page.browseId, section.index)
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 18
                        Repeater {
                            model: section.modelData.items
                            AlbumCard {
                                required property var modelData
                                required property int index
                                width: 168
                                shadow: modelData.kind === "artist" ? roundShadow : squareShadow
                                title: modelData.title
                                subtitle: modelData.subtitle
                                cover: modelData.cover
                                round: modelData.kind === "artist"
                                seed: index
                                revealDelay: index * 30
                                onClicked: page.open(modelData)
                                onPlayClicked: page.open(modelData)
                            }
                        }
                    }
                }
            }
        }
    }
}
