import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import Everplay

// The player: a floating glass dock along the bottom of the window, tinted by the cover.
Item {
    id: root

    Connections {
        target: Player
        function onCurrentChanged() { swap.restart() }
    }

    ParallelAnimation {
        id: swap
        NumberAnimation { target: cover; property: "scale"; from: 0.82; to: 1; duration: Theme.ms(560); easing.type: Easing.OutBack }
        NumberAnimation { target: info; property: "opacity"; from: 0; to: 1; duration: Theme.ms(420); easing.type: Easing.OutCubic }
        NumberAnimation { target: infoShift; property: "x"; from: 16; to: 0; duration: Theme.ms(600); easing.type: Easing.OutQuint }
    }

    // The dock itself: shadow, glass, a glow of the cover colour from the left
    Rectangle {
        id: dock
        anchors.fill: parent
        radius: Theme.radiusL + 2
        color: Theme.glassHi
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowBlur: 1.0
            shadowOpacity: 0.55
            shadowVerticalOffset: 10
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: dock.radius
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.alpha(Theme.accent, 0.22) }
            GradientStop { position: 0.38; color: Theme.alpha(Theme.accent, 0.05) }
            GradientStop { position: 1.0; color: Theme.alpha(Theme.accent2, 0.10) }
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: dock.radius
        color: "transparent"
        border.width: 1
        border.color: Theme.alpha(Theme.text, 0.09)
    }

    // ---- Left: current track ------------------------------------------------
    Item {
        id: left
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        width: Math.min(360, root.width * 0.27)
        height: 64

        ArtCover {
            id: cover
            width: 62; height: 62
            anchors.verticalCenter: parent.verticalCenter
            radius: Theme.r(14)
            colorA: Player.colorA
            colorB: Player.colorB
            seed: Player.seed
            source: Player.cover

            Rectangle {
                anchors.fill: parent
                radius: Theme.r(14)
                color: "#80000000"
                opacity: coverMouse.containsMouse ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
                Icon {
                    anchors.centerIn: parent
                    name: "expand"
                    size: 20
                    color: "#ffffff"
                }
            }
            MouseArea {
                id: coverMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Nav.immersive = true
            }
        }

        Column {
            id: info
            anchors {
                left: cover.right; leftMargin: 14
                right: likeBtn.left; rightMargin: 6
                verticalCenter: parent.verticalCenter
            }
            spacing: 3
            transform: Translate { id: infoShift }

            Text {
                width: parent.width
                text: Player.hasTrack ? Player.title : qsTr("Nothing playing")
                color: Theme.text
                font.family: Theme.fontDisplay
                font.pixelSize: 16
                font.weight: Font.Bold
                elide: Text.ElideRight
            }
            Row {
                width: parent.width
                spacing: 8
                // During a mix: a short note about the transition
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: Player.transitioning
                    width: mixRow.implicitWidth + 14
                    height: 20
                    radius: 10
                    color: Theme.alpha(Theme.accent, 0.22)
                    Row {
                        id: mixRow
                        anchors.centerIn: parent
                        spacing: 4
                        Icon { name: "mix"; size: 12; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: Player.transitionLabel || qsTr("Mix")
                            color: Theme.accent
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                    }
                }
                ArtistLink {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(0, parent.width - x)
                    text: Player.artist
                    artistId: Player.artistId
                    linkEnabled: Player.hasTrack
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
            }
        }

        IconButton {
            id: likeBtn
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            icon: Player.liked ? "heart" : "heartOutline"
            active: Player.liked
            enabled: Player.hasTrack
            showDot: false
            onClicked: {
                Player.toggleLiked()
                Nav.toast(Player.liked ? qsTr("Added to Liked") : qsTr("Removed from Liked"), "heart")
            }
        }
    }

    // ---- Center: controls and progress ---------------------------------------
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(620, root.width * 0.38)
        spacing: 4

        Row {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: "shuffle"
                iconSize: 19
                active: Player.shuffle && !Player.inRoom
                enabled: !Player.inRoom   // a listening room plays its queue in order
                opacity: enabled ? 1 : 0.35
                onClicked: Player.shuffle = !Player.shuffle
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: "prev"
                iconSize: 26
                color: Theme.text
                showDot: false
                onClicked: Player.previous()
            }
            PlayButton {
                anchors.verticalCenter: parent.verticalCenter
                size: 46
                bgColor: Theme.accent
                fgColor: Theme.onAccent
                onClicked: Player.toggle()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: "next"
                iconSize: 26
                color: Theme.text
                showDot: false
                onClicked: Player.next()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                icon: Player.repeatMode === 2 ? "repeatOne" : "repeat"
                iconSize: 19
                active: Player.repeatMode > 0 && !Player.inRoom
                enabled: !Player.inRoom
                opacity: enabled ? 1 : 0.35
                onClicked: Player.cycleRepeat()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Text {
                Layout.preferredWidth: 40
                horizontalAlignment: Text.AlignRight
                text: Player.live ? "" : Theme.formatTime(Player.position)
                color: Theme.textDim
                font.pixelSize: 11
                font.weight: Font.Medium
                font.features: { "tnum": 1 }
            }
            ProgressSlider {
                Layout.fillWidth: true
                enabled: !Player.live
                value: Player.live ? 1 : Player.position / Math.max(1, Player.duration)
                labelFor: function(v) { return Theme.formatTime(v * Player.duration) }
                onMoved: v => Player.seek(v * Player.duration)
            }
            Text {
                Layout.preferredWidth: 40
                text: Player.live ? qsTr("LIVE") : Theme.formatTime(Player.duration)
                color: Player.live ? Theme.warning : Theme.textDim
                font.pixelSize: 11
                font.weight: Player.live ? Font.Bold : Font.Medium
                font.features: { "tnum": 1 }
            }
        }
    }

    // ---- Right: tools in a quiet group, then the volume ------------------------
    Row {
        anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
        spacing: 10

        Visualizer {
            anchors.verticalCenter: parent.verticalCenter
            bars: 10
            barWidth: 3
            spacing: 2
            height: 22
            width: implicitWidth
            minHeight: 3
            opacity: Player.playing ? 1 : 0.3
            Behavior on opacity { NumberAnimation { duration: Theme.ms(500) } }
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: tools.implicitWidth + 8
            height: 44
            radius: 22
            color: Theme.alpha(Theme.text, 0.05)

            Row {
                id: tools
                anchors.centerIn: parent
                IconButton {
                    visible: Player.inRoom
                    icon: "group"
                    iconSize: 18
                    active: true
                    onClicked: Nav.navigate("room")
                }
                IconButton {
                    // A radio of what plays: tracks like it follow, on and on.
                    icon: "radio"
                    iconSize: 18
                    enabled: Player.hasTrack && !Player.inRoom
                    active: Player.queueContext.startsWith("radio:")
                    onClicked: Player.startRadio(Player.currentIndex)
                }
                IconButton {
                    icon: "share"
                    iconSize: 17
                    enabled: Player.hasTrack
                    onClicked: Online.shareCurrent()
                }
                IconButton {
                    icon: "equalizer"
                    iconSize: 18
                    active: Nav.page === "equalizer" || (Equalizer.enabled && Equalizer.preset !== "Flat")
                    onClicked: Nav.navigate("equalizer")
                }
                IconButton {
                    icon: "lyrics"
                    iconSize: 18
                    active: Nav.queueOpen && Nav.panelTab === "lyrics"
                    onClicked: {
                        if (Nav.queueOpen && Nav.panelTab === "lyrics") {
                            Nav.queueOpen = false
                        } else {
                            Nav.panelTab = "lyrics"
                            Nav.queueOpen = true
                        }
                    }
                }
                IconButton {
                    icon: "queue"
                    iconSize: 18
                    active: Nav.queueOpen && Nav.panelTab === "queue"
                    onClicked: {
                        if (Nav.queueOpen && Nav.panelTab === "queue") {
                            Nav.queueOpen = false
                        } else {
                            Nav.panelTab = "queue"
                            Nav.queueOpen = true
                        }
                    }
                }
                IconButton {
                    icon: "expand"
                    iconSize: 17
                    onClicked: Nav.immersive = true
                }
            }
        }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            icon: Player.muted || Player.volume === 0 ? "volumeOff" : Player.volume < 0.5 ? "volumeDown" : "volumeUp"
            iconSize: 19
            showDot: false
            onClicked: Player.muted = !Player.muted
        }
        ProgressSlider {
            anchors.verticalCenter: parent.verticalCenter
            width: 104
            value: Player.muted ? 0 : Player.volume
            labelFor: function(v) { return Math.round(v * 100) + "%" }
            onDragging: v => { Player.muted = false; Player.volume = v }
            onMoved: v => { Player.muted = false; Player.volume = v }
        }
    }
}
