import QtQuick
import QtQuick.Controls
import Everplay

// The window appears before anything else is made: it holds only the background colour it had last time
// (Theme saves it) until Shell.qml is built into it after its first frame. Theme, the services and the pages
// all come after the window, so none of them hold it back.
ApplicationWindow {
    id: win
    width: 1420
    height: 900
    minimumWidth: 1120
    minimumHeight: 720
    visible: true
    title: "Everplay"
    color: ready ? Theme.bg : Prefs.stringValue("ui/windowColor", "#08080b")
    // Made as a dialog on Windows: see main.cpp (it becomes a plain window right after).
    flags: (Qt.platform.os === "windows" ? Qt.Dialog : Qt.Window) | Qt.FramelessWindowHint

    readonly property bool ready: shellLoader.status === Loader.Ready
    readonly property bool maximized: visibility === Window.Maximized || visibility === Window.FullScreen
    onVisibilityChanged: visibility => { if (visibility === Window.Minimized) Memory.trimSoon(500) }

    // The interface's files are compiled on a thread of their own from the start; it's built (at once, which is
    // faster than piece by piece between frames) when the window's first frame is on screen.
    readonly property Component shell: Qt.createComponent("Shell.qml", Component.Asynchronous)
    Loader {
        id: shellLoader
        anchors.fill: parent
        active: false
        sourceComponent: win.shell.status === Component.Ready ? win.shell : null
    }
    Connections {
        target: win
        enabled: !shellLoader.active
        function onFrameSwapped() { shellLoader.active = true }
    }
    Connections {
        target: win.shell
        function onStatusChanged() {
            if (win.shell.status === Component.Error)
                console.error(win.shell.errorString())
        }
    }
}
