import QtQuick
import Everplay

// A layer effect that smears its item along the way it moved during the last frame (shaders/motionblur.frag).
// blur: that motion in pixels; zero leaves the picture as it is.
ShaderEffect {
    id: root
    property variant source
    property point motion: Qt.point(0, 0)
    readonly property point blur: Qt.point(width > 0 ? motion.x / width : 0, height > 0 ? motion.y / height : 0)
    fragmentShader: Qt.resolvedUrl("../shaders/motionblur.frag.qsb")
}
