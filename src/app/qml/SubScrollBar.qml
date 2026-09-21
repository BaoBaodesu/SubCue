import QtQuick
import QtQuick.Controls

// 深色滚动条：可见轨道 + 常驻滑块，避免 AsNeeded 淡出后几乎看不见。
ScrollBar {
    id: control
    implicitWidth: orientation === Qt.Vertical ? Theme.scrollBarSize : Theme.scrollThumbSize
    implicitHeight: orientation === Qt.Horizontal ? Theme.scrollBarSize : Theme.scrollThumbSize
    policy: ScrollBar.AlwaysOn
    padding: 2
    minimumSize: 0.12

    background: Rectangle {
        implicitWidth: Theme.scrollBarSize
        implicitHeight: Theme.scrollBarSize
        radius: 5
        visible: control.size < 1.0 && control.policy !== ScrollBar.AlwaysOff
        color: Theme.scrollBarTrack
    }

    contentItem: Rectangle {
        implicitWidth: Theme.scrollThumbSize
        implicitHeight: Theme.scrollThumbSize
        radius: 4
        visible: control.size < 1.0 && control.policy !== ScrollBar.AlwaysOff
        color: control.pressed ? Theme.scrollThumbPressed
             : control.hovered ? Theme.scrollThumbHover
             : Theme.scrollThumb
    }
}
