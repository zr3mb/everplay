import QtQuick
import Everplay

// Track source badge: local file / YouTube Music / Spotify / SoundCloud / the open FLAC sources / radio.
Rectangle {
    id: root
    property string source: "local"
    property bool showLabel: true
    property bool plain: false   // icon and name only, without the pill (lists, where every row has one)
    property bool onCover: false // over a cover: a dark glass pill that reads on any picture

    readonly property var info: ({
        local:   { label: qsTr("Local"),       icon: "folder",     color: "#9fbfa8" },
        youtube: { label: "YouTube Music", icon: "playCircle", color: "#cf9d9d" },
        spotify: { label: "Spotify",       icon: "music",      color: "#5fd58a" },
        soundcloud: { label: "SoundCloud", icon: "wavecloud",      color: "#d9a27e" },
        archive: { label: "Internet Archive", icon: "archive", color: "#9cc3c9" },
        commons: { label: "Wikimedia Commons", icon: "globe",  color: "#a9b6d9" },
        radio:   { label: qsTr("Radio"),       icon: "live",       color: "#cdb68e" },
        own:     { label: qsTr("Yours"),       icon: "library",    color: "#b7aac9" },
        stream:  { label: qsTr("Stream"),      icon: "link",       color: "#93a8c3" }
    })[source] || { label: source, icon: "music", color: "#a19e98" }

    implicitWidth: row.implicitWidth + (plain ? 0 : showLabel ? 18 : 12)
    implicitHeight: onCover ? 22 : 24
    radius: height / 2
    color: plain ? "transparent" : onCover ? Qt.rgba(0.05, 0.05, 0.07, 0.72) : Theme.alpha(info.color, 0.12)
    border.width: plain ? 0 : 1
    border.color: onCover ? Qt.rgba(1, 1, 1, 0.12) : Theme.alpha(info.color, 0.2)

    Row {
        id: row
        anchors.centerIn: root.plain ? undefined : parent
        anchors.verticalCenter: root.plain ? parent.verticalCenter : undefined
        spacing: 6

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: root.info.icon
            size: 13
            color: root.info.color
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.showLabel
            text: root.info.label
            color: root.plain ? Theme.textDim : root.onCover ? "#f4efe8" : Qt.lighter(root.info.color, 1.12)
            font.pixelSize: root.plain ? 13 : 11
            font.weight: root.plain ? Font.Normal : Font.DemiBold
        }
    }
}
