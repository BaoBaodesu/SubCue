import QtQuick
import QtQuick.Controls

// 普通深色按钮：精确对应 Theme 按钮色 Normal/Hover/Pressed/Disabled。
Button {
    id: control
    implicitHeight: 30
    leftPadding: 16
    rightPadding: 16
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.enabled ? Theme.text : Theme.disabledText
    }

    background: Rectangle {
        radius: Theme.radiusButton
        border.width: 1
        border.color: control.enabled
            ? (control.down ? Theme.buttonPressed : (control.hovered ? Theme.buttonHoverBorder : Theme.buttonBorder))
            : Theme.border
        color: !control.enabled ? Theme.buttonDisabled
             : control.down ? Theme.buttonPressed
             : control.hovered ? Theme.buttonHover
             : Theme.button
    }
}
