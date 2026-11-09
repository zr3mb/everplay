pragma Singleton
import QtQuick
import Everplay

// Palette: one of the themes below + an accent taken from the current track's cover art.
Item {
    id: theme

    // Every theme is dark: the covers, overlays and white-on-art text stay readable in all of them.
    // fixedAccent: the theme keeps its own accent instead of following the cover.
    readonly property var builtinThemes: ({
        everplay: { name: "Everplay", bg: "#08080b", surface: "#121217", surfaceHi: "#1b1b22",
                   text: "#f5f5f7", textDim: "#a4a4ae", textFaint: "#6a6a76",
                   accent: "#9aa6ff", accent2: "#5a3fd6", onAccent: "#0a0a12", fixedAccent: false },
        midnight: { name: "Midnight", bg: "#000000", surface: "#0c0c0f", surfaceHi: "#16161b",
                    text: "#f2f2f5", textDim: "#9e9eaa", textFaint: "#60606b",
                    accent: "#72b8ff", accent2: "#1c3f91", onAccent: "#050a14", fixedAccent: false },
        sunset: { name: "Sunset", bg: "#110b0a", surface: "#1b1312", surfaceHi: "#261b19",
                  text: "#fcf0e8", textDim: "#c4aa9c", textFaint: "#806b61",
                  accent: "#ff9d6e", accent2: "#a7335c", onAccent: "#1c0f0b", fixedAccent: false },
        cat: { name: "Cat", bg: "#110914", surface: "#1d1121", surfaceHi: "#2a172f",
                  text: "#fbeaf6", textDim: "#c9a3c2", textFaint: "#8a6a86",
                  accent: "#ff7ad6", accent2: "#8a3fd0", onAccent: "#2a0a23", fixedAccent: true }
    })
    // Plugins add their own ("<plugin>/<theme>"); a missing colour comes from the Everplay theme.
    readonly property var themes: {
        const all = Object.assign({}, builtinThemes)
        const extra = Plugins.themes
        for (const id in extra)
            all[id] = Object.assign({}, builtinThemes.everplay, { fixedAccent: true }, extra[id])
        return all
    }
    readonly property var themeIds: ["everplay", "midnight", "sunset", "cat"].concat(Object.keys(Plugins.themes))

    // "nokturn": the default theme's id under the app's former name.
    property string themeId: Prefs.stringValue("ui/theme", "everplay").replace(/^nokturn$/, "everplay")
    onThemeIdChanged: Prefs.setValue("ui/theme", themeId)
    readonly property var preset: themes[themeId] || themes.everplay
    readonly property bool cat: themeId === "cat"

    property color bg: preset.bg
    property color surface: preset.surface
    property color surfaceHi: preset.surfaceHi
    property color text: preset.text
    property color textDim: preset.textDim
    property color textFaint: preset.textFaint
    property color onAccent: preset.onAccent
    Behavior on bg { ColorAnimation { duration: 600 } }
    Behavior on surface { ColorAnimation { duration: 600 } }
    Behavior on surfaceHi { ColorAnimation { duration: 600 } }
    Behavior on text { ColorAnimation { duration: 600 } }
    Behavior on textDim { ColorAnimation { duration: 600 } }
    Behavior on textFaint { ColorAnimation { duration: 600 } }
    readonly property color success: "#7fd6a0"
    readonly property color danger: "#ff7b86"
    readonly property color warning: "#ffcc7a"

    // Plugins may bind these colours (docs/plugins.md); a Binding that goes away with its plugin
    // leaves its last value behind, so they get the theme's own bindings back before plugins rebuild.
    function restoreBindings() {
        bg = Qt.binding(() => preset.bg)
        surface = Qt.binding(() => preset.surface)
        surfaceHi = Qt.binding(() => preset.surfaceHi)
        text = Qt.binding(() => preset.text)
        textDim = Qt.binding(() => preset.textDim)
        textFaint = Qt.binding(() => preset.textFaint)
        onAccent = Qt.binding(() => preset.onAccent)
        accent = Qt.binding(() => followsCover ? vivid(Player.colorA, 0.72) : staticAccent)
        accent2 = Qt.binding(() => followsCover ? vivid(Player.colorB, 0.42) : staticAccent2)
    }
    Connections {
        target: Plugins
        function onAboutToChange() { theme.restoreBindings() }
    }

    // Selected pills (tabs, chips): filled with the accent.
    readonly property color selection: accent
    readonly property color onSelection: onAccent
    readonly property color panelBorder: cat ? alpha(accent, 0.16) : Qt.rgba(1, 1, 1, 0.07)

    // Corner style, remembered between launches: "rounded" or "semi". In "semi" panels, cards and
    // covers get crisper corners (about a third of the radius) while round controls (play buttons,
    // pills, avatars) stay round. Surface radii go through r(), so switching morphs them at once.
    property string shape: Prefs.stringValue("ui/shape", "rounded") === "rounded" ? "rounded" : "semi"
    onShapeChanged: Prefs.setValue("ui/shape", shape)
    readonly property bool semi: shape === "semi"
    property real roundness: semi ? 0 : 1
    Behavior on roundness { NumberAnimation { duration: 520; easing.type: Easing.InOutCubic } }
    function r(x) {
        const crisp = Math.min(x, Math.max(3, x * 0.3))
        return crisp + (x - crisp) * roundness
    }

    // Shape and surface tokens of the interface.
    readonly property real radiusS: r(10)
    readonly property real radiusM: r(16)
    readonly property real radiusL: r(22)
    readonly property real radiusXL: r(28)
    readonly property color glass: alpha(surface, 0.95)        // panels: nearly solid
    readonly property color glassHi: alpha(surfaceHi, 0.98)    // the player dock, menus
    readonly property color hairline: Qt.rgba(1, 1, 1, 0.07)
    readonly property color fill: alpha(text, 0.06)            // quiet controls at rest
    readonly property color fillHover: alpha(text, 0.1)
    readonly property real gap: 12                             // between the window's panels

    // Remembered between launches (Settings, first-run wizard).
    property bool dynamicAccent: Prefs.boolValue("ui/dynamicAccent", true)
    property bool ambient: Prefs.boolValue("ui/ambient", true)
    onDynamicAccentChanged: Prefs.setValue("ui/dynamicAccent", dynamicAccent)
    onAmbientChanged: Prefs.setValue("ui/ambient", ambient)
    readonly property color staticAccent: preset.accent
    readonly property color staticAccent2: preset.accent2
    readonly property bool followsCover: dynamicAccent && !preset.fixedAccent

    property color accent: followsCover ? vivid(Player.colorA, 0.72) : staticAccent
    property color accent2: followsCover ? vivid(Player.colorB, 0.42) : staticAccent2

    // Cover colours are often muted: the accent keeps their hue, with more colour and a fixed
    // lightness, so it reads the same on every cover. A grey cover gives a near-white accent.
    function vivid(c, lightness) {
        const col = Qt.lighter(c, 1.0)
        if (col.hslHue < 0 || col.hslSaturation < 0.07)
            return Qt.hsla(0, 0, lightness > 0.5 ? 0.88 : 0.32, 1)
        return Qt.hsla(col.hslHue, Math.min(0.92, col.hslSaturation * 1.35 + 0.22), lightness, 1)
    }
    Behavior on accent { ColorAnimation { duration: 900; easing.type: Easing.InOutCubic } }
    Behavior on accent2 { ColorAnimation { duration: 900; easing.type: Easing.InOutCubic } }

    // Picture behind the interface (any theme), dimmed so the text stays readable.
    property string wallpaper: Prefs.stringValue("ui/wallpaper", "")
    property real wallpaperDim: Prefs.realValue("ui/wallpaperDim", 0.6)
    onWallpaperChanged: Prefs.setValue("ui/wallpaper", wallpaper)
    onWallpaperDimChanged: Prefs.setValue("ui/wallpaperDim", wallpaperDim)

    // Windows: Segoe UI Variable; Linux/macOS: the system font (e.g. Cantarell, Noto Sans, Inter).
    readonly property bool windows: Qt.platform.os === "windows"
    // Headings: Bahnschrift on Windows (a DIN-style face, part of the new identity); elsewhere the system font.
    readonly property string fontDisplay: preset.fontDisplay || (windows ? "Bahnschrift" : Qt.application.font.family)
    readonly property string fontText: preset.fontText || (windows ? "Segoe UI Variable Text" : Qt.application.font.family)

    // Interface motion: every transition's length goes through ms(), so one factor sets how quick the
    // app feels (looping ambient animations keep their own pace).
    readonly property real motion: 0.6
    // Motion blur on page transitions and scrolling (Main.qml, MotionBlur.qml).
    // Off by default on Android: a full-screen pass per frame on every flick costs a phone's battery.
    property bool motionBlur: Prefs.boolValue("ui/motionBlur", Qt.platform.os !== "android")
    onMotionBlurChanged: Prefs.setValue("ui/motionBlur", motionBlur)
    function ms(base) { return Math.round(base * motion) }
    readonly property int fast: ms(150)
    readonly property int normal: ms(260)
    readonly property int slow: ms(520)

    // Bumped by main.cpp after a GPU reset: images then load again (they keep no memory copy).
    property int imageEpoch: 0

    function alpha(c, a) {
        const col = Qt.lighter(c, 1.0)
        return Qt.rgba(col.r, col.g, col.b, a)
    }

    function formatTime(sec) {
        const s = Math.max(0, Math.floor(sec))
        const m = Math.floor(s / 60)
        const r = s % 60
        return m + ":" + (r < 10 ? "0" : "") + r
    }

    function formatTotal(sec) {
        const m = Math.round(sec / 60)
        if (m < 60) return qsTr("%1 min").arg(m)
        return qsTr("%1 h %2 min").arg(Math.floor(m / 60)).arg(m % 60)
    }

    function roundedRectPath(w, h, r) {
        if (w <= 0 || h <= 0) return ""
        r = Math.max(0, Math.min(r, w / 2, h / 2))
        return `M ${r} 0 H ${w - r} A ${r} ${r} 0 0 1 ${w} ${r} V ${h - r} A ${r} ${r} 0 0 1 ${w - r} ${h} `
             + `H ${r} A ${r} ${r} 0 0 1 0 ${h - r} V ${r} A ${r} ${r} 0 0 1 ${r} 0 Z`
    }

    function greeting() {
        const h = new Date().getHours()
        if (h < 5) return qsTr("Good night")
        if (h < 12) return qsTr("Good morning")
        if (h < 18) return qsTr("Good afternoon")
        return qsTr("Good evening")
    }
}
