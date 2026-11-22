import QtQuick
import QtQuick.Layouts
import Everplay

// Title bar over the page: back / forward, the search field, the profile and the window buttons.
// The empty space drags the window; a double click maximizes it.
Item {
    id: root
    readonly property var win: Window.window

    Item {
        anchors.fill: parent
        DragHandler {
            target: null
            onActiveChanged: if (active) root.win.startSystemMove()
        }
        TapHandler {
            onDoubleTapped: root.win.visibility === Window.Maximized ? root.win.showNormal() : root.win.showMaximized()
        }
    }

    // Back / forward: two halves of one capsule
    Rectangle {
        id: history
        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
        width: 84; height: 40
        radius: 20
        color: Theme.glass
        border.width: 1
        border.color: Theme.hairline

        Row {
            anchors.centerIn: parent
            IconButton {
                width: 40; height: 40
                icon: "chevronLeft"
                iconSize: 22
                enabled: Nav.canGoBack
                showDot: false
                onClicked: Nav.back()
            }
            IconButton {
                width: 40; height: 40
                icon: "chevronRight"
                iconSize: 22
                enabled: Nav.canGoForward
                showDot: false
                onClicked: Nav.forward()
            }
        }
    }

    // Search field (opens the search page)
    Rectangle {
        id: search
        anchors { left: history.right; leftMargin: 12; verticalCenter: parent.verticalCenter }
        width: Math.min(560, root.width * 0.42)
        height: 44
        radius: 22
        color: searchMouse.containsMouse ? Theme.alpha(Theme.surfaceHi, 0.85) : Theme.glass
        border.width: 1
        border.color: searchMouse.containsMouse ? Theme.alpha(Theme.accent, 0.45) : Theme.hairline
        scale: searchMouse.pressed ? 0.99 : 1
        Behavior on color { ColorAnimation { duration: Theme.normal } }
        Behavior on border.color { ColorAnimation { duration: Theme.normal } }
        Behavior on scale { NumberAnimation { duration: Theme.ms(200); easing.type: Easing.OutBack } }

        Icon {
            id: searchIcon
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            size: 20
            color: searchMouse.containsMouse ? Theme.accent : Theme.textDim
            Behavior on color { ColorAnimation { duration: Theme.normal } }
        }
        Text {
            anchors { left: searchIcon.right; leftMargin: 12; right: kbd.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
            text: qsTr("Search tracks, artists, albums…")
            color: Theme.textFaint
            font.pixelSize: 14
            elide: Text.ElideRight
        }
        Rectangle {
            id: kbd
            anchors { right: parent.right; rightMargin: 9; verticalCenter: parent.verticalCenter }
            width: kbdText.implicitWidth + 14
            height: 26
            radius: 13
            color: Theme.fill
            Text {
                id: kbdText
                anchors.centerIn: parent
                text: "Ctrl K"
                color: Theme.textDim
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
        }
        MouseArea {
            id: searchMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: Nav.openSearch()
        }
    }

    Row {
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        spacing: 12

        // Profile: the avatar alone, ringed in the accent on hover; opens the profile box
        Rectangle {
            id: profileButton
            anchors.verticalCenter: parent.verticalCenter
            width: 40; height: 40; radius: 20
            color: "transparent"
            border.width: 2
            border.color: profileMouse.containsMouse || profilePopup.opened ? Theme.accent : Theme.hairline
            Behavior on border.color { ColorAnimation { duration: Theme.normal } }
            ProfileAvatar {
                anchors.centerIn: parent
                width: 32; height: 32
            }
            MouseArea {
                id: profileMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: profilePopup.opened ? profilePopup.close() : profilePopup.open()
            }
            ProfilePopup {
                id: profilePopup
                x: profileButton.width - width
                y: profileButton.height + 10
            }
        }

        WindowControls {
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    Shortcut { sequence: "Ctrl+K"; onActivated: Nav.openSearch() }
    Connections {
        target: Nav
        function onProfileRequested() { if (!Nav.mobile) profilePopup.open() }   // the phone layout has its own
    }
}
