import QtQuick
import QtQuick.Controls

// 主操作按钮（自动打轴/导出/保存等）：Accent 蓝底白字。
Button {
    id: control
    implicitHeight: 30
    leftPadding: 18
    rightPadding: 18
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        text: control.text
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.enabled ? Theme.accentText : Theme.disabledText
    }

    background: Rectangle {
        radius: Theme.radiusButton
        border.width: control.enabled ? 1 : 0
        border.color: control.down ? Theme.accentPressed : Theme.accent
        color: !control.enabled ? Theme.buttonDisabled
             : control.down ? Theme.accentPressed
             : control.hovered ? Theme.accentHover
             : Theme.accent
    }
}
