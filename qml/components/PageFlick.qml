import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Everplay

// Scrollable page: a column of sections with margins and a thin scroll bar.
Flickable {
    id: root
    default property alias content: column.data
    property real padding: 38
    property alias spacing: column.spacing

    contentWidth: width
    contentHeight: column.implicitHeight + padding * 2
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: 3200
    maximumFlickVelocity: 4200

    ScrollBar.vertical: ThinScrollBar {}
    // The wheel's speed and the position kept for Back / Forward.
    property alias scrollAssist: assist
    ScrollAssist { id: assist; view: root }

    ColumnLayout {
        id: column
        x: root.padding
        y: root.padding
        width: root.width - root.padding * 2
        spacing: 40
    }
}
