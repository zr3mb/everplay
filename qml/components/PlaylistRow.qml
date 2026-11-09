import QtQuick
import Everplay

// A compact playlist row (small cover, name, Play on hover): the shortcuts on the home page and the
// playlists found by the search. playlist: a Library.playlists entry (or one shaped like it).
Item {
    id: root
    property var playlist: ({})
    property string subtitle: ""   // a second line (who made it, how many tracks); taller row
    property string source: ""     // where it comes from (SourceBadge), before the second line
    signal clicked
    signal playClicked

    implicitWidth: 230
    implicitHeight: subtitle !== "" || source !== "" ? 48 : 34

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.clicked() }

    Rectangle {
        anchors.fill: parent
        radius: Theme.r(8)
        color: Theme.alpha(Theme.text, hover.hovered ? 0.06 : 0)
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }
    ArtCover {
        id: cover
        x: 4
        anchors.verticalCenter: parent.verticalCenter
        width: root.subtitle !== "" || root.source !== "" ? 38 : 26
        height: width
        radius: Theme.r(root.subtitle !== "" ? 8 : 7)
        decorated: false
        colorA: root.playlist.colorA || Theme.accent
        colorB: root.playlist.colorB || Theme.accent2
        seed: root.playlist.seed || 0
        source: root.playlist.cover || ""
        mosaic: root.playlist.mosaic || []
        Icon {
            anchors.centerIn: parent
            visible: !cover.hasPicture
            name: root.playlist.icon || "music"
            size: 15
            color: "#f4efe8"
        }
    }
    Column {
        anchors { left: cover.right; leftMargin: 12; right: play.left; rightMargin: 6; verticalCenter: parent.verticalCenter }
        spacing: 1
        Text {
            width: parent.width
            text: root.playlist.name || ""
            color: Theme.text
            font.pixelSize: 13
            font.weight: Font.Medium
            elide: Text.ElideRight
        }
        Row {
            width: parent.width
            visible: root.subtitle !== "" || root.source !== ""
            spacing: 6
            SourceBadge {
                id: badge
                visible: root.source !== ""
                source: root.source || "local"
                plain: true
                height: 16
            }
            Text {
                width: parent.width - (badge.visible ? badge.width + 6 : 0)
                text: (badge.visible && root.subtitle !== "" ? "· " : "") + root.subtitle
                color: Theme.textDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }
    IconButton {
        id: play
        anchors { right: parent.right; rightMargin: 2; verticalCenter: parent.verticalCenter }
        implicitWidth: 30
        implicitHeight: 30
        icon: "play"
        iconSize: 16
        showDot: false
        opacity: hover.hovered ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
        onClicked: root.playClicked()
    }
}
