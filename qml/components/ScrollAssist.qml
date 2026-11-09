import QtQuick
import Everplay

// For a page's main Flickable (PageFlick, the library's and a playlist's lists):
//  - the mouse wheel scrolls a fixed step per notch, smoothly — Flickable's own wheel handling moves
//    a few pixels and slows down at once; a touchpad's pixel deltas move the page directly;
//  - the scroll position is kept in the page's history entry (Nav.scrollY) and restored on Back / Forward,
//    also while the page is still filling in (it waits for the content to grow, a few seconds at most).
Item {
    id: root
    property Flickable view: null
    property bool remember: true
    property real step: 160                 // pixels per wheel notch

    property int ownSerial: -1               // the history entry this view belongs to
    property real lastY: 0
    property real restoreTo: -1

    function maxY() {
        if (!view)
            return 0
        return Math.max(view.originY, view.originY + view.contentHeight + view.topMargin + view.bottomMargin - view.height)
               - view.topMargin
    }
    function minY() { return view ? view.originY - view.topMargin : 0 }
    function clamp(y) { return Math.max(minY(), Math.min(maxY(), y)) }

    // A list may jump back to its top once more while it builds (a new model, its header): the position is
    // held a moment longer after it was reached, then the user's scrolling is theirs again.
    function tryRestore() {
        if (restoreTo < 0 || !view)
            return
        const y = clamp(restoreTo)
        if (Math.abs(view.contentY - y) > 0.5)
            view.contentY = y
        if (maxY() >= restoreTo && !settle.running)
            settle.start()
    }

    function start() {
        if (!view || ownSerial >= 0)
            return
        ownSerial = Nav.pageSerial
        if (remember && Nav.restoreScroll > 0) {
            restoreTo = Nav.restoreScroll
            tryRestore()
        }
    }
    onViewChanged: start()
    Component.onCompleted: start()

    Timer {
        running: root.restoreTo >= 0
        interval: 4000
        onTriggered: root.restoreTo = -1
    }
    Timer {
        id: settle
        interval: 700
        onTriggered: {
            root.restoreTo = -1
            if (root.remember && root.ownSerial === Nav.pageSerial)
                Nav.scrollY = root.view.contentY
        }
    }

    Connections {
        target: root.view
        function onContentHeightChanged() { root.tryRestore() }
        function onMovementStarted() { root.restoreTo = -1; settle.stop(); wheelAnim.stop() }
        function onContentYChanged() {
            const dy = root.view.contentY - root.lastY
            root.lastY = root.view.contentY
            if (root.restoreTo >= 0) {
                root.tryRestore()
            } else if (root.remember && root.ownSerial === Nav.pageSerial) {
                Nav.scrollY = root.view.contentY
                if (Theme.motionBlur && Math.abs(dy) < 400)   // not a jump (a restored position)
                    Nav.addMotion(dy)
            }
        }
    }

    NumberAnimation {
        id: wheelAnim
        target: root.view
        property: "contentY"
        duration: Theme.ms(240)
        easing.type: Easing.OutCubic
    }
    property real wheelTarget: 0

    WheelHandler {
        parent: root.view
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            if (!root.view || (event.angleDelta.y === 0 && event.pixelDelta.y === 0)) {
                event.accepted = false   // sideways: for whoever scrolls that way
                return
            }
            root.restoreTo = -1
            settle.stop()
            if (event.pixelDelta.y !== 0) {   // a touchpad that reports pixels: the page follows the fingers
                wheelAnim.stop()
                root.view.contentY = root.clamp(root.view.contentY - event.pixelDelta.y)
                return
            }
            const from = wheelAnim.running ? root.wheelTarget : root.view.contentY
            root.wheelTarget = root.clamp(from - event.angleDelta.y / 120 * root.step)
            wheelAnim.stop()
            wheelAnim.to = root.wheelTarget
            wheelAnim.start()
        }
    }
}
