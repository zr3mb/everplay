import QtQuick
import QtQuick.Layouts
import Everplay

// Listening room (Online): start one or join with a link; inside — who's listening, what's playing,
// the shared queue and the room's mode ("everyone adds" or "the host picks").
Item {
    id: page
    property var args: ({})
    readonly property var room: Online.room
    readonly property bool control: room.canControl === true
    // What's still to come in the room's queue (the current track and the played ones aside).
    readonly property var upcoming: {
        const q = room.queue || []
        let at = -1
        for (let i = 0; i < q.length; ++i)
            if (q[i].current) at = i
        return q.filter((item, i) => !item.played && i > at)
    }
    readonly property var nowPlaying: {
        if (Library.tracks.revision < 0)   // never: re-run when the tracks change
            return ({})
        return room.currentRow >= 0 ? Library.tracks.get(room.currentRow) : ({})
    }

    // Any Everplay link (room, playlist, track) or a room code typed by hand.
    function join(text) {
        if (!Online.openLink(text))
            Online.joinRoom(text)
    }

    component Heading: Text {
        Layout.topMargin: 6
        color: Theme.text
        font.family: Theme.fontDisplay
        font.pixelSize: 20
        font.weight: Font.DemiBold
    }

    component Card: Rectangle {
        default property alias content: cardColumn.data
        Layout.fillWidth: true
        implicitHeight: cardColumn.implicitHeight + 44
        radius: Theme.r(20)
        color: Theme.alpha(Theme.text, 0.04)
        border.width: 1
        border.color: Theme.alpha(Theme.text, 0.06)
        ColumnLayout {
            id: cardColumn
            x: 22
            y: 22
            width: parent.width - 44
            spacing: 12
        }
    }

    component Hint: Text {
        Layout.fillWidth: true
        color: Theme.textFaint
        font.pixelSize: 13
        wrapMode: Text.WordWrap
    }

    PageFlick {
        anchors.fill: parent
        spacing: 26

        // ---- title ----
        Reveal {
            RowLayout {
                width: parent.width
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        text: Online.inRoom ? qsTr("LISTENING ROOM · %1").arg(page.room.code) : qsTr("TOGETHER")
                        color: Theme.accent
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.6
                    }
                    Text {
                        Layout.fillWidth: true
                        text: Online.inRoom ? page.room.name : qsTr("Listening room")
                        color: Theme.text
                        font.family: Theme.fontDisplay
                        font.pixelSize: 38
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }
                }
                PillButton {
                    visible: Online.inRoom
                    kind: "secondary"
                    icon: "link"
                    text: qsTr("Copy invite link")
                    onClicked: Online.copyRoomLink()
                }
                PillButton {
                    visible: Online.inRoom || Online.joiningRoom
                    kind: "ghost"
                    icon: "exit"
                    text: qsTr("Leave")
                    onClicked: Online.leaveRoom()
                }
            }
        }

        // ---- joining ----
        Reveal {
            visible: Online.joiningRoom
            delay: 40
            RowLayout {
                width: parent.width
                spacing: 14
                Spinner { size: 22; color: Theme.accent }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Connecting to the room…")
                    color: Theme.textDim
                    font.pixelSize: 15
                }
            }
        }

        // ---- not in a room: start one or join ----
        Reveal {
            visible: !Online.inRoom && !Online.joiningRoom
            delay: 60
            ColumnLayout {
                width: parent.width
                spacing: 16

                Hint {
                    text: qsTr("Listen to the same music at the same time with friends: everyone hears the same track at the same moment, wherever they are. Send the invite link — whoever opens it joins the room in Everplay.")
                    font.pixelSize: 14
                    color: Theme.textDim
                }

                Card {
                    id: startCard
                    property int mode: 0
                    Heading { text: qsTr("Start a room"); Layout.topMargin: 0 }
                    InputField {
                        id: roomName
                        Layout.fillWidth: true
                        icon: "group"
                        maximumLength: 60
                        placeholderText: qsTr("Room name (optional)")
                        onAccepted: startButton.clicked()
                    }
                    SegmentedTabs {
                        model: [qsTr("Everyone adds"), qsTr("I pick the music")]
                        currentIndex: startCard.mode
                        onActivated: i => startCard.mode = i
                    }
                    Hint {
                        text: startCard.mode === 0
                              ? qsTr("Everyone in the room adds tracks to the queue, skips and pauses.")
                              : qsTr("Only you add tracks and control playback; the others listen along. You can hand the room over to someone else later.")
                    }
                    PillButton {
                        id: startButton
                        icon: "add"
                        text: qsTr("Start the room")
                        busy: Online.busy
                        onClicked: Online.createRoom(roomName.text, startCard.mode === 0 ? "open" : "host")
                    }
                }

                Card {
                    Heading { text: qsTr("Join a room"); Layout.topMargin: 0 }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        InputField {
                            id: joinField
                            Layout.fillWidth: true
                            icon: "link"
                            placeholderText: qsTr("Invite link or room code")
                            onAccepted: if (text.trim() !== "") page.join(text)
                        }
                        PillButton {
                            icon: "group"
                            text: qsTr("Join")
                            enabled: joinField.text.trim() !== ""
                            onClicked: page.join(joinField.text)
                        }
                    }
                    Hint { text: qsTr("Links to shared tracks and playlists work here too — paste any Everplay link.") }
                }
            }
        }

        // ---- in a room ----
        Reveal {
            visible: Online.inRoom
            delay: 60
            ColumnLayout {
                width: parent.width
                spacing: 22

                // Now playing
                Card {
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 18
                        ArtCover {
                            Layout.preferredWidth: 96
                            Layout.preferredHeight: 96
                            radius: Theme.r(14)
                            source: page.nowPlaying.cover || ""
                            colorA: page.nowPlaying.colorA || Theme.accent
                            colorB: page.nowPlaying.colorB || Theme.accent2
                            seed: page.nowPlaying.seed || 0
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            Text {
                                text: page.room.currentRow >= 0
                                      ? (page.room.playing ? qsTr("NOW PLAYING") : qsTr("PAUSED"))
                                      : qsTr("NOTHING PLAYING")
                                color: Theme.textFaint
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 1.4
                            }
                            Text {
                                Layout.fillWidth: true
                                text: page.room.currentRow >= 0 ? page.nowPlaying.title || "" : qsTr("The queue is empty")
                                color: Theme.text
                                font.family: Theme.fontDisplay
                                font.pixelSize: 22
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: page.room.currentRow >= 0 ? page.nowPlaying.artist || ""
                                      : page.control ? qsTr("Play any track or use ⋯ → Add to the listening room — it starts for everyone.")
                                                     : qsTr("The host hasn't picked anything yet.")
                                color: Theme.textDim
                                font.pixelSize: 14
                                elide: Text.ElideRight
                                wrapMode: page.room.currentRow >= 0 ? Text.NoWrap : Text.WordWrap
                            }
                        }
                        Row {
                            visible: page.control && page.room.currentRow >= 0
                            spacing: 6
                            IconButton { icon: "prev"; showDot: false; onClicked: Player.previous() }
                            IconButton {
                                icon: page.room.playing ? "pause" : "play"
                                iconSize: 26
                                showDot: false
                                onClicked: Player.toggle()
                            }
                            IconButton { icon: "next"; showDot: false; onClicked: Player.next() }
                        }
                    }
                }

                // Mode
                SettingRow {
                    icon: page.room.mode === "open" ? "group" : "star"
                    title: page.room.mode === "open" ? qsTr("Everyone adds") : qsTr("The host picks the music")
                    description: page.room.isHost
                                 ? (page.room.mode === "open" ? qsTr("Everyone adds tracks, skips and pauses. Switch so only you control the room.")
                                                              : qsTr("Only you add and control; the others listen along."))
                                 : page.room.mode === "open" ? qsTr("Everyone adds tracks, skips and pauses.")
                                                             : qsTr("The host adds and controls; you listen along.")
                    SegmentedTabs {
                        visible: page.room.isHost === true
                        model: [qsTr("Everyone"), qsTr("Only me")]
                        currentIndex: page.room.mode === "open" ? 0 : 1
                        onActivated: i => Online.setRoomMode(i === 0 ? "open" : "host")
                    }
                }

                // Up next
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Heading { text: qsTr("Up next"); Layout.fillWidth: true }
                        PillButton {
                            visible: page.control && page.upcoming.length > 1
                            Layout.topMargin: 6
                            kind: "ghost"
                            icon: "shuffle"
                            text: qsTr("Shuffle the queue")
                            fontSize: 13
                            implicitHeight: 34
                            onClicked: Online.shuffleRoom()
                        }
                        PillButton {
                            visible: page.control && page.upcoming.length > 0
                            Layout.topMargin: 6
                            kind: "ghost"
                            icon: "close"
                            text: qsTr("Clear the queue")
                            fontSize: 13
                            implicitHeight: 34
                            onClicked: Online.clearRoomQueue()
                        }
                    }
                    Hint {
                        visible: page.upcoming.length === 0
                        text: page.control ? qsTr("Nothing queued. Add tracks with ⋯ → Add to the listening room, or “Play next in the room”.")
                                           : qsTr("Nothing queued.")
                    }
                    Repeater {
                        model: page.upcoming
                        delegate: Item {
                            id: queueRow
                            required property var modelData
                            required property int index
                            readonly property var t: Library.tracks.revision >= 0 ? Library.tracks.get(modelData.row) : ({})
                            Layout.fillWidth: true
                            implicitHeight: 58
                            HoverHandler { id: rowHover }
                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.r(12)
                                color: Theme.alpha(Theme.text, rowHover.hovered ? 0.06 : 0)
                                Behavior on color { ColorAnimation { duration: Theme.fast } }
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: page.control
                                cursorShape: page.control ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onDoubleClicked: Online.playInRoom(queueRow.modelData.itemId)
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 14
                                Text {
                                    Layout.preferredWidth: 22
                                    text: queueRow.index + 1
                                    color: Theme.textFaint
                                    font.pixelSize: 13
                                    horizontalAlignment: Text.AlignRight
                                    font.features: { "tnum": 1 }
                                }
                                ArtCover {
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 42
                                    width: 42
                                    height: 42
                                    radius: Theme.r(8)
                                    source: queueRow.t.cover || ""
                                    colorA: queueRow.t.colorA || Theme.accent
                                    colorB: queueRow.t.colorB || Theme.accent2
                                    seed: queueRow.t.seed || 0
                                    decorated: false
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: queueRow.t.title || ""
                                        color: Theme.text
                                        font.pixelSize: 14
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: queueRow.t.artist || ""
                                        color: Theme.textDim
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                    }
                                }
                                Text {
                                    text: queueRow.modelData.addedBy ? qsTr("added by %1").arg(queueRow.modelData.addedBy) : ""
                                    color: Theme.textFaint
                                    font.pixelSize: 12
                                }
                                Text {
                                    Layout.preferredWidth: 44
                                    text: queueRow.t.durationText || ""
                                    color: Theme.textFaint
                                    font.pixelSize: 12
                                    horizontalAlignment: Text.AlignRight
                                }
                                IconButton {
                                    visible: page.control
                                    opacity: rowHover.hovered ? 1 : 0
                                    icon: "play"
                                    iconSize: 18
                                    showDot: false
                                    onClicked: Online.playInRoom(queueRow.modelData.itemId)
                                }
                                IconButton {
                                    visible: page.control
                                    opacity: rowHover.hovered ? 1 : 0
                                    icon: "close"
                                    iconSize: 18
                                    showDot: false
                                    onClicked: Online.removeFromRoom(queueRow.modelData.itemId)
                                }
                            }
                        }
                    }
                }

                // Who's here
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Heading { text: qsTr("Listening — %n", "", (page.room.members || []).filter(m => m.online).length) }
                    Repeater {
                        model: page.room.members || []
                        delegate: Item {
                            id: memberRow
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: 48
                            opacity: modelData.online ? 1 : 0.5
                            HoverHandler { id: memberHover }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 12
                                Rectangle {
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 32
                                    radius: Theme.r(16)
                                    color: Theme.alpha(Theme.accent, memberRow.modelData.me ? 0.3 : 0.14)
                                    Text {
                                        anchors.centerIn: parent
                                        visible: !memberPhoto.hasPicture
                                        text: (memberRow.modelData.name || "?").charAt(0).toUpperCase()
                                        color: Theme.text
                                        font.pixelSize: 14
                                        font.weight: Font.DemiBold
                                    }
                                    // Their profile picture, when they have one.
                                    ArtCover {
                                        id: memberPhoto
                                        anchors.fill: parent
                                        visible: hasPicture
                                        radius: width / 2
                                        decorated: false
                                        source: memberRow.modelData.avatar || ""
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: memberRow.modelData.name + (memberRow.modelData.me ? " " + qsTr("(you)") : "")
                                          + (memberRow.modelData.online ? "" : " · " + qsTr("reconnecting"))
                                    color: Theme.text
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                }
                                Row {
                                    visible: memberRow.modelData.host
                                    spacing: 6
                                    Icon { name: "star"; size: 16; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                                    Text {
                                        text: qsTr("host")
                                        color: Theme.accent
                                        font.pixelSize: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                PillButton {
                                    visible: page.room.isHost === true && !memberRow.modelData.me && memberHover.hovered
                                    kind: "ghost"
                                    icon: "star"
                                    text: qsTr("Make host")
                                    fontSize: 12
                                    implicitHeight: 32
                                    onClicked: Online.makeHost(memberRow.modelData.id)
                                }
                                PillButton {
                                    visible: page.room.isHost === true && !memberRow.modelData.me && memberHover.hovered
                                    kind: "ghost"
                                    icon: "close"
                                    text: qsTr("Remove")
                                    fontSize: 12
                                    implicitHeight: 32
                                    onClicked: Online.kick(memberRow.modelData.id)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
