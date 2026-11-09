import QtQuick
import QtQuick.Effects
import QtQuick.Layouts
import Everplay

// A Spotify artist (opened from a Spotify track's artist): photo, followers, genres, popular tracks, albums
// and singles from Spotify. The tracks play from YouTube Music, like every Spotify track.
Item {
    id: page
    property var args: ({})
    readonly property string artistId: args.artistId || ""

    readonly property var sp: Spotify.artistsRevision >= 0 && artistId !== "" ? Spotify.artist(artistId) : ({})
    readonly property string displayName: sp.name || args.name || ""
    readonly property bool loading: sp.loading === true
    readonly property var topRows: {
        if (Library.tracks.revision < 0 || Spotify.artistsRevision < 0)   // never: re-run when they change
            return []
        return Library.spotifyArtistRows(artistId)
    }
    readonly property string context: "spa:" + artistId
    readonly property bool playingHere: Player.playing && topRows.indexOf(Player.currentIndex) >= 0

    function openAlbum(a) {
        Nav.navigate("playlist", { kind: "spcollection", playlistId: a.id, title: a.title,
                                   subtitle: page.displayName + (a.subtitle ? " · " + a.subtitle : ""), cover: a.cover,
                                   label: a.kind === "album" ? qsTr("ALBUM · SPOTIFY") : qsTr("SINGLE · SPOTIFY") })
    }

    Component.onCompleted: Spotify.loadArtist(artistId)

    // Spotify's green glow behind the header (with a slight parallax)
    Rectangle {
        width: parent.width
        height: 460
        readonly property real scrolled: flick.contentY
        y: -scrolled * 0.45
        opacity: Math.max(0, 1 - scrolled / 460)
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.alpha("#1db954", 0.26) }
            GradientStop { position: 1; color: Theme.alpha("#1db954", 0) }
        }
    }

    CardShadow { id: squareShadow; size: 168 }

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
                    colorA: "#1db954"
                    colorB: "#14532d"
                    seed: 0
                    source: page.sp.photo || ""
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

                    SourceBadge { source: "spotify" }
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
                        text: [page.sp.followers > 0 ? qsTr("%n followers", "", page.sp.followers) : "",
                               (page.sp.genres || []).slice(0, 3).join(", ")].filter(s => s !== "").join("  ·  ")
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
                    enabled: page.topRows.length > 0
                    prefetchRow: page.topRows.length > 0 ? page.topRows[0] : -1
                    onClicked: page.playingHere ? Player.toggle() : Player.playFrom(page.topRows, page.topRows[0], page.context)
                }
                IconButton {
                    icon: "shuffle"
                    iconSize: 24
                    implicitWidth: 46; implicitHeight: 46
                    enabled: page.topRows.length > 0
                    onClicked: Player.playShuffled(page.topRows, page.context)
                }
                IconButton {
                    icon: "radio"
                    iconSize: 22
                    implicitWidth: 46; implicitHeight: 46
                    enabled: page.topRows.length > 0
                    onClicked: Player.startRadio(page.topRows[0])
                }
                Spinner {
                    visible: page.loading
                    size: 20
                    color: Theme.textDim
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.sp.error ? qsTr("Couldn't load the artist from Spotify: %1").arg(page.sp.error) : ""
                    color: Theme.textDim
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                }
                Item { Layout.fillWidth: true }
                PillButton {
                    kind: "ghost"
                    icon: "playCircle"
                    text: qsTr("On YouTube Music")
                    fontSize: 13
                    onClicked: Nav.navigate("artist", { name: page.displayName, browseId: "" })
                }
            }
        }

        // ---- popular tracks ----
        Reveal {
            delay: 120
            visible: page.topRows.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 6
                SectionHeader { title: qsTr("Popular") }
                Repeater {
                    model: page.topRows
                    TrackRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        trackIndex: modelData
                        number: index + 1
                        queue: page.topRows
                        context: page.context
                        revealDelay: 140 + index * 30
                    }
                }
            }
        }

        // ---- albums, then singles ----
        Repeater {
            model: [{ title: qsTr("Albums"), items: page.sp.albums || [] },
                    { title: qsTr("Singles and EPs"), items: page.sp.singles || [] }]
            Reveal {
                id: section
                required property var modelData
                required property int index
                delay: 150 + index * 40
                visible: modelData.items.length > 0
                Layout.fillWidth: true
                ColumnLayout {
                    width: parent.width
                    spacing: 14
                    SectionHeader { title: section.modelData.title }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 18
                        Repeater {
                            model: section.modelData.items
                            AlbumCard {
                                required property var modelData
                                required property int index
                                width: 168
                                shadow: squareShadow
                                title: modelData.title
                                subtitle: [modelData.subtitle, qsTr("%n tracks", "", modelData.count || 0)].filter(s => s).join(" · ")
                                cover: modelData.cover
                                colorA: "#1db954"
                                colorB: "#14532d"
                                seed: index
                                revealDelay: Math.min(index, 12) * 30
                                onClicked: page.openAlbum(modelData)
                                onPlayClicked: page.openAlbum(modelData)
                            }
                        }
                    }
                }
            }
        }
    }
}
