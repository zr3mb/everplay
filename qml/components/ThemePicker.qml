import QtQuick
import QtQuick.Layouts
import Everplay

// Theme cards (Settings): a miniature of each theme's colors; clicking one switches to it.
// Themes from plugins follow the built-in ones, five to a row.
GridLayout {
    id: root
    columns: 5
    columnSpacing: 12
    rowSpacing: 12

    Repeater {
        model: Theme.themeIds

        Item {
            id: card
            required property string modelData
            readonly property var t: Theme.themes[modelData]
            readonly property bool selected: Theme.themeId === modelData

            Layout.fillWidth: true
            Layout.preferredWidth: 100
            implicitHeight: 128

            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: Theme.themeId = card.modelData }

            Rectangle {
                id: frame
                anchors.fill: parent
                radius: Theme.r(16)
                color: card.t.bg
                border.width: card.selected ? 2 : 1
                border.color: card.selected ? card.t.accent : Theme.alpha(Theme.text, hover.hovered ? 0.22 : 0.1)
                scale: hover.hovered && !card.selected ? 1.02 : 1
                Behavior on scale { NumberAnimation { duration: Theme.ms(260); easing.type: Easing.OutBack } }
                Behavior on border.color { ColorAnimation { duration: Theme.normal } }

                // Miniature window: sidebar + content with a play button and two rows
                Rectangle {
                    x: 10; y: 10
                    width: 24; height: parent.height - 52
                    radius: Theme.r(6)
                    color: card.t.surface
                    Column {
                        x: 5; y: 7
                        spacing: 4
                        Repeater {
                            model: 4
                            Rectangle {
                                required property int index
                                width: 14; height: 3; radius: 1.5
                                color: index === 0 ? card.t.accent : card.t.textFaint
                            }
                        }
                    }
                }
                Rectangle {
                    x: 40; y: 10
                    width: parent.width - 50; height: parent.height - 52
                    radius: Theme.r(6)
                    color: card.t.surface
                    Rectangle {
                        x: 8; y: 8
                        width: 18; height: 18; radius: 9
                        color: card.t.accent
                    }
                    Rectangle { x: 32; y: 10; width: parent.width * 0.4; height: 4; radius: 2; color: card.t.text }
                    Rectangle { x: 32; y: 18; width: parent.width * 0.26; height: 3; radius: 1.5; color: card.t.textDim }
                    Rectangle {
                        x: 8; y: 34
                        width: parent.width - 16; height: 4; radius: 2
                        color: card.t.surfaceHi
                        Rectangle { width: parent.width * 0.55; height: parent.height; radius: 2; color: card.t.accent }
                    }
                    Icon {
                        visible: card.modelData === "cat"
                        anchors { right: parent.right; rightMargin: 7; top: parent.top; topMargin: 6 }
                        name: "heart"
                        size: 13
                        color: card.t.accent
                    }
                }

                Row {
                    anchors { left: parent.left; leftMargin: 12; bottom: parent.bottom; bottomMargin: 11 }
                    spacing: 7
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16; height: 16; radius: 8
                        color: card.selected ? card.t.accent : "transparent"
                        border.width: card.selected ? 0 : 1.5
                        border.color: Theme.alpha(card.t.text, 0.35)
                        Icon {
                            anchors.centerIn: parent
                            visible: card.selected
                            name: "check"
                            size: 12
                            color: card.t.onAccent
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: card.t.name
                        color: card.t.text
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                }
            }
        }
    }
}
