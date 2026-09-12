import QtQuick
import QtQuick.Controls

// 传输栏仅显示 SVG 图标，交互时使用低对比度背景。
ToolButton {
    id: control
    implicitWidth: 30
    implicitHeight: 30
    padding: 7
    hoverEnabled: true
    Accessible.name: text
    ToolTip.visible: hovered
    ToolTip.text: text
    ToolTip.delay: 600
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: Image {
        source: control.icon.source
        sourceSize: Qt.size(16, 16)
        fillMode: Image.PreserveAspectFit
        opacity: control.enabled ? 1 : 0.35
    }

    background: Rectangle {
        radius: Theme.radiusButton
        border.width: 0
        color: control.down ? Theme.buttonPressed
             : control.hovered ? Theme.buttonHover
             : "transparent"
    }
}
