import QtQuick
import QtQuick.Effects
import QtQuick.Layouts
import Everplay

// A SoundCloud profile (on SoundCloud artists are users): photo, description, popular tracks, the newest
// uploads, albums and playlists. Opened from a SoundCloud track's artist or from search.
Item {
    id: page
    property var args: ({})
    readonly property string userId: args.userId || ""
    property bool descriptionOpen: false
    property int tracksShown: 20

    readonly property var sc: SoundCloud.profilesRevision >= 0 && userId !== "" ? SoundCloud.profile(userId) : ({})
    readonly property string displayName: sc.name || args.name || ""
    readonly property bool loading: sc.loading === true
    readonly property var popularRows: {
        if (Library.tracks.revision < 0 || SoundCloud.profilesRevision < 0)   // never: re-run when they change
            return []
        return Library.soundCloudProfileRows(userId, "popular")
    }
    readonly property var trackRows: {
        if (Library.tracks.revision < 0 || SoundCloud.profilesRevision < 0)
            return []
        return Library.soundCloudProfileRows(userId, "tracks")
    }
    readonly property var playRows: popularRows.length > 0 ? popularRows : trackRows
    readonly property string context: "scu:" + userId + (popularRows.length > 0 ? "/popular" : "/tracks")
    readonly property bool playingHere: Player.playing && playRows.indexOf(Player.currentIndex) >= 0

    function openList(p) {
        Nav.navigate("playlist", { kind: "sccollection", playlistId: p.id, title: p.title, subtitle: p.subtitle,
                                   cover: p.cover })
    }

    Component.onCompleted: SoundCloud.loadProfile(userId)

    // Colored glow behind the header (with a slight parallax)
    Rectangle {
        width: parent.width
        height: 460
        readonly property real scrolled: flick.contentY
        y: -scrolled * 0.45
        opacity: Math.max(0, 1 - scrolled / 460)
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.alpha("#ff7a3d", 0.30) }
            GradientStop { position: 1; color: Theme.alpha("#ff7a3d", 0) }
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
                    colorA: "#d9a27e"
                    colorB: "#5a3a28"
                    seed: 0
                    source: page.sc.photo || page.args.photo || ""
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

                    Text {
                        text: page.sc.verified ? qsTr("PROFILE · SOUNDCLOUD · VERIFIED") : qsTr("PROFILE · SOUNDCLOUD")
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
                        text: [page.sc.fullName && page.sc.fullName !== page.displayName ? page.sc.fullName : "",
                               page.sc.place || "", page.sc.subtitle || ""].filter(s => s !== "").join("  ·  ")
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
                    enabled: page.trackRows.length > 0
                    onClicked: Player.playShuffled(page.trackRows, "scu:" + page.userId + "/tracks")
                }
                Spinner {
                    visible: page.loading
                    size: 20
                    color: Theme.textDim
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.sc.error ? qsTr("Couldn't load the profile from SoundCloud: %1").arg(page.sc.error)
                        : !page.loading && page.sc.loading === false && page.playRows.length === 0
                          ? qsTr("No tracks on this profile.") : ""
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
            visible: (page.sc.description || "") !== ""
            ColumnLayout {
                width: Math.min(parent.width, 860)
                spacing: 6
                Text {
                    id: description
                    Layout.fillWidth: true
                    text: page.sc.description || ""
                    color: Theme.textDim
                    font.pixelSize: 14
                    lineHeight: 1.2
                    wrapMode: Text.WordWrap
                    maximumLineCount: page.descriptionOpen ? 1000 : 3
                    elide: Text.ElideRight
                }
                Text {
                    visible: description.truncated || page.descriptionOpen
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

        // ---- popular tracks ----
        Reveal {
            delay: 120
            visible: page.popularRows.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 6
                SectionHeader { title: qsTr("Popular") }
                Repeater {
                    model: page.popularRows
                    TrackRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        trackIndex: modelData
                        number: index + 1
                        queue: page.popularRows
                        context: "scu:" + page.userId + "/popular"
                        revealDelay: 140 + index * 30
                    }
                }
            }
        }

        // ---- albums, then playlists ----
        Repeater {
            model: [{ title: qsTr("Albums"), items: page.sc.albums || [] },
                    { title: qsTr("Playlists"), items: page.sc.playlists || [] }]
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
                                subtitle: qsTr("%n tracks", "", modelData.count || 0)
                                cover: modelData.cover
                                colorA: "#d9a27e"
                                colorB: "#5a3a28"
                                seed: index
                                revealDelay: index * 30
                                onClicked: page.openList(modelData)
                                onPlayClicked: page.openList(modelData)
                            }
                        }
                    }
                }
            }
        }

        // ---- the newest uploads ----
        Reveal {
            delay: 200
            visible: page.trackRows.length > 0
            ColumnLayout {
                width: parent.width
                spacing: 6
                SectionHeader {
                    title: qsTr("Tracks")
                    subtitle: qsTr("Newest first")
                }
                Repeater {
                    model: page.trackRows.slice(0, page.tracksShown)
                    TrackRow {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        trackIndex: modelData
                        number: index + 1
                        queue: page.trackRows
                        context: "scu:" + page.userId + "/tracks"
                        revealDelay: 220 + Math.min(index, 12) * 25
                    }
                }
                PillButton {
                    visible: page.trackRows.length > page.tracksShown
                    Layout.topMargin: 6
                    kind: "ghost"
                    icon: "expandMore"
                    text: qsTr("Show all (%1)").arg(page.trackRows.length)
                    onClicked: page.tracksShown = page.trackRows.length
                }
            }
        }
    }
}
