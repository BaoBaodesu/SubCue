import QtQuick
import QtQuick.Controls

// 顶部命令栏按钮：无大边框，仅 hover/pressed 底色变化。
ToolButton {
    id: control
    implicitHeight: 28
    leftPadding: 10
    rightPadding: 10
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Theme.disabledText
             : control.highlighted ? Theme.accentText
             : (control.hovered || control.down) ? Theme.menuButtonTextHover
             : Theme.menuButtonText
    }

    background: Rectangle {
        radius: Theme.radiusButton
        color: control.down ? Theme.menuButtonPressed
             : control.highlighted ? Theme.selection
             : control.hovered ? Theme.menuButtonHover
             : "transparent"
        border.width: control.highlighted ? 1 : 0
        border.color: control.highlighted ? Theme.focusBorder : "transparent"

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 2
            color: Theme.accent
            visible: control.highlighted
        }
    }
}
