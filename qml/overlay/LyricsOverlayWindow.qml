import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import Everplay

// In-game HUD lyrics overlay: pure floating text directly over the dimmed game screen
// with click-through transparency and configurable full-screen dimming.
Window {
    id: win

    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool | Qt.NoDropShadowWindowHint
           | (Theme.windows ? Qt.WindowDoesNotAcceptFocus : 0)
           | Qt.WindowTransparentForInput
    transientParent: null
    color: "transparent"
    title: "Everplay Lyrics"
    x: GameOverlay.area.x
    y: GameOverlay.area.y
    width: Math.max(640, GameOverlay.area.width)
    height: Math.max(480, GameOverlay.area.height)

    property bool shown: false
    property real fade: shown ? 1 : 0
    Behavior on fade { NumberAnimation { duration: win.shown ? 260 : 180; easing.type: Easing.OutCubic } }
    visible: shown || fade > 0

    property bool prepared: false
    function open() {
        if (!prepared) {
            GameOverlay.prepare(win)
            prepared = true
        }
        shown = true
        GameOverlay.activateLyrics(win)
    }
    Component.onCompleted: if (GameOverlay.lyricsOpen) open()

    Connections {
        target: GameOverlay
        function onLyricsOpenChanged() {
            if (GameOverlay.lyricsOpen)
                win.open()
            else
                win.shown = false
        }
    }

    // ---- Entire screen slightly dimmed (customizable opacity) ----
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: GameOverlay.lyricsDimming * win.fade
        Behavior on opacity { NumberAnimation { duration: Theme.ms(200) } }
    }

    // ---- Floating lyrics text only (no panels, no frames, no borders) ----
    Item {
        id: hud
        anchors.fill: parent
        opacity: win.fade
        scale: 0.98 + 0.02 * win.fade
        Behavior on scale { NumberAnimation { duration: Theme.ms(260); easing.type: Easing.OutCubic } }

        readonly property real s: Math.max(0.7, Math.min(width / 1920, height / 1080))
        readonly property real lyricsTime: Player.position - Lyrics.offset
        readonly property int current: Lyrics.synced ? Lyrics.lineAt(lyricsTime + 0.15) : -1
        readonly property var currentLine: current >= 0 && current < Lyrics.lines.length ? Lyrics.lines[current] : null
        readonly property bool currentActive: currentLine !== null && (currentLine.end < 0 || lyricsTime < currentLine.end)

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

        function karaokeText(text, n) {
            let sung = text.slice(0, n), rest = text.slice(n)
            if (GameOverlay.lyricsHideNotes || GameOverlay.lyricsHidePunctuation) {
                sung = filtered(sung).replace(/\s{2,}/g, " ").replace(/^\s+/, "")
                rest = filtered(rest).replace(/\s{2,}/g, " ").replace(/\s+$/, "")
                if (sung.endsWith(" ") && rest.startsWith(" "))
                    rest = rest.slice(1)
            }
            return "<font color=\"" + String(Theme.accent) + "\">" + escaped(sung) + "</font>" + escaped(rest)
        }

        // What shows while nothing is sung; empty when music notes are turned off (Settings).
        readonly property string pause: GameOverlay.lyricsHideNotes ? "" : "♪ ♪ ♪"

        function isLetter(c) {
            return c !== undefined && c.toLowerCase() !== c.toUpperCase()
        }

        // Removes music notes and/or punctuation, as chosen in Settings. Spaces are kept as they are,
        // so the two halves of a karaoke line join up again.
        function filtered(text) {
            let t = text || ""
            if (GameOverlay.lyricsHideNotes)
                t = t.replace(/[♩-♬]|\uD83C[\uDFB5\uDFB6\uDFBC]/g, "")
            if (GameOverlay.lyricsHidePunctuation) {
                t = t.replace(/[.,!?¿¡;:…"“”„«»‹›()\[\]{}*_~\/\\|<>#]/g, "")
                     .replace(/[-‐‒–—]/g, " ")
                     // Apostrophes stay inside words ("don't") and go elsewhere.
                     .replace(/['‘’`´]/g, (m, i, all) => isLetter(all[i - 1]) && isLetter(all[i + 1]) ? m : "")
            }
            return t
        }

        // A whole line: filtered, with the gaps left behind closed up.
        function shown(text) {
            if (!GameOverlay.lyricsHideNotes && !GameOverlay.lyricsHidePunctuation)
                return text || ""
            return filtered(text).replace(/\s{2,}/g, " ").trim()
        }

        // Parse lines into verse sections (split on empty lines, or chunks of 4)
        readonly property var sections: {
            const lines = Lyrics.lines || []
            if (lines.length === 0)
                return []
            const out = []
            let cur = []
            let startIdx = 0
            const hasBlanks = lines.some(l => (l.text || "").trim() === "")

            if (hasBlanks) {
                for (let i = 0; i < lines.length; ++i) {
                    const l = lines[i]
                    if ((l.text || "").trim() === "") {
                        if (cur.length > 0) {
                            out.push({ startIndex: startIdx, endIndex: i - 1, lines: cur })
                            cur = []
                        }
                        startIdx = i + 1
                    } else {
                        cur.push({ index: i, line: l })
                    }
                }
                if (cur.length > 0)
                    out.push({ startIndex: startIdx, endIndex: lines.length - 1, lines: cur })
            } else {
                for (let i = 0; i < lines.length; i += 4) {
                    const chunk = []
                    for (let j = i; j < Math.min(lines.length, i + 4); ++j)
                        chunk.push({ index: j, line: lines[j] })
                    out.push({ startIndex: i, endIndex: Math.min(lines.length - 1, i + 3), lines: chunk })
                }
            }
            return out
        }

        // Active section based on current line index
        readonly property var activeSection: {
            if (sections.length === 0)
                return null
            if (current < 0)
                return sections[0]
            for (let i = 0; i < sections.length; ++i) {
                const s = sections[i]
                if (current >= s.startIndex && current <= s.endIndex)
                    return s
            }
            return sections[sections.length - 1]
        }

        // Active word for ADHD mode (single word currently being sung, whole word, no sweep)
        readonly property string activeAdhdWord: {
            if (!Player.hasTrack || Lyrics.lines.length === 0)
                return ""
            if (!Lyrics.synced)
                return currentLine ? (currentLine.text || "") : (Lyrics.lines[0] ? Lyrics.lines[0].text : "")

            const t = lyricsTime
            const idx = Lyrics.lineAt(t)
            if (idx < 0 || idx >= Lyrics.lines.length)
                return ""

            const line = Lyrics.lines[idx]
            if (!line)
                return ""

            // If the line has already ended (with a grace window of 0.3s)
            if (line.end >= 0 && t >= line.end + 0.3)
                return ""

            const words = line.words || []
            if (words.length === 0)
                return (line.end < 0 || t < line.end) ? (line.text || "") : ""

            for (let i = 0; i < words.length; ++i) {
                const w = words[i]
                const wStart = w[0]
                const nextStart = (i + 1 < words.length) ? words[i + 1][0] : (line.end >= 0 ? line.end : w[1] + 0.5)

                // Word stays active while sung, connecting smoothly to next word
                // if the gap is <= 0.35s. If pause is longer, clears after w[1] + 0.3s
                const maxHold = Math.min(w[1] + 0.35, nextStart)
                const wordEnd = Math.max(w[1], maxHold)

                if (t >= wStart && t < wordEnd) {
                    return (line.text || "").slice(w[2], w[2] + w[3])
                }
            }

            return ""
        }

        // Position HUD according to GameOverlay.lyricsPosition ("bottom", "center", "top")
        Item {
            id: hudBox
            width: Math.min(hud.width - 64 * hud.s, 1100 * hud.s)
            height: contentLayout.implicitHeight
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: GameOverlay.lyricsPosition === "top" ? parent.top : undefined
            anchors.topMargin: GameOverlay.lyricsPosition === "top" ? 64 * hud.s : 0
            anchors.bottom: GameOverlay.lyricsPosition === "bottom" ? parent.bottom : undefined
            anchors.bottomMargin: GameOverlay.lyricsPosition === "bottom" ? 64 * hud.s : 0
            anchors.verticalCenter: GameOverlay.lyricsPosition === "center" ? parent.verticalCenter : undefined

            ColumnLayout {
                id: contentLayout
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 12 * hud.s

                // ---- State: No track playing ----
                Text {
                    Layout.fillWidth: true
                    visible: !Player.hasTrack
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Play music in Everplay to see lyrics")
                    color: Theme.textDim
                    font.family: Theme.fontDisplay
                    font.pixelSize: 22 * hud.s
                    font.weight: Font.DemiBold
                    style: Text.Outline
                    styleColor: "#e6000000"
                }

                // ---- State: Lyrics loading or missing ----
                Text {
                    Layout.fillWidth: true
                    visible: Player.hasTrack && Lyrics.state !== "ready"
                    horizontalAlignment: Text.AlignHCenter
                    text: Lyrics.state === "loading" ? qsTr("Loading lyrics…")
                        : Lyrics.state === "missing" ? qsTr("No lyrics found for this track")
                        : qsTr("Lyrics unavailable")
                    color: Theme.textDim
                    font.family: Theme.fontDisplay
                    font.pixelSize: 20 * hud.s
                    font.weight: Font.DemiBold
                    style: Text.Outline
                    styleColor: "#e6000000"
                }

                // ==============================================================
                // MODE 1: KARAOKE STYLE (pure text)
                // ==============================================================
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10 * hud.s
                    visible: Player.hasTrack && Lyrics.state === "ready" && GameOverlay.lyricsMode === "karaoke"

                    // Previous line (dimmed preview)
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        visible: text !== ""
                        text: hud.current > 0 && hud.current - 1 < Lyrics.lines.length ? hud.shown(Lyrics.lines[hud.current - 1].text) : ""
                        color: Theme.alpha(Theme.text, 0.42)
                        font.family: Theme.fontDisplay
                        font.pixelSize: 20 * hud.s
                        font.weight: Font.Normal
                        wrapMode: Text.WordWrap
                        style: Text.Outline
                        styleColor: "#e6000000"
                    }

                    // Main active singing line (pure text with outline)
                    Text {
                        id: mainLineText
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        readonly property bool hasWords: (hud.currentLine && hud.currentLine.words && hud.currentLine.words.length > 0)
                        readonly property bool isKaraoke: Lyrics.karaoke && hud.currentActive && hasWords
                        text: {
                            if (!hud.currentLine)
                                return Lyrics.synced ? hud.pause : (Lyrics.lines[0] ? hud.shown(Lyrics.lines[0].text) : "")
                            if (isKaraoke && hud.shown(hud.currentLine.text) !== "")
                                return hud.karaokeText(hud.currentLine.text, hud.sungChars(hud.currentLine, hud.lyricsTime))
                            return hud.shown(hud.currentLine.text) || hud.pause
                        }
                        textFormat: isKaraoke ? Text.StyledText : Text.PlainText
                        color: hud.currentActive ? Theme.text : Theme.alpha(Theme.text, 0.75)
                        font.family: Theme.fontDisplay
                        font.pixelSize: 32 * hud.s
                        font.weight: Font.Bold
                        wrapMode: Text.WordWrap
                        style: Text.Outline
                        styleColor: "#f0000000"
                        scale: hud.currentActive ? 1.0 : 0.96
                        Behavior on scale { NumberAnimation { duration: Theme.ms(200); easing.type: Easing.OutCubic } }
                    }

                    // Upcoming next line (preview)
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        visible: text !== ""
                        text: hud.current >= 0 && hud.current + 1 < Lyrics.lines.length ? hud.shown(Lyrics.lines[hud.current + 1].text) : ""
                        color: Theme.alpha(Theme.text, 0.58)
                        font.family: Theme.fontDisplay
                        font.pixelSize: 22 * hud.s
                        font.weight: Font.Medium
                        wrapMode: Text.WordWrap
                        style: Text.Outline
                        styleColor: "#e6000000"
                    }
                }

                // ==============================================================
                // MODE 2: SECTIONS (pure text verses/stanzas)
                // ==============================================================
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8 * hud.s
                    visible: Player.hasTrack && Lyrics.state === "ready" && GameOverlay.lyricsMode === "sections"

                    Repeater {
                        model: hud.activeSection ? hud.activeSection.lines : []

                        Text {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            readonly property bool isCurrent: modelData.index === hud.current
                            text: hud.shown(modelData.line.text)
                            horizontalAlignment: Text.AlignHCenter
                            color: isCurrent ? (hud.currentActive ? Theme.text : Theme.alpha(Theme.text, 0.85)) : Theme.alpha(Theme.text, 0.50)
                            font.family: Theme.fontDisplay
                            font.pixelSize: (isCurrent ? 26 : 20) * hud.s
                            font.weight: isCurrent ? Font.Bold : Font.Normal
                            wrapMode: Text.WordWrap
                            style: Text.Outline
                            styleColor: "#f0000000"
                            Behavior on font.pixelSize { NumberAnimation { duration: Theme.ms(180) } }
                            Behavior on color { ColorAnimation { duration: Theme.ms(180) } }
                        }
                    }
                }

                // ==============================================================
                // MODE 3: ADHD STYLE (single active word, pure text, no highlight effect)
                // ==============================================================
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8 * hud.s
                    visible: Player.hasTrack && Lyrics.state === "ready" && GameOverlay.lyricsMode === "adhd"

                    Text {
                        id: adhdWordText
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        readonly property string word: hud.shown(hud.activeAdhdWord)
                        text: word !== "" ? word : (Player.playing && Lyrics.synced ? hud.pause : "")
                        color: word !== "" ? Theme.text : Theme.alpha(Theme.text, 0.45)
                        font.family: Theme.fontDisplay
                        font.pixelSize: (word !== "" ? 46 : 28) * hud.s
                        font.weight: Font.Bold
                        fontSizeMode: Text.Fit
                        minimumPixelSize: 22 * hud.s
                        style: Text.Outline
                        styleColor: "#f0000000"
                    }
                }
            }
        }
    }
}
