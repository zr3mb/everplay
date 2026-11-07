import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Everplay

// Lyrics of the playing track. With timestamps: the current line is highlighted and smoothly scrolled
// to the center, and clicking a line seeks the track. Without timestamps: plain text to read
// (optionally with a button for timing by speech recognition — alpha).
Item {
    id: root
    property real fontSize: 17
    property real lineSpacing: 10
    property bool large: false
    property int align: Text.AlignLeft

    // Position in the lyrics' own timeline (the manual correction shifts it).
    readonly property real lyricsTime: Player.position - Lyrics.offset
    readonly property int current: Lyrics.synced ? Lyrics.lineAt(lyricsTime + 0.15) : -1
    readonly property var currentLine: current >= 0 && current < Lyrics.lines.length ? Lyrics.lines[current] : null
    // After the last word of a line (e.g. in an instrumental break) the highlight fades out.
    readonly property bool currentActive: currentLine !== null && (currentLine.end < 0 || lyricsTime < currentLine.end)

    // Manual scrolling pauses following for a few seconds.
    property bool follow: true
    Timer { id: resumeFollow; interval: 4000; onTriggered: { root.follow = true; root.recenter() } }

    function recenter() {
        if (!follow || current < 0 || list.moving)
            return
        const item = list.itemAtIndex(current)
        if (!item)
            return list.positionViewAtIndex(current, ListView.Center)
        const target = item.y + item.height / 2 - list.height * 0.42
        scroll.to = Math.max(list.originY, Math.min(target, list.contentHeight + list.originY - list.height))
        scroll.restart()
    }
    onCurrentChanged: recenter()

    // Karaoke: how many characters of a line are sung by `time` (letter by letter within each word).
    function sungChars(line, time) {
        const words = line.words || []
        let n = 0
        for (let i = 0; i < words.length; ++i) {
            const w = words[i]
            if (time < w[0])
                break
            n = time >= w[1] ? w[2] + w[3] : w[2] + Math.floor(w[3] * (time - w[0]) / Math.max(0.05, w[1] - w[0]))
        }
        return n
    }
    function escaped(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function karaoke(text, n) {
        return "<font color=\"" + String(Theme.text) + "\">" + escaped(text.slice(0, n)) + "</font>" + escaped(text.slice(n))
    }
    onHeightChanged: recenter()
    Connections {
        target: Lyrics
        function onLyricsChanged() {
            root.follow = true
            list.positionViewAtBeginning()
        }
    }

    NumberAnimation { id: scroll; target: list; property: "contentY"; duration: Theme.ms(620); easing.type: Easing.OutCubic }

    // ---- states without lyrics ----------------------------------------------
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 320)
        spacing: 12
        visible: Lyrics.state !== "ready"

        Spinner {
            Layout.alignment: Qt.AlignHCenter
            visible: Lyrics.state === "loading"
            size: 22
            color: Theme.textDim
        }
        Icon {
            Layout.alignment: Qt.AlignHCenter
            visible: Lyrics.state === "missing" || Lyrics.state === "error" || Lyrics.state === "none"
            name: Lyrics.state === "error" ? "close" : "lyrics"
            size: 26
            color: Theme.textFaint
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: Lyrics.state === "loading" ? qsTr("Looking for lyrics…")
                : Lyrics.state === "missing" ? qsTr("No lyrics for this track")
                : Lyrics.state === "error" ? qsTr("Couldn't download the lyrics")
                : qsTr("Play a track to see its lyrics")
            color: Theme.textDim
            font.pixelSize: 14
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: Lyrics.state === "error" && Lyrics.error !== ""
            horizontalAlignment: Text.AlignHCenter
            text: Lyrics.error
            color: Theme.textFaint
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
        PillButton {
            Layout.alignment: Qt.AlignHCenter
            visible: (Lyrics.state === "error" || Lyrics.state === "missing") && !Lyrics.recognizing
            kind: "ghost"
            icon: "refresh"
            text: qsTr("Try again")
            fontSize: 13
            implicitHeight: 34
            onClicked: Lyrics.retry()
        }

        // No lyrics anywhere: recognize them from the recording (test feature)
        PillButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            visible: Lyrics.canTranscribe
            kind: "secondary"
            icon: "lyrics"
            text: qsTr("Recognize the lyrics (test)")
            fontSize: 13
            implicitHeight: 34
            onClicked: Lyrics.transcribeNow()
        }
        PillButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 6
            visible: Lyrics.state === "missing" && Lyrics.recognizeHardwareOk && !Lyrics.sttRecognizeReady && !Lyrics.recognizing
            kind: "ghost"
            icon: "lyrics"
            text: qsTr("Set up lyrics recognition (test)")
            fontSize: 12
            implicitHeight: 30
            onClicked: Nav.navigate("settings")
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: Lyrics.state === "missing" && Lyrics.recognizing
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Spinner { size: 14; color: Theme.accent }
                Text {
                    Layout.fillWidth: true
                    text: (Lyrics.alignStage || qsTr("Recognizing the singing")) + " · " + Math.round(Lyrics.alignProgress * 100) + "%"
                    color: Theme.textDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                IconButton {
                    icon: "close"
                    iconSize: 14
                    implicitWidth: 26; implicitHeight: 26
                    showDot: false
                    onClicked: Lyrics.cancelAlign()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                height: 3
                radius: 1.5
                color: Theme.alpha(Theme.text, 0.08)
                Rectangle {
                    width: parent.width * Lyrics.alignProgress
                    height: parent.height
                    radius: 1.5
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: Theme.ms(300) } }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            visible: Lyrics.canTranscribe || (Lyrics.state === "missing" && Lyrics.recognizing)
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Test feature: Everplay listens to the recording on this computer and writes down the words with their timing. Expect mistakes, especially over loud music.")
                  + " " + (Lyrics.sttUsesGpu ? qsTr("On your graphics card it takes a few seconds.") : qsTr("It takes a few minutes."))
            color: Theme.warning
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }
    }

    // ---- lyrics ---------------------------------------------------------------
    ListView {
        id: list
        anchors { left: parent.left; right: parent.right; top: parent.top; bottom: footer.top; bottomMargin: 8 }
        visible: Lyrics.state === "ready"
        clip: true
        model: Lyrics.lines
        spacing: root.lineSpacing
        boundsBehavior: Flickable.StopAtBounds
        header: Item { height: Lyrics.synced ? list.height * 0.3 : 4 }
        footer: Item { height: Lyrics.synced ? list.height * 0.45 : 12 }
        ScrollBar.vertical: ThinScrollBar {}

        onMovementStarted: { root.follow = false; scroll.stop(); resumeFollow.stop() }
        onMovementEnded: resumeFollow.restart()

        delegate: Item {
            id: line
            required property var modelData
            required property int index
            readonly property bool blank: modelData.text === ""
            readonly property bool isCurrent: index === root.current
            readonly property bool past: Lyrics.synced && root.current >= 0 && index < root.current
            // The line being sung lights up letter by letter.
            readonly property bool karaoke: Lyrics.karaoke && line.isCurrent && root.currentActive && (modelData.words || []).length > 0
            width: ListView.view.width
            height: blank ? root.fontSize * 0.6 : label.implicitHeight + 6

            Text {
                id: label
                width: parent.width
                y: 3
                visible: !line.blank
                text: line.karaoke ? root.karaoke(line.modelData.text, root.sungChars(line.modelData, root.lyricsTime))
                                   : line.modelData.text
                textFormat: line.karaoke ? Text.StyledText : Text.PlainText
                horizontalAlignment: root.align
                wrapMode: Text.WordWrap
                font.family: root.large ? Theme.fontDisplay : Theme.fontText
                font.pixelSize: root.fontSize
                font.weight: Lyrics.synced ? Font.Bold : Font.Medium
                lineHeight: 1.1
                color: !Lyrics.synced ? Theme.text
                     : line.karaoke ? Theme.alpha(Theme.text, 0.5)
                     : line.isCurrent && root.currentActive ? Theme.text
                     : Theme.alpha(Theme.text, line.past || line.isCurrent ? 0.42 : 0.3)
                Behavior on color { ColorAnimation { duration: Theme.ms(260) } }
                scale: line.isCurrent && root.currentActive ? 1.0 : (Lyrics.synced ? 0.97 : 1.0)
                transformOrigin: root.align === Text.AlignHCenter ? Item.Center : Item.Left
                Behavior on scale { NumberAnimation { duration: Theme.ms(360); easing.type: Easing.OutCubic } }
            }

            // A soft glow under the current line in the accent color
            Rectangle {
                anchors { left: label.left; bottom: label.bottom; bottomMargin: -3 }
                visible: Lyrics.synced && !line.blank
                width: line.isCurrent && root.currentActive ? Math.min(label.paintedWidth, 48) : 0
                height: 3
                radius: 1.5
                color: Theme.accent
                x: root.align === Text.AlignHCenter ? (label.width - width) / 2 : 0
                Behavior on width { NumberAnimation { duration: Theme.ms(420); easing.type: Easing.OutCubic } }
            }

            HoverHandler { id: lineHover; enabled: Lyrics.synced && !line.blank; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                enabled: Lyrics.synced && !line.blank && line.modelData.start >= 0
                onTapped: {
                    Player.seek(line.modelData.start + Lyrics.offset)
                    root.follow = true
                }
            }
        }
    }

    // ---- footer: source, timing (alpha) ----------------------------------------
    ColumnLayout {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        spacing: 8
        visible: Lyrics.state === "ready"
        height: visible ? implicitHeight : 0

        // Speech recognition progress
        ColumnLayout {
            Layout.fillWidth: true
            visible: Lyrics.aligning
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Spinner { size: 14; color: Theme.accent }
                Text {
                    Layout.fillWidth: true
                    text: (Lyrics.alignStage || qsTr("Aligning the timing")) + " · " + Math.round(Lyrics.alignProgress * 100) + "%"
                    color: Theme.textDim
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                IconButton {
                    icon: "close"
                    iconSize: 14
                    implicitWidth: 26; implicitHeight: 26
                    showDot: false
                    onClicked: Lyrics.cancelAlign()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                height: 3
                radius: 1.5
                color: Theme.alpha(Theme.text, 0.08)
                Rectangle {
                    width: parent.width * Lyrics.alignProgress
                    height: parent.height
                    radius: 1.5
                    color: Theme.accent
                    Behavior on width { NumberAnimation { duration: Theme.ms(300) } }
                }
            }
        }

        // Manual timing correction: the lyrics are early / late against this version of the track.
        RowLayout {
            visible: Lyrics.synced
            Layout.fillWidth: true
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: Lyrics.offset === 0 ? qsTr("Lyrics out of sync?")
                      : (Lyrics.offset > 0 ? qsTr("Lyrics %1 s later") : qsTr("Lyrics %1 s earlier"))
                            .arg(Math.abs(Lyrics.offset).toLocaleString(Qt.locale(), "f", 1))
                color: Lyrics.offset === 0 ? Theme.textFaint : Theme.textDim
                font.pixelSize: 11
                elide: Text.ElideRight
                horizontalAlignment: root.align
            }
            PillButton {
                kind: "ghost"
                text: qsTr("Earlier")
                fontSize: 11
                implicitHeight: 26
                onClicked: Lyrics.nudge(-0.5)
            }
            PillButton {
                kind: "ghost"
                text: qsTr("Later")
                fontSize: 11
                implicitHeight: 26
                onClicked: Lyrics.nudge(0.5)
            }
            IconButton {
                visible: Lyrics.offset !== 0
                icon: "refresh"
                iconSize: 13
                implicitWidth: 26; implicitHeight: 26
                showDot: false
                onClicked: Lyrics.resetOffset()
            }
        }

        PillButton {
            visible: Lyrics.canAlign && !Lyrics.aligning
            kind: "secondary"
            icon: "clock"
            text: qsTr("Align the timing (alpha)")
            fontSize: 12
            implicitHeight: 30
            onClicked: Lyrics.alignNow()
        }
        // Lines with times, words only estimated: speech recognition times each word (test feature).
        PillButton {
            visible: Lyrics.canTimeWords && !Lyrics.aligning
            kind: "ghost"
            icon: "clock"
            text: qsTr("Time every word (test)")
            fontSize: 11
            implicitHeight: 26
            onClicked: Lyrics.timeWordsNow()
        }
        // Lyrics or timing from whisper (before forced alignment): often wrong, so they can go.
        PillButton {
            visible: Lyrics.madeByWhisper && !Lyrics.aligning
            kind: "ghost"
            icon: "refresh"
            text: Lyrics.origin === "stt-text" ? qsTr("Remove the recognized lyrics") : qsTr("Remove the old timing")
            fontSize: 11
            implicitHeight: 26
            onClicked: Lyrics.retry()
        }

        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: {
                const parts = []
                if (Lyrics.source !== "")
                    parts.push(Lyrics.source)
                if (Lyrics.origin === "stt")
                    parts.push(qsTr("timing aligned automatically (alpha)"))
                else if (!Lyrics.synced)
                    parts.push(qsTr("not synced"))
                return parts.join(" · ")
            }
            // Recognized lyrics (test feature) stand out: they may be wrong.
            color: Lyrics.origin === "stt-text" ? Theme.warning : Theme.textFaint
            font.pixelSize: 11
            wrapMode: Text.WordWrap
            horizontalAlignment: root.align
        }
    }

}
