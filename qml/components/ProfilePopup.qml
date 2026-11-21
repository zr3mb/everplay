import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Everplay

// The box under the profile button in the title bar: the user's picture, name and bio. Kept on this
// computer only for now (Profile); changes are saved when the box closes.
Popup {
    id: root
    width: 340
    padding: 20
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function save() {
        Profile.name = nameField.text
        Profile.bio = bioField.text
    }
    onAboutToShow: {
        nameField.text = Profile.name
        bioField.text = Profile.bio
        pictureError.visible = false
    }
    onAboutToHide: save()

    background: Rectangle {
        radius: Theme.r(20)
        color: Theme.surfaceHi
        border.width: 1
        border.color: Theme.panelBorder
    }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.ms(180) }
        NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.ms(280); easing.type: Easing.OutBack }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.ms(140) } }

    contentItem: ColumnLayout {
        spacing: 14

        // Picture: click to choose another
        Item {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 96
            implicitHeight: 96

            ProfileAvatar {
                anchors.fill: parent
                iconSize: 44
            }
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Qt.rgba(0, 0, 0, 0.55)
                opacity: pictureMouse.containsMouse ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                Column {
                    anchors.centerIn: parent
                    spacing: 2
                    Icon { anchors.horizontalCenter: parent.horizontalCenter; name: "image"; size: 22; color: "#ffffff" }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Change")
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                }
            }
            MouseArea {
                id: pictureMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: pictureDialog.open()
            }
        }

        Text {
            id: pictureError
            visible: false
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("That file couldn't be opened as a picture.")
            color: Theme.danger
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
        Text {
            visible: Profile.picture.toString() !== ""
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: -6
            text: qsTr("Remove picture")
            color: removeMouse.containsMouse ? Theme.text : Theme.textDim
            font.pixelSize: 12
            MouseArea {
                id: removeMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Profile.removePicture()
            }
        }

        Text {
            text: qsTr("Name")
            color: Theme.textDim
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
        InputField {
            id: nameField
            Layout.fillWidth: true
            Layout.topMargin: -8
            implicitHeight: 44
            icon: "person"
            maximumLength: Profile.nameLimit
            placeholderText: qsTr("Your name")
            onAccepted: root.save()
        }

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Bio")
                color: Theme.textDim
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Text {
                text: bioField.length + " / " + Profile.bioLimit
                color: bioField.length >= Profile.bioLimit ? Theme.warning : Theme.textFaint
                font.pixelSize: 11
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: -8
            implicitHeight: 104
            radius: Theme.r(14)
            color: bioField.activeFocus ? Theme.alpha(Theme.surface, 0.9) : Theme.fill
            border.width: 1
            border.color: bioField.activeFocus ? Theme.alpha(Theme.accent, 0.7) : Theme.hairline
            Behavior on border.color { ColorAnimation { duration: Theme.fast } }

            Flickable {
                id: bioFlick
                anchors { fill: parent; margins: 4 }
                clip: true
                contentHeight: bioField.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                TextArea.flickable: TextArea {
                    id: bioField
                    wrapMode: TextEdit.Wrap
                    color: Theme.text
                    placeholderText: qsTr("A few words about you (optional)")
                    placeholderTextColor: Theme.textFaint
                    selectionColor: Theme.alpha(Theme.accent, 0.4)
                    font.pixelSize: 13
                    background: null
                    // TextArea has no length limit of its own.
                    onTextChanged: if (length > Profile.bioLimit) remove(Profile.bioLimit, length)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Your profile is kept on this computer only.")
            color: Theme.textFaint
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            PillButton {
                kind: "ghost"
                icon: "tune"
                text: qsTr("Settings")
                onClicked: {
                    root.close()
                    Nav.navigate("settings")
                }
            }
            Item { Layout.fillWidth: true }
            PillButton {
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }

    FileDialog {
        id: pictureDialog
        title: qsTr("Choose a profile picture")
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp *.bmp *.gif)")]
        onAccepted: pictureError.visible = !Profile.setPicture(selectedFile)
    }
}
