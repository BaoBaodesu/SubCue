import QtQuick
import QtQuick.Controls

// 深色复选框：勾选态用强调蓝边框与对勾。
CheckBox {
    id: control
    implicitHeight: 22
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSize

    contentItem: Text {
        text: control.text
        font: control.font
        leftPadding: control.indicator.width + control.spacing
        verticalAlignment: Text.AlignVCenter
        color: control.enabled ? Theme.text : Theme.disabledText
    }

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        radius: 2
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        color: control.down ? Theme.buttonPressed
             : control.hovered ? Theme.buttonHover
             : Theme.input
        border.width: 1
        border.color: control.checked || control.hovered ? Theme.accent : Theme.border

        Text {
            visible: control.checked
            anchors.centerIn: parent
            text: "✓"
            font.pixelSize: 11
            color: Theme.accentHover
        }
    }
}
