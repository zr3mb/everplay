import QtQuick
import Everplay

// The user's picture in a circle (Profile); without one, the person icon on the accent gradient.
Item {
    id: root
    property real iconSize: 18

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        visible: !photo.hasPicture
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.accent }
            GradientStop { position: 1; color: Theme.accent2 }
        }
        Icon {
            anchors.centerIn: parent
            name: "person"
            size: root.iconSize
            color: Theme.onAccent
        }
    }
    ArtCover {
        id: photo
        anchors.fill: parent
        visible: hasPicture
        radius: width / 2
        decorated: false
        source: Profile.picture.toString()
    }
}
