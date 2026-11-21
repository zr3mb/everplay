import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import Everplay

// Shown once to people coming from Nokturn (the app's former name): the old name breaks apart, a
// shockwave clears the stage and Everplay arrives with what's new. Everything follows one clock, t in
// seconds, so the whole show can be jumped through (Skip) or held on a frame (Nav.introTime, the tour).
Item {
    id: root

    // Used the app before (the wizard is done) and hasn't seen this yet. New users finish the wizard
    // instead, which marks this as seen.
    property bool shown: false
    Component.onCompleted: shown = Prefs.boolValue("onboarding/done", false) && !Prefs.boolValue("everplay/introSeen", false)

    readonly property bool held: Nav.introTime >= 0
    readonly property real end: 6.6
    property real clock: 0
    readonly property real t: held ? Nav.introTime : clock
    property bool leaving: false
    property real leave: 0

    visible: (shown || held) && leave < 1
    enabled: visible
    opacity: 1 - leave
    focus: visible

    NumberAnimation on clock {
        running: root.shown && !root.held && !root.leaving
        from: 0; to: root.end
        duration: Theme.ms(root.end * 1000)
    }
    NumberAnimation {
        id: leaveAnim
        target: root; property: "leave"
        from: 0; to: 1
        duration: Theme.ms(650)
        easing.type: Easing.InCubic
        onFinished: root.shown = false
    }

    function close() {
        if (leaving)
            return
        Prefs.setValue("everplay/introSeen", true)
        leaving = true
        leaveAnim.start()
    }
    function skip() { clock = end }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            t >= end - 0.3 ? close() : skip()
            event.accepted = true
        }
    }

    // ---- the clock's helpers ----
    function seg(a, b) { return Math.max(0, Math.min(1, (t - a) / (b - a))) }
    function outCubic(x) { return 1 - Math.pow(1 - x, 3) }
    function inCubic(x) { return x * x * x }
    function outBack(x) { const c = 1.9; return 1 + (c + 1) * Math.pow(x - 1, 3) + c * Math.pow(x - 1, 2) }
    function bump(a, peak, b) { return t <= a || t >= b ? 0 : t < peak ? (t - a) / (peak - a) : 1 - (t - peak) / (b - peak) }
    function rnd(i, k) { const v = Math.sin(i * 12.9898 + k * 78.233) * 43758.5453; return v - Math.floor(v) }

    readonly property real k: Math.max(0.5, Math.min(1, Math.min(width / 1000, height / 720)))
    readonly property color oldAccent: "#9fb5a8"

    // Nothing behind reacts while this is up.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => wheel.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: "#040508"
    }

    // ---- aurora: the new colours slowly drifting in ----
    component Glow: Shape {
        id: glow
        property color color
        property real size
        width: size; height: size
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: glow.size / 2; centerY: glow.size / 2; centerRadius: glow.size / 2
                focalX: centerX; focalY: centerY
                GradientStop { position: 0; color: glow.color }
                GradientStop { position: 0.45; color: Qt.rgba(glow.color.r, glow.color.g, glow.color.b, glow.color.a * 0.45) }
                GradientStop { position: 1; color: "transparent" }
            }
            PathAngleArc { centerX: glow.size / 2; centerY: glow.size / 2; radiusX: glow.size / 2; radiusY: glow.size / 2; startAngle: 0; sweepAngle: 360 }
        }
    }
    Item {
        anchors.fill: parent
        opacity: root.seg(2.5, 4.2)
        Glow {
            size: Math.max(root.width, root.height) * 1.1
            x: root.width * (0.22 + 0.06 * Math.sin(root.t * 0.6)) - size / 2
            y: root.height * (0.3 + 0.05 * Math.cos(root.t * 0.5)) - size / 2
            color: Theme.alpha(Theme.accent, 0.42)
        }
        Glow {
            size: Math.max(root.width, root.height) * 1.0
            x: root.width * (0.8 + 0.05 * Math.cos(root.t * 0.45)) - size / 2
            y: root.height * (0.75 + 0.06 * Math.sin(root.t * 0.55)) - size / 2
            color: Theme.alpha(Qt.lighter(Theme.accent2, 1.3), 0.45)
        }
    }

    // A faint grid that sweeps forward, like a stage being lit
    Repeater {
        model: 14
        Rectangle {
            required property int index
            width: root.width; height: 1
            y: root.height * (0.55 + Math.pow((index + (root.t * 0.6) % 1) / 14, 1.8) * 0.45)
            color: Theme.alpha(Theme.accent, 0.10 * root.seg(2.8, 4) * (index / 14))
        }
    }

    // ---- act 1: the old name ----
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.height / 2 - 110 * root.k
        text: qsTr("Something is changing…")
        color: Theme.alpha("#ffffff", 0.55)
        font.pixelSize: Math.round(15 * Math.max(0.85, root.k))
        font.letterSpacing: 4
        font.weight: Font.DemiBold
        opacity: root.seg(0.5, 1.0) * (1 - root.seg(1.5, 1.9))
    }

    Row {
        id: oldWord
        anchors.centerIn: parent
        spacing: 2 * root.k
        Repeater {
            model: "Nokturn".split("")
            Text {
                required property string modelData
                required property int index
                readonly property real fly: root.inCubic(root.seg(1.75 + index * 0.035, 2.55 + index * 0.035))
                text: modelData
                color: Qt.tint(root.oldAccent, Theme.alpha("#ffffff", root.bump(1.5, 1.8, 2.2) * 0.8))
                font.family: Theme.fontLight
                font.pixelSize: Math.round(84 * root.k)
                font.weight: Font.Light
                font.letterSpacing: 6 * root.k
                opacity: root.seg(0.15 + index * 0.06, 0.75 + index * 0.06) * (1 - root.seg(2.0 + index * 0.03, 2.6 + index * 0.03))
                transform: [
                    Rotation {
                        origin.x: width / 2; origin.y: height / 2
                        angle: (root.rnd(index, 1) - 0.5) * 220 * fly
                    },
                    Translate {
                        x: (index - 3) * 70 * root.k * fly + (root.rnd(index, 2) - 0.5) * 380 * root.k * fly
                        y: (root.rnd(index, 3) - 0.65) * 420 * root.k * fly + (1 - root.outCubic(root.seg(0.15 + index * 0.06, 0.9 + index * 0.06))) * 24
                    }
                ]
                scale: 1 + fly * 0.8
            }
        }
    }

    // The light that cuts through it
    Rectangle {
        y: root.height / 2 - height / 2
        width: root.width * 0.6; height: 2
        x: -width + (root.width + width) * root.outCubic(root.seg(1.45, 2.15))
        opacity: root.bump(1.4, 1.7, 2.3)
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 0.8; color: "#ffffff" }
            GradientStop { position: 1; color: "transparent" }
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width; height: 26
            opacity: 0.35
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "transparent" }
                GradientStop { position: 0.85; color: Theme.alpha(Theme.accent, 0.8) }
                GradientStop { position: 1; color: "transparent" }
            }
        }
    }

    // ---- act 2: the shockwave ----
    Repeater {
        model: 3
        Rectangle {
            required property int index
            readonly property real p: root.outCubic(root.seg(2.45 + index * 0.14, 3.7 + index * 0.14))
            anchors.centerIn: parent
            width: 30 + Math.hypot(root.width, root.height) * 1.1 * p
            height: width
            radius: width / 2
            color: "transparent"
            border.width: Math.max(1, (8 - index * 2) * (1 - p))
            border.color: index === 1 ? Theme.accent2 : Theme.accent
            opacity: (p > 0 ? 1 : 0) * (1 - p) * 0.9
        }
    }
    Repeater {
        model: 84
        Rectangle {
            required property int index
            readonly property real angle: index / 84 * Math.PI * 2 + root.rnd(index, 4) * 0.2
            readonly property real dist: root.outCubic(root.seg(2.5, 3.6 + root.rnd(index, 5) * 0.6)) * (140 + root.rnd(index, 6) * 560) * root.k
            readonly property real size: 2 + root.rnd(index, 7) * 5
            x: root.width / 2 + Math.cos(angle) * dist - size / 2
            y: root.height / 2 + Math.sin(angle) * dist - size / 2
            width: size; height: size; radius: size / 2
            color: index % 3 === 0 ? "#ffffff" : index % 3 === 1 ? Theme.accent : Qt.lighter(Theme.accent2, 1.4)
            opacity: root.seg(2.48, 2.55) * (1 - root.seg(3.1 + root.rnd(index, 8) * 0.5, 4.3))
        }
    }
    Rectangle {
        anchors.fill: parent
        color: Qt.lighter(Theme.accent, 1.5)
        opacity: root.bump(2.42, 2.5, 2.8) * 0.22
    }

    // ---- act 3: Everplay ----
    Item {
        id: stage
        anchors.fill: parent
        readonly property real rise: root.outCubic(root.seg(3.55, 4.3))
        transform: Translate { y: -110 * root.k * stage.rise }

        // The glow behind the logo, breathing
        Rectangle {
            id: halo
            anchors.centerIn: logo
            width: 360 * root.k * (0.6 + 0.4 * root.outCubic(root.seg(2.7, 3.6))) * (1 + 0.04 * Math.sin(root.t * 3))
            height: width; radius: width / 2
            color: Theme.alpha(Theme.accent, 0.35)
            opacity: root.seg(2.7, 3.4)
            visible: false
        }
        MultiEffect {
            anchors.fill: halo
            source: halo
            opacity: halo.opacity
            blurEnabled: true; blur: 1; blurMax: 64
            autoPaddingEnabled: true
        }

        LogoMark {
            id: logo
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.height / 2 - height / 2 - 70 * root.k * stage.rise
            readonly property real p: root.seg(2.65, 3.5)
            width: 144 * root.k; height: width
            scale: root.outBack(p)
            rotation: -200 * (1 - root.outCubic(p))
            opacity: root.seg(2.65, 2.85)
        }

        // The name, letter by letter, then a shine running over it
        Item {
            id: wordBox
            anchors.horizontalCenter: parent.horizontalCenter
            y: root.height / 2 + 34 * root.k
            width: word.width; height: word.height

            Row {
                id: word
                visible: false   // drawn by the effect below, with its glow
                Repeater {
                    model: "Everplay".split("")
                    Text {
                        required property string modelData
                        required property int index
                        readonly property real p: root.outBack(root.seg(3.75 + index * 0.075, 4.45 + index * 0.075))
                        text: modelData
                        color: "#ffffff"
                        font.family: Theme.fontDisplay
                        font.pixelSize: Math.round(104 * root.k)
                        font.weight: Font.Bold
                        font.letterSpacing: -2 * root.k
                        opacity: root.seg(3.75 + index * 0.075, 4.05 + index * 0.075)
                        transform: [
                            Rotation {
                                origin.x: width / 2; origin.y: height
                                angle: (1 - p) * (root.rnd(index, 9) - 0.5) * 70
                            },
                            Translate { y: (1 - p) * 90 * root.k }
                        ]
                    }
                }
            }
            MultiEffect {
                anchors.fill: word
                source: word
                shadowEnabled: true
                shadowColor: Theme.accent
                shadowBlur: 1
                shadowOpacity: 0.9 * root.seg(4.2, 4.9)
                shadowVerticalOffset: 0
                shadowHorizontalOffset: 0
            }
            Item {
                anchors.fill: word
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: word
                }
                Rectangle {
                    width: parent.width * 0.35; height: parent.height
                    x: -width + (parent.width + width) * root.outCubic(root.seg(4.75, 5.6))
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "transparent" }
                        GradientStop { position: 0.5; color: Theme.alpha(Qt.lighter(Theme.accent, 1.3), 0.95) }
                        GradientStop { position: 1; color: "transparent" }
                    }
                }
            }
        }

        Text {
            id: tagline
            anchors.horizontalCenter: parent.horizontalCenter
            y: wordBox.y + wordBox.height + 14 * root.k + (1 - root.outCubic(root.seg(4.7, 5.3))) * 20
            width: Math.min(root.width - 48, 720)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Nokturn is now Everplay. A new name and a new look — your music, likes and playlists came along.")
            color: Theme.alpha("#ffffff", 0.75)
            font.pixelSize: Math.round(18 * Math.max(0.8, root.k))
            lineHeight: 1.15
            opacity: root.seg(4.7, 5.3)
        }

        Grid {
            id: chips
            anchors.horizontalCenter: parent.horizontalCenter
            y: tagline.y + tagline.height + 22 * root.k
            columns: root.width < 760 ? 1 : 3   // a phone: one under another, centred
            horizontalItemAlignment: Grid.AlignHCenter
            spacing: 10
            Repeater {
                model: [
                    { icon: "sparkle", text: qsTr("Redesigned from scratch") },
                    { icon: "palette", text: qsTr("Rounded or crisp corners") },
                    { icon: "heart", text: qsTr("Your library and settings stay") }
                ]
                Rectangle {
                    required property var modelData
                    required property int index
                    readonly property real p: root.outBack(root.seg(5.15 + index * 0.13, 5.7 + index * 0.13))
                    width: chipRow.implicitWidth + 28
                    height: 38
                    radius: 19
                    color: Theme.alpha(Theme.accent, 0.14)
                    border.width: 1
                    border.color: Theme.alpha(Theme.accent, 0.4)
                    opacity: root.seg(5.15 + index * 0.13, 5.4 + index * 0.13)
                    scale: 0.6 + 0.4 * p
                    Row {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 8
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8; height: 8; radius: 4
                            color: Theme.accent
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            color: "#ffffff"
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
        }

        PillButton {
            id: go
            anchors.horizontalCenter: parent.horizontalCenter
            y: chips.y + chips.height + 30 * root.k + (1 - root.outCubic(root.seg(5.9, 6.4))) * 16
            icon: "play"
            text: qsTr("Let's play")
            opacity: root.seg(5.9, 6.4)
            enabled: opacity > 0.5
            onClicked: root.close()

            // A ring that keeps calling
            Rectangle {
                anchors.centerIn: parent
                z: -1
                readonly property real cycle: root.t > 6.3 ? ((root.t - 6.3) % 1.6) / 1.6 : 0
                width: parent.width + 40 * cycle; height: parent.height + 40 * cycle
                radius: height / 2
                color: "transparent"
                border.width: 2
                border.color: Theme.accent
                opacity: root.t > 6.3 ? (1 - cycle) * 0.6 : 0
            }
        }
    }

    // Keeps the ring calling once the show is over.
    NumberAnimation on clock {
        running: root.shown && !root.held && !root.leaving && root.clock >= root.end
        from: root.end; to: root.end + 1.6
        loops: Animation.Infinite
        duration: 1600
    }

    Text {
        anchors { top: parent.top; right: parent.right; margins: 22 }
        text: qsTr("Skip")
        color: skipHover.hovered ? "#ffffff" : Theme.alpha("#ffffff", 0.5)
        font.pixelSize: 14
        font.weight: Font.DemiBold
        visible: root.t < root.end - 0.4
        HoverHandler { id: skipHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.skip() }
    }
}
