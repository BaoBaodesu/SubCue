import QtQuick
import QtQuick.Controls

// 深色滚动条：透明轨道 + 6~9px 滑块。
ScrollBar {
    id: control
    policy: ScrollBar.AsNeeded

    background: Rectangle {
        implicitWidth: Theme.scrollThumbSize
        implicitHeight: Theme.scrollThumbSize
        color: "transparent"
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
